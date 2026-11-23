/*
 * WINMM multimedia timers: timeGetDevCaps, timeBeginPeriod, timeEndPeriod
 * and timeSetEvent.
 *
 * The host timers have a resolution better than 1 ms, so the timer
 * period of timeBeginPeriod is always in effect. The runtime only checks
 * and counts the requests.
 *
 * Each timer of timeSetEvent has one runtime thread with a guest thread
 * state (rt_thread_start_runtime), as the timer thread of WINMM on
 * Windows. The thread calls the callback function at each period.
 * mono.dll does not import timeKillEvent, so a periodic timer runs until
 * the process ends.
 */
#include "rt.h"
#include "rt_thread.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TIMERR_NOERROR 0u
#define TIMERR_NOCANDO 97u

#define TIME_PERIODIC 0x0001u
#define TIME_CALLBACK_EVENT_SET 0x0010u
#define TIME_CALLBACK_EVENT_PULSE 0x0020u
#define TIME_KILL_SYNCHRONOUS 0x0100u

/* TIMECAPS of Windows XP. */
#define PERIOD_MIN 1u
#define PERIOD_MAX 1000000u

#define MAX_TIMERS 16
#define TIMER_STACK_SIZE (256u * 1024u)

static pthread_mutex_t tm_lock = PTHREAD_MUTEX_INITIALIZER;

static void mlock(void)
{
    if (pthread_mutex_lock(&tm_lock) != 0) rt_fatal("WINMM lock failed");
}

static void munlock(void)
{
    if (pthread_mutex_unlock(&tm_lock) != 0) rt_fatal("WINMM unlock failed");
}

static uint64_t uptime_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

/* ---------------------------------------------------------------- periods */

/* Active timeBeginPeriod requests (tm_lock). */
static uint32_t period_requests;

/* MMRESULT timeGetDevCaps(LPTIMECAPS, UINT size) */
void rt_timeGetDevCaps(_cpu *cpu)
{
    uint32_t caps = ARG(0);
    if (caps == 0 || ARG(1) < 8)
    {
        RET(TIMERR_NOCANDO);
        return;
    }
    wr32(caps, PERIOD_MIN);
    wr32(caps + 4, PERIOD_MAX);
    RET(TIMERR_NOERROR);
}

/* MMRESULT timeBeginPeriod(UINT period) */
void rt_timeBeginPeriod(_cpu *cpu)
{
    uint32_t p = ARG(0);
    if (p < PERIOD_MIN || p > PERIOD_MAX)
    {
        RET(TIMERR_NOCANDO);
        return;
    }
    mlock();
    if (period_requests == UINT32_MAX)
    {
        munlock();
        RET(TIMERR_NOCANDO);
        return;
    }
    period_requests++;
    munlock();
    RET(TIMERR_NOERROR);
}

/* MMRESULT timeEndPeriod(UINT period) */
void rt_timeEndPeriod(_cpu *cpu)
{
    uint32_t p = ARG(0);
    if (p < PERIOD_MIN || p > PERIOD_MAX)
    {
        RET(TIMERR_NOCANDO);
        return;
    }
    mlock();
    if (period_requests == 0)
    {
        munlock();
        RET(TIMERR_NOCANDO);
        return;
    }
    period_requests--;
    munlock();
    RET(TIMERR_NOERROR);
}

/* ---------------------------------------------------------------- timers */

typedef struct mmtimer {
    bool used, periodic;
    uint32_t id, delay_ms, proc, user;
} mmtimer;

static mmtimer timers[MAX_TIMERS];
static uint32_t next_timer_id = 1;

/* The timer thread. A one-shot timer releases its slot after the call. */
static void timer_body(void *ctx, _cpu *cpu)
{
    mmtimer *t = ctx;
    uint64_t period, deadline;
    uint32_t id, proc, user;
    bool periodic;
    mlock();
    period = (uint64_t)t->delay_ms * 1000000u;
    id = t->id;
    proc = t->proc;
    user = t->user;
    periodic = t->periodic;
    munlock();
    deadline = uptime_ns() + period;
    for (;;)
    {
        uint64_t now = uptime_ns();
        if (now < deadline)
        {
            uint64_t ns = deadline - now;
            struct timespec ts = { (time_t)(ns / 1000000000u), (long)(ns % 1000000000u) };
            while (nanosleep(&ts, &ts) != 0)
                if (errno != EINTR) rt_fatal("timer sleep failed: %s", strerror(errno));
            continue;
        }
        /* void CALLBACK TimeProc(UINT id, UINT msg, DWORD_PTR user,
           DWORD_PTR dw1, DWORD_PTR dw2) */
        rt_call(cpu, proc, true, 0, 5, (const uint32_t[]){ id, 0, user, 0, 0 });
        if (!periodic) break;
        deadline += period;
        /* After a long callback, start the next period from now: Windows
           does not call the callback again for each missed period. */
        now = uptime_ns();
        if (deadline + period < now) deadline = now + period;
    }
    mlock();
    memset(t, 0, sizeof(*t));
    munlock();
}

/* MMRESULT timeSetEvent(UINT delay, UINT resolution, LPTIMECALLBACK,
   DWORD_PTR user, UINT flags): the timer id, or 0. */
void rt_timeSetEvent(_cpu *cpu)
{
    uint32_t delay = ARG(0), proc = ARG(2), user = ARG(3), flags = ARG(4), id;
    int i, slot = -1;
    mmtimer *t;
    if (flags & (TIME_CALLBACK_EVENT_SET | TIME_CALLBACK_EVENT_PULSE))
        rt_fatal("timeSetEvent: event callbacks are not supported (flags 0x%x)", flags);
    if (flags & ~(TIME_PERIODIC | TIME_KILL_SYNCHRONOUS))
    {
        RET(0);
        return;
    }
    if (delay < PERIOD_MIN || delay > PERIOD_MAX || proc == 0)
    {
        RET(0);
        return;
    }
    mlock();
    for (i = 0; i < MAX_TIMERS && slot < 0; i++)
        if (!timers[i].used) slot = i;
    if (slot < 0)
    {
        munlock();
        RET(0);
        return;
    }
    t = &timers[slot];
    t->used = true;
    id = next_timer_id++;
    if (next_timer_id == 0) next_timer_id = 1;
    t->id = id;
    t->delay_ms = delay;
    t->proc = proc;
    t->user = user;
    t->periodic = (flags & TIME_PERIODIC) != 0;
    munlock();
    rt_thread_start_runtime(timer_body, t, TIMER_STACK_SIZE);
    RET(id);
}
