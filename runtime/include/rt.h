/*
 * Core declarations of the Win32 runtime for the translated game code.
 *
 * Guest addresses are 32-bit offsets from rt_ptrofs (the llasm
 * "pointer offset"). All guest-visible memory (the program image, the
 * import thunks, guest stacks and the guest heap) is in the 2 GiB window
 * [rt_ptrofs, rt_ptrofs + RT_WINDOW_SIZE). Guest address 0 is NULL.
 */
#ifndef RT_H
#define RT_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "llasm_cpu.h"

#define RT_WINDOW_SIZE UINT64_C(0x80000000)
#define RT_GRANULE 0x10000u /* Windows allocation granularity */

extern uint64_t rt_ptrofs;

/* Print a message and stop the process. Used for every condition that the
   runtime does not support; there is no fallback behavior. */
_Noreturn void rt_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Guest address to host pointer. Guest address 0 gives NULL. */
static inline void *G2H(uint32_t g)
{
    return g ? (void *)(uintptr_t)(rt_ptrofs + g) : NULL;
}

/* Host pointer to guest address. NULL gives 0. A pointer outside the
   window is a runtime defect and stops the process. */
uint32_t rt_h2g_checked(const void *p);
static inline uint32_t H2G(const void *p)
{
    uintptr_t v = (uintptr_t)p;
    if (p == NULL) return 0;
    if (v <= rt_ptrofs || v - rt_ptrofs >= RT_WINDOW_SIZE) return rt_h2g_checked(p);
    return (uint32_t)(v - rt_ptrofs);
}

static inline uint32_t rd32(uint32_t g) { uint32_t v; memcpy(&v, G2H(g), 4); return v; }
static inline uint16_t rd16(uint32_t g) { uint16_t v; memcpy(&v, G2H(g), 2); return v; }
static inline uint8_t rd8(uint32_t g) { return *(uint8_t *)G2H(g); }
static inline uint64_t rd64(uint32_t g) { uint64_t v; memcpy(&v, G2H(g), 8); return v; }
static inline double rdf64(uint32_t g) { double v; memcpy(&v, G2H(g), 8); return v; }
static inline void wr32(uint32_t g, uint32_t v) { memcpy(G2H(g), &v, 4); }
static inline void wr16(uint32_t g, uint16_t v) { memcpy(G2H(g), &v, 2); }
static inline void wr8(uint32_t g, uint8_t v) { *(uint8_t *)G2H(g) = v; }
static inline void wr64(uint32_t g, uint64_t v) { memcpy(G2H(g), &v, 8); }

/* Argument n (0-based) of an import call: [esp + 4 + 4*n]. */
#define ARG(n) rd32(esp + 4u + 4u * (uint32_t)(n))
#define ARGP(n) G2H(ARG(n))
/* Address of argument n on the guest stack (for variable arguments). */
#define ARGADR(n) (esp + 4u + 4u * (uint32_t)(n))

/* Set the 32-bit result. */
#define RET(v) do { eax = (uint32_t)(v); } while (0)
/* Set a 64-bit result (edx:eax). */
#define RET64(v) do { uint64_t rv_ = (uint64_t)(v); eax = (uint32_t)rv_; edx = (uint32_t)(rv_ >> 32); } while (0)

/* The x87 register stack of the guest. A function result of type double
   is in st(0). */
static inline void rt_fpu_push(_cpu *cpu, double v)
{
    cpu->_st_top = (cpu->_st_top + 7u) & 7u;
    cpu->_st[cpu->_st_top & 7u] = v;
}
static inline double rt_fpu_pop(_cpu *cpu)
{
    double v = cpu->_st[cpu->_st_top & 7u];
    cpu->_st_top = (cpu->_st_top + 1u) & 7u;
    return v;
}
static inline double rt_fpu_st(_cpu *cpu, unsigned i)
{
    return cpu->_st[(cpu->_st_top + i) & 7u];
}
#define RETF(v) rt_fpu_push(cpu, (double)(v))

