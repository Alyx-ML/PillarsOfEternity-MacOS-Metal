/*
 * KERNEL32 heaps (HeapCreate, HeapAlloc and the other Heap functions),
 * GlobalAlloc, GlobalReAlloc, GlobalFree and LocalFree, and the OLE32 task
 * allocator (CoTaskMemAlloc and related functions).
 *
 * Heap functions: each block of a heap (the process heap or a heap from
 * HeapCreate) is a guest heap block with a 32-byte header in front of the
 * payload:
 *   +0  magic      BLOCK_MAGIC
 *   +4  heap       the heap handle
 *   +8  size       the requested size (HeapSize)
 *   +12 prev       previous block of the heap (payload address), 0 if none
 *   +16 next       next block of the heap (payload address), 0 if none
 * The block list lets HeapDestroy release all blocks of a heap.
 *
 * GlobalAlloc (GMEM_FIXED), LocalFree and the task allocator use guest heap
 * blocks without a header.
 *
 * Movable global memory (GMEM_MOVEABLE): the handle is the guest address
 * of a 4-byte slot that holds the address of the data, as on Windows. The
 * slot is at offset 8 of a 16-byte guest block, so a movable handle is
 * never a fixed block (fixed blocks are 16-byte aligned). GlobalLock gives
 * the data address and counts the locks; the data does not move while the
 * handle has locks. The runtime gives such blocks to the guest for
 * LocalFree (FormatMessage, security descriptors) and CoTaskMemFree.
 */
#include "rt.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_MAGIC 0x4B425048u /* "HPBK" */
#define HDR 32u

#define HEAP_NO_SERIALIZE 0x1u
#define HEAP_GENERATE_EXCEPTIONS 0x4u
#define HEAP_ZERO_MEMORY 0x8u
#define HEAP_REALLOC_IN_PLACE_ONLY 0x10u
#define HEAP_CREATE_ENABLE_EXECUTE 0x40000u

#define MAX_HEAPS 256u
/* The largest block of a heap with a maximum size (Windows: 0x7FFF8). */
#define FIXED_HEAP_BLOCK_MAX 0x7FFF8u

typedef struct heap {
    uint32_t handle;      /* guest address of the heap handle block; 0 if the slot is free */
    uint32_t first;       /* first block (payload address), 0 if none */
    uint32_t maximum;     /* 0: the heap can grow */
    uint64_t used;        /* requested bytes of all blocks */
    bool generate_exceptions;
    pthread_mutex_t lock;
} heap;

static pthread_mutex_t heaps_lock = PTHREAD_MUTEX_INITIALIZER;
static heap heaps[MAX_HEAPS];

static void heap_lock(heap *h)
{
    int r = pthread_mutex_lock(&h->lock);
    if (r != 0) rt_fatal("heap lock failed (%d)", r);
}

static void heap_unlock(heap *h)
{
    int r = pthread_mutex_unlock(&h->lock);
    if (r != 0) rt_fatal("heap unlock failed (%d)", r);
}

/* Add a heap with the given handle. heaps_lock held. */
static heap *heap_add_locked(uint32_t handle, uint32_t maximum, bool gen)
{
    uint32_t i;
    for (i = 0; i < MAX_HEAPS; i++)
    {
        if (heaps[i].handle == 0)
        {
            int r = pthread_mutex_init(&heaps[i].lock, NULL);
            if (r != 0) rt_fatal("heap lock creation failed (%d)", r);
            heaps[i].handle = handle;
            heaps[i].first = 0;
            heaps[i].maximum = maximum;
            heaps[i].used = 0;
            heaps[i].generate_exceptions = gen;
            return &heaps[i];
        }
    }
    return NULL;
}

/* The heap of handle h. The process heap is added on first use. A handle
   that is not a heap stops the process (on Windows it corrupts memory). */
