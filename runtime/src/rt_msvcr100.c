/*
 * MSVCR100 and MSVCP100: the functions of the Visual C++ 2010 runtime that
 * PillarsOfEternity_data/Plugins/ShroudUnityPlugin.dll imports.
 *
 * The other imports of these DLLs are in rt_seh.c (_except_handler4_common,
 * __CppXcptFilter, _amsg_exit, _purecall, _invoke_watson,
 * _crt_debugger_hook).
 *
 * Memory: malloc, free, operator new and operator delete use the guest heap
 * (rt_heap.c). An allocation of 0 bytes gives a block of 1 byte, as
 * MSVCR100 does.
 *
 * Termination tables: the C runtime start-up code of a DLL (_CRT_INIT)
 * keeps its own atexit table and gives it to __dllonexit. The DLL keeps the
 * begin and end pointers of the table encoded (EncodePointer), but it
 * decodes them before the call: __dllonexit gets the addresses of plain
 * pointers and writes plain pointers back. The function argument is already
 * encoded by the DLL, and __dllonexit stores it unchanged (verified in the
 * atexit code of ShroudUnityPlugin.dll at 0x1107357D). __dllonexit grows
 * the table with the guest heap. _onexit (the table of a program that uses MSVCR100.dll) is not
 * supported: it stops the process.
 *
 * Invalid parameters: the default handler of MSVCR100 ends the process
 * (_invoke_watson). The runtime does the same; _set_invalid_parameter_handler
 * is not imported.
 *
 * C++ exceptions of MSVCP100 (std::_Xlength_error, std::_Xout_of_range):
 * ShroudUnityPlugin.dll has no C++ exception handlers (it does not import
 * __CxxFrameHandler3), so on Windows the exception ends the process through
 * the unhandled exception filter. The runtime stops the process with the
 * message of the exception.
 *
 * sprintf_s: the conversions d, i, u, o, x, X, c, s, p, e, E, f, g, G and %%
 * with the flags, width, precision and the size prefixes h, l, ll, L, I,
 * I32 and I64. The exponent has at least 3 digits, as in MSVCR100.
 * Wide characters (%C, %S, %lc, %ls, %wc, %ws), %a, %A and the forms of
 * MSVCR100 for infinite and NaN values (1.#INF, 1.#QNAN) are not supported:
 * they stop the process. %n is disabled in MSVCR100 (an invalid parameter).
 */
#include "rt.h"
#include "rt_thread.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STATUS_INVALID_CRUNTIME_PARAMETER 0xC0000417u
#define NLOCKS 64u /* MSVCR100 has _TOTAL_LOCKS = 36 (UNCONFIRMED); the runtime accepts more */
#define EXIT_LOCK1 8u
#define FMT_MAX 4096u

/* ---------------------------------------------------------------- errors */

/* rt_seh.c: ends the process with STATUS_INVALID_CRUNTIME_PARAMETER. */
void rt__invoke_watson(_cpu *cpu);

static void invalid_parameter(_cpu *cpu)
{
    rt__invoke_watson(cpu);
}

/* ---------------------------------------------------------------- locks */

static pthread_mutex_t crt_locks[NLOCKS];
static pthread_once_t crt_locks_once = PTHREAD_ONCE_INIT;

static void crt_locks_init(void)
{
    pthread_mutexattr_t a;
    uint32_t i;
    if (pthread_mutexattr_init(&a) != 0 || pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE) != 0)
        rt_fatal("MSVCR100: mutex attribute failed");
    for (i = 0; i < NLOCKS; i++)
        if (pthread_mutex_init(&crt_locks[i], &a) != 0) rt_fatal("MSVCR100: lock initialization failed");
    pthread_mutexattr_destroy(&a);
}

static void crt_lock(uint32_t n)
{
    if (pthread_once(&crt_locks_once, crt_locks_init) != 0) rt_fatal("MSVCR100: pthread_once failed");
    if (n >= NLOCKS) rt_fatal("MSVCR100: _lock(%u): no such lock", n);
    if (pthread_mutex_lock(&crt_locks[n]) != 0) rt_fatal("MSVCR100: lock failed");
}

