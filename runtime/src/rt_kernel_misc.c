/*
 * KERNEL32: virtual memory, the process heap handle, system information,
 * locale and code page functions, FormatMessageA/W and debug output.
 *
 * The emulated system is Windows XP Service Pack 3 (5.1.2600), English
 * (United States), ANSI code page 1252. The game executable is not large
 * address aware, so memory sizes are limited to 2 GiB as on Windows.
 */
#include "rt.h"
#include "rt_text.h"
#include "rt_thread.h"

#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <pthread.h>
#include <wctype.h>
#include <xlocale.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <time.h>

#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_DECOMMIT 0x4000u
#define MEM_RELEASE 0x8000u
#define MEM_TOP_DOWN 0x100000u

#define PAGE_NOACCESS 0x01u
#define PAGE_READONLY 0x02u
#define PAGE_READWRITE 0x04u
#define PAGE_EXECUTE 0x10u
#define PAGE_EXECUTE_READ 0x20u
#define PAGE_EXECUTE_READWRITE 0x40u

#define ERROR_INVALID_ADDRESS 487u
#define ERROR_INVALID_FLAGS 1004u
#define ERROR_NO_UNICODE_TRANSLATION 1113u
#define ERROR_MR_MID_NOT_FOUND 317u

#define GUEST_PAGE 0x1000u
#define LIMIT_2G 0x7FFFFFFFu

/* ---------------------------------------------------------------- virtual memory */

/* LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect)
   Committed memory is always readable and writable: read-only and
   no-access protection are not enforced (the guest code cannot execute
   data, so execute protections need no action). The protection is
   recorded for VirtualQuery and VirtualProtect. */
void rt_VirtualAlloc(_cpu *cpu)
{
    uint32_t addr = ARG(0), size = ARG(1), type = ARG(2), prot = ARG(3);
    uint32_t base;

    if (prot != PAGE_READWRITE && prot != PAGE_READONLY && prot != PAGE_NOACCESS && prot != PAGE_EXECUTE &&
        prot != PAGE_EXECUTE_READ && prot != PAGE_EXECUTE_READWRITE)
        rt_fatal("VirtualAlloc: protection 0x%x is not supported", prot);
    if ((type & ~(MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN)) != 0 || (type & (MEM_COMMIT | MEM_RESERVE)) == 0)
        rt_fatal("VirtualAlloc: allocation type 0x%x is not supported", type);
    if (size == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }

    if ((type & MEM_RESERVE) || addr == 0)
    {
        /* A reservation (MEM_COMMIT alone with a NULL address reserves
           too). The base is rounded down to 64 KiB; the size covers the
           pages of [addr, addr + size). */
        uint32_t start = addr & ~(RT_GRANULE - 1);
        uint64_t end = ((uint64_t)addr + size + GUEST_PAGE - 1) & ~(uint64_t)(GUEST_PAGE - 1);
        if (end - start > 0xFFFFFFFFu)
        {
            rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
            RET(0);
            return;
        }
        base = rt_va_reserve(start, (uint32_t)(end - start));
        if (base == 0)
        {
            /* The guest window has 2 GiB; a large-address-aware program
               has 4 GiB on 64-bit Windows. */
            rt_set_last_error(addr != 0 ? ERROR_INVALID_ADDRESS : ERROR_NOT_ENOUGH_MEMORY);
            RET(0);
            return;
        }
        rt_va_set_alloc_info(base, RT_MEM_PRIVATE, prot);
        if (type & MEM_COMMIT)
        {
            uint32_t old;
            if (rt_va_commit(base, (uint32_t)(end - start)) != 0 || rt_va_protect(base, (uint32_t)(end - start), prot, &old) != 0)
                rt_fatal("VirtualAlloc: commit of a new reservation failed");
        }
        RET(base);
        return;
    }

    /* Commit in an existing reservation. */
    {
        uint32_t start = addr & ~(GUEST_PAGE - 1);
        uint64_t end = ((uint64_t)addr + size + GUEST_PAGE - 1) & ~(uint64_t)(GUEST_PAGE - 1);
        uint32_t old;
        if (end > RT_WINDOW_SIZE || rt_va_commit(start, (uint32_t)(end - start)) != 0)
        {
            rt_set_last_error(ERROR_INVALID_ADDRESS);
            RET(0);
            return;
        }
        if (rt_va_protect(start, (uint32_t)(end - start), prot, &old) != 0) rt_fatal("VirtualAlloc: protection of committed pages failed");
        RET(start);
    }
}

/* BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type) */
void rt_VirtualFree(_cpu *cpu)
{
    uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
    if (type == MEM_RELEASE)
    {
        if (size != 0 || rt_va_release(addr) != 0)
        {
            rt_set_last_error(size != 0 ? ERROR_INVALID_PARAMETER : ERROR_INVALID_ADDRESS);
            RET(0);
            return;
        }
        RET(1);
        return;
    }
    if (type == MEM_DECOMMIT)
    {
        uint32_t start = addr & ~(GUEST_PAGE - 1);
        uint64_t end;
        if (size == 0) rt_fatal("VirtualFree: MEM_DECOMMIT of a whole region (size 0) is not supported");
        end = ((uint64_t)addr + size + GUEST_PAGE - 1) & ~(uint64_t)(GUEST_PAGE - 1);
        if (end > RT_WINDOW_SIZE || rt_va_decommit(start, (uint32_t)(end - start)) != 0)
        {
            rt_set_last_error(ERROR_INVALID_ADDRESS);
            RET(0);
            return;
        }
        RET(1);
        return;
    }
    rt_set_last_error(ERROR_INVALID_PARAMETER);
    RET(0);
}

/* Protection values that VirtualProtect accepts: one base protection and
   the modifiers PAGE_GUARD (0x100) and PAGE_NOCACHE (0x200). */
static bool valid_protect(uint32_t prot)
{
    uint32_t base = prot & 0xFFu;
    if ((prot & ~0x3FFu) != 0) return false;
    return base == PAGE_NOACCESS || base == PAGE_READONLY || base == PAGE_READWRITE || base == 0x08u ||
           base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ || base == PAGE_EXECUTE_READWRITE || base == 0x80u;
}

/* BOOL VirtualProtect(LPVOID addr, SIZE_T size, DWORD new, PDWORD old).
   The protection is recorded (see VirtualAlloc). A mapped view keeps the
   host protection of MapViewOfFile. */
