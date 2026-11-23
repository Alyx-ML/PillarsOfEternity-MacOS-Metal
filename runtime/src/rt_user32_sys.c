/*
 * USER32 system functions: rectangles, icons and cursors, system metrics
 * and parameters, displays and monitors, the clipboard, the window
 * station, wsprintfA and device notifications.
 *
 * Coordinates are points (one point is one Win32 pixel); the system has
 * 96 dots per inch. The displays come from rt_window.m (NSScreen and
 * Core Graphics). The main display is "\\.\DISPLAY1".
 */
#include "rt.h"
#include "rt_com.h"
#include "rt_file.h"
#include "rt_text.h"
#include "rt_thread.h"
#include "rt_user32.h"
#include "rt_window.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ERROR_INVALID_WINDOW_HANDLE 1400u
#define ERROR_INVALID_CURSOR_HANDLE 1402u
#define ERROR_INVALID_ICON_HANDLE 1414u
#define ERROR_CLIPBOARD_NOT_OPEN 1418u
#define ERROR_RESOURCE_DATA_NOT_FOUND 1812u
#define ERROR_RESOURCE_TYPE_NOT_FOUND 1813u
#define ERROR_RESOURCE_NAME_NOT_FOUND 1814u

static pthread_mutex_t sys_lock = PTHREAD_MUTEX_INITIALIZER;

static void lock(void)
{
    if (pthread_mutex_lock(&sys_lock) != 0) rt_fatal("USER32 lock failed");
}

static void unlock(void)
{
    if (pthread_mutex_unlock(&sys_lock) != 0) rt_fatal("USER32 unlock failed");
}

static void write_rect(uint32_t g, rt_rect r)
{
    wr32(g, (uint32_t)r.left);
    wr32(g + 4, (uint32_t)r.top);
    wr32(g + 8, (uint32_t)r.right);
    wr32(g + 12, (uint32_t)r.bottom);
}

static rt_rect read_rect(uint32_t g)
{
    return (rt_rect){ (int32_t)rd32(g), (int32_t)rd32(g + 4), (int32_t)rd32(g + 8), (int32_t)rd32(g + 12) };
}

/* ---------------------------------------------------------------- rectangles */

/* BOOL CopyRect(LPRECT dst, const RECT *src) */
void rt_CopyRect(_cpu *cpu)
{
    uint32_t d = ARG(0), s = ARG(1);
    if (d == 0 || s == 0)
    {
        RET(0);
        return;
    }
    write_rect(d, read_rect(s));
    RET(1);
}

/* BOOL OffsetRect(LPRECT, int dx, int dy) */
void rt_OffsetRect(_cpu *cpu)
{
    uint32_t r = ARG(0);
    int32_t dx = (int32_t)ARG(1), dy = (int32_t)ARG(2);
    rt_rect v;
    if (r == 0)
    {
        RET(0);
        return;
    }
    v = read_rect(r);
    v.left = (int32_t)((uint32_t)v.left + (uint32_t)dx);
    v.right = (int32_t)((uint32_t)v.right + (uint32_t)dx);
    v.top = (int32_t)((uint32_t)v.top + (uint32_t)dy);
    v.bottom = (int32_t)((uint32_t)v.bottom + (uint32_t)dy);
    write_rect(r, v);
    RET(1);
}

/* ---------------------------------------------------------------- icons and cursors */

#define MAX_ICONS 1024
#define ICON_TAG 0x60000000u
#define MAX_IMAGE_FILE 0x4000000u /* 64 MiB */

#define IMAGE_BITMAP 0u
#define IMAGE_ICON 1u
#define IMAGE_CURSOR 2u
#define LR_MONOCHROME 0x0001u
#define LR_LOADFROMFILE 0x0010u
#define LR_LOADTRANSPARENT 0x0020u
#define LR_DEFAULTSIZE 0x0040u
#define LR_VGACOLOR 0x0080u
#define LR_LOADMAP3DCOLORS 0x1000u
#define LR_CREATEDIBSECTION 0x2000u
#define LR_SHARED 0x8000u

typedef struct icon {
    uint32_t handle; /* 0: free */
    bool cursor, shared, destroyed;
    uint32_t system_id;     /* IDI_* or IDC_* of a system image, else 0 */
    uint8_t *rgba;
    uint32_t w, h;
    int32_t hot_x, hot_y;
    void *native;           /* native cursor (made when the cursor is first used) */
    uint32_t module, res_id; /* shared resource images: the module and the id */
    uint16_t res_name[64];  /* or the string name (upper case) */
} icon;

static icon icons[MAX_ICONS];
static uint16_t icon_gens[MAX_ICONS];
static uint32_t cur_cursor;   /* SetCursor */
static int32_t cursor_count;  /* ShowCursor display count (a mouse is present: 0) */

/* lock held */
static icon *icon_new(void)
{
    int i;
    for (i = 0; i < MAX_ICONS; i++)
    {
        if (icons[i].handle != 0) continue;
        memset(&icons[i], 0, sizeof icons[i]);
        icon_gens[i] = (uint16_t)((icon_gens[i] + 1u) & 0xFFFu);
        if (icon_gens[i] == 0) icon_gens[i] = 1;
        icons[i].handle = ICON_TAG | ((uint32_t)icon_gens[i] << 16) | ((uint32_t)(i + 1) << 2);
        return &icons[i];
    }
    return NULL;
}

/* lock held */
static icon *icon_get(uint32_t h)
{
    uint32_t i;
    if ((h & 0xF0000000u) != ICON_TAG || (h & 3u) != 0) return NULL;
    i = ((h & 0xFFFFu) >> 2);
    if (i == 0 || i > MAX_ICONS) return NULL;
    i--;
    if (icons[i].handle != h || icons[i].destroyed) return NULL;
    return &icons[i];
}

uint32_t rt_user32_icon_create(const uint8_t *rgba, uint32_t w, uint32_t h, bool cursor, int32_t hot_x, int32_t hot_y)
{
    icon *ic;
    uint32_t hd;
    size_t n = (size_t)w * h * 4u;
    uint8_t *copy;
    if (w == 0 || h == 0 || w > 1024 || h > 1024) return 0;
    copy = malloc(n);
    if (copy == NULL) rt_fatal("out of host memory (icon)");
    memcpy(copy, rgba, n);
    lock();
    ic = icon_new();
    if (ic == NULL)
    {
        unlock();
        free(copy);
        return 0;
    }
    ic->cursor = cursor;
    ic->rgba = copy;
    ic->w = w;
    ic->h = h;
    ic->hot_x = hot_x;
    ic->hot_y = hot_y;
    hd = ic->handle;
    unlock();
    return hd;
}

bool rt_user32_icon_pixels(uint32_t h, uint8_t **rgba, uint32_t *w, uint32_t *hgt, uint32_t *system_id)
{
    icon *ic;
    lock();
    ic = icon_get(h);
    if (ic == NULL)
    {
        unlock();
        return false;
    }
    *system_id = ic->system_id;
    *rgba = NULL;
    *w = ic->w;
    *hgt = ic->h;
    if (ic->rgba != NULL)
    {
        size_t n = (size_t)ic->w * ic->h * 4u;
        *rgba = malloc(n);
        if (*rgba == NULL) rt_fatal("out of host memory (icon)");
        memcpy(*rgba, ic->rgba, n);
    }
    unlock();
    return true;
}

static void icon_release(icon *ic)
{
    void *native = ic->native;
    free(ic->rgba);
    memset(ic, 0, sizeof *ic);
    if (native != NULL) rt_cocoa_cursor_release(native);
}

/* The native cursor of a cursor or icon handle (lock held). */
static void *native_cursor(icon *ic)
{
    if (ic->native == NULL)
    {
        if (ic->system_id != 0 && ic->rgba == NULL)
            ic->native = rt_cocoa_cursor_system(ic->system_id);
        else
            ic->native = rt_cocoa_cursor_create(ic->rgba, ic->w, ic->h, ic->cursor ? ic->hot_x : (int32_t)(ic->w / 2u),
                                                ic->cursor ? ic->hot_y : (int32_t)(ic->h / 2u));
        if (ic->native == NULL) rt_fatal("USER32: the native cursor of 0x%08x could not be made", ic->handle);
    }
    return ic->native;
}

/* Show the current cursor (lock held). */
static void apply_cursor(void)
{
    icon *ic = cur_cursor != 0 ? icon_get(cur_cursor) : NULL;
    if (ic == NULL && cur_cursor != 0)
    {
        /* A destroyed cursor that is still current. */
        uint32_t i = ((cur_cursor & 0xFFFFu) >> 2) - 1u;
        if (i < MAX_ICONS && icons[i].handle == cur_cursor) ic = &icons[i];
    }
    rt_cocoa_cursor_set(ic != NULL ? native_cursor(ic) : NULL, cursor_count >= 0 && ic != NULL);
}

uint32_t rt_user32_cursor_set(uint32_t hcursor)
{
    uint32_t old;
    lock();
    if (hcursor != 0 && icon_get(hcursor) == NULL)
    {
        unlock();
        rt_set_last_error(ERROR_INVALID_CURSOR_HANDLE);
        return 0;
    }
    old = cur_cursor;
    cur_cursor = hcursor;
    if (old != 0 && old != hcursor)
    {
        /* Free a cursor that DestroyCursor destroyed while it was current. */
        uint32_t i = ((old & 0xFFFFu) >> 2) - 1u;
        if (i < MAX_ICONS && icons[i].handle == old && icons[i].destroyed)
        {
            apply_cursor();
            icon_release(&icons[i]);
            unlock();
            return 0;
        }
    }
    apply_cursor();
    unlock();
    return old;
}