/* A double argument at argument dword n. */
#define ARGF(n) rdf64(esp + 4u + 4u * (uint32_t)(n))

/* ---------------------------------------------------------------- memory */

void rt_mem_init(void);
/* Place each address space allocation without a requested address (and
   each heap pool) only in the __GUEST segments of the program image. The
   loader reserves these segments with the image, so their guest addresses
   are the same in each run; the free gaps of the window around host
   mappings are not. Then the guest addresses of the allocations depend
   only on the order of the allocations. The AOT compiler host uses this
   mode, because the compiler writes some heap addresses into its output.
   An allocation that does not fit in the free __GUEST space fails (as
   VirtualAlloc without memory). Call it after rt_mem_init and before the
   first allocation. */
void rt_mem_use_guest_space_only(void);
/* The file of the D3D9 pipeline cache (rt_d3d9_draw.m); before the first
   CreateDevice. */
void rt_d3d9_pipeline_cache_init(const char *path);
/* Write the back buffer of a presented frame to dir/NNNNNN.png, at most
   one frame each interval_s seconds (player host, RT_CAPTURE). Call it
   before the program starts. */
void rt_d3d9_capture_init(const char *dir, unsigned interval_s);
/* Address space (Windows VirtualAlloc semantics, 64 KiB granularity). */
uint32_t rt_va_reserve(uint32_t addr, uint32_t size);           /* returns base or 0 */
/* The highest free range below 0x80000000 (MEM_TOP_DOWN); base or 0. */
uint32_t rt_va_reserve_top(uint32_t size);
int rt_va_commit(uint32_t addr, uint32_t size);                 /* 0 on success */
int rt_va_decommit(uint32_t addr, uint32_t size);
int rt_va_release(uint32_t base);
/* Committed and readable guest range check. The program image counts as
   committed. */
int rt_va_is_committed(uint32_t addr, uint32_t size);
uint64_t rt_va_free_bytes(void);

/* Windows memory states, types and protections (VirtualQuery). */
#define RT_MEM_COMMIT 0x1000u
#define RT_MEM_RESERVE 0x2000u
#define RT_MEM_FREE 0x10000u
#define RT_MEM_PRIVATE 0x20000u
#define RT_MEM_MAPPED 0x40000u
#define RT_MEM_IMAGE 0x1000000u
#define RT_PAGE_NOACCESS 0x01u
#define RT_PAGE_READONLY 0x02u
#define RT_PAGE_READWRITE 0x04u
#define RT_PAGE_WRITECOPY 0x08u
#define RT_PAGE_EXECUTE 0x10u
#define RT_PAGE_EXECUTE_READ 0x20u
#define RT_PAGE_EXECUTE_READWRITE 0x40u
#define RT_PAGE_EXECUTE_WRITECOPY 0x80u

/* One region of MEMORY_BASIC_INFORMATION. */
typedef struct rt_va_region {
    uint32_t base, alloc_base, alloc_protect, size, state, protect, type;
} rt_va_region;

/* The region that starts at the page of addr. Returns -1 if addr is
   outside the guest window. */
int rt_va_query(uint32_t addr, rt_va_region *r);
/* Set the type (RT_MEM_PRIVATE or RT_MEM_IMAGE) and the allocation
   protection of the allocation at base. */
void rt_va_set_alloc_info(uint32_t base, uint32_t type, uint32_t protect);
/* Record the protection of the committed pages of [addr, addr + size) (one
   allocation, or the program image). *old gets the protection of the first
   page. Returns -1 if a page is not committed or the range is not in one
   allocation. The host protection does not change. */
int rt_va_protect(uint32_t addr, uint32_t size, uint32_t protect, uint32_t *old);
/* Reserve free granules for a view of size bytes and call map(host, len,
   ctx) to map host memory at the host address of the range (len: size
   rounded up to host pages). map returns 0 or an errno value. Returns the
   guest base, or 0: no free range (*err 0) or map failed (*err its value).
   The pages of the view are committed with the given protection. */
