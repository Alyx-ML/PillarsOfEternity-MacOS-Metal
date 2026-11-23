/*
 * Raw input (WM_INPUT) and the raw input device list.
 *
 * Devices: one system mouse and one system keyboard (the macOS pointer
 * and keyboard events), and the HID game controllers (rt_hid.c).
 *
 * Mouse: the relative motion comes from the mouse events of macOS (local
 * and global event monitors, rt_window.m). It is the motion of the macOS
 * cursor in points, with the pointer acceleration of the system; Windows
 * gives the motion of the device without acceleration. The fractions of
 * a point are kept for the next event.
 *
 * Each WM_INPUT message has a raw input record (HRAWINPUT) in a ring of
 * RECORDS records; GetRawInputData of a record that the ring replaced
 * fails with ERROR_INVALID_HANDLE.
 */
#include "rt.h"
#include "rt_hid.h"
#include "rt_thread.h"
#include "rt_user32.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define ERROR_INVALID_WINDOW_HANDLE 1400u

#define RIM_TYPEMOUSE 0u
#define RIM_TYPEKEYBOARD 1u
#define RIM_TYPEHID 2u

#define RIM_INPUT 0u
#define RIM_INPUTSINK 1u

#define RIDEV_REMOVE 0x00000001u
#define RIDEV_INPUTSINK 0x00000100u
#define RIDEV_DEVNOTIFY 0x00002000u

#define RID_INPUT 0x10000003u
#define RID_HEADER 0x10000005u

#define RIDI_PREPARSEDDATA 0x20000005u
#define RIDI_DEVICENAME 0x20000007u
#define RIDI_DEVICEINFO 0x2000000Bu

#define GIDC_ARRIVAL 1u
#define GIDC_REMOVAL 2u

#define RI_KEY_BREAK 0x0001u
#define RI_KEY_E0 0x0002u

#define RAW_MOUSE 0x00010041u
#define RAW_KEYBOARD 0x00010043u

#define HEADER_SIZE 16u
#define RECORDS 1024u
#define MAX_REGISTERED 32

static const char mouse_name[] = "\\\\?\\Root#RTMOUSE#0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}";
static const char keyboard_name[] = "\\\\?\\Root#RTKEYBOARD#0000#{884b96c3-56ef-11d1-bc8c-00a0c91405dd}";

typedef struct reg {
    uint16_t page, usage;
    uint32_t flags, hwnd;
} reg;

typedef struct record {
    uint32_t seq; /* 0: free */
    uint32_t type, device, wparam;
    uint32_t size; /* the data after the header */
    uint8_t *data;
} record;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static reg g_regs[MAX_REGISTERED];
static int g_nregs;
static record g_records[RECORDS];
static uint32_t g_seq;
static double g_frac_x, g_frac_y;

static void rlock(void)
{
    if (pthread_mutex_lock(&g_lock) != 0) rt_fatal("raw input: lock failed");
}

static void runlock(void)
{
    if (pthread_mutex_unlock(&g_lock) != 0) rt_fatal("raw input: unlock failed");
}

static int find_reg(uint16_t page, uint16_t usage) /* lock held */
{
    int i;
    for (i = 0; i < g_nregs; i++)
        if (g_regs[i].page == page && g_regs[i].usage == usage) return i;
    return -1;
}

/* The window for input of a registration (lock held); 0: no message. */
static uint32_t target_of(const reg *r, uint32_t *wparam)
{
    bool fg = rt_user32_app_is_active();
    if (!fg && !(r->flags & RIDEV_INPUTSINK)) return 0;
    *wparam = fg ? RIM_INPUT : RIM_INPUTSINK;
    if (r->hwnd != 0) return r->hwnd;
    return fg ? rt_user32_raw_target() : 0;
}

