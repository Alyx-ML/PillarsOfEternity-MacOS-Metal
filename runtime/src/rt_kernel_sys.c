/*
 * KERNEL32: version and system information, processor features, the
 * computer name, the command line, the startup information, the process
 * environment, pointer encoding, the last error and debug output (wide).
 *
 * The emulated system is Windows XP Service Pack 3 (5.1.2600) on one
 * Pentium 4 processor package (family 15, model 2, stepping 9: the values
 * of the cpuid helper of the translated code) with the host processor
 * count.
 */
#include "rt.h"
#include "rt_module.h"
#include "rt_text.h"
#include "rt_thread.h"

#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ERROR_BUFFER_OVERFLOW 111u
#define ERROR_ENVVAR_NOT_FOUND 203u

/* ---------------------------------------------------------------- version */

/* DWORD GetVersion(void): build number in the high word, minor and major
   version in the low word. Windows XP: 2600, 5.1. */
void rt_GetVersion(_cpu *cpu)
{
    RET((2600u << 16) | (1u << 8) | 5u);
}

/* BOOL GetVersionExW(LPOSVERSIONINFOW): OSVERSIONINFOW (276 bytes) or
   OSVERSIONINFOEXW (284 bytes). The values are those of GetVersionExA. */
void rt_GetVersionExW(_cpu *cpu)
{
    uint32_t vi = ARG(0), size = rd32(vi);
    if (size != 276u && size != 284u)
    {
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        RET(0);
        return;
    }
    memset(G2H(vi + 4), 0, size - 4u);
    wr32(vi + 4, 5);     /* dwMajorVersion */
    wr32(vi + 8, 1);     /* dwMinorVersion */
    wr32(vi + 12, 2600); /* dwBuildNumber */
    wr32(vi + 16, 2);    /* VER_PLATFORM_WIN32_NT */
    if (!rt_put_wfield(vi + 20, 128, "Service Pack 3")) rt_fatal("GetVersionExW: service pack field");
    if (size == 284u)
    {
        wr16(vi + 276, 3);     /* wServicePackMajor */
        wr16(vi + 278, 0);     /* wServicePackMinor */
        wr16(vi + 280, 0x100); /* wSuiteMask: VER_SUITE_SINGLEUSERTS */
        wr8(vi + 282, 1);      /* wProductType: VER_NT_WORKSTATION */
    }
    RET(1);
}

/* ULONGLONG VerSetConditionMask(ULONGLONG mask, DWORD type, BYTE op): 3
   bits for each type, as in ntdll. Only the highest type bit of the call
   is used. */
void rt_VerSetConditionMask(_cpu *cpu)
{
    uint64_t mask = ((uint64_t)ARG(1) << 32) | ARG(0);
    uint32_t type = ARG(2), op = ARG(3) & 0xFFu;
    unsigned shift;
    if (type == 0 || op == 0)
    {
        RET64(mask);
        return;
    }
    op &= 7u;
    if (type & 0x80u) shift = 21;      /* VER_PRODUCT_TYPE */
    else if (type & 0x40u) shift = 18; /* VER_SUITENAME */
    else if (type & 0x20u) shift = 15; /* VER_SERVICEPACKMAJOR */
    else if (type & 0x10u) shift = 12; /* VER_SERVICEPACKMINOR */
    else if (type & 0x08u) shift = 9;  /* VER_PLATFORMID */
    else if (type & 0x04u) shift = 6;  /* VER_BUILDNUMBER */
    else if (type & 0x02u) shift = 3;  /* VER_MAJORVERSION */
    else if (type & 0x01u) shift = 0;  /* VER_MINORVERSION */
    else
    {
        RET64(mask);
        return;
    }
    RET64(mask | ((uint64_t)op << shift));
}

#define VER_EQUAL 1u
#define VER_GREATER 2u
#define VER_GREATER_EQUAL 3u
#define VER_LESS 4u
#define VER_LESS_EQUAL 5u
#define VER_AND 6u
#define VER_OR 7u
#define ERROR_BAD_ARGUMENTS 160u
#define ERROR_OLD_WIN_VERSION 1150u