static heap *heap_of(uint32_t h, const char *fn)
{
    uint32_t i;
    heap *r = NULL;
    if (h == 0) rt_fatal("%s: NULL heap handle", fn);
    if (pthread_mutex_lock(&heaps_lock) != 0) rt_fatal("heaps_lock lock failed");
    for (i = 0; i < MAX_HEAPS; i++)
        if (heaps[i].handle == h) r = &heaps[i];
    if (r == NULL && h == rt_process_heap())
    {
        r = heap_add_locked(h, 0, false);
        if (r == NULL) rt_fatal("%s: no heap slot for the process heap", fn);
    }
    if (pthread_mutex_unlock(&heaps_lock) != 0) rt_fatal("heaps_lock unlock failed");
    if (r == NULL) rt_fatal("%s: 0x%08x is not a heap handle", fn, h);
    return r;
}

/* Check that p is a block of heap h. Heap lock held. */
static void check_block(const heap *h, uint32_t p, const char *fn)
{
    if (p < HDR || (p & 15u) != 0 || !rt_va_is_committed(p - HDR, HDR) || rd32(p - HDR) != BLOCK_MAGIC)
        rt_fatal("%s: 0x%08x is not a heap block", fn, p);
    if (rd32(p - HDR + 4) != h->handle)
        rt_fatal("%s: block 0x%08x belongs to heap 0x%08x, not to heap 0x%08x", fn, p, rd32(p - HDR + 4), h->handle);
}

static void link_block(heap *h, uint32_t p)
{
    wr32(p - HDR + 12, 0);
    wr32(p - HDR + 16, h->first);
    if (h->first != 0) wr32(h->first - HDR + 12, p);
    h->first = p;
}

static void unlink_block(heap *h, uint32_t p)
{
    uint32_t prev = rd32(p - HDR + 12), next = rd32(p - HDR + 16);
    if (prev != 0) wr32(prev - HDR + 16, next); else h->first = next;
    if (next != 0) wr32(next - HDR + 12, prev);
}

/* Can the heap take size more bytes? Heap lock held. */
static bool heap_room(const heap *h, uint64_t more, uint32_t size)
{
    if (h->maximum == 0) return true;
    return size <= FIXED_HEAP_BLOCK_MAX && h->used + more <= h->maximum;
}

/* A failed allocation of a heap with HEAP_GENERATE_EXCEPTIONS raises
   STATUS_NO_MEMORY on Windows. The runtime cannot raise an exception from
   inside a heap function, so it stops the process with a message. */
static void alloc_failed(const heap *h, uint32_t flags, uint32_t size, const char *fn)
{
    if (h->generate_exceptions || (flags & HEAP_GENERATE_EXCEPTIONS))
        rt_fatal("%s: allocation of %u bytes failed (HEAP_GENERATE_EXCEPTIONS: STATUS_NO_MEMORY)", fn, size);
}

/* HANDLE HeapCreate(DWORD options, SIZE_T initial, SIZE_T maximum) */
void rt_HeapCreate(_cpu *cpu)
{
    uint32_t options = ARG(0), initial = ARG(1), maximum = ARG(2), handle;
    heap *h;
    if (options & ~(HEAP_NO_SERIALIZE | HEAP_GENERATE_EXCEPTIONS | HEAP_CREATE_ENABLE_EXECUTE))
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (maximum != 0 && initial > maximum)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    /* The handle is the address of a guest block, as on Windows. */
    handle = rt_heap_calloc(1, 64);
    if (handle == 0)
    {
        rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        RET(0);
        return;
    }
    if (pthread_mutex_lock(&heaps_lock) != 0) rt_fatal("heaps_lock lock failed");
    h = heap_add_locked(handle, maximum, (options & HEAP_GENERATE_EXCEPTIONS) != 0);
    if (pthread_mutex_unlock(&heaps_lock) != 0) rt_fatal("heaps_lock unlock failed");
    if (h == NULL)
    {
        rt_heap_free(handle);
        rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        RET(0);
        return;
    }
    RET(handle);
}