/* Add a record and post WM_INPUT (lock held; the data is copied). */
static void post_record(uint32_t hwnd, uint32_t type, uint32_t device, uint32_t wparam, const uint8_t *data, uint32_t size)
{
    uint32_t seq, h;
    record *r;
    if (++g_seq > 0x3FFFFFFFu) g_seq = 1;
    seq = g_seq;
    r = &g_records[seq % RECORDS];
    free(r->data);
    r->data = malloc(size > 0 ? size : 1);
    if (r->data == NULL) rt_fatal("out of host memory (raw input)");
    memcpy(r->data, data, size);
    r->seq = seq;
    r->type = type;
    r->device = device;
    r->wparam = wparam;
    r->size = size;
    h = 0x40000000u | seq;
    rt_user32_post_raw(hwnd, wparam, h);
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

void rt_rawinput_mouse(double dx, double dy, uint32_t buttons, int32_t wheel)
{
    uint8_t m[24];
    uint32_t hwnd, wp = 0;
    double ix, iy;
    int i;
    rlock();
    i = find_reg(1, 2);
    if (i < 0)
    {
        runlock();
        return;
    }
    hwnd = target_of(&g_regs[i], &wp);
    g_frac_x += dx;
    g_frac_y += dy;
    ix = trunc(g_frac_x);
    iy = trunc(g_frac_y);
    if (hwnd == 0 || (ix == 0.0 && iy == 0.0 && buttons == 0))
    {
        runlock();
        return;
    }
    g_frac_x -= trunc(g_frac_x);
    g_frac_y -= trunc(g_frac_y);
    memset(m, 0, sizeof m);
    put16(m + 0, 0); /* MOUSE_MOVE_RELATIVE */
    put16(m + 4, buttons & 0xFFFFu);
    put16(m + 6, (buttons & 0x0C00u) ? (uint32_t)(uint16_t)(int16_t)wheel : 0u);
    put32(m + 12, (uint32_t)(int32_t)ix);
    put32(m + 16, (uint32_t)(int32_t)iy);
    post_record(hwnd, RIM_TYPEMOUSE, RAW_MOUSE, wp, m, sizeof m);
    runlock();
}

void rt_rawinput_key(uint32_t vk, uint32_t scan, bool ext, bool down)
{
    uint8_t k[16];
    uint32_t hwnd, wp = 0;
    int i;
    rlock();
    i = find_reg(1, 6);
    if (i < 0)
    {
        runlock();
        return;
    }
    hwnd = target_of(&g_regs[i], &wp);
    if (hwnd == 0)
    {
        runlock();
        return;
    }
    memset(k, 0, sizeof k);
    put16(k + 0, scan & 0xFFu);
    put16(k + 2, (down ? 0u : RI_KEY_BREAK) | (ext ? RI_KEY_E0 : 0u));
    put16(k + 6, vk);
    put32(k + 8, down ? WM_KEYDOWN : WM_KEYUP);
    post_record(hwnd, RIM_TYPEKEYBOARD, RAW_KEYBOARD, wp, k, sizeof k);
    runlock();
}

void rt_rawinput_hid_report(uint32_t raw_handle, uint16_t page, uint16_t usage, const uint8_t *report, uint32_t len)
{
    uint8_t buf[8 + 4097];
    uint32_t hwnd, wp = 0;
    int i;
    if (len > 4097) rt_fatal("raw input: a HID report of %u bytes", len);
    rlock();
    i = find_reg(page, usage);
    if (i < 0)
    {
        runlock();
        return;
    }
    hwnd = target_of(&g_regs[i], &wp);
    if (hwnd == 0)
    {
        runlock();
        return;
    }
    put32(buf, len);
    put32(buf + 4, 1);
    memcpy(buf + 8, report, len);
    post_record(hwnd, RIM_TYPEHID, raw_handle, wp, buf, 8u + len);
    runlock();
}

void rt_rawinput_device_change(uint32_t raw_handle, uint16_t page, uint16_t usage, bool arrival)
{
    uint32_t hwnd = 0;
    int i;
    rlock();
    i = find_reg(page, usage);
    if (i >= 0 && (g_regs[i].flags & RIDEV_DEVNOTIFY)) hwnd = g_regs[i].hwnd != 0 ? g_regs[i].hwnd : rt_user32_raw_target();
    runlock();
    if (hwnd != 0) rt_user32_post_message(hwnd, WM_INPUT_DEVICE_CHANGE, arrival ? GIDC_ARRIVAL : GIDC_REMOVAL, raw_handle);
}

/* ---------------------------------------------------------------- functions */

/* UINT GetRawInputData(HRAWINPUT, UINT command, LPVOID data, PUINT size,
   UINT header_size) */
void rt_GetRawInputData(_cpu *cpu)
{
    uint32_t h = ARG(0), cmd = ARG(1), data = ARG(2), psize = ARG(3), hs = ARG(4), seq, size, i;
    record *r;
    if (hs != HEADER_SIZE || psize == 0 || (cmd != RID_INPUT && cmd != RID_HEADER))
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0xFFFFFFFFu);
        return;
    }
    rlock();
    seq = h & 0x3FFFFFFFu;
    r = &g_records[seq % RECORDS];
    if ((h & 0xC0000000u) != 0x40000000u || seq == 0 || r->seq != seq)
    {
        runlock();
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0xFFFFFFFFu);
        return;
    }
    size = cmd == RID_HEADER ? HEADER_SIZE : HEADER_SIZE + r->size;
    if (data == 0)
    {
        runlock();
        wr32(psize, size);
        RET(0);
        return;
    }
    if (rd32(psize) < size)
    {
        runlock();
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        RET(0xFFFFFFFFu);
        return;
    }
    wr32(data, r->type);
    wr32(data + 4, HEADER_SIZE + r->size);
    wr32(data + 8, r->device);
    wr32(data + 12, r->wparam);
    if (cmd == RID_INPUT)
        for (i = 0; i < r->size; i++) wr8(data + HEADER_SIZE + i, r->data[i]);
    runlock();
    RET(size);
}