/* Compare a system value with a requested value. Returns -1 for a
   condition that is not valid. */
static int ver_compare(uint32_t sys, uint32_t want, uint32_t cond)
{
    switch (cond)
    {
    case VER_EQUAL: return sys == want;
    case VER_GREATER: return sys > want;
    case VER_GREATER_EQUAL: return sys >= want;
    case VER_LESS: return sys < want;
    case VER_LESS_EQUAL: return sys <= want;
    default: return -1;
    }
}

/* BOOL VerifyVersionInfoW(LPOSVERSIONINFOEXW, DWORD type, ULONGLONG mask):
   the rules of RtlVerifyVersionInfo against Windows XP SP3 (5.1.2600, SP
   3.0, VER_PLATFORM_WIN32_NT, suite VER_SUITE_SINGLEUSERTS, product type
   VER_NT_WORKSTATION). Major version, minor version and the service pack
   numbers form one ordered value: a lower part counts only when the higher
   parts are equal, and the first condition of the group applies to all of
   them. */
void rt_VerifyVersionInfoW(_cpu *cpu)
{
    static const uint32_t sys_major = 5, sys_minor = 1, sys_build = 2600, sys_platform = 2;
    static const uint32_t sys_spmajor = 3, sys_spminor = 0, sys_suite = 0x100, sys_product = 1;
    uint32_t vi = ARG(0), type = ARG(1);
    uint64_t mask = ((uint64_t)ARG(3) << 32) | ARG(2);
    int ok = 1, r;
    if (vi == 0 || !rt_va_is_committed(vi, 284) || rd32(vi) != 284u || type == 0 || mask == 0)
    {
        rt_set_last_error(vi == 0 || (vi != 0 && rt_va_is_committed(vi, 4) && rd32(vi) != 284u) ? ERROR_INVALID_PARAMETER : ERROR_BAD_ARGUMENTS);
        RET(0);
        return;
    }
#define COND(sh) ((uint32_t)(mask >> (sh)) & 7u)
    if (type & 0x80u)
    {
        r = ver_compare(sys_product, rd8(vi + 282), COND(21));
        if (r < 0) goto bad;
        ok &= r;
    }
    if (type & 0x40u)
    {
        uint32_t want = rd16(vi + 280);
        if (COND(18) == VER_AND) ok &= (sys_suite & want) == want;
        else if (COND(18) == VER_OR) ok &= (sys_suite & want) != 0;
        else goto bad;
    }
    if (type & 0x08u)
    {
        r = ver_compare(sys_platform, rd32(vi + 16), COND(9));
        if (r < 0) goto bad;
        ok &= r;
    }
    if (type & 0x04u)
    {
        r = ver_compare(sys_build, rd32(vi + 12), COND(6));
        if (r < 0) goto bad;
        ok &= r;
    }
    if (type & 0x33u)
    {
        uint32_t cond = 0;
        bool next = true;
        if (type & 0x02u)
        {
            cond = COND(3);
            r = ver_compare(sys_major, rd32(vi + 4), cond);
            if (r < 0) goto bad;
            ok &= r;
            next = sys_major == rd32(vi + 4) && cond >= VER_EQUAL && cond <= VER_LESS_EQUAL;
        }
        if ((type & 0x01u) && next)
        {
            if (cond == 0) cond = COND(0);
            r = ver_compare(sys_minor, rd32(vi + 8), cond);
            if (r < 0) goto bad;
            ok &= r;
            next = sys_minor == rd32(vi + 8) && cond >= VER_EQUAL && cond <= VER_LESS_EQUAL;
        }
        if ((type & 0x20u) && next)
        {
            if (cond == 0) cond = COND(15);
            r = ver_compare(sys_spmajor, rd16(vi + 276), cond);
            if (r < 0) goto bad;
            ok &= r;
            next = sys_spmajor == rd16(vi + 276) && cond >= VER_EQUAL && cond <= VER_LESS_EQUAL;
        }
        if ((type & 0x10u) && next)
        {
            if (cond == 0) cond = COND(12);
            r = ver_compare(sys_spminor, rd16(vi + 278), cond);
            if (r < 0) goto bad;
            ok &= r;
        }
    }
#undef COND
    if (!ok)
    {
        rt_set_last_error(ERROR_OLD_WIN_VERSION);
        RET(0);
        return;
    }
    RET(1);
    return;
bad:
    rt_set_last_error(ERROR_INVALID_PARAMETER);
    RET(0);
}

