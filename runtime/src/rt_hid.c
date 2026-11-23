/*
 * HID game controllers and HID.DLL.
 *
 * A thread with its own run loop keeps an IOKit HID manager. The manager
 * matches the game controllers: the Joystick, Game Pad and Multi-axis
 * Controller usages of the Generic Desktop page, and the Simulation
 * Controls page. Other HID devices (keyboards, mice) need the Input
 * Monitoring permission of macOS; the raw input functions show the
 * keyboard and the mouse as one system device each (rt_rawinput.c).
 *
 * Each device shows as the Windows HID device of its first top-level
 * collection:
 *   - a device path for CreateFileW ("\\?\HID#VID_vvvv&PID_pppp#...#{HID
 *     interface class}");
 *   - input reports for ReadFile in the Windows form: the report ID byte
 *     (0 when the device has no report IDs), then the report data, padded
 *     to InputReportByteLength;
 *   - preparsed data for the HidP functions: this runtime keeps the report
 *     descriptor in the preparsed data and parses it with the rules of the
 *     Windows HID parser (button caps for arrays and 1-bit fields, value
 *     caps for the other fields, data indices in descriptor order).
 *
 * A HID handle has a queue of 32 input reports (the HID class default);
 * a full queue drops the oldest report. Reads with an OVERLAPPED structure
 * complete on the HID thread when a report arrives.
 */
#include "rt_hid.h"

#include "rt_object.h"
#include "rt_text.h"
#include "rt_thread.h"
#include "rt_user32.h"

#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDManager.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ERROR_IO_PENDING 997u
#define ERROR_DEVICE_NOT_CONNECTED 1167u
#define ERROR_INVALID_USER_BUFFER 1784u
#define ERROR_GEN_FAILURE 31u

#define STATUS_PENDING 0x00000103u
#define STATUS_CANCELLED 0xC0000120u
#define STATUS_DEVICE_NOT_CONNECTED 0xC000009Du

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_FLAG_OVERLAPPED 0x40000000u

#define HIDP_STATUS_SUCCESS 0x00110000u
#define HIDP_STATUS_INVALID_PREPARSED_DATA 0xC0110001u
#define HIDP_STATUS_INVALID_REPORT_TYPE 0xC0110002u
#define HIDP_STATUS_INVALID_REPORT_LENGTH 0xC0110003u
#define HIDP_STATUS_BUFFER_TOO_SMALL 0xC0110007u
#define HIDP_STATUS_INCOMPATIBLE_REPORT_ID 0xC011000Au
#define HIDP_STATUS_REPORT_DOES_NOT_EXIST 0xC0110010u

#define HANDLE_QUEUE 32
#define MAX_PENDING 16
#define MAX_DESCRIPTOR 16384
#define MAX_REPORT 4096
#define MAX_CAPS 512
#define MAX_LINKS 256
#define MAX_LOCAL_USAGES 256
#define PARSE_CACHE 8
#define PREPARSED_MAGIC 0x50485452u /* "RTHP" */

/* {4D1E55B2-F16F-11CF-88CB-001111000030} in GUID memory order. */
static const uint8_t hid_guid[16] = { 0xB2, 0x55, 0x1E, 0x4D, 0x6F, 0xF1, 0xCF, 0x11,
                                      0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 };

/* ---------------------------------------------------------------- report descriptor parser */

enum { RT_INPUT = 0, RT_OUTPUT = 1, RT_FEATURE = 2 };

typedef struct hcap {
    uint8_t type;      /* RT_INPUT, RT_OUTPUT, RT_FEATURE */
    bool button;       /* button caps (else value caps) */
    bool array;        /* an array main item (selectors) */
    bool is_range;
    uint8_t report_id;
    uint16_t page;
    uint16_t umin, umax; /* usage or usage range */
    uint16_t smin, smax, dmin, dmax;
    bool is_string_range, is_designator_range;
    uint16_t didx_min, didx_max;
    uint16_t bitfield;  /* the main item bits */
    uint16_t link;
    bool absolute, has_null;
    uint16_t bit_size, report_count; /* value caps; buttons: the fields of the cap */
    uint32_t units, units_exp;
    int32_t lmin, lmax, pmin, pmax;
    uint32_t bit_offset; /* the bit of the first field after the report ID byte */
    /* Arrays: the fields of the main item and the first usage index of
       this cap in the usage list of the item. */
    uint16_t array_fields, array_first;
} hcap;

typedef struct hlink {
    uint16_t page, usage;
} hlink;

typedef struct hparsed {
    uint16_t page, usage; /* the top-level collection */
    bool ids;             /* the collection uses report IDs */
    uint16_t len[3];      /* byte lengths with the report ID byte */
    uint16_t nbutton[3], nvalue[3], nindex[3];
    int ncaps;
    hcap caps[MAX_CAPS];
    int nlinks;
    hlink links[MAX_LINKS];
    bool present[3][256]; /* report IDs of each type */
    uint32_t bits[3][256];
} hparsed;

typedef struct globals {
    uint16_t page;
    int32_t lmin, lmax, pmin, pmax;
    uint32_t unit_exp, unit;
    uint32_t size, count;
    uint8_t id;
} globals;

typedef struct local_usage {
    uint16_t page, min, max;
} local_usage;

typedef struct locals {
    local_usage u[MAX_LOCAL_USAGES];
    int n;
    bool have_min;
    uint32_t pending_min; /* a usage minimum (with page) that waits for its maximum */
    uint32_t smin, smax, dmin, dmax;
    bool srange, drange, sset, dset;
} locals;

static int32_t sval(uint32_t v, int size)
{
    if (size == 1) return (int8_t)v;
    if (size == 2) return (int16_t)v;
    return (int32_t)v;
}

/* Add the caps of one main item. Returns false for a descriptor that is
   not valid or too large. */
