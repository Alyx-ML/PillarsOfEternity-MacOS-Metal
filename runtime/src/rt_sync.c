/*
 * KERNEL32 critical sections, interlocked operations, thread local storage
 * (TLS) and time functions.
 */
#include "rt_object.h"
#include "rt_thread.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

/* ---------------------------------------------------------------- critical sections */

/*
 * Guest CRITICAL_SECTION (24 bytes):
 *   +0  DebugInfo       +4  LockCount     +8  RecursionCount
 *   +12 OwningThread    +16 LockSemaphore +20 SpinCount
 * LockSemaphore holds the index of a host recursive mutex. RecursionCount
 * and OwningThread are kept current for guest code that reads them.
 *
 * Windows DeleteCriticalSection does not fail when a thread owns the
 * critical section or waits for it (the state of the waiting threads is
 * then undefined). A host mutex that is locked or has a waiter cannot be
 * destroyed. DeleteCriticalSection then marks the entry as deleted, and
 * the LeaveCriticalSection that makes the mutex free destroys it and frees
 * the index.
 */
#define CS_CHUNK 256u
#define CS_CHUNKS 256u
#define CS_MAX (CS_CHUNK * CS_CHUNKS)

typedef struct cs_entry {
    pthread_mutex_t m;
    uint32_t guest; /* guest address of the CRITICAL_SECTION, 0 if free */
    _Atomic(bool) deleted; /* deleted while in use; the mutex is not yet destroyed */
} cs_entry;

static pthread_mutex_t cs_table_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic(cs_entry *) cs_chunks[CS_CHUNKS];
static uint32_t cs_next = 1; /* index 0 is never used */
static uint32_t *cs_free_list;
static uint32_t cs_free_count, cs_free_cap;

static cs_entry *cs_entry_at(uint32_t idx)
{
    cs_entry *chunk;
    if (idx == 0 || idx >= CS_MAX) return NULL;
    chunk = atomic_load_explicit(&cs_chunks[idx / CS_CHUNK], memory_order_acquire);
    if (chunk == NULL) return NULL;
    return &chunk[idx % CS_CHUNK];
}

static cs_entry *cs_lookup(uint32_t cs)
{
    cs_entry *e;
    if (cs == 0) rt_fatal("critical section function with a NULL pointer");
    e = cs_entry_at(rd32(cs + 16));
    if (e == NULL || e->guest != cs) rt_fatal("use of an uninitialized critical section at 0x%08x", cs);
    return e;
}

void rt_InitializeCriticalSection(_cpu *cpu)
{
    uint32_t cs = ARG(0), idx;
    cs_entry *e;
    pthread_mutexattr_t a;

    if (pthread_mutex_lock(&cs_table_lock) != 0) rt_fatal("critical section table lock failed");
    if (cs_free_count > 0)
    {
        idx = cs_free_list[--cs_free_count];
    }
    else
    {
        if (cs_next >= CS_MAX) rt_fatal("more than %u critical sections", CS_MAX - 1u);
        idx = cs_next++;
        if (atomic_load_explicit(&cs_chunks[idx / CS_CHUNK], memory_order_relaxed) == NULL)
        {
            cs_entry *chunk = calloc(CS_CHUNK, sizeof(cs_entry));
            if (chunk == NULL) rt_fatal("out of host memory for critical sections");
            atomic_store_explicit(&cs_chunks[idx / CS_CHUNK], chunk, memory_order_release);
        }
    }
    e = cs_entry_at(idx);
    if (pthread_mutexattr_init(&a) != 0) rt_fatal("pthread_mutexattr_init failed");
    if (pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE) != 0) rt_fatal("pthread_mutexattr_settype failed");
    if (pthread_mutex_init(&e->m, &a) != 0) rt_fatal("pthread_mutex_init failed");
    pthread_mutexattr_destroy(&a);
    e->guest = cs;
    if (pthread_mutex_unlock(&cs_table_lock) != 0) rt_fatal("critical section table unlock failed");

    wr32(cs + 0, 0);
    wr32(cs + 4, 0xFFFFFFFFu);
    wr32(cs + 8, 0);
    wr32(cs + 12, 0);
    wr32(cs + 16, idx);
    wr32(cs + 20, 0);
}