static void crt_unlock(uint32_t n)
{
    if (n >= NLOCKS) rt_fatal("MSVCR100: _unlock(%u): no such lock", n);
    if (pthread_mutex_unlock(&crt_locks[n]) != 0) rt_fatal("MSVCR100: _unlock(%u) of a lock that the thread does not hold", n);
}

/* void _lock(int) */
void rt__lock(_cpu *cpu)
{
    crt_lock(ARG(0));
}

/* void _unlock(int) */
void rt__unlock(_cpu *cpu)
{
    crt_unlock(ARG(0));
}

/* ---------------------------------------------------------------- initialization and termination */

/* void _initterm(_PVFV *begin, _PVFV *end) */
void rt__initterm(_cpu *cpu)
{
    uint32_t p = ARG(0), end = ARG(1);
    for (; p < end; p += 4)
    {
        uint32_t fn = rd32(p);
        if (fn != 0) (void)rt_call_cdecl0(cpu, fn);
    }
}

/* int _initterm_e(_PIFV *begin, _PIFV *end): stops at the first function
   that does not return 0 and returns its value. */
void rt__initterm_e(_cpu *cpu)
{
    uint32_t p = ARG(0), end = ARG(1), r = 0;
    for (; p < end && r == 0; p += 4)
    {
        uint32_t fn = rd32(p);
        if (fn != 0) r = rt_call_cdecl0(cpu, fn);
    }
    RET(r);
}

/* void *_encoded_null(void) */
void rt__encoded_null(_cpu *cpu)
{
    RET(rt_encode_pointer(0));
}

/* _onexit_t __dllonexit(_onexit_t func, _PVFV **pbegin, _PVFV **pend) */
void rt___dllonexit(_cpu *cpu)
{
    uint32_t func = ARG(0), pbegin = ARG(1), pend = ARG(2), begin, end, n, cap, nb;
    crt_lock(EXIT_LOCK1);
    begin = rd32(pbegin);
    end = rd32(pend);
    if (begin == 0 || end < begin || ((end - begin) & 3u) != 0)
    {
        crt_unlock(EXIT_LOCK1);
        RET(0);
        return;
    }
    n = (end - begin) / 4u;
    cap = rt_heap_size(begin) / 4u;
    nb = begin;
    if (cap < n + 1u)
    {
        /* Grow the table: double it, at least 16 entries more (UNCONFIRMED:
           the growth of MSVCR100, which is not visible to the DLL). */
        uint32_t more = n < 16u ? 16u : n;
        if (n > 0x3FFFFFFFu / 4u - more) nb = 0;
        else nb = rt_heap_realloc(begin, (n + more) * 4u);
        if (nb == 0)
        {
            crt_unlock(EXIT_LOCK1);
            RET(0);
            return;
        }
    }
    wr32(nb + 4u * n, func);
    wr32(pbegin, nb);
    wr32(pend, nb + 4u * (n + 1u));
    crt_unlock(EXIT_LOCK1);
    RET(func);
}

/* _onexit_t _onexit(_onexit_t func) */
void rt__onexit(_cpu *cpu)
{
    (void)cpu;
    rt_fatal("MSVCR100: _onexit (the termination table of a program that uses MSVCR100.dll) is not supported by the runtime");
}

/* void __clean_type_info_names_internal(__type_info_node *root):
   free the names that type_info::name made (node: _MemPtr 0, _Next 4). */
void rt___clean_type_info_names_internal(_cpu *cpu)
{
    uint32_t root = ARG(0), node;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    if (pthread_mutex_lock(&lock) != 0) rt_fatal("MSVCR100: lock failed");
    node = rd32(root + 4);
    while (node != 0)
    {
        uint32_t next = rd32(node + 4);
        if (rd32(node) != 0) rt_heap_free(rd32(node));
        rt_heap_free(node);
        node = next;
    }
    wr32(root + 4, 0);
    if (pthread_mutex_unlock(&lock) != 0) rt_fatal("MSVCR100: unlock failed");
}