/* HCURSOR SetCursor(HCURSOR) */
void rt_SetCursor(_cpu *cpu)
{
    RET(rt_user32_cursor_set(ARG(0)));
}

/* int ShowCursor(BOOL show): returns the new display count. */
void rt_ShowCursor(_cpu *cpu)
{
    int32_t after;
    lock();
    if (ARG(0) != 0)
    {
        if (cursor_count == INT32_MAX) rt_fatal("ShowCursor: count overflow");
        cursor_count++;
    }
    else
    {
        if (cursor_count == INT32_MIN) rt_fatal("ShowCursor: count overflow");
        cursor_count--;
    }
    after = cursor_count;
    apply_cursor();
    unlock();
    RET((uint32_t)after);
}

/* BOOL DestroyCursor(HCURSOR) */
void rt_DestroyCursor(_cpu *cpu)
{
    uint32_t h = ARG(0);
    icon *ic;
    lock();
    ic = icon_get(h);
    if (ic == NULL)
    {
        unlock();
        rt_set_last_error(ERROR_INVALID_CURSOR_HANDLE);
        RET(0);
        return;
    }
    if (ic->shared)
    {
        /* Shared images (LoadCursor, LoadIcon, LR_SHARED) stay. */
        unlock();
        RET(1);
        return;
    }
    if (h == cur_cursor)
        ic->destroyed = true; /* freed when another cursor replaces it */
    else
        icon_release(ic);
    unlock();
    RET(1);
}

/* An icon or cursor from decoded pixels (lock held). */
static uint32_t icon_from_pixels(uint8_t *rgba, uint32_t w, uint32_t h, bool cursor, int32_t hx, int32_t hy)
{
    icon *ic = icon_new();
    if (ic == NULL)
    {
        free(rgba);
        return 0;
    }
    ic->cursor = cursor;
    ic->rgba = rgba;
    ic->w = w;
    ic->h = h;
    ic->hot_x = hx;
    ic->hot_y = hy;
    return ic->handle;
}

/* A system image: IDC_* (cursor) or IDI_* (icon). */
static uint32_t system_image(uint32_t id, bool cursor)
{
    uint32_t h;
    int i;
    lock();
    for (i = 0; i < MAX_ICONS; i++)
    {
        if (icons[i].handle != 0 && icons[i].shared && icons[i].module == 0 && icons[i].system_id == id &&
            icons[i].cursor == cursor)
        {
            h = icons[i].handle;
            unlock();
            return h;
        }
    }
    if (cursor)
    {
        void *n = rt_cocoa_cursor_system(id);
        icon *ic;
        if (n == NULL)
        {
            unlock();
            return 0;
        }
        ic = icon_new();
        if (ic == NULL)
        {
            unlock();
            rt_cocoa_cursor_release(n);
            return 0;
        }
        ic->cursor = true;
        ic->shared = true;
        ic->system_id = id;
        ic->native = n;
        ic->w = ic->h = 32;
        h = ic->handle;
    }
    else
    {
        icon *ic;
        if (id < 32512u || id > 32518u)
        {
            unlock();
            return 0;
        }
        ic = icon_new();
        if (ic == NULL)
        {
            unlock();
            return 0;
        }
        ic->shared = true;
        ic->system_id = id;
        ic->w = ic->h = 32;
        h = ic->handle;
    }
    unlock();
    return h;
}

/* Read a whole file of the guest (malloc). */
static uint8_t *read_guest_file(uint32_t gpath, bool wide, size_t *n)
{
    char in[RT_PATH_BYTES], full[RT_PATH_BYTES], host[RT_HOST_PATH];
    bool exists = false;
    struct stat st;
    uint8_t *buf;
    size_t done = 0;
    int fd;
    if ((wide ? rt_gstr_w(in, sizeof in, gpath) : rt_gstr_a(in, sizeof in, gpath)) != RT_STR_OK) return NULL;
    if (rt_path_full(in, full, sizeof full) != 0 || rt_path_host(full, host, sizeof host, &exists) != 0 || !exists)
        return NULL;
    fd = open(host, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > (off_t)MAX_IMAGE_FILE)
    {
        close(fd);
        return NULL;
    }
    buf = malloc((size_t)st.st_size);
    if (buf == NULL) rt_fatal("out of host memory (image file)");
    while (done < (size_t)st.st_size)
    {
        ssize_t r = read(fd, buf + done, (size_t)st.st_size - done);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0)
        {
            close(fd);
            free(buf);
            return NULL;
        }
        done += (size_t)r;
    }
    close(fd);
    *n = done;
    return buf;
}

/* The best image of a directory for the size cx x cy: the nearest size,
   then the most bits per pixel. */
static int best_entry(const uint32_t *ws, const uint32_t *hs, const uint32_t *bpps, int n, uint32_t cx, uint32_t cy)
{
    int i, best = -1;
    uint32_t best_d = 0, best_bpp = 0;
    for (i = 0; i < n; i++)
    {
        uint32_t d = (ws[i] > cx ? ws[i] - cx : cx - ws[i]) + (hs[i] > cy ? hs[i] - cy : cy - hs[i]);
        if (best < 0 || d < best_d || (d == best_d && bpps[i] > best_bpp))
        {
            best = i;
            best_d = d;
            best_bpp = bpps[i];
        }
    }
    return best;
}

/* An icon or cursor from a group resource of the module. */
static uint32_t load_group(uint32_t module, uint32_t name, bool wide, bool cursor, uint32_t cx, uint32_t cy)
{
    uint32_t data, size, n, i, id[64], ws[64], hs[64], bpps[64], d2, s2, hd;
    uint8_t *rgba;
    uint32_t w, h;
    int32_t hx = 0, hy = 0;
    bool alpha;
    int b;
    if (!rt_resource_find(module, cursor ? RT_RES_GROUP_CURSOR : RT_RES_GROUP_ICON, name, wide, &data, &size))
    {
        rt_set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND);
        return 0;
    }
    if (size < 6 || rd16(data + 2) != (cursor ? 2u : 1u))
    {
        rt_set_last_error(ERROR_RESOURCE_DATA_NOT_FOUND);
        return 0;
    }
    n = rd16(data + 4);
    if (n == 0 || n > 64 || 6u + 14u * n > size)
    {
        rt_set_last_error(ERROR_RESOURCE_DATA_NOT_FOUND);
        return 0;
    }
    for (i = 0; i < n; i++)
    {
        uint32_t e = data + 6u + 14u * i;
        if (cursor)
        {
            ws[i] = rd16(e);
            hs[i] = rd16(e + 2) / 2u;
            bpps[i] = rd16(e + 6);
        }
        else
        {
            ws[i] = rd8(e) != 0 ? rd8(e) : 256u;
            hs[i] = rd8(e + 1) != 0 ? rd8(e + 1) : 256u;
            bpps[i] = rd16(e + 6);
        }
        id[i] = rd16(e + 12);
    }
    b = best_entry(ws, hs, bpps, (int)n, cx, cy);
    if (!rt_resource_find(module, cursor ? RT_RES_CURSOR : RT_RES_ICON, id[b], false, &d2, &s2) ||
        s2 < (cursor ? 4u : 0u) + 8u)
    {
        rt_set_last_error(ERROR_RESOURCE_DATA_NOT_FOUND);
        return 0;
    }
    if (cursor)
    {
        hx = (int16_t)rd16(d2);
        hy = (int16_t)rd16(d2 + 2);
        d2 += 4;
        s2 -= 4;
    }
    if (!rt_image_decode(G2H(d2), s2, true, &rgba, &w, &h, &alpha))
    {
        rt_set_last_error(ERROR_RESOURCE_DATA_NOT_FOUND);
        return 0;
    }
    lock();
    hd = icon_from_pixels(rgba, w, h, cursor, hx, hy);
    unlock();
    if (hd == 0) rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return hd;
}

/* An icon or cursor from an .ico or .cur file. */
static uint32_t load_icon_file(const uint8_t *p, size_t n, bool cursor, uint32_t cx, uint32_t cy)
{
    uint32_t count, i, ws[64], hs[64], bpps[64], hd, off, size, w, h;
    uint8_t *rgba;
    bool alpha;
    int b;
    if (n < 6 || p[0] != 0 || p[1] != 0 || p[2] != (cursor ? 2u : 1u) || p[3] != 0) return 0;
    count = (uint32_t)p[4] | ((uint32_t)p[5] << 8);
    if (count == 0 || count > 64 || 6u + 16u * count > n) return 0;
    for (i = 0; i < count; i++)
    {
        const uint8_t *e = p + 6u + 16u * i;
        ws[i] = e[0] != 0 ? e[0] : 256u;
        hs[i] = e[1] != 0 ? e[1] : 256u;
        bpps[i] = cursor ? 32u : (uint32_t)e[6] | ((uint32_t)e[7] << 8);
    }
    b = best_entry(ws, hs, bpps, (int)count, cx, cy);
    {
        const uint8_t *e = p + 6u + 16u * (uint32_t)b;
        memcpy(&size, e + 8, 4);
        memcpy(&off, e + 12, 4);
        if (off > n || size > n - off) return 0;
        if (!rt_image_decode(p + off, size, true, &rgba, &w, &h, &alpha)) return 0;
        lock();
        hd = icon_from_pixels(rgba, w, h, cursor, cursor ? (int32_t)((uint32_t)e[4] | ((uint32_t)e[5] << 8)) : 0,
                              cursor ? (int32_t)((uint32_t)e[6] | ((uint32_t)e[7] << 8)) : 0);
        unlock();
    }
    return hd;
}