static bool add_main(hparsed *p, int type, uint32_t flags, const globals *g, const locals *l, uint16_t link,
                     bool *seen_report)
{
    uint32_t total, offset;
    bool constant = (flags & 1u) != 0, variable = (flags & 2u) != 0;
    int32_t lmax = g->lmax;
    if (g->size == 0 || g->count == 0) return true;
    if (g->size > 32 && !constant) return false;
    total = g->size * g->count;
    if (total > MAX_REPORT * 8u) return false;
    offset = p->bits[type][g->id];
    if (offset + total > MAX_REPORT * 8u) return false;
    p->bits[type][g->id] = offset + total;
    p->present[type][g->id] = true;
    *seen_report = true;
    if (constant) return true;
    /* A logical maximum below the minimum is an unsigned value. */
    if (lmax < g->lmin && g->size < 32) lmax = (int32_t)((uint32_t)lmax & ((1u << g->size) - 1u));
    if (variable)
    {
        bool button = g->size == 1;
        uint32_t field = 0;
        int k;
        local_usage none = { g->page, 0, 0 };
        int nu = l->n > 0 ? l->n : 1;
        for (k = 0; k < nu && field < g->count; k++)
        {
            const local_usage *u = l->n > 0 ? &l->u[k] : &none;
            uint32_t span = (uint32_t)u->max - u->min + 1u, fields;
            hcap *c;
            if (p->ncaps >= MAX_CAPS) return false;
            fields = span;
            if (k == nu - 1 && span == 1) fields = g->count - field; /* the last usage applies to the other fields */
            if (fields > g->count - field) fields = g->count - field;
            c = &p->caps[p->ncaps++];
            memset(c, 0, sizeof *c);
            c->type = (uint8_t)type;
            c->button = button;
            c->report_id = g->id;
            c->page = u->page;
            c->umin = u->min;
            c->umax = span > 1 ? (uint16_t)(u->min + fields - 1u) : u->min;
            c->is_range = span > 1;
            c->bitfield = (uint16_t)flags;
            c->link = link;
            c->absolute = !(flags & 4u);
            c->has_null = (flags & 0x40u) != 0;
            c->bit_size = (uint16_t)g->size;
            c->report_count = (uint16_t)fields;
            c->units = g->unit;
            c->units_exp = g->unit_exp;
            c->lmin = g->lmin;
            c->lmax = lmax;
            c->pmin = g->pmin;
            c->pmax = g->pmax;
            c->bit_offset = offset + field * g->size;
            c->smin = (uint16_t)l->smin;
            c->smax = (uint16_t)(l->srange ? l->smax : l->smin);
            c->dmin = (uint16_t)l->dmin;
            c->dmax = (uint16_t)(l->drange ? l->dmax : l->dmin);
            c->is_string_range = l->srange;
            c->is_designator_range = l->drange;
            field += fields;
        }
    }
    else
    {
        /* An array: each field selects one usage of the usage list. */
        int k;
        uint16_t first = 0;
        if (l->n == 0) return true;
        for (k = 0; k < l->n; k++)
        {
            const local_usage *u = &l->u[k];
            hcap *c;
            if (p->ncaps >= MAX_CAPS) return false;
            c = &p->caps[p->ncaps++];
            memset(c, 0, sizeof *c);
            c->type = (uint8_t)type;
            c->button = true;
            c->array = true;
            c->report_id = g->id;
            c->page = u->page;
            c->umin = u->min;
            c->umax = u->max;
            c->is_range = u->max != u->min;
            c->bitfield = (uint16_t)flags;
            c->link = link;
            c->absolute = !(flags & 4u);
            c->has_null = (flags & 0x40u) != 0;
            c->bit_size = (uint16_t)g->size;
            c->report_count = (uint16_t)g->count;
            c->lmin = g->lmin;
            c->lmax = lmax;
            c->pmin = g->pmin;
            c->pmax = g->pmax;
            c->units = g->unit;
            c->units_exp = g->unit_exp;
            c->bit_offset = offset;
            c->array_fields = (uint16_t)g->count;
            c->array_first = first;
            c->smin = (uint16_t)l->smin;
            c->smax = (uint16_t)(l->srange ? l->smax : l->smin);
            c->dmin = (uint16_t)l->dmin;
            c->dmax = (uint16_t)(l->drange ? l->dmax : l->dmin);
            c->is_string_range = l->srange;
            c->is_designator_range = l->drange;
            first = (uint16_t)(first + (u->max - u->min + 1u));
        }
    }
    return true;
}

/* Parse the first top-level collection of a report descriptor. */
static bool parse_descriptor(const uint8_t *d, size_t n, hparsed *p)
{
    globals g, stack[8];
    locals l;
    int sp = 0, depth = 0, i, t;
    uint16_t link_stack[64];
    bool in_tlc = false, done = false, seen = false;
    size_t pos = 0;
    memset(p, 0, sizeof *p);
    memset(&g, 0, sizeof g);
    memset(&l, 0, sizeof l);
    while (pos < n && !done)
    {
        uint8_t pre = d[pos++];
        int size = (pre & 3) == 3 ? 4 : (pre & 3), type = (pre >> 2) & 3, tag = pre >> 4;
        uint32_t v = 0;
        if (pre == 0xFE) return false; /* long items: no HID device uses them */
        if (pos + (size_t)size > n) return false;
        for (i = 0; i < size; i++) v |= (uint32_t)d[pos + (size_t)i] << (8 * i);
        pos += (size_t)size;
        if (type == 0) /* main */
        {
            switch (tag)
            {
            case 0x8: case 0x9: case 0xB:
                if (in_tlc)
                {
                    int rt = tag == 0x8 ? RT_INPUT : tag == 0x9 ? RT_OUTPUT : RT_FEATURE;
                    if (!add_main(p, rt, v, &g, &l, link_stack[depth - 1], &seen)) return false;
                }
                break;
            case 0xA: /* collection */
                if (depth == 0)
                {
                    if (l.n == 0) return false;
                    if (p->nlinks > 0) /* the next top-level collection: another Windows device */
                    {
                        done = true;
                        break;
                    }
                    in_tlc = true;
                    p->page = l.u[0].page;
                    p->usage = l.u[0].min;
                }
                if (depth >= 64 || p->nlinks >= MAX_LINKS) return false;
                p->links[p->nlinks].page = l.n > 0 ? l.u[0].page : g.page;
                p->links[p->nlinks].usage = l.n > 0 ? l.u[0].min : 0;
                link_stack[depth++] = (uint16_t)p->nlinks++;
                break;
            case 0xC: /* end collection */
                if (depth == 0) return false;
                depth--;
                if (depth == 0 && in_tlc) done = true;
                break;
            default:
                return false;
            }
            memset(&l, 0, sizeof l);
        }
        else if (type == 1) /* global */
        {
            switch (tag)
            {
            case 0: g.page = (uint16_t)v; break;
            case 1: g.lmin = sval(v, size); break;
            case 2: g.lmax = sval(v, size); break;
            case 3: g.pmin = sval(v, size); break;
            case 4: g.pmax = sval(v, size); break;
            case 5: g.unit_exp = v; break;
            case 6: g.unit = v; break;
            case 7: g.size = v; break;
            case 8:
                if (v == 0 || v > 255) return false;
                g.id = (uint8_t)v;
                if (in_tlc) p->ids = true;
                break;
            case 9: g.count = v; break;
            case 10:
                if (sp >= 8) return false;
                stack[sp++] = g;
                break;
            case 11:
                if (sp == 0) return false;
                g = stack[--sp];
                break;
            default: return false;
            }
        }
        else if (type == 2) /* local */
        {
            uint16_t page = size == 4 ? (uint16_t)(v >> 16) : g.page;
            uint16_t u16 = (uint16_t)v;
            switch (tag)
            {
            case 0:
                if (l.n >= MAX_LOCAL_USAGES) return false;
                l.u[l.n++] = (local_usage){ page, u16, u16 };
                break;
            case 1:
                l.have_min = true;
                l.pending_min = ((uint32_t)page << 16) | u16;
                break;
            case 2:
                if (!l.have_min || (l.pending_min >> 16) != page || (l.pending_min & 0xFFFFu) > u16) return false;
                if (l.n >= MAX_LOCAL_USAGES) return false;
                l.u[l.n++] = (local_usage){ page, (uint16_t)l.pending_min, u16 };
                l.have_min = false;
                break;
            case 3: l.dmin = v; l.dset = true; break;
            case 4: l.dmin = v; l.drange = true; break;
            case 5: l.dmax = v; l.drange = true; break;
            case 7: l.smin = v; l.sset = true; break;
            case 8: l.smin = v; l.srange = true; break;
            case 9: l.smax = v; l.srange = true; break;
            case 10: return false; /* delimiters (aliased usages) */
            default: return false;
            }
        }
        else
            return false;
    }
    if (p->nlinks == 0 || !seen) return false;
    /* Data indices in descriptor order for each report type. */
    for (i = 0; i < p->ncaps; i++)
    {
        hcap *c = &p->caps[i];
        uint32_t count = c->is_range ? (uint32_t)c->umax - c->umin + 1u : 1u;
        if ((uint32_t)p->nindex[c->type] + count > 0xFFFFu) return false;
        c->didx_min = p->nindex[c->type];
        c->didx_max = (uint16_t)(p->nindex[c->type] + count - 1u);
        p->nindex[c->type] = (uint16_t)(p->nindex[c->type] + count);
        if (c->button)
            p->nbutton[c->type]++;
        else
            p->nvalue[c->type]++;
    }
    for (t = 0; t < 3; t++)
    {
        uint32_t maxbits = 0;
        bool have = false;
        for (i = 0; i < 256; i++)
            if (p->present[t][i])
            {
                have = true;
                if (p->bits[t][i] > maxbits) maxbits = p->bits[t][i];
            }
        p->len[t] = have ? (uint16_t)((maxbits + 7u) / 8u + 1u) : 0;
    }
    return p->len[RT_INPUT] <= MAX_REPORT;
}

