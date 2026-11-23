/*
 * Processes: KERNEL32 process functions, PSAPI EnumProcesses, SHELL32
 * ShellExecuteW and ShellExecuteExW, USER32 WaitForInputIdle and ADVAPI32
 * CreateProcessWithLogonW.
 *
 * The emulated system runs one process: the game. Process handles are
 * handles of the current process. Other Windows programs cannot run, so
 * CreateProcess fails with ERROR_NOT_SUPPORTED. ShellExecuteW and
 * ShellExecuteExW open web addresses and guest files with the host
 * default application (Launch Services), as the Windows shell opens them
 * with the associated program.
 */
#include "rt_file.h"
#include "rt_object.h"
#include "rt_text.h"
#include "rt_thread.h"

#include <CoreServices/CoreServices.h>
#include <pthread.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

#define STILL_ACTIVE 259u
#define NORMAL_PRIORITY_CLASS 0x20u
#define FILETIME_UNIX_EPOCH UINT64_C(116444736000000000)

/* Is h the current process (the pseudo handle or a process handle)? */
static bool current_process(uint32_t h)
{
    rt_object *o;
    if (h == RT_PSEUDO_PROCESS) return true;
    o = rt_handle_get(h, OBJ_PROCESS);
    if (o == NULL) return false;
    rt_object_unref(o);
    return true;
}

/* Fail with ERROR_INVALID_HANDLE if h is not the current process. */
#define REQUIRE_PROCESS(h, fail)                   \
    do                                             \
    {                                              \
        if (!current_process(h))                   \
        {                                          \
            rt_set_last_error(ERROR_INVALID_HANDLE); \
            RET(fail);                             \
            return;                                \
        }                                          \
    } while (0)

/* HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD pid) */
void rt_OpenProcess(_cpu *cpu)
{
    rt_object *o;
    uint32_t h;
    if (ARG(2) != rt_current_process_id())
    {
        /* No other process exists. */
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    o = rt_process_object();
    h = rt_handle_new(o);
    rt_object_unref(o);
    if (h == 0)
    {
        rt_set_last_error(ERROR_TOO_MANY_OPEN_FILES);
        RET(0);
        return;
    }
    RET(h);
}

/* BOOL GetExitCodeProcess(HANDLE, LPDWORD): the current process runs. */
void rt_GetExitCodeProcess(_cpu *cpu)
{
    REQUIRE_PROCESS(ARG(0), 0);
    if (ARG(1) == 0)
    {
        rt_set_last_error(998u); /* ERROR_NOACCESS */
        RET(0);
        return;
    }
    wr32(ARG(1), STILL_ACTIVE);
    RET(1);
}

/* BOOL IsWow64Process(HANDLE process, PBOOL wow64). The emulated system is
   32-bit Windows XP SP3 (GetVersionEx, GetSystemInfo), where no process
   runs under WOW64: *wow64 is FALSE. */
void rt_IsWow64Process(_cpu *cpu)
{
    REQUIRE_PROCESS(ARG(0), 0);
    if (ARG(1) == 0)
    {
        rt_set_last_error(998u); /* ERROR_NOACCESS */
        RET(0);
        return;
    }
    wr32(ARG(1), 0);
    RET(1);
}

/* ---------------------------------------------------------------- priority class and working set */

/* The priority class and the working set limits are recorded values: the
   host scheduler and the host memory manager do not use them. */
static pthread_mutex_t proc_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t priority_class = NORMAL_PRIORITY_CLASS;
static uint32_t ws_min = 204800u, ws_max = 1413120u; /* Windows XP defaults */

void rt_GetPriorityClass(_cpu *cpu)
{
    uint32_t c;
    REQUIRE_PROCESS(ARG(0), 0);
    if (pthread_mutex_lock(&proc_lock) != 0) rt_fatal("proc_lock lock failed");
    c = priority_class;
    if (pthread_mutex_unlock(&proc_lock) != 0) rt_fatal("proc_lock unlock failed");
    RET(c);
}

void rt_SetPriorityClass(_cpu *cpu)
{
    uint32_t c = ARG(1);
    REQUIRE_PROCESS(ARG(0), 0);
    /* IDLE, BELOW_NORMAL, NORMAL, ABOVE_NORMAL, HIGH, REALTIME */
    if (c != 0x40u && c != 0x4000u && c != 0x20u && c != 0x8000u && c != 0x80u && c != 0x100u)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (pthread_mutex_lock(&proc_lock) != 0) rt_fatal("proc_lock lock failed");
    priority_class = c;
    if (pthread_mutex_unlock(&proc_lock) != 0) rt_fatal("proc_lock unlock failed");
    RET(1);
}

void rt_GetProcessWorkingSetSize(_cpu *cpu)
{
    REQUIRE_PROCESS(ARG(0), 0);
    if (ARG(1) == 0 || ARG(2) == 0)
    {
        rt_set_last_error(998u);
        RET(0);
        return;
    }
    if (pthread_mutex_lock(&proc_lock) != 0) rt_fatal("proc_lock lock failed");
    wr32(ARG(1), ws_min);
    wr32(ARG(2), ws_max);
    if (pthread_mutex_unlock(&proc_lock) != 0) rt_fatal("proc_lock unlock failed");
    RET(1);
}

/* BOOL SetProcessWorkingSetSize(HANDLE, SIZE_T min, SIZE_T max): both
   (SIZE_T)-1 asks to trim the working set, which keeps the limits. */
void rt_SetProcessWorkingSetSize(_cpu *cpu)
{
    uint32_t mn = ARG(1), mx = ARG(2);
    REQUIRE_PROCESS(ARG(0), 0);
    if (mn == 0xFFFFFFFFu && mx == 0xFFFFFFFFu)
    {
        RET(1);
        return;
    }
    if (mn > mx || mx < 13u * 4096u)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (pthread_mutex_lock(&proc_lock) != 0) rt_fatal("proc_lock lock failed");
    ws_min = mn;
    ws_max = mx;
    if (pthread_mutex_unlock(&proc_lock) != 0) rt_fatal("proc_lock unlock failed");
    RET(1);
}

/* ---------------------------------------------------------------- times */

static uint64_t filetime_of(struct timeval tv)
{
    return FILETIME_UNIX_EPOCH + (uint64_t)tv.tv_sec * 10000000u + (uint64_t)tv.tv_usec * 10u;
}

static uint64_t duration_of(struct timeval tv)
{
    return (uint64_t)tv.tv_sec * 10000000u + (uint64_t)tv.tv_usec * 10u;
}

/* BOOL GetProcessTimes(HANDLE, LPFILETIME creation, LPFILETIME exit,
   LPFILETIME kernel, LPFILETIME user) */
void rt_GetProcessTimes(_cpu *cpu)
{
    struct kinfo_proc kp;
    size_t len = sizeof(kp);
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)getpid() };
    struct rusage ru;
    uint32_t i;
    REQUIRE_PROCESS(ARG(0), 0);
    for (i = 1; i <= 4; i++)
    {
        if (ARG(i) == 0)
        {
            rt_set_last_error(998u);
            RET(0);
            return;
        }
    }
    if (sysctl(mib, 4, &kp, &len, NULL, 0) != 0 || len != sizeof(kp)) rt_fatal("GetProcessTimes: sysctl KERN_PROC_PID failed");
    if (getrusage(RUSAGE_SELF, &ru) != 0) rt_fatal("GetProcessTimes: getrusage failed");
    wr64(ARG(1), filetime_of(kp.kp_proc.p_starttime));
    wr64(ARG(2), 0);
    wr64(ARG(3), duration_of(ru.ru_stime));
    wr64(ARG(4), duration_of(ru.ru_utime));
    RET(1);
}