/* ---------------------------------------------------------------- system information */

static const char *fixed_computer_name;
static uint32_t fixed_processors;
static atomic_bool identity_used;

void rt_set_fixed_host_identity(const char *name, uint32_t processors)
{
    size_t i, n = strlen(name);
    if (atomic_load(&identity_used)) rt_fatal("rt_set_fixed_host_identity: called after the first use");
    if (n == 0 || n > 15) rt_fatal("rt_set_fixed_host_identity: the computer name %s is not 1 to 15 characters", name);
    for (i = 0; i < n; i++)
        if (!((name[i] >= 'A' && name[i] <= 'Z') || (name[i] >= '0' && name[i] <= '9') || name[i] == '-'))
            rt_fatal("rt_set_fixed_host_identity: the computer name %s has a character other than A-Z, 0-9, '-'", name);
    if (processors < 1 || processors > 32) rt_fatal("rt_set_fixed_host_identity: %u processors", processors);
    fixed_computer_name = name;
    fixed_processors = processors;
}

uint32_t rt_host_processor_count(void)
{
    long n;
    atomic_store(&identity_used, true);
    if (fixed_processors != 0) return fixed_processors;
    n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0) rt_fatal("sysconf(_SC_NPROCESSORS_ONLN) failed");
    return n > 32 ? 32u : (uint32_t)n;
}

/* void GetSystemInfo(LPSYSTEM_INFO), 36 bytes. */
void rt_GetSystemInfo(_cpu *cpu)
{
    uint32_t si = ARG(0), n = rt_host_processor_count();
    wr16(si + 0, 0);                   /* PROCESSOR_ARCHITECTURE_INTEL */
    wr16(si + 2, 0);
    wr32(si + 4, 0x1000);              /* dwPageSize */
    wr32(si + 8, 0x10000);             /* lpMinimumApplicationAddress */
    wr32(si + 12, 0x7FFEFFFF);         /* lpMaximumApplicationAddress */
    wr32(si + 16, n >= 32 ? 0xFFFFFFFFu : (1u << n) - 1u); /* dwActiveProcessorMask */
    wr32(si + 20, n);                  /* dwNumberOfProcessors */
    wr32(si + 24, 586);                /* PROCESSOR_INTEL_PENTIUM */
    wr32(si + 28, RT_GRANULE);         /* dwAllocationGranularity */
    wr16(si + 32, 15);                 /* wProcessorLevel: family */
    wr16(si + 34, 0x0209);             /* wProcessorRevision: model 2, stepping 9 */
}

/* BOOL IsProcessorFeaturePresent(DWORD feature). The features of the
   cpuid helper: CMPXCHG8B, MMX, SSE, RDTSC, SSE2. */
void rt_IsProcessorFeaturePresent(_cpu *cpu)
{
    switch (ARG(0))
    {
    case 2:  /* PF_COMPARE_EXCHANGE_DOUBLE */
    case 3:  /* PF_MMX_INSTRUCTIONS_AVAILABLE */
    case 6:  /* PF_XMMI_INSTRUCTIONS_AVAILABLE */
    case 8:  /* PF_RDTSC_INSTRUCTION_AVAILABLE */
    case 10: /* PF_XMMI64_INSTRUCTIONS_AVAILABLE */
        RET(1);
        break;
    default:
        RET(0);
        break;
    }
}

/* UINT GetSystemDirectoryA(LPSTR buf, UINT size) */
void rt_GetSystemDirectoryA(_cpu *cpu)
{
    RET(rt_put_astr(ARG(0), ARG(1), RT_GUEST_SYSTEM_DIR));
}