/* A small cache of parsed descriptors (by content). */
static pthread_mutex_t g_parse_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    uint8_t *desc;
    uint32_t n;
    hparsed *p;
    bool ok;
} g_cache[PARSE_CACHE];
static int g_cache_next;

/* The parse of desc (owned by the cache; valid while g_parse_lock is
   held). */
static const hparsed *parsed_locked(const uint8_t *desc, uint32_t n)
{
    int i;
    for (i = 0; i < PARSE_CACHE; i++)
        if (g_cache[i].desc != NULL && g_cache[i].n == n && memcmp(g_cache[i].desc, desc, n) == 0)
            return g_cache[i].ok ? g_cache[i].p : NULL;
    i = g_cache_next;
    g_cache_next = (g_cache_next + 1) % PARSE_CACHE;
    free(g_cache[i].desc);
    if (g_cache[i].p == NULL)
    {
        g_cache[i].p = malloc(sizeof(hparsed));
        if (g_cache[i].p == NULL) rt_fatal("out of host memory (HID parser)");
    }
    g_cache[i].desc = malloc(n > 0 ? n : 1);
    if (g_cache[i].desc == NULL) rt_fatal("out of host memory (HID parser)");
    memcpy(g_cache[i].desc, desc, n);
    g_cache[i].n = n;
    g_cache[i].ok = parse_descriptor(desc, n, g_cache[i].p);
    return g_cache[i].ok ? g_cache[i].p : NULL;
}

/* ---------------------------------------------------------------- devices */

typedef struct pending {
    uint32_t buf, len, ov, tid;
} pending;

typedef struct hid_handle hid_handle;

typedef struct hid_dev {
    bool present;
    uint32_t serial; /* raw handle = 0x01000000 | serial */
    IOHIDDeviceRef dev;
    uint16_t vendor, product, version, page, usage;
    uint16_t in_len, out_len, feat_len;
    bool ids;
    uint8_t *desc;
    uint32_t ndesc;
    uint16_t product_name[128];
    int product_chars;
    char path[RT_HID_NAME_CHARS];
    uint8_t *cb_buf; /* the IOKit report buffer */
    CFIndex cb_size;
    hid_handle *handles; /* open handles (list) */
    bool id_valid[256];  /* the input report IDs of the collection */
} hid_dev;

struct hid_handle {
    int slot;
    uint32_t serial;
    uint32_t access;
    bool overlapped;
    bool gone; /* the device was removed */
    uint8_t *queue; /* HANDLE_QUEUE reports of in_len bytes */
    uint16_t in_len;
    int head, count;
    pending pend[MAX_PENDING];
    int npend;
    hid_handle *next;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER; /* a report arrived, a read completed, init done */
static hid_dev g_devs[RT_HID_MAX_DEVICES];
static uint32_t g_serial;
static bool g_ready;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void hlock(void)
{
    if (pthread_mutex_lock(&g_lock) != 0) rt_fatal("HID: lock failed");
}

static void hunlock(void)
{
    if (pthread_mutex_unlock(&g_lock) != 0) rt_fatal("HID: unlock failed");
}

static void hbroadcast(void)
{
    if (pthread_cond_broadcast(&g_cond) != 0) rt_fatal("HID: condition broadcast failed");
}

static void hwait(void)
{
    if (pthread_cond_wait(&g_cond, &g_lock) != 0) rt_fatal("HID: condition wait failed");
}

static void set_event(uint32_t h, bool signaled)
{
    rt_object *e;
    if (h == 0) return;
    e = rt_handle_get(h, OBJ_EVENT);
    if (e == NULL) return;
    rt_obj_lock();
    e->signaled = signaled;
    if (signaled) rt_obj_signal(e, e->manual_reset ? 0u : 1u);
    rt_obj_unlock();
    rt_object_unref(e);
}

/* Complete a read (lock held): OVERLAPPED Internal and InternalHigh, then
   the event. */
static void complete(const pending *pd, uint32_t status, uint32_t n)
{
    if (pd->ov == 0) return;
    wr32(pd->ov + 4, n);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    wr32(pd->ov, status);
    set_event(rd32(pd->ov + 16), true);
}

/* Copy queued reports to a read buffer (lock held). Returns the bytes. */
static uint32_t take_reports(hid_handle *hh, uint32_t buf, uint32_t len)
{
    uint32_t done = 0;
    while (hh->count > 0 && done + hh->in_len <= len)
    {
        memcpy((uint8_t *)G2H(buf + done), hh->queue + (size_t)hh->head * hh->in_len, hh->in_len);
        hh->head = (hh->head + 1) % HANDLE_QUEUE;
        hh->count--;
        done += hh->in_len;
    }
    return done;
}

static void deliver(hid_dev *dv, const uint8_t *rep)
{
    hid_handle *hh;
    for (hh = dv->handles; hh != NULL; hh = hh->next)
    {
        int tail;
        if (hh->count == HANDLE_QUEUE)
        {
            /* The queue is full: drop the oldest report (HID class). */
            hh->head = (hh->head + 1) % HANDLE_QUEUE;
            hh->count--;
        }
        tail = (hh->head + hh->count) % HANDLE_QUEUE;
        memcpy(hh->queue + (size_t)tail * hh->in_len, rep, hh->in_len);
        hh->count++;
        while (hh->npend > 0 && hh->count > 0)
        {
            pending pd = hh->pend[0];
            uint32_t n = take_reports(hh, pd.buf, pd.len);
            memmove(&hh->pend[0], &hh->pend[1], (size_t)(hh->npend - 1) * sizeof(pending));
            hh->npend--;
            complete(&pd, 0, n);
        }
    }
    hbroadcast();
}

static void input_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t id, uint8_t *report,
                         CFIndex length)
{
    uint8_t rep[MAX_REPORT + 1];
    uintptr_t slot = (uintptr_t)ctx;
    hid_dev *dv;
    uint32_t raw;
    uint16_t page, usage, in_len;
    (void)sender;
    if (result != kIOReturnSuccess || type != kIOHIDReportTypeInput || length < 0 || slot >= RT_HID_MAX_DEVICES) return;
    hlock();
    dv = &g_devs[slot];
    if (!dv->present)
    {
        hunlock();
        return;
    }
    memset(rep, 0, sizeof rep);
    if (dv->ids)
    {
        /* The IOKit report starts with the report ID byte. */
        const uint8_t *data = report;
        size_t dn = (size_t)length;
        if (id == 0 || id > 255 || !dv->id_valid[id])
        {
            hunlock();
            return; /* a report of another top-level collection */
        }
        if (dn > 0 && report[0] == id)
        {
            data++;
            dn--;
        }
        rep[0] = (uint8_t)id;
        if (dn > (size_t)dv->in_len - 1u) dn = (size_t)dv->in_len - 1u;
        memcpy(rep + 1, data, dn);
    }
    else
    {
        size_t dn = (size_t)length;
        if (dn > (size_t)dv->in_len - 1u) dn = (size_t)dv->in_len - 1u;
        rep[0] = 0;
        memcpy(rep + 1, report, dn);
    }
    deliver(dv, rep);
    raw = 0x01000000u | dv->serial;
    page = dv->page;
    usage = dv->usage;
    in_len = dv->in_len;
    hunlock();
    rt_rawinput_hid_report(raw, page, usage, rep, in_len);
}