uint32_t rt_va_map_view(uint32_t size, uint32_t protect, int (*map)(void *host, size_t len, void *ctx), void *ctx, int *err);
/* Remove the mapped view that contains addr. Returns -1 if addr is not in
   a view. */
int rt_va_unmap_view(uint32_t addr);
uint32_t rt_host_page_size(void);
/* Is guest address g in the program image (the translated code, the
   import thunks and the runtime)? */
int rt_guest_in_program_image(uint32_t g);

/* Guest heap (16-byte aligned blocks). Return 0 when no memory is left. */
uint32_t rt_heap_alloc(uint32_t size);
uint32_t rt_heap_calloc(uint32_t count, uint32_t size);
uint32_t rt_heap_realloc(uint32_t ptr, uint32_t size);
void rt_heap_free(uint32_t ptr);
uint32_t rt_heap_size(uint32_t ptr);
/* The process heap handle (GetProcessHeap, rt_kernel_misc.c). */
uint32_t rt_process_heap(void);
/* Global memory (rt_heap.c). A movable block (GMEM_MOVEABLE) has a handle;
   rt_global_lock gives its data address (a fixed block: the handle
   itself), and rt_global_unlock releases one lock. rt_global_size gives
   the requested size. rt_global_free frees a fixed or movable block. */
uint32_t rt_global_alloc_movable(uint32_t size, bool zero);
uint32_t rt_global_lock(uint32_t h);
void rt_global_unlock(uint32_t h);
uint32_t rt_global_size(uint32_t h);
void rt_global_free(uint32_t h);

/* USER32: show no alerts (MessageBoxA stops the process with its text).
   For command-line tools such as the AOT host. */
void rt_user32_set_headless(void);
/* Timed mouse and key events from a script file for a headless run (player
   host, RT_INPUT; syntax at the definition in rt_user32.c). Main thread,
   after rt_user32_init. */
void rt_user32_input_script(const char *path);

/* Audio output: silence. The DirectSound and waveOut audio units keep
   running, so the play positions and the buffer notifications of the guest
   do not change; the units send zero samples to the output device. Call it
   before the program starts (player host, RT_MUTE). */
void rt_audio_set_muted(void);
bool rt_audio_muted(void);

/* USER32 PostMessage for runtime code on any host thread (for example the
   Winsock resolver thread). Returns false if hwnd is not a window. */
bool rt_user32_post_message(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam);

/* Register the code of mono.dll that Mono copies to its own memory and
   calls, and set the stack overflow helper of Mono (rt_mono.c). Call it
   after mono.dll is mapped at base and before Mono starts. */
void rt_mono_dll_loaded(uint32_t base);
/* Set the AOT-only mode of the mapped mono.dll (base: its handle), as the
   option --full-aot of the Mono driver does (rt_mono.c). Call it before
   Mono starts. */
void rt_mono_set_aot_only(uint32_t base);
/* Load the AOT instance assembly at guest_path (tools/gen_aot_instances.cs)
   into the domain that mono_jit_init_version or mono_jit_init of the mapped
   mono.dll (base: its handle) makes. GetProcAddress gives wrappers of these
   two functions (rt_mono.c). Call it before Mono starts. */
void rt_mono_use_aot_instances(uint32_t base, const char *guest_path);
/* Call the managed Tick of the port's graphics rows (the assembly at
   guest_path, tools/port_graphics/PortGraphics.cs) from rt_mono_port_tick.
   Call it before Mono starts. */
void rt_mono_use_port_graphics(uint32_t base, const char *guest_path);
/* Run PortGraphics.GraphicsOptions.Tick once in the current domain of the
   main thread. Before Mono has loaded Assembly-CSharp, it does nothing. */
void rt_mono_port_tick(_cpu *cpu);