/* UINT GetWindowsDirectoryA(LPSTR buf, UINT size) */
void rt_GetWindowsDirectoryA(_cpu *cpu)
{
    RET(rt_put_astr(ARG(0), ARG(1), RT_GUEST_WINDOWS_DIR));
}

/* ---------------------------------------------------------------- computer name */

static char computer_name[16];
static pthread_once_t computer_once = PTHREAD_ONCE_INIT;

/* The NetBIOS form of the host name: the first label, in upper case, with
   the characters A-Z, 0-9 and '-', at most 15 characters. */
static void make_computer_name(void)
{
    char host[256];
    size_t i, n = 0;
    if (fixed_computer_name != NULL)
    {
        snprintf(computer_name, sizeof computer_name, "%s", fixed_computer_name);
        return;
    }
    if (gethostname(host, sizeof(host)) != 0) rt_fatal("gethostname failed");
    host[sizeof(host) - 1] = 0;
    for (i = 0; host[i] != 0 && host[i] != '.' && n < 15; i++)
    {
        unsigned char c = (unsigned char)host[i];
        if (isascii(c) && (isalnum(c) || c == '-')) computer_name[n++] = (char)toupper(c);
    }
    computer_name[n] = 0;
    if (n == 0) rt_fatal("the host name \"%s\" has no character for a Windows computer name (A-Z, 0-9, '-')", host);
}

const char *rt_computer_name(void)
{
    atomic_store(&identity_used, true);
    if (pthread_once(&computer_once, make_computer_name) != 0) rt_fatal("computer name setup failed");
    return computer_name;
}