static int32_t num_prop(IOHIDDeviceRef dev, CFStringRef key)
{
    CFTypeRef v = IOHIDDeviceGetProperty(dev, key);
    int32_t r = 0;
    if (v != NULL && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberSInt32Type, &r);
    return r;
}

static void device_added(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef dev)
{
    int i, slot = -1;
    hid_dev *dv;
    CFTypeRef d, name;
    const hparsed *p;
    uint32_t loc, raw;
    char path[RT_HID_NAME_CHARS];
    uint16_t page, usage;
    (void)ctx;
    (void)result;
    (void)sender;
    hlock();
    for (i = 0; i < RT_HID_MAX_DEVICES; i++)
    {
        if (g_devs[i].present && g_devs[i].dev == dev)
        {
            hunlock();
            return;
        }
        if (!g_devs[i].present && g_devs[i].handles == NULL && slot < 0) slot = i;
    }
    if (slot < 0)
    {
        hunlock();
        rt_log("HID: more than %d game controllers; a device is not shown", RT_HID_MAX_DEVICES);
        return;
    }
    d = IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDReportDescriptorKey));
    if (d == NULL || CFGetTypeID(d) != CFDataGetTypeID() || CFDataGetLength((CFDataRef)d) <= 0 ||
        CFDataGetLength((CFDataRef)d) > MAX_DESCRIPTOR)
    {
        hunlock();
        rt_log("HID: a game controller has no report descriptor; the device is not shown");
        return;
    }
    dv = &g_devs[slot];
    free(dv->desc);
    free(dv->cb_buf);
    memset(dv, 0, sizeof *dv);
    dv->ndesc = (uint32_t)CFDataGetLength((CFDataRef)d);
    dv->desc = malloc(dv->ndesc);
    if (dv->desc == NULL) rt_fatal("out of host memory (HID device)");
    memcpy(dv->desc, CFDataGetBytePtr((CFDataRef)d), dv->ndesc);
    if (pthread_mutex_lock(&g_parse_lock) != 0) rt_fatal("HID: lock failed");
    p = parsed_locked(dv->desc, dv->ndesc);
    if (p != NULL)
    {
        dv->page = p->page;
        dv->usage = p->usage;
        dv->in_len = p->len[RT_INPUT];
        dv->out_len = p->len[RT_OUTPUT];
        dv->feat_len = p->len[RT_FEATURE];
        dv->ids = p->ids;
        for (i = 0; i < 256; i++) dv->id_valid[i] = p->present[RT_INPUT][i];
    }
    if (pthread_mutex_unlock(&g_parse_lock) != 0) rt_fatal("HID: unlock failed");
    if (p == NULL || dv->in_len < 2)
    {
        free(dv->desc);
        dv->desc = NULL;
        hunlock();
        rt_log("HID: the report descriptor of a game controller is not supported; the device is not shown");
        return;
    }
    dv->dev = (IOHIDDeviceRef)CFRetain(dev);
    dv->vendor = (uint16_t)num_prop(dev, CFSTR(kIOHIDVendorIDKey));
    dv->product = (uint16_t)num_prop(dev, CFSTR(kIOHIDProductIDKey));
    dv->version = (uint16_t)num_prop(dev, CFSTR(kIOHIDVersionNumberKey));
    loc = (uint32_t)num_prop(dev, CFSTR(kIOHIDLocationIDKey));
    name = IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDProductKey));
    if (name != NULL && CFGetTypeID(name) == CFStringGetTypeID())
    {
        CFIndex len = CFStringGetLength((CFStringRef)name);
        if (len > 126) len = 126;
        CFStringGetCharacters((CFStringRef)name, CFRangeMake(0, len), dv->product_name);
        dv->product_chars = (int)len;
    }
    dv->serial = ++g_serial;
    if (g_serial > 0x00FFFFFFu) rt_fatal("HID: no device handles left");
    snprintf(dv->path, sizeof dv->path, "\\\\?\\HID#VID_%04X&PID_%04X#%X&%08X&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}",
             dv->vendor, dv->product, dv->serial & 0xFFFu, loc);
    dv->cb_size = (CFIndex)dv->in_len + 1;
    {
        int32_t mx = num_prop(dev, CFSTR(kIOHIDMaxInputReportSizeKey));
        if (mx > dv->cb_size && mx <= MAX_REPORT + 1) dv->cb_size = mx;
    }
    dv->cb_buf = calloc(1, (size_t)dv->cb_size);
    if (dv->cb_buf == NULL) rt_fatal("out of host memory (HID device)");
    dv->present = true;
    IOHIDDeviceRegisterInputReportCallback(dev, dv->cb_buf, dv->cb_size, input_report, (void *)(uintptr_t)slot);
    memcpy(path, dv->path, sizeof path);
    raw = 0x01000000u | dv->serial;
    page = dv->page;
    usage = dv->usage;
    hunlock();
    if (g_ready)
    {
        rt_user32_device_interface_event(true, hid_guid, path);
        rt_rawinput_device_change(raw, page, usage, true);
    }
}