/* ---------------------------------------------------------------- memory */

static uint32_t crt_malloc(uint32_t size)
{
    return rt_heap_alloc(size == 0 ? 1u : size);
}

/* void *_malloc_crt(size_t) */
void rt__malloc_crt(_cpu *cpu)
{
    RET(crt_malloc(ARG(0)));
}

/* void free(void *) */
void rt_msvcr100_free(_cpu *cpu)
{
    if (ARG(0) != 0) rt_heap_free(ARG(0));
}

/* void *operator new(size_t) */
void rt_msvcr100_new(_cpu *cpu)
{
    uint32_t p = crt_malloc(ARG(0));
    if (p == 0) rt_fatal("MSVCR100: operator new(%u): out of guest memory (std::bad_alloc, no handler in the DLL)", ARG(0));
    RET(p);
}

/* void operator delete(void *) */
void rt_msvcr100_delete(_cpu *cpu)
{
    if (ARG(0) != 0) rt_heap_free(ARG(0));
}

/* void operator delete[](void *) */
void rt_msvcr100_delete_array(_cpu *cpu)
{
    if (ARG(0) != 0) rt_heap_free(ARG(0));
}

/* void *_aligned_malloc(size_t size, size_t alignment): the address of the
   heap block is in the dword before the aligned address. */
void rt__aligned_malloc(_cpu *cpu)
{
    uint32_t size = ARG(0), align = ARG(1), block, p;
    if (align == 0 || (align & (align - 1u)) != 0)
    {
        invalid_parameter(cpu);
        return;
    }
    if (align < 4u) align = 4u;
    if (size > UINT32_MAX - (align - 1u) - 4u)
    {
        RET(0);
        return;
    }
    block = crt_malloc(size + (align - 1u) + 4u);
    if (block == 0)
    {
        RET(0);
        return;
    }
    p = (block + 4u + (align - 1u)) & ~(align - 1u);
    wr32(p - 4u, block);
    RET(p);
}

/* void _aligned_free(void *) */
void rt__aligned_free(_cpu *cpu)
{
    uint32_t p = ARG(0);
    if (p == 0) return;
    rt_heap_free(rd32((p & ~3u) - 4u));
}

/* void *memset(void *, int, size_t) */
void rt_msvcr100_memset(_cpu *cpu)
{
    if (ARG(2) != 0) memset(G2H(ARG(0)), (int)(uint8_t)ARG(1), ARG(2));
    RET(ARG(0));
}

/* void *memcpy(void *, const void *, size_t): MSVCR100 copies overlapping
   ranges as memmove does. */
void rt_msvcr100_memcpy(_cpu *cpu)
{
    if (ARG(2) != 0) memmove(G2H(ARG(0)), G2H(ARG(1)), ARG(2));
    RET(ARG(0));
}

/* void *memmove(void *, const void *, size_t) */
void rt_msvcr100_memmove(_cpu *cpu)
{
    if (ARG(2) != 0) memmove(G2H(ARG(0)), G2H(ARG(1)), ARG(2));
    RET(ARG(0));
}

/* ---------------------------------------------------------------- strings */

/* char *strchr(const char *, int) */
void rt_msvcr100_strchr(_cpu *cpu)
{
    uint32_t s = ARG(0);
    uint8_t c = (uint8_t)ARG(1), ch;
    for (;; s++)
    {
        ch = rd8(s);
        if (ch == c)
        {
            RET(s);
            return;
        }
        if (ch == 0) break;
    }
    RET(0);
}

/* char *strstr(const char *, const char *) */
void rt_msvcr100_strstr(_cpu *cpu)
{
    uint32_t s = ARG(0), f = ARG(1), i;
    if (rd8(f) == 0)
    {
        RET(s);
        return;
    }
    for (; rd8(s) != 0; s++)
    {
        for (i = 0; rd8(f + i) != 0 && rd8(s + i) == rd8(f + i); i++) {}
        if (rd8(f + i) == 0)
        {
            RET(s);
            return;
        }
    }
    RET(0);
}