/* Graphics settings of the port (rt_portgfx.c). */
enum {
    RT_GFX_UPSCALING,
    RT_GFX_SHARPNESS, /* 0 = no RCAS sharpening */
    RT_GFX_HDR,
    RT_GFX_ANISOTROPIC,
    RT_GFX_SHADOWS,
    RT_GFX_FLOAT_TARGETS,
    RT_GFX_COUNT
};
/* Read the settings file at path (host path). Call it before the program
   starts. */
void rt_portgfx_init(const char *path);
/* The value of setting id that is in effect. */
int rt_portgfx_get(int id);
/* A frame was presented (any thread). */
void rt_portgfx_frame(void);
/* The main thread is in PeekMessage: after a presented frame, run the
   managed Tick (rt_mono_port_tick). */
void rt_portgfx_pump(_cpu *cpu);

/* Copy a NUL-terminated guest string with at most max-1 characters into a
   host buffer. Returns 0 on success, -1 if the string is longer. */
int rt_gstr(char *dst, size_t max, uint32_t gstr);

/* Windows-1252 (the ANSI code page) byte to Unicode (rt_kernel_misc.c). */
uint16_t rt_cp1252_to_unicode(uint8_t c);

/* ---------------------------------------------------------------- errors */

void rt_set_last_error(uint32_t err);
/* EncodePointer and DecodePointer (rt_kernel_sys.c). */
uint32_t rt_encode_pointer(uint32_t p);
uint32_t rt_decode_pointer(uint32_t p);
uint32_t rt_get_last_error(void);

#define ERROR_SUCCESS 0u
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_PATH_NOT_FOUND 3u
#define ERROR_TOO_MANY_OPEN_FILES 4u
#define ERROR_ACCESS_DENIED 5u
#define ERROR_INVALID_HANDLE 6u
#define ERROR_NOT_ENOUGH_MEMORY 8u
#define ERROR_INVALID_DATA 13u
#define ERROR_OUTOFMEMORY 14u
#define ERROR_NO_MORE_FILES 18u
#define ERROR_HANDLE_EOF 38u
#define ERROR_NOT_SUPPORTED 50u
#define ERROR_FILE_EXISTS 80u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_INSUFFICIENT_BUFFER 122u
#define ERROR_MOD_NOT_FOUND 126u
#define ERROR_PROC_NOT_FOUND 127u
#define ERROR_ALREADY_EXISTS 183u
#define ERROR_MORE_DATA 234u
#define ERROR_NO_MORE_ITEMS 259u

/* ---------------------------------------------------------------- process environment (rt_kernel_sys.c) */

/* Set (value non-NULL) or delete an environment variable of the guest
   process. name and value are UTF-8. */
void rt_env_set(const char *name, const char *value);
/* Set the command line that GetCommandLineA returns (UTF-8). The host
   calls it once, before guest code runs. */
void rt_set_command_line(const char *utf8);
/* The computer name (NetBIOS form of the host name). */
const char *rt_computer_name(void);
/* The number of emulated processors: the host processors that are online,
   at most 32 (GetSystemInfo, GetProcessAffinityMask, the registry). */
uint32_t rt_host_processor_count(void);
/* Use name (1 to 15 characters A-Z, 0-9 and '-') as the computer name and
   processors (1 to 32) as the processor count, in place of the values of
   the host. The guest then gets the same values on each Mac. The AOT
   compiler host uses this, so that its output does not depend on the
   computer. Call it before the first use of either value. */
void rt_set_fixed_host_identity(const char *name, uint32_t processors);

/* ---------------------------------------------------------------- registry (rt_registry.c) */

/* Set a value of a volatile key below HKEY_LOCAL_MACHINE\HARDWARE (path
   relative to HKEY_LOCAL_MACHINE, for example "HARDWARE\\DEVICEMAP\\VIDEO").
   Missing keys are created. The runtime uses it for data that it gets from
   the host at run time. */
void rt_reg_set_volatile(const char *path, const char *name, uint32_t type, const void *data, uint32_t size);
/* The same for a REG_SZ value from a UTF-8 string. */
void rt_reg_set_volatile_sz(const char *path, const char *name, const char *utf8);

#endif
