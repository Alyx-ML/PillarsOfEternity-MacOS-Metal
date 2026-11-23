/*
 * Guest memory: the 2 GiB guest window, the guest address space manager
 * (VirtualAlloc semantics) and the guest heap.
 *
 * Window layout:
 *   rt_ptrofs            guest address 0. The first 64 KiB are never
 *                        accessible, so guest NULL pointer accesses fault.
 *   image                the program image (translated code and data,
 *                        thunks, runtime data). The linker places it. It
 *                        can have more than one page range (see below).
 *   __GUEST              optional zero-fill segments of the program image.
 *                        The loader reserves them with the image, so no
 *                        other host mapping can be there. At startup the
 *                        runtime replaces them with PROT_NONE memory, and
 *                        they become free guest address space. This is
 *                        where fixed-base guest images (mono.dll, AOT
 *                        images) are mapped.
 *   free gaps            every free gap in the window is reserved at
 *                        startup (PROT_NONE). VirtualAlloc and the heap use
 *                        only these reserved gaps and the __GUEST space.
 *
 * The guest address space is managed in 64 KiB granules (Windows allocation
 * granularity). Commit state is kept per 4 KiB guest page. The host page
 * size (16 KiB on Apple silicon) is larger, so a host page is readable and
 * writable when at least one of its guest pages is committed.
 *
 * Each allocation has a type (private, mapped view or image) and an
 * allocation protection, and each committed page a Windows protection
 * value, for VirtualQuery and VirtualProtect. Protection values are
 * records only: committed private and image memory is always readable and
 * writable. A mapped view (MapViewOfFile) is a host file or shared memory
 * mapping at the guest address, with the host protection of the view.
 *
 * The guest heap is a TLSF allocator (two-level segregated fit, O(1)) on
 * pools taken from the address space manager. Blocks of 1 MiB or more are
 * separate address space allocations.
 */
#include "rt.h"
#include "rt_thread.h"

#include <errno.h>
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

uint64_t rt_ptrofs;
uint64_t pointer_offset; /* name used by upstream SR support code */

#define NUM_GRANULES ((uint32_t)(RT_WINDOW_SIZE / RT_GRANULE))
#define GUEST_PAGE 0x1000u
#define NUM_PAGES ((uint32_t)(RT_WINDOW_SIZE / GUEST_PAGE))
#define PAGES_PER_GRANULE (RT_GRANULE / GUEST_PAGE)

enum { G_UNAVAILABLE = 0, G_FREE = 1, G_USED = 2 };

static pthread_mutex_t va_lock = PTHREAD_MUTEX_INITIALIZER;

/* The address space lock and the heap lock are counted for SuspendThread
   (rt_thread.h, locks_held): a thread is never suspended while it holds
   them, so the suspending thread can use them. */
static void lock_counted(pthread_mutex_t *m)
{
    int r;
    rt_lock_count_add(1);
    r = pthread_mutex_lock(m);
    if (r != 0) rt_fatal("memory lock failed (%d)", r);
}

static void unlock_counted(pthread_mutex_t *m)
{
    int r = pthread_mutex_unlock(m);
    if (r != 0) rt_fatal("memory unlock failed (%d)", r);
    rt_lock_count_add(-1);
}
static uint8_t granule_state[NUM_GRANULES];
/* 1 for each granule of a __GUEST segment (rt_mem_use_guest_space_only). */
static uint8_t granule_guest_space[NUM_GRANULES];
static bool guest_space_only;
/* For the first granule of an allocation: number of granules. 0 elsewhere. */
static uint32_t alloc_granules[NUM_GRANULES];
/* For every granule of an allocation: the first granule. */
static uint32_t alloc_base[NUM_GRANULES];
static uint64_t page_committed[NUM_PAGES / 64];
/* For the first granule of an allocation: the type (RT_MEM_PRIVATE,
   RT_MEM_MAPPED, RT_MEM_IMAGE) and the allocation protection. */
static uint32_t alloc_type[NUM_GRANULES];
static uint16_t alloc_protect[NUM_GRANULES];
/* Protection of each committed guest page (PAGE_* values). */
static uint16_t page_protect[NUM_PAGES];
static uint32_t host_pg_size;
static uint32_t guest_pages_per_host_page;
/* The program image (the translated code and its data, mapped by the
   loader): ranges of guest pages [lo, hi), in address order, without the
   __GUEST segments. The pages are readable for the whole process life, as
   the image of a Windows module is. */
#define MAX_IMAGE_RANGES 8
typedef struct image_range {
    uint32_t lo, hi;
} image_range;
static image_range image_ranges[MAX_IMAGE_RANGES];
static unsigned num_image_ranges;

/* The image range that holds page p, or NULL. */
static const image_range *image_range_of(uint32_t p)
{
    unsigned i;
    for (i = 0; i < num_image_ranges; i++)
        if (p >= image_ranges[i].lo && p < image_ranges[i].hi) return &image_ranges[i];
    return NULL;
}

/* ---------------------------------------------------------------- helpers */

uint32_t rt_h2g_checked(const void *p)
{
    rt_fatal("host pointer %p is outside the guest window [0x%llx, 0x%llx)", p,
             (unsigned long long)rt_ptrofs, (unsigned long long)(rt_ptrofs + RT_WINDOW_SIZE));
}

int rt_gstr(char *dst, size_t max, uint32_t gstr)
{
    const char *s = G2H(gstr);
    size_t i;

    if (max == 0) return -1;
    if (s == NULL) { dst[0] = 0; return -1; }
    for (i = 0; i + 1 < max; i++)
    {
        dst[i] = s[i];
        if (s[i] == 0) return 0;
    }
    dst[max - 1] = 0;
    return (s[max - 1] == 0) ? 0 : -1;
}

/* The bitmap words are changed with atomic operations while va_lock is
   held, so that rt_va_is_committed can read them without the lock. */