/* The shared image of (module, name), if it was loaded before. */
static uint32_t find_shared(uint32_t module, uint32_t name, bool wide, bool cursor, uint16_t *key)
{
    int i, j;
    memset(key, 0, 64 * sizeof(uint16_t));
    if (name >= 0x10000u)
    {
        for (j = 0; j < 63; j++)
        {
            uint16_t c = wide ? rd16(name + 2u * (uint32_t)j) : rt_cp1252_to_unicode(rd8(name + (uint32_t)j));
            if (c == 0) break;
            key[j] = (c >= 'a' && c <= 'z') ? (uint16_t)(c - 32u) : c;
        }
    }
    lock();
    for (i = 0; i < MAX_ICONS; i++)
    {
        if (icons[i].handle == 0 || !icons[i].shared || icons[i].module != module || icons[i].cursor != cursor) continue;
        if (name < 0x10000u ? icons[i].res_id == name : memcmp(icons[i].res_name, key, 64 * sizeof(uint16_t)) == 0)
        {
            uint32_t h = icons[i].handle;
            unlock();
            return h;
        }
    }
    unlock();
    return 0;
}

static void mark_shared(uint32_t h, uint32_t module, uint32_t name, const uint16_t *key)
{
    icon *ic;
    lock();
    ic = icon_get(h);
    if (ic != NULL)
    {
        ic->shared = true;
        ic->module = module;
        if (name < 0x10000u)
            ic->res_id = name;
        else
            memcpy(ic->res_name, key, 64 * sizeof(uint16_t));
    }
    unlock();
}

/* LoadCursor and LoadIcon: a shared image of the module or a system
   image (module 0). */
static uint32_t load_shared(uint32_t module, uint32_t name, bool cursor)
{
    uint16_t key[64];
    uint32_t h;
    if (module == 0)
    {
        if (name >= 0x10000u) rt_fatal("%s: a string name for a system image is not supported", cursor ? "LoadCursorA" : "LoadIconA");
        h = system_image(name, cursor);
        if (h == 0) rt_set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND);
        return h;
    }
    h = find_shared(module, name, false, cursor, key);
    if (h != 0) return h;
    h = load_group(module, name, false, cursor, 32, 32);
    if (h != 0) mark_shared(h, module, name, key);
    return h;
}

/* HCURSOR LoadCursorA(HINSTANCE, LPCSTR name) */
void rt_LoadCursorA(_cpu *cpu)
{
    RET(load_shared(ARG(0), ARG(1), true));
}

/* HICON LoadIconA(HINSTANCE, LPCSTR name) */
void rt_LoadIconA(_cpu *cpu)
{
    RET(load_shared(ARG(0), ARG(1), false));
}

/* A bitmap resource (RT_BITMAP): the DIB without the file header. The
   bits follow the header and the color table. */
static uint32_t load_bitmap_resource(uint32_t module, uint32_t name, bool dib_section)
{
    uint32_t data, size, hs, bpp, ncol, table, bits_off, comp, h;
    if (!rt_resource_find(module, RT_RES_BITMAP, name, false, &data, &size))
    {
        rt_set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND);
        return 0;
    }
    hs = size >= 4 ? rd32(data) : 0;
    if ((hs != 12 && hs != 40 && hs != 108 && hs != 124) || size < hs) goto bad;
    if (hs == 12)
    {
        bpp = rd16(data + 10);
        ncol = bpp <= 8 ? 1u << bpp : 0;
        table = ncol * 3u;
    }
    else
    {
        bpp = rd16(data + 14);
        comp = rd32(data + 16);
        ncol = rd32(data + 32);
        if (bpp <= 8)
        {
            if (ncol == 0 || ncol > (1u << bpp)) ncol = 1u << bpp;
        }
        else if (ncol > 256)
            goto bad;
        /* BI_BITFIELDS with a 40-byte header: three masks follow. */
        table = ncol * 4u + (hs == 40 && comp == 3u ? 12u : 0u);
    }
    bits_off = hs + table;
    if (bits_off > size) goto bad;
    h = rt_gdi_bitmap_from_dib(G2H(data), size, G2H(data + bits_off), size - bits_off, dib_section);
    if (h == 0) goto bad;
    return h;
bad:
    rt_set_last_error(ERROR_RESOURCE_DATA_NOT_FOUND);
    return 0;
}

/* For the dialog manager (rt_dialog.c): an icon or a bitmap resource. */
uint32_t rt_user32_load_icon(uint32_t module, uint32_t name)
{
    return load_shared(module, name, false);
}

uint32_t rt_user32_load_bitmap(uint32_t module, uint32_t name)
{
    return load_bitmap_resource(module, name, false);
}

/* HANDLE LoadImageA(HINSTANCE, LPCSTR name, UINT type, int cx, int cy,
   UINT flags) */