/* BOOL InitializeCriticalSectionAndSpinCount(cs, spin): the spin count is
   kept in the SpinCount field (offset 20, without the flag bits); the host
   mutex does its own spinning. */
void rt_InitializeCriticalSectionAndSpinCount(_cpu *cpu)
{
    uint32_t cs = ARG(0), spin = ARG(1);
    rt_InitializeCriticalSection(cpu);
    wr32(cs + 20, spin & 0x00FFFFFFu);
    RET(1);
}

static void cs_free_entry(cs_entry *e, uint32_t idx);

void rt_DeleteCriticalSection(_cpu *cpu)
{
    uint32_t cs = ARG(0);
    cs_entry *e;
    uint32_t k;
    bool zero = cs != 0;
    /* Windows does nothing for a critical section that is all zero (never
       initialized): it has no debug data and no semaphore to release. The
       engine deletes such a section at exit. */
    for (k = 0; zero && k < 24u; k += 4u)
        if (rd32(cs + k) != 0) zero = false;
    if (zero) return;
    e = cs_lookup(cs);
    uint32_t idx = rd32(cs + 16);
    int r;

    if (pthread_mutex_lock(&cs_table_lock) != 0) rt_fatal("critical section table lock failed");
    if (atomic_load_explicit(&e->deleted, memory_order_relaxed))
    {
        if (pthread_mutex_unlock(&cs_table_lock) != 0) rt_fatal("critical section table unlock failed");
        rt_fatal("DeleteCriticalSection of the critical section at 0x%08x, which is already deleted", cs);
    }
    r = pthread_mutex_destroy(&e->m);
    if (r == EBUSY)
    {
        /* The section is in use (see the comment at the top). The owner
           can release the mutex before it sees the mark. The fences make
           sure that the owner sees the mark (and destroys the mutex) or
           the second destroy here finds the mutex free. */
        atomic_store_explicit(&e->deleted, true, memory_order_relaxed);
        atomic_thread_fence(memory_order_seq_cst);
        r = pthread_mutex_destroy(&e->m);
        if (r == 0)
        {
            cs_free_entry(e, idx);
            if (pthread_mutex_unlock(&cs_table_lock) != 0) rt_fatal("critical section table unlock failed");
            wr32(cs + 16, 0);
            return;
        }
        if (r != EBUSY) rt_fatal("DeleteCriticalSection failed (%d)", r);
        if (pthread_mutex_unlock(&cs_table_lock) != 0) rt_fatal("critical section table unlock failed");
        rt_log("DeleteCriticalSection of the critical section at 0x%08x while it is in use (owner thread %u, "
               "recursion %u, calling thread %u, return address 0x%08x); the host mutex is destroyed after the "
               "last LeaveCriticalSection",
               cs, rd32(cs + 12), rd32(cs + 8), rt_self()->tid, rd32(esp));
        return;
    }
    if (r != 0) rt_fatal("DeleteCriticalSection failed (%d)", r);
    cs_free_entry(e, idx);
    if (pthread_mutex_unlock(&cs_table_lock) != 0) rt_fatal("critical section table unlock failed");
    wr32(cs + 16, 0);
}

/* After a LeaveCriticalSection of a deleted section: destroy the host mutex
   if it is free, and free the index. */
static void cs_reap(cs_entry *e, uint32_t cs, uint32_t idx)
{
    int r;
    if (pthread_mutex_lock(&cs_table_lock) != 0) rt_fatal("critical section table lock failed");
    if (atomic_load_explicit(&e->deleted, memory_order_relaxed) && e->guest == cs)
    {
        r = pthread_mutex_destroy(&e->m);
        if (r == 0)
        {
            cs_free_entry(e, idx);
            /* The guest can have initialized the memory again. */
            if (rd32(cs + 16) == idx) wr32(cs + 16, 0);
        }
        else if (r != EBUSY)
            rt_fatal("destruction of the deleted critical section at 0x%08x failed (%d)", cs, r);
    }
    if (pthread_mutex_unlock(&cs_table_lock) != 0) rt_fatal("critical section table unlock failed");
}