static bool is_space(uint8_t c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

/* strtol and strtoul with 32-bit results, as MSVCR100 (strtoxl). Without
   digits, *end is the start of the string, also after "0x". */
static uint32_t str_to_ul(uint32_t s, uint32_t endp, int base, bool is_signed)
{
    uint32_t p = s, limit, value = 0;
    bool neg = false, overflow = false, digits = false;
    uint8_t c;
    if (base == 1 || base < 0 || base > 36)
    {
        if (endp != 0) wr32(endp, s);
        return 0;
    }
    while (is_space(rd8(p))) p++;
    c = rd8(p);
    if (c == '-' || c == '+')
    {
        neg = c == '-';
        p++;
    }
    if (base == 0)
    {
        if (rd8(p) != '0') base = 10;
        else if (rd8(p + 1) == 'x' || rd8(p + 1) == 'X') base = 16;
        else base = 8;
    }
    if (base == 16 && rd8(p) == '0' && (rd8(p + 1) == 'x' || rd8(p + 1) == 'X')) p += 2;
    limit = UINT32_MAX / (uint32_t)base;
    for (;; p++)
    {
        uint32_t d;
        c = rd8(p);
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10u;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10u;
        else break;
        if (d >= (uint32_t)base) break;
        digits = true;
        if (value > limit || (value == limit && d > UINT32_MAX % (uint32_t)base)) overflow = true;
        else value = value * (uint32_t)base + d;
    }
    if (!digits)
    {
        if (endp != 0) wr32(endp, s);
        return 0;
    }
    if (endp != 0) wr32(endp, p);
    if (is_signed)
    {
        if (overflow || (!neg && value > 0x7FFFFFFFu) || (neg && value > 0x80000000u)) return neg ? 0x80000000u : 0x7FFFFFFFu;
        return neg ? (uint32_t)(-(int64_t)value) : value;
    }
    if (overflow) return UINT32_MAX;
    return neg ? (uint32_t)(-(int64_t)value) : value;
}

/* long strtol(const char *, char **, int) */
void rt_msvcr100_strtol(_cpu *cpu)
{
    RET(str_to_ul(ARG(0), ARG(1), (int)ARG(2), true));
}

/* unsigned long strtoul(const char *, char **, int) */
void rt_msvcr100_strtoul(_cpu *cpu)
{
    RET(str_to_ul(ARG(0), ARG(1), (int)ARG(2), false));
}

/* double strtod(const char *, char **): the decimal syntax of MSVCR100
   ([sign] digits [. digits] [e [sign] digits]). The result is in st(0). */
void rt_msvcr100_strtod(_cpu *cpu)
{
    uint32_t s = ARG(0), endp = ARG(1), p = s, start, n = 0, mant = 0;
    char buf[512];
    double v = 0.0;
    while (is_space(rd8(p))) p++;
    start = p;
    if (rd8(p) == '+' || rd8(p) == '-') p++;
    while (rd8(p) >= '0' && rd8(p) <= '9') { p++; mant++; }
    if (rd8(p) == '.')
    {
        p++;
        while (rd8(p) >= '0' && rd8(p) <= '9') { p++; mant++; }
    }
    if (mant == 0)
    {
        if (endp != 0) wr32(endp, s);
        RETF(0.0);
        return;
    }
    if (rd8(p) == 'e' || rd8(p) == 'E')
    {
        uint32_t q = p + 1, ed = 0;
        if (rd8(q) == '+' || rd8(q) == '-') q++;
        while (rd8(q) >= '0' && rd8(q) <= '9') { q++; ed++; }
        if (ed != 0) p = q;
    }
    if (p - start >= sizeof buf) rt_fatal("MSVCR100: strtod: number of %u characters", p - start);
    for (n = 0; start + n < p; n++) buf[n] = (char)rd8(start + n);
    buf[n] = 0;
    v = strtod(buf, NULL);
    if (endp != 0) wr32(endp, p);
    RETF(v);
}

/* ---------------------------------------------------------------- sprintf_s */

typedef struct out_buf {
    char b[FMT_MAX];
    uint32_t n;
} out_buf;

static void out_ch(out_buf *o, char c)
{
    if (o->n + 1u >= FMT_MAX) rt_fatal("MSVCR100: sprintf_s output longer than %u characters", FMT_MAX - 1u);
    o->b[o->n++] = c;
}

static void out_pad(out_buf *o, char c, int n)
{
    for (; n > 0; n--) out_ch(o, c);
}

/* Apply width and flags to text (sign or prefix of length pre at the
   start, for zero padding). */
static void out_field(out_buf *o, const char *text, int len, int pre, int width, bool left, bool zero)
{
    int i;
    if (left)
    {
        for (i = 0; i < len; i++) out_ch(o, text[i]);
        out_pad(o, ' ', width - len);
        return;
    }
    if (zero)
    {
        for (i = 0; i < pre; i++) out_ch(o, text[i]);
        out_pad(o, '0', width - len);
        for (i = pre; i < len; i++) out_ch(o, text[i]);
        return;
    }
    out_pad(o, ' ', width - len);
    for (i = 0; i < len; i++) out_ch(o, text[i]);
}

/* Format into o. Returns false for an invalid format (invalid parameter). */
static bool format(out_buf *o, uint32_t fmt, uint32_t args)
{
    for (;;)
    {
        uint8_t c = rd8(fmt++);
        bool left = false, plus = false, space = false, alt = false, zero = false, i64 = false, sh = false;
        int width = 0, prec = -1;
        char tmp[512];
        int len = 0, pre = 0;
        if (c == 0) return true;
        if (c != '%')
        {
            out_ch(o, (char)c);
            continue;
        }
        for (;; fmt++)
        {
            c = rd8(fmt);
            if (c == '-') left = true;
            else if (c == '+') plus = true;
            else if (c == ' ') space = true;
            else if (c == '#') alt = true;
            else if (c == '0') zero = true;
            else break;
        }
        if (rd8(fmt) == '*')
        {
            width = (int32_t)rd32(args);
            args += 4;
            fmt++;
            if (width < 0)
            {
                left = true;
                width = width == INT32_MIN ? INT32_MAX : -width;
            }
        }
        else
            while (rd8(fmt) >= '0' && rd8(fmt) <= '9') width = width * 10 + (rd8(fmt++) - '0');
        if (width > (int)FMT_MAX) rt_fatal("MSVCR100: sprintf_s width %d", width);
        if (rd8(fmt) == '.')
        {
            fmt++;
            prec = 0;
            if (rd8(fmt) == '*')
            {
                prec = (int32_t)rd32(args);
                args += 4;
                fmt++;
                if (prec < 0) prec = -1;
            }
            else
                while (rd8(fmt) >= '0' && rd8(fmt) <= '9') prec = prec * 10 + (rd8(fmt++) - '0');
            if (prec > 400) rt_fatal("MSVCR100: sprintf_s precision %d", prec);
        }
        c = rd8(fmt);
        if (c == 'h')
        {
            fmt++;
            if (rd8(fmt) == 'h') rt_fatal("MSVCR100: sprintf_s size prefix hh is not supported (not in Visual C++ 2010)");
            sh = true;
        }
        else if (c == 'l')
        {
            fmt++;
            if (rd8(fmt) == 'l')
            {
                fmt++;
                i64 = true;
            }
        }
        else if (c == 'L')
            fmt++;
        else if (c == 'w')
            rt_fatal("MSVCR100: sprintf_s with the wide prefix w is not supported by the runtime");
        else if (c == 'I')
        {
            if (rd8(fmt + 1) == '6' && rd8(fmt + 2) == '4')
            {
                fmt += 3;
                i64 = true;
            }
            else if (rd8(fmt + 1) == '3' && rd8(fmt + 2) == '2')
                fmt += 3;
            else
                fmt++;
        }
        c = rd8(fmt++);
        switch (c)
        {
        case '%':
            out_ch(o, '%');
            break;
        case 'c':
        {
            uint32_t v = rd32(args);
            args += 4;
            tmp[0] = (char)(uint8_t)v;
            out_field(o, tmp, 1, 0, width, left, false);
            break;
        }
        case 's':
        {
            uint32_t s = rd32(args), n;
            const char *nul = "(null)";
            args += 4;
            if (s == 0)
                for (n = 0; nul[n] != 0 && (prec < 0 || (int)n < prec); n++) tmp[n] = nul[n];
            else
            {
                char *big;
                for (n = 0; (prec < 0 || (int)n < prec) && rd8(s + n) != 0; n++)
                    if (n >= FMT_MAX) rt_fatal("MSVCR100: sprintf_s string argument longer than %u characters", FMT_MAX);
                big = malloc(n + 1u);
                if (big == NULL) rt_fatal("out of host memory");
                memcpy(big, G2H(s), n);
                if (!left) out_pad(o, ' ', width - (int)n);
                for (uint32_t k = 0; k < n; k++) out_ch(o, big[k]);
                if (left) out_pad(o, ' ', width - (int)n);
                free(big);
                break;
            }
            out_field(o, tmp, (int)n, 0, width, left, false);
            break;
        }
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p':
        {
            uint64_t v;
            bool neg = false, sgn = c == 'd' || c == 'i';
            unsigned b = (c == 'o') ? 8u : (c == 'x' || c == 'X' || c == 'p') ? 16u : 10u;
            const char *dig = (c == 'X' || c == 'p') ? "0123456789ABCDEF" : "0123456789abcdef";
            char num[32];
            int nd = 0, k;
            if (c == 'p')
            {
                prec = 8;
                i64 = false;
                alt = false;
            }
            if (i64)
            {
                v = (uint64_t)rd32(args) | ((uint64_t)rd32(args + 4) << 32);
                args += 8;
                if (sgn && (int64_t)v < 0)
                {
                    neg = true;
                    v = 0u - v;
                }
            }
            else
            {
                uint32_t a = rd32(args);
                args += 4;
                if (sh) a = sgn ? (uint32_t)(int32_t)(int16_t)a : (a & 0xFFFFu);
                if (sgn && (int32_t)a < 0)
                {
                    neg = true;
                    v = (uint64_t)(-(int64_t)(int32_t)a);
                }
                else
                    v = a;
            }
            while (v != 0)
            {
                num[nd++] = dig[v % b];
                v /= b;
            }
            if (neg) tmp[len++] = '-';
            else if (sgn && plus) tmp[len++] = '+';
            else if (sgn && space) tmp[len++] = ' ';
            if (alt && b == 16u && nd > 0)
            {
                tmp[len++] = '0';
                tmp[len++] = c == 'X' ? 'X' : 'x';
            }
            pre = len;
            /* The 0 flag has no effect with a precision. */
            zero = zero && prec < 0;
            if (prec < 0) prec = 1;
            if (alt && b == 8u && nd >= prec) prec = nd + 1;
            for (k = nd; k < prec; k++) tmp[len++] = '0';
            for (k = nd - 1; k >= 0; k--) tmp[len++] = num[k];
            out_field(o, tmp, len, pre, width, left, zero);
            break;
        }
        case 'e': case 'E': case 'f': case 'g': case 'G':
        {
            double v = rdf64(args);
            char spec[16], body[480];
            int sl = 0, bl, e;
            args += 8;
            if (!isfinite(v)) rt_fatal("MSVCR100: sprintf_s of an infinite or NaN value is not supported by the runtime");
            spec[sl++] = '%';
            if (plus) spec[sl++] = '+';
            else if (space) spec[sl++] = ' ';
            if (alt) spec[sl++] = '#';
            spec[sl++] = '.';
            spec[sl++] = '*';
            spec[sl++] = (char)c;
            spec[sl] = 0;
            bl = snprintf(body, sizeof body, spec, prec < 0 ? 6 : prec, v);
            if (bl < 0 || bl >= (int)sizeof body - 2) rt_fatal("MSVCR100: sprintf_s floating-point text too long");
            /* At least 3 exponent digits. */
            for (e = 0; e < bl; e++)
                if (body[e] == 'e' || body[e] == 'E') break;
            if (e < bl && e + 2 < bl)
            {
                int digits = bl - (e + 2);
                if (digits < 3)
                {
                    int add = 3 - digits;
                    memmove(body + e + 2 + add, body + e + 2, (size_t)digits + 1u);
                    memset(body + e + 2, '0', (size_t)add);
                    bl += add;
                }
            }
            pre = (body[0] == '-' || body[0] == '+' || body[0] == ' ') ? 1 : 0;
            out_field(o, body, bl, pre, width, left, zero);
            break;
        }
        case 'n':
            return false;
        default:
            if (c == 'C' || c == 'S' || c == 'a' || c == 'A')
                rt_fatal("MSVCR100: sprintf_s conversion %%%c is not supported by the runtime", c);
            return false;
        }
    }
}

/* int sprintf_s(char *buffer, size_t size, const char *format, ...) */
void rt_msvcr100_sprintf_s(_cpu *cpu)
{
    uint32_t dst = ARG(0), size = ARG(1), fmt = ARG(2);
    out_buf *o;
    if (dst == 0 || size == 0 || fmt == 0)
    {
        invalid_parameter(cpu);
        return;
    }
    o = malloc(sizeof *o);
    if (o == NULL) rt_fatal("out of host memory");
    o->n = 0;
    if (!format(o, fmt, ARGADR(3)))
    {
        free(o);
        wr8(dst, 0);
        invalid_parameter(cpu);
        return;
    }
    if (o->n + 1u > size)
    {
        free(o);
        wr8(dst, 0);
        invalid_parameter(cpu);
        return;
    }
    memcpy(G2H(dst), o->b, o->n);
    wr8(dst + o->n, 0);
    RET(o->n);
    free(o);
}

/* ---------------------------------------------------------------- math and random numbers */

/* rand: the generator of MSVCR100, one state for each thread (seed 1;
   srand is not imported). */
void rt_msvcr100_rand(_cpu *cpu)
{
    static _Thread_local uint32_t holdrand = 1u;
    holdrand = holdrand * 214013u + 2531011u;
    RET((holdrand >> 16) & 0x7FFFu);
}

/* _CIacos: the argument and the result are in st(0). */
void rt__CIacos(_cpu *cpu)
{
    double x = rt_fpu_pop(cpu);
    rt_fpu_push(cpu, acos(x));
}

/* __libm_sse2_cosf and __libm_sse2_sinf: the argument and the result are
   the low float of xmm0. */
void rt___libm_sse2_cosf(_cpu *cpu)
{
    cpu->_xmm[0].f[0] = cosf(cpu->_xmm[0].f[0]);
}

void rt___libm_sse2_sinf(_cpu *cpu)
{
    cpu->_xmm[0].f[0] = sinf(cpu->_xmm[0].f[0]);
}

/* ---------------------------------------------------------------- MSVCP100 */

static void cxx_exception(uint32_t msg, const char *type)
{
    char text[256];
    uint32_t i;
    for (i = 0; msg != 0 && i + 1u < sizeof text && rd8(msg + i) != 0; i++) text[i] = (char)rd8(msg + i);
    text[i] = 0;
    rt_fatal("MSVCP100: unhandled C++ exception %s (\"%s\"); the DLL has no exception handler", type, text);
}

/* void std::_Xlength_error(const char *) */
void rt_msvcp100_Xlength_error(_cpu *cpu)
{
    cxx_exception(ARG(0), "std::length_error");
}

/* void std::_Xout_of_range(const char *) */
void rt_msvcp100_Xout_of_range(_cpu *cpu)
{
    cxx_exception(ARG(0), "std::out_of_range");
}

/* std::_Raise_handler: a function pointer, NULL (no handler). */
char X__Q__Raise__handler_Astd_A_A3P6AXABVexception_Astdext_A_A_AZA_asm2c[4] __attribute__((aligned(4)));