/* BOOL HeapDestroy(HANDLE heap): releases all blocks of the heap. */
void rt_HeapDestroy(_cpu *cpu)
{
    uint32_t handle = ARG(0), p;
    heap *h;
    if (handle == rt_process_heap()) rt_fatal("HeapDestroy of the process heap");
    h = heap_of(handle, "HeapDestroy");
    heap_lock(h);
    p = h->first;
    while (p != 0)
    {
        uint32_t next = rd32(p - HDR + 16);
        wr32(p - HDR, 0);
        rt_heap_free(p - HDR);
        p = next;
    }
    h->first = 0;
    heap_unlock(h);
    if (pthread_mutex_lock(&heaps_lock) != 0) rt_fatal("heaps_lock lock failed");
    {
        int r = pthread_mutex_destroy(&h->lock);
        if (r != 0) rt_fatal("heap lock destruction failed (%d)", r);
    }
    h->handle = 0;
    if (pthread_mutex_unlock(&heaps_lock) != 0) rt_fatal("heaps_lock unlock failed");
    rt_heap_free(handle);
    RET(1);
}

/* LPVOID HeapAlloc(HANDLE heap, DWORD flags, SIZE_T size) */
void rt_HeapAlloc(_cpu *cpu)
{
    uint32_t flags = ARG(1), size = ARG(2), b;
    heap *h = heap_of(ARG(0), "HeapAlloc");
    heap_lock(h);
    if (!heap_room(h, size, size) || size > 0xFFFFFFFFu - HDR - RT_GRANULE)
    {
        heap_unlock(h);
        alloc_failed(h, flags, size, "HeapAlloc");
        RET(0);
        return;
    }
    b = rt_heap_alloc(size + HDR);
    if (b == 0)
    {
        heap_unlock(h);
        alloc_failed(h, flags, size, "HeapAlloc");
        RET(0);
        return;
    }
    wr32(b + 0, BLOCK_MAGIC);
    wr32(b + 4, h->handle);
    wr32(b + 8, size);
    wr32(b + 20, 0);
    wr32(b + 24, 0);
    wr32(b + 28, 0);
    link_block(h, b + HDR);
    h->used += size;
    heap_unlock(h);
    if (flags & HEAP_ZERO_MEMORY) memset(G2H(b + HDR), 0, size);
    RET(b + HDR);
}

/* BOOL HeapFree(HANDLE heap, DWORD flags, LPVOID mem) */
void rt_HeapFree(_cpu *cpu)
{
    uint32_t p = ARG(2);
    heap *h = heap_of(ARG(0), "HeapFree");
    if (p == 0)
    {
        RET(1);
        return;
    }
    heap_lock(h);
    check_block(h, p, "HeapFree");
    unlink_block(h, p);
    h->used -= rd32(p - HDR + 8);
    wr32(p - HDR, 0);
    heap_unlock(h);
    rt_heap_free(p - HDR);
    RET(1);
}

/* SIZE_T HeapSize(HANDLE heap, DWORD flags, LPCVOID mem): the requested
   size of the block. */
void rt_HeapSize(_cpu *cpu)
{
    uint32_t p = ARG(2), size;
    heap *h = heap_of(ARG(0), "HeapSize");
    heap_lock(h);
    check_block(h, p, "HeapSize");
    size = rd32(p - HDR + 8);
    heap_unlock(h);
    RET(size);
}

