/*
 * KERNEL32 file mapping: CreateFileMappingW, MapViewOfFile and
 * UnmapViewOfFile.
 *
 * A file mapping object (OBJ_SECTION) holds a duplicate of the host file
 * descriptor of the file, or, for a mapping backed by the paging file
 * (INVALID_HANDLE_VALUE), host shared anonymous memory. A view is a host
 * mapping at the guest address:
 *   - a file view is mmap of the file (MAP_SHARED; MAP_PRIVATE for
 *     FILE_MAP_COPY), so views and file writes see the same data;
 *   - a view of shared memory is a mach_vm_remap of the shared pages (a
 *     copy for FILE_MAP_COPY).
 * A view stays valid after the mapping handle is closed, as on Windows.
 * Named mappings and image sections (SEC_IMAGE) are not supported.
 */
#include "rt_file.h"
#include "rt_object.h"
#include "rt_text.h"

#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define INVALID_HANDLE_VALUE 0xFFFFFFFFu

#define SEC_IMAGE 0x1000000u
#define SEC_RESERVE 0x4000000u
#define SEC_COMMIT 0x8000000u
#define SEC_NOCACHE 0x10000000u

#define FILE_MAP_COPY 0x1u
#define FILE_MAP_WRITE 0x2u
#define FILE_MAP_READ 0x4u
#define FILE_MAP_EXECUTE 0x20u

#define ERROR_FILE_INVALID 1006u
#define ERROR_MAPPED_ALIGNMENT 1132u

typedef struct section {
    int fd;           /* file mapping: host descriptor; -1 for shared memory */
    void *shared;     /* shared memory mapping: the host pages */
    size_t shared_len;
    uint64_t size;    /* the maximum size of the mapping */
    uint32_t protect; /* PAGE_READONLY, PAGE_READWRITE, PAGE_WRITECOPY, or the execute forms */
} section;

static void section_destroy(rt_object *o)
{
    section *s = o->data;
    if (s == NULL) return;
    if (s->fd >= 0 && close(s->fd) != 0) rt_log("file mapping: close failed: %s", strerror(errno));
    if (s->shared != NULL && munmap(s->shared, s->shared_len) != 0) rt_fatal("file mapping: munmap failed: %s", strerror(errno));
    free(s);
}

static bool protect_writes(uint32_t p) { return p == RT_PAGE_READWRITE || p == RT_PAGE_EXECUTE_READWRITE; }

/* HANDLE CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES sa,
   DWORD protect, DWORD size_high, DWORD size_low, LPCWSTR name) */