/* UINT GetRawInputDeviceList(PRAWINPUTDEVICELIST, PUINT count, UINT size) */
void rt_GetRawInputDeviceList(_cpu *cpu)
{
    rt_hid_info hids[RT_HID_MAX_DEVICES];
    uint32_t list = ARG(0), pcount = ARG(1), n, i;
    int nh;
    if (ARG(2) != 8 || pcount == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0xFFFFFFFFu);
        return;
    }
    nh = rt_hid_devices(hids, RT_HID_MAX_DEVICES);
    n = 2u + (uint32_t)nh;
    if (list == 0)
    {
        wr32(pcount, n);
        RET(0);
        return;
    }
    if (rd32(pcount) < n)
    {
        wr32(pcount, n);
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        RET(0xFFFFFFFFu);
        return;
    }
    wr32(list, RAW_MOUSE);
    wr32(list + 4, RIM_TYPEMOUSE);
    wr32(list + 8, RAW_KEYBOARD);
    wr32(list + 12, RIM_TYPEKEYBOARD);
    for (i = 0; i < (uint32_t)nh; i++)
    {
        wr32(list + 16u + 8u * i, hids[i].raw_handle);
        wr32(list + 20u + 8u * i, RIM_TYPEHID);
    }
    RET(n);
}

/* UINT GetRawInputDeviceInfoW(HANDLE device, UINT command, LPVOID data,
   PUINT size) */
void rt_GetRawInputDeviceInfoW(_cpu *cpu)
{
    uint32_t dev = ARG(0), cmd = ARG(1), data = ARG(2), psize = ARG(3), need, cap, i;
    rt_hid_info hi;
    bool is_hid = false;
    const char *name;
    if (psize == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0xFFFFFFFFu);
        return;
    }
    if (dev != RAW_MOUSE && dev != RAW_KEYBOARD)
    {
        if (!rt_hid_device(dev, &hi))
        {
            rt_set_last_error(ERROR_INVALID_HANDLE);
            RET(0xFFFFFFFFu);
            return;
        }
        is_hid = true;
    }
    cap = rd32(psize);
    switch (cmd)
    {
    case RIDI_DEVICENAME:
        /* The size is in characters, with the NUL. */
        name = is_hid ? hi.name : dev == RAW_MOUSE ? mouse_name : keyboard_name;
        need = (uint32_t)strlen(name) + 1u;
        if (data == 0)
        {
            wr32(psize, need);
            RET(0);
            return;
        }
        if (cap < need)
        {
            wr32(psize, need);
            rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
            RET(0xFFFFFFFFu);
            return;
        }
        for (i = 0; i < need; i++) wr16(data + 2u * i, (uint8_t)name[i]);
        RET(need);
        return;
    case RIDI_DEVICEINFO:
        need = 32;
        if (data == 0)
        {
            wr32(psize, need);
            RET(0);
            return;
        }
        if (cap < need)
        {
            wr32(psize, need);
            rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
            RET(0xFFFFFFFFu);
            return;
        }
        for (i = 0; i < 32; i += 4) wr32(data + i, 0);
        wr32(data, 32);
        if (is_hid)
        {
            wr32(data + 4, RIM_TYPEHID);
            wr32(data + 8, hi.vendor);
            wr32(data + 12, hi.product);
            wr32(data + 16, hi.version);
            wr16(data + 20, hi.page);
            wr16(data + 22, hi.usage);
        }
        else if (dev == RAW_MOUSE)
        {
            wr32(data + 4, RIM_TYPEMOUSE);
            wr32(data + 8, 0x100);  /* dwId: a mouse with a wheel */
            wr32(data + 12, 5);     /* buttons: left, right, middle, X1, X2 */
            wr32(data + 16, 0);     /* sample rate: not known for a USB mouse */
            wr32(data + 20, 1);     /* horizontal wheel */
        }
        else
        {
            wr32(data + 4, RIM_TYPEKEYBOARD);
            wr32(data + 8, 0x51);   /* the HID keyboard type */
            wr32(data + 12, 0);
            wr32(data + 16, 1);     /* scan code set 1 */
            wr32(data + 20, 12);    /* function keys */
            wr32(data + 24, 3);     /* indicators */
            wr32(data + 28, 101);   /* keys */
        }
        RET(need);
        return;
    case RIDI_PREPARSEDDATA:
    {
        uint8_t *tmp;
        if (!is_hid)
        {
            wr32(psize, 0);
            RET(0);
            return;
        }
        need = rt_hid_preparsed(dev, NULL, 0);
        if (need == 0)
        {
            rt_set_last_error(ERROR_INVALID_HANDLE);
            RET(0xFFFFFFFFu);
            return;
        }
        if (data == 0)
        {
            wr32(psize, need);
            RET(0);
            return;
        }
        if (cap < need)
        {
            wr32(psize, need);
            rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
            RET(0xFFFFFFFFu);
            return;
        }
        tmp = malloc(need);
        if (tmp == NULL) rt_fatal("out of host memory (raw input)");
        need = rt_hid_preparsed(dev, tmp, need);
        for (i = 0; i < need; i++) wr8(data + i, tmp[i]);
        free(tmp);
        RET(need);
        return;
    }
    default:
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0xFFFFFFFFu);
        return;
    }
}