static bool page_is_committed(uint32_t page)
{
    return (__atomic_load_n(&page_committed[page >> 6], __ATOMIC_RELAXED) >> (page & 63)) & 1;
}

static void set_page_committed(uint32_t page, bool v)
{
    if (v) __atomic_fetch_or(&page_committed[page >> 6], UINT64_C(1) << (page & 63), __ATOMIC_RELAXED);
    else __atomic_fetch_and(&page_committed[page >> 6], ~(UINT64_C(1) << (page & 63)), __ATOMIC_RELAXED);
}

static void *host_of(uint32_t guest)
{
    return (void *)(uintptr_t)(rt_ptrofs + guest);
}

/* Set the protection of every host page in [first_page, last_page] (guest
   page numbers) from the commit state of its guest pages. */
static void update_host_protection(uint32_t first_page, uint32_t last_page)
{
    uint32_t hp_first = first_page / guest_pages_per_host_page;
    uint32_t hp_last = last_page / guest_pages_per_host_page;
    uint32_t hp;

    for (hp = hp_first; hp <= hp_last; hp++)
    {
        uint32_t p0 = hp * guest_pages_per_host_page;
        bool any = false;
        uint32_t i;

        for (i = 0; i < guest_pages_per_host_page; i++)
        {
            if (page_is_committed(p0 + i)) { any = true; break; }
        }
        if (mprotect(host_of(p0 * GUEST_PAGE), host_pg_size, any ? (PROT_READ | PROT_WRITE) : PROT_NONE) != 0)
        {
            rt_fatal("mprotect of guest 0x%08x failed: %s", p0 * GUEST_PAGE, strerror(errno));
        }
    }
}

/* ---------------------------------------------------------------- window */

/* Segment name of the zero-fill guest space segments. */
#define SEG_GUEST "__GUEST"
#define MAX_SEGMENTS 16

typedef struct host_segment {
    uint64_t lo, hi; /* host addresses (with the slide) */
    bool guest_space;
} host_segment;

/* The segments of the image that holds the runtime and the translated code
   (the game bundle that the loader opens), in address order, without
   __PAGEZERO. The guest window must hold these segments, because guest
   addresses of the translated code, the import thunks and the COM vtables
   are offsets in the window. */
static unsigned find_segments(host_segment *segs)
{
    Dl_info info;
    const struct mach_header_64 *mh;
    const uint8_t *cmd;
    uint32_t i;
    unsigned n = 0, j;
    intptr_t slide = 0;
    bool have_text = false;

    if (dladdr((const void *)&find_segments, &info) == 0 || info.dli_fbase == NULL)
        rt_fatal("the runtime image header was not found");
    mh = (const struct mach_header_64 *)info.dli_fbase;
    if (mh->magic != MH_MAGIC_64) rt_fatal("the runtime image is not a 64-bit Mach-O image");
    /* The slide is the distance between the loaded header and the address
       of __TEXT (the segment that starts with the header). */
    cmd = (const uint8_t *)(mh + 1);
    for (i = 0; i < mh->ncmds; i++)
    {
        const struct load_command *lc = (const struct load_command *)cmd;
        if (lc->cmd == LC_SEGMENT_64 && strcmp(((const struct segment_command_64 *)lc)->segname, SEG_TEXT) == 0)
        {
            slide = (intptr_t)((uintptr_t)mh - (uintptr_t)((const struct segment_command_64 *)lc)->vmaddr);
            have_text = true;
        }
        cmd += lc->cmdsize;
    }
    if (!have_text) rt_fatal("the runtime image has no __TEXT segment");
    cmd = (const uint8_t *)(mh + 1);
    for (i = 0; i < mh->ncmds; i++)
    {
        const struct load_command *lc = (const struct load_command *)cmd;
        if (lc->cmd == LC_SEGMENT_64)
        {
            const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
            if (strcmp(seg->segname, SEG_PAGEZERO) != 0 && seg->vmsize != 0)
            {
                host_segment h;
                if (n == MAX_SEGMENTS) rt_fatal("the runtime image has more than %d segments", MAX_SEGMENTS);
                h.lo = seg->vmaddr + (uint64_t)slide;
                h.hi = h.lo + seg->vmsize;
                h.guest_space = strncmp(seg->segname, SEG_GUEST, sizeof seg->segname) == 0;
                if (h.guest_space && (seg->filesize != 0 || seg->initprot != (VM_PROT_READ | VM_PROT_WRITE)))
                    rt_fatal("the %s segment is not a read-write zero-fill segment", SEG_GUEST);
                /* Insert in address order. */
                for (j = n; j > 0 && segs[j - 1].lo > h.lo; j--) segs[j] = segs[j - 1];
                segs[j] = h;
                n++;
            }
        }
        cmd += lc->cmdsize;
    }
    if (n == 0) rt_fatal("the runtime image has no segments");
    for (j = 1; j < n; j++)
        if (segs[j].lo < segs[j - 1].hi) rt_fatal("the segments of the runtime image overlap");
    return n;
}