static void device_removed(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef dev)
{
    int i;
    char path[RT_HID_NAME_CHARS];
    uint32_t raw = 0;
    uint16_t page = 0, usage = 0;
    bool found = false;
    (void)ctx;
    (void)result;
    (void)sender;
    hlock();
    for (i = 0; i < RT_HID_MAX_DEVICES; i++)
    {
        hid_dev *dv = &g_devs[i];
        hid_handle *hh;
        if (!dv->present || dv->dev != dev) continue;
        IOHIDDeviceRegisterInputReportCallback(dev, dv->cb_buf, dv->cb_size, NULL, NULL);
        dv->present = false;
        CFRelease(dv->dev);
        dv->dev = NULL;
        for (hh = dv->handles; hh != NULL; hh = hh->next)
        {
            int k;
            hh->gone = true;
            for (k = 0; k < hh->npend; k++) complete(&hh->pend[k], STATUS_DEVICE_NOT_CONNECTED, 0);
            hh->npend = 0;
        }
        memcpy(path, dv->path, sizeof path);
        raw = 0x01000000u | dv->serial;
        page = dv->page;
        usage = dv->usage;
        found = true;
        break;
    }
    hbroadcast();
    hunlock();
    if (found)
    {
        rt_user32_device_interface_event(false, hid_guid, path);
        rt_rawinput_device_change(raw, page, usage, false);
    }
}