/* LPVOID HeapReAlloc(HANDLE heap, DWORD flags, LPVOID mem, SIZE_T size) */
void rt_HeapReAlloc(_cpu *cpu)
{
    uint32_t flags = ARG(1), p = ARG(2), size = ARG(3), old, b, np;
    heap *h = heap_of(ARG(0), "HeapReAlloc");
    if (p == 0)
    {
        /* Windows fails without setting an error code. */
        RET(0);
        return;
    }
    heap_lock(h);
    check_block(h, p, "HeapReAlloc");
    old = rd32(p - HDR + 8);
    if ((size > old && !heap_room(h, (uint64_t)size - old, size)) || size > 0xFFFFFFFFu - HDR - RT_GRANULE)
    {
        heap_unlock(h);
        alloc_failed(h, flags, size, "HeapReAlloc");
        RET(0);
        return;
    }
    if (flags & HEAP_REALLOC_IN_PLACE_ONLY)
    {
        if (rt_heap_size(p - HDR) - HDR < size)
        {
            heap_unlock(h);
            alloc_failed(h, flags, size, "HeapReAlloc");
            RET(0);
            return;
        }
        np = p;
    }
    else
    {
        /* Take the block out of the list: rt_heap_realloc can move it. */
        unlink_block(h, p);
        b = rt_heap_realloc(p - HDR, size + HDR);
        if (b == 0)
        {
            link_block(h, p);
            heap_unlock(h);
            alloc_failed(h, flags, size, "HeapReAlloc");
            RET(0);
            return;
        }
        np = b + HDR;
        link_block(h, np);
    }
    wr32(np - HDR + 8, size);
    h->used = h->used - old + size;
    heap_unlock(h);
    if ((flags & HEAP_ZERO_MEMORY) && size > old) memset(G2H(np + old), 0, size - old);
    RET(np);
}

/* ---------------------------------------------------------------- Global and Local */

#define GMEM_MOVEABLE 0x2u
#define GMEM_ZEROINIT 0x40u
#define GMEM_MODIFY 0x80u
#define GMEM_VALID_FLAGS 0x7F72u

#define GMEM_LOCKCOUNT 0x00FFu
#define GMEM_DISCARDED 0x4000u
/* The most movable handles at one time (Windows: 65535 for each process). */
#define MAX_MOVABLE 65535u

typedef struct movable {
    uint32_t handle; /* 0: the slot is free */
    uint32_t size;   /* the requested size */
    uint32_t locks;
} movable;

static pthread_mutex_t movable_lock = PTHREAD_MUTEX_INITIALIZER;
static movable *movables;
static uint32_t movable_count, movable_cap;

static void mv_lock(void)
{
    if (pthread_mutex_lock(&movable_lock) != 0) rt_fatal("movable memory lock failed");
}

static void mv_unlock(void)
{
    if (pthread_mutex_unlock(&movable_lock) != 0) rt_fatal("movable memory unlock failed");
}

/* The entry of movable handle h, or NULL (movable_lock held). */
static movable *mv_find(uint32_t h)
{
    uint32_t i;
    if ((h & 15u) != 8u) return NULL;
    for (i = 0; i < movable_count; i++)
        if (movables[i].handle == h) return &movables[i];
    return NULL;
}

static uint32_t mv_data(const movable *m)
{
    return rd32(m->handle);
}