/* Free entry idx. The table lock is held. */
static void cs_free_entry(cs_entry *e, uint32_t idx)
{
    atomic_store_explicit(&e->deleted, false, memory_order_relaxed);
    e->guest = 0;
    if (cs_free_count == cs_free_cap)
    {
        uint32_t cap = cs_free_cap ? cs_free_cap * 2u : 64u;
        uint32_t *n = realloc(cs_free_list, (size_t)cap * sizeof(uint32_t));
        if (n == NULL) rt_fatal("out of host memory for critical sections");
        cs_free_list = n;
        cs_free_cap = cap;
    }
    cs_free_list[cs_free_count++] = idx;
}

static void cs_owned(uint32_t cs)
{
    wr32(cs + 8, rd32(cs + 8) + 1u);
    wr32(cs + 12, rt_self()->tid);
}

void rt_EnterCriticalSection(_cpu *cpu)
{
    uint32_t cs = ARG(0);
    int r = pthread_mutex_lock(&cs_lookup(cs)->m);
    if (r != 0) rt_fatal("EnterCriticalSection failed (%d)", r);
    cs_owned(cs);
}

void rt_TryEnterCriticalSection(_cpu *cpu)
{
    uint32_t cs = ARG(0);
    int r = pthread_mutex_trylock(&cs_lookup(cs)->m);
    if (r == EBUSY)
    {
        RET(0);
        return;
    }
    if (r != 0) rt_fatal("TryEnterCriticalSection failed (%d)", r);
    cs_owned(cs);
    RET(1);
}

void rt_LeaveCriticalSection(_cpu *cpu)
{
    uint32_t cs = ARG(0);
    cs_entry *e = cs_lookup(cs);
    uint32_t rec = rd32(cs + 8);
    int r;
    if (rec == 0 || rd32(cs + 12) != rt_self()->tid)
        rt_fatal("LeaveCriticalSection of a critical section that the thread does not own");
    wr32(cs + 8, rec - 1u);
    if (rec == 1u) wr32(cs + 12, 0);
    r = pthread_mutex_unlock(&e->m);
    if (r != 0) rt_fatal("LeaveCriticalSection failed (%d)", r);
    atomic_thread_fence(memory_order_seq_cst);
    if (atomic_load_explicit(&e->deleted, memory_order_relaxed)) cs_reap(e, cs, rd32(cs + 16));
}

/* ---------------------------------------------------------------- interlocked */

static _Atomic(uint32_t) *atomic32(uint32_t g)
{
    if (g == 0 || (g & 3u) != 0) rt_fatal("interlocked operation on a bad address 0x%08x", g);
    return (_Atomic(uint32_t) *)G2H(g);
}

void rt_InterlockedCompareExchange(_cpu *cpu)
{
    uint32_t expected = ARG(2);
    atomic_compare_exchange_strong(atomic32(ARG(0)), &expected, ARG(1));
    RET(expected);
}

void rt_InterlockedExchange(_cpu *cpu)
{
    RET(atomic_exchange(atomic32(ARG(0)), ARG(1)));
}

void rt_InterlockedIncrement(_cpu *cpu)
{
    RET(atomic_fetch_add(atomic32(ARG(0)), 1u) + 1u);
}

void rt_InterlockedDecrement(_cpu *cpu)
{
    RET(atomic_fetch_sub(atomic32(ARG(0)), 1u) - 1u);
}

/* ---------------------------------------------------------------- TLS */

static pthread_mutex_t tls_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t tls_used;

#define TLS_OUT_OF_INDEXES 0xFFFFFFFFu

/* The reserved index of rt_tls_reserve_for_call. */
static uint32_t tls_fixed_ret;
static uint32_t tls_fixed_index = TLS_OUT_OF_INDEXES;
static bool tls_fixed_given;

void rt_tls_reserve_for_call(uint32_t ret_addr, uint32_t index)
{
    if (index >= TLS_SLOT_COUNT) rt_fatal("TLS index %u cannot be reserved (the limit is %u)", index, TLS_SLOT_COUNT);
    if (pthread_mutex_lock(&tls_lock) != 0) rt_fatal("TLS lock failed");
    if (tls_fixed_index != TLS_OUT_OF_INDEXES) rt_fatal("a TLS index is already reserved for the call at 0x%08x", tls_fixed_ret);
    if (tls_used & (UINT64_C(1) << index)) rt_fatal("TLS index %u cannot be reserved: it is in use", index);
    tls_used |= UINT64_C(1) << index;
    tls_fixed_ret = ret_addr;
    tls_fixed_index = index;
    if (pthread_mutex_unlock(&tls_lock) != 0) rt_fatal("TLS unlock failed");
}