static CFDictionaryRef match_dict(int page, int usage)
{
    CFMutableDictionaryRef m = CFDictionaryCreateMutable(NULL, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFNumberRef p = CFNumberCreate(NULL, kCFNumberIntType, &page);
    if (m == NULL || p == NULL) rt_fatal("HID: out of memory (matching)");
    CFDictionarySetValue(m, CFSTR(kIOHIDDeviceUsagePageKey), p);
    CFRelease(p);
    if (usage >= 0)
    {
        CFNumberRef u = CFNumberCreate(NULL, kCFNumberIntType, &usage);
        if (u == NULL) rt_fatal("HID: out of memory (matching)");
        CFDictionarySetValue(m, CFSTR(kIOHIDDeviceUsageKey), u);
        CFRelease(u);
    }
    return m;
}

static void *hid_thread(void *arg)
{
    IOHIDManagerRef mgr;
    CFDictionaryRef dicts[4];
    CFArrayRef match;
    CFSetRef set;
    IOReturn r;
    (void)arg;
    pthread_setname_np("rt HID");
    mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (mgr == NULL) rt_fatal("HID: IOHIDManagerCreate failed");
    dicts[0] = match_dict(1, 4); /* Joystick */
    dicts[1] = match_dict(1, 5); /* Game Pad */
    dicts[2] = match_dict(1, 8); /* Multi-axis Controller */
    dicts[3] = match_dict(2, -1); /* Simulation Controls */
    match = CFArrayCreate(NULL, (const void **)dicts, 4, &kCFTypeArrayCallBacks);
    if (match == NULL) rt_fatal("HID: out of memory (matching)");
    IOHIDManagerSetDeviceMatchingMultiple(mgr, match);
    CFRelease(match);
    for (int i = 0; i < 4; i++) CFRelease(dicts[i]);
    IOHIDManagerRegisterDeviceMatchingCallback(mgr, device_added, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(mgr, device_removed, NULL);
    IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    r = IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
    if (r != kIOReturnSuccess)
        rt_log("HID: IOHIDManagerOpen failed (0x%08x); game controllers are not available", (unsigned)r);
    /* The devices that are present now. */
    set = IOHIDManagerCopyDevices(mgr);
    if (set != NULL)
    {
        CFIndex n = CFSetGetCount(set), i;
        const void **devs = calloc((size_t)(n > 0 ? n : 1), sizeof *devs);
        if (devs == NULL) rt_fatal("out of host memory (HID devices)");
        CFSetGetValues(set, devs);
        for (i = 0; i < n; i++) device_added(NULL, kIOReturnSuccess, NULL, (IOHIDDeviceRef)devs[i]);
        free(devs);
        CFRelease(set);
    }
    hlock();
    g_ready = true;
    hbroadcast();
    hunlock();
    CFRunLoopRun();
    rt_fatal("HID: the run loop of the HID thread ended");
}

static void start_once(void)
{
    pthread_t t;
    pthread_attr_t a;
    if (pthread_attr_init(&a) != 0 || pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED) != 0)
        rt_fatal("HID: thread attributes failed");
    if (pthread_create(&t, &a, hid_thread, NULL) != 0) rt_fatal("HID: the HID thread could not start");
    pthread_attr_destroy(&a);
}

/* Start the HID thread and wait for the first device list (lock held on
   return). */
static void start_locked(void)
{
    if (pthread_once(&g_once, start_once) != 0) rt_fatal("HID: start failed");
    hlock();
    while (!g_ready) hwait();
}

static hid_dev *dev_by_raw(uint32_t raw) /* lock held */
{
    int i;
    if ((raw & 0xFF000000u) != 0x01000000u) return NULL;
    for (i = 0; i < RT_HID_MAX_DEVICES; i++)
        if (g_devs[i].present && (0x01000000u | g_devs[i].serial) == raw) return &g_devs[i];
    return NULL;
}

static void fill_info(const hid_dev *dv, rt_hid_info *o)
{
    o->raw_handle = 0x01000000u | dv->serial;
    o->vendor = dv->vendor;
    o->product = dv->product;
    o->version = dv->version;
    o->page = dv->page;
    o->usage = dv->usage;
    o->in_len = dv->in_len;
    memcpy(o->name, dv->path, sizeof o->name);
}

int rt_hid_devices(rt_hid_info *out, int max)
{
    int i, n = 0;
    start_locked();
    for (i = 0; i < RT_HID_MAX_DEVICES && n < max; i++)
        if (g_devs[i].present) fill_info(&g_devs[i], &out[n++]);
    hunlock();
    return n;
}

bool rt_hid_device(uint32_t raw, rt_hid_info *out)
{
    hid_dev *dv;
    start_locked();
    dv = dev_by_raw(raw);
    if (dv != NULL) fill_info(dv, out);
    hunlock();
    return dv != NULL;
}

/* Preparsed data: magic, size, the descriptor length, the descriptor. */
static uint32_t write_preparsed(const hid_dev *dv, uint8_t *dst, uint32_t cap)
{
    uint32_t size = 12u + dv->ndesc;
    uint8_t hdr[12];
    uint32_t n;
    if (dst == NULL) return size;
    memcpy(hdr, &(uint32_t){ PREPARSED_MAGIC }, 4);
    memcpy(hdr + 4, &size, 4);
    memcpy(hdr + 8, &dv->ndesc, 4);
    n = cap < 12u ? cap : 12u;
    memcpy(dst, hdr, n);
    if (cap > 12u) memcpy(dst + 12, dv->desc, cap - 12u < dv->ndesc ? cap - 12u : dv->ndesc);
    return size;
}

uint32_t rt_hid_preparsed(uint32_t raw, uint8_t *dst, uint32_t cap)
{
    hid_dev *dv;
    uint32_t size = 0;
    start_locked();
    dv = dev_by_raw(raw);
    if (dv != NULL) size = write_preparsed(dv, dst, cap);
    hunlock();
    return size;
}

/* ---------------------------------------------------------------- device handles */

bool rt_hid_is_path(const char *path)
{
    return strncasecmp(path, "\\\\?\\hid#", 8) == 0;
}

static void handle_destroy(rt_object *o)
{
    hid_handle *hh = o->data, **pp;
    int k;
    hlock();
    for (k = 0; k < hh->npend; k++) complete(&hh->pend[k], STATUS_CANCELLED, 0);
    hh->npend = 0;
    for (pp = &g_devs[hh->slot].handles; *pp != NULL; pp = &(*pp)->next)
        if (*pp == hh)
        {
            *pp = hh->next;
            break;
        }
    hbroadcast();
    hunlock();
    free(hh->queue);
    free(hh);
}

uint32_t rt_hid_open(const char *path, uint32_t access, uint32_t flags, uint32_t *err)
{
    int i;
    hid_dev *dv = NULL;
    hid_handle *hh;
    rt_object *o;
    uint32_t h;
    if (access & ~(GENERIC_READ | GENERIC_WRITE)) rt_fatal("CreateFileW(\"%s\"): access 0x%08x is not supported for a HID device", path, access);
    if (flags & ~FILE_FLAG_OVERLAPPED & ~0x00000080u /* FILE_ATTRIBUTE_NORMAL */)
        rt_fatal("CreateFileW(\"%s\"): flags 0x%08x are not supported for a HID device", path, flags);
    start_locked();
    for (i = 0; i < RT_HID_MAX_DEVICES; i++)
        if (g_devs[i].present && strcasecmp(g_devs[i].path, path) == 0) dv = &g_devs[i];
    if (dv == NULL)
    {
        hunlock();
        *err = ERROR_FILE_NOT_FOUND;
        return 0;
    }
    hh = calloc(1, sizeof *hh);
    if (hh == NULL) rt_fatal("out of host memory (HID handle)");
    hh->slot = (int)(dv - g_devs);
    hh->serial = dv->serial;
    hh->access = access;
    hh->overlapped = (flags & FILE_FLAG_OVERLAPPED) != 0;
    hh->in_len = dv->in_len;
    hh->queue = calloc(HANDLE_QUEUE, dv->in_len);
    if (hh->queue == NULL) rt_fatal("out of host memory (HID handle)");
    hh->next = dv->handles;
    dv->handles = hh;
    hunlock();
    o = rt_object_new(OBJ_HID);
    o->data = hh;
    o->destroy = handle_destroy;
    h = rt_handle_new(o);
    rt_object_unref(o);
    if (h == 0)
    {
        *err = ERROR_TOO_MANY_OPEN_FILES;
        return 0;
    }
    return h;
}

bool rt_hid_is_handle(uint32_t h)
{
    rt_object *o = rt_handle_get(h, OBJ_HID);
    if (o == NULL) return false;
    rt_object_unref(o);
    return true;
}

/* BOOL ReadFile(HANDLE, LPVOID buf, DWORD n, LPDWORD read, LPOVERLAPPED) */
void rt_hid_read_file(_cpu *cpu)
{
    rt_object *o = rt_handle_get(ARG(0), OBJ_HID);
    uint32_t buf = ARG(1), n = ARG(2), out = ARG(3), ov = ARG(4), done;
    hid_handle *hh;
    if (o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    hh = o->data;
    if (out != 0) wr32(out, 0);
    if (!(hh->access & GENERIC_READ))
    {
        rt_object_unref(o);
        rt_set_last_error(ERROR_ACCESS_DENIED);
        RET(0);
        return;
    }
    if (n < hh->in_len)
    {
        rt_object_unref(o);
        rt_set_last_error(ERROR_INVALID_USER_BUFFER);
        RET(0);
        return;
    }
    if (!rt_va_is_committed(buf, n) || (ov != 0 && !rt_va_is_committed(ov, 20)))
    {
        rt_object_unref(o);
        rt_set_last_error(998u); /* ERROR_NOACCESS */
        RET(0);
        return;
    }
    if (hh->overlapped && ov == 0) rt_fatal("ReadFile: a HID handle with FILE_FLAG_OVERLAPPED needs an OVERLAPPED structure");
    hlock();
    if (hh->gone)
    {
        hunlock();
        rt_object_unref(o);
        rt_set_last_error(ERROR_DEVICE_NOT_CONNECTED);
        RET(0);
        return;
    }
    if (hh->overlapped)
    {
        set_event(rd32(ov + 16), false);
        done = take_reports(hh, buf, n);
        if (done > 0)
        {
            pending pd = { buf, n, ov, rt_self()->tid };
            complete(&pd, 0, done);
            hunlock();
            rt_object_unref(o);
            if (out != 0) wr32(out, done);
            RET(1);
            return;
        }
        if (hh->npend >= MAX_PENDING) rt_fatal("ReadFile: more than %d pending reads on a HID handle", MAX_PENDING);
        hh->pend[hh->npend++] = (pending){ buf, n, ov, rt_self()->tid };
        wr32(ov + 4, 0);
        wr32(ov, STATUS_PENDING);
        hunlock();
        rt_object_unref(o);
        rt_set_last_error(ERROR_IO_PENDING);
        RET(0);
        return;
    }
    /* A synchronous read waits for a report. */
    while (hh->count == 0 && !hh->gone) hwait();
    if (hh->gone)
    {
        hunlock();
        rt_object_unref(o);
        rt_set_last_error(ERROR_DEVICE_NOT_CONNECTED);
        RET(0);
        return;
    }
    done = take_reports(hh, buf, n);
    hunlock();
    rt_object_unref(o);
    if (out != 0) wr32(out, done);
    if (ov != 0)
    {
        wr32(ov + 4, done);
        wr32(ov, 0);
        set_event(rd32(ov + 16), true);
    }
    RET(1);
}

/* BOOL WriteFile(HANDLE, ...) on a HID handle: output reports. */
void rt_hid_write_file(_cpu *cpu)
{
    (void)cpu;
    rt_fatal("WriteFile: HID output reports are not supported");
}

/* BOOL CancelIo(HANDLE): cancel the reads of the calling thread. */
void rt_hid_cancel_io(_cpu *cpu)
{
    rt_object *o = rt_handle_get(ARG(0), OBJ_HID);
    hid_handle *hh;
    uint32_t tid = rt_self()->tid;
    int k, keep = 0;
    if (o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    hh = o->data;
    hlock();
    for (k = 0; k < hh->npend; k++)
    {
        if (hh->pend[k].tid == tid)
            complete(&hh->pend[k], STATUS_CANCELLED, 0);
        else
            hh->pend[keep++] = hh->pend[k];
    }
    hh->npend = keep;
    hbroadcast();
    hunlock();
    rt_object_unref(o);
    RET(1);
}

void rt_hid_wait_overlapped(uint32_t h, uint32_t ov)
{
    rt_object *o = rt_handle_get(h, OBJ_HID);
    if (o == NULL) return;
    hlock();
    while (__atomic_load_n((uint32_t *)G2H(ov), __ATOMIC_ACQUIRE) == STATUS_PENDING) hwait();
    hunlock();
    rt_object_unref(o);
}

/* ---------------------------------------------------------------- HID.DLL */

/* The handle data of a HID handle (a reference in *o), or NULL. */
static hid_handle *handle_arg(uint32_t h, rt_object **o)
{
    *o = rt_handle_get(h, OBJ_HID);
    if (*o == NULL)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        return NULL;
    }
    return (*o)->data;
}

/* void HidD_GetHidGuid(LPGUID) */
void rt_HidD_GetHidGuid(_cpu *cpu)
{
    uint32_t g = ARG(0), i;
    for (i = 0; i < 16; i++) wr8(g + i, hid_guid[i]);
}

/* BOOLEAN HidD_GetPreparsedData(HANDLE, PHIDP_PREPARSED_DATA *) */
void rt_HidD_GetPreparsedData(_cpu *cpu)
{
    rt_object *o;
    hid_handle *hh = handle_arg(ARG(0), &o);
    uint32_t size, g;
    uint8_t *tmp;
    if (hh == NULL)
    {
        RET(0);
        return;
    }
    hlock();
    if (hh->gone || !g_devs[hh->slot].present || g_devs[hh->slot].serial != hh->serial)
    {
        hunlock();
        rt_object_unref(o);
        rt_set_last_error(ERROR_DEVICE_NOT_CONNECTED);
        RET(0);
        return;
    }
    size = write_preparsed(&g_devs[hh->slot], NULL, 0);
    tmp = malloc(size);
    if (tmp == NULL) rt_fatal("out of host memory (preparsed data)");
    write_preparsed(&g_devs[hh->slot], tmp, size);
    hunlock();
    rt_object_unref(o);
    g = rt_heap_alloc(size);
    if (g == 0)
    {
        free(tmp);
        rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        RET(0);
        return;
    }
    memcpy(G2H(g), tmp, size);
    free(tmp);
    wr32(ARG(1), g);
    RET(1);
}

/* BOOLEAN HidD_FreePreparsedData(PHIDP_PREPARSED_DATA) */
void rt_HidD_FreePreparsedData(_cpu *cpu)
{
    uint32_t p = ARG(0);
    if (p == 0 || !rt_va_is_committed(p, 12) || rd32(p) != PREPARSED_MAGIC)
    {
        RET(0);
        return;
    }
    wr32(p, 0);
    rt_heap_free(p);
    RET(1);
}

/* BOOLEAN HidD_GetProductString(HANDLE, PVOID buf, ULONG bytes) */
void rt_HidD_GetProductString(_cpu *cpu)
{
    rt_object *o;
    hid_handle *hh = handle_arg(ARG(0), &o);
    uint32_t buf = ARG(1), len = ARG(2), i;
    uint16_t name[128];
    int n;
    if (hh == NULL)
    {
        RET(0);
        return;
    }
    hlock();
    if (hh->gone || g_devs[hh->slot].serial != hh->serial)
    {
        hunlock();
        rt_object_unref(o);
        rt_set_last_error(ERROR_DEVICE_NOT_CONNECTED);
        RET(0);
        return;
    }
    n = g_devs[hh->slot].product_chars;
    memcpy(name, g_devs[hh->slot].product_name, sizeof name);
    hunlock();
    rt_object_unref(o);
    if (n == 0)
    {
        rt_set_last_error(ERROR_GEN_FAILURE); /* the device has no product string */
        RET(0);
        return;
    }
    if (len < 2u * ((uint32_t)n + 1u))
    {
        rt_set_last_error(ERROR_INVALID_USER_BUFFER);
        RET(0);
        return;
    }
    for (i = 0; i < (uint32_t)n; i++) wr16(buf + 2u * i, name[i]);
    wr16(buf + 2u * (uint32_t)n, 0);
    RET(1);
}

/* The parse of guest preparsed data (g_parse_lock held on success). */
static const hparsed *preparsed_arg(uint32_t pp)
{
    uint32_t size, n;
    const hparsed *p;
    if (pp == 0 || !rt_va_is_committed(pp, 12) || rd32(pp) != PREPARSED_MAGIC) return NULL;
    size = rd32(pp + 4);
    n = rd32(pp + 8);
    if (n == 0 || n > MAX_DESCRIPTOR || size != 12u + n || !rt_va_is_committed(pp, size)) return NULL;
    if (pthread_mutex_lock(&g_parse_lock) != 0) rt_fatal("HID: lock failed");
    p = parsed_locked((const uint8_t *)G2H(pp + 12), n);
    if (p == NULL && pthread_mutex_unlock(&g_parse_lock) != 0) rt_fatal("HID: unlock failed");
    return p;
}

static void parse_done(void)
{
    if (pthread_mutex_unlock(&g_parse_lock) != 0) rt_fatal("HID: unlock failed");
}

/* NTSTATUS HidP_GetCaps(PHIDP_PREPARSED_DATA, PHIDP_CAPS) */
void rt_HidP_GetCaps(_cpu *cpu)
{
    const hparsed *p = preparsed_arg(ARG(0));
    uint32_t c = ARG(1), i;
    if (p == NULL)
    {
        RET(HIDP_STATUS_INVALID_PREPARSED_DATA);
        return;
    }
    for (i = 0; i < 64; i += 4) wr32(c + i, 0);
    wr16(c + 0, p->usage);
    wr16(c + 2, p->page);
    wr16(c + 4, p->len[RT_INPUT]);
    wr16(c + 6, p->len[RT_OUTPUT]);
    wr16(c + 8, p->len[RT_FEATURE]);
    wr16(c + 44, (uint16_t)p->nlinks);
    wr16(c + 46, p->nbutton[RT_INPUT]);
    wr16(c + 48, p->nvalue[RT_INPUT]);
    wr16(c + 50, p->nindex[RT_INPUT]);
    wr16(c + 52, p->nbutton[RT_OUTPUT]);
    wr16(c + 54, p->nvalue[RT_OUTPUT]);
    wr16(c + 56, p->nindex[RT_OUTPUT]);
    wr16(c + 58, p->nbutton[RT_FEATURE]);
    wr16(c + 60, p->nvalue[RT_FEATURE]);
    wr16(c + 62, p->nindex[RT_FEATURE]);
    parse_done();
    RET(HIDP_STATUS_SUCCESS);
}

/* The common head of HIDP_BUTTON_CAPS and HIDP_VALUE_CAPS (72 bytes). */
static void write_cap_head(uint32_t o, const hparsed *p, const hcap *c)
{
    uint32_t i;
    for (i = 0; i < 72; i += 4) wr32(o + i, 0);
    wr16(o + 0, c->page);
    wr8(o + 2, c->report_id);
    wr8(o + 3, 0); /* IsAlias */
    wr16(o + 4, c->bitfield);
    wr16(o + 6, c->link);
    wr16(o + 8, p->links[c->link].usage);
    wr16(o + 10, p->links[c->link].page);
    wr8(o + 12, c->is_range ? 1 : 0);
    wr8(o + 13, c->is_string_range ? 1 : 0);
    wr8(o + 14, c->is_designator_range ? 1 : 0);
    wr8(o + 15, c->absolute ? 1 : 0);
    if (c->is_range)
    {
        wr16(o + 56, c->umin);
        wr16(o + 58, c->umax);
        wr16(o + 60, c->smin);
        wr16(o + 62, c->smax);
        wr16(o + 64, c->dmin);
        wr16(o + 66, c->dmax);
        wr16(o + 68, c->didx_min);
        wr16(o + 70, c->didx_max);
    }
    else
    {
        wr16(o + 56, c->umin);
        wr16(o + 60, c->smin);
        wr16(o + 64, c->dmin);
        wr16(o + 68, c->didx_min);
    }
}

static void get_caps(_cpu *cpu, bool buttons)
{
    uint32_t type = ARG(0), out = ARG(1), plen = ARG(2), cap, n = 0;
    const hparsed *p;
    int i;
    if (type > 2)
    {
        RET(HIDP_STATUS_INVALID_REPORT_TYPE);
        return;
    }
    p = preparsed_arg(ARG(3));
    if (p == NULL)
    {
        RET(HIDP_STATUS_INVALID_PREPARSED_DATA);
        return;
    }
    cap = rd16(plen);
    for (i = 0; i < p->ncaps; i++)
    {
        const hcap *c = &p->caps[i];
        uint32_t o;
        if (c->type != type || c->button != buttons) continue;
        if (n >= cap)
        {
            n++;
            continue;
        }
        o = out + 72u * n;
        write_cap_head(o, p, c);
        if (!buttons)
        {
            wr8(o + 16, c->has_null ? 1 : 0);
            wr16(o + 18, c->bit_size);
            wr16(o + 20, c->report_count);
            wr32(o + 32, c->units_exp);
            wr32(o + 36, c->units);
            wr32(o + 40, (uint32_t)c->lmin);
            wr32(o + 44, (uint32_t)c->lmax);
            wr32(o + 48, (uint32_t)c->pmin);
            wr32(o + 52, (uint32_t)c->pmax);
        }
        n++;
    }
    parse_done();
    if (n > cap)
    {
        wr16(plen, (uint16_t)cap);
        RET(HIDP_STATUS_BUFFER_TOO_SMALL);
        return;
    }
    wr16(plen, (uint16_t)n);
    RET(HIDP_STATUS_SUCCESS);
}

/* NTSTATUS HidP_GetButtonCaps(HIDP_REPORT_TYPE, PHIDP_BUTTON_CAPS, PUSHORT
   length, PHIDP_PREPARSED_DATA) */
void rt_HidP_GetButtonCaps(_cpu *cpu) { get_caps(cpu, true); }
/* NTSTATUS HidP_GetValueCaps(HIDP_REPORT_TYPE, PHIDP_VALUE_CAPS, PUSHORT
   length, PHIDP_PREPARSED_DATA) */
void rt_HidP_GetValueCaps(_cpu *cpu) { get_caps(cpu, false); }

/* ULONG HidP_MaxDataListLength(HIDP_REPORT_TYPE, PHIDP_PREPARSED_DATA) */
void rt_HidP_MaxDataListLength(_cpu *cpu)
{
    uint32_t type = ARG(0), r;
    const hparsed *p;
    if (type > 2)
    {
        RET(0);
        return;
    }
    p = preparsed_arg(ARG(1));
    if (p == NULL)
    {
        RET(0);
        return;
    }
    r = p->nindex[type];
    parse_done();
    RET(r);
}

static uint32_t get_bits(const uint8_t *r, uint32_t bit, uint32_t size)
{
    uint32_t v = 0, i;
    for (i = 0; i < size; i++)
    {
        uint32_t b = bit + i;
        if (r[b >> 3] & (1u << (b & 7u))) v |= 1u << i;
    }
    return v;
}

/* NTSTATUS HidP_GetData(HIDP_REPORT_TYPE, PHIDP_DATA list, PULONG length,
   PHIDP_PREPARSED_DATA, PCHAR report, ULONG report_length) */
void rt_HidP_GetData(_cpu *cpu)
{
    uint32_t type = ARG(0), list = ARG(1), plen = ARG(2), rep = ARG(4), rlen = ARG(5), cap, n = 0;
    const hparsed *p;
    const uint8_t *r;
    uint8_t id;
    bool overflow = false;
    int i;
    if (type > 2)
    {
        RET(HIDP_STATUS_INVALID_REPORT_TYPE);
        return;
    }
    p = preparsed_arg(ARG(3));
    if (p == NULL)
    {
        RET(HIDP_STATUS_INVALID_PREPARSED_DATA);
        return;
    }
    if (rlen != p->len[type] || rlen == 0 || !rt_va_is_committed(rep, rlen))
    {
        parse_done();
        RET(HIDP_STATUS_INVALID_REPORT_LENGTH);
        return;
    }
    r = (const uint8_t *)G2H(rep);
    id = r[0];
    if ((p->ids && id == 0) || (!p->ids && id != 0))
    {
        parse_done();
        RET(HIDP_STATUS_INCOMPATIBLE_REPORT_ID);
        return;
    }
    if (!p->present[type][id])
    {
        parse_done();
        RET(HIDP_STATUS_REPORT_DOES_NOT_EXIST);
        return;
    }
    cap = rd32(plen);
    r++; /* the data after the report ID byte */
#define ADD_DATA(index, value)                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        if (n < cap)                                                                                                   \
        {                                                                                                              \
            wr16(list + 8u * n, (uint16_t)(index));                                                                    \
            wr16(list + 8u * n + 2u, 0);                                                                               \
            wr32(list + 8u * n + 4u, (value));                                                                         \
        }                                                                                                              \
        else                                                                                                           \
            overflow = true;                                                                                           \
        n++;                                                                                                           \
    } while (0)
    for (i = 0; i < p->ncaps; i++)
    {
        const hcap *c = &p->caps[i];
        uint32_t f;
        if (c->type != type || c->report_id != id) continue;
        if (c->button && c->array)
        {
            uint32_t span = (uint32_t)c->umax - c->umin + 1u;
            for (f = 0; f < c->array_fields; f++)
            {
                int64_t v = (int64_t)get_bits(r, c->bit_offset + f * c->bit_size, c->bit_size), k;
                if (v < c->lmin || v > c->lmax) continue; /* no selection */
                k = v - c->lmin - c->array_first;
                if (k < 0 || k >= (int64_t)span) continue; /* a usage of another cap of the item */
                ADD_DATA(c->didx_min + (uint32_t)k, 1u);
            }
        }
        else if (c->button)
        {
            for (f = 0; f < c->report_count; f++)
                if (get_bits(r, c->bit_offset + f, 1))
                    ADD_DATA(c->is_range ? c->didx_min + f : c->didx_min, 1u);
        }
        else
        {
            /* A usage value array (one usage, more than one value):
               HidP_GetData does not return data for it (Windows driver
               documentation of HidP_GetData); HidP_GetUsageValueArray
               reads it. */
            if (!c->is_range && c->report_count > 1) continue;
            for (f = 0; f < c->report_count; f++)
                ADD_DATA(c->is_range ? c->didx_min + f : c->didx_min, get_bits(r, c->bit_offset + f * c->bit_size, c->bit_size));
        }
    }
#undef ADD_DATA
    parse_done();
    if (overflow)
    {
        wr32(plen, n);
        RET(HIDP_STATUS_BUFFER_TOO_SMALL);
        return;
    }
    wr32(plen, n);
    RET(HIDP_STATUS_SUCCESS);
}