void rt_LoadImageA(_cpu *cpu)
{
    uint32_t module = ARG(0), name = ARG(1), type = ARG(2), flags = ARG(5), h;
    int32_t cx = (int32_t)ARG(3), cy = (int32_t)ARG(4);
    if (flags & (LR_MONOCHROME | LR_LOADTRANSPARENT | LR_VGACOLOR | LR_LOADMAP3DCOLORS))
        rt_fatal("LoadImageA: flags 0x%x (color changes) are not supported", flags);
    if (type > IMAGE_CURSOR) rt_fatal("LoadImageA: image type %u is not supported", type);
    if (cx < 0 || cy < 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (type != IMAGE_BITMAP)
    {
        /* The system size (32) for 0 with LR_DEFAULTSIZE; LoadImage keeps
           the size of the chosen image (the runtime does not scale). */
        if (cx == 0) cx = 32;
        if (cy == 0) cy = 32;
    }
    if (flags & LR_LOADFROMFILE)
    {
        size_t n = 0;
        uint8_t *p = read_guest_file(name, false, &n);
        if (p == NULL)
        {
            rt_set_last_error(ERROR_FILE_NOT_FOUND);
            RET(0);
            return;
        }
        if (type == IMAGE_BITMAP)
        {
            uint32_t off;
            /* BITMAPFILEHEADER (14 bytes), then the DIB. */
            if (n < 14 + 12 || p[0] != 'B' || p[1] != 'M')
                h = 0;
            else
            {
                memcpy(&off, p + 10, 4);
                h = off >= 14 && off <= n ? rt_gdi_bitmap_from_dib(p + 14, n - 14, p + off, n - off, (flags & LR_CREATEDIBSECTION) != 0)
                                          : 0;
            }
        }
        else
            h = load_icon_file(p, n, type == IMAGE_CURSOR, (uint32_t)cx, (uint32_t)cy);
        free(p);
        if (h == 0) rt_set_last_error(ERROR_INVALID_DATA);
        RET(h);
        return;
    }
    if (module == 0) rt_fatal("LoadImageA: system (OEM) images are not supported");
    if (type == IMAGE_BITMAP)
    {
        RET(load_bitmap_resource(module, name, (flags & LR_CREATEDIBSECTION) != 0));
        return;
    }
    if (flags & LR_SHARED)
    {
        uint16_t key[64];
        h = find_shared(module, name, false, type == IMAGE_CURSOR, key);
        if (h == 0)
        {
            h = load_group(module, name, false, type == IMAGE_CURSOR, (uint32_t)cx, (uint32_t)cy);
            if (h != 0) mark_shared(h, module, name, key);
        }
    }
    else
        h = load_group(module, name, false, type == IMAGE_CURSOR, (uint32_t)cx, (uint32_t)cy);
    RET(h);
}

/* HICON CreateIconIndirect(PICONINFO): fIcon 0, xHotspot 4, yHotspot 8,
   hbmMask 12, hbmColor 16. */
void rt_CreateIconIndirect(_cpu *cpu)
{
    uint32_t ii = ARG(0), hmask, hcolor, w, h, mw, mh, i, hd;
    uint8_t *color = NULL, *mask = NULL, *rgba;
    bool alpha, malpha, is_icon;
    int32_t hx, hy;
    if (ii == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    is_icon = rd32(ii) != 0;
    hx = (int32_t)rd32(ii + 4);
    hy = (int32_t)rd32(ii + 8);
    hmask = rd32(ii + 12);
    hcolor = rd32(ii + 16);
    if (hmask == 0 || !rt_gdi_bitmap_rgba(hmask, &mask, &mw, &mh, &malpha))
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (hcolor == 0)
    {
        /* A monochrome image: the AND mask on top of the XOR mask. */
        if (mh < 2 || mh % 2u != 0)
        {
            free(mask);
            rt_set_last_error(ERROR_INVALID_PARAMETER);
            RET(0);
            return;
        }
        w = mw;
        h = mh / 2u;
        rgba = malloc((size_t)w * h * 4u);
        if (rgba == NULL) rt_fatal("out of host memory (icon)");
        for (i = 0; i < w * h; i++)
        {
            bool and_bit = mask[4u * i] != 0, xor_bit = mask[4u * (i + w * h)] != 0;
            /* AND 0: black (XOR 0) or white (XOR 1); AND 1, XOR 0:
               transparent. AND 1, XOR 1 inverts the screen, which macOS
               cursors cannot do: the runtime shows black. */
            uint8_t v = !and_bit && xor_bit ? 255 : 0;
            rgba[4u * i] = rgba[4u * i + 1u] = rgba[4u * i + 2u] = v;
            rgba[4u * i + 3u] = and_bit && !xor_bit ? 0 : 255;
        }
        free(mask);
    }
    else
    {
        if (!rt_gdi_bitmap_rgba(hcolor, &color, &w, &h, &alpha))
        {
            free(mask);
            rt_set_last_error(ERROR_INVALID_PARAMETER);
            RET(0);
            return;
        }
        if (!alpha)
        {
            /* No alpha channel: the mask gives the transparency (1:
               transparent). */
            if (mw != w || mh < h)
            {
                free(mask);
                free(color);
                rt_set_last_error(ERROR_INVALID_PARAMETER);
                RET(0);
                return;
            }
            for (i = 0; i < w * h; i++) color[4u * i + 3u] = mask[4u * i] != 0 ? 0 : 255;
        }
        free(mask);
        rgba = color;
    }
    if (is_icon)
    {
        hx = (int32_t)(w / 2u);
        hy = (int32_t)(h / 2u);
    }
    lock();
    hd = icon_from_pixels(rgba, w, h, !is_icon, hx, hy);
    unlock();
    if (hd == 0) rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    RET(hd);
}

/* BOOL GetCursorPos(LPPOINT) */
void rt_GetCursorPos(_cpu *cpu)
{
    int32_t x, y;
    if (ARG(0) == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    rt_cocoa_cursor_pos(&x, &y);
    wr32(ARG(0), (uint32_t)x);
    wr32(ARG(0) + 4, (uint32_t)y);
    RET(1);
}

static bool clip_set;
static rt_rect clip_rect;

/* BOOL SetCursorPos(int x, int y): the position stays in the ClipCursor
   rectangle. */
void rt_SetCursorPos(_cpu *cpu)
{
    int32_t x = (int32_t)ARG(0), y = (int32_t)ARG(1);
    lock();
    if (clip_set)
    {
        if (x < clip_rect.left) x = clip_rect.left;
        if (x >= clip_rect.right) x = clip_rect.right - 1;
        if (y < clip_rect.top) y = clip_rect.top;
        if (y >= clip_rect.bottom) y = clip_rect.bottom - 1;
    }
    unlock();
    rt_cocoa_cursor_move(x, y);
    RET(1);
}

/* BOOL ClipCursor(const RECT *): NULL removes the limit. */
void rt_ClipCursor(_cpu *cpu)
{
    uint32_t r = ARG(0);
    rt_rect v;
    if (r == 0)
    {
        lock();
        clip_set = false;
        unlock();
        rt_cocoa_cursor_clip(NULL);
        RET(1);
        return;
    }
    v = read_rect(r);
    if (v.right < v.left || v.bottom < v.top)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    lock();
    clip_set = true;
    clip_rect = v;
    unlock();
    rt_cocoa_cursor_clip(&v);
    RET(1);
}

/* ---------------------------------------------------------------- displays */

static rt_rect virtual_screen(const rt_display *d, int n)
{
    rt_rect v = d[0].frame;
    int i;
    for (i = 1; i < n; i++)
    {
        if (d[i].frame.left < v.left) v.left = d[i].frame.left;
        if (d[i].frame.top < v.top) v.top = d[i].frame.top;
        if (d[i].frame.right > v.right) v.right = d[i].frame.right;
        if (d[i].frame.bottom > v.bottom) v.bottom = d[i].frame.bottom;
    }
    return v;
}

/* int GetSystemMetrics(int index) */
void rt_GetSystemMetrics(_cpu *cpu)
{
    rt_display d[RT_MAX_DISPLAYS];
    int n = rt_cocoa_displays(d);
    uint32_t index = ARG(0);
    int32_t w = d[0].frame.right - d[0].frame.left, h = d[0].frame.bottom - d[0].frame.top;
    int32_t ww = d[0].work.right - d[0].work.left, wh = d[0].work.bottom - d[0].work.top;
    int32_t cap = rt_cocoa_title_height();
    rt_rect v = virtual_screen(d, n);
    int32_t r;
    switch (index)
    {
    case 0: r = w; break;                        /* SM_CXSCREEN */
    case 1: r = h; break;                        /* SM_CYSCREEN */
    case 2: case 3: case 9: case 10: case 20: case 21: r = 15; break; /* scroll bar sizes (macOS: 15) */
    case 4: r = cap; break;                      /* SM_CYCAPTION */
    case 5: case 6: r = 0; break;                /* SM_CXBORDER: macOS frames have no border */
    case 7: case 8: case 32: case 33: case 92: r = 0; break; /* frame sizes: no frame border */
    case 11: case 12: r = 32; break;             /* SM_CXICON */
    case 13: case 14: r = 32; break;             /* SM_CXCURSOR */
    case 15: r = 0; break;                       /* SM_CYMENU: windows have no menu bar */
    case 16: r = ww; break;                      /* SM_CXFULLSCREEN */
    case 17: r = wh - cap; break;                /* SM_CYFULLSCREEN */
    case 19: r = 1; break;                       /* SM_MOUSEPRESENT */
    case 22: r = 0; break;                       /* SM_DEBUG */
    case 23: r = 0; break;                       /* SM_SWAPBUTTON */
    case 36: case 37: r = 4; break;              /* SM_CXDOUBLECLK */
    case 38: case 39: r = 75; break;             /* SM_CXICONSPACING */
    case 41: r = 0; break;                       /* SM_PENWINDOWS */
    case 42: r = 0; break;                       /* SM_DBCSENABLED */
    case 43: r = 3; break;                       /* SM_CMOUSEBUTTONS */
    case 49: case 50: r = 16; break;             /* SM_CXSMICON */
    case 61: r = ww; break;                      /* SM_CXMAXIMIZED */
    case 62: r = wh; break;                      /* SM_CYMAXIMIZED */
    case 59: r = v.right - v.left; break;        /* SM_CXMAXTRACK */
    case 60: r = v.bottom - v.top; break;        /* SM_CYMAXTRACK */
    case 63: r = 1; break;                       /* SM_NETWORK */
    case 67: r = 0; break;                       /* SM_CLEANBOOT */
    case 68: case 69: r = 4; break;              /* SM_CXDRAG */
    case 70: r = 0; break;                       /* SM_SHOWSOUNDS */
    case 73: r = 0; break;                       /* SM_SLOWMACHINE */
    case 74: r = 0; break;                       /* SM_MIDEASTENABLED */
    case 75: r = 1; break;                       /* SM_MOUSEWHEELPRESENT */
    case 76: r = v.left; break;                  /* SM_XVIRTUALSCREEN */
    case 77: r = v.top; break;                   /* SM_YVIRTUALSCREEN */
    case 78: r = v.right - v.left; break;        /* SM_CXVIRTUALSCREEN */
    case 79: r = v.bottom - v.top; break;        /* SM_CYVIRTUALSCREEN */
    case 80: r = n; break;                       /* SM_CMONITORS */
    case 81: r = 1; break;                       /* SM_SAMEDISPLAYFORMAT */
    case 82: r = 1; break;                       /* SM_IMMENABLED */
    case 83: case 84: r = 1; break;              /* SM_CXFOCUSBORDER */
    case 86: r = 0; break;                       /* SM_TABLETPC */
    case 87: r = 0; break;                       /* SM_MEDIACENTER */
    case 88: r = 0; break;                       /* SM_STARTER */
    case 89: r = 0; break;                       /* SM_SERVERR2 */
    case 91: r = 1; break;                       /* SM_MOUSEHORIZONTALWHEELPRESENT */
    case 94: r = 0; break;                       /* SM_DIGITIZER */
    case 95: r = 0; break;                       /* SM_MAXIMUMTOUCHES */
    case 0x1000: r = 0; break;                   /* SM_REMOTESESSION */
    case 0x2000: r = 0; break;                   /* SM_SHUTTINGDOWN */
    case 0x2001: r = 0; break;                   /* SM_REMOTECONTROL */
    case 0x2003: r = 1; break;                   /* SM_CARETBLINKINGENABLED */
    default: rt_fatal("GetSystemMetrics: index %u is not supported", index);
    }
    RET((uint32_t)r);
}

static uint32_t mouse_trails;

/* BOOL SystemParametersInfoW(UINT action, UINT ui, PVOID pv, UINT winini) */
void rt_SystemParametersInfoW(_cpu *cpu)
{
    uint32_t action = ARG(0), ui = ARG(1), pv = ARG(2);
    switch (action)
    {
    case 0x0030: /* SPI_GETWORKAREA */
    {
        rt_display d[RT_MAX_DISPLAYS];
        if (pv == 0) break;
        rt_cocoa_displays(d);
        write_rect(pv, d[0].work);
        RET(1);
        return;
    }
    case 0x005D: /* SPI_SETMOUSETRAILS: macOS has no mouse trails; the value is kept */
        lock();
        mouse_trails = ui;
        unlock();
        RET(1);
        return;
    case 0x005E: /* SPI_GETMOUSETRAILS */
        if (pv == 0) break;
        lock();
        wr32(pv, mouse_trails);
        unlock();
        RET(1);
        return;
    case 0x0068: /* SPI_GETWHEELSCROLLLINES */
    case 0x006C: /* SPI_GETWHEELSCROLLCHARS */
        if (pv == 0) break;
        wr32(pv, 3);
        RET(1);
        return;
    default:
        rt_fatal("SystemParametersInfoW: action 0x%04x is not supported", action);
    }
    rt_set_last_error(ERROR_INVALID_PARAMETER);
    RET(0);
}

/* UINT GetCaretBlinkTime(void) */
void rt_GetCaretBlinkTime(_cpu *cpu)
{
    RET(rt_cocoa_caret_blink_ms());
}

/* The display index of "\\.\DISPLAYn" (NULL: the main display), or -1. */
static int display_index(uint32_t gname, int n)
{
    char name[64];
    unsigned v = 0;
    size_t i;
    if (gname == 0) return 0;
    if (rt_gstr_a(name, sizeof name, gname) != RT_STR_OK) return -1;
    if (strncasecmp(name, "\\\\.\\DISPLAY", 11) != 0 || name[11] == 0) return -1;
    for (i = 11; name[i] != 0; i++)
    {
        if (name[i] < '0' || name[i] > '9' || v > 100) return -1;
        v = v * 10u + (unsigned)(name[i] - '0');
    }
    if (v == 0 || (int)v > n) return -1;
    return (int)v - 1;
}

#define DM_POSITION 0x00000020u
#define DM_DISPLAYORIENTATION 0x00000080u
#define DM_BITSPERPEL 0x00040000u
#define DM_PELSWIDTH 0x00080000u
#define DM_PELSHEIGHT 0x00100000u
#define DM_DISPLAYFLAGS 0x00200000u
#define DM_DISPLAYFREQUENCY 0x00400000u
#define DEVMODE_SIZE 156u

#define MAX_MODES 512
static rt_display_mode start_mode[RT_MAX_DISPLAYS]; /* the mode at the first query (ENUM_REGISTRY_SETTINGS) */
static bool start_known[RT_MAX_DISPLAYS];

static void write_devmode(uint32_t dm, const rt_display *d, const rt_display_mode *m, bool position)
{
    uint32_t i;
    /* dmDeviceName (offset 0) is written by the caller. */
    for (i = 0; i < DEVMODE_SIZE; i++) wr8(dm + i, 0);
    wr16(dm + 32, 0x0401);          /* dmSpecVersion (DM_SPECVERSION) */
    wr16(dm + 34, 0x0401);          /* dmDriverVersion */
    wr16(dm + 36, DEVMODE_SIZE);    /* dmSize */
    wr16(dm + 38, 0);               /* dmDriverExtra */
    wr32(dm + 40, DM_POSITION | DM_DISPLAYORIENTATION | DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFLAGS |
                      DM_DISPLAYFREQUENCY);
    wr32(dm + 44, position ? (uint32_t)d->frame.left : 0);
    wr32(dm + 48, position ? (uint32_t)d->frame.top : 0);
    wr32(dm + 52, 0);               /* DMDO_DEFAULT */
    wr16(dm + 102, 96);             /* dmLogPixels */
    wr32(dm + 104, 32);
    wr32(dm + 108, m->width);
    wr32(dm + 112, m->height);
    wr32(dm + 116, 0);
    wr32(dm + 120, m->refresh);
}

/* BOOL EnumDisplaySettingsA(LPCSTR device, DWORD mode, DEVMODEA *) */
void rt_EnumDisplaySettingsA(_cpu *cpu)
{
    rt_display d[RT_MAX_DISPLAYS];
    rt_display_mode *modes;
    uint32_t mode = ARG(1), dm = ARG(2);
    int n = rt_cocoa_displays(d), idx = display_index(ARG(0), n), count, cur = 0;
    char dev[32];
    if (idx < 0 || dm == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (rd16(dm + 36) < 124u) rt_fatal("EnumDisplaySettingsA: a DEVMODEA of %u bytes is not supported", rd16(dm + 36));
    modes = malloc(MAX_MODES * sizeof *modes);
    if (modes == NULL) rt_fatal("out of host memory (display modes)");
    count = rt_cocoa_display_modes(d[idx].id, modes, MAX_MODES, &cur);
    if (count > MAX_MODES) count = MAX_MODES;
    lock();
    if (!start_known[idx] && cur >= 0 && cur < count)
    {
        start_mode[idx] = modes[cur];
        start_known[idx] = true;
    }
    unlock();
    snprintf(dev, sizeof dev, "\\\\.\\DISPLAY%d", idx + 1);
    if (mode == 0xFFFFFFFFu || mode == 0xFFFFFFFEu)
    {
        rt_display_mode m;
        if (cur < 0 || cur >= count) rt_fatal("EnumDisplaySettingsA: the current mode of display %d is not known", idx + 1);
        m = modes[cur];
        lock();
        if (mode == 0xFFFFFFFEu && start_known[idx]) m = start_mode[idx];
        unlock();
        write_devmode(dm, &d[idx], &m, true);
        rt_put_afield(dm, 32, dev);
        free(modes);
        RET(1);
        return;
    }
    if (mode >= (uint32_t)count)
    {
        free(modes);
        rt_set_last_error(ERROR_NO_MORE_FILES);
        RET(0);
        return;
    }
    write_devmode(dm, &d[idx], &modes[mode], false);
    rt_put_afield(dm, 32, dev);
    free(modes);
    RET(1);
}

#define CDS_UPDATEREGISTRY 0x00000001u
#define CDS_TEST 0x00000002u
#define CDS_FULLSCREEN 0x00000004u
#define DISP_CHANGE_SUCCESSFUL 0
#define DISP_CHANGE_FAILED (-1)
#define DISP_CHANGE_BADMODE (-2)
#define WM_DISPLAYCHANGE 0x007Eu

/* LONG ChangeDisplaySettingsA(DEVMODEA *, DWORD flags): the main display.
   A change lasts until ChangeDisplaySettingsA(NULL, 0) or the end of the
   process. */
void rt_ChangeDisplaySettingsA(_cpu *cpu)
{
    rt_display d[RT_MAX_DISPLAYS];
    rt_display_mode modes[MAX_MODES], m;
    uint32_t dm = ARG(0), flags = ARG(1), fields;
    int count, cur = 0;
    bool test = (flags & CDS_TEST) != 0;
    if (flags & CDS_UPDATEREGISTRY)
        rt_fatal("ChangeDisplaySettingsA: CDS_UPDATEREGISTRY (a permanent change of the Mac display mode) is not supported");
    if (flags & ~(CDS_TEST | CDS_FULLSCREEN)) rt_fatal("ChangeDisplaySettingsA: flags 0x%x are not supported", flags);
    if (dm == 0)
    {
        if (flags != 0 && flags != CDS_FULLSCREEN) rt_fatal("ChangeDisplaySettingsA(NULL): flags 0x%x are not supported", flags);
        rt_cocoa_restore_display_modes();
        {
            rt_display_mode cm[MAX_MODES];
            int c2, k = 0;
            rt_cocoa_displays(d);
            c2 = rt_cocoa_display_modes(d[0].id, cm, MAX_MODES, &k);
            if (k >= 0 && k < c2 && k < MAX_MODES)
                rt_user32_send(rt_self()->cpu, 0xFFFFu, WM_DISPLAYCHANGE, 32, (cm[k].width & 0xFFFFu) | (cm[k].height << 16), false);
        }
        RET((uint32_t)DISP_CHANGE_SUCCESSFUL);
        return;
    }
    rt_cocoa_displays(d);
    count = rt_cocoa_display_modes(d[0].id, modes, MAX_MODES, &cur);
    if (count > MAX_MODES) count = MAX_MODES;
    if (cur < 0 || cur >= count) rt_fatal("ChangeDisplaySettingsA: the current mode of the main display is not known");
    m = modes[cur];
    fields = rd32(dm + 40);
    if ((fields & DM_BITSPERPEL) && rd32(dm + 104) != 32u)
    {
        RET((uint32_t)DISP_CHANGE_BADMODE);
        return;
    }
    if (fields & DM_PELSWIDTH) m.width = rd32(dm + 108);
    if (fields & DM_PELSHEIGHT) m.height = rd32(dm + 112);
    m.refresh = (fields & DM_DISPLAYFREQUENCY) ? rd32(dm + 120) : 0;
    if (m.refresh == 1) m.refresh = 0; /* 0 and 1: the default refresh rate of the mode */
    if (!rt_cocoa_set_display_mode(d[0].id, &m, test))
    {
        RET((uint32_t)DISP_CHANGE_BADMODE);
        return;
    }
    if (!test)
        rt_user32_send(rt_self()->cpu, 0xFFFFu, WM_DISPLAYCHANGE, 32, (m.width & 0xFFFFu) | (m.height << 16), false);
    RET((uint32_t)DISP_CHANGE_SUCCESSFUL);
}

/* BOOL EnumDisplayDevicesA(LPCSTR device, DWORD index, PDISPLAY_DEVICEA,
   DWORD flags). DISPLAY_DEVICEA: cb 0, DeviceName[32] 4,
   DeviceString[128] 36, StateFlags 164, DeviceID[128] 168,
   DeviceKey[128] 296 (424 bytes). */
void rt_EnumDisplayDevicesA(_cpu *cpu)
{
    rt_display d[RT_MAX_DISPLAYS];
    uint32_t dev = ARG(0), index = ARG(1), dd = ARG(2), cb, i;
    int n = rt_cocoa_displays(d);
    char name[64];
    if (dd == 0 || (cb = rd32(dd)) < 168u)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    for (i = 4; i < cb && i < 424u; i++) wr8(dd + i, 0);
    if (dev == 0)
    {
        /* The display adapters: one for each display. */
        if (index >= (uint32_t)n)
        {
            RET(0);
            return;
        }
        snprintf(name, sizeof name, "\\\\.\\DISPLAY%u", index + 1u);
        rt_put_afield(dd + 4, 32, name);
        if (!rt_put_afield(dd + 36, 128, rt_gpu_name())) rt_put_afield(dd + 36, 128, "Apple GPU");
        /* DISPLAY_DEVICE_ATTACHED_TO_DESKTOP, DISPLAY_DEVICE_PRIMARY_DEVICE */
        wr32(dd + 164, 0x1u | (d[index].main ? 0x4u : 0u));
        RET(1);
        return;
    }
    {
        int idx = display_index(dev, n);
        if (idx < 0 || index != 0)
        {
            RET(0);
            return;
        }
        snprintf(name, sizeof name, "\\\\.\\DISPLAY%d\\Monitor0", idx + 1);
        rt_put_afield(dd + 4, 32, name);
        if (!rt_put_afield(dd + 36, 128, d[idx].name)) rt_put_afield(dd + 36, 128, "Display");
        wr32(dd + 164, 0x3u); /* DISPLAY_DEVICE_ACTIVE, DISPLAY_DEVICE_ATTACHED */
        RET(1);
    }
}

#define HMONITOR_TAG 0x4D4F0000u

static int monitor_index(uint32_t h, int n)
{
    if ((h & 0xFFFF0000u) != HMONITOR_TAG) return -1;
    if ((h & 0xFFFFu) == 0 || (int)(h & 0xFFFFu) > n) return -1;
    return (int)(h & 0xFFFFu) - 1;
}

uint32_t rt_user32_main_monitor(void)
{
    rt_display d[RT_MAX_DISPLAYS];
    int n = rt_cocoa_displays(d), i;
    for (i = 0; i < n; i++)
        if (d[i].main) return HMONITOR_TAG | (uint32_t)(i + 1);
    rt_fatal("the display list has no main display");
}

/* HMONITOR MonitorFromWindow(HWND, DWORD flags) */
void rt_MonitorFromWindow(_cpu *cpu)
{
    rt_display d[RT_MAX_DISPLAYS];
    uint32_t hwnd = ARG(0), flags = ARG(1);
    int n = rt_cocoa_displays(d), i, best = -1;
    int64_t best_area = 0;
    rt_rect r;
    if (flags > 2) rt_fatal("MonitorFromWindow: flags 0x%x are not supported", flags);
    if (!rt_user32_is_window(hwnd))
    {
        rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        RET(flags == 1 ? HMONITOR_TAG | 1u : 0u);
        return;
    }
    r = rt_user32_screen_rect(hwnd);
    for (i = 0; i < n; i++)
    {
        int64_t w = (int64_t)(r.right < d[i].frame.right ? r.right : d[i].frame.right) -
                    (r.left > d[i].frame.left ? r.left : d[i].frame.left);
        int64_t h = (int64_t)(r.bottom < d[i].frame.bottom ? r.bottom : d[i].frame.bottom) -
                    (r.top > d[i].frame.top ? r.top : d[i].frame.top);
        if (w > 0 && h > 0 && w * h > best_area)
        {
            best_area = w * h;
            best = i;
        }
    }
    if (best < 0)
    {
        if (flags == 0)
        {
            RET(0);
            return;
        }
        if (flags == 2)
        {
            /* MONITOR_DEFAULTTONEAREST: the display nearest to the window
               center. */
            int64_t best_d = -1;
            int32_t cx = r.left + (r.right - r.left) / 2, cy = r.top + (r.bottom - r.top) / 2;
            for (i = 0; i < n; i++)
            {
                int64_t dx = cx < d[i].frame.left ? d[i].frame.left - cx : cx >= d[i].frame.right ? cx - d[i].frame.right + 1 : 0;
                int64_t dy = cy < d[i].frame.top ? d[i].frame.top - cy : cy >= d[i].frame.bottom ? cy - d[i].frame.bottom + 1 : 0;
                if (best_d < 0 || dx * dx + dy * dy < best_d)
                {
                    best_d = dx * dx + dy * dy;
                    best = i;
                }
            }
        }
        else
            best = 0;
    }
    RET(HMONITOR_TAG | (uint32_t)(best + 1));
}

/* BOOL GetMonitorInfoA(HMONITOR, LPMONITORINFO): cbSize 40
   (MONITORINFO) or 72 (MONITORINFOEXA). */
void rt_GetMonitorInfoA(_cpu *cpu)
{
    rt_display d[RT_MAX_DISPLAYS];
    uint32_t mi = ARG(1), cb;
    int n = rt_cocoa_displays(d), idx = monitor_index(ARG(0), n);
    if (idx < 0 || mi == 0 || ((cb = rd32(mi)) != 40u && cb != 72u))
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    write_rect(mi + 4, d[idx].frame);
    write_rect(mi + 20, d[idx].work);
    wr32(mi + 36, d[idx].main ? 1u : 0u); /* MONITORINFOF_PRIMARY */
    if (cb == 72)
    {
        char name[32];
        snprintf(name, sizeof name, "\\\\.\\DISPLAY%d", idx + 1);
        rt_put_afield(mi + 40, 32, name);
    }
    RET(1);
}

/* BOOL EnumDisplayMonitors(HDC, LPCRECT clip, MONITORENUMPROC, LPARAM) */
void rt_EnumDisplayMonitors(_cpu *cpu)
{
    rt_display d[RT_MAX_DISPLAYS];
    uint32_t clip = ARG(1), proc = ARG(2), data = ARG(3), rect;
    int n = rt_cocoa_displays(d), i;
    rt_rect c = { 0, 0, 0, 0 };
    if (ARG(0) != 0) rt_fatal("EnumDisplayMonitors: a device context is not supported");
    if (proc == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (clip != 0) c = read_rect(clip);
    rect = rt_heap_alloc(16);
    if (rect == 0) rt_fatal("out of guest memory (EnumDisplayMonitors)");
    for (i = 0; i < n; i++)
    {
        rt_rect r = d[i].frame;
        if (clip != 0)
        {
            if (c.left > r.left) r.left = c.left;
            if (c.top > r.top) r.top = c.top;
            if (c.right < r.right) r.right = c.right;
            if (c.bottom < r.bottom) r.bottom = c.bottom;
            if (r.left >= r.right || r.top >= r.bottom) continue;
        }
        write_rect(rect, r);
        if (rt_call_cb(cpu, proc, 4, HMONITOR_TAG | (uint32_t)(i + 1), 0, rect, data) == 0) break;
    }
    rt_heap_free(rect);
    RET(1);
}

/* ---------------------------------------------------------------- clipboard */

#define CF_TEXT 1u
#define CF_OEMTEXT 7u
#define CF_UNICODETEXT 13u

static uint32_t clip_owner_tid; /* the thread that opened the clipboard, 0: closed */
static uint32_t clip_handles[3]; /* the data handles of CF_TEXT, CF_OEMTEXT, CF_UNICODETEXT */

static int clip_slot(uint32_t fmt)
{
    return fmt == CF_TEXT ? 0 : fmt == CF_OEMTEXT ? 1 : fmt == CF_UNICODETEXT ? 2 : -1;
}

/* lock held */
static void clip_free_handles(void)
{
    int i;
    for (i = 0; i < 3; i++)
    {
        if (clip_handles[i] != 0) rt_global_free(clip_handles[i]);
        clip_handles[i] = 0;
    }
}

/* BOOL OpenClipboard(HWND) */
void rt_OpenClipboard(_cpu *cpu)
{
    uint32_t tid = rt_self()->tid;
    if (ARG(0) != 0 && !rt_user32_is_window(ARG(0)))
    {
        rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        RET(0);
        return;
    }
    lock();
    if (clip_owner_tid != 0 && clip_owner_tid != tid)
    {
        unlock();
        rt_set_last_error(ERROR_ACCESS_DENIED);
        RET(0);
        return;
    }
    clip_owner_tid = tid;
    unlock();
    RET(1);
}

/* BOOL CloseClipboard(void) */
void rt_CloseClipboard(_cpu *cpu)
{
    lock();
    if (clip_owner_tid != rt_self()->tid)
    {
        unlock();
        rt_set_last_error(ERROR_CLIPBOARD_NOT_OPEN);
        RET(0);
        return;
    }
    clip_owner_tid = 0;
    clip_free_handles();
    unlock();
    RET(1);
}

/* BOOL EmptyClipboard(void) */
void rt_EmptyClipboard(_cpu *cpu)
{
    lock();
    if (clip_owner_tid != rt_self()->tid)
    {
        unlock();
        rt_set_last_error(ERROR_CLIPBOARD_NOT_OPEN);
        RET(0);
        return;
    }
    clip_free_handles();
    unlock();
    rt_cocoa_clipboard_set_text(NULL);
    RET(1);
}

/* HANDLE SetClipboardData(UINT format, HANDLE): the clipboard owns the
   handle. */
void rt_SetClipboardData(_cpu *cpu)
{
    uint32_t fmt = ARG(0), h = ARG(1), p, size, n;
    int slot = clip_slot(fmt);
    char *text;
    if (slot < 0) rt_fatal("SetClipboardData: clipboard format %u is not supported (text formats are)", fmt);
    if (h == 0) rt_fatal("SetClipboardData: delayed rendering (a NULL handle) is not supported");
    lock();
    if (clip_owner_tid != rt_self()->tid)
    {
        unlock();
        rt_set_last_error(ERROR_CLIPBOARD_NOT_OPEN);
        RET(0);
        return;
    }
    unlock();
    p = rt_global_lock(h);
    size = rt_global_size(h);
    if (p == 0)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    if (fmt == CF_UNICODETEXT)
    {
        for (n = 0; 2u * n + 1u < size && rd16(p + 2u * n) != 0; n++) {}
        text = malloc(3u * (size_t)n + 1u);
        if (text == NULL) rt_fatal("out of host memory (clipboard)");
        if (rt_gstrn_w(text, 3u * (size_t)n + 1u, p, n) != RT_STR_OK)
        {
            rt_global_unlock(h);
            free(text);
            rt_set_last_error(ERROR_INVALID_DATA);
            RET(0);
            return;
        }
    }
    else
    {
        size_t o = 0;
        for (n = 0; n < size && rd8(p + n) != 0; n++) {}
        text = malloc(3u * (size_t)n + 1u);
        if (text == NULL) rt_fatal("out of host memory (clipboard)");
        for (uint32_t i = 0; i < n; i++)
        {
            uint8_t c = rd8(p + i);
            uint32_t cp = fmt == CF_OEMTEXT ? rt_cp437_to_unicode(c) : rt_cp1252_to_unicode(c);
            o += rt_utf8_encode(cp, text + o);
        }
        text[o] = 0;
    }
    rt_global_unlock(h);
    rt_cocoa_clipboard_set_text(text);
    free(text);
    lock();
    if (clip_handles[slot] != 0 && clip_handles[slot] != h) rt_global_free(clip_handles[slot]);
    clip_handles[slot] = h;
    unlock();
    RET(h);
}

/* HANDLE GetClipboardData(UINT format): a text format of the pasteboard.
   The handle is valid until the clipboard is closed. */
void rt_GetClipboardData(_cpu *cpu)
{
    uint32_t fmt = ARG(0), h, p;
    int slot = clip_slot(fmt);
    char *text;
    if (slot < 0) rt_fatal("GetClipboardData: clipboard format %u is not supported (text formats are)", fmt);
    lock();
    if (clip_owner_tid != rt_self()->tid)
    {
        unlock();
        rt_set_last_error(ERROR_CLIPBOARD_NOT_OPEN);
        RET(0);
        return;
    }
    unlock();
    text = rt_cocoa_clipboard_text();
    if (text == NULL)
    {
        RET(0);
        return;
    }
    {
        size_t units;
        uint32_t cap;
        if (!rt_utf16_length(text, &units) || units > 0x3FFFFFFu) rt_fatal("GetClipboardData: the pasteboard text is too long");
        cap = fmt == CF_UNICODETEXT ? 2u * ((uint32_t)units + 1u) : (uint32_t)units + 1u;
        h = rt_global_alloc_movable(cap, true);
        if (h == 0)
        {
            free(text);
            rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
            RET(0);
            return;
        }
        p = rt_global_lock(h);
        if (fmt == CF_UNICODETEXT)
            rt_put_wstr(p, (uint32_t)units + 1u, text);
        else if (fmt == CF_TEXT)
            rt_put_astr(p, (uint32_t)units + 1u, text);
        else
        {
            /* CF_OEMTEXT: code page 437. */
            size_t i = 0, len = strlen(text);
            uint32_t o = 0;
            while (i < len)
            {
                uint32_t cp;
                int k = rt_utf8_decode((const uint8_t *)text + i, len - i, &cp);
                int c;
                if (k <= 0) rt_fatal("GetClipboardData: the pasteboard text is not valid UTF-8");
                i += (size_t)k;
                c = rt_unicode_to_cp437(cp);
                wr8(p + o++, (uint8_t)(c < 0 ? '?' : c));
            }
            wr8(p + o, 0);
        }
        rt_global_unlock(h);
    }
    free(text);
    lock();
    if (clip_handles[slot] != 0) rt_global_free(clip_handles[slot]);
    clip_handles[slot] = h;
    unlock();
    RET(h);
}

/* BOOL IsClipboardFormatAvailable(UINT format) */
void rt_IsClipboardFormatAvailable(_cpu *cpu)
{
    uint32_t fmt = ARG(0);
    if (clip_slot(fmt) < 0) rt_fatal("IsClipboardFormatAvailable: clipboard format %u is not supported (text formats are)", fmt);
    RET(rt_cocoa_clipboard_has_text() ? 1u : 0u);
}

/* ---------------------------------------------------------------- window station and desktop */

#define WINSTA_HANDLE 0x7FFF0010u
#define DESKTOP_HANDLE 0x7FFF0014u

/* HWINSTA GetProcessWindowStation(void) */
void rt_GetProcessWindowStation(_cpu *cpu)
{
    RET(WINSTA_HANDLE);
}

/* HDESK GetThreadDesktop(DWORD tid) */
void rt_GetThreadDesktop(_cpu *cpu)
{
    if (ARG(0) == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    RET(DESKTOP_HANDLE);
}

static void user_object_info(_cpu *cpu, bool wide)
{
    uint32_t h = ARG(0), index = ARG(1), buf = ARG(2), len = ARG(3), pneed = ARG(4), need;
    bool winsta = h == WINSTA_HANDLE;
    const char *s = NULL;
    if (h != WINSTA_HANDLE && h != DESKTOP_HANDLE)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    switch (index)
    {
    case 1: /* UOI_FLAGS: USEROBJECTFLAGS { fInherit, fReserved, dwFlags } */
        need = 12;
        if (pneed != 0) wr32(pneed, need);
        if (len < need || buf == 0)
        {
            rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
            RET(0);
            return;
        }
        wr32(buf, 0);
        wr32(buf + 4, 0);
        wr32(buf + 8, winsta ? 1u : 0u); /* WSF_VISIBLE: the window station has a display */
        RET(1);
        return;
    case 2: s = winsta ? "WinSta0" : "Default"; break;         /* UOI_NAME */
    case 3: s = winsta ? "WindowStation" : "Desktop"; break;   /* UOI_TYPE */
    case 6: /* UOI_IO: the desktop gets the input */
        need = 4;
        if (pneed != 0) wr32(pneed, need);
        if (len < need || buf == 0)
        {
            rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
            RET(0);
            return;
        }
        wr32(buf, 1);
        RET(1);
        return;
    default:
        rt_fatal("GetUserObjectInformation%c: index %u is not supported", wide ? 'W' : 'A', index);
    }
    need = (uint32_t)(strlen(s) + 1u) * (wide ? 2u : 1u);
    if (pneed != 0) wr32(pneed, need);
    if (len < need || buf == 0)
    {
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        RET(0);
        return;
    }
    if (wide)
        rt_put_wstr(buf, len / 2u, s);
    else
        rt_put_astr(buf, len, s);
    RET(1);
}

/* BOOL GetUserObjectInformationA(HANDLE, int index, PVOID, DWORD, LPDWORD) */
void rt_GetUserObjectInformationA(_cpu *cpu) { user_object_info(cpu, false); }
/* BOOL GetUserObjectInformationW(HANDLE, int index, PVOID, DWORD, LPDWORD) */
void rt_GetUserObjectInformationW(_cpu *cpu) { user_object_info(cpu, true); }

/* ---------------------------------------------------------------- wsprintfA */

#define WSPRINTF_MAX 1024u /* the wsprintf output limit with the NUL */

typedef struct fmt_out {
    char buf[WSPRINTF_MAX];
    uint32_t n;
} fmt_out;

static void out_ch(fmt_out *o, char c)
{
    if (o->n + 1u < WSPRINTF_MAX) o->buf[o->n++] = c;
}

static void out_pad(fmt_out *o, char c, int32_t n)
{
    while (n-- > 0) out_ch(o, c);
}

/* Format with the rules of wsprintfA (as the Wine project documents
   them): %c %C %d %i %u %x %X %s %S %p %% with the flags '-' '#' '0',
   a width, a precision, and the size prefixes h, l and I64. args: the
   address of the first argument (32-bit slots). */
static uint32_t wsprintf_a(uint32_t dst, uint32_t fmt, uint32_t args)
{
    fmt_out o;
    uint32_t f = fmt;
    o.n = 0;
    for (;;)
    {
        char c = (char)rd8(f++);
        bool left = false, alt = false, zero = false, wide_s = false, ansi_s = false, i64 = false;
        int32_t width = 0, prec = -1;
        char num[32], *np;
        uint64_t v;
        int len, i;
        if (c == 0) break;
        if (c != '%')
        {
            out_ch(&o, c);
            continue;
        }
        c = (char)rd8(f++);
        if (c == '%')
        {
            out_ch(&o, '%');
            continue;
        }
        if (c == '-') { left = true; c = (char)rd8(f++); }
        if (c == '#') { alt = true; c = (char)rd8(f++); }
        if (c == '0') { zero = true; c = (char)rd8(f++); }
        while (c >= '0' && c <= '9')
        {
            if (width < 10000) width = width * 10 + (c - '0');
            c = (char)rd8(f++);
        }
        if (c == '.')
        {
            prec = 0;
            c = (char)rd8(f++);
            while (c >= '0' && c <= '9')
            {
                if (prec < 10000) prec = prec * 10 + (c - '0');
                c = (char)rd8(f++);
            }
        }
        if (c == 'l') { wide_s = true; c = (char)rd8(f++); }
        else if (c == 'h') { ansi_s = true; c = (char)rd8(f++); }
        else if (c == 'I' && rd8(f) == '6' && rd8(f + 1) == '4') { i64 = true; f += 2; c = (char)rd8(f++); }
        switch (c)
        {
        case 'c': case 'C':
        {
            bool w = c == 'C' ? !ansi_s : wide_s;
            uint32_t ch = rd32(args);
            int b;
            args += 4;
            if (w)
            {
                b = rt_unicode_to_cp1252(ch & 0xFFFFu);
                ch = (uint32_t)(b < 0 ? '?' : b);
            }
            if (!left) out_pad(&o, zero ? '0' : ' ', width - 1);
            out_ch(&o, (char)(ch & 0xFFu));
            if (left) out_pad(&o, ' ', width - 1);
            continue;
        }
        case 's': case 'S':
        {
            bool w = c == 'S' ? !ansi_s : wide_s;
            uint32_t s = rd32(args), slen = 0;
            char tmp[WSPRINTF_MAX];
            uint32_t tn = 0;
            args += 4;
            if (s == 0)
            {
                memcpy(tmp, "(null)", 6);
                tn = 6;
            }
            else
            {
                for (;;)
                {
                    uint32_t ch = w ? rd16(s + 2u * slen) : rd8(s + slen);
                    if (ch == 0 || (prec >= 0 && (int32_t)slen >= prec) || tn + 1u >= WSPRINTF_MAX) break;
                    if (w)
                    {
                        int b = rt_unicode_to_cp1252(ch);
                        if (ch >= 0xD800u && ch < 0xDC00u && rd16(s + 2u * slen + 2u) >= 0xDC00u &&
                            rd16(s + 2u * slen + 2u) < 0xE000u)
                            slen++; /* one '?' for a surrogate pair */
                        ch = (uint32_t)(b < 0 ? '?' : b);
                    }
                    tmp[tn++] = (char)ch;
                    slen++;
                }
            }
            if (prec >= 0 && tn > (uint32_t)prec) tn = (uint32_t)prec;
            if (!left) out_pad(&o, zero ? '0' : ' ', width - (int32_t)tn);
            for (i = 0; i < (int)tn; i++) out_ch(&o, tmp[i]);
            if (left) out_pad(&o, ' ', width - (int32_t)tn);
            continue;
        }
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'p':
        {
            bool neg = false, hex = c == 'x' || c == 'X' || c == 'p', upper = c == 'X' || c == 'p';
            const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
            int ndig = 0, prefix = 0, total;
            if (c == 'p')
            {
                width = 8;
                zero = true;
                left = false;
                prec = -1;
            }
            if (i64)
            {
                v = (uint64_t)rd32(args) | ((uint64_t)rd32(args + 4) << 32);
                args += 8;
                if ((c == 'd' || c == 'i') && (int64_t)v < 0)
                {
                    neg = true;
                    v = (uint64_t)(-(int64_t)v);
                }
            }
            else
            {
                uint32_t a = rd32(args);
                args += 4;
                if (ansi_s) a &= 0xFFFFu;
                if ((c == 'd' || c == 'i') && (ansi_s ? (int16_t)a < 0 : (int32_t)a < 0))
                {
                    neg = true;
                    a = ansi_s ? (uint32_t)(-(int32_t)(int16_t)a) : (uint32_t)(-(int64_t)(int32_t)a);
                }
                v = a;
            }
            np = num + sizeof num;
            do
            {
                *--np = digits[hex ? v % 16u : v % 10u];
                v = hex ? v / 16u : v / 10u;
                ndig++;
            } while (v != 0);
            if (prec == 0 && ndig == 1 && *np == '0') ndig = 0;
            if (neg) prefix = 1;
            if (alt && hex) prefix = 2;
            total = prefix + (prec > ndig ? prec : ndig);
            if (!left && !(zero && prec < 0)) out_pad(&o, ' ', width - total);
            if (neg) out_ch(&o, '-');
            if (alt && hex)
            {
                out_ch(&o, '0');
                out_ch(&o, upper ? 'X' : 'x');
            }
            if (!left && zero && prec < 0) out_pad(&o, '0', width - total);
            out_pad(&o, '0', prec - ndig);
            for (len = 0; len < ndig; len++) out_ch(&o, np[len]);
            if (left) out_pad(&o, ' ', width - total);
            continue;
        }
        case 0:
            goto done;
        default:
            /* An unknown type: wsprintf writes the character. */
            out_ch(&o, c);
            continue;
        }
    }
done:
    o.buf[o.n] = 0;
    for (uint32_t k = 0; k <= o.n; k++) wr8(dst + k, (uint8_t)o.buf[k]);
    return o.n;
}

/* int wsprintfA(LPSTR, LPCSTR, ...) (cdecl) */
void rt_wsprintfA(_cpu *cpu)
{
    if (ARG(0) == 0 || ARG(1) == 0) rt_fatal("wsprintfA with a NULL buffer or format");
    RET(wsprintf_a(ARG(0), ARG(1), ARGADR(2)));
}

/* int wvsprintfA(LPSTR, LPCSTR, va_list) */
void rt_wvsprintfA(_cpu *cpu)
{
    if (ARG(0) == 0 || ARG(1) == 0) rt_fatal("wvsprintfA with a NULL buffer or format");
    RET(wsprintf_a(ARG(0), ARG(1), ARG(2)));
}

/* ---------------------------------------------------------------- device notifications */

#define MAX_NOTIFY 64
#define NOTIFY_TAG 0x44450000u
#define DBT_DEVTYP_DEVICEINTERFACE 5u
#define DEVICE_NOTIFY_ALL_INTERFACE_CLASSES 4u
#define WM_DEVICECHANGE_MSG 0x0219u
#define DBT_DEVNODES_CHANGED 0x0007u
#define DBT_DEVICEARRIVAL 0x8000u
#define DBT_DEVICEREMOVECOMPLETE 0x8004u

typedef struct notify_reg {
    bool used;
    uint32_t hwnd;
    bool all;
    uint8_t guid[16];
} notify_reg;

static notify_reg notifies[MAX_NOTIFY];

/* HDEVNOTIFY RegisterDeviceNotificationW(HANDLE recipient, LPVOID filter,
   DWORD flags) */
void rt_RegisterDeviceNotificationW(_cpu *cpu)
{
    uint32_t hwnd = ARG(0), filter = ARG(1), flags = ARG(2), i;
    if (flags & 1u) rt_fatal("RegisterDeviceNotificationW: service handles are not supported");
    if (flags & ~(1u | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES))
        rt_fatal("RegisterDeviceNotificationW: flags 0x%x are not supported", flags);
    if (!rt_user32_is_window(hwnd))
    {
        rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        RET(0);
        return;
    }
    if (filter == 0 || rd32(filter) < 28u)
    {
        rt_set_last_error(ERROR_INVALID_DATA);
        RET(0);
        return;
    }
    if (rd32(filter + 4) != DBT_DEVTYP_DEVICEINTERFACE)
        rt_fatal("RegisterDeviceNotificationW: device type %u is not supported (device interfaces are)", rd32(filter + 4));
    lock();
    for (i = 0; i < MAX_NOTIFY; i++)
    {
        if (notifies[i].used) continue;
        notifies[i].used = true;
        notifies[i].hwnd = hwnd;
        notifies[i].all = (flags & DEVICE_NOTIFY_ALL_INTERFACE_CLASSES) != 0;
        memcpy(notifies[i].guid, G2H(filter + 12), 16);
        unlock();
        RET(NOTIFY_TAG | (i + 1u));
        return;
    }
    unlock();
    rt_fatal("RegisterDeviceNotificationW: more than %d registrations", MAX_NOTIFY);
}

/* BOOL UnregisterDeviceNotification(HDEVNOTIFY) */
void rt_UnregisterDeviceNotification(_cpu *cpu)
{
    uint32_t h = ARG(0), i = (h & 0xFFFFu) - 1u;
    lock();
    if ((h & 0xFFFF0000u) != NOTIFY_TAG || i >= MAX_NOTIFY || !notifies[i].used)
    {
        unlock();
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    notifies[i].used = false;
    unlock();
    RET(1);
}

typedef struct device_event {
    bool arrival;
    uint8_t guid[16];
    char *name; /* UTF-8 device interface path */
} device_event;

static void device_event_main(_cpu *cpu, void *ctx)
{
    device_event *e = ctx;
    uint32_t list[MAX_NOTIFY], n = 0, i, dbcc, size;
    size_t units;
    lock();
    for (i = 0; i < MAX_NOTIFY; i++)
        if (notifies[i].used && (notifies[i].all || memcmp(notifies[i].guid, e->guid, 16) == 0)) list[n++] = notifies[i].hwnd;
    unlock();
    /* The devices changed: all top-level windows. */
    rt_user32_send(cpu, 0xFFFFu, WM_DEVICECHANGE_MSG, DBT_DEVNODES_CHANGED, 0, true);
    if (!rt_utf16_length(e->name, &units) || units > 1024) rt_fatal("USER32: a device interface name is not valid");
    /* DEV_BROADCAST_DEVICEINTERFACE_W: dbcc_size, dbcc_devicetype,
       dbcc_reserved, dbcc_classguid, dbcc_name. */
    size = 28u + 2u * ((uint32_t)units + 1u);
    dbcc = rt_heap_calloc(1, size);
    if (dbcc == 0) rt_fatal("out of guest memory (WM_DEVICECHANGE)");
    wr32(dbcc, size);
    wr32(dbcc + 4, DBT_DEVTYP_DEVICEINTERFACE);
    memcpy(G2H(dbcc + 12), e->guid, 16);
    rt_put_wstr(dbcc + 28, (uint32_t)units + 1u, e->name);
    for (i = 0; i < n; i++)
        if (rt_user32_is_window(list[i]))
            rt_user32_send(cpu, list[i], WM_DEVICECHANGE_MSG, e->arrival ? DBT_DEVICEARRIVAL : DBT_DEVICEREMOVECOMPLETE, dbcc, true);
    rt_heap_free(dbcc);
    free(e->name);
    free(e);
}

/* A device interface arrived or was removed (HID, rt_hid.c). Any thread:
   the windows get WM_DEVICECHANGE at the next message check of the main
   thread. */
void rt_user32_device_interface_event(bool arrival, const uint8_t guid[16], const char *name)
{
    device_event *e = calloc(1, sizeof *e);
    if (e == NULL) rt_fatal("out of host memory (device event)");
    e->arrival = arrival;
    memcpy(e->guid, guid, 16);
    e->name = strdup(name);
    if (e->name == NULL) rt_fatal("out of host memory (device event)");
    rt_user32_main_call(device_event_main, e);
}