void rt_TlsAlloc(_cpu *cpu)
{
    uint32_t i;
    uint32_t ret = rd32(esp);
    if (pthread_mutex_lock(&tls_lock) != 0) rt_fatal("TLS lock failed");
    if (tls_fixed_index != TLS_OUT_OF_INDEXES && ret == tls_fixed_ret)
    {
        if (tls_fixed_given) rt_fatal("TlsAlloc: the call at 0x%08x ran a second time; its TLS index %u is given", ret, tls_fixed_index);
        tls_fixed_given = true;
        i = tls_fixed_index;
        if (pthread_mutex_unlock(&tls_lock) != 0) rt_fatal("TLS unlock failed");
        RET(i);
        return;
    }
    for (i = 0; i < TLS_SLOT_COUNT; i++)
    {
        if ((tls_used & (UINT64_C(1) << i)) == 0)
        {
            tls_used |= UINT64_C(1) << i;
            break;
        }
    }
    if (pthread_mutex_unlock(&tls_lock) != 0) rt_fatal("TLS unlock failed");
    if (i == TLS_SLOT_COUNT)
    {
        rt_set_last_error(ERROR_NO_MORE_ITEMS);
        RET(TLS_OUT_OF_INDEXES);
        return;
    }
    /* A new slot is zero in every thread (TlsFree cleared it). */
    RET(i);
}

static void clear_slot(rt_thread *t, void *ctx)
{
    uint32_t i = *(uint32_t *)ctx;
    wr32(t->teb + TEB_TLS_SLOTS + 4u * i, 0);
}

void rt_TlsFree(_cpu *cpu)
{
    uint32_t i = ARG(0);
    bool ok;
    if (pthread_mutex_lock(&tls_lock) != 0) rt_fatal("TLS lock failed");
    ok = i < TLS_SLOT_COUNT && (tls_used & (UINT64_C(1) << i)) != 0;
    if (ok)
    {
        tls_used &= ~(UINT64_C(1) << i);
        rt_thread_foreach(clear_slot, &i);
    }
    if (pthread_mutex_unlock(&tls_lock) != 0) rt_fatal("TLS unlock failed");
    if (!ok) rt_set_last_error(ERROR_INVALID_PARAMETER);
    RET(ok ? 1 : 0);
}

void rt_TlsGetValue(_cpu *cpu)
{
    uint32_t i = ARG(0);
    rt_thread *t = rt_self();
    if (i >= TLS_SLOT_COUNT)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    rt_set_last_error(ERROR_SUCCESS);
    RET(rd32(t->teb + TEB_TLS_SLOTS + 4u * i));
}

void rt_TlsSetValue(_cpu *cpu)
{
    uint32_t i = ARG(0);
    rt_thread *t = rt_self();
    if (i >= TLS_SLOT_COUNT)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    wr32(t->teb + TEB_TLS_SLOTS + 4u * i, ARG(1));
    RET(1);
}

/* ---------------------------------------------------------------- time */

/* 100 ns intervals between 1601-01-01 and 1970-01-01. */
#define FILETIME_UNIX_EPOCH UINT64_C(116444736000000000)

uint64_t rt_filetime_now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) rt_fatal("clock_gettime failed");
    return FILETIME_UNIX_EPOCH + (uint64_t)ts.tv_sec * 10000000u + (uint64_t)ts.tv_nsec / 100u;
}