void rt_CreateFileMappingW(_cpu *cpu)
{
    uint32_t file = ARG(0), flags = ARG(2), name = ARG(5), protect = flags & 0xFFu, h;
    uint64_t size = ((uint64_t)ARG(3) << 32) | ARG(4);
    section *s;
    rt_object *o;

    if (name != 0) rt_fatal("CreateFileMapping: named file mappings are not supported");
    if (flags & (SEC_IMAGE | SEC_RESERVE)) rt_fatal("CreateFileMappingW: SEC_IMAGE and SEC_RESERVE are not supported (flags 0x%x)", flags);
    if ((flags & ~(0xFFu | SEC_COMMIT | SEC_NOCACHE)) != 0 ||
        (protect != RT_PAGE_READONLY && protect != RT_PAGE_READWRITE && protect != RT_PAGE_WRITECOPY &&
         protect != RT_PAGE_EXECUTE_READ && protect != RT_PAGE_EXECUTE_READWRITE))
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    s = calloc(1, sizeof(*s));
    if (s == NULL) rt_fatal("out of host memory (file mapping)");
    s->fd = -1;
    s->protect = protect;

    if (file == INVALID_HANDLE_VALUE)
    {
        uint32_t pg = rt_host_page_size();
        void *m;
        if (size == 0 || size > 0x80000000u)
        {
            free(s);
            rt_set_last_error(size == 0 ? ERROR_INVALID_PARAMETER : ERROR_NOT_ENOUGH_MEMORY);
            RET(0);
            return;
        }
        s->shared_len = (size_t)((size + pg - 1) / pg * pg);
        m = mmap(NULL, s->shared_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
        if (m == MAP_FAILED)
        {
            free(s);
            rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
            RET(0);
            return;
        }
        s->shared = m;
        s->size = size;
    }
    else
    {
        rt_object *fo = rt_handle_get(file, OBJ_FILE);
        int fd, mode;
        struct stat st;
        if (fo == NULL)
        {
            free(s);
            rt_set_last_error(ERROR_INVALID_HANDLE);
            RET(0);
            return;
        }
        fd = rt_file_object_fd(fo);
        rt_object_unref(fo);
        if (fd < 0)
        {
            /* A directory handle. */
            free(s);
            rt_set_last_error(ERROR_ACCESS_DENIED);
            RET(0);
            return;
        }
        mode = fcntl(fd, F_GETFL);
        if (mode < 0) rt_fatal("CreateFileMappingW: fcntl failed: %s", strerror(errno));
        mode &= O_ACCMODE;
        if (mode == O_WRONLY || (protect_writes(protect) && mode != O_RDWR))
        {
            free(s);
            rt_set_last_error(ERROR_ACCESS_DENIED);
            RET(0);
            return;
        }
        if (fstat(fd, &st) != 0) rt_fatal("CreateFileMappingW: fstat failed: %s", strerror(errno));
        if (size == 0)
        {
            if (st.st_size == 0)
            {
                free(s);
                rt_set_last_error(ERROR_FILE_INVALID);
                RET(0);
                return;
            }
            size = (uint64_t)st.st_size;
        }
        else if (size > (uint64_t)st.st_size)
        {
            /* Windows makes the file larger for a mapping with write
               access, and refuses a larger read-only mapping. */
            if (!protect_writes(protect))
            {
                free(s);
                rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
                RET(0);
                return;
            }
            if (ftruncate(fd, (off_t)size) != 0)
            {
                uint32_t e = rt_errno_to_win32(errno, ERROR_FILE_INVALID);
                free(s);
                rt_set_last_error(e);
                RET(0);
                return;
            }
        }
        s->fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
        if (s->fd < 0)
        {
            free(s);
            rt_set_last_error(ERROR_TOO_MANY_OPEN_FILES);
            RET(0);
            return;
        }
        s->size = size;
    }

    o = rt_object_new(OBJ_SECTION);
    o->data = s;
    o->destroy = section_destroy;
    h = rt_handle_new(o);
    rt_object_unref(o);
    if (h == 0)
    {
        rt_set_last_error(ERROR_TOO_MANY_OPEN_FILES);
        RET(0);
        return;
    }
    rt_set_last_error(ERROR_SUCCESS);
    RET(h);
}

/* HANDLE CreateFileMappingA(HANDLE, LPSECURITY_ATTRIBUTES, DWORD protect,
   DWORD size_high, DWORD size_low, LPCSTR name): the name is the only
   string, and the runtime has no named file mappings, so the two forms
   are the same. */
void rt_CreateFileMappingA(_cpu *cpu)
{
    rt_CreateFileMappingW(cpu);
}

typedef struct view_map {
    const section *s;
    uint64_t offset;
    bool copy, write;
} view_map;

static int map_view_host(void *host, size_t len, void *ctx)
{
    const view_map *v = ctx;
    int prot = PROT_READ | ((v->write || v->copy) ? PROT_WRITE : 0);
    if (v->s->fd >= 0)
    {
        void *m = mmap(host, len, prot, (v->copy ? MAP_PRIVATE : MAP_SHARED) | MAP_FIXED, v->s->fd, (off_t)v->offset);
        if (m == MAP_FAILED) return errno;
        if (m != host) rt_fatal("MapViewOfFile: mmap ignored MAP_FIXED");
        return 0;
    }
    {
        mach_vm_address_t target = (mach_vm_address_t)(uintptr_t)host;
        vm_prot_t cur, max;
        kern_return_t kr;
        if (v->offset + len > v->s->shared_len) rt_fatal("MapViewOfFile: view outside the shared memory");
        kr = mach_vm_remap(mach_task_self(), &target, len, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                           (mach_vm_address_t)(uintptr_t)v->s->shared + v->offset, v->copy ? TRUE : FALSE, &cur, &max,
                           VM_INHERIT_NONE);
        if (kr != KERN_SUCCESS) return ENOMEM;
        if (target != (mach_vm_address_t)(uintptr_t)host) rt_fatal("MapViewOfFile: mach_vm_remap moved the view");
        if (mprotect(host, len, prot) != 0) rt_fatal("MapViewOfFile: mprotect failed: %s", strerror(errno));
        return 0;
    }
}

/* LPVOID MapViewOfFile(HANDLE mapping, DWORD access, DWORD offset_high,
   DWORD offset_low, SIZE_T bytes) */
void rt_MapViewOfFile(_cpu *cpu)
{
    uint32_t access = ARG(1), bytes = ARG(4), base, page_prot;
    uint64_t offset = ((uint64_t)ARG(2) << 32) | ARG(3), len;
    rt_object *o = rt_handle_get(ARG(0), OBJ_SECTION);
    section *s;
    view_map v;
    int err;

    if (o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    s = o->data;
    if ((offset & (RT_GRANULE - 1)) != 0)
    {
        rt_object_unref(o);
        rt_set_last_error(ERROR_MAPPED_ALIGNMENT);
        RET(0);
        return;
    }
    if (access & ~(FILE_MAP_COPY | FILE_MAP_WRITE | FILE_MAP_READ | FILE_MAP_EXECUTE | 0xF0000u | 0x18u))
    {
        rt_object_unref(o);
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    v.s = s;
    v.offset = offset;
    v.copy = access == FILE_MAP_COPY;
    v.write = !v.copy && (access & FILE_MAP_WRITE) != 0;
    if (v.write && !protect_writes(s->protect))
    {
        rt_object_unref(o);
        rt_set_last_error(ERROR_ACCESS_DENIED);
        RET(0);
        return;
    }
    if (offset >= s->size || (bytes != 0 && bytes > s->size - offset))
    {
        rt_object_unref(o);
        rt_set_last_error(ERROR_ACCESS_DENIED);
        RET(0);
        return;
    }
    len = bytes != 0 ? bytes : s->size - offset;
    if (len > 0x7FFF0000u)
    {
        rt_object_unref(o);
        rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        RET(0);
        return;
    }
    {
        bool x = (access & FILE_MAP_EXECUTE) != 0;
        page_prot = v.copy ? (x ? RT_PAGE_EXECUTE_WRITECOPY : RT_PAGE_WRITECOPY)
                  : v.write ? (x ? RT_PAGE_EXECUTE_READWRITE : RT_PAGE_READWRITE)
                            : (x ? RT_PAGE_EXECUTE_READ : RT_PAGE_READONLY);
    }
    base = rt_va_map_view((uint32_t)len, page_prot, map_view_host, &v, &err);
    rt_object_unref(o);
    if (base == 0)
    {
        rt_set_last_error(err == 0 ? ERROR_NOT_ENOUGH_MEMORY : rt_errno_to_win32(err, ERROR_ACCESS_DENIED));
        RET(0);
        return;
    }
    RET(base);
}

/* BOOL UnmapViewOfFile(LPCVOID base) */
void rt_UnmapViewOfFile(_cpu *cpu)
{
    if (rt_va_unmap_view(ARG(0)) != 0)
    {
        rt_set_last_error(487u); /* ERROR_INVALID_ADDRESS */
        RET(0);
        return;
    }
    RET(1);
}