/* A new movable block. Returns the handle, or 0 with the last error set. */
static uint32_t mv_alloc(uint32_t size, bool zero)
{
    uint32_t e, d = 0, i;
    movable *m = NULL;
    e = rt_heap_alloc(16);
    if (e == 0)
    {
        rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    /* A block of size 0 is a discarded block: it has no data. */
    if (size != 0)
    {
        d = rt_heap_alloc(size);
        if (d == 0)
        {
            rt_heap_free(e);
            rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        if (zero) memset(G2H(d), 0, rt_heap_size(d));
    }
    memset(G2H(e), 0, 16);
    wr32(e + 8, d);
    mv_lock();
    for (i = 0; i < movable_count; i++)
        if (movables[i].handle == 0)
        {
            m = &movables[i];
            break;
        }
    if (m == NULL)
    {
        if (movable_count == MAX_MOVABLE)
        {
            mv_unlock();
            if (d != 0) rt_heap_free(d);
            rt_heap_free(e);
            rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        if (movable_count == movable_cap)
        {
            uint32_t ncap = movable_cap ? 2u * movable_cap : 64u;
            movable *n = realloc(movables, ncap * sizeof *n);
            if (n == NULL) rt_fatal("out of host memory (movable memory table)");
            movables = n;
            movable_cap = ncap;
        }
        m = &movables[movable_count++];
    }
    m->handle = e + 8;
    m->size = size;
    m->locks = 0;
    mv_unlock();
    return e + 8;
}

uint32_t rt_global_alloc_movable(uint32_t size, bool zero)
{
    return mv_alloc(size, zero);
}

uint32_t rt_global_lock(uint32_t h)
{
    movable *m;
    uint32_t d;
    if (h == 0) return 0;
    mv_lock();
    m = mv_find(h);
    if (m == NULL)
    {
        mv_unlock();
        return h; /* fixed memory: the handle is the address */
    }
    d = mv_data(m);
    if (d != 0 && m->locks < GMEM_LOCKCOUNT) m->locks++;
    mv_unlock();
    return d;
}

uint32_t rt_global_size(uint32_t h)
{
    movable *m;
    uint32_t n;
    if (h == 0) return 0;
    mv_lock();
    m = mv_find(h);
    n = m != NULL ? m->size : 0;
    mv_unlock();
    return m != NULL ? n : rt_heap_size(h);
}

/* Unlock movable handle h. Returns the new lock count, or -1 if h is not a
   movable handle. */
static int mv_unlock_handle(uint32_t h)
{
    movable *m;
    int r;
    mv_lock();
    m = mv_find(h);
    if (m == NULL)
    {
        mv_unlock();
        return -1;
    }
    if (m->locks == 0)
        r = -2;
    else
        r = (int)--m->locks;
    mv_unlock();
    return r;
}

void rt_global_unlock(uint32_t h)
{
    (void)mv_unlock_handle(h);
}

/* Free movable handle h. Returns false if h is not a movable handle. */
static bool mv_free(uint32_t h)
{
    movable *m;
    uint32_t d;
    mv_lock();
    m = mv_find(h);
    if (m == NULL)
    {
        mv_unlock();
        return false;
    }
    d = mv_data(m);
    m->handle = 0;
    mv_unlock();
    if (d != 0) rt_heap_free(d);
    rt_heap_free(h - 8);
    return true;
}

void rt_global_free(uint32_t h)
{
    if (h == 0) return;
    if (!mv_free(h)) rt_heap_free(h);
}

/* HGLOBAL GlobalAlloc(UINT flags, SIZE_T size). With GMEM_ZEROINIT the
   whole usable size of the block is zero, so a later GlobalReAlloc with
   GMEM_ZEROINIT gives zero bytes after the old size. */
void rt_GlobalAlloc(_cpu *cpu)
{
    uint32_t flags = ARG(0), size = ARG(1), p;
    if (flags & ~GMEM_VALID_FLAGS)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (flags & GMEM_MOVEABLE)
    {
        RET(mv_alloc(size, (flags & GMEM_ZEROINIT) != 0));
        return;
    }
    p = rt_heap_alloc(size);
    if (p == 0)
    {
        rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        RET(0);
        return;
    }
    if (flags & GMEM_ZEROINIT) memset(G2H(p), 0, rt_heap_size(p));
    RET(p);
}

/* HGLOBAL GlobalReAlloc(HGLOBAL mem, SIZE_T size, UINT flags). Without
   GMEM_MOVEABLE a fixed block can only change in place. */
void rt_GlobalReAlloc(_cpu *cpu)
{
    uint32_t p = ARG(0), size = ARG(1), flags = ARG(2), old, np;
    if (p == 0)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    if (flags & GMEM_MODIFY) rt_fatal("GlobalReAlloc: GMEM_MODIFY is not supported");
    {
        movable *m;
        uint32_t d, nd;
        mv_lock();
        m = mv_find(p);
        if (m != NULL)
        {
            /* A locked block can change only in place, as on Windows
               without GMEM_MOVEABLE in flags. */
            d = mv_data(m);
            old = d != 0 ? rt_heap_size(d) : 0;
            if (m->locks != 0 && !(flags & GMEM_MOVEABLE) && size > old)
            {
                mv_unlock();
                rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
                RET(0);
                return;
            }
            if (size == 0)
            {
                if (m->locks != 0)
                {
                    mv_unlock();
                    rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
                    RET(0);
                    return;
                }
                wr32(p, 0);
                m->size = 0;
                mv_unlock();
                if (d != 0) rt_heap_free(d);
                RET(p);
                return;
            }
            nd = d != 0 ? rt_heap_realloc(d, size) : rt_heap_alloc(size);
            if (nd == 0)
            {
                mv_unlock();
                rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
                RET(0);
                return;
            }
            if (flags & GMEM_ZEROINIT)
            {
                uint32_t now = rt_heap_size(nd);
                if (now > old) memset(G2H(nd + old), 0, now - old);
            }
            wr32(p, nd);
            m->size = size;
            mv_unlock();
            RET(p);
            return;
        }
        mv_unlock();
    }
    old = rt_heap_size(p);
    if (!(flags & GMEM_MOVEABLE))
    {
        if (size > old)
        {
            rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
            RET(0);
            return;
        }
        RET(p);
        return;
    }
    np = rt_heap_realloc(p, size == 0 ? 1u : size);
    if (np == 0)
    {
        rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        RET(0);
        return;
    }
    if (flags & GMEM_ZEROINIT)
    {
        uint32_t now = rt_heap_size(np);
        if (now > old) memset(G2H(np + old), 0, now - old);
    }
    RET(np);
}

/* HGLOBAL GlobalFree(HGLOBAL) and HLOCAL LocalFree(HLOCAL): NULL on
   success. */
void rt_GlobalFree(_cpu *cpu)
{
    rt_global_free(ARG(0));
    RET(0);
}

/* LPVOID GlobalLock(HGLOBAL): the data address. NULL for a discarded block
   (ERROR_DISCARDED) or a handle that is not valid. */
void rt_GlobalLock(_cpu *cpu)
{
    uint32_t h = ARG(0), d;
    if (h == 0)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    d = rt_global_lock(h);
    if (d == 0) rt_set_last_error(157u); /* ERROR_DISCARDED */
    RET(d);
}

/* BOOL GlobalUnlock(HGLOBAL): nonzero while the block has locks. When the
   count becomes 0 the result is 0 with the last error NO_ERROR; a block
   without locks gives 0 with ERROR_NOT_LOCKED. Fixed memory gives 1. */
void rt_GlobalUnlock(_cpu *cpu)
{
    int r = mv_unlock_handle(ARG(0));
    if (r == -1)
    {
        RET(1);
        return;
    }
    if (r == -2)
    {
        rt_set_last_error(158u); /* ERROR_NOT_LOCKED */
        RET(0);
        return;
    }
    if (r == 0) rt_set_last_error(ERROR_SUCCESS);
    RET((uint32_t)r);
}

/* ---------------------------------------------------------------- heap validation and walk */

#define PROCESS_HEAP_ENTRY_BUSY 0x0004u

/* BOOL HeapValidate(HANDLE, DWORD flags, LPCVOID mem): mem NULL checks all
   blocks of the heap (the block headers and the block list). */
void rt_HeapValidate(_cpu *cpu)
{
    uint32_t p = ARG(2), ok = 1;
    heap *h = heap_of(ARG(0), "HeapValidate");
    heap_lock(h);
    if (p != 0)
    {
        if (p < HDR || (p & 15u) != 0 || !rt_va_is_committed(p - HDR, HDR) || rd32(p - HDR) != BLOCK_MAGIC ||
            rd32(p - HDR + 4) != h->handle)
            ok = 0;
    }
    else
    {
        uint32_t q = h->first, prev = 0, n = 0;
        uint64_t used = 0;
        while (q != 0)
        {
            if (q < HDR || (q & 15u) != 0 || !rt_va_is_committed(q - HDR, HDR) || rd32(q - HDR) != BLOCK_MAGIC ||
                rd32(q - HDR + 4) != h->handle || rd32(q - HDR + 12) != prev || ++n == 0)
            {
                ok = 0;
                break;
            }
            used += rd32(q - HDR + 8);
            prev = q;
            q = rd32(q - HDR + 16);
        }
        if (ok && used != h->used) ok = 0;
    }
    heap_unlock(h);
    RET(ok);
}

/* BOOL HeapWalk(HANDLE, LPPROCESS_HEAP_ENTRY). The blocks of a runtime
   heap are not in one region, so the walk gives only the allocated blocks
   (PROCESS_HEAP_ENTRY_BUSY), in the order of the block list. lpData NULL
   starts the walk; after the last block the result is FALSE with
   ERROR_NO_MORE_ITEMS. PROCESS_HEAP_ENTRY (28 bytes): lpData, cbData,
   cbOverhead (byte), iRegionIndex (byte), wFlags, then 16 bytes. */
void rt_HeapWalk(_cpu *cpu)
{
    uint32_t e = ARG(1), cur, next;
    heap *h = heap_of(ARG(0), "HeapWalk");
    if (e == 0 || !rt_va_is_committed(e, 28))
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    heap_lock(h);
    cur = rd32(e);
    if (cur == 0)
        next = h->first;
    else
    {
        check_block(h, cur, "HeapWalk");
        next = rd32(cur - HDR + 16);
    }
    if (next == 0)
    {
        heap_unlock(h);
        rt_set_last_error(ERROR_NO_MORE_ITEMS);
        RET(0);
        return;
    }
    wr32(e, next);
    wr32(e + 4, rd32(next - HDR + 8));
    wr8(e + 8, (uint8_t)HDR);
    wr8(e + 9, 0);
    wr16(e + 10, PROCESS_HEAP_ENTRY_BUSY);
    memset(G2H(e + 12), 0, 16);
    heap_unlock(h);
    RET(1);
}

/* BOOL HeapSetInformation(HANDLE, HEAP_INFORMATION_CLASS, PVOID, SIZE_T).
   HeapCompatibilityInformation (0) with the value 2 (the low-fragmentation
   heap) and HeapEnableTerminationOnCorruption (1) succeed: the runtime
   heap has no other modes, and a heap error always stops the process. */
void rt_HeapSetInformation(_cpu *cpu)
{
    uint32_t cls = ARG(1), info = ARG(2), len = ARG(3);
    if (cls == 1)
    {
        if (ARG(0) != 0 || info != 0 || len != 0)
        {
            rt_set_last_error(ERROR_INVALID_PARAMETER);
            RET(0);
            return;
        }
        RET(1);
        return;
    }
    if (cls == 0)
    {
        if (info == 0 || len != 4 || ARG(0) == 0)
        {
            rt_set_last_error(ERROR_INVALID_PARAMETER);
            RET(0);
            return;
        }
        (void)heap_of(ARG(0), "HeapSetInformation");
        if (rd32(info) != 2u)
        {
            rt_set_last_error(ERROR_INVALID_PARAMETER);
            RET(0);
            return;
        }
        RET(1);
        return;
    }
    rt_set_last_error(ERROR_INVALID_PARAMETER);
    RET(0);
}

void rt_LocalFree(_cpu *cpu)
{
    rt_heap_free(ARG(0));
    RET(0);
}

/* ---------------------------------------------------------------- task allocator */

void rt_CoTaskMemAlloc(_cpu *cpu)
{
    RET(rt_heap_alloc(ARG(0)));
}

void rt_CoTaskMemFree(_cpu *cpu)
{
    rt_heap_free(ARG(0));
}

/* LPVOID CoTaskMemRealloc(LPVOID p, SIZE_T size): size 0 frees the block
   and returns NULL. */
void rt_CoTaskMemRealloc(_cpu *cpu)
{
    RET(rt_heap_realloc(ARG(0), ARG(1)));
}