void rt_VirtualProtect(_cpu *cpu)
{
    uint32_t addr = ARG(0), size = ARG(1), prot = ARG(2), oldp = ARG(3), old, start;
    uint64_t end;
    rt_va_region r;
    if (oldp == 0 || !valid_protect(prot))
    {
        rt_set_last_error(oldp == 0 ? 998u /* ERROR_NOACCESS */ : ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (size == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    start = addr & ~(GUEST_PAGE - 1);
    end = ((uint64_t)addr + size + GUEST_PAGE - 1) & ~(uint64_t)(GUEST_PAGE - 1);
    if (end > RT_WINDOW_SIZE || rt_va_query(start, &r) != 0)
    {
        rt_set_last_error(ERROR_INVALID_ADDRESS);
        RET(0);
        return;
    }
    if (r.type == RT_MEM_MAPPED) rt_fatal("VirtualProtect: protection change of a mapped view (0x%08x) is not supported", addr);
    if (rt_va_protect(start, (uint32_t)(end - start), prot, &old) != 0)
    {
        rt_set_last_error(ERROR_INVALID_ADDRESS);
        RET(0);
        return;
    }
    wr32(oldp, old);
    RET(1);
}

/* SIZE_T VirtualQuery(LPCVOID addr, PMEMORY_BASIC_INFORMATION mbi, SIZE_T n) */
void rt_VirtualQuery(_cpu *cpu)
{
    uint32_t addr = ARG(0), mbi = ARG(1), n = ARG(2);
    rt_va_region r;
    if (n < 28u)
    {
        rt_set_last_error(24u); /* ERROR_BAD_LENGTH */
        RET(0);
        return;
    }
    if (addr > 0x7FFEFFFFu || rt_va_query(addr, &r) != 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if ((uint64_t)r.base + r.size > 0x7FFF0000u) r.size = 0x7FFF0000u - r.base;
    wr32(mbi + 0, r.base);
    wr32(mbi + 4, r.alloc_base);
    wr32(mbi + 8, r.alloc_protect);
    wr32(mbi + 12, r.size);
    wr32(mbi + 16, r.state);
    wr32(mbi + 20, r.protect);
    wr32(mbi + 24, r.type);
    RET(28);
}

/* ---------------------------------------------------------------- process heap */

/* The process heap handle is the guest address of a reserved block, as on
   Windows (a heap handle is the address of the heap). The heap functions
   are in rt_heap.c. */
static uint32_t process_heap;
static pthread_once_t heap_once = PTHREAD_ONCE_INIT;

static void make_process_heap(void)
{
    process_heap = rt_heap_calloc(1, 0x1000);
    if (process_heap == 0) rt_fatal("no guest memory for the process heap handle");
    wr32(rt_peb + 0x18u, process_heap); /* PEB.ProcessHeap */
}

uint32_t rt_process_heap(void)
{
    if (pthread_once(&heap_once, make_process_heap) != 0) rt_fatal("process heap setup failed");
    return process_heap;
}

void rt_GetProcessHeap(_cpu *cpu)
{
    RET(rt_process_heap());
}

/* ---------------------------------------------------------------- system information */

static uint64_t host_total_memory(void)
{
    uint64_t v = 0;
    size_t n = sizeof(v);
    if (sysctlbyname("hw.memsize", &v, &n, NULL, 0) != 0) rt_fatal("sysctl hw.memsize failed");
    return v;
}

static uint64_t host_available_memory(void)
{
    vm_statistics64_data_t st;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t page;
    if (host_page_size(mach_host_self(), &page) != KERN_SUCCESS) rt_fatal("host_page_size failed");
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&st, &count) != KERN_SUCCESS)
        rt_fatal("host_statistics64 failed");
    return ((uint64_t)st.free_count + st.inactive_count + st.purgeable_count) * page;
}

static uint32_t cap2g(uint64_t v)
{
    return v > LIMIT_2G ? LIMIT_2G : (uint32_t)v;
}

/* void GlobalMemoryStatus(LPMEMORYSTATUS) */
void rt_GlobalMemoryStatus(_cpu *cpu)
{
    uint32_t ms = ARG(0);
    uint64_t total = host_total_memory(), avail = host_available_memory();
    uint64_t vfree = rt_va_free_bytes();
    if (avail > total) avail = total;
    wr32(ms + 0, 32);
    wr32(ms + 4, total != 0 ? (uint32_t)(((total - avail) * 100u) / total) : 0u);
    wr32(ms + 8, cap2g(total));
    wr32(ms + 12, cap2g(avail));
    wr32(ms + 16, cap2g(total));
    wr32(ms + 20, cap2g(avail));
    wr32(ms + 24, 0x7FFE0000u); /* user address space of a 32-bit process */
    wr32(ms + 28, cap2g(vfree));
}

/* BOOL GlobalMemoryStatusEx(LPMEMORYSTATUSEX): dwLength must be 64. The
   page file values are the physical memory values (macOS has no fixed page
   file). The virtual values are those of a 32-bit process with 2 GiB of
   user address space. */
void rt_GlobalMemoryStatusEx(_cpu *cpu)
{
    uint32_t ms = ARG(0);
    uint64_t total = host_total_memory(), avail = host_available_memory();
    if (ms == 0 || !rt_va_is_committed(ms, 64) || rd32(ms) != 64u)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (avail > total) avail = total;
    wr32(ms + 4, total != 0 ? (uint32_t)(((total - avail) * 100u) / total) : 0u);
    wr64(ms + 8, total);
    wr64(ms + 16, avail);
    wr64(ms + 24, total);
    wr64(ms + 32, avail);
    wr64(ms + 40, 0x7FFE0000u);
    wr64(ms + 48, rt_va_free_bytes());
    wr64(ms + 56, 0);
    RET(1);
}

/* BOOL GetVersionExA(LPOSVERSIONINFOA): Windows XP SP3. */
void rt_GetVersionExA(_cpu *cpu)
{
    uint32_t vi = ARG(0), size = rd32(vi);
    static const char sp[] = "Service Pack 3";
    if (size != 148u && size != 156u)
    {
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        RET(0);
        return;
    }
    memset(G2H(vi + 4), 0, size - 4u);
    wr32(vi + 4, 5);    /* dwMajorVersion */
    wr32(vi + 8, 1);    /* dwMinorVersion */
    wr32(vi + 12, 2600); /* dwBuildNumber */
    wr32(vi + 16, 2);   /* VER_PLATFORM_WIN32_NT */
    memcpy(G2H(vi + 20), sp, sizeof(sp));
    if (size == 156u)
    {
        wr16(vi + 148, 3);     /* wServicePackMajor */
        wr16(vi + 150, 0);     /* wServicePackMinor */
        wr16(vi + 152, 0x100); /* wSuiteMask: VER_SUITE_SINGLEUSERTS */
        wr8(vi + 154, 1);      /* wProductType: VER_NT_WORKSTATION */
    }
    RET(1);
}

/* void GetStartupInfoA(LPSTARTUPINFOA): no startup information. */
void rt_GetStartupInfoA(_cpu *cpu)
{
    uint32_t si = ARG(0);
    memset(G2H(si), 0, 68);
    wr32(si, 68);
}

void rt_GetLastError(_cpu *cpu)
{
    RET(rt_get_last_error());
}

void rt_OutputDebugStringA(_cpu *cpu)
{
    (void)cpu;
}

/* lstrlenA and lstrlenW: NULL gives 0. */
void rt_lstrlenA(_cpu *cpu)
{
    uint32_t s = ARG(0);
    RET(s != 0 ? (uint32_t)strlen(G2H(s)) : 0u);
}

void rt_lstrlenW(_cpu *cpu)
{
    uint32_t s = ARG(0), n = 0;
    if (s != 0)
        while (rd16(s + 2u * n) != 0) n++;
    RET(n);
}

/* LPSTR lstrcpyA(LPSTR dst, LPCSTR src): NULL for a NULL or unreadable
   string (Windows catches the access violation). */
void rt_lstrcpyA(_cpu *cpu)
{
    uint32_t d = ARG(0), sp = ARG(1), n = 0;
    if (d == 0 || sp == 0)
    {
        RET(0);
        return;
    }
    while (rt_va_is_committed(sp + n, 1) && rd8(sp + n) != 0) n++;
    if (!rt_va_is_committed(sp + n, 1) || !rt_va_is_committed(d, n + 1))
    {
        RET(0);
        return;
    }
    memmove(G2H(d), G2H(sp), n + 1);
    RET(d);
}

/* LPSTR lstrcpynA(LPSTR dst, LPCSTR src, int n): at most n-1 characters
   and a NUL. */
void rt_lstrcpynA(_cpu *cpu)
{
    uint32_t d = ARG(0), sp = ARG(1), count = ARG(2), i = 0;
    if (d == 0 || sp == 0)
    {
        RET(0);
        return;
    }
    if ((int32_t)count <= 0)
    {
        RET(d);
        return;
    }
    while (i + 1 < count && rd8(sp + i) != 0)
    {
        wr8(d + i, rd8(sp + i));
        i++;
    }
    wr8(d + i, 0);
    RET(d);
}

/* LPWSTR lstrcpynW(LPWSTR dst, LPCWSTR src, int n) */
void rt_lstrcpynW(_cpu *cpu)
{
    uint32_t d = ARG(0), sp = ARG(1), count = ARG(2), i = 0;
    if (d == 0 || sp == 0)
    {
        RET(0);
        return;
    }
    if ((int32_t)count <= 0)
    {
        RET(d);
        return;
    }
    while (i + 1 < count && rd16(sp + 2u * i) != 0)
    {
        wr16(d + 2u * i, rd16(sp + 2u * i));
        i++;
    }
    wr16(d + 2u * i, 0);
    RET(d);
}

/* LONG InterlockedExchangeAdd(LONG volatile *, LONG): the old value. */
void rt_InterlockedExchangeAdd(_cpu *cpu)
{
    uint32_t p = ARG(0);
    if ((p & 3u) != 0) rt_fatal("InterlockedExchangeAdd: address 0x%08x is not aligned to 4 bytes", p);
    RET(__atomic_fetch_add((uint32_t *)G2H(p), ARG(1), __ATOMIC_SEQ_CST));
}

/* BOOL IsBadReadPtr(const void *, UINT_PTR n): nonzero if a byte of the
   range cannot be read. */
void rt_IsBadReadPtr(_cpu *cpu)
{
    uint32_t p = ARG(0), n = ARG(1);
    if (n == 0)
    {
        RET(0);
        return;
    }
    if (p == 0 || n > 0xFFFFFFFFu - p + 1u)
    {
        RET(1);
        return;
    }
    RET(rt_va_is_committed(p, n) ? 0u : 1u);
}

/* ---------------------------------------------------------------- locale */

#define LCID_EN_US 0x0409u

/* LANGID GetUserDefaultLangID(void) */
void rt_GetUserDefaultLangID(_cpu *cpu) { RET(LCID_EN_US); }

void rt_GetACP(_cpu *cpu) { RET(1252); }
void rt_GetThreadLocale(_cpu *cpu) { RET(LCID_EN_US); }
void rt_GetSystemDefaultLangID(_cpu *cpu) { RET(LCID_EN_US); }

static bool lcid_is_en_us(uint32_t lcid)
{
    /* LOCALE_NEUTRAL (0, which ConvertDefaultLocale makes the user default
       locale), LOCALE_USER_DEFAULT, LOCALE_SYSTEM_DEFAULT and 0x0409 (with
       any sort identifier 0). */
    return lcid == 0u || lcid == 0x0400u || lcid == 0x0800u || lcid == LCID_EN_US;
}

/* The locale information of English (United States) on Windows XP SP3.
   LOCALE_FONTSIGNATURE (0x58) is binary data and is not in the table.
   Windows XP has no other types (LOCALE_SNAME, 0x5C, came with Vista). */
static const struct { uint32_t type; const char *value; } locale_en_us[] = {
    { 0x0001, "0409" },                       /* LOCALE_ILANGUAGE */
    { 0x0002, "English (United States)" },    /* LOCALE_SLANGUAGE */
    { 0x0003, "ENU" },                        /* LOCALE_SABBREVLANGNAME */
    { 0x0004, "English" },                    /* LOCALE_SNATIVELANGNAME */
    { 0x0005, "1" },                          /* LOCALE_ICOUNTRY */
    { 0x0006, "United States" },              /* LOCALE_SCOUNTRY */
    { 0x0007, "USA" },                        /* LOCALE_SABBREVCTRYNAME */
    { 0x0008, "United States" },              /* LOCALE_SNATIVECTRYNAME */
    { 0x0009, "0409" },                       /* LOCALE_IDEFAULTLANGUAGE */
    { 0x000A, "1" },                          /* LOCALE_IDEFAULTCOUNTRY */
    { 0x000B, "437" },                        /* LOCALE_IDEFAULTCODEPAGE */
    { 0x000C, "," },                          /* LOCALE_SLIST */
    { 0x000D, "1" },                          /* LOCALE_IMEASURE */
    { 0x000E, "." },                          /* LOCALE_SDECIMAL */
    { 0x000F, "," },                          /* LOCALE_STHOUSAND */
    { 0x0010, "3;0" },                        /* LOCALE_SGROUPING */
    { 0x0011, "2" },                          /* LOCALE_IDIGITS */
    { 0x0012, "1" },                          /* LOCALE_ILZERO */
    { 0x0013, "0123456789" },                 /* LOCALE_SNATIVEDIGITS */
    { 0x0014, "$" },                          /* LOCALE_SCURRENCY */
    { 0x0015, "USD" },                        /* LOCALE_SINTLSYMBOL */
    { 0x0016, "." },                          /* LOCALE_SMONDECIMALSEP */
    { 0x0017, "," },                          /* LOCALE_SMONTHOUSANDSEP */
    { 0x0018, "3;0" },                        /* LOCALE_SMONGROUPING */
    { 0x0019, "2" },                          /* LOCALE_ICURRDIGITS */
    { 0x001A, "2" },                          /* LOCALE_IINTLCURRDIGITS */
    { 0x001B, "0" },                          /* LOCALE_ICURRENCY */
    { 0x001C, "0" },                          /* LOCALE_INEGCURR */
    { 0x001D, "/" },                          /* LOCALE_SDATE */
    { 0x001E, ":" },                          /* LOCALE_STIME */
    { 0x001F, "M/d/yyyy" },                   /* LOCALE_SSHORTDATE */
    { 0x0020, "dddd, MMMM dd, yyyy" },        /* LOCALE_SLONGDATE */
    { 0x0021, "0" },                          /* LOCALE_IDATE */
    { 0x0022, "0" },                          /* LOCALE_ILDATE */
    { 0x0023, "0" },                          /* LOCALE_ITIME */
    { 0x0024, "1" },                          /* LOCALE_ICENTURY */
    { 0x0025, "0" },                          /* LOCALE_ITLZERO */
    { 0x0026, "0" },                          /* LOCALE_IDAYLZERO */
    { 0x0027, "0" },                          /* LOCALE_IMONLZERO */
    { 0x0028, "AM" },                         /* LOCALE_S1159 */
    { 0x0029, "PM" },                         /* LOCALE_S2359 */
    { 0x002A, "Monday" },                     /* LOCALE_SDAYNAME1 */
    { 0x002B, "Tuesday" },                    /* LOCALE_SDAYNAME2 */
    { 0x002C, "Wednesday" },                  /* LOCALE_SDAYNAME3 */
    { 0x002D, "Thursday" },                   /* LOCALE_SDAYNAME4 */
    { 0x002E, "Friday" },                     /* LOCALE_SDAYNAME5 */
    { 0x002F, "Saturday" },                   /* LOCALE_SDAYNAME6 */
    { 0x0030, "Sunday" },                     /* LOCALE_SDAYNAME7 */
    { 0x0031, "Mon" },                        /* LOCALE_SABBREVDAYNAME1 */
    { 0x0032, "Tue" },                        /* LOCALE_SABBREVDAYNAME2 */
    { 0x0033, "Wed" },                        /* LOCALE_SABBREVDAYNAME3 */
    { 0x0034, "Thu" },                        /* LOCALE_SABBREVDAYNAME4 */
    { 0x0035, "Fri" },                        /* LOCALE_SABBREVDAYNAME5 */
    { 0x0036, "Sat" },                        /* LOCALE_SABBREVDAYNAME6 */
    { 0x0037, "Sun" },                        /* LOCALE_SABBREVDAYNAME7 */
    { 0x0038, "January" },                    /* LOCALE_SMONTHNAME1 */
    { 0x0039, "February" },                   /* LOCALE_SMONTHNAME2 */
    { 0x003A, "March" },                      /* LOCALE_SMONTHNAME3 */
    { 0x003B, "April" },                      /* LOCALE_SMONTHNAME4 */
    { 0x003C, "May" },                        /* LOCALE_SMONTHNAME5 */
    { 0x003D, "June" },                       /* LOCALE_SMONTHNAME6 */
    { 0x003E, "July" },                       /* LOCALE_SMONTHNAME7 */
    { 0x003F, "August" },                     /* LOCALE_SMONTHNAME8 */
    { 0x0040, "September" },                  /* LOCALE_SMONTHNAME9 */
    { 0x0041, "October" },                    /* LOCALE_SMONTHNAME10 */
    { 0x0042, "November" },                   /* LOCALE_SMONTHNAME11 */
    { 0x0043, "December" },                   /* LOCALE_SMONTHNAME12 */
    { 0x0044, "Jan" },                        /* LOCALE_SABBREVMONTHNAME1 */
    { 0x0045, "Feb" },                        /* LOCALE_SABBREVMONTHNAME2 */
    { 0x0046, "Mar" },                        /* LOCALE_SABBREVMONTHNAME3 */
    { 0x0047, "Apr" },                        /* LOCALE_SABBREVMONTHNAME4 */
    { 0x0048, "May" },                        /* LOCALE_SABBREVMONTHNAME5 */
    { 0x0049, "Jun" },                        /* LOCALE_SABBREVMONTHNAME6 */
    { 0x004A, "Jul" },                        /* LOCALE_SABBREVMONTHNAME7 */
    { 0x004B, "Aug" },                        /* LOCALE_SABBREVMONTHNAME8 */
    { 0x004C, "Sep" },                        /* LOCALE_SABBREVMONTHNAME9 */
    { 0x004D, "Oct" },                        /* LOCALE_SABBREVMONTHNAME10 */
    { 0x004E, "Nov" },                        /* LOCALE_SABBREVMONTHNAME11 */
    { 0x004F, "Dec" },                        /* LOCALE_SABBREVMONTHNAME12 */
    { 0x0050, "" },                           /* LOCALE_SPOSITIVESIGN */
    { 0x0051, "-" },                          /* LOCALE_SNEGATIVESIGN */
    { 0x0052, "3" },                          /* LOCALE_IPOSSIGNPOSN */
    { 0x0053, "0" },                          /* LOCALE_INEGSIGNPOSN */
    { 0x0054, "1" },                          /* LOCALE_IPOSSYMPRECEDES */
    { 0x0055, "0" },                          /* LOCALE_IPOSSEPBYSPACE */
    { 0x0056, "1" },                          /* LOCALE_INEGSYMPRECEDES */
    { 0x0057, "0" },                          /* LOCALE_INEGSEPBYSPACE */
    { 0x0059, "en" },                         /* LOCALE_SISO639LANGNAME */
    { 0x005A, "US" },                         /* LOCALE_SISO3166CTRYNAME */
    { 0x005B, "244" },                        /* LOCALE_IGEOID */
    { 0x1001, "English" },                    /* LOCALE_SENGLANGUAGE */
    { 0x1002, "United States" },              /* LOCALE_SENGCOUNTRY */
    { 0x1003, "h:mm:ss tt" },                 /* LOCALE_STIMEFORMAT */
    { 0x1004, "1252" },                       /* LOCALE_IDEFAULTANSICODEPAGE */
    { 0x1005, "0" },                          /* LOCALE_ITIMEMARKPOSN */
    { 0x1006, "MMMM, yyyy" },                 /* LOCALE_SYEARMONTH */
    { 0x1007, "US Dollar" },                  /* LOCALE_SENGCURRNAME */
    { 0x1008, "US Dollar" },                  /* LOCALE_SNATIVECURRNAME */
    { 0x1009, "1" },                          /* LOCALE_ICALENDARTYPE */
    { 0x100A, "1" },                          /* LOCALE_IPAPERSIZE */
    { 0x100B, "0" },                          /* LOCALE_IOPTIONALCALENDAR */
    { 0x100C, "6" },                          /* LOCALE_IFIRSTDAYOFWEEK (6: Sunday) */
    { 0x100D, "0" },                          /* LOCALE_IFIRSTWEEKOFYEAR */
    { 0x100E, "" },                           /* LOCALE_SMONTHNAME13 */
    { 0x100F, "" },                           /* LOCALE_SABBREVMONTHNAME13 */
    { 0x1010, "1" },                          /* LOCALE_INEGNUMBER */
    { 0x1011, "10000" },                      /* LOCALE_IDEFAULTMACCODEPAGE */
    { 0x1012, "037" },                        /* LOCALE_IDEFAULTEBCDICCODEPAGE */
    { 0x1013, "Default" },                    /* LOCALE_SSORTNAME */
    { 0x1014, "1" },                          /* LOCALE_IDIGITSUBSTITUTION */
};

/* The value of a locale information type, or NULL if the type is not in
   the table. */
static const char *locale_value(uint32_t t)
{
    size_t i;
    for (i = 0; i < sizeof(locale_en_us) / sizeof(locale_en_us[0]); i++)
        if (locale_en_us[i].type == t) return locale_en_us[i].value;
    return NULL;
}

/* GetLocaleInfoA and GetLocaleInfoW: n is the buffer size in characters
   of the variant. */
static void get_locale_info(_cpu *cpu, bool wide)
{
    const char *fn = wide ? "GetLocaleInfoW" : "GetLocaleInfoA";
    uint32_t lcid = ARG(0), type = ARG(1), buf = ARG(2);
    int32_t n = (int32_t)ARG(3);
    bool number = (type & 0x20000000u) != 0; /* LOCALE_RETURN_NUMBER */
    uint32_t t = type & ~(0x80000000u | 0x20000000u | 0x40000000u); /* NOUSEROVERRIDE, USE_CP_ACP */
    const char *v;
    uint32_t len;

    if (!lcid_is_en_us(lcid)) rt_fatal("%s: locale 0x%x is not supported (English, United States only)", fn, lcid);
    if (t == 0x58u) rt_fatal("%s: LOCALE_FONTSIGNATURE is not supported", fn);
    v = locale_value(t);
    if (v == NULL)
    {
        /* Not a locale information type of Windows XP. */
        rt_set_last_error(ERROR_INVALID_FLAGS);
        RET(0);
        return;
    }
    if (n < 0 || (n > 0 && buf == 0))
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (number)
    {
        char *end;
        unsigned long nv = strtoul(v, &end, (t == 0x0001 || t == 0x0009) ? 16 : 10);
        /* A DWORD is 2 characters of a wide buffer and 4 of an ANSI
           buffer. */
        uint32_t need = wide ? 2u : 4u;
        if (*v == 0 || *end != 0)
        {
            /* LOCALE_RETURN_NUMBER of a text type. */
            rt_set_last_error(ERROR_INVALID_FLAGS);
            RET(0);
            return;
        }
        if (n == 0)
        {
            RET(need);
            return;
        }
        if ((uint32_t)n < need)
        {
            rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
            RET(0);
            return;
        }
        wr32(buf, (uint32_t)nv);
        RET(need);
        return;
    }
    len = wide ? rt_put_wstr(0, 0, v) : rt_put_astr(0, 0, v); /* with the NUL */
    if (n == 0)
    {
        RET(len);
        return;
    }
    if ((uint32_t)n < len)
    {
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        RET(0);
        return;
    }
    if (wide) rt_put_wstr(buf, (uint32_t)n, v); else rt_put_astr(buf, (uint32_t)n, v);
    RET(len);
}

/* int GetLocaleInfoA/W(LCID, LCTYPE, buf, int n) */
/* ---------------------------------------------------------------- date and time formats */

#define DATE_SHORTDATE 0x1u
#define DATE_LONGDATE 0x2u
#define DATE_USE_ALT_CALENDAR 0x4u
#define DATE_YEARMONTH 0x8u
#define TIME_NOMINUTESORSECONDS 0x1u
#define TIME_NOSECONDS 0x2u
#define TIME_NOTIMEMARKER 0x4u
#define TIME_FORCE24HOURFORMAT 0x8u
#define LOCALE_NOUSEROVERRIDE 0x80000000u
#define LOCALE_USE_CP_ACP 0x40000000u

typedef struct fmt_buf {
    char s[512];
    size_t n;
    bool overflow;
} fmt_buf;

static void fb_add(fmt_buf *b, const char *t, size_t n)
{
    if (b->n + n >= sizeof b->s)
    {
        b->overflow = true;
        return;
    }
    memcpy(b->s + b->n, t, n);
    b->n += n;
    b->s[b->n] = 0;
}

static void fb_num(fmt_buf *b, unsigned v, int width)
{
    char t[16];
    int k = snprintf(t, sizeof t, "%0*u", width, v);
    fb_add(b, t, (size_t)k);
}

/* The SYSTEMTIME argument, or the local time for NULL. Returns false for a
   SYSTEMTIME that is not valid (the date or the time part as selected). */
static bool fmt_time(uint32_t st, bool date, unsigned v[8])
{
    static const unsigned mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    unsigned i;
    if (st == 0)
    {
        struct timespec ts;
        struct tm tm;
        if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || localtime_r(&ts.tv_sec, &tm) == NULL) rt_fatal("local time failed");
        v[0] = (unsigned)tm.tm_year + 1900u;
        v[1] = (unsigned)tm.tm_mon + 1u;
        v[2] = (unsigned)tm.tm_wday;
        v[3] = (unsigned)tm.tm_mday;
        v[4] = (unsigned)tm.tm_hour;
        v[5] = (unsigned)tm.tm_min;
        v[6] = (unsigned)tm.tm_sec;
        v[7] = (unsigned)(ts.tv_nsec / 1000000);
        return true;
    }
    if (!rt_va_is_committed(st, 16)) return false;
    for (i = 0; i < 8; i++) v[i] = rd16(st + 2u * i);
    if (date)
    {
        bool leap = (v[0] % 4 == 0 && v[0] % 100 != 0) || v[0] % 400 == 0;
        int64_t a, y, mm, dn;
        if (v[0] < 1601 || v[0] > 30827 || v[1] < 1 || v[1] > 12 || v[3] < 1 || v[3] > mdays[v[1] - 1] + (v[1] == 2 && leap))
            return false;
        /* The day of the week comes from the date, as on Windows. */
        a = (14 - (int64_t)v[1]) / 12;
        y = (int64_t)v[0] + 4800 - a;
        mm = (int64_t)v[1] + 12 * a - 3;
        dn = (int64_t)v[3] + (153 * mm + 2) / 5 + 365 * y + y / 4 - y / 100 + y / 400 - 32045;
        v[2] = (unsigned)((dn + 1) % 7);
    }
    else if (v[4] > 23 || v[5] > 59 || v[6] > 59 || v[7] > 999)
        return false;
    return true;
}

/* Format a date or time picture of the en-US locale. Quoted text ('...',
   '' for a quote) is literal. Returns false if the picture has a
   character that the function does not accept. */
static bool fmt_picture(fmt_buf *b, const char *pic, const unsigned v[8], bool date, uint32_t tflags)
{
    size_t i = 0, n = strlen(pic);
    while (i < n)
    {
        char c = pic[i];
        size_t run = 1;
        if (c == '\'')
        {
            i++;
            while (i < n)
            {
                if (pic[i] == '\'' && i + 1 < n && pic[i + 1] == '\'')
                {
                    fb_add(b, "'", 1);
                    i += 2;
                }
                else if (pic[i] == '\'')
                {
                    i++;
                    break;
                }
                else
                    fb_add(b, pic + i++, 1);
            }
            continue;
        }
        while (i + run < n && pic[i + run] == c) run++;
        if (date && c == 'd')
        {
            if (run == 1) fb_num(b, v[3], 1);
            else if (run == 2) fb_num(b, v[3], 2);
            else
            {
                const char *name = locale_value((run == 3 ? 0x31u : 0x2Au) + (v[2] + 6u) % 7u);
                fb_add(b, name, strlen(name));
            }
        }
        else if (date && c == 'M')
        {
            if (run == 1) fb_num(b, v[1], 1);
            else if (run == 2) fb_num(b, v[1], 2);
            else
            {
                const char *name = locale_value((run == 3 ? 0x44u : 0x38u) + v[1] - 1u);
                fb_add(b, name, strlen(name));
            }
        }
        else if (date && c == 'y')
        {
            if (run == 1) fb_num(b, v[0] % 100u, 1);
            else if (run == 2) fb_num(b, v[0] % 100u, 2);
            else fb_num(b, v[0], 4);
        }
        else if (date && c == 'g')
            fb_add(b, "A.D.", 4);
        else if (!date && (c == 'h' || c == 'H'))
        {
            unsigned h = v[4];
            if (c == 'h' && !(tflags & TIME_FORCE24HOURFORMAT)) h = h % 12u == 0 ? 12u : h % 12u;
            fb_num(b, h, run >= 2 ? 2 : 1);
        }
        else if (!date && c == 'm')
        {
            if (tflags & TIME_NOMINUTESORSECONDS)
            {
                /* Drop the minutes and the separator in front of them. */
                while (b->n > 0 && b->s[b->n - 1] != ' ' && !(b->s[b->n - 1] >= '0' && b->s[b->n - 1] <= '9')) b->s[--b->n] = 0;
            }
            else
                fb_num(b, v[5], run >= 2 ? 2 : 1);
        }
        else if (!date && c == 's')
        {
            if (tflags & (TIME_NOSECONDS | TIME_NOMINUTESORSECONDS))
            {
                while (b->n > 0 && b->s[b->n - 1] != ' ' && !(b->s[b->n - 1] >= '0' && b->s[b->n - 1] <= '9')) b->s[--b->n] = 0;
            }
            else
                fb_num(b, v[6], run >= 2 ? 2 : 1);
        }
        else if (!date && c == 't')
        {
            if (!(tflags & (TIME_NOTIMEMARKER | TIME_FORCE24HOURFORMAT)))
            {
                const char *m = v[4] < 12 ? "AM" : "PM";
                fb_add(b, m, run >= 2 ? 2 : 1);
            }
            else
            {
                /* Drop the space in front of the marker. */
                while (b->n > 0 && b->s[b->n - 1] == ' ') b->s[--b->n] = 0;
            }
        }
        else
            fb_add(b, pic + i, run);
        i += run;
    }
    return !b->overflow;
}

/* GetDateFormatA and GetTimeFormatA: (LCID, DWORD flags, const SYSTEMTIME *,
   LPCSTR format, LPSTR buf, int n). The result is the length with the NUL;
   n 0 gives the size that is necessary. */
static void date_time_format(_cpu *cpu, bool date)
{
    uint32_t lcid = ARG(0), flags = ARG(1), st = ARG(2), fmt = ARG(3), buf = ARG(4);
    int32_t n = (int32_t)ARG(5);
    char pic[256];
    unsigned v[8];
    fmt_buf b;
    uint32_t need;
    if (!lcid_is_en_us(lcid) || n < 0 || (n > 0 && buf == 0))
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    flags &= ~(LOCALE_NOUSEROVERRIDE | LOCALE_USE_CP_ACP);
    if (date)
    {
        if ((flags & ~(DATE_SHORTDATE | DATE_LONGDATE | DATE_YEARMONTH | DATE_USE_ALT_CALENDAR)) ||
            ((flags & DATE_SHORTDATE) && (flags & (DATE_LONGDATE | DATE_YEARMONTH))) || ((flags & DATE_LONGDATE) && (flags & DATE_YEARMONTH)) ||
            (fmt != 0 && (flags & (DATE_SHORTDATE | DATE_LONGDATE | DATE_YEARMONTH))))
        {
            rt_set_last_error(ERROR_INVALID_FLAGS);
            RET(0);
            return;
        }
    }
    else if ((flags & ~(TIME_NOMINUTESORSECONDS | TIME_NOSECONDS | TIME_NOTIMEMARKER | TIME_FORCE24HOURFORMAT)))
    {
        rt_set_last_error(ERROR_INVALID_FLAGS);
        RET(0);
        return;
    }
    if (fmt != 0)
    {
        if (rt_gstr_a(pic, sizeof pic, fmt) != RT_STR_OK)
        {
            rt_set_last_error(ERROR_INVALID_PARAMETER);
            RET(0);
            return;
        }
    }
    else
    {
        const char *p = locale_value(date ? (flags & DATE_LONGDATE ? 0x20u : flags & DATE_YEARMONTH ? 0x1006u : 0x1Fu) : 0x1003u);
        memcpy(pic, p, strlen(p) + 1);
    }
    if (!fmt_time(st, date, v))
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    b.n = 0;
    b.s[0] = 0;
    b.overflow = false;
    if (!fmt_picture(&b, pic, v, date, flags)) rt_fatal("%s: the result is longer than %zu bytes", date ? "GetDateFormatA" : "GetTimeFormatA", sizeof b.s - 1);
    need = (uint32_t)b.n + 1u;
    if (n == 0)
    {
        RET(need);
        return;
    }
    if ((uint32_t)n < need)
    {
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        RET(0);
        return;
    }
    memcpy(G2H(buf), b.s, need);
    RET(need);
}

void rt_GetDateFormatA(_cpu *cpu) { date_time_format(cpu, true); }
void rt_GetTimeFormatA(_cpu *cpu) { date_time_format(cpu, false); }

void rt_GetLocaleInfoA(_cpu *cpu) { get_locale_info(cpu, false); }
void rt_GetLocaleInfoW(_cpu *cpu) { get_locale_info(cpu, true); }

void rt_GetUserDefaultLCID(_cpu *cpu) { RET(LCID_EN_US); }

/* BOOL IsValidLocale(LCID, DWORD flags): the installed and supported
   locales are English (United States) and the invariant locale. */
void rt_IsValidLocale(_cpu *cpu)
{
    uint32_t lcid = ARG(0), flags = ARG(1);
    if (flags != 1u && flags != 2u) /* LCID_INSTALLED, LCID_SUPPORTED */
    {
        rt_set_last_error(ERROR_INVALID_FLAGS);
        RET(0);
        return;
    }
    RET(lcid == LCID_EN_US || lcid == 0x007Fu);
}

/* BOOL EnumSystemLocalesA(LOCALE_ENUMPROCA proc, DWORD flags): calls proc
   with the identifier of each installed locale ("00000409"). */
void rt_EnumSystemLocalesA(_cpu *cpu)
{
    static const char id[] = "00000409";
    uint32_t proc = ARG(0), flags = ARG(1), saved_esp = esp, str;
    if (proc == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (flags != 1u && flags != 2u)
    {
        rt_set_last_error(ERROR_INVALID_FLAGS);
        RET(0);
        return;
    }
    /* The string is on the guest stack below the stack pointer, so no
       memory is held across the guest call. */
    esp -= 16u;
    str = esp;
    memcpy(G2H(str), id, sizeof(id));
    (void)rt_call_std(cpu, proc, 1, str);
    esp = saved_esp;
    RET(1);
}

/* int VerLanguageNameW(DWORD lang, LPWSTR buf, DWORD n): the name of a
   language identifier. Windows XP shows "Language Neutral" for an
   identifier that it does not know. The length without the NUL is
   returned; a buffer that is too small gets the start of the name. */
static const struct { uint16_t id; const char *name; } language_names[] = {
    { 0x0000, "Language Neutral" },
    { 0x0400, "Process Default Language" },
    { 0x0401, "Arabic (Saudi Arabia)" },
    { 0x0404, "Chinese (Taiwan)" },
    { 0x0405, "Czech" },
    { 0x0406, "Danish" },
    { 0x0407, "German (Germany)" },
    { 0x0408, "Greek" },
    { 0x0409, "English (United States)" },
    { 0x040A, "Spanish (Traditional Sort)" },
    { 0x040B, "Finnish" },
    { 0x040C, "French (France)" },
    { 0x040E, "Hungarian" },
    { 0x0410, "Italian (Italy)" },
    { 0x0411, "Japanese" },
    { 0x0412, "Korean" },
    { 0x0413, "Dutch (Netherlands)" },
    { 0x0414, "Norwegian (Bokmal)" },
    { 0x0415, "Polish" },
    { 0x0416, "Portuguese (Brazil)" },
    { 0x0419, "Russian" },
    { 0x041D, "Swedish" },
    { 0x041F, "Turkish" },
    { 0x0804, "Chinese (PRC)" },
    { 0x0809, "English (United Kingdom)" },
    { 0x0816, "Portuguese (Portugal)" },
    { 0x0C0A, "Spanish (International Sort)" },
};

void rt_VerLanguageNameW(_cpu *cpu)
{
    uint32_t lang = ARG(0), buf = ARG(1), n = ARG(2), len, i;
    const char *name = language_names[0].name;
    for (i = 0; i < sizeof(language_names) / sizeof(language_names[0]); i++)
        if (language_names[i].id == lang) name = language_names[i].name;
    len = (uint32_t)strlen(name); /* ASCII names: one unit per byte */
    if (buf != 0 && n > 0)
    {
        uint32_t k = len < n - 1u ? len : n - 1u;
        for (i = 0; i < k; i++) wr16(buf + 2u * i, (uint16_t)(uint8_t)name[i]);
        wr16(buf + 2u * k, 0);
    }
    RET(len);
}

/* ---------------------------------------------------------------- code pages */

/* int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR src, int srclen,
   LPWSTR dst, int dstlen). Code pages: CP_ACP (0), CP_THREAD_ACP (3; the
   thread locale is en-US, so its ANSI code page is 1252), 1252, CP_OEMCP
   (1) and 437, and CP_UTF8.
   UTF-8: invalid sequences are removed (the Windows XP behavior), or the
   call fails with MB_ERR_INVALID_CHARS. */
void rt_MultiByteToWideChar(_cpu *cpu)
{
    uint32_t cp = ARG(0), flags = ARG(1), src = ARG(2), dst = ARG(4);
    int32_t srclen = (int32_t)ARG(3), dstlen = (int32_t)ARG(5);
    const uint8_t *s;
    size_t n, i = 0;
    uint32_t out = 0;
    bool utf8, oem = false;

    if (cp == 0u || cp == 3u || cp == 1252u) utf8 = false;
    else if (cp == 1u || cp == 437u) utf8 = false, oem = true;
    else if (cp == 65001u) utf8 = true;
    else rt_fatal("MultiByteToWideChar: code page %u is not supported", cp);
    if (src == 0 || srclen == 0 || dstlen < 0 || (dstlen > 0 && dst == 0) || (dst != 0 && dst == src))
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (utf8 ? (flags & ~0x8u) != 0 : (flags & ~0x9u) != 0)
    {
        if (!utf8 && (flags & 0x6u) != 0)
            rt_fatal("MultiByteToWideChar: flags 0x%x (MB_COMPOSITE or MB_USEGLYPHCHARS) are not supported", flags);
        rt_set_last_error(ERROR_INVALID_FLAGS);
        RET(0);
        return;
    }
    s = G2H(src);
    n = srclen < 0 ? strlen((const char *)s) + 1 : (size_t)srclen;

    while (i < n)
    {
        uint32_t c;
        uint16_t units[2];
        int nu = 1, k;
        if (utf8)
        {
            int r = rt_utf8_decode(s + i, n - i, &c);
            if (r < 0)
            {
                if (flags & 0x8u)
                {
                    rt_set_last_error(ERROR_NO_UNICODE_TRANSLATION);
                    RET(0);
                    return;
                }
                i += (size_t)(-r);
                continue;
            }
            i += (size_t)r;
        }
        else
        {
            c = oem ? rt_cp437_to_unicode(s[i++]) : rt_cp1252_to_unicode(s[i++]);
        }
        if (c >= 0x10000)
        {
            units[0] = (uint16_t)(0xD800u + ((c - 0x10000u) >> 10));
            units[1] = (uint16_t)(0xDC00u + ((c - 0x10000u) & 0x3FFu));
            nu = 2;
        }
        else
        {
            units[0] = (uint16_t)c;
        }
        for (k = 0; k < nu; k++)
        {
            if (dstlen > 0)
            {
                if (out >= (uint32_t)dstlen)
                {
                    rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
                    RET(0);
                    return;
                }
                wr16(dst + 2u * out, units[k]);
            }
            out++;
        }
    }
    RET(out);
}

/* Code page of a code page argument: CP_ACP and CP_THREAD_ACP are 1252,
   CP_OEMCP is 437. Returns 0 for a code page that the emulated system does
   not have. */
static uint32_t resolve_code_page(uint32_t cp)
{
    switch (cp)
    {
    case 0u: case 3u: case 1252u: return 1252u;
    case 1u: case 437u: return 437u;
    case 65001u: return 65001u;
    default: return 0u;
    }
}

void rt_GetOEMCP(_cpu *cpu) { RET(437); }

/* BOOL IsValidCodePage(UINT cp): the installed code pages. */
void rt_IsValidCodePage(_cpu *cpu)
{
    uint32_t cp = ARG(0);
    RET(cp == 437u || cp == 1252u || cp == 65001u);
}

/* BOOL GetCPInfo(UINT cp, LPCPINFO info): CPINFO is MaxCharSize (4
   bytes), DefaultChar[2] and LeadByte[12]. */
void rt_GetCPInfo(_cpu *cpu)
{
    uint32_t cp = resolve_code_page(ARG(0)), info = ARG(1);
    if (cp == 0u || info == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    memset(G2H(info), 0, 20);
    wr32(info, cp == 65001u ? 4u : 1u);
    wr8(info + 4, '?');
    RET(1);
}

/* Best fit of a Unicode character in code page 1252 or 437 (WideCharToMultiByte
   without WC_NO_BEST_FIT_CHARS): the character itself, the base letter of
   a letter with a diacritic when the base letter is in the code page, or
   the ASCII form of a full-width ASCII character. Returns -1 if there is
   no such character. */
static int best_fit(uint32_t c, bool oem, bool no_best_fit)
{
    int b = oem ? rt_unicode_to_cp437(c) : rt_unicode_to_cp1252(c);
    if (b >= 0 || no_best_fit) return b;
    if (c >= 0xFF01u && c <= 0xFF5Eu) return (int)(c - 0xFF01u + 0x21u);
    {
        UniChar u = (UniChar)c, d[8];
        CFMutableStringRef ms;
        CFIndex n;
        if (c > 0xFFFFu) return -1;
        ms = CFStringCreateMutable(kCFAllocatorDefault, 0);
        if (ms == NULL) rt_fatal("out of host memory (CFString)");
        CFStringAppendCharacters(ms, &u, 1);
        CFStringNormalize(ms, kCFStringNormalizationFormD);
        n = CFStringGetLength(ms);
        if (n >= 2 && n <= 8)
        {
            CFStringGetCharacters(ms, CFRangeMake(0, n), d);
            CFRelease(ms);
            /* The base must be a letter that has no diacritic. */
            if (d[0] < 0x80u && ((d[0] >= 'A' && d[0] <= 'Z') || (d[0] >= 'a' && d[0] <= 'z'))) return d[0];
            return -1;
        }
        CFRelease(ms);
    }
    return -1;
}

/* int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR src, int srclen,
   LPSTR dst, int dstlen, LPCSTR defchar, LPBOOL used_default).
   Code pages: as MultiByteToWideChar. 1252 and 437 use the best fit of
   best_fit; a character without one becomes the default character ('?'
   or defchar). UTF-8: unpaired surrogates are encoded as 3-byte sequences
   (the Windows XP behavior); flags, defchar and used_default must be 0. */
void rt_WideCharToMultiByte(_cpu *cpu)
{
    uint32_t cp = resolve_code_page(ARG(0)), flags = ARG(1), src = ARG(2), dst = ARG(4);
    uint32_t defchar = ARG(6), used = ARG(7);
    int32_t srclen = (int32_t)ARG(3), dstlen = (int32_t)ARG(5);
    uint32_t n, i, out = 0;
    bool used_default = false;
    uint8_t def = '?';

    if (cp == 0u) rt_fatal("WideCharToMultiByte: code page %u is not supported", ARG(0));
    if (src == 0 || srclen == 0 || dstlen < 0 || (dstlen > 0 && dst == 0) || (dst != 0 && dst == src))
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (cp == 65001u)
    {
        if (flags != 0)
        {
            rt_set_last_error(ERROR_INVALID_FLAGS);
            RET(0);
            return;
        }
        if (defchar != 0 || used != 0)
        {
            rt_set_last_error(ERROR_INVALID_PARAMETER);
            RET(0);
            return;
        }
    }
    else
    {
        /* WC_COMPOSITECHECK (0x200) and its options (0x10, 0x20, 0x40) need
           composition tables; WC_NO_BEST_FIT_CHARS is 0x400. */
        if ((flags & ~0x400u) != 0)
        {
            if (flags & 0x270u) rt_fatal("WideCharToMultiByte: flags 0x%x (WC_COMPOSITECHECK) are not supported", flags);
            rt_set_last_error(ERROR_INVALID_FLAGS);
            RET(0);
            return;
        }
        if (defchar != 0) def = rd8(defchar);
    }
    n = srclen < 0 ? rt_wstrlen(src) + 1u : (uint32_t)srclen;

    for (i = 0; i < n; i++)
    {
        uint32_t c = rd16(src + 2u * i);
        uint8_t bytes[4];
        uint32_t nb, k;
        if (cp == 65001u)
        {
            if (c >= 0xD800u && c <= 0xDBFFu && i + 1 < n)
            {
                uint32_t lo = rd16(src + 2u * (i + 1));
                if (lo >= 0xDC00u && lo <= 0xDFFFu)
                {
                    c = 0x10000u + ((c - 0xD800u) << 10) + (lo - 0xDC00u);
                    i++;
                }
            }
            if (c < 0x80u) { bytes[0] = (uint8_t)c; nb = 1; }
            else if (c < 0x800u) { bytes[0] = (uint8_t)(0xC0u | (c >> 6)); bytes[1] = (uint8_t)(0x80u | (c & 0x3Fu)); nb = 2; }
            else if (c < 0x10000u)
            {
                bytes[0] = (uint8_t)(0xE0u | (c >> 12));
                bytes[1] = (uint8_t)(0x80u | ((c >> 6) & 0x3Fu));
                bytes[2] = (uint8_t)(0x80u | (c & 0x3Fu));
                nb = 3;
            }
            else
            {
                bytes[0] = (uint8_t)(0xF0u | (c >> 18));
                bytes[1] = (uint8_t)(0x80u | ((c >> 12) & 0x3Fu));
                bytes[2] = (uint8_t)(0x80u | ((c >> 6) & 0x3Fu));
                bytes[3] = (uint8_t)(0x80u | (c & 0x3Fu));
                nb = 4;
            }
        }
        else
        {
            int b = best_fit(c, cp == 437u, (flags & 0x400u) != 0);
            if (b < 0)
            {
                b = def;
                used_default = true;
            }
            bytes[0] = (uint8_t)b;
            nb = 1;
        }
        for (k = 0; k < nb; k++)
        {
            if (dstlen > 0)
            {
                if (out >= (uint32_t)dstlen)
                {
                    rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
                    RET(0);
                    return;
                }
                wr8(dst + out, bytes[k]);
            }
            out++;
        }
    }
    if (used != 0) wr32(used, used_default ? 1u : 0u);
    RET(out);
}

/* ---------------------------------------------------------------- string comparison and mapping */

#define NORM_IGNORECASE 0x1u
#define NORM_IGNORENONSPACE 0x2u
#define NORM_IGNORESYMBOLS 0x4u
#define SORT_STRINGSORT 0x1000u
#define NORM_IGNOREKANATYPE 0x10000u
#define NORM_IGNOREWIDTH 0x20000u

/* Locales of the comparison and mapping functions: English (United
   States), the default locales (which are English) and the invariant
   locale. */
static bool lcid_sort_ok(uint32_t lcid)
{
    return lcid_is_en_us(lcid) || lcid == 0x007Fu;
}

static pthread_once_t en_us_once = PTHREAD_ONCE_INIT;
static CFLocaleRef en_us_cf;
static locale_t en_us_c;
static CFCharacterSetRef illegal_chars, decimal_digits; /* immutable predefined sets */

static void make_en_us(void)
{
    en_us_cf = CFLocaleCreate(kCFAllocatorDefault, CFSTR("en_US"));
    if (en_us_cf == NULL) rt_fatal("CFLocaleCreate(en_US) failed");
    en_us_c = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", (locale_t)0);
    if (en_us_c == (locale_t)0) rt_fatal("newlocale(en_US.UTF-8) failed");
    illegal_chars = CFCharacterSetGetPredefined(kCFCharacterSetIllegal);
    decimal_digits = CFCharacterSetGetPredefined(kCFCharacterSetDecimalDigit);
}

static void en_us_init(void)
{
    if (pthread_once(&en_us_once, make_en_us) != 0) rt_fatal("locale setup failed");
}

/* A UTF-16 copy of n units at g without the units that the comparison
   ignores. */
static UniChar *sort_units(const uint8_t *g, uint32_t n, uint32_t flags, bool drop_word_punct, CFIndex *len)
{
    UniChar *u = malloc((size_t)(n ? n : 1u) * sizeof(UniChar));
    uint32_t i;
    CFIndex k = 0;
    if (u == NULL) rt_fatal("out of host memory (CompareStringW)");
    for (i = 0; i < n; i++)
    {
        uint16_t c;
        memcpy(&c, g + 2u * i, 2);
        if (c == 0) continue; /* NUL has no sort weight */
        if (drop_word_punct && (c == '-' || c == '\'' || c == 0x2019u)) continue;
        if ((flags & NORM_IGNORESYMBOLS) && (iswpunct_l(c, en_us_c) || iswspace_l(c, en_us_c))) continue;
        u[k++] = c;
    }
    *len = k;
    return u;
}

static CFComparisonResult compare_units(const uint8_t *g1, uint32_t n1, const uint8_t *g2, uint32_t n2, uint32_t flags, bool drop_word_punct)
{
    CFIndex l1, l2;
    UniChar *u1 = sort_units(g1, n1, flags, drop_word_punct, &l1);
    UniChar *u2 = sort_units(g2, n2, flags, drop_word_punct, &l2);
    CFStringRef s1 = CFStringCreateWithCharactersNoCopy(kCFAllocatorDefault, u1, l1, kCFAllocatorNull);
    CFStringRef s2 = CFStringCreateWithCharactersNoCopy(kCFAllocatorDefault, u2, l2, kCFAllocatorNull);
    CFStringCompareFlags cf = kCFCompareLocalized | kCFCompareNonliteral;
    CFComparisonResult r;
    if (s1 == NULL || s2 == NULL) rt_fatal("out of host memory (CFString)");
    if (flags & NORM_IGNORECASE) cf |= kCFCompareCaseInsensitive;
    if (flags & NORM_IGNORENONSPACE) cf |= kCFCompareDiacriticInsensitive;
    if (flags & NORM_IGNOREWIDTH) cf |= kCFCompareWidthInsensitive;
    r = CFStringCompareWithOptionsAndLocale(s1, s2, CFRangeMake(0, l1), cf, en_us_cf);
    CFRelease(s1);
    CFRelease(s2);
    free(u1);
    free(u2);
    return r;
}

/* The comparison of CompareStringW (word sort unless SORT_STRINGSORT). */
static CFComparisonResult compare_strings(const uint8_t *s1, uint32_t l1, const uint8_t *s2, uint32_t l2, uint32_t flags)
{
    CFComparisonResult r;
    if (flags & SORT_STRINGSORT) return compare_units(s1, l1, s2, l2, flags, false);
    r = compare_units(s1, l1, s2, l2, flags, true);
    if (r == kCFCompareEqualTo) r = compare_units(s1, l1, s2, l2, flags, false);
    return r;
}

/* int CompareStringW(LCID, DWORD flags, LPCWSTR s1, int n1, LPCWSTR s2,
   int n2): CSTR_LESS_THAN (1), CSTR_EQUAL (2) or CSTR_GREATER_THAN (3).
   The order is the Unicode collation of the en_US locale of the host.
   Word sort (the default) gives the hyphen and the apostrophe a weight
   only when the strings are equal without them, as Windows does;
   SORT_STRINGSORT gives them the weight of other symbols. Kana type
   (NORM_IGNOREKANATYPE) has no effect on this order. */
void rt_CompareStringW(_cpu *cpu)
{
    uint32_t lcid = ARG(0), flags = ARG(1), s1 = ARG(2), s2 = ARG(4);
    int32_t n1 = (int32_t)ARG(3), n2 = (int32_t)ARG(5);
    uint32_t l1, l2;
    CFComparisonResult r;

    if (!lcid_sort_ok(lcid) || s1 == 0 || s2 == 0 || n1 < -1 || n2 < -1)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (flags & ~(NORM_IGNORECASE | NORM_IGNORENONSPACE | NORM_IGNORESYMBOLS | SORT_STRINGSORT | NORM_IGNOREKANATYPE |
                  NORM_IGNOREWIDTH))
    {
        rt_set_last_error(ERROR_INVALID_FLAGS);
        RET(0);
        return;
    }
    en_us_init();
    l1 = n1 < 0 ? rt_wstrlen(s1) : (uint32_t)n1;
    l2 = n2 < 0 ? rt_wstrlen(s2) : (uint32_t)n2;
    r = compare_strings(G2H(s1), l1, G2H(s2), l2, flags);
    RET(r == kCFCompareLessThan ? 1u : r == kCFCompareEqualTo ? 2u : 3u);
}

/* int lstrcmpiA(LPCSTR, LPCSTR): CompareStringA(LOCALE_USER_DEFAULT,
   NORM_IGNORECASE, ...) - 2, the word sort of the en_US locale. NULL is
   less than any string. */
void rt_lstrcmpiA(_cpu *cpu)
{
    uint32_t a = ARG(0), b = ARG(1), la = 0, lb = 0, i;
    uint16_t *wa, *wb;
    CFComparisonResult r;
    if (a == 0 || b == 0)
    {
        RET(a == b ? 0u : a == 0 ? (uint32_t)-1 : 1u);
        return;
    }
    while (rd8(a + la) != 0) la++;
    while (rd8(b + lb) != 0) lb++;
    wa = malloc(2u * (size_t)(la + 1u));
    wb = malloc(2u * (size_t)(lb + 1u));
    if (wa == NULL || wb == NULL) rt_fatal("out of host memory (lstrcmpiA)");
    for (i = 0; i < la; i++) wa[i] = rt_cp1252_to_unicode(rd8(a + i));
    for (i = 0; i < lb; i++) wb[i] = rt_cp1252_to_unicode(rd8(b + i));
    en_us_init();
    r = compare_strings((const uint8_t *)wa, la, (const uint8_t *)wb, lb, NORM_IGNORECASE);
    free(wa);
    free(wb);
    RET(r == kCFCompareLessThan ? (uint32_t)-1 : r == kCFCompareEqualTo ? 0u : 1u);
}

#define LCMAP_LOWERCASE 0x100u
#define LCMAP_UPPERCASE 0x200u
#define LCMAP_SORTKEY 0x400u
#define LCMAP_BYTEREV 0x800u
#define LCMAP_LINGUISTIC_CASING 0x1000000u

/* int LCMapStringW(LCID, DWORD flags, LPCWSTR src, int n, LPWSTR dst,
   int dstn): lower case and upper case mapping of each UTF-16 unit (the
   length does not change, as on Windows) and byte reversal. English has
   no linguistic casing rules, so LCMAP_LINGUISTIC_CASING has no effect. */
void rt_LCMapStringW(_cpu *cpu)
{
    uint32_t lcid = ARG(0), flags = ARG(1), src = ARG(2), dst = ARG(4);
    int32_t n = (int32_t)ARG(3), dstn = (int32_t)ARG(5);
    uint32_t len, i, the_case;

    if (flags & LCMAP_SORTKEY) rt_fatal("LCMapStringW: LCMAP_SORTKEY is not supported");
    if (flags & ~(LCMAP_LOWERCASE | LCMAP_UPPERCASE | LCMAP_BYTEREV | LCMAP_LINGUISTIC_CASING))
    {
        if (flags & 0x06F00000u) rt_fatal("LCMapStringW: flags 0x%x (kana, width or Chinese mapping) are not supported", flags);
        rt_set_last_error(ERROR_INVALID_FLAGS);
        RET(0);
        return;
    }
    the_case = flags & (LCMAP_LOWERCASE | LCMAP_UPPERCASE);
    if (the_case == (LCMAP_LOWERCASE | LCMAP_UPPERCASE) || flags == 0 ||
        ((flags & LCMAP_LINGUISTIC_CASING) && the_case == 0))
    {
        rt_set_last_error(ERROR_INVALID_FLAGS);
        RET(0);
        return;
    }
    if (!lcid_sort_ok(lcid) || src == 0 || n == 0 || n < -1 || dstn < 0 || (dstn > 0 && dst == 0))
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    len = n < 0 ? rt_wstrlen(src) + 1u : (uint32_t)n;
    if (dstn == 0)
    {
        RET(len);
        return;
    }
    if ((uint32_t)dstn < len)
    {
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        RET(0);
        return;
    }
    en_us_init();
    /* src and dst can be the same buffer: each unit is read before it is
       written. */
    for (i = 0; i < len; i++)
    {
        uint32_t c = rd16(src + 2u * i);
        if (c < 0xD800u || c > 0xDFFFu)
        {
            if (the_case == LCMAP_LOWERCASE) c = (uint32_t)towlower_l((wint_t)c, en_us_c);
            else if (the_case == LCMAP_UPPERCASE) c = (uint32_t)towupper_l((wint_t)c, en_us_c);
            if (c > 0xFFFFu) rt_fatal("LCMapStringW: case mapping left the basic multilingual plane");
        }
        if (flags & LCMAP_BYTEREV) c = ((c & 0xFFu) << 8) | (c >> 8);
        wr16(dst + 2u * i, (uint16_t)c);
    }
    RET(len);
}

/* BOOL GetStringTypeW(DWORD type, LPCWSTR src, int n, LPWORD out):
   CT_CTYPE1 classes of each UTF-16 unit from the Unicode character
   classes of the host en_US.UTF-8 locale. */
void rt_GetStringTypeW(_cpu *cpu)
{
    uint32_t type = ARG(0), src = ARG(1), outp = ARG(3);
    int32_t n = (int32_t)ARG(2);
    uint32_t len, i;

    if (type == 2u || type == 4u) rt_fatal("GetStringTypeW: CT_CTYPE2 and CT_CTYPE3 are not supported");
    if (type != 1u || src == 0 || outp == 0 || n == 0 || n < -1)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    en_us_init();
    len = n < 0 ? rt_wstrlen(src) + 1u : (uint32_t)n;
    for (i = 0; i < len; i++)
    {
        uint16_t c = rd16(src + 2u * i);
        uint16_t t = 0;
        if (c >= 0xD800u && c <= 0xDFFFu)
        {
            t = 0x200; /* C1_DEFINED */
        }
        else
        {
            wint_t w = c;
            if (iswupper_l(w, en_us_c)) t |= 0x001;  /* C1_UPPER */
            if (iswlower_l(w, en_us_c)) t |= 0x002;  /* C1_LOWER */
            /* Decimal digits and the superscript digits 1, 2 and 3 of
               Latin-1, as on Windows. */
            if (CFCharacterSetIsCharacterMember(decimal_digits, c) || c == 0xB2u || c == 0xB3u || c == 0xB9u) t |= 0x004;
            if (iswspace_l(w, en_us_c)) t |= 0x008;  /* C1_SPACE */
            if (iswpunct_l(w, en_us_c)) t |= 0x010;  /* C1_PUNCT */
            if (iswcntrl_l(w, en_us_c)) t |= 0x020;  /* C1_CNTRL */
            if (iswblank_l(w, en_us_c)) t |= 0x040;  /* C1_BLANK */
            if (iswxdigit_l(w, en_us_c)) t |= 0x080; /* C1_XDIGIT */
            if (iswalpha_l(w, en_us_c)) t |= 0x100;  /* C1_ALPHA */
            if (!CFCharacterSetIsCharacterMember(illegal_chars, c)) t |= 0x200; /* C1_DEFINED */
        }
        wr16(outp + 2u * i, t);
    }
    RET(1);
}

/* ---------------------------------------------------------------- FormatMessageA and FormatMessageW */

#define FM_ALLOCATE_BUFFER 0x100u
#define FM_IGNORE_INSERTS 0x200u
#define FM_FROM_STRING 0x400u
#define FM_FROM_HMODULE 0x800u
#define FM_FROM_SYSTEM 0x1000u
#define FM_ARGUMENT_ARRAY 0x2000u
#define FM_MAX_WIDTH_MASK 0xFFu

/* English system messages of the error codes that the runtime sets. */
static const struct { uint32_t id; const char *text; } system_messages[] = {
    { 0, "The operation completed successfully.\r\n" },
    { 2, "The system cannot find the file specified.\r\n" },
    { 3, "The system cannot find the path specified.\r\n" },
    { 4, "The system cannot open the file.\r\n" },
    { 5, "Access is denied.\r\n" },
    { 6, "The handle is invalid.\r\n" },
    { 8, "Not enough storage is available to process this command.\r\n" },
    { 13, "The data is invalid.\r\n" },
    { 14, "Not enough storage is available to complete this operation.\r\n" },
    { 18, "There are no more files.\r\n" },
    { 19, "The media is write protected.\r\n" },
    { 32, "The process cannot access the file because it is being used by another process.\r\n" },
    { 38, "Reached the end of the file.\r\n" },
    { 50, "The request is not supported.\r\n" },
    { 80, "The file exists.\r\n" },
    { 87, "The parameter is incorrect.\r\n" },
    { 122, "The data area passed to a system call is too small.\r\n" },
    { 123, "The filename, directory name, or volume label syntax is incorrect.\r\n" },
    { 126, "The specified module could not be found.\r\n" },
    { 127, "The specified procedure could not be found.\r\n" },
    { 183, "Cannot create a file when that file already exists.\r\n" },
    { 206, "The filename or extension is too long.\r\n" },
    { 234, "More data is available.\r\n" },
    { 259, "No more data is available.\r\n" },
    { 487, "Attempt to access invalid address.\r\n" },
    { 1004, "Invalid flags.\r\n" },
    { 1113, "No mapping for the Unicode character exists in the target multi-byte code page.\r\n" },
};

typedef struct fm_out {
    char *buf;
    size_t len, cap;
} fm_out;

static void fm_put(fm_out *o, char c)
{
    if (o->len + 1 >= o->cap)
    {
        size_t ncap = o->cap ? o->cap * 2 : 256;
        char *nb;
        if (ncap > 0x1000000u) rt_fatal("FormatMessage: message too long");
        nb = realloc(o->buf, ncap);
        if (nb == NULL) rt_fatal("out of host memory");
        o->buf = nb;
        o->cap = ncap;
    }
    o->buf[o->len++] = c;
    o->buf[o->len] = 0;
}

/* A wide guest string as a new UTF-8 host string. */
static char *wide_to_host(uint32_t g, const char *fn)
{
    size_t max = (size_t)rt_wstrlen(g) * 3u + 1u;
    char *h = malloc(max);
    if (h == NULL) rt_fatal("out of host memory");
    if (rt_gstr_w(h, max, g) != RT_STR_OK) rt_fatal("%s: a wide string has an unpaired surrogate", fn);
    return h;
}

/* One FormatMessage insert: spec is the printf format of the insert
   without the "%" ("s" when the insert has no !format!), a the 32-bit
   argument. Flags (- + space # 0), a width and a precision of digits are
   supported, with the conversions d i u x X o c and the strings s, S, hs
   and ls. In FormatMessageW, s and ls are UTF-16 strings and S and hs are
   code page 1252 strings; in FormatMessageA, s and hs are code page 1252
   strings and S and ls are UTF-16 strings. A code page 1252 string is
   copied byte for byte. */
static void fm_insert(fm_out *o, const char *spec, uint32_t a, bool wide, const char *fn)
{
    char fmt[32], num[64];
    size_t k = 0, len = strlen(spec);
    char conv = len > 0 ? spec[len - 1] : 0;
    bool wide_str;
    int r;
    while (k < len && strchr("-+ #0", spec[k]) != NULL) k++;
    while (k < len && spec[k] >= '0' && spec[k] <= '9') k++;
    if (k < len && spec[k] == '.')
    {
        k++;
        while (k < len && spec[k] >= '0' && spec[k] <= '9') k++;
    }
    if (conv == 's' || conv == 'S')
    {
        size_t m = len - 1 - k; /* the size prefix */
        if (m == 0)
            wide_str = (conv == 's') == wide;
        else if (m == 1 && (spec[k] == 'h' || spec[k] == 'l'))
            wide_str = spec[k] == 'l';
        else
            rt_fatal("%s: insert format !%s! is not supported", fn, spec);
        if (k != 0) rt_fatal("%s: insert format !%s! with a width or a precision is not supported", fn, spec);
        if (a == 0) rt_fatal("%s: NULL string insert", fn);
        if (wide_str)
        {
            char *ins = wide_to_host(a, fn);
            for (const char *q = ins; *q; q++) fm_put(o, *q);
            free(ins);
        }
        else
        {
            for (const char *q = G2H(a); *q; q++) fm_put(o, *q);
        }
        return;
    }
    if (k + 1 != len || strchr("diuxXoc", conv) == NULL || conv == 0)
        rt_fatal("%s: insert format !%s! is not supported", fn, spec);
    if (snprintf(fmt, sizeof fmt, "%%%s", spec) >= (int)sizeof fmt) rt_fatal("%s: insert format !%s! is too long", fn, spec);
    if (conv == 'c')
    {
        if (a > 0x7Fu) rt_fatal("%s: character insert 0x%x is not ASCII", fn, a);
        r = snprintf(num, sizeof num, fmt, (int)a);
    }
    else if (conv == 'd' || conv == 'i')
        r = snprintf(num, sizeof num, fmt, (int32_t)a);
    else
        r = snprintf(num, sizeof num, fmt, a);
    if (r < 0 || (size_t)r >= sizeof num) rt_fatal("%s: insert format !%s! gives too long a result", fn, spec);
    for (const char *q = num; *q; q++) fm_put(o, *q);
}

/* DWORD FormatMessageA/W(DWORD flags, LPCVOID source, DWORD id, DWORD lang,
   LPTSTR buf, DWORD size, va_list *args). The A form works on the bytes of
   code page 1252; the W form converts the guest strings to UTF-8, formats
   and writes UTF-16. size and the result count characters of the form. */
static void format_message(_cpu *cpu, bool wide)
{
    const char *fn = wide ? "FormatMessageW" : "FormatMessageA";
    uint32_t flags = ARG(0), source = ARG(1), id = ARG(2), lang = ARG(3), buf = ARG(4), size = ARG(5), args = ARG(6);
    uint32_t width = flags & FM_MAX_WIDTH_MASK;
    const char *text = NULL;
    char *text_host = NULL;
    fm_out o = { NULL, 0, 0 };
    size_t i;
    uint32_t out_len;

    if (flags & FM_FROM_HMODULE) rt_fatal("%s: FORMAT_MESSAGE_FROM_HMODULE is not supported", fn);
    if (flags & FM_FROM_STRING)
    {
        if (source == 0)
        {
            rt_set_last_error(ERROR_INVALID_PARAMETER);
            RET(0);
            return;
        }
        if (wide) text = text_host = wide_to_host(source, fn);
        else text = G2H(source);
    }
    else if (flags & FM_FROM_SYSTEM)
    {
        if (lang != 0 && lang != LCID_EN_US && lang != 0x0400u && lang != 0x0800u)
            rt_fatal("%s: language 0x%x is not supported", fn, lang);
        for (i = 0; i < sizeof(system_messages) / sizeof(system_messages[0]); i++)
            if (system_messages[i].id == id) text = system_messages[i].text;
        if (text == NULL)
        {
            rt_set_last_error(ERROR_MR_MID_NOT_FOUND);
            RET(0);
            return;
        }
    }
    else
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (width != 0 && width != FM_MAX_WIDTH_MASK) rt_fatal("%s: line width %u is not supported", fn, width);

    for (const char *p = text; *p; p++)
    {
        if (width == FM_MAX_WIDTH_MASK && (*p == '\r' || *p == '\n'))
        {
            /* Line breaks become spaces; "\r\n" is one break. */
            if (*p == '\r' && p[1] == '\n') p++;
            fm_put(&o, ' ');
            continue;
        }
        if (*p != '%')
        {
            fm_put(&o, *p);
            continue;
        }
        p++;
        if (*p == 0) break;
        if (flags & FM_IGNORE_INSERTS)
        {
            fm_put(&o, '%');
            fm_put(&o, *p);
            continue;
        }
        if (*p >= '1' && *p <= '9')
        {
            unsigned n = (unsigned)(*p - '0');
            uint32_t a;
            char spec[24] = "s"; /* the printf format between the ! marks */
            if (p[1] >= '0' && p[1] <= '9') n = n * 10u + (unsigned)(*++p - '0');
            if (p[1] == '!')
            {
                const char *e = strchr(p + 2, '!');
                size_t sl;
                if (e == NULL) rt_fatal("%s: insert %%%u has no closing !", fn, n);
                sl = (size_t)(e - (p + 2));
                if (sl == 0 || sl >= sizeof spec) rt_fatal("%s: insert format of %u characters is not supported", fn, (unsigned)sl);
                memcpy(spec, p + 2, sl);
                spec[sl] = 0;
                p = e;
            }
            if (args == 0) rt_fatal("%s: insert %%%u without arguments", fn, n);
            /* With FORMAT_MESSAGE_ARGUMENT_ARRAY, args is an array of
               values; otherwise a pointer to a va_list. Each insert is one
               32-bit argument. */
            a = (flags & FM_ARGUMENT_ARRAY) ? rd32(args + 4u * (n - 1u)) : rd32(rd32(args) + 4u * (n - 1u));
            fm_insert(&o, spec, a, wide, fn);
            continue;
        }
        switch (*p)
        {
        case '0': goto done; /* end of the message, no new line */
        case 'n': fm_put(&o, '\r'); fm_put(&o, '\n'); break;
        case 'r': fm_put(&o, '\r'); break;
        case 't': fm_put(&o, '\t'); break;
        case 'b': fm_put(&o, ' '); break;
        default: fm_put(&o, *p); break; /* %% %. %! and others: the character */
        }
    }
done:
    free(text_host);
    if (o.buf == NULL)
    {
        /* An empty message: make the buffer for the terminator. */
        fm_put(&o, ' ');
        o.len = 0;
        o.buf[0] = 0;
    }

    if (wide)
    {
        size_t units;
        if (!rt_utf16_length(o.buf, &units)) rt_fatal("%s: the message is not valid UTF-8", fn);
        out_len = (uint32_t)units;
    }
    else
    {
        out_len = (uint32_t)o.len;
    }

    if (flags & FM_ALLOCATE_BUFFER)
    {
        /* size is the minimum number of characters to allocate. */
        uint32_t need = out_len + 1u, g;
        if (need < size) need = size;
        if (need > 0x7FFFFFFFu / 2u) rt_fatal("%s: buffer size %u is too large", fn, need);
        g = rt_heap_alloc(wide ? need * 2u : need);
        if (g == 0)
        {
            free(o.buf);
            rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
            RET(0);
            return;
        }
        if (wide) rt_put_wstr(g, need, o.buf); else memcpy(G2H(g), o.buf, o.len + 1);
        wr32(buf, g);
    }
    else
    {
        if (out_len + 1u > size)
        {
            free(o.buf);
            rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
            RET(0);
            return;
        }
        if (wide) rt_put_wstr(buf, size, o.buf); else memcpy(G2H(buf), o.buf, o.len + 1);
    }
    RET(out_len);
    free(o.buf);
}

void rt_FormatMessageA(_cpu *cpu) { format_message(cpu, false); }
void rt_FormatMessageW(_cpu *cpu) { format_message(cpu, true); }