/* Reserve one free host range [s, e) (both granule aligned) for the guest. */
static void reserve_gap(uint64_t s, uint64_t e)
{
    void *want = (void *)(uintptr_t)s;
    void *got;
    uint32_t g;

    if (e <= s) return;
    got = mmap(want, (size_t)(e - s), PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (got == MAP_FAILED) rt_fatal("mmap of guest gap 0x%llx failed: %s", (unsigned long long)s, strerror(errno));
    if (got != want)
    {
        munmap(got, (size_t)(e - s));
        rt_fatal("guest gap 0x%llx..0x%llx could not be reserved", (unsigned long long)s, (unsigned long long)e);
    }
    for (g = (uint32_t)((s - rt_ptrofs) / RT_GRANULE); g < (uint32_t)((e - rt_ptrofs) / RT_GRANULE); g++)
    {
        granule_state[g] = G_FREE;
    }
}

void rt_mem_init(void)
{
    host_segment segs[MAX_SEGMENTS];
    unsigned nsegs, i;
    uint64_t img_lo, win_lo, win_hi, addr, free_bytes;
    long ps = sysconf(_SC_PAGESIZE);

    if (ps <= 0 || ps > (long)RT_GRANULE || ((uint32_t)ps & ((uint32_t)ps - 1)) != 0)
    {
        rt_fatal("unsupported host page size %ld", ps);
    }
    host_pg_size = (uint32_t)ps;
    guest_pages_per_host_page = host_pg_size / GUEST_PAGE;

    nsegs = find_segments(segs);
    img_lo = segs[0].lo;
    /* Guest address 0 is 2 MiB below the image. */
    if (img_lo < UINT64_C(0x200000)) rt_fatal("program image is too low in memory");
    win_lo = (img_lo - UINT64_C(0x200000)) & ~(uint64_t)(RT_GRANULE - 1);
    win_hi = win_lo + RT_WINDOW_SIZE;
    if (segs[nsegs - 1].hi > win_hi)
        rt_fatal("the runtime image (0x%llx bytes) does not fit in the guest window",
                 (unsigned long long)(segs[nsegs - 1].hi - img_lo));
    rt_ptrofs = win_lo;
    pointer_offset = win_lo;
    /* Image ranges: the runs of segments between __GUEST segments. */
    num_image_ranges = 0;
    for (i = 0; i < nsegs; i++)
    {
        uint32_t lo, hi, p;
        if (segs[i].guest_space) continue;
        lo = (uint32_t)((segs[i].lo - win_lo) / GUEST_PAGE);
        hi = (uint32_t)((segs[i].hi - win_lo + GUEST_PAGE - 1) / GUEST_PAGE);
        if (num_image_ranges != 0 && !segs[i - 1].guest_space && lo <= image_ranges[num_image_ranges - 1].hi)
        {
            image_ranges[num_image_ranges - 1].hi = hi;
        }
        else
        {
            if (num_image_ranges == MAX_IMAGE_RANGES) rt_fatal("the runtime image has more than %d page ranges", MAX_IMAGE_RANGES);
            image_ranges[num_image_ranges].lo = lo;
            image_ranges[num_image_ranges].hi = hi;
            num_image_ranges++;
        }
        for (p = lo; p < hi; p++) page_protect[p] = RT_PAGE_EXECUTE_READ;
    }
    /* Each __GUEST segment: the granules that it holds completely become
       free guest address space. MAP_FIXED replaces the zero-fill memory of
       the loader with reserved memory. */
    for (i = 0; i < nsegs; i++)
    {
        uint64_t s, e;
        void *got;
        uint32_t g;
        if (!segs[i].guest_space) continue;
        s = (segs[i].lo + RT_GRANULE - 1) & ~(uint64_t)(RT_GRANULE - 1);
        e = segs[i].hi & ~(uint64_t)(RT_GRANULE - 1);
        if (e <= s) continue;
        got = mmap((void *)(uintptr_t)s, (size_t)(e - s), PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_FIXED, -1, 0);
        if (got == MAP_FAILED || got != (void *)(uintptr_t)s)
            rt_fatal("mmap of the guest space 0x%llx..0x%llx failed: %s", (unsigned long long)s, (unsigned long long)e,
                     got == MAP_FAILED ? strerror(errno) : "wrong address");
        for (g = (uint32_t)((s - win_lo) / RT_GRANULE); g < (uint32_t)((e - win_lo) / RT_GRANULE); g++)
        {
            granule_state[g] = G_FREE;
            granule_guest_space[g] = 1;
        }
    }

    /* Reserve every free gap from the start of the image to the end of the
       window. The range below the image (guest NULL region and up to the
       image) stays unavailable. Mapped ranges (the image, the __GUEST
       space, host memory) are not gaps. */
    addr = (img_lo + RT_GRANULE - 1) & ~(uint64_t)(RT_GRANULE - 1);
    while (addr < win_hi)
    {
        mach_vm_address_t raddr = addr;
        mach_vm_size_t rsize = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t kr;
        uint64_t gap_end;

        kr = mach_vm_region(mach_task_self(), &raddr, &rsize, VM_REGION_BASIC_INFO_64,
                            (vm_region_info_t)&info, &count, &object);
        if (kr == KERN_INVALID_ADDRESS)
        {
            raddr = win_hi; /* nothing mapped above addr */
            rsize = 0;
        }
        else if (kr != KERN_SUCCESS)
        {
            rt_fatal("mach_vm_region failed: %d", kr);
        }
        gap_end = (raddr < win_hi ? (uint64_t)raddr : win_hi) & ~(uint64_t)(RT_GRANULE - 1);
        if (gap_end > addr) reserve_gap(addr, gap_end);
        if (raddr >= win_hi) break;
        addr = ((uint64_t)raddr + rsize + RT_GRANULE - 1) & ~(uint64_t)(RT_GRANULE - 1);
    }
    free_bytes = rt_va_free_bytes();
    if (free_bytes < UINT64_C(0x40000000)) rt_fatal("less than 1 GiB of guest memory is free");
}

void rt_mem_use_guest_space_only(void)
{
    uint32_t g, n = 0;
    lock_counted(&va_lock);
    for (g = 0; g < NUM_GRANULES; g++) n += granule_guest_space[g];
    guest_space_only = true;
    unlock_counted(&va_lock);
    if (n == 0) rt_fatal("rt_mem_use_guest_space_only: the program image has no __GUEST segment");
}

/* ---------------------------------------------------------------- address space */

/* The free guest address space. Takes va_lock: do not call it while
   va_lock is held. */
uint64_t rt_va_free_bytes(void)
{
    uint64_t n = 0;
    uint32_t g;
    lock_counted(&va_lock);
    for (g = 0; g < NUM_GRANULES; g++) if (granule_state[g] == G_FREE) n += RT_GRANULE;
    unlock_counted(&va_lock);
    return n;
}

static uint32_t va_reserve_locked(uint32_t addr, uint32_t count)
{
    uint32_t g, run = 0, start = 0;

    if (count == 0 || count > NUM_GRANULES) return 0;
    if (addr != 0)
    {
        uint32_t g0 = addr / RT_GRANULE;
        if ((uint64_t)g0 + count > NUM_GRANULES) return 0;
        for (g = g0; g < g0 + count; g++) if (granule_state[g] != G_FREE) return 0;
        start = g0;
    }
    else
    {
        for (g = 0; g < NUM_GRANULES; g++)
        {
            if (granule_state[g] == G_FREE && (!guest_space_only || granule_guest_space[g]))
            {
                if (run == 0) start = g;
                if (++run == count) break;
            }
            else run = 0;
        }
        if (run != count) return 0;
    }
    for (g = start; g < start + count; g++)
    {
        granule_state[g] = G_USED;
        alloc_base[g] = start;
    }
    alloc_granules[start] = count;
    alloc_type[start] = RT_MEM_PRIVATE;
    alloc_protect[start] = RT_PAGE_READWRITE;
    return start * RT_GRANULE;
}

/* The highest free range of size bytes that ends at or below 0x80000000
   (the end of the user address space of a 32-bit Windows process without
   /LARGEADDRESSAWARE), as VirtualAlloc with MEM_TOP_DOWN. Returns base or 0. */
uint32_t rt_va_reserve_top(uint32_t size)
{
    uint32_t count, g, run = 0, base = 0;
    const uint32_t top = UINT32_C(0x80000000) / RT_GRANULE;

    if (size == 0) return 0;
    count = (uint32_t)(((uint64_t)size + RT_GRANULE - 1) / RT_GRANULE);
    if (count > top) return 0;
    lock_counted(&va_lock);
    for (g = top; g > 0; g--)
    {
        if (granule_state[g - 1] != G_FREE || (guest_space_only && !granule_guest_space[g - 1]))
        {
            run = 0;
            continue;
        }
        if (++run == count)
        {
            base = va_reserve_locked((g - 1) * RT_GRANULE, count);
            break;
        }
    }
    unlock_counted(&va_lock);
    return base;
}

uint32_t rt_va_reserve(uint32_t addr, uint32_t size)
{
    uint32_t count, base;

    if (size == 0) return 0;
    count = (uint32_t)(((uint64_t)size + RT_GRANULE - 1) / RT_GRANULE);
    lock_counted(&va_lock);
    base = va_reserve_locked(addr & ~(RT_GRANULE - 1), count);
    unlock_counted(&va_lock);
    return base;
}

/* Base granule of the allocation that contains granule g, or UINT32_MAX. */
static uint32_t alloc_base_of(uint32_t g)
{
    if (g >= NUM_GRANULES || granule_state[g] != G_USED) return UINT32_MAX;
    return alloc_base[g];
}

static int range_in_one_alloc(uint32_t addr, uint32_t size)
{
    uint64_t end = (uint64_t)addr + size;
    uint32_t b;
    if (size == 0 || end > RT_WINDOW_SIZE) return 0;
    b = alloc_base_of(addr / RT_GRANULE);
    if (b == UINT32_MAX || alloc_type[b] == RT_MEM_MAPPED) return 0;
    return end <= (uint64_t)(b + alloc_granules[b]) * RT_GRANULE;
}

int rt_va_commit(uint32_t addr, uint32_t size)
{
    uint32_t p0, p1, p;

    lock_counted(&va_lock);
    if (!range_in_one_alloc(addr, size)) { unlock_counted(&va_lock); return -1; }
    p0 = addr / GUEST_PAGE;
    p1 = (uint32_t)(((uint64_t)addr + size - 1) / GUEST_PAGE);
    for (p = p0; p <= p1; p++)
    {
        /* A new page gets read-write protection; a committed page keeps
           its protection. */
        if (!page_is_committed(p)) page_protect[p] = RT_PAGE_READWRITE;
        set_page_committed(p, true);
    }
    update_host_protection(p0, p1);
    unlock_counted(&va_lock);
    return 0;
}

/* Make the host pages of guest pages [p0, p1] that have no committed guest
   page zero and inaccessible, and zero decommitted guest pages that share a
   host page with a committed page. */
static void discard_pages(uint32_t p0, uint32_t p1)
{
    uint32_t hp;
    for (hp = p0 / guest_pages_per_host_page; hp <= p1 / guest_pages_per_host_page; hp++)
    {
        uint32_t first = hp * guest_pages_per_host_page, i;
        bool any = false;
        for (i = 0; i < guest_pages_per_host_page; i++) if (page_is_committed(first + i)) { any = true; break; }
        if (!any)
        {
            void *h = host_of(first * GUEST_PAGE);
            if (mmap(h, host_pg_size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0) != h)
            {
                rt_fatal("mmap reset of guest 0x%08x failed: %s", first * GUEST_PAGE, strerror(errno));
            }
        }
        else
        {
            for (i = 0; i < guest_pages_per_host_page; i++)
            {
                uint32_t p = first + i;
                if (p >= p0 && p <= p1 && !page_is_committed(p)) memset(host_of(p * GUEST_PAGE), 0, GUEST_PAGE);
            }
        }
    }
}

int rt_va_decommit(uint32_t addr, uint32_t size)
{
    uint32_t p0, p1, p;

    lock_counted(&va_lock);
    if (!range_in_one_alloc(addr, size)) { unlock_counted(&va_lock); return -1; }
    p0 = addr / GUEST_PAGE;
    p1 = (uint32_t)(((uint64_t)addr + size - 1) / GUEST_PAGE);
    for (p = p0; p <= p1; p++) set_page_committed(p, false);
    discard_pages(p0, p1);
    unlock_counted(&va_lock);
    return 0;
}

int rt_va_release(uint32_t base)
{
    uint32_t g0, count, g, p0, p1, p;

    lock_counted(&va_lock);
    g0 = base / RT_GRANULE;
    if ((base & (RT_GRANULE - 1)) != 0 || g0 >= NUM_GRANULES || granule_state[g0] != G_USED || alloc_granules[g0] == 0 ||
        alloc_type[g0] == RT_MEM_MAPPED)
    {
        unlock_counted(&va_lock);
        return -1;
    }
    count = alloc_granules[g0];
    p0 = g0 * PAGES_PER_GRANULE;
    p1 = (g0 + count) * PAGES_PER_GRANULE - 1;
    for (p = p0; p <= p1; p++) set_page_committed(p, false);
    /* The allocation is granule aligned, so it covers whole host pages. */
    if (mmap(host_of(base), (size_t)count * RT_GRANULE, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0) != host_of(base))
    {
        rt_fatal("mmap reset of guest 0x%08x failed: %s", base, strerror(errno));
    }
    for (g = g0; g < g0 + count; g++) granule_state[g] = G_FREE;
    alloc_granules[g0] = 0;
    unlock_counted(&va_lock);
    return 0;
}

int rt_va_is_committed(uint32_t addr, uint32_t size)
{
    uint32_t p0, p1, p;
    int ok = 1;
    if (size == 0) return 1;
    if ((uint64_t)addr + size > RT_WINDOW_SIZE) return 0;
    p0 = addr / GUEST_PAGE;
    p1 = (uint32_t)(((uint64_t)addr + size - 1) / GUEST_PAGE);
    /* No lock: the bitmap reads are atomic, and the image range does not
       change after start-up. A result is only valid until the next commit
       or decommit, as with the lock. */
    for (p = p0; p <= p1; p++)
    {
        if (!page_is_committed(p) && image_range_of(p) == NULL) { ok = 0; break; }
    }
    return ok;
}

int rt_guest_in_program_image(uint32_t g)
{
    return image_range_of(g / GUEST_PAGE) != NULL;
}

/* ---------------------------------------------------------------- allocation information */

void rt_va_set_alloc_info(uint32_t base, uint32_t type, uint32_t protect)
{
    uint32_t g = base / RT_GRANULE;
    lock_counted(&va_lock);
    if ((base & (RT_GRANULE - 1)) != 0 || g >= NUM_GRANULES || granule_state[g] != G_USED || alloc_granules[g] == 0)
        rt_fatal("rt_va_set_alloc_info: 0x%08x is not an allocation base", base);
    if (type != RT_MEM_PRIVATE && type != RT_MEM_IMAGE) rt_fatal("rt_va_set_alloc_info: type 0x%x", type);
    if (alloc_type[g] == RT_MEM_MAPPED) rt_fatal("rt_va_set_alloc_info: 0x%08x is a mapped view", base);
    alloc_type[g] = type;
    alloc_protect[g] = (uint16_t)protect;
    unlock_counted(&va_lock);
}

int rt_va_protect(uint32_t addr, uint32_t size, uint32_t protect, uint32_t *old)
{
    uint32_t p0, p1, p;
    bool image;
    const image_range *ir;
    if (size == 0 || (uint64_t)addr + size > RT_WINDOW_SIZE) return -1;
    p0 = addr / GUEST_PAGE;
    p1 = (uint32_t)(((uint64_t)addr + size - 1) / GUEST_PAGE);
    ir = image_range_of(p0);
    image = ir != NULL && p1 < ir->hi;
    lock_counted(&va_lock);
    if (!image)
    {
        if (!range_in_one_alloc(addr, size))
        {
            unlock_counted(&va_lock);
            return -1;
        }
        for (p = p0; p <= p1; p++)
        {
            if (!page_is_committed(p))
            {
                unlock_counted(&va_lock);
                return -1;
            }
        }
    }
    *old = page_protect[p0];
    for (p = p0; p <= p1; p++) page_protect[p] = (uint16_t)protect;
    unlock_counted(&va_lock);
    return 0;
}

/* Does granule gr hold a page of the program image? */
static bool granule_has_image(uint32_t gr)
{
    uint32_t p = gr * PAGES_PER_GRANULE;
    unsigned i;
    for (i = 0; i < num_image_ranges; i++)
        if (p < image_ranges[i].hi && p + PAGES_PER_GRANULE > image_ranges[i].lo) return true;
    return false;
}

/* The state of page p (RT_MEM_COMMIT or RT_MEM_RESERVE) and its
   protection (0 for a reserved page). va_lock held. */
static void page_state(uint32_t p, uint32_t *state, uint32_t *protect)
{
    if (page_is_committed(p))
    {
        *state = RT_MEM_COMMIT;
        *protect = page_protect[p];
    }
    else
    {
        *state = RT_MEM_RESERVE;
        *protect = 0;
    }
}

int rt_va_query(uint32_t addr, rt_va_region *r)
{
    uint32_t page = addr / GUEST_PAGE, g, p;
    const image_range *ir;
    unsigned i;
    if ((uint64_t)addr >= RT_WINDOW_SIZE) return -1;
    memset(r, 0, sizeof(*r));
    r->base = page * GUEST_PAGE;
    ir = image_range_of(page);
    if (ir != NULL)
    {
        /* A page range of the program image: one image allocation,
           committed. */
        uint32_t prot = page_protect[page];
        r->alloc_base = ir->lo * GUEST_PAGE;
        r->alloc_protect = RT_PAGE_EXECUTE_READ;
        r->state = RT_MEM_COMMIT;
        r->protect = prot;
        r->type = RT_MEM_IMAGE;
        lock_counted(&va_lock);
        for (p = page; p < ir->hi && page_protect[p] == prot; p++) {}
        unlock_counted(&va_lock);
        r->size = (p - page) * GUEST_PAGE;
        return 0;
    }
    g = addr / RT_GRANULE;
    lock_counted(&va_lock);
    if (granule_state[g] == G_USED)
    {
        uint32_t b = alloc_base[g], end = (b + alloc_granules[b]) * PAGES_PER_GRANULE, state, prot, s2, p2;
        page_state(page, &state, &prot);
        for (p = page + 1; p < end; p++)
        {
            page_state(p, &s2, &p2);
            if (s2 != state || p2 != prot) break;
        }
        r->alloc_base = b * RT_GRANULE;
        r->alloc_protect = alloc_protect[b];
        r->state = state;
        r->protect = prot;
        r->type = alloc_type[b];
        r->size = (p - page) * GUEST_PAGE;
    }
    else
    {
        /* Free granules, or granules that the guest cannot use (the host
           uses them, or they are near the program image): a reserved
           region without access. The first granule is free and never
           available, as the first 64 KiB on Windows. */
        uint8_t st = granule_state[g];
        bool is_free = st == G_FREE || g == 0;
        uint64_t end = (uint64_t)(g + 1) * RT_GRANULE;
        if (g != 0)
            while (end < RT_WINDOW_SIZE && granule_state[end / RT_GRANULE] == st && !granule_has_image((uint32_t)(end / RT_GRANULE)))
                end += RT_GRANULE;
        for (i = 0; i < num_image_ranges; i++)
            if (r->base < image_ranges[i].lo * GUEST_PAGE && end > image_ranges[i].lo * GUEST_PAGE)
                end = image_ranges[i].lo * GUEST_PAGE;
        r->size = (uint32_t)(end - r->base);
        if (is_free)
        {
            r->state = RT_MEM_FREE;
            r->protect = RT_PAGE_NOACCESS;
        }
        else
        {
            uint32_t lo = g, lo_addr;
            while (lo > 1 && granule_state[lo - 1] == G_UNAVAILABLE && !granule_has_image(lo - 1)) lo--;
            lo_addr = lo * RT_GRANULE;
            for (i = 0; i < num_image_ranges; i++)
                if (r->base >= image_ranges[i].hi * GUEST_PAGE && lo_addr < image_ranges[i].hi * GUEST_PAGE)
                    lo_addr = image_ranges[i].hi * GUEST_PAGE;
            r->alloc_base = lo_addr;
            r->alloc_protect = RT_PAGE_NOACCESS;
            r->state = RT_MEM_RESERVE;
            r->type = RT_MEM_PRIVATE;
        }
    }
    unlock_counted(&va_lock);
    return 0;
}

/* ---------------------------------------------------------------- mapped views */

uint32_t rt_va_map_view(uint32_t size, uint32_t protect, int (*map)(void *host, size_t len, void *ctx), void *ctx, int *err)
{
    uint32_t count, base, g0, p0, p1, p;
    size_t len;
    int e;
    *err = 0;
    if (size == 0) rt_fatal("rt_va_map_view of 0 bytes");
    count = (uint32_t)(((uint64_t)size + RT_GRANULE - 1) / RT_GRANULE);
    len = (size_t)(((uint64_t)size + host_pg_size - 1) / host_pg_size * host_pg_size);
    lock_counted(&va_lock);
    base = va_reserve_locked(0, count);
    if (base == 0)
    {
        unlock_counted(&va_lock);
        return 0;
    }
    g0 = base / RT_GRANULE;
    e = map(host_of(base), len, ctx);
    if (e != 0)
    {
        uint32_t g;
        for (g = g0; g < g0 + count; g++) granule_state[g] = G_FREE;
        alloc_granules[g0] = 0;
        unlock_counted(&va_lock);
        *err = e;
        return 0;
    }
    alloc_type[g0] = RT_MEM_MAPPED;
    alloc_protect[g0] = (uint16_t)protect;
    p0 = base / GUEST_PAGE;
    p1 = (uint32_t)(((uint64_t)base + size - 1) / GUEST_PAGE);
    for (p = p0; p <= p1; p++)
    {
        page_protect[p] = (uint16_t)protect;
        set_page_committed(p, true);
    }
    unlock_counted(&va_lock);
    return base;
}

int rt_va_unmap_view(uint32_t addr)
{
    uint32_t g = addr / RT_GRANULE, b, count, p, gg;
    if ((uint64_t)addr >= RT_WINDOW_SIZE) return -1;
    lock_counted(&va_lock);
    if (granule_state[g] != G_USED || alloc_type[alloc_base[g]] != RT_MEM_MAPPED)
    {
        unlock_counted(&va_lock);
        return -1;
    }
    b = alloc_base[g];
    count = alloc_granules[b];
    for (p = b * PAGES_PER_GRANULE; p < (b + count) * PAGES_PER_GRANULE; p++) set_page_committed(p, false);
    if (mmap(host_of(b * RT_GRANULE), (size_t)count * RT_GRANULE, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0) !=
        host_of(b * RT_GRANULE))
        rt_fatal("mmap reset of view 0x%08x failed: %s", b * RT_GRANULE, strerror(errno));
    for (gg = b; gg < b + count; gg++) granule_state[gg] = G_FREE;
    alloc_granules[b] = 0;
    alloc_type[b] = 0;
    unlock_counted(&va_lock);
    return 0;
}

uint32_t rt_host_page_size(void)
{
    return host_pg_size;
}

/* Reserve and commit a whole allocation. Returns guest base or 0. */
static uint32_t va_alloc_committed(uint32_t size)
{
    uint32_t base = rt_va_reserve(0, size);
    if (base == 0) return 0;
    if (rt_va_commit(base, size) != 0) rt_fatal("commit of new allocation 0x%08x failed", base);
    return base;
}

/* ---------------------------------------------------------------- heap (TLSF) */

/*
 * Block layout (all sizes are multiples of 16):
 *   +0  prev_phys  host pointer to the physically previous block
 *   +8  size       block size including this 16-byte header; bit 0: free,
 *                  bit 1: last block of a pool
 *   +16 payload    for free blocks: next_free, prev_free (host pointers)
 */
typedef struct tblock {
    struct tblock *prev_phys;
    uint64_t size;
    struct tblock *next_free;
    struct tblock *prev_free;
} tblock;

#define TB_FREE UINT64_C(1)
#define TB_LAST UINT64_C(2)
#define TB_FLAGS UINT64_C(15)
#define TB_HDR 16u
#define TB_MIN 32u
#define SL_LOG 4
#define SL_COUNT (1 << SL_LOG)
#define FL_COUNT 32
#define LARGE_LIMIT 0x100000u
#define POOL_SIZE 0x1000000u
#define LARGE_MAGIC 0x4c524148u /* "HARL" */

static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t fl_bitmap;
static uint32_t sl_bitmap[FL_COUNT];
static tblock *free_heads[FL_COUNT][SL_COUNT];
/* Kind of each granule for rt_heap_free: 1 = TLSF pool, 2 = large block. */
static uint8_t heap_granule_kind[NUM_GRANULES];

static uint64_t tb_size(const tblock *b) { return b->size & ~TB_FLAGS; }
static tblock *tb_next(tblock *b) { return (tblock *)((uint8_t *)b + tb_size(b)); }

static void mapping(uint64_t size, int *fl, int *sl)
{
    if (size < (UINT64_C(1) << (SL_LOG + 4)))
    {
        *fl = 0;
        *sl = (int)(size >> 4) & (SL_COUNT - 1);
    }
    else
    {
        int f = 63 - __builtin_clzll(size);
        *sl = (int)(size >> (f - SL_LOG)) & (SL_COUNT - 1);
        *fl = f - (SL_LOG + 4) + 1;
    }
}

static void remove_free(tblock *b)
{
    int fl, sl;
    mapping(tb_size(b), &fl, &sl);
    if (b->prev_free) b->prev_free->next_free = b->next_free;
    else
    {
        free_heads[fl][sl] = b->next_free;
        if (b->next_free == NULL)
        {
            sl_bitmap[fl] &= ~(1u << sl);
            if (sl_bitmap[fl] == 0) fl_bitmap &= ~(1u << fl);
        }
    }
    if (b->next_free) b->next_free->prev_free = b->prev_free;
    b->next_free = b->prev_free = NULL;
}

static void insert_free(tblock *b)
{
    int fl, sl;
    mapping(tb_size(b), &fl, &sl);
    b->prev_free = NULL;
    b->next_free = free_heads[fl][sl];
    if (b->next_free) b->next_free->prev_free = b;
    free_heads[fl][sl] = b;
    fl_bitmap |= 1u << fl;
    sl_bitmap[fl] |= 1u << sl;
}

static tblock *find_free(uint64_t size)
{
    int fl, sl;
    uint32_t slmap, flmap;

    /* round the request up to the next list so every block in it fits */
    if (size >= (UINT64_C(1) << (SL_LOG + 4)))
    {
        size += (UINT64_C(1) << (63 - __builtin_clzll(size) - SL_LOG)) - 1;
    }
    mapping(size, &fl, &sl);
    if (fl >= FL_COUNT) return NULL;
    slmap = sl_bitmap[fl] & (~0u << sl);
    if (slmap == 0)
    {
        flmap = (fl + 1 < FL_COUNT) ? (fl_bitmap & (~0u << (fl + 1))) : 0;
        if (flmap == 0) return NULL;
        fl = __builtin_ctz(flmap);
        slmap = sl_bitmap[fl];
    }
    sl = __builtin_ctz(slmap);
    return free_heads[fl][sl];
}

static int add_pool(uint32_t min_payload)
{
    uint64_t want = (uint64_t)min_payload + 2 * TB_HDR + TB_MIN;
    uint32_t size, base, g;
    tblock *b, *end;

    if (want < POOL_SIZE) want = POOL_SIZE;
    if (want > UINT32_MAX - RT_GRANULE) return -1;
    size = (uint32_t)((want + RT_GRANULE - 1) & ~(uint64_t)(RT_GRANULE - 1));
    base = va_alloc_committed(size);
    if (base == 0) return -1;
    for (g = base / RT_GRANULE; g < (base + size) / RT_GRANULE; g++) heap_granule_kind[g] = 1;
    b = (tblock *)G2H(base);
    b->prev_phys = NULL;
    b->size = (uint64_t)(size - TB_HDR) | TB_FREE;
    end = tb_next(b);
    end->prev_phys = b;
    end->size = TB_LAST; /* size 0, used */
    insert_free(b);
    return 0;
}

static uint32_t tlsf_alloc_locked(uint64_t need)
{
    tblock *b = find_free(need);
    uint64_t bs;

    if (b == NULL)
    {
        if (add_pool((uint32_t)need) != 0) return 0;
        b = find_free(need);
        if (b == NULL) rt_fatal("heap pool added but no block of %llu bytes found", (unsigned long long)need);
    }
    remove_free(b);
    bs = tb_size(b);
    if (bs - need >= TB_MIN)
    {
        tblock *rest = (tblock *)((uint8_t *)b + need);
        tblock *after = tb_next(b);
        rest->prev_phys = b;
        rest->size = (bs - need) | TB_FREE;
        after->prev_phys = rest;
        b->size = need;
        insert_free(rest);
    }
    else
    {
        b->size = bs;
    }
    return H2G((uint8_t *)b + TB_HDR);
}

static void tlsf_free_locked(tblock *b)
{
    tblock *next = tb_next(b);
    if (b->size & TB_FREE) rt_fatal("double free of guest heap block 0x%08x", H2G((uint8_t *)b + TB_HDR));
    b->size |= TB_FREE;
    if (next->size & TB_FREE)
    {
        remove_free(next);
        b->size = (tb_size(b) + tb_size(next)) | TB_FREE;
        tb_next(b)->prev_phys = b;
    }
    if (b->prev_phys != NULL && (b->prev_phys->size & TB_FREE))
    {
        tblock *prev = b->prev_phys;
        remove_free(prev);
        prev->size = (tb_size(prev) + tb_size(b)) | TB_FREE;
        tb_next(prev)->prev_phys = prev;
        b = prev;
    }
    insert_free(b);
}

static uint64_t round_request(uint32_t size)
{
    uint64_t need = ((uint64_t)size + TB_HDR + 15) & ~UINT64_C(15);
    return need < TB_MIN ? TB_MIN : need;
}

uint32_t rt_heap_alloc(uint32_t size)
{
    uint32_t r;

    if (size >= LARGE_LIMIT)
    {
        uint64_t total = (uint64_t)size + 16;
        uint32_t base, g;
        if (total > UINT32_MAX - RT_GRANULE) return 0;
        base = va_alloc_committed((uint32_t)total);
        if (base == 0) return 0;
        lock_counted(&heap_lock);
        for (g = base / RT_GRANULE; g < base / RT_GRANULE + alloc_granules[base / RT_GRANULE]; g++) heap_granule_kind[g] = 2;
        unlock_counted(&heap_lock);
        wr32(base, LARGE_MAGIC);
        wr32(base + 4, size);
        return base + 16;
    }
    lock_counted(&heap_lock);
    r = tlsf_alloc_locked(round_request(size));
    unlock_counted(&heap_lock);
    return r;
}

uint32_t rt_heap_calloc(uint32_t count, uint32_t size)
{
    uint64_t total = (uint64_t)count * size;
    uint32_t p;
    if (total > UINT32_MAX) return 0;
    p = rt_heap_alloc((uint32_t)total);
    if (p != 0) memset(G2H(p), 0, (size_t)total);
    return p;
}

static int is_large(uint32_t ptr)
{
    uint32_t g;
    if (ptr < 16) return 0;
    g = (ptr - 16) / RT_GRANULE;
    return heap_granule_kind[g] == 2 && ((ptr - 16) & (RT_GRANULE - 1)) == 0 && rd32(ptr - 16) == LARGE_MAGIC;
}

static void check_heap_ptr(uint32_t ptr)
{
    uint32_t g = ptr / RT_GRANULE;
    if ((ptr & 15) != 0 || g >= NUM_GRANULES || heap_granule_kind[g] == 0)
    {
        rt_fatal("pointer 0x%08x was not allocated by the guest heap", ptr);
    }
}

uint32_t rt_heap_size(uint32_t ptr)
{
    uint32_t r;
    if (ptr == 0) return 0;
    check_heap_ptr(ptr);
    if (is_large(ptr)) return rd32(ptr - 12);
    lock_counted(&heap_lock);
    r = (uint32_t)(tb_size((tblock *)((uint8_t *)G2H(ptr) - TB_HDR)) - TB_HDR);
    unlock_counted(&heap_lock);
    return r;
}

void rt_heap_free(uint32_t ptr)
{
    if (ptr == 0) return;
    check_heap_ptr(ptr);
    if (is_large(ptr))
    {
        uint32_t base = ptr - 16, g, n;
        lock_counted(&heap_lock);
        n = alloc_granules[base / RT_GRANULE];
        for (g = base / RT_GRANULE; g < base / RT_GRANULE + n; g++) heap_granule_kind[g] = 0;
        unlock_counted(&heap_lock);
        wr32(base, 0);
        if (rt_va_release(base) != 0) rt_fatal("release of large heap block 0x%08x failed", ptr);
        return;
    }
    lock_counted(&heap_lock);
    tlsf_free_locked((tblock *)((uint8_t *)G2H(ptr) - TB_HDR));
    unlock_counted(&heap_lock);
}

uint32_t rt_heap_realloc(uint32_t ptr, uint32_t size)
{
    uint32_t old_size, np;

    if (ptr == 0) return rt_heap_alloc(size);
    if (size == 0) { rt_heap_free(ptr); return 0; }
    check_heap_ptr(ptr);
    if (!is_large(ptr) && size < LARGE_LIMIT)
    {
        uint64_t need = round_request(size);
        tblock *b;
        lock_counted(&heap_lock);
        b = (tblock *)((uint8_t *)G2H(ptr) - TB_HDR);
        if (tb_size(b) >= need)
        {
            unlock_counted(&heap_lock);
            return ptr;
        }
        {
            tblock *next = tb_next(b);
            if ((next->size & TB_FREE) && tb_size(b) + tb_size(next) >= need)
            {
                uint64_t total = tb_size(b) + tb_size(next);
                remove_free(next);
                if (total - need >= TB_MIN)
                {
                    tblock *rest = (tblock *)((uint8_t *)b + need);
                    rest->prev_phys = b;
                    rest->size = (total - need) | TB_FREE;
                    b->size = need;
                    tb_next(rest)->prev_phys = rest;
                    insert_free(rest);
                }
                else
                {
                    b->size = total;
                    tb_next(b)->prev_phys = b;
                }
                unlock_counted(&heap_lock);
                return ptr;
            }
        }
        unlock_counted(&heap_lock);
    }
    old_size = rt_heap_size(ptr);
    np = rt_heap_alloc(size);
    if (np == 0) return 0;
    memcpy(G2H(np), G2H(ptr), old_size < size ? old_size : size);
    rt_heap_free(ptr);
    return np;
}