/* BOOL GetComputerNameW(LPWSTR buf, LPDWORD size) */
void rt_GetComputerNameW(_cpu *cpu)
{
    uint32_t buf = ARG(0), psize = ARG(1);
    const char *name = rt_computer_name();
    uint32_t len = (uint32_t)strlen(name), size;
    if (psize == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    size = rd32(psize);
    if (buf == 0 || size < len + 1u)
    {
        wr32(psize, len + 1u);
        rt_set_last_error(ERROR_BUFFER_OVERFLOW);
        RET(0);
        return;
    }
    rt_put_wstr(buf, size, name);
    wr32(psize, len);
    RET(1);
}

/* ---------------------------------------------------------------- command line */

static pthread_mutex_t cmdline_lock = PTHREAD_MUTEX_INITIALIZER;
static char *cmdline_utf8;
static uint32_t cmdline_a;

void rt_set_command_line(const char *utf8)
{
    size_t units;
    if (!rt_utf16_length(utf8, &units)) rt_fatal("rt_set_command_line: the command line is not valid UTF-8");
    if (pthread_mutex_lock(&cmdline_lock) != 0) rt_fatal("cmdline_lock lock failed");
    if (cmdline_utf8 != NULL) rt_fatal("rt_set_command_line called twice");
    cmdline_utf8 = strdup(utf8);
    if (cmdline_utf8 == NULL) rt_fatal("out of host memory (command line)");
    if (pthread_mutex_unlock(&cmdline_lock) != 0) rt_fatal("cmdline_lock unlock failed");
}

static uint32_t cmdline_w;

/* LPWSTR GetCommandLineW(void): one wide copy for the process life. */
void rt_GetCommandLineW(_cpu *cpu)
{
    if (pthread_mutex_lock(&cmdline_lock) != 0) rt_fatal("cmdline_lock lock failed");
    if (cmdline_utf8 == NULL) rt_fatal("GetCommandLineW before the host set the command line");
    if (cmdline_w == 0)
    {
        uint32_t need = rt_put_wstr(0, 0, cmdline_utf8);
        cmdline_w = rt_heap_alloc(2u * need);
        if (cmdline_w == 0) rt_fatal("no guest memory for the command line");
        rt_put_wstr(cmdline_w, need, cmdline_utf8);
    }
    if (pthread_mutex_unlock(&cmdline_lock) != 0) rt_fatal("cmdline_lock unlock failed");
    RET(cmdline_w);
}

/* LPSTR GetCommandLineA(void): one ANSI copy for the process life. */
void rt_GetCommandLineA(_cpu *cpu)
{
    if (pthread_mutex_lock(&cmdline_lock) != 0) rt_fatal("cmdline_lock lock failed");
    if (cmdline_utf8 == NULL) rt_fatal("GetCommandLineA before the host set the command line");
    if (cmdline_a == 0)
    {
        uint32_t need = rt_put_astr(0, 0, cmdline_utf8);
        cmdline_a = rt_heap_alloc(need);
        if (cmdline_a == 0) rt_fatal("no guest memory for the command line");
        rt_put_astr(cmdline_a, need, cmdline_utf8);
    }
    if (pthread_mutex_unlock(&cmdline_lock) != 0) rt_fatal("cmdline_lock unlock failed");
    RET(cmdline_a);
}

/* void GetStartupInfoW(LPSTARTUPINFOW): no startup information. */
void rt_GetStartupInfoW(_cpu *cpu)
{
    uint32_t si = ARG(0);
    memset(G2H(si), 0, 68);
    wr32(si, 68);
}

/* ---------------------------------------------------------------- environment */

/*
 * The environment is a list of UTF-8 "NAME=VALUE" strings, sorted by name
 * without case, as RtlSetEnvironmentVariable keeps it. Names compare
 * without case for the ASCII letters.
 */
static pthread_mutex_t env_lock = PTHREAD_MUTEX_INITIALIZER;
static char **env;
static size_t env_count, env_cap;
static bool env_ready;

static int name_cmp(const char *a, size_t alen, const char *b, size_t blen)
{
    size_t i, n = alen < blen ? alen : blen;
    for (i = 0; i < n; i++)
    {
        int ca = toupper((unsigned char)a[i]), cb = toupper((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
    }
    return alen < blen ? -1 : alen > blen ? 1 : 0;
}

static size_t entry_name_len(const char *e)
{
    /* A name can start with '=' (the per-drive directories "=C:"). */
    const char *eq = strchr(e + 1, '=');
    if (eq == NULL) rt_fatal("environment entry without '='");
    return (size_t)(eq - e);
}

/* Index of name, or the insert position with *found false. Lock held. */
static size_t env_find(const char *name, size_t len, bool *found)
{
    size_t lo = 0, hi = env_count;
    while (lo < hi)
    {
        size_t mid = lo + (hi - lo) / 2;
        int c = name_cmp(env[mid], entry_name_len(env[mid]), name, len);
        if (c == 0)
        {
            *found = true;
            return mid;
        }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    *found = false;
    return lo;
}

/* Set (value non-NULL) or delete a variable. Lock held. */
static void env_set_locked(const char *name, const char *value)
{
    size_t len = strlen(name), i;
    bool found;
    if (len == 0 || strchr(name + 1, '=') != NULL) rt_fatal("environment name \"%s\" is not valid", name);
    i = env_find(name, len, &found);
    if (value == NULL)
    {
        if (found)
        {
            free(env[i]);
            memmove(&env[i], &env[i + 1], (env_count - i - 1) * sizeof(*env));
            env_count--;
        }
        return;
    }
    {
        size_t vlen = strlen(value);
        char *e = malloc(len + vlen + 2);
        if (e == NULL) rt_fatal("out of host memory (environment)");
        memcpy(e, name, len);
        e[len] = '=';
        memcpy(e + len + 1, value, vlen + 1);
        if (found)
        {
            free(env[i]);
            env[i] = e;
            return;
        }
        if (env_count == env_cap)
        {
            size_t cap = env_cap ? env_cap * 2 : 64;
            char **n = realloc(env, cap * sizeof(*env));
            if (n == NULL) rt_fatal("out of host memory (environment)");
            env = n;
            env_cap = cap;
        }
        memmove(&env[i + 1], &env[i], (env_count - i) * sizeof(*env));
        env[i] = e;
        env_count++;
    }
}

/* The environment of a new Windows XP user session (user "Player"). Lock
   held. */
static void env_init_locked(void)
{
    if (env_ready) return;
    env_ready = true;
    env_set_locked("ALLUSERSPROFILE", "C:\\ProgramData");
    env_set_locked("APPDATA", "C:\\Users\\Player\\AppData\\Roaming");
    env_set_locked("CommonProgramFiles", "C:\\Program Files\\Common Files");
    env_set_locked("COMPUTERNAME", rt_computer_name());
    env_set_locked("ComSpec", "C:\\Windows\\system32\\cmd.exe");
    env_set_locked("HOMEDRIVE", "C:");
    env_set_locked("HOMEPATH", "\\Users\\Player");
    env_set_locked("LOCALAPPDATA", "C:\\Users\\Player\\AppData\\Local");
    env_set_locked("OS", "Windows_NT");
    env_set_locked("Path", "C:\\Windows\\system32;C:\\Windows;C:\\Windows\\System32\\Wbem");
    env_set_locked("PATHEXT", ".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH");
    env_set_locked("PROCESSOR_ARCHITECTURE", "x86");
    env_set_locked("PROCESSOR_IDENTIFIER", "x86 Family 15 Model 2 Stepping 9, GenuineIntel");
    env_set_locked("PROCESSOR_LEVEL", "15");
    env_set_locked("PROCESSOR_REVISION", "0209");
    env_set_locked("ProgramData", "C:\\ProgramData");
    env_set_locked("ProgramFiles", "C:\\Program Files");
    env_set_locked("SystemDrive", "C:");
    env_set_locked("SystemRoot", "C:\\Windows");
    env_set_locked("TEMP", "C:\\Users\\Player\\AppData\\Local\\Temp");
    env_set_locked("TMP", "C:\\Users\\Player\\AppData\\Local\\Temp");
    env_set_locked("USERDOMAIN", rt_computer_name());
    env_set_locked("USERNAME", "Player");
    env_set_locked("USERPROFILE", "C:\\Users\\Player");
    env_set_locked("windir", "C:\\Windows");
    {
        char n[16];
        uint32_t c = rt_host_processor_count();
        snprintf(n, sizeof(n), "%u", c);
        env_set_locked("NUMBER_OF_PROCESSORS", n);
    }
}

void rt_env_set(const char *name, const char *value)
{
    if (pthread_mutex_lock(&env_lock) != 0) rt_fatal("env_lock lock failed");
    env_init_locked();
    env_set_locked(name, value);
    if (pthread_mutex_unlock(&env_lock) != 0) rt_fatal("env_lock unlock failed");
}

/* LPWCH GetEnvironmentStringsW(void): a new block of "NAME=VALUE\0"
   strings with a second NUL at the end. */
void rt_GetEnvironmentStringsW(_cpu *cpu)
{
    size_t units = 1, i, u;
    uint32_t block, pos;
    if (pthread_mutex_lock(&env_lock) != 0) rt_fatal("env_lock lock failed");
    env_init_locked();
    for (i = 0; i < env_count; i++)
    {
        if (!rt_utf16_length(env[i], &u)) rt_fatal("environment entry is not valid UTF-8");
        units += u + 1;
    }
    if (units > 0x7FFFFFFFu / 2u) rt_fatal("the environment is too large");
    block = rt_heap_alloc((uint32_t)(units * 2u));
    if (block == 0)
    {
        if (pthread_mutex_unlock(&env_lock) != 0) rt_fatal("env_lock unlock failed");
        rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        RET(0);
        return;
    }
    pos = block;
    for (i = 0; i < env_count; i++)
    {
        uint32_t n = rt_put_wstr(pos, (uint32_t)units, env[i]);
        pos += 2u * (n + 1u);
    }
    wr16(pos, 0);
    if (pthread_mutex_unlock(&env_lock) != 0) rt_fatal("env_lock unlock failed");
    RET(block);
}

/* BOOL FreeEnvironmentStringsW(LPWCH) */
void rt_FreeEnvironmentStringsW(_cpu *cpu)
{
    uint32_t block = ARG(0);
    if (block == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    rt_heap_free(block);
    RET(1);
}

/* Read a variable name from a guest string. Returns false (with the last
   error set) if the name is not valid. */
static bool read_name(char *dst, size_t max, uint32_t g, bool wide)
{
    int r = wide ? rt_gstr_w(dst, max, g) : rt_gstr_a(dst, max, g);
    if (g == 0 || r != RT_STR_OK || dst[0] == 0 || strchr(dst + 1, '=') != NULL)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        return false;
    }
    return true;
}

/* DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR buf, DWORD size) */
void rt_GetEnvironmentVariableW(_cpu *cpu)
{
    char name[1024];
    size_t i;
    bool found;
    uint32_t r;
    if (!read_name(name, sizeof(name), ARG(0), true))
    {
        RET(0);
        return;
    }
    if (pthread_mutex_lock(&env_lock) != 0) rt_fatal("env_lock lock failed");
    env_init_locked();
    i = env_find(name, strlen(name), &found);
    if (!found)
    {
        if (pthread_mutex_unlock(&env_lock) != 0) rt_fatal("env_lock unlock failed");
        rt_set_last_error(ERROR_ENVVAR_NOT_FOUND);
        RET(0);
        return;
    }
    r = rt_put_wstr(ARG(2) != 0 ? ARG(1) : 0, ARG(2), env[i] + entry_name_len(env[i]) + 1);
    if (pthread_mutex_unlock(&env_lock) != 0) rt_fatal("env_lock unlock failed");
    RET(r);
}

/* DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD size) */
void rt_GetEnvironmentVariableA(_cpu *cpu)
{
    char name[1024];
    size_t i;
    bool found;
    uint32_t r;
    if (!read_name(name, sizeof(name), ARG(0), false))
    {
        RET(0);
        return;
    }
    if (pthread_mutex_lock(&env_lock) != 0) rt_fatal("env_lock lock failed");
    env_init_locked();
    i = env_find(name, strlen(name), &found);
    if (!found)
    {
        if (pthread_mutex_unlock(&env_lock) != 0) rt_fatal("env_lock unlock failed");
        rt_set_last_error(ERROR_ENVVAR_NOT_FOUND);
        RET(0);
        return;
    }
    r = rt_put_astr(ARG(2) != 0 ? ARG(1) : 0, ARG(2), env[i] + entry_name_len(env[i]) + 1);
    if (pthread_mutex_unlock(&env_lock) != 0) rt_fatal("env_lock unlock failed");
    RET(r);
}

/* DWORD ExpandEnvironmentStringsA(LPCSTR src, LPSTR dst, DWORD size):
   each %name% of a defined variable becomes its value; other text stays.
   The result is the length with the NUL in characters (as the ANSI
   function of Windows XP, which counts bytes of the ANSI result). If dst
   is too small, nothing is written. */
void rt_ExpandEnvironmentStringsA(_cpu *cpu)
{
    char src[32768], *out = NULL;
    size_t len = 0, cap = 0, i = 0, n;
    uint32_t need;
    if (ARG(0) == 0 || rt_gstr_a(src, sizeof src, ARG(0)) != RT_STR_OK)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    n = strlen(src);
    if (pthread_mutex_lock(&env_lock) != 0) rt_fatal("env_lock lock failed");
    env_init_locked();
    while (i < n)
    {
        const char *piece = src + i;
        size_t plen = 1;
        if (src[i] == '%')
        {
            const char *end = strchr(src + i + 1, '%');
            if (end != NULL && end > src + i + 1)
            {
                bool found;
                size_t k = env_find(src + i + 1, (size_t)(end - src - i - 1), &found);
                if (found)
                {
                    piece = env[k] + entry_name_len(env[k]) + 1;
                    plen = strlen(piece);
                    i = (size_t)(end - src) + 1;
                }
                else
                {
                    /* An unknown name stays with its first '%'; the second
                       '%' can start the next name. */
                    plen = (size_t)(end - src) - i;
                    i += plen;
                }
            }
            else
                i++;
        }
        else
            i++;
        if (len + plen + 1 > cap)
        {
            while (len + plen + 1 > cap) cap = cap ? 2 * cap : 256;
            out = realloc(out, cap);
            if (out == NULL) rt_fatal("out of host memory (ExpandEnvironmentStringsA)");
        }
        memcpy(out + len, piece, plen);
        len += plen;
    }
    if (pthread_mutex_unlock(&env_lock) != 0) rt_fatal("env_lock unlock failed");
    if (out == NULL)
    {
        out = malloc(1);
        if (out == NULL) rt_fatal("out of host memory (ExpandEnvironmentStringsA)");
    }
    out[len] = 0;
    need = rt_put_astr(0, 0, out);
    if (ARG(1) != 0 && ARG(2) >= need) rt_put_astr(ARG(1), ARG(2), out);
    free(out);
    RET(need);
}

static void set_variable(_cpu *cpu, bool wide)
{
    char name[1024];
    char *value = NULL;
    uint32_t gv = ARG(1);
    if (!read_name(name, sizeof(name), ARG(0), wide))
    {
        RET(0);
        return;
    }
    if (gv != 0)
    {
        uint32_t n = wide ? rt_wstrlen(gv) : (uint32_t)strlen((const char *)G2H(gv));
        size_t max = (size_t)n * 3u + 1u;
        int r;
        value = malloc(max);
        if (value == NULL) rt_fatal("out of host memory (environment)");
        r = wide ? rt_gstr_w(value, max, gv) : rt_gstr_a(value, max, gv);
        if (r != RT_STR_OK)
        {
            free(value);
            rt_set_last_error(ERROR_INVALID_PARAMETER);
            RET(0);
            return;
        }
    }
    if (pthread_mutex_lock(&env_lock) != 0) rt_fatal("env_lock lock failed");
    env_init_locked();
    env_set_locked(name, value);
    if (pthread_mutex_unlock(&env_lock) != 0) rt_fatal("env_lock unlock failed");
    free(value);
    RET(1);
}

/* BOOL SetEnvironmentVariableA/W(name, value): a NULL value deletes the
   variable. */
void rt_SetEnvironmentVariableA(_cpu *cpu) { set_variable(cpu, false); }
void rt_SetEnvironmentVariableW(_cpu *cpu) { set_variable(cpu, true); }

/* ---------------------------------------------------------------- pointer encoding */

/* EncodePointer and DecodePointer with a per-process secret, as Windows
   does: the value is XORed with the secret and rotated right by the low 5
   bits of the secret. */
static _Atomic uint32_t pointer_cookie;

static uint32_t cookie(void)
{
    uint32_t c = atomic_load_explicit(&pointer_cookie, memory_order_acquire);
    if (c == 0)
    {
        uint32_t want = arc4random();
        if (want == 0) want = 1;
        if (!atomic_compare_exchange_strong_explicit(&pointer_cookie, &c, want, memory_order_acq_rel, memory_order_acquire))
            return c;
        c = want;
    }
    return c;
}

static uint32_t ror32(uint32_t v, uint32_t n)
{
    n &= 31u;
    return n == 0 ? v : (v >> n) | (v << (32u - n));
}

uint32_t rt_encode_pointer(uint32_t p)
{
    uint32_t c = cookie();
    return ror32(p ^ c, c & 31u);
}

uint32_t rt_decode_pointer(uint32_t p)
{
    uint32_t c = cookie();
    return ror32(p, 32u - (c & 31u)) ^ c;
}

void rt_EncodePointer(_cpu *cpu)
{
    RET(rt_encode_pointer(ARG(0)));
}

void rt_DecodePointer(_cpu *cpu)
{
    RET(rt_decode_pointer(ARG(0)));
}

/* ---------------------------------------------------------------- errors and debug output */

void rt_SetLastError(_cpu *cpu)
{
    rt_set_last_error(ARG(0));
}

void rt_OutputDebugStringW(_cpu *cpu)
{
    uint32_t s = ARG(0);
    char buf[2048];
    if (s == 0) return;
    if (rt_gstr_w(buf, sizeof(buf), s) == RT_STR_INVALID)
        rt_log("debug: (a wide string with an unpaired surrogate)");
    else
        rt_log("debug: %s", buf); /* a longer string is shown in part */
}