/* BOOL EnumProcesses(DWORD *pids, DWORD cb, DWORD *needed) */
void rt_EnumProcesses(_cpu *cpu)
{
    uint32_t pids = ARG(0), cb = ARG(1), needed = ARG(2), n = 0;
    if (needed == 0)
    {
        rt_set_last_error(998u);
        RET(0);
        return;
    }
    if (cb >= 4u && pids != 0)
    {
        wr32(pids, rt_current_process_id());
        n = 4;
    }
    wr32(needed, n);
    RET(1);
}

/* DWORD WaitForInputIdle(HANDLE process, DWORD ms): the current process
   has no message queue in this runtime, so Windows returns at once with
   WAIT_FAILED and ERROR_NOT_GUI_PROCESS. */
void rt_WaitForInputIdle(_cpu *cpu)
{
    REQUIRE_PROCESS(ARG(0), 0xFFFFFFFFu);
    rt_set_last_error(1471u); /* ERROR_NOT_GUI_PROCESS */
    RET(0xFFFFFFFFu);
}

/* ---------------------------------------------------------------- new processes */

static void refuse_process(const char *fn, uint32_t app, uint32_t cmd, bool wide)
{
    char a[RT_PATH_BYTES] = "", c[2048] = "";
    if (app != 0) (void)(wide ? rt_gstr_w(a, sizeof(a), app) : rt_gstr_a(a, sizeof(a), app));
    if (cmd != 0) (void)(wide ? rt_gstr_w(c, sizeof(c), cmd) : rt_gstr_a(c, sizeof(c), cmd));
    rt_log("%s: other programs cannot run (application \"%s\", command line \"%s\")", fn, a, c);
    rt_set_last_error(ERROR_NOT_SUPPORTED);
}

/* BOOL CreateProcessA/W(app, cmd, psa, tsa, inherit, flags, env, dir,
   startup, process_information) */
void rt_CreateProcessA(_cpu *cpu)
{
    refuse_process("CreateProcessA", ARG(0), ARG(1), false);
    RET(0);
}