static uint64_t uptime_ns(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

void rt_GetTickCount(_cpu *cpu)
{
    RET((uint32_t)(uptime_ns() / 1000000u));
}

void rt_timeGetTime(_cpu *cpu)
{
    RET((uint32_t)(uptime_ns() / 1000000u));
}

/* QueryPerformanceFrequency: 10 MHz, the value of current Windows
   versions. */
#define QPC_FREQUENCY UINT64_C(10000000)

void rt_QueryPerformanceCounter(_cpu *cpu)
{
    if (ARG(0) == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    wr64(ARG(0), uptime_ns() / 100u);
    RET(1);
}

void rt_QueryPerformanceFrequency(_cpu *cpu)
{
    if (ARG(0) == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    wr64(ARG(0), QPC_FREQUENCY);
    RET(1);
}

void rt_GetSystemTimeAsFileTime(_cpu *cpu)
{
    wr64(ARG(0), rt_filetime_now());
}

/* SYSTEMTIME: wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond,
   wMilliseconds (16-bit each). */
void rt_write_systemtime(uint32_t g, const struct tm *tm, uint32_t ms)
{
    wr16(g + 0, (uint16_t)(tm->tm_year + 1900));
    wr16(g + 2, (uint16_t)(tm->tm_mon + 1));
    wr16(g + 4, (uint16_t)tm->tm_wday);
    wr16(g + 6, (uint16_t)tm->tm_mday);
    wr16(g + 8, (uint16_t)tm->tm_hour);
    wr16(g + 10, (uint16_t)tm->tm_min);
    wr16(g + 12, (uint16_t)tm->tm_sec);
    wr16(g + 14, (uint16_t)ms);
}

void rt_GetLocalTime(_cpu *cpu)
{
    struct timespec ts;
    struct tm tm;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) rt_fatal("clock_gettime failed");
    if (localtime_r(&ts.tv_sec, &tm) == NULL) rt_fatal("localtime_r failed");
    rt_write_systemtime(ARG(0), &tm, (uint32_t)(ts.tv_nsec / 1000000L));
}

static void write_wname(uint32_t g, const char *s)
{
    uint32_t i;
    for (i = 0; i < 31u && s != NULL && s[i] != 0; i++) wr16(g + 2u * i, (uint16_t)(uint8_t)s[i]);
    for (; i < 32u; i++) wr16(g + 2u * i, 0);
}

static long gmtoff_at(time_t t)
{
    struct tm tm;
    if (localtime_r(&t, &tm) == NULL) rt_fatal("localtime_r failed");
    return tm.tm_gmtoff;
}

/* Find the time in (a, b] where the UTC offset changes (to the minute). */
static time_t find_transition(time_t a, time_t b)
{
    long off_a = gmtoff_at(a);
    while (b - a > 60)
    {
        time_t m = a + (b - a) / 2;
        if (gmtoff_at(m) == off_a) a = m;
        else b = m;
    }
    return b;
}

/* Write the SYSTEMTIME at g as a yearly transition rule of
   TIME_ZONE_INFORMATION, in the form that Windows uses for the time zones of
   the system: wYear 0, wMonth, wDayOfWeek, wDay the occurrence of that day
   of the week in the month (1 to 4, 5 for the last), and the local time of
   the change in the time before the change. Programs convert this form to
   a date of a given year (for example Mono's GetTimeZoneData icall). */
static void write_tz_rule(uint32_t g, time_t change)
{
    time_t before = change - 1, t;
    struct tm a, b;
    int days;
    if (localtime_r(&before, &a) == NULL) rt_fatal("localtime_r failed");
    /* Add the second with the fields as UTC, so that timegm normalizes them
       (for example 23:59:59 + 1 s is 0:00 of the next day). */
    a.tm_sec += 1;
    a.tm_isdst = 0;
    t = timegm(&a);
    if (t == (time_t)-1 || gmtime_r(&t, &a) == NULL) rt_fatal("timegm failed");
    b = (struct tm){ .tm_year = a.tm_year, .tm_mon = a.tm_mon + 1, .tm_mday = 0 };
    t = timegm(&b);
    if (t == (time_t)-1 || gmtime_r(&t, &b) == NULL) rt_fatal("timegm failed");
    days = b.tm_mday; /* days in the month of the change */
    wr16(g + 0, 0);
    wr16(g + 2, (uint16_t)(a.tm_mon + 1));
    wr16(g + 4, (uint16_t)a.tm_wday);
    wr16(g + 6, (uint16_t)(a.tm_mday + 7 > days ? 5 : (a.tm_mday - 1) / 7 + 1));
    wr16(g + 8, (uint16_t)a.tm_hour);
    wr16(g + 10, (uint16_t)a.tm_min);
    wr16(g + 12, (uint16_t)a.tm_sec);
    wr16(g + 14, 0);
}

/* TIME_ZONE_INFORMATION (172 bytes):
   +0 Bias, +4 StandardName[32], +68 StandardDate, +84 StandardBias,
   +88 DaylightName[32], +152 DaylightDate, +168 DaylightBias.
   The transition dates are the rules of write_tz_rule, from the changes of
   the current year. */
void rt_GetTimeZoneInformation(_cpu *cpu)
{
    uint32_t g = ARG(0);
    time_t now = time(NULL), year_start, year_end, t1 = 0, t2 = 0;
    struct tm tm;
    long std_off, dst_off, off_start, off_mid;
    uint32_t result;
    int transitions = 0;

    if (g == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0xFFFFFFFFu);
        return;
    }
    memset(G2H(g), 0, 172);
    tzset();
    if (localtime_r(&now, &tm) == NULL) rt_fatal("localtime_r failed");
    struct tm jan = { .tm_year = tm.tm_year, .tm_mon = 0, .tm_mday = 1, .tm_isdst = -1 };
    struct tm dec = { .tm_year = tm.tm_year + 1, .tm_mon = 0, .tm_mday = 1, .tm_isdst = -1 };
    year_start = mktime(&jan);
    year_end = mktime(&dec);
    if (year_start == (time_t)-1 || year_end == (time_t)-1) rt_fatal("mktime failed");

    off_start = gmtoff_at(year_start);
    /* Find up to two offset changes in the year, by days. */
    {
        time_t d, prev = year_start;
        long prev_off = off_start;
        for (d = year_start + 86400; d <= year_end; d += 86400)
        {
            long o = gmtoff_at(d);
            if (o != prev_off)
            {
                time_t tr = find_transition(prev, d);
                if (transitions == 0) t1 = tr;
                else if (transitions == 1) t2 = tr;
                transitions++;
                prev_off = o;
            }
            prev = d;
        }
    }
    if (transitions == 0)
    {
        std_off = dst_off = off_start;
    }
    else
    {
        off_mid = gmtoff_at(t1);
        /* Standard time has the smaller offset. */
        std_off = off_start < off_mid ? off_start : off_mid;
        dst_off = off_start < off_mid ? off_mid : off_start;
    }

    wr32(g + 0, (uint32_t)(int32_t)(-(std_off / 60)));
    write_wname(g + 4, tzname[0]);
    write_wname(g + 88, tzname[1]);
    wr32(g + 84, 0);
    if (transitions >= 2)
    {
        time_t to_dst, to_std;
        if (gmtoff_at(t1) == dst_off) { to_dst = t1; to_std = t2; }
        else { to_dst = t2; to_std = t1; }
        write_tz_rule(g + 152, to_dst);
        write_tz_rule(g + 68, to_std);
        wr32(g + 168, (uint32_t)(int32_t)(-((dst_off - std_off) / 60)));
        result = (gmtoff_at(now) == dst_off) ? 2u : 1u; /* TIME_ZONE_ID_DAYLIGHT / STANDARD */
    }
    else if (transitions == 1)
    {
        /* A permanent change of the zone during this year (no daylight
           saving). Report the current offset as standard time. */
        wr32(g + 0, (uint32_t)(int32_t)(-(gmtoff_at(now) / 60)));
        result = 0u; /* TIME_ZONE_ID_UNKNOWN */
    }
    else
    {
        result = 0u;
    }
    RET(result);
}

/* ---------------------------------------------------------------- FILETIME and SYSTEMTIME conversion */

#define FT_PER_DAY UINT64_C(864000000000)
/* Days from 1601-01-01 to 1970-01-01. */
#define DAYS_1601_TO_1970 134774

/* Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's
   days_from_civil). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    int64_t era;
    unsigned yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);
    doy = (153u * (m + (m > 2 ? (unsigned)-3 : 9u)) + 2u) / 5u + d - 1u;
    doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d)
{
    int64_t era;
    unsigned doe, yoe, doy, mp;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (unsigned)(z - era * 146097);
    yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    mp = (5u * doy + 2u) / 153u;
    *d = doy - (153u * mp + 2u) / 5u + 1u;
    *m = mp < 10u ? mp + 3u : mp - 9u;
    *y = (int64_t)yoe + era * 400 + (*m <= 2);
}

static bool leap(int64_t y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

/* BOOL FileTimeToSystemTime(const FILETIME *, LPSYSTEMTIME) */
void rt_FileTimeToSystemTime(_cpu *cpu)
{
    uint64_t ft = rd64(ARG(0)), rem;
    int64_t days, y;
    unsigned m, d;
    uint32_t st = ARG(1);
    if (ft > UINT64_C(0x7FFFFFFFFFFFFFFF))
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    days = (int64_t)(ft / FT_PER_DAY);
    rem = ft % FT_PER_DAY;
    civil_from_days(days - DAYS_1601_TO_1970, &y, &m, &d);
    wr16(st + 0, (uint16_t)y);
    wr16(st + 2, (uint16_t)m);
    wr16(st + 4, (uint16_t)((days + 1) % 7)); /* 1601-01-01 was a Monday */
    wr16(st + 6, (uint16_t)d);
    wr16(st + 8, (uint16_t)(rem / UINT64_C(36000000000)));
    wr16(st + 10, (uint16_t)(rem / 600000000u % 60u));
    wr16(st + 12, (uint16_t)(rem / 10000000u % 60u));
    wr16(st + 14, (uint16_t)(rem / 10000u % 1000u));
    RET(1);
}

/* BOOL FileTimeToDosDateTime(const FILETIME *, LPWORD date, LPWORD time).
   MS-DOS dates cover 1980-01-01 to 2107-12-31; the time has 2-second
   steps. */
void rt_FileTimeToDosDateTime(_cpu *cpu)
{
    uint64_t ft = rd64(ARG(0)), rem;
    int64_t days, y;
    unsigned m, d;
    uint32_t pd = ARG(1), pt = ARG(2);
    if (ft > UINT64_C(0x7FFFFFFFFFFFFFFF))
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    days = (int64_t)(ft / FT_PER_DAY);
    rem = ft % FT_PER_DAY;
    civil_from_days(days - DAYS_1601_TO_1970, &y, &m, &d);
    if (y < 1980 || y > 2107)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (pd != 0) wr16(pd, (uint16_t)(((uint32_t)(y - 1980) << 9) | (m << 5) | d));
    if (pt != 0)
    {
        uint32_t h = (uint32_t)(rem / UINT64_C(36000000000)), mi = (uint32_t)(rem / 600000000u % 60u),
                 sec = (uint32_t)(rem / 10000000u % 60u);
        wr16(pt, (uint16_t)((h << 11) | (mi << 5) | (sec / 2u)));
    }
    RET(1);
}

/* BOOL SystemTimeToFileTime(const SYSTEMTIME *, LPFILETIME). wDayOfWeek
   is not used. */
void rt_SystemTimeToFileTime(_cpu *cpu)
{
    static const unsigned mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    uint32_t st = ARG(0);
    unsigned y = rd16(st), m = rd16(st + 2), d = rd16(st + 6), h = rd16(st + 8), mi = rd16(st + 10), s = rd16(st + 12),
             ms = rd16(st + 14);
    int64_t days;
    if (y < 1601 || y > 30827 || m < 1 || m > 12 || d < 1 || d > mdays[m - 1] + (m == 2 && leap(y)) || h > 23 || mi > 59 ||
        s > 59 || ms > 999)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    days = days_from_civil(y, m, d) + DAYS_1601_TO_1970;
    wr64(ARG(1), (uint64_t)days * FT_PER_DAY + (uint64_t)h * UINT64_C(36000000000) + (uint64_t)mi * 600000000u +
                     (uint64_t)s * 10000000u + (uint64_t)ms * 10000u);
    RET(1);
}

/* FileTimeToLocalFileTime and LocalFileTimeToFileTime use the current UTC
   offset (with the current daylight saving state), as Windows does, not
   the offset at the converted time. */
static int64_t current_bias_ft(void)
{
    return (int64_t)gmtoff_at(time(NULL)) * 10000000;
}

static void shift_filetime(_cpu *cpu, int64_t by)
{
    uint64_t ft = rd64(ARG(0));
    int64_t v = (int64_t)ft + by;
    if (ft > UINT64_C(0x7FFFFFFFFFFFFFFF) || v < 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    wr64(ARG(1), (uint64_t)v);
    RET(1);
}

void rt_FileTimeToLocalFileTime(_cpu *cpu) { shift_filetime(cpu, current_bias_ft()); }
void rt_LocalFileTimeToFileTime(_cpu *cpu) { shift_filetime(cpu, -current_bias_ft()); }

void rt_CompareFileTime(_cpu *cpu)
{
    uint64_t a = rd64(ARG(0)), b = rd64(ARG(1));
    RET(a < b ? 0xFFFFFFFFu : (a > b ? 1u : 0u));
}