/* BOOL RegisterRawInputDevices(PCRAWINPUTDEVICE, UINT count, UINT size) */
void rt_RegisterRawInputDevices(_cpu *cpu)
{
    uint32_t list = ARG(0), n = ARG(1), i;
    reg add[MAX_REGISTERED];
    bool hid = false;
    if (ARG(2) != 12 || (n != 0 && list == 0) || n > MAX_REGISTERED)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    for (i = 0; i < n; i++)
    {
        uint32_t e = list + 12u * i;
        reg r = { rd16(e), rd16(e + 2), rd32(e + 4), rd32(e + 8) };
        if (r.flags & ~(RIDEV_REMOVE | RIDEV_INPUTSINK | RIDEV_DEVNOTIFY))
            rt_fatal("RegisterRawInputDevices: flags 0x%08x are not supported (usage %04x:%04x)", r.flags, r.page, r.usage);
        if (r.page == 0 || ((r.flags & RIDEV_REMOVE) && r.hwnd != 0) || ((r.flags & RIDEV_INPUTSINK) && r.hwnd == 0))
        {
            rt_set_last_error(ERROR_INVALID_PARAMETER);
            RET(0);
            return;
        }
        if (r.hwnd != 0 && !rt_user32_is_window(r.hwnd))
        {
            rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
            RET(0);
            return;
        }
        if (!(r.page == 1 && (r.usage == 2 || r.usage == 6))) hid = true;
        add[i] = r;
    }
    if (hid) (void)rt_hid_devices(NULL, 0); /* start the HID thread */
    rlock();
    for (i = 0; i < n; i++)
    {
        int k = find_reg(add[i].page, add[i].usage);
        if (add[i].flags & RIDEV_REMOVE)
        {
            if (k >= 0) g_regs[k] = g_regs[--g_nregs];
            continue;
        }
        if (k < 0)
        {
            if (g_nregs >= MAX_REGISTERED)
            {
                runlock();
                rt_fatal("RegisterRawInputDevices: more than %d registered usages", MAX_REGISTERED);
            }
            k = g_nregs++;
        }
        g_regs[k] = add[i];
        if (add[i].page == 1 && add[i].usage == 2) g_frac_x = g_frac_y = 0.0;
    }
    runlock();
    /* RIDEV_DEVNOTIFY: an arrival message for each present device. */
    for (i = 0; i < n; i++)
    {
        rt_hid_info hids[RT_HID_MAX_DEVICES];
        int nh, k;
        if (!(add[i].flags & RIDEV_DEVNOTIFY) || (add[i].flags & RIDEV_REMOVE)) continue;
        if (add[i].page == 1 && add[i].usage == 2)
            rt_rawinput_device_change(RAW_MOUSE, 1, 2, true);
        else if (add[i].page == 1 && add[i].usage == 6)
            rt_rawinput_device_change(RAW_KEYBOARD, 1, 6, true);
        else
        {
            nh = rt_hid_devices(hids, RT_HID_MAX_DEVICES);
            for (k = 0; k < nh; k++)
                if (hids[k].page == add[i].page && hids[k].usage == add[i].usage)
                    rt_rawinput_device_change(hids[k].raw_handle, hids[k].page, hids[k].usage, true);
        }
    }
    RET(1);
}