void rt_CreateProcessW(_cpu *cpu)
{
    refuse_process("CreateProcessW", ARG(0), ARG(1), true);
    RET(0);
}

/* BOOL CreateProcessWithLogonW(user, domain, password, logon_flags, app,
   cmd, flags, env, dir, startup, process_information) */
void rt_CreateProcessWithLogonW(_cpu *cpu)
{
    refuse_process("CreateProcessWithLogonW", ARG(4), ARG(5), true);
    RET(0);
}

/* ---------------------------------------------------------------- ShellExecuteExW */

#define SEE_MASK_NOCLOSEPROCESS 0x40u
#define SE_ERR_FNF 2u
#define SE_ERR_NOASSOC 31u
#define ERROR_NO_ASSOCIATION 1155u

static bool has_prefix_nocase(const char *s, const char *p)
{
    return strncasecmp(s, p, strlen(p)) == 0;
}

/* Open url (a CFURL) with the host default application. */
static bool open_url(CFURLRef url)
{
    OSStatus st = LSOpenCFURLRef(url, NULL);
    if (st != noErr) rt_log("ShellExecute: Launch Services error %d", (int)st);
    return st == noErr;
}

/* Open a file or web address for ShellExecuteW and ShellExecuteExW. The
   verbs "open" and "explore" (and no verb) open a web address (http,
   https, mailto) or a guest file or folder with the host default
   application. Other verbs and parameters fail with ERROR_NO_ASSOCIATION.
   Returns the hInstApp value: 33 for success, else an SE_ERR code (the
   last error is then set). */
static uint32_t shell_open(const char *fn, uint32_t verb, uint32_t file, uint32_t params)
{
    char v[64] = "", f[2048], p[16] = "";
    CFURLRef url = NULL;
    bool ok;

    if (file == 0 || rt_gstr_w(f, sizeof(f), file) != RT_STR_OK ||
        (verb != 0 && rt_gstr_w(v, sizeof(v), verb) != RT_STR_OK) ||
        (params != 0 && rt_gstr_w(p, sizeof(p), params) != RT_STR_OK))
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        return SE_ERR_FNF;
    }
    if ((v[0] != 0 && strcasecmp(v, "open") != 0 && strcasecmp(v, "explore") != 0) || p[0] != 0)
    {
        rt_log("%s: verb \"%s\" with parameters for \"%s\" is not supported", fn, v, f);
        rt_set_last_error(ERROR_NO_ASSOCIATION);
        return SE_ERR_NOASSOC;
    }
    if (has_prefix_nocase(f, "http://") || has_prefix_nocase(f, "https://") || has_prefix_nocase(f, "mailto:"))
    {
        url = CFURLCreateWithBytes(kCFAllocatorDefault, (const UInt8 *)f, (CFIndex)strlen(f), kCFStringEncodingUTF8, NULL);
    }
    else
    {
        char full[RT_PATH_BYTES], host[RT_HOST_PATH];
        bool exists = false;
        uint32_t e = rt_path_full(f, full, sizeof(full));
        if (e == 0) e = rt_path_host(full, host, sizeof(host), &exists);
        if (e == 0 && !exists) e = ERROR_FILE_NOT_FOUND;
        if (e != 0)
        {
            rt_set_last_error(e);
            return SE_ERR_FNF;
        }
        url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, (const UInt8 *)host, (CFIndex)strlen(host), false);
    }
    if (url == NULL)
    {
        rt_set_last_error(ERROR_FILE_NOT_FOUND);
        return SE_ERR_FNF;
    }
    ok = open_url(url);
    CFRelease(url);
    if (!ok)
    {
        rt_set_last_error(ERROR_NO_ASSOCIATION);
        return SE_ERR_NOASSOC;
    }
    return 33u;
}

/* BOOL ShellExecuteExW(SHELLEXECUTEINFOW *info). See shell_open. No
   process handle is given. */
void rt_ShellExecuteExW(_cpu *cpu)
{
    uint32_t info = ARG(0), mask, r;

    if (info == 0 || rd32(info) != 60u)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    mask = rd32(info + 4);
    if (mask & SEE_MASK_NOCLOSEPROCESS) wr32(info + 56, 0);
    r = shell_open("ShellExecuteExW", rd32(info + 12), rd32(info + 16), rd32(info + 20));
    wr32(info + 32, r); /* hInstApp: > 32 for success */
    RET(r > 32u ? 1u : 0u);
}

/* HINSTANCE ShellExecuteW(HWND, LPCWSTR verb, LPCWSTR file, LPCWSTR
   params, LPCWSTR dir, INT show). See shell_open. Returns a value above
   32 for success, else an SE_ERR code. */
void rt_ShellExecuteW(_cpu *cpu)
{
    RET(shell_open("ShellExecuteW", ARG(1), ARG(2), ARG(3)));
}
