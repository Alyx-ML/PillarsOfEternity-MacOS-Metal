/*
 * USER32: window classes, windows, message queues, focus and activation,
 * keyboard and mouse input, and message boxes.
 *
 * The native windows and events are in rt_window.m (rt_window.h). This
 * file keeps the Win32 state and sends the Win32 messages. Icons,
 * cursors, displays, system settings and the clipboard are in
 * rt_user32_sys.c. Dialogs and the system control classes are in
 * rt_dialog.c.
 *
 * Model:
 *   - Each thread that uses USER32 has one message queue: posted
 *     messages, system notifications and (main thread) input messages.
 *     PeekMessage takes, in this order: system notifications (they act
 *     as messages sent by another thread), posted messages, WM_QUIT,
 *     input, WM_PAINT. This is the order of Windows (as the Wine project
 *     documents it).
 *   - A message that Windows sends (SendMessage) calls the window
 *     procedure directly. Only the owner thread of a window can send to
 *     it; a send from another thread stops the process.
 *   - Native windows exist only for windows of the main thread. A
 *     top-level window gets its NSWindow when it is shown (or when the
 *     renderer asks for its layer); its child windows then get their
 *     views.
 *   - Native events become queue entries. Guest code runs only in
 *     PeekMessage, GetMessage, DispatchMessage, SendMessage and the other
 *     USER32 calls of the guest.
 *   - Coordinates are points (one point is one Win32 pixel). The screen
 *     origin is the top left corner of the main display.
 *   - Keys map by physical position to the US keyboard layout. The
 *     Option key is Alt and the Command key is the Windows key.
 */
#include "rt.h"
#include "rt_module.h"
#include "rt_object.h"
#include "rt_text.h"
#include "rt_thread.h"
#include "rt_user32.h"
#include "rt_window.h"

#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* ---------------------------------------------------------------- constants */

#define WM_APPCOMMAND 0x0319u
#define SC_SIZE 0xF000u
#define SC_MOVE 0xF010u
#define SC_MINIMIZE 0xF020u
#define SC_MAXIMIZE 0xF030u
#define SC_CLOSE 0xF060u
#define SC_KEYMENU 0xF100u
#define SC_RESTORE 0xF120u
#define SC_SCREENSAVE 0xF140u
#define SC_MONITORPOWER 0xF170u

#define CS_DBLCLKS 0x0008u
#define CW_USEDEFAULT 0x80000000u

#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_SHOWMINIMIZED 2
#define SW_SHOWMAXIMIZED 3
#define SW_SHOWNOACTIVATE 4
#define SW_SHOW 5
#define SW_MINIMIZE 6
#define SW_SHOWMINNOACTIVE 7
#define SW_SHOWNA 8
#define SW_RESTORE 9
#define SW_SHOWDEFAULT 10
#define SW_FORCEMINIMIZE 11

#define SIZE_RESTORED 0u
#define SIZE_MINIMIZED 1u
#define SIZE_MAXIMIZED 2u

#define WA_INACTIVE 0u
#define WA_ACTIVE 1u
#define WA_CLICKACTIVE 2u

#define SWP_NOSIZE 0x0001u
#define SWP_NOMOVE 0x0002u
#define SWP_NOZORDER 0x0004u
#define SWP_NOREDRAW 0x0008u
#define SWP_NOACTIVATE 0x0010u
#define SWP_FRAMECHANGED 0x0020u
#define SWP_SHOWWINDOW 0x0040u
#define SWP_HIDEWINDOW 0x0080u
#define SWP_NOCOPYBITS 0x0100u
#define SWP_NOOWNERZORDER 0x0200u
#define SWP_NOSENDCHANGING 0x0400u
#define SWP_NOCLIENTSIZE 0x0800u
#define SWP_NOCLIENTMOVE 0x1000u
#define SWP_DEFERERASE 0x2000u
#define SWP_ASYNCWINDOWPOS 0x4000u
#define SWP_STATECHANGED 0x8000u

#define HWND_TOP 0u
#define HWND_BOTTOM 1u
#define HWND_TOPMOST 0xFFFFFFFFu
#define HWND_NOTOPMOST 0xFFFFFFFEu
#define HWND_MESSAGE 0xFFFFFFFDu
#define HWND_BROADCAST 0xFFFFu

#define GA_PARENT 1u
#define GA_ROOT 2u
#define GA_ROOTOWNER 3u

#define PM_REMOVE 0x0001u
#define PM_NOYIELD 0x0002u
#define PM_QS_INPUT (0x0407u << 16)
#define PM_QS_POSTMESSAGE ((0x0008u | 0x0080u | 0x0010u) << 16)
#define PM_QS_PAINT (0x0020u << 16)
#define PM_QS_SENDMESSAGE (0x0040u << 16)

#define QS_KEY 0x0001u
#define QS_MOUSEMOVE 0x0002u
#define QS_MOUSEBUTTON 0x0004u
#define QS_POSTMESSAGE 0x0008u
#define QS_TIMER 0x0010u
#define QS_PAINT 0x0020u
#define QS_SENDMESSAGE 0x0040u
#define QS_HOTKEY 0x0080u
#define QS_ALLPOSTMESSAGE 0x0100u
#define QS_RAWINPUT 0x0400u
#define QS_ALL_KINDS 0x05FFu

#define MWMO_WAITALL 0x0001u
#define MWMO_ALERTABLE 0x0002u
#define MWMO_INPUTAVAILABLE 0x0004u

#define GWL_WNDPROC (-4)
#define GWL_HINSTANCE (-6)
#define GWL_HWNDPARENT (-8)
#define GWL_ID (-12)
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define GWL_USERDATA (-21)

#define MK_LBUTTON 0x01u
#define MK_RBUTTON 0x02u
#define MK_SHIFT 0x04u
#define MK_CONTROL 0x08u
#define MK_MBUTTON 0x10u
#define MK_XBUTTON1 0x20u
#define MK_XBUTTON2 0x40u

#define VK_LBUTTON 0x01u
#define VK_RBUTTON 0x02u
#define VK_MBUTTON 0x04u
#define VK_XBUTTON1 0x05u
#define VK_XBUTTON2 0x06u
#define VK_BACK 0x08u
#define VK_TAB 0x09u
#define VK_CLEAR 0x0Cu
#define VK_RETURN 0x0Du
#define VK_SHIFT 0x10u
#define VK_CONTROL 0x11u
#define VK_MENU 0x12u
#define VK_CAPITAL 0x14u
#define VK_ESCAPE 0x1Bu
#define VK_F4 0x73u
#define VK_F10 0x79u
#define VK_NUMLOCK 0x90u
#define VK_SCROLL 0x91u
#define VK_LSHIFT 0xA0u
#define VK_RSHIFT 0xA1u
#define VK_LCONTROL 0xA2u
#define VK_RCONTROL 0xA3u
#define VK_LMENU 0xA4u
#define VK_RMENU 0xA5u

#define ERROR_INVALID_WINDOW_HANDLE 1400u
#define ERROR_CANNOT_FIND_WND_CLASS 1407u
#define ERROR_WINDOW_OF_OTHER_THREAD 1408u
#define ERROR_CLASS_ALREADY_EXISTS 1410u
#define ERROR_CLASS_DOES_NOT_EXIST 1411u
#define ERROR_CLASS_HAS_WINDOWS 1412u
#define ERROR_INVALID_INDEX 1413u
#define ERROR_NOT_ENOUGH_QUOTA 1816u

#define MAX_CLASSES 128
#define MAX_WINDOWS 4096
#define MAX_QUEUES 128
#define MAX_WINDOW_EXTRA 1024u
#define QUEUE_LIMIT 10000u /* the Windows default limit of posted messages */
#define SYS_LIMIT 4096u
#define MAX_WINDOW_MESSAGES 1024
#define MAX_TEXT 0x100000u /* bytes of a window text */

/* ---------------------------------------------------------------- state */

typedef struct wclass {
    bool used;
    bool runtime;
    bool unicode;
    char name[256];
    uint16_t atom;
    uint32_t style, wndproc, cls_extra, wnd_extra, hinstance, icon, cursor, brush, icon_sm;
    rt_host_proc host;
} wclass;

typedef struct wnd {
    uint32_t hwnd; /* 0: free slot */
    uint32_t tid;
    int cls;
    uint32_t style, exstyle, wndproc, hinstance, id, userdata, parent, owner;
    bool unicode, destroying, need_paint;
    bool ime;            /* the window has the input context of its thread (ImmAssociateContext) */
    uint64_t z;          /* larger values are above (siblings, top-level windows) */
    uint8_t *extra;
    uint32_t extra_size;
    char *text;          /* UTF-8 */
    rt_rect wrect;       /* screen (top-level) or parent client coordinates (child) */
    rt_rect restore;     /* the window rectangle before a minimize or maximize */
    int32_t title_h;     /* top-level titled window: title bar height */
    void *ns;            /* NSWindow (top-level) or NSView (child) */
    void *class_data;
    uint32_t icon_big, icon_small;
    int32_t min_w, min_h; /* MINMAXINFO track size limits (0: none) */
} wnd;

typedef struct qmsg {
    uint32_t hwnd, msg, wparam, lparam, time;
    int32_t x, y;
    uint16_t chars[8]; /* key-down: the characters of the key (TranslateMessage) */
    uint8_t nchars;
    bool uchar;        /* a character message with a UTF-16 unit in wparam */
} qmsg;

typedef struct ring {
    qmsg *buf;
    uint32_t head, count, cap;
} ring;

/* System notifications: messages that the system would send. */
#define SYS_ACTIVE 1
#define SYS_APP_ACTIVE 2
#define SYS_CLOSE 3
#define SYS_FRAME 4
#define SYS_MINIMIZED 5
#define SYS_QUIT 6
#define SYS_CONTROL 7
#define SYS_IME_COMP 8
#define SYS_IME_RESULT 9
#define SYS_CALL 10 /* call fn(cpu, ctx) (other runtime parts) */
#define SYS_SENT 11 /* a message that another thread sends */

/* The reply of a message sent to another thread. */
typedef struct sent_reply {
    int refs;       /* the sender and the queue entry (user lock) */
    bool done;
    uint32_t result;
} sent_reply;

typedef struct sysmsg {
    int kind;
    uint32_t hwnd;
    int32_t a, b, c;
    uint16_t *text; /* IME text (malloc) */
    void (*fn)(_cpu *cpu, void *ctx);
    void *ctx;
    uint32_t msg, wp, lp; /* SYS_SENT */
    bool unicode;
    sent_reply *reply;
} sysmsg;

typedef struct mqueue {
    uint32_t tid; /* 0: free */
    ring posted, input;
    sysmsg *sys;
    uint32_t sys_head, sys_count;
    bool quit;
    uint32_t quit_code;
    uint32_t changed;      /* QS_* of the entries added since the last check */
    rt_object *event;      /* signaled while changed != 0 */
    qmsg last_key;         /* the last key-down that PeekMessage removed */
    bool last_key_valid;
    uint32_t focus;        /* the focus window of the thread */
    uint32_t capture;      /* the mouse capture window */
    uint32_t active;       /* the active window of the thread */
    uint32_t last_active;  /* the window that was active before the last deactivation (SYS_APP_ACTIVE) */
    uint32_t last_check;   /* now_ms() of the last message check (SMTO_ABORTIFHUNG) */
    uint32_t himc;         /* the input context of the thread (IMM32) */
    bool ime_closed;       /* ImmSetOpenStatus(FALSE) */
} mqueue;

static pthread_mutex_t user_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sent_cond = PTHREAD_COND_INITIALIZER; /* a reply is done, or a queue got a sent message */
static wclass classes[MAX_CLASSES];
static wnd windows[MAX_WINDOWS]; /* slot 0: the desktop window */
static uint16_t generations[MAX_WINDOWS];
static mqueue queues[MAX_QUEUES];
static uint64_t z_counter;
static uint32_t himc_serial; /* the last input context number */
static uint32_t main_tid;
static bool app_active;
static atomic_bool headless;

/* Input state (main thread events; read with the lock). */
static int32_t mouse_x, mouse_y;
static uint8_t input_down[256];   /* key and button state as the events arrive */
static uint8_t async_pressed[256]; /* pressed since the last GetAsyncKeyState */
static uint8_t sync_state[256];    /* GetKeyState: 0x80 down, 0x01 toggled */
static uint32_t last_mods;

/* IME state of the main thread: the current composition. */
static uint16_t *ime_comp, *ime_result;
static int ime_ncomp, ime_nresult, ime_caret;

/* RegisterWindowMessage names. */
static char *window_messages[MAX_WINDOW_MESSAGES];
static int n_window_messages;

static void lock(void)
{
    if (pthread_mutex_lock(&user_lock) != 0) rt_fatal("USER32 lock failed");
}

static void unlock(void)
{
    if (pthread_mutex_unlock(&user_lock) != 0) rt_fatal("USER32 unlock failed");
}

static uint32_t now_ms(void)
{
    return (uint32_t)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000u);
}

static uint32_t self_tid(void)
{
    return rt_self()->tid;
}

static bool on_main(void)
{
    return rt_cocoa_is_main_thread();
}

static uint32_t make_lparam(int32_t lo, int32_t hi)
{
    return (uint32_t)(uint16_t)(int16_t)(lo < -32768 ? -32768 : lo > 32767 ? 32767 : lo) |
           ((uint32_t)(uint16_t)(int16_t)(hi < -32768 ? -32768 : hi > 32767 ? 32767 : hi) << 16);
}

static void *xcalloc(size_t n, size_t size)
{
    void *p = calloc(n, size);
    if (p == NULL) rt_fatal("out of host memory (USER32)");
    return p;
}

static char *xstrdup(const char *s)
{
    char *p = strdup(s != NULL ? s : "");
    if (p == NULL) rt_fatal("out of host memory (USER32)");
    return p;
}

/* ---------------------------------------------------------------- handles */

#define DESKTOP_HWND 0x00010004u

static uint32_t make_hwnd(int i)
{
    if (generations[i] == 0 || generations[i] >= 0x7FFF) generations[i] = 1;
    return ((uint32_t)generations[i] << 16) | ((uint32_t)(i + 1) << 2);
}

/* The slot of hwnd, or -1 (lock held). Slot 0 is the desktop. */
static int slot_of(uint32_t hwnd)
{
    uint32_t i;
    if ((hwnd & 3u) != 0 || (hwnd & 0xFFFFu) == 0) return -1;
    i = ((hwnd & 0xFFFFu) >> 2) - 1u;
    if (i >= MAX_WINDOWS || windows[i].hwnd != hwnd) return -1;
    return (int)i;
}

/* A window that is not the desktop, or NULL (lock held). */
static wnd *get(uint32_t hwnd)
{
    int i = slot_of(hwnd);
    return i > 0 ? &windows[i] : NULL;
}

/* ---------------------------------------------------------------- queues */

static void ring_init(ring *r, uint32_t cap)
{
    if (r->buf != NULL) return;
    r->buf = xcalloc(cap, sizeof *r->buf);
    r->cap = cap;
}

static qmsg *ring_at(ring *r, uint32_t i)
{
    return &r->buf[(r->head + i) % r->cap];
}

static bool ring_push(ring *r, const qmsg *m, bool front)
{
    if (r->count >= r->cap) return false;
    if (front)
    {
        r->head = (r->head + r->cap - 1u) % r->cap;
        r->buf[r->head] = *m;
    }
    else
        r->buf[(r->head + r->count) % r->cap] = *m;
    r->count++;
    return true;
}

static void ring_remove(ring *r, uint32_t i)
{
    uint32_t j;
    if (i == 0)
    {
        r->head = (r->head + 1u) % r->cap;
        r->count--;
        return;
    }
    for (j = i; j + 1u < r->count; j++) *ring_at(r, j) = *ring_at(r, j + 1u);
    r->count--;
}

static void ring_drop_window(ring *r, uint32_t hwnd)
{
    uint32_t i = 0;
    while (i < r->count)
    {
        if (ring_at(r, i)->hwnd == hwnd)
            ring_remove(r, i);
        else
            i++;
    }
}

/* The queue of thread tid (lock held). create: make it if it does not
   exist. */
static mqueue *queue_of(uint32_t tid, bool create)
{
    int i, free_slot = -1;
    for (i = 0; i < MAX_QUEUES; i++)
    {
        if (queues[i].tid == tid) return &queues[i];
        if (queues[i].tid == 0 && free_slot < 0) free_slot = i;
    }
    if (!create) return NULL;
    if (free_slot < 0) rt_fatal("USER32: more than %d threads have message queues", MAX_QUEUES);
    {
        mqueue *q = &queues[free_slot];
        memset(q, 0, sizeof *q);
        q->tid = tid;
        if (himc_serial >= 0x00FFFFFFu) rt_fatal("USER32: no input context handles left");
        q->himc = 0x49000000u | ++himc_serial;
        ring_init(&q->posted, QUEUE_LIMIT);
        if (tid == main_tid) ring_init(&q->input, QUEUE_LIMIT);
        q->sys = xcalloc(SYS_LIMIT, sizeof *q->sys);
        q->event = rt_object_new(OBJ_EVENT);
        q->event->manual_reset = true;
        return q;
    }
}

static mqueue *my_queue(void)
{
    mqueue *q;
    lock();
    q = queue_of(self_tid(), true);
    unlock();
    return q;
}

/* New entries (lock held). */
static void queue_signal(mqueue *q, uint32_t bits)
{
    q->changed |= bits;
    rt_obj_lock();
    if (!q->event->signaled)
    {
        q->event->signaled = true;
        rt_obj_signal(q->event, 0);
    }
    rt_obj_unlock();
}

/* The thread looked at its queue: entries are no longer new (lock held). */
static void queue_seen(mqueue *q)
{
    q->changed = 0;
    rt_obj_lock();
    q->event->signaled = false;
    rt_obj_unlock();
}

static void sys_push(mqueue *q, const sysmsg *m)
{
    if (q->sys_count >= SYS_LIMIT) rt_fatal("USER32: more than %u system notifications wait", SYS_LIMIT);
    q->sys[(q->sys_head + q->sys_count) % SYS_LIMIT] = *m;
    q->sys_count++;
    queue_signal(q, QS_SENDMESSAGE);
    if (pthread_cond_broadcast(&sent_cond) != 0) rt_fatal("USER32: condition broadcast failed");
}

/* The window needs WM_PAINT (lock held). */
static void mark_paint(wnd *w)
{
    w->need_paint = true;
    queue_signal(queue_of(w->tid, true), QS_PAINT);
}

/* A system notification for the main thread (native events). */
static void main_sys(int kind, uint32_t hwnd, int32_t a, int32_t b)
{
    sysmsg m;
    memset(&m, 0, sizeof m);
    m.kind = kind;
    m.hwnd = hwnd;
    m.a = a;
    m.b = b;
    lock();
    sys_push(queue_of(main_tid, true), &m);
    unlock();
}

static bool post_to(uint32_t hwnd, const qmsg *m)
{
    mqueue *q;
    wnd *w;
    bool ok;
    lock();
    w = get(hwnd);
    if (w == NULL)
    {
        unlock();
        rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        return false;
    }
    q = queue_of(w->tid, true);
    ok = ring_push(&q->posted, m, false);
    if (ok) queue_signal(q, QS_POSTMESSAGE | QS_ALLPOSTMESSAGE);
    unlock();
    if (!ok) rt_set_last_error(ERROR_NOT_ENOUGH_QUOTA);
    return ok;
}

bool rt_user32_post_message(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam)
{
    qmsg m;
    memset(&m, 0, sizeof m);
    m.hwnd = hwnd;
    m.msg = msg;
    m.wparam = wparam;
    m.lparam = lparam;
    m.time = now_ms();
    lock();
    m.x = mouse_x;
    m.y = mouse_y;
    unlock();
    return post_to(hwnd, &m);
}

/* Input message for the main thread. A WM_MOUSEMOVE replaces a
   WM_MOUSEMOVE for the same window at the end of the queue, as on
   Windows. */
static void post_input(const qmsg *m, uint32_t bits)
{
    mqueue *q;
    bool ok;
    lock();
    q = queue_of(main_tid, true);
    if (m->msg == WM_MOUSEMOVE && q->input.count > 0)
    {
        qmsg *last = ring_at(&q->input, q->input.count - 1u);
        if (last->msg == WM_MOUSEMOVE && last->hwnd == m->hwnd)
        {
            *last = *m;
            queue_signal(q, bits);
            unlock();
            return;
        }
    }
    ok = ring_push(&q->input, m, false);
    if (ok) queue_signal(q, bits);
    unlock();
    if (!ok) rt_log("USER32: the input queue is full (%u messages); input message 0x%04x was discarded", QUEUE_LIMIT, m->msg);
}

/* Run fn(cpu, ctx) on the main thread at its next message check (other
   runtime parts: raw input, device notifications). Any thread. */
void rt_user32_main_call(void (*fn)(_cpu *cpu, void *ctx), void *ctx)
{
    sysmsg m;
    memset(&m, 0, sizeof m);
    m.kind = SYS_CALL;
    m.fn = fn;
    m.ctx = ctx;
    lock();
    sys_push(queue_of(main_tid, true), &m);
    unlock();
}

/* ---------------------------------------------------------------- text */

/* Length of a NUL-terminated guest string in characters. */
static uint32_t guest_len(uint32_t g, bool wide)
{
    uint32_t n = 0;
    if (wide) return rt_wstrlen(g);
    while (rd8(g + n) != 0)
    {
        if (n >= MAX_TEXT) rt_fatal("USER32: a guest string is longer than %u characters", MAX_TEXT);
        n++;
    }
    return n;
}

/* A guest string (ANSI or wide) as a new UTF-8 string. NULL for g == 0. */
static char *guest_text(uint32_t g, bool wide)
{
    uint32_t n;
    size_t cap;
    char *s;
    int r;
    if (g == 0) return NULL;
    n = guest_len(g, wide);
    if (n > MAX_TEXT) rt_fatal("USER32: a guest string is longer than %u characters", MAX_TEXT);
    cap = 3u * (size_t)n + 1u;
    s = malloc(cap);
    if (s == NULL) rt_fatal("out of host memory (USER32 text)");
    r = wide ? rt_gstr_w(s, cap, g) : rt_gstr_a(s, cap, g);
    if (r != RT_STR_OK) rt_fatal("USER32: a guest string is not valid UTF-16 (unpaired surrogate)");
    return s;
}

/* A guest string as UTF-8 (malloc; NULL for g = 0). */
char *rt_user32_guest_text(uint32_t g, bool wide)
{
    return guest_text(g, wide);
}

/* UTF-8 to UTF-16 (malloc). */
static uint16_t *utf16_of(const char *s, uint32_t *n)
{
    size_t len = strlen(s), i = 0, o = 0;
    uint16_t *u = malloc(2u * (len + 1u));
    if (u == NULL) rt_fatal("out of host memory (USER32 text)");
    while (i < len)
    {
        uint32_t cp;
        int k = rt_utf8_decode((const uint8_t *)s + i, len - i, &cp);
        if (k <= 0) rt_fatal("USER32: runtime text is not valid UTF-8");
        i += (size_t)k;
        if (cp >= 0x10000u)
        {
            cp -= 0x10000u;
            u[o++] = (uint16_t)(0xD800u + (cp >> 10));
            u[o++] = (uint16_t)(0xDC00u + (cp & 0x3FFu));
        }
        else
            u[o++] = (uint16_t)cp;
    }
    u[o] = 0;
    *n = (uint32_t)o;
    return u;
}

/* Characters of s in the ANSI (A) or wide (W) form. */
static uint32_t text_length(const char *s, bool wide)
{
    uint32_t n;
    uint16_t *u = utf16_of(s, &n);
    if (!wide)
    {
        /* Windows-1252 has one byte per character; a surrogate pair is
           one character ('?'). */
        uint32_t i, a = 0;
        for (i = 0; i < n; i++, a++)
            if (u[i] >= 0xD800u && u[i] < 0xDC00u && i + 1u < n) i++;
        n = a;
    }
    free(u);
    return n;
}

/* Copy s to a guest buffer of cap characters (with the NUL), truncated
   as WM_GETTEXT does. Returns the characters copied without the NUL. */
static uint32_t copy_text(const char *s, uint32_t buf, uint32_t cap, bool wide)
{
    uint32_t n, i, o = 0;
    uint16_t *u;
    if (cap == 0 || buf == 0) return 0;
    u = utf16_of(s, &n);
    for (i = 0; i < n && o + 1u < cap; i++)
    {
        if (wide)
            wr16(buf + 2u * o++, u[i]);
        else
        {
            uint32_t cp = u[i];
            int b;
            if (cp >= 0xD800u && cp < 0xDC00u && i + 1u < n)
            {
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (u[i + 1u] - 0xDC00u);
                i++;
            }
            b = rt_unicode_to_cp1252(cp);
            wr8(buf + o++, (uint8_t)(b < 0 ? '?' : b));
        }
    }
    if (wide)
        wr16(buf + 2u * o, 0);
    else
        wr8(buf + o, 0);
    free(u);
    return o;
}

/* A new guest copy of s in the A or W form (rt_heap_alloc). */
static uint32_t guest_copy(const char *s, bool wide)
{
    uint32_t n = text_length(s, wide), size = (n + 1u) * (wide ? 2u : 1u), g;
    g = rt_heap_alloc(size);
    if (g == 0) rt_fatal("out of guest memory (USER32 text)");
    copy_text(s, g, n + 1u, wide);
    return g;
}

/* A character message parameter in the other form. */
static uint32_t char_to_wide(uint32_t c)
{
    return rt_cp1252_to_unicode((uint8_t)(c & 0xFFu));
}

static uint32_t char_to_ansi(uint32_t c)
{
    int b = rt_unicode_to_cp1252(c & 0xFFFFu);
    return (uint32_t)(b < 0 ? '?' : b);
}

/* ---------------------------------------------------------------- classes */

static int find_class(const char *name, uint16_t atom, uint32_t hinstance)
{
    int i, any = -1;
    for (i = 0; i < MAX_CLASSES; i++)
    {
        if (!classes[i].used) continue;
        if (atom != 0 ? classes[i].atom != atom : strcasecmp(classes[i].name, name) != 0) continue;
        if (classes[i].runtime || classes[i].hinstance == hinstance) return i;
        if (any < 0) any = i;
    }
    return any;
}

static int class_slot(void)
{
    int i;
    for (i = 0; i < MAX_CLASSES; i++)
        if (!classes[i].used) return i;
    rt_fatal("USER32: more than %d window classes", MAX_CLASSES);
}

void rt_user32_register_runtime_class(const char *name, rt_host_proc proc, uint32_t wnd_extra)
{
    int i;
    lock();
    if (find_class(name, 0, 0) >= 0) rt_fatal("USER32: runtime class %s is registered two times", name);
    i = class_slot();
    memset(&classes[i], 0, sizeof classes[i]);
    classes[i].used = true;
    classes[i].runtime = true;
    classes[i].unicode = true;
    snprintf(classes[i].name, sizeof classes[i].name, "%s", name);
    classes[i].atom = (uint16_t)(0xC000u + (uint32_t)i);
    classes[i].host = proc;
    classes[i].wnd_extra = wnd_extra;
    classes[i].style = CS_DBLCLKS;
    unlock();
}

static uint32_t register_class(uint32_t style, uint32_t proc, uint32_t cls_extra, uint32_t wnd_extra, uint32_t hinst,
                               uint32_t icon, uint32_t cursor, uint32_t brush, uint32_t menu, uint32_t name,
                               uint32_t icon_sm, bool unicode)
{
    char *text;
    int i;
    if (proc == 0 || name == 0 || (int32_t)cls_extra < 0 || (int32_t)wnd_extra < 0 || wnd_extra > MAX_WINDOW_EXTRA ||
        cls_extra > MAX_WINDOW_EXTRA)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (menu != 0) rt_fatal("RegisterClass: class menus are not supported (the runtime windows have no menus)");
    if (name < 0x10000u) rt_fatal("RegisterClass: a class name given as an atom is not supported");
    text = guest_text(name, unicode);
    if (strlen(text) >= sizeof classes[0].name) rt_fatal("RegisterClass: the class name is too long");
    lock();
    for (i = 0; i < MAX_CLASSES; i++)
    {
        if (classes[i].used && (classes[i].runtime || classes[i].hinstance == hinst) &&
            strcasecmp(classes[i].name, text) == 0)
        {
            unlock();
            free(text);
            rt_set_last_error(ERROR_CLASS_ALREADY_EXISTS);
            return 0;
        }
    }
    i = class_slot();
    memset(&classes[i], 0, sizeof classes[i]);
    classes[i].used = true;
    classes[i].unicode = unicode;
    snprintf(classes[i].name, sizeof classes[i].name, "%s", text);
    classes[i].atom = (uint16_t)(0xC000u + (uint32_t)i);
    classes[i].style = style;
    classes[i].wndproc = proc;
    classes[i].cls_extra = cls_extra;
    classes[i].wnd_extra = wnd_extra;
    classes[i].hinstance = hinst;
    classes[i].icon = icon;
    classes[i].cursor = cursor;
    classes[i].brush = brush;
    classes[i].icon_sm = icon_sm;
    unlock();
    free(text);
    return classes[i].atom;
}

/* ATOM RegisterClassW(const WNDCLASSW *) */
void rt_RegisterClassW(_cpu *cpu)
{
    uint32_t c = ARG(0);
    if (c == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    RET(register_class(rd32(c), rd32(c + 4), rd32(c + 8), rd32(c + 12), rd32(c + 16), rd32(c + 20), rd32(c + 24),
                       rd32(c + 28), rd32(c + 32), rd32(c + 36), 0, true));
}

/* ATOM RegisterClassExW(const WNDCLASSEXW *) */
void rt_RegisterClassExW(_cpu *cpu)
{
    uint32_t c = ARG(0);
    if (c == 0 || rd32(c) != 48)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    RET(register_class(rd32(c + 4), rd32(c + 8), rd32(c + 12), rd32(c + 16), rd32(c + 20), rd32(c + 24), rd32(c + 28),
                       rd32(c + 32), rd32(c + 36), rd32(c + 40), rd32(c + 44), true));
}

/* BOOL UnregisterClassW(LPCWSTR name, HINSTANCE) */
void rt_UnregisterClassW(_cpu *cpu)
{
    uint32_t name = ARG(0), hinst = ARG(1);
    char *text = name >= 0x10000u ? guest_text(name, true) : NULL;
    int i, j;
    lock();
    i = find_class(text != NULL ? text : "", name < 0x10000u ? (uint16_t)name : 0, hinst);
    free(text);
    if (i < 0 || classes[i].runtime || classes[i].hinstance != hinst)
    {
        unlock();
        rt_set_last_error(ERROR_CLASS_DOES_NOT_EXIST);
        RET(0);
        return;
    }
    for (j = 1; j < MAX_WINDOWS; j++)
    {
        if (windows[j].hwnd != 0 && windows[j].cls == i)
        {
            unlock();
            rt_set_last_error(ERROR_CLASS_HAS_WINDOWS);
            RET(0);
            return;
        }
    }
    classes[i].used = false;
    unlock();
    RET(1);
}

/* ---------------------------------------------------------------- geometry */

static bool titled(uint32_t style)
{
    return (style & WS_CHILD) == 0 && (style & WS_CAPTION) == WS_CAPTION;
}

static unsigned kind_of(const wnd *w)
{
    unsigned k = 0;
    if (titled(w->style))
    {
        k |= RT_WIN_TITLED;
        if (w->style & WS_SYSMENU) k |= RT_WIN_CLOSABLE;
        if (w->style & WS_MINIMIZEBOX) k |= RT_WIN_MINIMIZABLE;
        if (w->style & WS_MAXIMIZEBOX) k |= RT_WIN_ZOOMABLE;
        if (w->style & WS_THICKFRAME) k |= RT_WIN_RESIZABLE;
    }
    if (!classes[w->cls].runtime) k |= RT_WIN_METAL;
    if (w->exstyle & WS_EX_TOPMOST) k |= RT_WIN_TOPMOST;
    return k;
}

/* The client rectangle in the coordinates of wrect (lock held). */
static rt_rect client_of(const wnd *w)
{
    rt_rect c = w->wrect;
    if (w->style & WS_MINIMIZE) return (rt_rect){ c.left, c.top, c.left, c.top };
    if (w->parent == 0 && titled(w->style)) c.top += w->title_h;
    if (c.bottom < c.top) c.bottom = c.top;
    if (c.right < c.left) c.right = c.left;
    return c;
}

/* The screen position of the client origin of hwnd (lock held). */
static void client_origin(uint32_t hwnd, int32_t *x, int32_t *y)
{
    int32_t ox = 0, oy = 0;
    wnd *w = get(hwnd);
    while (w != NULL)
    {
        rt_rect c = client_of(w);
        ox += c.left;
        oy += c.top;
        w = w->parent != 0 ? get(w->parent) : NULL;
    }
    *x = ox;
    *y = oy;
}

static void ime_apply(uint32_t hwnd);

static rt_rect main_display_rect(void)
{
    rt_display d[RT_MAX_DISPLAYS];
    rt_cocoa_displays(d);
    return d[0].frame;
}

/* ---------------------------------------------------------------- calls to window procedures */

static rt_host_proc host_of(const wnd *w)
{
    return classes[w->cls].runtime ? classes[w->cls].host : NULL;
}

static uint32_t send_other(_cpu *cpu, uint32_t hwnd, uint32_t tid, uint32_t msg, uint32_t wp, uint32_t lp, bool unicode,
                           uint32_t smto, uint32_t timeout, bool *ok);

/* Call the window procedure of hwnd. The message is in the form of the
   sender (unicode); a guest procedure of the other form gets the
   character and text messages converted. */
static uint32_t call_proc(_cpu *cpu, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp, bool unicode)
{
    uint32_t proc, tid;
    rt_host_proc host;
    bool wuni;
    wnd *w;
    lock();
    w = get(hwnd);
    if (w == NULL)
    {
        unlock();
        return 0;
    }
    proc = w->wndproc;
    host = host_of(w);
    wuni = w->unicode;
    tid = w->tid;
    unlock();
    if (tid != self_tid()) return send_other(cpu, hwnd, tid, msg, wp, lp, unicode, 0, 0xFFFFFFFFu, NULL);
    if (host != NULL) return host(cpu, hwnd, msg, wp, lp, unicode);
    if (proc == 0) rt_fatal("USER32: window 0x%08x has no window procedure", hwnd);
    return rt_user32_call_proc(cpu, proc, hwnd, msg, wp, lp, unicode, wuni);
}

/* Call the guest procedure proc (charset wuni) with a message from a
   sender with charset unicode: the text messages are converted. */
uint32_t rt_user32_call_proc(_cpu *cpu, uint32_t proc, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp, bool unicode,
                             bool wuni)
{
    uint32_t r, tmp = 0;
    if (wuni != unicode)
    {
        switch (msg)
        {
        case WM_CHAR: case WM_DEADCHAR: case WM_SYSCHAR: case WM_SYSDEADCHAR: case WM_IME_CHAR:
            wp = wuni ? char_to_wide(wp) : char_to_ansi(wp);
            break;
        case WM_SETTEXT:
            if (lp != 0)
            {
                char *s = guest_text(lp, unicode);
                tmp = guest_copy(s, wuni);
                free(s);
                lp = tmp;
            }
            break;
        case WM_GETTEXT:
        {
            /* Get the text in the form of the procedure, then copy it to
               the buffer of the sender. */
            uint32_t buf = lp, cap = wp, n;
            char *s;
            if (cap == 0 || buf == 0) break;
            tmp = rt_heap_alloc((cap > 0x8000u ? cap : 0x8000u) * 2u + 2u);
            if (tmp == 0) rt_fatal("out of guest memory (WM_GETTEXT)");
            n = rt_call_cb(cpu, proc, 4, hwnd, msg, (cap > 0x8000u ? cap : 0x8000u), tmp);
            (void)n;
            s = guest_text(tmp, wuni);
            rt_heap_free(tmp);
            r = copy_text(s, buf, cap, unicode);
            free(s);
            return r;
        }
        case WM_GETTEXTLENGTH:
        {
            /* The length in the form of the sender (Windows can give a
               larger value; this one is exact). */
            uint32_t cap = rt_call_cb(cpu, proc, 4, hwnd, msg, 0, 0);
            char *s;
            if (cap > MAX_TEXT) cap = MAX_TEXT;
            tmp = rt_heap_alloc((cap + 1u) * 2u);
            if (tmp == 0) rt_fatal("out of guest memory (WM_GETTEXTLENGTH)");
            rt_call_cb(cpu, proc, 4, hwnd, WM_GETTEXT, cap + 1u, tmp);
            s = guest_text(tmp, wuni);
            rt_heap_free(tmp);
            r = text_length(s, unicode);
            free(s);
            return r;
        }
        default:
            break;
        }
    }
    r = rt_call_cb(cpu, proc, 4, hwnd, msg, wp, lp);
    if (tmp != 0) rt_heap_free(tmp);
    return r;
}

static uint32_t send_any(_cpu *cpu, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp, bool unicode, uint32_t smto,
                         uint32_t timeout, bool *ok);

uint32_t rt_user32_send(_cpu *cpu, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp, bool unicode)
{
    return send_any(cpu, hwnd, msg, wp, lp, unicode, 0, 0xFFFFFFFFu, NULL);
}

static uint32_t send0(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    bool u;
    wnd *w;
    lock();
    w = get(hwnd);
    u = w != NULL && w->unicode;
    unlock();
    if (w == NULL) return 0;
    return call_proc(rt_self()->cpu, hwnd, msg, wp, lp, u);
}

/* ---------------------------------------------------------------- native objects */

static void make_native(uint32_t hwnd);

/* Native objects of the children of hwnd (main thread). */
static void make_child_natives(uint32_t parent)
{
    uint32_t list[MAX_WINDOWS];
    uint64_t zs[MAX_WINDOWS];
    int i, n = 0, a, b;
    lock();
    for (i = 1; i < MAX_WINDOWS; i++)
    {
        if (windows[i].hwnd != 0 && windows[i].parent == parent && windows[i].ns == NULL && !windows[i].destroying)
        {
            list[n] = windows[i].hwnd;
            zs[n++] = windows[i].z;
        }
    }
    unlock();
    /* Bottom first: later views are above. */
    for (a = 1; a < n; a++)
    {
        for (b = a; b > 0 && zs[b - 1] > zs[b]; b--)
        {
            uint64_t tz = zs[b];
            uint32_t th = list[b];
            zs[b] = zs[b - 1];
            list[b] = list[b - 1];
            zs[b - 1] = tz;
            list[b - 1] = th;
        }
    }
    for (i = 0; i < n; i++) make_native(list[i]);
}

/* Make the native object of hwnd if its parent has one (main thread). */
static void make_native(uint32_t hwnd)
{
    rt_host_proc host;
    void *parent_view = NULL, *ns;
    char *title;
    rt_rect c;
    unsigned kind;
    bool visible, disabled, top;
    wnd *w;
    if (!on_main()) rt_fatal("USER32: window 0x%08x of another thread needs a native window; only the main thread "
                             "can have native windows",
                             hwnd);
    lock();
    w = get(hwnd);
    if (w == NULL || w->ns != NULL || w->destroying)
    {
        unlock();
        return;
    }
    if (w->tid != main_tid) rt_fatal("USER32: window 0x%08x of thread %u cannot have a native window (main thread only)",
                                     hwnd, w->tid);
    top = w->parent == 0;
    if (!top)
    {
        wnd *p = get(w->parent);
        if (p == NULL || p->ns == NULL)
        {
            unlock();
            return;
        }
        parent_view = rt_cocoa_content_view(p->ns);
    }
    host = host_of(w);
    kind = kind_of(w);
    c = top ? client_of(w) : w->wrect;
    title = xstrdup(w->text);
    visible = (w->style & WS_VISIBLE) != 0;
    disabled = (w->style & WS_DISABLED) != 0;
    unlock();
    if (top)
    {
        ns = rt_cocoa_window_create(hwnd, c, kind, title);
        if (disabled) rt_cocoa_window_enable(ns, false);
    }
    else if (host == NULL)
    {
        ns = rt_cocoa_view_create(hwnd, parent_view, RT_VIEW_METAL, c, "", RT_ALIGN_LEFT, false);
        rt_cocoa_view_set_visible(ns, visible);
    }
    else
        ns = NULL; /* the runtime class makes its view (WM_NULL with wp = "RTNV", below) */
    free(title);
    if (ns != NULL)
    {
        lock();
        w = get(hwnd);
        if (w != NULL) w->ns = ns;
        unlock();
        if (w == NULL) rt_fatal("USER32: window 0x%08x was destroyed while its native object was made", hwnd);
        ime_apply(hwnd);
    }
    else if (host != NULL && !top)
    {
        /* Runtime classes make their native views on this private request:
           WM_NULL with wp = 0x52544E56 ("RTNV"). */
        host(rt_self()->cpu, hwnd, WM_NULL, 0x52544E56u, 0, true);
    }
    make_child_natives(hwnd);
    if (top && visible)
    {
        lock();
        w = get(hwnd);
        ns = w != NULL ? w->ns : NULL;
        unlock();
        if (ns != NULL && !rt_cocoa_headless()) rt_cocoa_window_show(ns, true, false);
    }
}

/* Runtime classes: set the window text without messages (UTF-8). */
void rt_user32_set_text(uint32_t hwnd, const char *text)
{
    char *t = xstrdup(text);
    wnd *w;
    lock();
    w = get(hwnd);
    if (w != NULL)
    {
        free(w->text);
        w->text = t;
        t = NULL;
    }
    unlock();
    free(t);
}

/* The window rectangle of a child in the client coordinates of its
   parent (the screen rectangle of a top-level window). */
rt_rect rt_user32_child_rect(uint32_t hwnd)
{
    rt_rect r = { 0, 0, 0, 0 };
    wnd *w;
    lock();
    w = get(hwnd);
    if (w != NULL) r = w->wrect;
    unlock();
    return r;
}

void rt_user32_client_origin(uint32_t hwnd, int32_t *x, int32_t *y)
{
    lock();
    client_origin(hwnd, x, y);
    unlock();
}

static bool set_pos(_cpu *cpu, uint32_t hwnd, uint32_t after, int32_t x, int32_t y, int32_t cx, int32_t cy, uint32_t flags);

/* SetWindowPos for the runtime classes. */
bool rt_user32_set_pos(_cpu *cpu, uint32_t hwnd, uint32_t after, int32_t x, int32_t y, int32_t cx, int32_t cy, uint32_t flags)
{
    return set_pos(cpu, hwnd, after, x, y, cx, cy, flags);
}

/* Runtime classes: set the native view of a child window (rt_dialog.c). */
void rt_user32_set_native_view(uint32_t hwnd, void *view)
{
    wnd *w;
    lock();
    w = get(hwnd);
    if (w != NULL) w->ns = view;
    unlock();
}

void *rt_user32_native_view(uint32_t hwnd)
{
    void *ns;
    bool top;
    wnd *w;
    lock();
    w = get(hwnd);
    ns = w != NULL ? w->ns : NULL;
    top = w != NULL && w->parent == 0;
    unlock();
    if (ns == NULL) return NULL;
    return top ? rt_cocoa_content_view(ns) : ns;
}

/* The CAMetalLayer of a window (the renderer). Makes the native window if
   it does not exist yet. Main thread. */
void *rt_user32_metal_layer(uint32_t hwnd)
{
    uint32_t top = hwnd;
    void *ns;
    wnd *w;
    lock();
    w = get(hwnd);
    if (w == NULL)
    {
        unlock();
        return NULL;
    }
    while (w != NULL && w->parent != 0)
    {
        top = w->parent;
        w = get(w->parent);
    }
    unlock();
    make_native(top);
    if (top != hwnd) make_native(hwnd);
    lock();
    w = get(hwnd);
    ns = w != NULL ? w->ns : NULL;
    top = w != NULL && w->parent == 0;
    unlock();
    if (ns == NULL) return NULL;
    return top ? rt_cocoa_window_layer(ns) : rt_cocoa_window_layer(ns);
}

/* Update the native frame of hwnd from the Win32 state (main thread). */
static void native_frame(uint32_t hwnd)
{
    void *ns;
    rt_rect r;
    bool top;
    wnd *w;
    lock();
    w = get(hwnd);
    if (w == NULL || w->ns == NULL)
    {
        unlock();
        return;
    }
    ns = w->ns;
    top = w->parent == 0;
    r = top ? client_of(w) : w->wrect;
    unlock();
    if (top)
        rt_cocoa_window_set_client(ns, r);
    else
        rt_cocoa_view_set_frame(ns, r);
}

/* ---------------------------------------------------------------- activation and focus */

static void clear_input_state(void)
{
    int i;
    lock();
    memset(input_down, 0, sizeof input_down);
    for (i = 0; i < 256; i++) sync_state[i] &= 0x01;
    unlock();
}

static uint32_t top_of(uint32_t hwnd)
{
    wnd *w = get(hwnd);
    while (w != NULL && w->parent != 0)
    {
        hwnd = w->parent;
        w = get(hwnd);
    }
    return w != NULL ? hwnd : 0;
}

static uint32_t set_focus(_cpu *cpu, uint32_t hwnd);

/* Send msg to every top-level window of the calling thread. */
static void send_top_windows(uint32_t msg, uint32_t wp, uint32_t lp)
{
    uint32_t list[MAX_WINDOWS];
    int i, n = 0;
    lock();
    for (i = 1; i < MAX_WINDOWS; i++)
        if (windows[i].hwnd != 0 && windows[i].parent == 0 && windows[i].tid == self_tid() && !windows[i].destroying)
            list[n++] = windows[i].hwnd;
    unlock();
    for (i = 0; i < n; i++) send0(list[i], msg, wp, lp);
}

/* Activate the top-level window hwnd of the calling thread with the
   message order of Windows: WM_ACTIVATEAPP (when the application becomes
   active), WM_NCACTIVATE and WM_ACTIVATE to the old window, WM_NCACTIVATE
   and WM_ACTIVATE to the new window. DefWindowProc sets the focus. */
static void set_active(_cpu *cpu, uint32_t hwnd)
{
    uint32_t old;
    bool minimized, app_was_active;
    mqueue *q;
    wnd *w;
    (void)cpu;
    lock();
    q = queue_of(self_tid(), true);
    old = q->active;
    w = get(hwnd);
    if (w == NULL || w->destroying || w->parent != 0 || old == hwnd)
    {
        unlock();
        return;
    }
    minimized = (w->style & WS_MINIMIZE) != 0;
    q->active = hwnd;
    app_was_active = app_active;
    app_active = true;
    unlock();
    if (!app_was_active) send_top_windows(WM_ACTIVATEAPP, 1, 0);
    if (old != 0 && rt_user32_is_window(old))
    {
        send0(old, WM_NCACTIVATE, 0, 0);
        send0(old, WM_ACTIVATE, WA_INACTIVE, hwnd);
    }
    send0(hwnd, WM_NCACTIVATE, 1, 0);
    send0(hwnd, WM_ACTIVATE, WA_ACTIVE | ((minimized ? 1u : 0u) << 16), old);
}

/* Deactivate hwnd if it is the active window of the calling thread.
   app_too: the application is no longer active (WM_ACTIVATEAPP). */
static void deactivate(_cpu *cpu, uint32_t hwnd, bool app_too)
{
    bool minimized = false, was_active, send_app;
    uint32_t focus;
    mqueue *q;
    wnd *w;
    lock();
    q = queue_of(self_tid(), true);
    was_active = hwnd != 0 && q->active == hwnd;
    if (was_active)
    {
        q->active = 0;
        q->last_active = hwnd;
    }
    w = get(hwnd);
    if (w != NULL) minimized = (w->style & WS_MINIMIZE) != 0;
    send_app = app_too && app_active;
    if (app_too) app_active = false;
    focus = q->focus;
    unlock();
    if (!was_active && !send_app) return;
    /* Key-up events do not arrive after the focus goes to another
       application; release all keys so that no key stays down. */
    clear_input_state();
    if (was_active && w != NULL)
    {
        send0(hwnd, WM_NCACTIVATE, 0, 0);
        send0(hwnd, WM_ACTIVATE, WA_INACTIVE | ((minimized ? 1u : 0u) << 16), 0);
    }
    if (send_app) send_top_windows(WM_ACTIVATEAPP, 0, 0);
    if (focus != 0 && (!was_active || rt_user32_is_self_or_child(hwnd, focus) || send_app)) set_focus(cpu, 0);
}

static uint32_t set_focus(_cpu *cpu, uint32_t hwnd)
{
    uint32_t old, top;
    void *view = NULL;
    mqueue *q;
    wnd *w;
    lock();
    q = queue_of(self_tid(), true);
    old = q->focus;
    if (hwnd != 0)
    {
        w = get(hwnd);
        if (w == NULL)
        {
            unlock();
            rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
            return 0;
        }
        if (w->tid != self_tid())
        {
            unlock();
            rt_set_last_error(ERROR_WINDOW_OF_OTHER_THREAD);
            return 0;
        }
        if (old == hwnd)
        {
            unlock();
            return old;
        }
        top = top_of(hwnd);
        view = w->ns;
    }
    else
        top = 0;
    q->focus = hwnd;
    unlock();
    if (old != 0) send0(old, WM_KILLFOCUS, hwnd, 0);
    if (hwnd != 0)
    {
        bool active;
        lock();
        active = queue_of(self_tid(), true)->active == top;
        unlock();
        if (!active) set_active(cpu, top);
        send0(hwnd, WM_SETFOCUS, old, 0);
        if (view != NULL && on_main()) rt_cocoa_view_focus(top == hwnd ? rt_cocoa_content_view(view) : view);
    }
    return old;
}

uint32_t rt_user32_set_focus(_cpu *cpu, uint32_t hwnd)
{
    return set_focus(cpu, hwnd);
}

uint32_t rt_user32_focus(void)
{
    uint32_t f;
    lock();
    f = queue_of(self_tid(), true)->focus;
    unlock();
    return f;
}

/* HWND SetFocus(HWND) */
void rt_SetFocus(_cpu *cpu)
{
    RET(set_focus(cpu, ARG(0)));
}

/* HWND GetFocus(void) */
void rt_GetFocus(_cpu *cpu)
{
    RET(rt_user32_focus());
}

/* BOOL SetForegroundWindow(HWND) */
void rt_SetForegroundWindow(_cpu *cpu)
{
    uint32_t hwnd = ARG(0), top;
    void *ns;
    wnd *w;
    lock();
    top = top_of(hwnd);
    w = get(top);
    if (w == NULL || w->tid != self_tid())
    {
        unlock();
        RET(0);
        return;
    }
    unlock();
    if (on_main())
    {
        make_native(top);
        lock();
        w = get(top);
        ns = w != NULL ? w->ns : NULL;
        unlock();
        if (ns != NULL && !rt_cocoa_headless()) rt_cocoa_activate(ns);
    }
    set_active(cpu, top);
    RET(1);
}

/* ---------------------------------------------------------------- capture */

/* HWND SetCapture(HWND) */
void rt_SetCapture(_cpu *cpu)
{
    uint32_t hwnd = ARG(0), old;
    mqueue *q;
    lock();
    q = queue_of(self_tid(), true);
    if (get(hwnd) == NULL)
    {
        unlock();
        RET(0);
        return;
    }
    old = q->capture;
    q->capture = hwnd;
    unlock();
    if (old != 0 && old != hwnd) send0(old, WM_CAPTURECHANGED, 0, hwnd);
    RET(old);
}

/* BOOL ReleaseCapture(void) */
void rt_ReleaseCapture(_cpu *cpu)
{
    uint32_t old;
    mqueue *q;
    lock();
    q = queue_of(self_tid(), true);
    old = q->capture;
    q->capture = 0;
    unlock();
    if (old != 0) send0(old, WM_CAPTURECHANGED, 0, 0);
    RET(1);
}

/* ---------------------------------------------------------------- creation and destruction */

static void send_minmax(_cpu *cpu, uint32_t hwnd);

static int window_slot(void)
{
    int i;
    for (i = 1; i < MAX_WINDOWS; i++)
        if (windows[i].hwnd == 0) return i;
    rt_fatal("USER32: more than %d windows", MAX_WINDOWS - 1);
}

static bool show_window(_cpu *cpu, uint32_t hwnd, int cmd);
static bool destroy_window(_cpu *cpu, uint32_t hwnd);

uint32_t rt_user32_create(_cpu *cpu, uint32_t exstyle, const char *cls, const char *title, uint32_t style, int32_t x,
                          int32_t y, int32_t w, int32_t h, uint32_t parent, uint32_t id, uint32_t hinst, uint32_t param,
                          bool unicode)
{
    int ci, slot;
    uint32_t hwnd, cs, gname, gclass, owner = 0, extra;
    bool wuni, child = (style & WS_CHILD) != 0, use_default = false;
    rt_rect r;
    wnd *pw;

    if (parent == HWND_MESSAGE) rt_fatal("CreateWindowEx: message-only windows (HWND_MESSAGE) are not supported");
    lock();
    ci = find_class(cls, 0, hinst);
    if (ci < 0)
    {
        unlock();
        rt_set_last_error(ERROR_CANNOT_FIND_WND_CLASS);
        return 0;
    }
    if (parent == DESKTOP_HWND) parent = 0;
    if (parent != 0)
    {
        pw = get(parent);
        if (pw == NULL)
        {
            unlock();
            rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
            return 0;
        }
        if (!child)
        {
            /* A top-level window with an owner: the owner is the top-level
               window of parent. */
            owner = top_of(parent);
            parent = 0;
        }
        else if (pw->tid != self_tid())
            rt_fatal("CreateWindowEx: a child of a window of another thread is not supported");
    }
    else if (child)
    {
        unlock();
        rt_set_last_error(1406u); /* ERROR_TLW_WITH_WSCHILD */
        return 0;
    }
    slot = window_slot();
    wuni = classes[ci].runtime ? unicode : classes[ci].unicode;
    extra = classes[ci].wnd_extra;
    unlock();

    /* An overlapped window always has a title bar. */
    if (!child && !(style & WS_POPUP)) style |= WS_CAPTION | WS_CLIPSIBLINGS;
    if (child)
    {
        if ((uint32_t)x == CW_USEDEFAULT) x = y = 0;
        if ((uint32_t)w == CW_USEDEFAULT) w = h = 0;
        r = (rt_rect){ x, y, x + w, y + h };
    }
    else
    {
        rt_rect screen = main_display_rect();
        int32_t sw = screen.right - screen.left, sh = screen.bottom - screen.top;
        if ((uint32_t)w == CW_USEDEFAULT)
        {
            w = (style & WS_POPUP) ? 0 : sw * 3 / 4;
            h = (style & WS_POPUP) ? 0 : sh * 3 / 4;
        }
        if ((uint32_t)x == CW_USEDEFAULT)
        {
            use_default = true;
            x = (style & WS_POPUP) ? 0 : screen.left + (sw - w) / 2;
            y = (style & WS_POPUP) ? 0 : screen.top + (sh - h) / 2;
        }
        r = (rt_rect){ x, y, x + w, y + h };
    }
    if (w < 0 || h < 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }

    lock();
    if (windows[slot].hwnd != 0) slot = window_slot();
    generations[slot]++;
    hwnd = make_hwnd(slot);
    memset(&windows[slot], 0, sizeof windows[slot]);
    windows[slot].hwnd = hwnd;
    windows[slot].tid = self_tid();
    windows[slot].cls = ci;
    windows[slot].style = style & ~(WS_VISIBLE | WS_MINIMIZE | WS_MAXIMIZE);
    windows[slot].exstyle = exstyle;
    windows[slot].wndproc = classes[ci].wndproc;
    windows[slot].hinstance = hinst;
    windows[slot].id = child ? id : 0;
    windows[slot].parent = parent;
    windows[slot].owner = owner;
    windows[slot].unicode = wuni;
    windows[slot].z = ++z_counter;
    windows[slot].wrect = r;
    windows[slot].restore = r;
    windows[slot].title_h = rt_cocoa_title_height();
    windows[slot].text = xstrdup(title);
    windows[slot].ime = true; /* Windows gives every window the input context of its thread */
    mark_paint(&windows[slot]);
    if (extra != 0)
    {
        windows[slot].extra = xcalloc(1, extra);
        windows[slot].extra_size = extra;
    }
    queue_of(self_tid(), true);
    unlock();

    send_minmax(cpu, hwnd);

    /* CREATESTRUCT (48 bytes) for WM_NCCREATE and WM_CREATE. */
    cs = rt_heap_calloc(1, 48);
    gname = guest_copy(title != NULL ? title : "", wuni);
    gclass = guest_copy(cls, wuni);
    if (cs == 0) rt_fatal("out of guest memory (CREATESTRUCT)");
    wr32(cs, param);
    wr32(cs + 4, hinst);
    wr32(cs + 8, child ? id : 0);
    wr32(cs + 12, child ? parent : owner);
    wr32(cs + 16, (uint32_t)(r.bottom - r.top));
    wr32(cs + 20, (uint32_t)(r.right - r.left));
    wr32(cs + 24, (uint32_t)r.top);
    wr32(cs + 28, (uint32_t)r.left);
    wr32(cs + 32, style);
    wr32(cs + 36, gname);
    wr32(cs + 40, gclass);
    wr32(cs + 44, exstyle);
    if (call_proc(cpu, hwnd, WM_NCCREATE, 0, cs, wuni) == 0 || (int32_t)call_proc(cpu, hwnd, WM_CREATE, 0, cs, wuni) == -1)
    {
        rt_heap_free(cs);
        rt_heap_free(gname);
        rt_heap_free(gclass);
        destroy_window(cpu, hwnd);
        rt_set_last_error(1400u);
        return 0;
    }
    rt_heap_free(cs);
    rt_heap_free(gname);
    rt_heap_free(gclass);
    if (!rt_user32_is_window(hwnd)) return 0;

    /* WM_SIZE and WM_MOVE after WM_CREATE. */
    {
        rt_rect c;
        lock();
        pw = get(hwnd);
        c = pw != NULL ? client_of(pw) : (rt_rect){ 0, 0, 0, 0 };
        unlock();
        call_proc(cpu, hwnd, WM_SIZE, SIZE_RESTORED, make_lparam(c.right - c.left, c.bottom - c.top), wuni);
        call_proc(cpu, hwnd, WM_MOVE, 0, make_lparam(c.left, c.top), wuni);
    }
    if (child && on_main()) make_native(hwnd);
    if (style & (WS_MINIMIZE | WS_MAXIMIZE))
        show_window(cpu, hwnd, (style & WS_MINIMIZE) ? SW_SHOWMINNOACTIVE : SW_SHOWMAXIMIZED);
    if (style & WS_VISIBLE) show_window(cpu, hwnd, (use_default && (uint32_t)y != CW_USEDEFAULT && !child) ? SW_SHOW : SW_SHOW);
    return hwnd;
}

/* HWND CreateWindowExW(DWORD exstyle, LPCWSTR class, LPCWSTR title,
   DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu,
   HINSTANCE, LPVOID param) */
void rt_CreateWindowExW(_cpu *cpu)
{
    uint32_t cname = ARG(1), gtitle = ARG(2), style = ARG(3), menu = ARG(9);
    char *cls, *title;
    uint32_t hwnd;
    if (!(style & WS_CHILD) && menu != 0) rt_fatal("CreateWindowExW: windows with menus are not supported");
    if (cname == 0)
    {
        rt_set_last_error(ERROR_CANNOT_FIND_WND_CLASS);
        RET(0);
        return;
    }
    if (cname < 0x10000u)
    {
        int i;
        lock();
        for (i = 0; i < MAX_CLASSES; i++)
            if (classes[i].used && classes[i].atom == cname) break;
        cls = xstrdup(i < MAX_CLASSES ? classes[i].name : "");
        unlock();
    }
    else
        cls = guest_text(cname, true);
    title = guest_text(gtitle, true);
    hwnd = rt_user32_create(cpu, ARG(0), cls, title, style, (int32_t)ARG(4), (int32_t)ARG(5), (int32_t)ARG(6),
                            (int32_t)ARG(7), ARG(8), menu, ARG(10), ARG(11), true);
    free(cls);
    free(title);
    RET(hwnd);
}

static bool destroy_window(_cpu *cpu, uint32_t hwnd)
{
    uint32_t list[MAX_WINDOWS];
    int i, n = 0;
    void *ns;
    bool top;
    uint8_t *extra;
    char *text;
    mqueue *q;
    wnd *w;
    lock();
    w = get(hwnd);
    if (w == NULL || w->destroying)
    {
        unlock();
        return w != NULL;
    }
    if (w->tid != self_tid())
    {
        unlock();
        rt_set_last_error(ERROR_ACCESS_DENIED);
        return false;
    }
    w->destroying = true;
    top = w->parent == 0;
    /* Owned windows first, as on Windows. */
    for (i = 1; i < MAX_WINDOWS; i++)
        if (windows[i].hwnd != 0 && windows[i].owner == hwnd && !windows[i].destroying) list[n++] = windows[i].hwnd;
    unlock();
    for (i = 0; i < n; i++) destroy_window(cpu, list[i]);

    /* Hide the window, without activation of another window. */
    lock();
    w = get(hwnd);
    ns = w != NULL ? w->ns : NULL;
    if (w != NULL) w->style &= ~WS_VISIBLE;
    unlock();
    if (ns != NULL)
    {
        if (top)
            rt_cocoa_window_show(ns, false, false);
        else
            rt_cocoa_view_set_visible(ns, false);
    }
    lock();
    q = queue_of(self_tid(), true);
    {
        bool was_active = q->active == hwnd;
        bool had_focus = q->focus != 0 && (q->focus == hwnd || top_of(q->focus) == hwnd);
        if (had_focus) q->focus = 0;
        if (q->capture == hwnd) q->capture = 0;
        unlock();
        if (was_active) deactivate(cpu, hwnd, false);
        (void)had_focus;
    }

    call_proc(cpu, hwnd, WM_DESTROY, 0, 0, rt_user32_is_unicode(hwnd));
    /* The children, in creation order. */
    n = 0;
    lock();
    for (i = 1; i < MAX_WINDOWS; i++)
        if (windows[i].hwnd != 0 && windows[i].parent == hwnd && !windows[i].destroying) list[n++] = windows[i].hwnd;
    unlock();
    for (i = 0; i < n; i++) destroy_window(cpu, list[i]);
    call_proc(cpu, hwnd, WM_NCDESTROY, 0, 0, rt_user32_is_unicode(hwnd));

    lock();
    w = get(hwnd);
    if (w == NULL)
    {
        unlock();
        return true;
    }
    ns = w->ns;
    extra = w->extra;
    text = w->text;
    for (i = 0; i < MAX_QUEUES; i++)
    {
        if (queues[i].tid == 0) continue;
        ring_drop_window(&queues[i].posted, hwnd);
        if (queues[i].input.buf != NULL) ring_drop_window(&queues[i].input, hwnd);
        if (queues[i].last_key_valid && queues[i].last_key.hwnd == hwnd) queues[i].last_key_valid = false;
        if (queues[i].focus == hwnd) queues[i].focus = 0;
        if (queues[i].active == hwnd) queues[i].active = 0;
        if (queues[i].capture == hwnd) queues[i].capture = 0;
    }
    memset(w, 0, sizeof *w);
    unlock();
    free(extra);
    free(text);
    if (ns != NULL)
    {
        if (top)
            rt_cocoa_window_destroy(ns);
        else
            rt_cocoa_view_destroy(ns);
    }
    return true;
}

bool rt_user32_destroy(_cpu *cpu, uint32_t hwnd)
{
    return destroy_window(cpu, hwnd);
}

/* BOOL DestroyWindow(HWND) */
void rt_DestroyWindow(_cpu *cpu)
{
    uint32_t hwnd = ARG(0);
    if (!rt_user32_is_window(hwnd))
    {
        rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        RET(0);
        return;
    }
    RET(destroy_window(cpu, hwnd) ? 1u : 0u);
}

/* Windows destroys the windows of a thread that ends. The thread ends, so
   its window procedures are not called. */
void rt_user32_thread_exit(uint32_t tid)
{
    void *natives[MAX_WINDOWS];
    int i, n = 0;
    lock();
    for (i = 1; i < MAX_WINDOWS; i++)
    {
        if (windows[i].hwnd != 0 && windows[i].tid == tid)
        {
            if (windows[i].ns != NULL) natives[n++] = windows[i].ns;
            free(windows[i].extra);
            free(windows[i].text);
            memset(&windows[i], 0, sizeof windows[i]);
        }
    }
    for (i = 0; i < MAX_QUEUES; i++)
    {
        if (queues[i].tid == tid && tid != main_tid)
        {
            uint32_t k;
            for (k = 0; k < queues[i].sys_count; k++)
            {
                sysmsg *m = &queues[i].sys[(queues[i].sys_head + k) % SYS_LIMIT];
                free(m->text);
                if (m->reply != NULL)
                {
                    m->reply->done = true;
                    m->reply->result = 0;
                    if (--m->reply->refs == 0) free(m->reply);
                }
            }
            if (pthread_cond_broadcast(&sent_cond) != 0) rt_fatal("USER32: condition broadcast failed");
            free(queues[i].posted.buf);
            free(queues[i].sys);
            rt_object_unref(queues[i].event);
            memset(&queues[i], 0, sizeof queues[i]);
        }
    }
    unlock();
    if (n != 0) rt_fatal("USER32: thread %u ends with %d native windows (native windows belong to the main thread)", tid, n);
}

/* ---------------------------------------------------------------- position, size and visibility */

/* WM_GETMINMAXINFO: the track size limits (MINMAXINFO, 40 bytes). */
static void send_minmax(_cpu *cpu, uint32_t hwnd)
{
    uint32_t mmi = rt_heap_calloc(1, 40);
    rt_rect s = main_display_rect();
    bool child;
    wnd *w;
    (void)cpu;
    if (mmi == 0) rt_fatal("out of guest memory (MINMAXINFO)");
    lock();
    w = get(hwnd);
    child = w == NULL || w->parent != 0;
    unlock();
    if (child)
    {
        rt_heap_free(mmi);
        return;
    }
    wr32(mmi + 8, (uint32_t)(s.right - s.left));   /* ptMaxSize */
    wr32(mmi + 12, (uint32_t)(s.bottom - s.top));
    wr32(mmi + 16, (uint32_t)s.left);              /* ptMaxPosition */
    wr32(mmi + 20, (uint32_t)s.top);
    wr32(mmi + 24, 0);                             /* ptMinTrackSize */
    wr32(mmi + 28, 0);
    wr32(mmi + 32, 0x7FFFFFFFu);                   /* ptMaxTrackSize */
    wr32(mmi + 36, 0x7FFFFFFFu);
    send0(hwnd, WM_GETMINMAXINFO, 0, mmi);
    lock();
    w = get(hwnd);
    if (w != NULL)
    {
        w->min_w = (int32_t)rd32(mmi + 24);
        w->min_h = (int32_t)rd32(mmi + 28);
    }
    unlock();
    rt_heap_free(mmi);
}

/* Move, size, show or hide hwnd with WM_WINDOWPOSCHANGING and
   WM_WINDOWPOSCHANGED, as SetWindowPos does. */
static bool set_pos(_cpu *cpu, uint32_t hwnd, uint32_t after, int32_t x, int32_t y, int32_t cx, int32_t cy, uint32_t flags)
{
    uint32_t wp;
    rt_rect old, nr;
    bool top, was_visible, show, hide;
    void *ns;
    wnd *w;
    lock();
    w = get(hwnd);
    if (w == NULL)
    {
        unlock();
        rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        return false;
    }
    old = w->wrect;
    top = w->parent == 0;
    was_visible = (w->style & WS_VISIBLE) != 0;
    unlock();
    if (flags & SWP_NOMOVE)
    {
        x = old.left;
        y = old.top;
    }
    if (flags & SWP_NOSIZE)
    {
        cx = old.right - old.left;
        cy = old.bottom - old.top;
    }
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;

    wp = rt_heap_calloc(1, 28);
    if (wp == 0) rt_fatal("out of guest memory (WINDOWPOS)");
    wr32(wp, hwnd);
    wr32(wp + 4, after);
    wr32(wp + 8, (uint32_t)x);
    wr32(wp + 12, (uint32_t)y);
    wr32(wp + 16, (uint32_t)cx);
    wr32(wp + 20, (uint32_t)cy);
    wr32(wp + 24, flags);
    if (!(flags & SWP_NOSENDCHANGING))
    {
        send0(hwnd, WM_WINDOWPOSCHANGING, 0, wp);
        flags = rd32(wp + 24);
        if (!(flags & SWP_NOMOVE))
        {
            x = (int32_t)rd32(wp + 8);
            y = (int32_t)rd32(wp + 12);
        }
        if (!(flags & SWP_NOSIZE))
        {
            cx = (int32_t)rd32(wp + 16);
            cy = (int32_t)rd32(wp + 20);
        }
        after = rd32(wp + 4);
    }
    nr = (rt_rect){ x, y, x + cx, y + cy };
    show = (flags & SWP_SHOWWINDOW) && !was_visible;
    hide = (flags & SWP_HIDEWINDOW) && was_visible;

    lock();
    w = get(hwnd);
    if (w == NULL)
    {
        unlock();
        rt_heap_free(wp);
        return false;
    }
    {
        rt_rect oc = client_of(w);
        w->wrect = nr;
        if (!(w->style & (WS_MINIMIZE | WS_MAXIMIZE))) w->restore = nr;
        rt_rect ncl = client_of(w);
        if (ncl.left == oc.left && ncl.top == oc.top) flags |= SWP_NOCLIENTMOVE;
        if (ncl.right - ncl.left == oc.right - oc.left && ncl.bottom - ncl.top == oc.bottom - oc.top) flags |= SWP_NOCLIENTSIZE;
        if (!(flags & SWP_NOSIZE) && (cx != old.right - old.left || cy != old.bottom - old.top)) mark_paint(w);
    }
    if (show)
    {
        w->style |= WS_VISIBLE;
        mark_paint(w);
    }
    if (hide) w->style &= ~WS_VISIBLE;
    if (!(flags & SWP_NOZORDER))
    {
        if (after == HWND_TOPMOST) w->exstyle |= WS_EX_TOPMOST;
        if (after == HWND_NOTOPMOST) w->exstyle &= ~WS_EX_TOPMOST;
        if (after == HWND_BOTTOM)
            w->z = 0;
        else if (after == HWND_TOP || after == HWND_TOPMOST || after == HWND_NOTOPMOST)
            w->z = ++z_counter;
        else
        {
            wnd *a = get(after);
            if (a != NULL) w->z = a->z == 0 ? 0 : a->z - 1u;
        }
    }
    ns = w->ns;
    unlock();

    if (on_main())
    {
        if (ns == NULL && (show || (top && was_visible && !hide)))
        {
            make_native(top ? hwnd : top_of(hwnd));
            lock();
            w = get(hwnd);
            ns = w != NULL ? w->ns : NULL;
            unlock();
        }
        if (ns != NULL)
        {
            native_frame(hwnd);
            if (top)
            {
                lock();
                w = get(hwnd);
                unsigned kind = w != NULL ? kind_of(w) : 0;
                unlock();
                if (!(flags & SWP_NOZORDER) || (flags & SWP_FRAMECHANGED)) rt_cocoa_window_set_kind(ns, kind);
                if (show && !rt_cocoa_headless()) rt_cocoa_window_show(ns, true, !(flags & SWP_NOACTIVATE));
                if (hide) rt_cocoa_window_show(ns, false, false);
                if (!show && !hide && !(flags & SWP_NOZORDER) && was_visible && !rt_cocoa_headless())
                    rt_cocoa_window_show(ns, true, false);
            }
            else
            {
                if (show) rt_cocoa_view_set_visible(ns, true);
                if (hide) rt_cocoa_view_set_visible(ns, false);
                if (!(flags & SWP_NOZORDER)) rt_cocoa_view_set_frame(ns, nr); /* re-adds the view on top */
            }
        }
    }
    else if (ns != NULL)
        rt_fatal("SetWindowPos: the window 0x%08x has a native window and the call is not on the main thread", hwnd);

    wr32(wp + 8, (uint32_t)x);
    wr32(wp + 12, (uint32_t)y);
    wr32(wp + 16, (uint32_t)cx);
    wr32(wp + 20, (uint32_t)cy);
    wr32(wp + 24, flags);
    send0(hwnd, WM_WINDOWPOSCHANGED, 0, wp);
    rt_heap_free(wp);

    if (top && !(flags & SWP_NOACTIVATE) && (show || (!(flags & SWP_NOZORDER) && was_visible && !hide)))
        set_active(cpu, hwnd);
    if (hide && top) deactivate(cpu, hwnd, false);
    return true;
}

/* BOOL SetWindowPos(HWND, HWND after, int x, int y, int cx, int cy,
   UINT flags) */
void rt_SetWindowPos(_cpu *cpu)
{
    uint32_t flags = ARG(6);
    if (flags & ~(SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOREDRAW | SWP_NOACTIVATE | SWP_FRAMECHANGED |
                  SWP_SHOWWINDOW | SWP_HIDEWINDOW | SWP_NOCOPYBITS | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING |
                  SWP_DEFERERASE | SWP_ASYNCWINDOWPOS))
    {
        rt_set_last_error(1004u); /* ERROR_INVALID_FLAGS */
        RET(0);
        return;
    }
    RET(set_pos(cpu, ARG(0), ARG(1), (int32_t)ARG(2), (int32_t)ARG(3), (int32_t)ARG(4), (int32_t)ARG(5),
                flags & ~SWP_ASYNCWINDOWPOS)
            ? 1u
            : 0u);
}

/* The work area of the display that holds most of r. */
static rt_rect work_area_for(rt_rect r)
{
    rt_display d[RT_MAX_DISPLAYS];
    int n = rt_cocoa_displays(d), i, best = 0;
    int64_t best_area = -1;
    for (i = 0; i < n; i++)
    {
        int64_t w = (int64_t)(r.right < d[i].frame.right ? r.right : d[i].frame.right) -
                    (r.left > d[i].frame.left ? r.left : d[i].frame.left);
        int64_t h = (int64_t)(r.bottom < d[i].frame.bottom ? r.bottom : d[i].frame.bottom) -
                    (r.top > d[i].frame.top ? r.top : d[i].frame.top);
        int64_t a = (w > 0 && h > 0) ? w * h : 0;
        if (a > best_area)
        {
            best_area = a;
            best = i;
        }
    }
    return d[best].work;
}

static bool show_window(_cpu *cpu, uint32_t hwnd, int cmd)
{
    bool was_visible, show, activate, top;
    uint32_t style, new_style, flags = SWP_NOZORDER | SWP_FRAMECHANGED;
    rt_rect r, restore;
    void *ns;
    wnd *w;
    lock();
    w = get(hwnd);
    if (w == NULL)
    {
        unlock();
        rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        return false;
    }
    style = w->style;
    was_visible = (style & WS_VISIBLE) != 0;
    r = w->wrect;
    restore = w->restore;
    top = w->parent == 0;
    ns = w->ns;
    unlock();
    new_style = style;
    switch (cmd)
    {
    case SW_HIDE:
        if (!was_visible) return false;
        show = false;
        activate = false;
        break;
    case SW_SHOWNORMAL: case SW_RESTORE: case SW_SHOWDEFAULT:
        show = true;
        activate = true;
        if (style & (WS_MINIMIZE | WS_MAXIMIZE))
        {
            new_style &= ~(WS_MINIMIZE | WS_MAXIMIZE);
            r = restore;
        }
        break;
    case SW_SHOW:
        show = true;
        activate = true;
        break;
    case SW_SHOWNOACTIVATE: case SW_SHOWNA:
        show = true;
        activate = false;
        if (cmd == SW_SHOWNOACTIVATE && (style & (WS_MINIMIZE | WS_MAXIMIZE)))
        {
            new_style &= ~(WS_MINIMIZE | WS_MAXIMIZE);
            r = restore;
        }
        break;
    case SW_SHOWMAXIMIZED:
        show = true;
        activate = true;
        new_style = (new_style & ~WS_MINIMIZE) | WS_MAXIMIZE;
        if (top) r = work_area_for(r);
        break;
    case SW_SHOWMINIMIZED: case SW_MINIMIZE: case SW_SHOWMINNOACTIVE: case SW_FORCEMINIMIZE:
        if (top && !(style & WS_MINIMIZE)) rt_log("USER32: ShowWindow(0x%08x, %d): the program minimizes the window", hwnd, cmd);
        show = true;
        activate = cmd == SW_SHOWMINIMIZED;
        new_style = (new_style & ~WS_MAXIMIZE) | WS_MINIMIZE;
        break;
    default:
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        return false;
    }
    if (show != was_visible) send0(hwnd, WM_SHOWWINDOW, show ? 1u : 0u, 0);
    if (!rt_user32_is_window(hwnd)) return was_visible;
    lock();
    w = get(hwnd);
    if (w != NULL)
    {
        if ((new_style ^ style) & (WS_MINIMIZE | WS_MAXIMIZE))
        {
            if (!(style & (WS_MINIMIZE | WS_MAXIMIZE))) w->restore = w->wrect;
            w->style = (w->style & ~(WS_MINIMIZE | WS_MAXIMIZE)) | (new_style & (WS_MINIMIZE | WS_MAXIMIZE));
            flags |= SWP_STATECHANGED;
        }
        else
            flags |= SWP_NOMOVE | SWP_NOSIZE;
    }
    unlock();
    if (!activate) flags |= SWP_NOACTIVATE;
    flags |= show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
    set_pos(cpu, hwnd, 0, r.left, r.top, r.right - r.left, r.bottom - r.top, flags & ~SWP_STATECHANGED);
    if (top && on_main())
    {
        lock();
        w = get(hwnd);
        ns = w != NULL ? w->ns : NULL;
        unlock();
        if (ns != NULL && !rt_cocoa_headless() && ((new_style ^ style) & WS_MINIMIZE))
            rt_cocoa_window_minimize(ns, (new_style & WS_MINIMIZE) != 0);
    }
    /* ShowWindow activates the window when it restores a minimized window.
       set_pos does not, because the window is already visible and the call
       has SWP_NOZORDER. */
    if (top && activate && (style & WS_MINIMIZE) && !(new_style & WS_MINIMIZE) && rt_user32_is_window(hwnd))
    {
        if (ns != NULL && on_main() && !rt_cocoa_headless()) rt_cocoa_window_show(ns, true, true);
        set_active(cpu, hwnd);
    }
    return was_visible;
}

bool rt_user32_show(_cpu *cpu, uint32_t hwnd, int cmd)
{
    return show_window(cpu, hwnd, cmd);
}

/* BOOL ShowWindow(HWND, int cmd): returns whether the window was visible. */
void rt_ShowWindow(_cpu *cpu)
{
    RET(show_window(cpu, ARG(0), (int)ARG(1)) ? 1u : 0u);
}

/* BOOL EnableWindow(HWND, BOOL enable): returns whether the window was
   disabled. */
bool rt_user32_enable(_cpu *cpu, uint32_t hwnd, bool enable)
{
    bool was_disabled, top;
    uint32_t focus;
    void *ns;
    wnd *w;
    lock();
    w = get(hwnd);
    if (w == NULL)
    {
        unlock();
        rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        return false;
    }
    was_disabled = (w->style & WS_DISABLED) != 0;
    if (was_disabled == !enable)
    {
        unlock();
        return was_disabled;
    }
    if (enable)
        w->style &= ~WS_DISABLED;
    else
        w->style |= WS_DISABLED;
    ns = w->ns;
    top = w->parent == 0;
    focus = queue_of(self_tid(), true)->focus;
    unlock();
    if (!enable && focus != 0 && rt_user32_is_self_or_child(hwnd, focus)) set_focus(cpu, 0);
    if (ns != NULL && top) rt_cocoa_window_enable(ns, enable);
    send0(hwnd, WM_ENABLE, enable ? 1u : 0u, 0);
    return was_disabled;
}

void rt_EnableWindow(_cpu *cpu)
{
    RET(rt_user32_enable(cpu, ARG(0), ARG(1) != 0) ? 1u : 0u);
}

/* BOOL SetWindowTextW(HWND, LPCWSTR) */
void rt_SetWindowTextW(_cpu *cpu)
{
    uint32_t hwnd = ARG(0);
    if (!rt_user32_is_window(hwnd))
    {
        rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        RET(0);
        return;
    }
    RET(call_proc(cpu, hwnd, WM_SETTEXT, 0, ARG(1), true));
}

/* BOOL ValidateRect(HWND, const RECT *) */
void rt_ValidateRect(_cpu *cpu)
{
    uint32_t hwnd = ARG(0), r = ARG(1);
    int i;
    lock();
    if (hwnd == 0)
    {
        for (i = 1; i < MAX_WINDOWS; i++)
            if (windows[i].hwnd != 0 && windows[i].tid == self_tid()) windows[i].need_paint = false;
        unlock();
        RET(1);
        return;
    }
    {
        wnd *w = get(hwnd);
        if (w == NULL)
        {
            unlock();
            rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
            RET(0);
            return;
        }
        if (r == 0)
            w->need_paint = false;
        else
        {
            /* The runtime keeps one invalid flag for the whole client area:
               a rectangle that covers it validates the window. A smaller
               rectangle leaves the rest invalid, as on Windows. */
            rt_rect c = client_of(w);
            if ((int32_t)rd32(r) <= 0 && (int32_t)rd32(r + 4) <= 0 && (int32_t)rd32(r + 8) >= c.right - c.left &&
                (int32_t)rd32(r + 12) >= c.bottom - c.top)
                w->need_paint = false;
        }
    }
    unlock();
    RET(1);
}

/* ---------------------------------------------------------------- window queries */

bool rt_user32_is_window(uint32_t hwnd)
{
    bool ok;
    lock();
    ok = get(hwnd) != NULL;
    unlock();
    return ok;
}

uint32_t rt_user32_parent(uint32_t hwnd)
{
    uint32_t p;
    wnd *w;
    lock();
    w = get(hwnd);
    p = w != NULL ? w->parent : 0;
    unlock();
    return p;
}

uint32_t rt_user32_style(uint32_t hwnd)
{
    uint32_t s;
    wnd *w;
    lock();
    w = get(hwnd);
    s = w != NULL ? w->style : 0;
    unlock();
    return s;
}

uint32_t rt_user32_id(uint32_t hwnd)
{
    uint32_t s;
    wnd *w;
    lock();
    w = get(hwnd);
    s = w != NULL ? w->id : 0;
    unlock();
    return s;
}

bool rt_user32_is_unicode(uint32_t hwnd)
{
    bool u;
    wnd *w;
    lock();
    w = get(hwnd);
    u = w != NULL && w->unicode;
    unlock();
    return u;
}

/* The children of parent in creation order (lowest z first). Returns the
   number of children (at most max are written). */
int rt_user32_children(uint32_t parent, uint32_t *out, int max)
{
    uint64_t zs[256];
    int n = 0, i, j;
    if (max > 256) max = 256;
    lock();
    for (i = 1; i < MAX_WINDOWS; i++)
    {
        if (windows[i].hwnd == 0 || windows[i].parent != parent || windows[i].destroying || parent == 0) continue;
        if (n == max) rt_fatal("USER32: window 0x%08x has more than %d children", parent, max);
        /* Insertion by z. */
        for (j = n; j > 0 && zs[j - 1] > windows[i].z; j--)
        {
            zs[j] = zs[j - 1];
            out[j] = out[j - 1];
        }
        zs[j] = windows[i].z;
        out[j] = windows[i].hwnd;
        n++;
    }
    unlock();
    return n;
}

uint32_t rt_user32_child_by_id(uint32_t parent, uint32_t id)
{
    uint32_t r = 0;
    uint64_t z = 0;
    int i;
    lock();
    for (i = 1; i < MAX_WINDOWS; i++)
    {
        /* The first child in creation order (the lowest z of the
           children that keep their creation order). */
        if (windows[i].hwnd != 0 && windows[i].parent == parent && windows[i].id == id && !windows[i].destroying &&
            (r == 0 || windows[i].z < z))
        {
            r = windows[i].hwnd;
            z = windows[i].z;
        }
    }
    unlock();
    return r;
}

bool rt_user32_is_self_or_child(uint32_t parent, uint32_t hwnd)
{
    bool r = false;
    wnd *w;
    lock();
    w = get(hwnd);
    while (w != NULL)
    {
        if (hwnd == parent)
        {
            r = true;
            break;
        }
        hwnd = w->parent;
        w = hwnd != 0 ? get(hwnd) : NULL;
    }
    unlock();
    return r;
}

void *rt_user32_class_data(uint32_t hwnd)
{
    void *d;
    wnd *w;
    lock();
    w = get(hwnd);
    d = w != NULL ? w->class_data : NULL;
    unlock();
    return d;
}

void rt_user32_set_class_data(uint32_t hwnd, void *data)
{
    wnd *w;
    lock();
    w = get(hwnd);
    if (w != NULL) w->class_data = data;
    unlock();
}

char *rt_user32_text(uint32_t hwnd)
{
    char *t;
    wnd *w;
    lock();
    w = get(hwnd);
    t = xstrdup(w != NULL ? w->text : "");
    unlock();
    return t;
}

rt_rect rt_user32_client_rect(uint32_t hwnd)
{
    rt_rect c = { 0, 0, 0, 0 };
    wnd *w;
    lock();
    w = get(hwnd);
    if (w != NULL)
    {
        rt_rect k = client_of(w);
        c.right = k.right - k.left;
        c.bottom = k.bottom - k.top;
    }
    else if (hwnd == DESKTOP_HWND)
    {
        unlock();
        c = main_display_rect();
        return (rt_rect){ 0, 0, c.right - c.left, c.bottom - c.top };
    }
    unlock();
    return c;
}

rt_rect rt_user32_screen_rect(uint32_t hwnd)
{
    rt_rect r = { 0, 0, 0, 0 };
    wnd *w;
    lock();
    w = get(hwnd);
    if (w != NULL)
    {
        int32_t ox = 0, oy = 0;
        r = w->wrect;
        if (w->parent != 0) client_origin(w->parent, &ox, &oy);
        r.left += ox;
        r.right += ox;
        r.top += oy;
        r.bottom += oy;
    }
    unlock();
    if (w == NULL && hwnd == DESKTOP_HWND) r = main_display_rect();
    return r;
}

bool rt_user32_get_extra(uint32_t hwnd, uint32_t offset, uint32_t *value)
{
    bool ok;
    wnd *w;
    lock();
    w = get(hwnd);
    ok = w != NULL && offset <= w->extra_size && w->extra_size - offset >= 4u;
    if (ok) memcpy(value, w->extra + offset, 4);
    unlock();
    return ok;
}

bool rt_user32_set_extra(uint32_t hwnd, uint32_t offset, uint32_t value)
{
    bool ok;
    wnd *w;
    lock();
    w = get(hwnd);
    ok = w != NULL && offset <= w->extra_size && w->extra_size - offset >= 4u;
    if (ok) memcpy(w->extra + offset, &value, 4);
    unlock();
    return ok;
}

static void write_rect(uint32_t g, rt_rect r)
{
    wr32(g, (uint32_t)r.left);
    wr32(g + 4, (uint32_t)r.top);
    wr32(g + 8, (uint32_t)r.right);
    wr32(g + 12, (uint32_t)r.bottom);
}

/* BOOL GetClientRect(HWND, LPRECT) */
void rt_GetClientRect(_cpu *cpu)
{
    uint32_t hwnd = ARG(0), r = ARG(1);
    if (r == 0 || (!rt_user32_is_window(hwnd) && hwnd != DESKTOP_HWND))
    {
        rt_set_last_error(r == 0 ? ERROR_INVALID_PARAMETER : ERROR_INVALID_WINDOW_HANDLE);
        RET(0);
        return;
    }
    write_rect(r, rt_user32_client_rect(hwnd));
    RET(1);
}

/* BOOL GetWindowRect(HWND, LPRECT) */
void rt_GetWindowRect(_cpu *cpu)
{
    uint32_t hwnd = ARG(0), r = ARG(1);
    if (r == 0 || (!rt_user32_is_window(hwnd) && hwnd != DESKTOP_HWND))
    {
        rt_set_last_error(r == 0 ? ERROR_INVALID_PARAMETER : ERROR_INVALID_WINDOW_HANDLE);
        RET(0);
        return;
    }
    write_rect(r, rt_user32_screen_rect(hwnd));
    RET(1);
}

static bool client_screen(uint32_t hwnd, uint32_t p, int sign)
{
    int32_t ox = 0, oy = 0;
    lock();
    if (get(hwnd) == NULL && hwnd != DESKTOP_HWND)
    {
        unlock();
        rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        return false;
    }
    if (hwnd != DESKTOP_HWND) client_origin(hwnd, &ox, &oy);
    unlock();
    wr32(p, (uint32_t)((int32_t)rd32(p) + sign * ox));
    wr32(p + 4, (uint32_t)((int32_t)rd32(p + 4) + sign * oy));
    return true;
}

/* BOOL ClientToScreen(HWND, LPPOINT) */
void rt_ClientToScreen(_cpu *cpu)
{
    if (ARG(1) == 0)
    {
        RET(0);
        return;
    }
    RET(client_screen(ARG(0), ARG(1), 1) ? 1u : 0u);
}

/* BOOL ScreenToClient(HWND, LPPOINT) */
void rt_ScreenToClient(_cpu *cpu)
{
    if (ARG(1) == 0)
    {
        RET(0);
        return;
    }
    RET(client_screen(ARG(0), ARG(1), -1) ? 1u : 0u);
}

/* HWND GetForegroundWindow(void): the active window of the thread that has
   one, while the application is active. When another application is active,
   the foreground window belongs to another process, which the emulated
   system does not have: the result is NULL, as Windows gives in some
   states. */
void rt_GetForegroundWindow(_cpu *cpu)
{
    uint32_t h = 0;
    int i;
    lock();
    if (app_active)
        for (i = 0; i < MAX_QUEUES && h == 0; i++)
            if (queues[i].tid != 0 && queues[i].active != 0 && get(queues[i].active) != NULL) h = queues[i].active;
    unlock();
    RET(h);
}

/* HWND FindWindowA(LPCSTR class, LPCSTR title): the top-level window
   (highest in the z order) with the class (a name or an atom in the low
   word; NULL: any class) and the title (NULL: any title). The comparisons
   ignore case. */
void rt_FindWindowA(_cpu *cpu)
{
    uint32_t gc = ARG(0), gt = ARG(1), h = 0;
    char *cls = (gc >> 16) != 0 ? guest_text(gc, false) : NULL;
    char *title = guest_text(gt, false);
    uint64_t best_z = 0;
    int i;
    lock();
    for (i = 1; i < MAX_WINDOWS; i++)
    {
        const wnd *w = &windows[i];
        if (w->hwnd == 0 || w->destroying || w->parent != 0) continue;
        if (gc != 0 && (cls != NULL ? strcasecmp(classes[w->cls].name, cls) != 0 : classes[w->cls].atom != (gc & 0xFFFFu)))
            continue;
        if (title != NULL && strcasecmp(w->text != NULL ? w->text : "", title) != 0) continue;
        if (h == 0 || w->z > best_z)
        {
            h = w->hwnd;
            best_z = w->z;
        }
    }
    unlock();
    free(cls);
    free(title);
    RET(h);
}

/* HWND GetDesktopWindow(void) */
void rt_GetDesktopWindow(_cpu *cpu)
{
    RET(DESKTOP_HWND);
}

/* HWND GetParent(HWND): the parent of a child window, the owner of a
   pop-up window, else NULL. */
void rt_GetParent(_cpu *cpu)
{
    uint32_t r = 0;
    wnd *w;
    lock();
    w = get(ARG(0));
    if (w == NULL)
    {
        unlock();
        rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        RET(0);
        return;
    }
    if (w->style & WS_CHILD)
        r = w->parent;
    else if (w->style & WS_POPUP)
        r = w->owner;
    unlock();
    RET(r);
}

/* HWND GetAncestor(HWND, UINT flags) */
void rt_GetAncestor(_cpu *cpu)
{
    uint32_t hwnd = ARG(0), flags = ARG(1), r = 0;
    wnd *w;
    lock();
    w = get(hwnd);
    if (w == NULL)
    {
        unlock();
        RET(0);
        return;
    }
    switch (flags)
    {
    case GA_PARENT: r = w->parent != 0 ? w->parent : DESKTOP_HWND; break;
    case GA_ROOT: r = top_of(hwnd); break;
    case GA_ROOTOWNER:
        r = top_of(hwnd);
        while (get(r) != NULL && get(r)->owner != 0) r = get(r)->owner;
        break;
    default:
        r = 0;
    }
    unlock();
    RET(r);
}

/* BOOL IsIconic(HWND) */
void rt_IsIconic(_cpu *cpu)
{
    RET((rt_user32_style(ARG(0)) & WS_MINIMIZE) ? 1u : 0u);
}

/* BOOL IsWindowVisible(HWND): the window and all its parents have
   WS_VISIBLE. */
void rt_IsWindowVisible(_cpu *cpu)
{
    uint32_t hwnd = ARG(0);
    bool vis = false;
    wnd *w;
    lock();
    w = get(hwnd);
    while (w != NULL)
    {
        vis = (w->style & WS_VISIBLE) != 0;
        if (!vis || w->parent == 0) break;
        w = get(w->parent);
    }
    unlock();
    RET(vis ? 1u : 0u);
}

/* BOOL GetWindowPlacement(HWND, WINDOWPLACEMENT *) */
void rt_GetWindowPlacement(_cpu *cpu)
{
    uint32_t p = ARG(1);
    wnd *w;
    if (p == 0)
    {
        RET(0);
        return;
    }
    lock();
    w = get(ARG(0));
    if (w == NULL)
    {
        unlock();
        rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        RET(0);
        return;
    }
    wr32(p, 44);
    wr32(p + 4, 0);
    wr32(p + 8, (w->style & WS_MINIMIZE) ? (uint32_t)SW_SHOWMINIMIZED : (w->style & WS_MAXIMIZE) ? (uint32_t)SW_SHOWMAXIMIZED
                                                                                              : (uint32_t)SW_SHOWNORMAL);
    wr32(p + 12, 0xFFFFFFFFu);
    wr32(p + 16, 0xFFFFFFFFu);
    wr32(p + 20, 0xFFFFFFFFu);
    wr32(p + 24, 0xFFFFFFFFu);
    write_rect(p + 28, (w->style & (WS_MINIMIZE | WS_MAXIMIZE)) ? w->restore : w->wrect);
    unlock();
    RET(1);
}

static bool contains(rt_rect r, int32_t x, int32_t y)
{
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

/* HWND WindowFromPoint(POINT): the visible, enabled window at the screen
   point (the deepest child). */
void rt_WindowFromPoint(_cpu *cpu)
{
    int32_t x = (int32_t)ARG(0), y = (int32_t)ARG(1);
    uint32_t best = 0, parent = 0;
    int i;
    lock();
    for (;;)
    {
        uint32_t found = 0;
        uint64_t z = 0;
        int32_t ox = 0, oy = 0;
        if (parent != 0) client_origin(parent, &ox, &oy);
        for (i = 1; i < MAX_WINDOWS; i++)
        {
            wnd *w = &windows[i];
            rt_rect r;
            if (w->hwnd == 0 || w->destroying || w->parent != parent || !(w->style & WS_VISIBLE)) continue;
            r = w->wrect;
            if (!contains(r, x - ox, y - oy)) continue;
            if (found == 0 || w->z >= z)
            {
                found = w->hwnd;
                z = w->z;
            }
        }
        if (found == 0) break;
        if (get(found)->style & WS_DISABLED)
        {
            if (parent == 0) best = found; /* a disabled top-level window gets the point itself */
            break;
        }
        best = found;
        parent = found;
    }
    unlock();
    RET(best != 0 ? best : DESKTOP_HWND);
}

/* BOOL EnumWindows(WNDENUMPROC, LPARAM): the top-level windows, top
   first. */
void rt_EnumWindows(_cpu *cpu)
{
    uint32_t proc = ARG(0), param = ARG(1), list[MAX_WINDOWS];
    uint64_t zs[MAX_WINDOWS];
    int i, n = 0, a, b;
    lock();
    for (i = 1; i < MAX_WINDOWS; i++)
    {
        if (windows[i].hwnd != 0 && windows[i].parent == 0 && !windows[i].destroying)
        {
            list[n] = windows[i].hwnd;
            zs[n++] = windows[i].z;
        }
    }
    unlock();
    for (a = 1; a < n; a++)
    {
        for (b = a; b > 0 && zs[b - 1] < zs[b]; b--)
        {
            uint64_t tz = zs[b];
            uint32_t th = list[b];
            zs[b] = zs[b - 1];
            list[b] = list[b - 1];
            zs[b - 1] = tz;
            list[b - 1] = th;
        }
    }
    for (i = 0; i < n; i++)
    {
        if (!rt_user32_is_window(list[i])) continue;
        if (rt_call_cb(cpu, proc, 2, list[i], param) == 0)
        {
            RET(0);
            return;
        }
    }
    RET(1);
}

/* BOOL AdjustWindowRectEx(LPRECT, DWORD style, BOOL menu, DWORD exstyle)
   The native frame of macOS has a title bar and no borders. */
void rt_AdjustWindowRectEx(_cpu *cpu)
{
    uint32_t r = ARG(0), style = ARG(1);
    if (ARG(2) != 0) rt_fatal("AdjustWindowRectEx: windows with menus are not supported");
    if (r == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (titled(style) || !(style & (WS_POPUP | WS_CHILD)))
        wr32(r + 4, (uint32_t)((int32_t)rd32(r + 4) - rt_cocoa_title_height()));
    RET(1);
}

/* ---------------------------------------------------------------- GetWindowLong and SetWindowLong */

static uint32_t get_long(uint32_t hwnd, int32_t index, bool *ok)
{
    uint32_t v = 0;
    wnd *w;
    *ok = true;
    lock();
    w = get(hwnd);
    if (w == NULL)
    {
        unlock();
        rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        *ok = false;
        return 0;
    }
    switch (index)
    {
    case GWL_WNDPROC:
        if (classes[w->cls].runtime)
            rt_fatal("GetWindowLong: GWL_WNDPROC of a window of the runtime class %s is not supported", classes[w->cls].name);
        v = w->wndproc;
        break;
    case GWL_HINSTANCE: v = w->hinstance; break;
    case GWL_HWNDPARENT: v = w->parent != 0 ? w->parent : w->owner; break;
    case GWL_ID: v = w->id; break;
    case GWL_STYLE: v = w->style; break;
    case GWL_EXSTYLE: v = w->exstyle; break;
    case GWL_USERDATA: v = w->userdata; break;
    default:
        if (index < 0 || (uint32_t)index > w->extra_size || w->extra_size - (uint32_t)index < 4u)
        {
            unlock();
            rt_set_last_error(ERROR_INVALID_INDEX);
            *ok = false;
            return 0;
        }
        memcpy(&v, w->extra + index, 4);
    }
    unlock();
    return v;
}

static void get_window_long(_cpu *cpu)
{
    bool ok;
    RET(get_long(ARG(0), (int32_t)ARG(1), &ok));
}

void rt_GetWindowLongA(_cpu *cpu) { get_window_long(cpu); }
void rt_GetWindowLongW(_cpu *cpu) { get_window_long(cpu); }

static uint32_t set_style(_cpu *cpu, uint32_t hwnd, int32_t index, uint32_t value)
{
    uint32_t old, ss;
    void *ns;
    bool top, vis_change;
    unsigned kind;
    wnd *w;
    (void)cpu;
    lock();
    w = get(hwnd);
    old = index == GWL_STYLE ? w->style : w->exstyle;
    unlock();
    ss = rt_heap_calloc(1, 8);
    if (ss == 0) rt_fatal("out of guest memory (STYLESTRUCT)");
    wr32(ss, old);
    wr32(ss + 4, value);
    send0(hwnd, WM_STYLECHANGING, (uint32_t)index, ss);
    value = rd32(ss + 4);
    lock();
    w = get(hwnd);
    if (w == NULL)
    {
        unlock();
        rt_heap_free(ss);
        return 0;
    }
    if (index == GWL_STYLE)
    {
        vis_change = ((w->style ^ value) & WS_VISIBLE) != 0;
        if ((w->style ^ value) & WS_CHILD) rt_fatal("SetWindowLong: a change of WS_CHILD is not supported");
        w->style = value;
    }
    else
    {
        vis_change = false;
        w->exstyle = value;
    }
    ns = w->ns;
    top = w->parent == 0;
    kind = kind_of(w);
    unlock();
    /* The window rectangle stays; the client rectangle follows the new
       frame. */
    if (ns != NULL && top)
    {
        rt_cocoa_window_set_kind(ns, kind);
        native_frame(hwnd);
    }
    if (vis_change && ns != NULL)
    {
        bool v = (value & WS_VISIBLE) != 0;
        if (top)
        {
            if (!rt_cocoa_headless() || !v) rt_cocoa_window_show(ns, v, false);
        }
        else
            rt_cocoa_view_set_visible(ns, v);
    }
    else if (vis_change && (value & WS_VISIBLE) && on_main())
        make_native(top ? hwnd : top_of(hwnd));
    send0(hwnd, WM_STYLECHANGED, (uint32_t)index, ss);
    rt_heap_free(ss);
    return old;
}

static void set_window_long(_cpu *cpu, bool unicode)
{
    uint32_t hwnd = ARG(0), value = ARG(2), old = 0;
    int32_t index = (int32_t)ARG(1);
    wnd *w;
    lock();
    w = get(hwnd);
    if (w == NULL)
    {
        unlock();
        rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        RET(0);
        return;
    }
    if (w->tid != self_tid() && (index == GWL_STYLE || index == GWL_EXSTYLE || index == GWL_WNDPROC))
    {
        unlock();
        rt_set_last_error(ERROR_ACCESS_DENIED);
        RET(0);
        return;
    }
    switch (index)
    {
    case GWL_STYLE:
    case GWL_EXSTYLE:
        unlock();
        rt_set_last_error(0);
        RET(set_style(cpu, hwnd, index, value));
        return;
    case GWL_WNDPROC:
        if (classes[w->cls].runtime)
            rt_fatal("SetWindowLong: GWL_WNDPROC of a window of the runtime class %s (subclassing) is not supported",
                     classes[w->cls].name);
        if (value == 0)
        {
            unlock();
            rt_set_last_error(ERROR_INVALID_PARAMETER);
            RET(0);
            return;
        }
        old = w->wndproc;
        w->wndproc = value;
        w->unicode = unicode;
        break;
    case GWL_HINSTANCE: old = w->hinstance; w->hinstance = value; break;
    case GWL_ID: old = w->id; w->id = value; break;
    case GWL_USERDATA: old = w->userdata; w->userdata = value; break;
    case GWL_HWNDPARENT:
        if (w->parent != 0) rt_fatal("SetWindowLong: GWL_HWNDPARENT of a child window (SetParent) is not supported");
        old = w->owner;
        w->owner = value != 0 ? top_of(value) : 0;
        break;
    default:
        if (index < 0 || (uint32_t)index > w->extra_size || w->extra_size - (uint32_t)index < 4u)
        {
            unlock();
            rt_set_last_error(ERROR_INVALID_INDEX);
            RET(0);
            return;
        }
        memcpy(&old, w->extra + index, 4);
        memcpy(w->extra + index, &value, 4);
    }
    unlock();
    rt_set_last_error(0);
    RET(old);
}

void rt_SetWindowLongA(_cpu *cpu) { set_window_long(cpu, false); }
void rt_SetWindowLongW(_cpu *cpu) { set_window_long(cpu, true); }

/* ---------------------------------------------------------------- default window procedure */

static uint32_t def_proc(_cpu *cpu, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp, bool unicode)
{
    wnd *w;
    switch (msg)
    {
    case WM_NCCREATE:
        if (lp != 0 && rd32(lp + 36) != 0)
        {
            uint32_t name = rd32(lp + 36);
            char *t = name >= 0x10000u ? guest_text(name, unicode) : xstrdup("");
            lock();
            w = get(hwnd);
            if (w != NULL)
            {
                free(w->text);
                w->text = t;
                t = NULL;
            }
            unlock();
            free(t);
        }
        return 1;
    case WM_CLOSE:
        destroy_window(cpu, hwnd);
        return 0;
    case WM_SYSCOMMAND:
        switch (wp & 0xFFF0u)
        {
        case SC_CLOSE: send0(hwnd, WM_CLOSE, 0, 0); break;
        case SC_MINIMIZE: show_window(cpu, hwnd, SW_MINIMIZE); break;
        case SC_MAXIMIZE: show_window(cpu, hwnd, SW_SHOWMAXIMIZED); break;
        case SC_RESTORE: show_window(cpu, hwnd, SW_RESTORE); break;
        case SC_KEYMENU: break;     /* the windows have no menus */
        case SC_SCREENSAVE: break;  /* macOS starts the screen saver itself */
        case SC_MONITORPOWER: break; /* macOS controls display sleep itself */
        case SC_SIZE: case SC_MOVE: break; /* the native frame moves and sizes the window */
        default: rt_fatal("DefWindowProc: WM_SYSCOMMAND 0x%04x is not supported", wp & 0xFFF0u);
        }
        return 0;
    case WM_SYSKEYDOWN:
        /* Alt+F4 closes the window. */
        if (wp == VK_F4 && (lp & (1u << 29))) send0(top_of(hwnd), WM_SYSCOMMAND, SC_CLOSE, 0);
        return 0;
    case WM_ACTIVATE:
        if ((wp & 0xFFFFu) != WA_INACTIVE && !(wp >> 16))
        {
            uint32_t f = rt_user32_focus();
            if (f == 0 || !rt_user32_is_self_or_child(hwnd, f)) set_focus(cpu, hwnd);
        }
        return 0;
    case WM_PAINT:
        lock();
        w = get(hwnd);
        if (w != NULL) w->need_paint = false;
        unlock();
        return 0;
    case WM_WINDOWPOSCHANGED:
    {
        uint32_t flags = rd32(lp + 24), style;
        rt_rect c;
        lock();
        w = get(hwnd);
        if (w == NULL)
        {
            unlock();
            return 0;
        }
        c = client_of(w);
        style = w->style;
        unlock();
        if (!(flags & SWP_NOCLIENTMOVE)) send0(hwnd, WM_MOVE, 0, make_lparam(c.left, c.top));
        if (!(flags & SWP_NOCLIENTSIZE) || (flags & SWP_STATECHANGED))
            send0(hwnd, WM_SIZE,
                  (style & WS_MINIMIZE) ? SIZE_MINIMIZED : (style & WS_MAXIMIZE) ? SIZE_MAXIMIZED : SIZE_RESTORED,
                  make_lparam(c.right - c.left, c.bottom - c.top));
        return 0;
    }
    case WM_SETCURSOR:
        /* The parent first; then the class cursor in the client area. */
        {
            uint32_t parent = rt_user32_parent(hwnd), cursor = 0;
            if (parent != 0 && send0(parent, WM_SETCURSOR, wp, lp) != 0) return 1;
            if ((lp & 0xFFFFu) != HTCLIENT) return 0;
            lock();
            w = get(hwnd);
            if (w != NULL) cursor = classes[w->cls].cursor;
            unlock();
            if (cursor != 0) rt_user32_cursor_set(cursor);
            return cursor != 0 ? 1u : 0u;
        }
    case WM_NCACTIVATE:
    case WM_ERASEBKGND:
    case WM_QUERYENDSESSION:
    case WM_QUERYOPEN:
        return 1;
    case WM_MOUSEACTIVATE:
    {
        uint32_t parent = rt_user32_parent(hwnd);
        if (parent != 0)
        {
            uint32_t r = send0(parent, WM_MOUSEACTIVATE, wp, lp);
            if (r != 0) return r;
        }
        return MA_ACTIVATE;
    }
    case WM_NCHITTEST:
    {
        int32_t x = (int16_t)(lp & 0xFFFFu), y = (int16_t)(lp >> 16), ox, oy;
        rt_rect c;
        lock();
        w = get(hwnd);
        if (w == NULL)
        {
            unlock();
            return 0;
        }
        c = client_of(w);
        client_origin(hwnd, &ox, &oy);
        unlock();
        if (x >= ox && y >= oy && x < ox + (c.right - c.left) && y < oy + (c.bottom - c.top)) return HTCLIENT;
        return y < oy ? 2u /* HTCAPTION */ : 0u /* HTNOWHERE */;
    }
    case WM_GETTEXTLENGTH:
    {
        char *t = rt_user32_text(hwnd);
        uint32_t n = text_length(t, unicode);
        free(t);
        return n;
    }
    case WM_GETTEXT:
    {
        char *t = rt_user32_text(hwnd);
        uint32_t n = copy_text(t, lp, wp, unicode);
        free(t);
        return n;
    }
    case WM_SETTEXT:
    {
        char *t = lp != 0 ? guest_text(lp, unicode) : xstrdup("");
        void *ns = NULL;
        bool top = false;
        lock();
        w = get(hwnd);
        if (w != NULL)
        {
            free(w->text);
            w->text = xstrdup(t);
            ns = w->ns;
            top = w->parent == 0;
        }
        unlock();
        if (ns != NULL && top) rt_cocoa_window_set_title(ns, t);
        free(t);
        return 1;
    }
    case WM_GETICON:
        lock();
        w = get(hwnd);
        {
            uint32_t r = w == NULL ? 0 : wp == 1 ? w->icon_big : w->icon_small;
            unlock();
            return r;
        }
    case WM_SETICON:
        lock();
        w = get(hwnd);
        {
            uint32_t r = 0;
            if (w != NULL)
            {
                if (wp == 1)
                {
                    r = w->icon_big;
                    w->icon_big = lp;
                }
                else
                {
                    r = w->icon_small;
                    w->icon_small = lp;
                }
            }
            unlock();
            return r;
        }
    case WM_IME_CHAR:
        /* The character of the input method becomes a WM_CHAR message. */
        {
            qmsg m;
            memset(&m, 0, sizeof m);
            m.hwnd = hwnd;
            m.msg = WM_CHAR;
            m.wparam = unicode ? wp & 0xFFFFu : char_to_wide(wp);
            m.lparam = lp;
            m.time = now_ms();
            m.uchar = true;
            post_to(hwnd, &m);
            return 0;
        }
    case WM_CONTEXTMENU:
    case WM_APPCOMMAND:
    {
        uint32_t parent = rt_user32_parent(hwnd);
        if (parent != 0) return send0(parent, msg, wp, lp);
        return 0;
    }
    default:
        /* The other messages have no default action on Windows: the result
           is 0. */
        return 0;
    }
}

uint32_t rt_user32_default_proc(_cpu *cpu, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp, bool unicode)
{
    return def_proc(cpu, hwnd, msg, wp, lp, unicode);
}

/* LRESULT DefWindowProcW(HWND, UINT, WPARAM, LPARAM) */
void rt_DefWindowProcW(_cpu *cpu)
{
    RET(def_proc(cpu, ARG(0), ARG(1), ARG(2), ARG(3), true));
}

/* ---------------------------------------------------------------- messages between threads */

#define ERROR_TIMEOUT 1460u
#define SMTO_BLOCK 0x0001u
#define SMTO_ABORTIFHUNG 0x0002u
#define SMTO_NOTIMEOUTIFNOTHUNG 0x0008u
#define SMTO_ERRORONEXIT 0x0020u
#define HUNG_MS 5000u /* the Windows limit for a thread that does not check its queue */

static void process_sys(_cpu *cpu);

/* Send a message to a window of thread tid and wait for the reply. The
   target thread handles it at its next message check. While the sender
   waits, it handles the messages that other threads send to it (not with
   SMTO_BLOCK). timeout: milliseconds, 0xFFFFFFFF for no limit. ok: NULL
   (SendMessage), else false at the timeout. */
static uint32_t send_other(_cpu *cpu, uint32_t hwnd, uint32_t tid, uint32_t msg, uint32_t wp, uint32_t lp, bool unicode,
                           uint32_t smto, uint32_t timeout, bool *ok)
{
    uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    sent_reply *r = xcalloc(1, sizeof *r);
    uint32_t result;
    sysmsg m;
    mqueue *tq, *me;
    bool main = on_main();
    memset(&m, 0, sizeof m);
    m.kind = SYS_SENT;
    m.hwnd = hwnd;
    m.msg = msg;
    m.wp = wp;
    m.lp = lp;
    m.unicode = unicode;
    m.reply = r;
    r->refs = 2;
    lock();
    tq = queue_of(tid, true);
    if ((smto & SMTO_ABORTIFHUNG) && now_ms() - tq->last_check > HUNG_MS)
    {
        unlock();
        free(r);
        if (ok != NULL) *ok = false;
        rt_set_last_error(ERROR_TIMEOUT);
        return 0;
    }
    sys_push(tq, &m);
    me = queue_of(self_tid(), true);
    for (;;)
    {
        uint64_t now, wait_ns;
        int e;
        if (r->done) break;
        if (!(smto & SMTO_BLOCK) && me->sys_count != 0)
        {
            unlock();
            process_sys(cpu);
            lock();
            continue;
        }
        now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (timeout != 0xFFFFFFFFu && now - start >= (uint64_t)timeout * 1000000u) break;
        wait_ns = timeout == 0xFFFFFFFFu ? 1000000000u : (uint64_t)timeout * 1000000u - (now - start);
        /* The main thread handles native events at least every 10 ms. */
        if (main && wait_ns > 10000000u) wait_ns = 10000000u;
        {
            struct timespec ts = { (time_t)(wait_ns / 1000000000u), (long)(wait_ns % 1000000000u) };
            e = pthread_cond_timedwait_relative_np(&sent_cond, &user_lock, &ts);
        }
        if (e != 0 && e != ETIMEDOUT) rt_fatal("USER32: condition wait failed (%d)", e);
        if (main && !r->done)
        {
            unlock();
            rt_cocoa_pump();
            lock();
        }
    }
    result = r->result;
    if (ok != NULL) *ok = r->done;
    if (!r->done) rt_set_last_error(ERROR_TIMEOUT);
    if (--r->refs == 0) free(r);
    unlock();
    return result;
}

/* ---------------------------------------------------------------- system notifications */

#define GCS_COMPSTR 0x0008u
#define GCS_COMPATTR 0x0010u
#define GCS_CURSORPOS 0x0080u
#define GCS_RESULTSTR 0x0800u

static bool ime_composing;

/* The window that gets keyboard and IME messages: the focus window, or
   the active window (lock held). */
static uint32_t key_target(mqueue *q)
{
    if (q->focus != 0 && get(q->focus) != NULL) return q->focus;
    if (q->active != 0 && get(q->active) != NULL) return q->active;
    return 0;
}

static void ime_end(uint32_t target)
{
    if (!ime_composing) return;
    ime_composing = false;
    if (target != 0) send0(target, WM_IME_ENDCOMPOSITION, 0, 0);
}

static void ime_composition(uint32_t target, uint16_t *text, int n, int caret)
{
    free(ime_comp);
    ime_comp = text;
    ime_ncomp = n;
    ime_caret = caret;
    if (target == 0) return;
    if (n == 0)
    {
        send0(target, WM_IME_COMPOSITION, 0, 0);
        ime_end(target);
        return;
    }
    if (!ime_composing)
    {
        ime_composing = true;
        send0(target, WM_IME_STARTCOMPOSITION, 0, 0);
    }
    send0(target, WM_IME_COMPOSITION, text[n - 1], GCS_COMPSTR | GCS_COMPATTR | GCS_CURSORPOS);
}

static void ime_result_text(uint32_t target, uint16_t *text, int n)
{
    free(ime_result);
    ime_result = text;
    ime_nresult = n;
    free(ime_comp);
    ime_comp = NULL;
    ime_ncomp = 0;
    ime_caret = 0;
    if (target == 0 || n == 0) return;
    if (!ime_composing)
    {
        ime_composing = true;
        send0(target, WM_IME_STARTCOMPOSITION, 0, 0);
    }
    send0(target, WM_IME_COMPOSITION, text[0], GCS_RESULTSTR);
    ime_end(target);
}

/* A native frame change: the user moved or sized the window. */
static void apply_native_frame(_cpu *cpu, uint32_t hwnd)
{
    rt_rect c, nr, old;
    uint32_t wp, flags = SWP_NOZORDER | SWP_NOACTIVATE;
    void *ns;
    wnd *w;
    (void)cpu;
    lock();
    w = get(hwnd);
    if (w == NULL || w->ns == NULL || w->parent != 0 || (w->style & WS_MINIMIZE))
    {
        unlock();
        return;
    }
    ns = w->ns;
    old = w->wrect;
    unlock();
    c = rt_cocoa_window_client(ns);
    lock();
    w = get(hwnd);
    if (w == NULL)
    {
        unlock();
        return;
    }
    nr = (rt_rect){ c.left, c.top - (titled(w->style) ? w->title_h : 0), c.right, c.bottom };
    unlock();
    if (nr.left == old.left && nr.top == old.top && nr.right == old.right && nr.bottom == old.bottom) return;
    if (nr.left == old.left && nr.top == old.top) flags |= SWP_NOMOVE;
    if (nr.right - nr.left == old.right - old.left && nr.bottom - nr.top == old.bottom - old.top) flags |= SWP_NOSIZE;
    wp = rt_heap_calloc(1, 28);
    if (wp == 0) rt_fatal("out of guest memory (WINDOWPOS)");
    wr32(wp, hwnd);
    wr32(wp + 8, (uint32_t)nr.left);
    wr32(wp + 12, (uint32_t)nr.top);
    wr32(wp + 16, (uint32_t)(nr.right - nr.left));
    wr32(wp + 20, (uint32_t)(nr.bottom - nr.top));
    wr32(wp + 24, flags);
    send0(hwnd, WM_WINDOWPOSCHANGING, 0, wp);
    lock();
    w = get(hwnd);
    if (w != NULL)
    {
        w->wrect = nr;
        if (!(w->style & WS_MAXIMIZE)) w->restore = nr;
        if (flags & SWP_NOMOVE) flags |= SWP_NOCLIENTMOVE;
        if (flags & SWP_NOSIZE)
            flags |= SWP_NOCLIENTSIZE;
        else
            mark_paint(w);
    }
    unlock();
    wr32(wp + 8, (uint32_t)nr.left);
    wr32(wp + 12, (uint32_t)nr.top);
    wr32(wp + 16, (uint32_t)(nr.right - nr.left));
    wr32(wp + 20, (uint32_t)(nr.bottom - nr.top));
    wr32(wp + 24, flags);
    if (w != NULL) send0(hwnd, WM_WINDOWPOSCHANGED, 0, wp);
    rt_heap_free(wp);
}

/* The user minimized the window (Dock, button) or restored it. */
static void apply_minimized(_cpu *cpu, uint32_t hwnd, bool minimized)
{
    uint32_t wp;
    rt_rect r;
    wnd *w;
    (void)cpu;
    lock();
    w = get(hwnd);
    if (w == NULL || w->parent != 0 || ((w->style & WS_MINIMIZE) != 0) == minimized)
    {
        unlock();
        return;
    }
    if (minimized)
    {
        rt_log("USER32: window 0x%08x was minimized in macOS (the Dock or the title bar)", hwnd);
        if (!(w->style & WS_MAXIMIZE)) w->restore = w->wrect;
        w->style |= WS_MINIMIZE;
    }
    else
    {
        w->style &= ~WS_MINIMIZE;
        mark_paint(w);
    }
    r = w->wrect;
    unlock();
    wp = rt_heap_calloc(1, 28);
    if (wp == 0) rt_fatal("out of guest memory (WINDOWPOS)");
    wr32(wp, hwnd);
    wr32(wp + 8, (uint32_t)r.left);
    wr32(wp + 12, (uint32_t)r.top);
    wr32(wp + 16, (uint32_t)(r.right - r.left));
    wr32(wp + 20, (uint32_t)(r.bottom - r.top));
    wr32(wp + 24, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_STATECHANGED | SWP_NOCLIENTMOVE);
    send0(hwnd, WM_WINDOWPOSCHANGED, 0, wp);
    rt_heap_free(wp);
}

/* The application became active again (the Command-Tab switcher, the Dock).
   The Windows task switcher restores a minimized window that the user
   selects with WM_SYSCOMMAND SC_RESTORE. A full-screen game minimizes its
   window when the application is no longer active (WM_ACTIVATEAPP 0), so
   the window that was active before the deactivation gets SC_RESTORE when
   it is minimized. */
static void restore_last_active(void)
{
    uint32_t hwnd;
    bool restore = false;
    wnd *w;
    lock();
    hwnd = queue_of(self_tid(), true)->last_active;
    w = get(hwnd);
    if (w != NULL && !w->destroying && w->parent == 0 && (w->style & WS_VISIBLE) && (w->style & WS_MINIMIZE))
        restore = true;
    unlock();
    if (!restore) return;
    rt_log("USER32: the application is active again; window 0x%08x is minimized and gets SC_RESTORE", hwnd);
    send0(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
}

/* Handle the system notifications of the calling thread. Guest code runs
   here: window procedures get the messages that the system sends. */
static void process_sys(_cpu *cpu)
{
    for (;;)
    {
        sysmsg m;
        uint32_t target;
        mqueue *q;
        lock();
        q = queue_of(self_tid(), true);
        q->last_check = now_ms();
        if (q->sys_count == 0)
        {
            unlock();
            return;
        }
        m = q->sys[q->sys_head];
        q->sys_head = (q->sys_head + 1u) % SYS_LIMIT;
        q->sys_count--;
        target = key_target(q);
        unlock();
        switch (m.kind)
        {
        case SYS_ACTIVE:
            if (m.a != 0)
                set_active(cpu, m.hwnd);
            else
                deactivate(cpu, m.hwnd, false);
            break;
        case SYS_APP_ACTIVE:
            if (m.a == 0)
            {
                uint32_t act;
                lock();
                act = queue_of(self_tid(), true)->active;
                unlock();
                deactivate(cpu, act, true);
            }
            else
            {
                bool was;
                lock();
                was = app_active;
                app_active = true;
                unlock();
                if (!was) send_top_windows(WM_ACTIVATEAPP, 1, 0);
                restore_last_active();
            }
            break;
        case SYS_CLOSE:
            send0(m.hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
            break;
        case SYS_FRAME:
            apply_native_frame(cpu, m.hwnd);
            break;
        case SYS_MINIMIZED:
            apply_minimized(cpu, m.hwnd, m.a != 0);
            break;
        case SYS_QUIT:
        {
            /* The Quit command of the application menu: close every
               visible top-level window, as Alt+F4 does. */
            uint32_t list[MAX_WINDOWS];
            int i, n = 0;
            lock();
            for (i = 1; i < MAX_WINDOWS; i++)
                if (windows[i].hwnd != 0 && windows[i].parent == 0 && windows[i].tid == self_tid() &&
                    (windows[i].style & WS_VISIBLE))
                    list[n++] = windows[i].hwnd;
            unlock();
            for (i = 0; i < n; i++)
                if (rt_user32_is_window(list[i])) send0(list[i], WM_SYSCOMMAND, SC_CLOSE, 0);
            break;
        }
        case SYS_CONTROL:
            rt_dialog_control_event(cpu, m.hwnd, m.a, m.b, m.c);
            break;
        case SYS_IME_COMP:
            ime_composition(target, m.text, m.a, m.b);
            break;
        case SYS_IME_RESULT:
            ime_result_text(target, m.text, m.a);
            break;
        case SYS_CALL:
            m.fn(cpu, m.ctx);
            break;
        case SYS_SENT:
        {
            uint32_t r = call_proc(cpu, m.hwnd, m.msg, m.wp, m.lp, m.unicode);
            lock();
            m.reply->result = r;
            m.reply->done = true;
            if (--m.reply->refs == 0) free(m.reply);
            if (pthread_cond_broadcast(&sent_cond) != 0) rt_fatal("USER32: condition broadcast failed");
            unlock();
            break;
        }
        default:
            rt_fatal("USER32: system notification kind %d is not known", m.kind);
        }
    }
}

/* ---------------------------------------------------------------- PeekMessage and GetMessage */

static void write_msg(uint32_t g, const qmsg *m)
{
    wr32(g, m->hwnd);
    wr32(g + 4, m->msg);
    wr32(g + 8, m->wparam);
    wr32(g + 12, m->lparam);
    wr32(g + 16, m->time);
    wr32(g + 20, (uint32_t)m->x);
    wr32(g + 24, (uint32_t)m->y);
}

/* The hwnd filter of PeekMessage: 0 any message of the thread, -1 thread
   messages only, else the window and its children (lock held). */
static bool hwnd_matches(uint32_t filter, uint32_t hwnd)
{
    wnd *w;
    if (filter == 0) return true;
    if (filter == 0xFFFFFFFFu) return hwnd == 0;
    if (hwnd == 0) return false;
    w = get(hwnd);
    while (w != NULL)
    {
        if (hwnd == filter) return true;
        hwnd = w->parent;
        w = hwnd != 0 ? get(hwnd) : NULL;
    }
    return false;
}

static bool msg_matches(uint32_t msg, uint32_t lo, uint32_t hi)
{
    if (lo == 0 && hi == 0) return true;
    if (lo > hi) return msg >= lo || msg <= hi;
    return msg >= lo && msg <= hi;
}

static bool is_mouse_msg(uint32_t msg)
{
    return msg >= WM_MOUSEMOVE && msg <= WM_MOUSEHWHEEL;
}

static bool is_button_down(uint32_t msg)
{
    return msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK ||
           msg == WM_MBUTTONDOWN || msg == WM_MBUTTONDBLCLK || msg == WM_XBUTTONDOWN || msg == WM_XBUTTONDBLCLK;
}

/* GetKeyState state for a removed message (lock held). */
static void sync_key_state(const qmsg *m)
{
    uint32_t vk = 0;
    bool down = false;
    switch (m->msg)
    {
    case WM_KEYDOWN: case WM_SYSKEYDOWN: vk = m->wparam & 0xFFu; down = true; break;
    case WM_KEYUP: case WM_SYSKEYUP: vk = m->wparam & 0xFFu; down = false; break;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: vk = VK_LBUTTON; down = true; break;
    case WM_LBUTTONUP: vk = VK_LBUTTON; down = false; break;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: vk = VK_RBUTTON; down = true; break;
    case WM_RBUTTONUP: vk = VK_RBUTTON; down = false; break;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: vk = VK_MBUTTON; down = true; break;
    case WM_MBUTTONUP: vk = VK_MBUTTON; down = false; break;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: vk = (m->wparam >> 16) == 2 ? VK_XBUTTON2 : VK_XBUTTON1; down = true; break;
    case WM_XBUTTONUP: vk = (m->wparam >> 16) == 2 ? VK_XBUTTON2 : VK_XBUTTON1; down = false; break;
    default: return;
    }
    if (vk == 0xE5u) return; /* VK_PROCESSKEY */
    if (down && !(m->lparam & (1u << 30)) && !(sync_state[vk] & 0x80)) sync_state[vk] ^= 0x01;
    sync_state[vk] = (uint8_t)((sync_state[vk] & 0x01) | (down ? 0x80 : 0));
    /* The left and right modifier keys: the scan code (Shift) or the
       extended bit (Ctrl, Alt) selects the right key. */
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU)
    {
        uint32_t scan = (m->lparam >> 16) & 0xFFu;
        bool right = vk == VK_SHIFT ? scan == 0x36 : (m->lparam & (1u << 24)) != 0;
        uint32_t side = vk == VK_SHIFT ? (right ? VK_RSHIFT : VK_LSHIFT)
                      : vk == VK_CONTROL ? (right ? VK_RCONTROL : VK_LCONTROL)
                      : (right ? VK_RMENU : VK_LMENU);
        uint32_t other = vk == VK_SHIFT ? (right ? VK_LSHIFT : VK_RSHIFT)
                       : vk == VK_CONTROL ? (right ? VK_LCONTROL : VK_RCONTROL)
                       : (right ? VK_LMENU : VK_RMENU);
        sync_state[side] = (uint8_t)((sync_state[side] & 0x01) | (down ? 0x80 : 0));
        if (!down && (sync_state[other] & 0x80)) sync_state[vk] |= 0x80; /* the other key is still down */
    }
}

/* Mouse processing of a removed mouse message (main thread): the hit
   test, the mouse activation and the cursor. Returns false if the window
   eats the message (MA_ACTIVATEANDEAT, MA_NOACTIVATEANDEAT). */
static bool mouse_removed(_cpu *cpu, const qmsg *m)
{
    uint32_t hit = HTCLIENT, capture, top, active;
    lock();
    {
        mqueue *q = queue_of(self_tid(), true);
        capture = q->capture;
        active = q->active;
        top = top_of(m->hwnd);
    }
    unlock();
    if (!rt_user32_is_window(m->hwnd)) return false;
    if (capture == 0) hit = send0(m->hwnd, WM_NCHITTEST, 0, make_lparam(m->x, m->y));
    if (is_button_down(m->msg) && top != 0 && top != active)
    {
        uint32_t r = send0(m->hwnd, WM_MOUSEACTIVATE, top, (hit & 0xFFFFu) | (m->msg << 16));
        if (r == 1u || r == 2u) set_active(cpu, top); /* MA_ACTIVATE, MA_ACTIVATEANDEAT */
        if (r == 2u || r == 4u) return false;          /* MA_ACTIVATEANDEAT, MA_NOACTIVATEANDEAT */
    }
    if (capture == 0 && rt_user32_is_window(m->hwnd)) send0(m->hwnd, WM_SETCURSOR, m->hwnd, (hit & 0xFFFFu) | (m->msg << 16));
    return true;
}

#define KINDS_SENT 1u
#define KINDS_POSTED 2u
#define KINDS_INPUT 4u
#define KINDS_PAINT 8u

/* PeekMessage. Returns true and writes the MSG if a message matches. */
static bool peek(_cpu *cpu, uint32_t pmsg, uint32_t filter, uint32_t lo, uint32_t hi, uint32_t flags, bool unicode)
{
    bool remove = (flags & PM_REMOVE) != 0;
    unsigned kinds = KINDS_SENT | KINDS_POSTED | KINDS_INPUT | KINDS_PAINT;
    if (flags >> 16)
    {
        kinds = 0;
        if (flags & PM_QS_SENDMESSAGE) kinds |= KINDS_SENT;
        if (flags & PM_QS_POSTMESSAGE) kinds |= KINDS_POSTED;
        if (flags & PM_QS_INPUT) kinds |= KINDS_INPUT;
        if (flags & PM_QS_PAINT) kinds |= KINDS_PAINT;
    }
    if (on_main())
    {
        rt_cocoa_pump();
        /* The managed half of the port's graphics rows, between two frames
           of the player loop (rt_portgfx.c). */
        rt_portgfx_pump(cpu);
    }
    for (;;)
    {
        mqueue *q;
        uint32_t i;
        qmsg m;
        int k;
        if (kinds & KINDS_SENT) process_sys(cpu);
        lock();
        q = queue_of(self_tid(), true);
        q->last_check = now_ms();
        queue_seen(q);
        if (q->sys_count != 0 && (kinds & KINDS_SENT))
        {
            unlock();
            continue;
        }
        /* Posted messages, then WM_QUIT, then input. */
        for (k = 0; k < 3; k++)
        {
            ring *r = k == 0 ? &q->posted : &q->input;
            if (k == 1)
            {
                /* WM_QUIT: with no posted message left, for any filter. */
                if (!q->quit || !(kinds & KINDS_POSTED)) continue;
                memset(&m, 0, sizeof m);
                m.msg = WM_QUIT;
                m.wparam = q->quit_code;
                m.time = now_ms();
                m.x = mouse_x;
                m.y = mouse_y;
                if (remove) q->quit = false;
                unlock();
                write_msg(pmsg, &m);
                return true;
            }
            if (k == 0 ? !(kinds & KINDS_POSTED) : (!(kinds & KINDS_INPUT) || r->buf == NULL)) continue;
            for (i = 0; i < r->count; i++)
            {
                m = *ring_at(r, i);
                if (!hwnd_matches(filter, m.hwnd) || !msg_matches(m.msg, lo, hi)) continue;
                if (m.uchar && !unicode)
                {
                    /* The ANSI form of a UTF-16 character message. The low
                       half of a surrogate pair has no ANSI character: the
                       high half gave '?'. */
                    if (m.wparam >= 0xDC00u && m.wparam < 0xE000u)
                    {
                        if (remove)
                        {
                            ring_remove(r, i);
                            i--;
                        }
                        continue;
                    }
                    m.wparam = m.wparam >= 0xD800u && m.wparam < 0xDC00u ? (uint32_t)'?' : char_to_ansi(m.wparam);
                }
                if (remove)
                {
                    ring_remove(r, i);
                    if (k == 2)
                    {
                        sync_key_state(&m);
                        if (m.msg == WM_KEYDOWN || m.msg == WM_SYSKEYDOWN)
                        {
                            q->last_key = m;
                            q->last_key_valid = true;
                        }
                    }
                }
                unlock();
                if (remove && k == 2 && is_mouse_msg(m.msg) && !mouse_removed(cpu, &m)) goto again;
                write_msg(pmsg, &m);
                return true;
            }
        }
        /* WM_PAINT for a visible window that needs it. */
        if ((kinds & KINDS_PAINT) && msg_matches(WM_PAINT, lo, hi))
        {
            for (i = 1; i < MAX_WINDOWS; i++)
            {
                wnd *w = &windows[i];
                if (w->hwnd == 0 || w->tid != self_tid() || !w->need_paint || w->destroying || !(w->style & WS_VISIBLE) ||
                    !hwnd_matches(filter, w->hwnd))
                    continue;
                memset(&m, 0, sizeof m);
                m.hwnd = w->hwnd;
                m.msg = WM_PAINT;
                m.time = now_ms();
                m.x = mouse_x;
                m.y = mouse_y;
                unlock();
                write_msg(pmsg, &m);
                return true;
            }
        }
        unlock();
        return false;
    again:;
    }
}

static void peek_message(_cpu *cpu, bool unicode)
{
    uint32_t pmsg = ARG(0), flags = ARG(4);
    if (pmsg == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (flags & ~(PM_REMOVE | PM_NOYIELD | PM_QS_INPUT | PM_QS_POSTMESSAGE | PM_QS_PAINT | PM_QS_SENDMESSAGE))
    {
        rt_set_last_error(1004u); /* ERROR_INVALID_FLAGS */
        RET(0);
        return;
    }
    RET(peek(cpu, pmsg, ARG(1), ARG(2), ARG(3), flags, unicode) ? 1u : 0u);
}

/* BOOL PeekMessageA(LPMSG, HWND, UINT min, UINT max, UINT remove) */
void rt_PeekMessageA(_cpu *cpu) { peek_message(cpu, false); }
/* BOOL PeekMessageW(LPMSG, HWND, UINT min, UINT max, UINT remove) */
void rt_PeekMessageW(_cpu *cpu) { peek_message(cpu, true); }

/* The main thread waits with native events: the wait hook. */
static bool main_block(rt_wait_hook *h, bool infinite, uint64_t deadline_ns)
{
    return rt_cocoa_wait(&h->woken, infinite, deadline_ns);
}

/* Wait until the queue of the calling thread has new entries. */
static void wait_queue(_cpu *cpu, uint32_t ms)
{
    rt_wait_hook hook;
    rt_object *ev;
    mqueue *q = my_queue();
    lock();
    ev = q->event;
    rt_object_ref(ev);
    unlock();
    atomic_init(&hook.woken, false);
    hook.notify = rt_cocoa_wake;
    hook.block = main_block;
    (void)rt_wait_object_list(cpu, 1, &ev, false, ms, false, on_main() ? &hook : NULL);
    rt_object_unref(ev);
}

static int get_message(_cpu *cpu, uint32_t pmsg, uint32_t filter, uint32_t lo, uint32_t hi, bool unicode)
{
    if (pmsg == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        return -1;
    }
    if (filter != 0 && filter != 0xFFFFFFFFu && !rt_user32_is_window(filter))
    {
        rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        return -1;
    }
    for (;;)
    {
        if (peek(cpu, pmsg, filter, lo, hi, PM_REMOVE, unicode)) return rd32(pmsg + 4) == WM_QUIT ? 0 : 1;
        wait_queue(cpu, 0xFFFFFFFFu);
    }
}

bool rt_user32_get_message(_cpu *cpu, uint32_t pmsg, bool unicode)
{
    return get_message(cpu, pmsg, 0, 0, 0, unicode) > 0;
}

/* BOOL GetMessageA(LPMSG, HWND, UINT min, UINT max) */
void rt_GetMessageA(_cpu *cpu)
{
    RET((uint32_t)get_message(cpu, ARG(0), ARG(1), ARG(2), ARG(3), false));
}

/* DWORD MsgWaitForMultipleObjects(DWORD count, const HANDLE *, BOOL all,
   DWORD ms, DWORD wake_mask) */
void rt_MsgWaitForMultipleObjects(_cpu *cpu)
{
    uint32_t count = ARG(0), hs = ARG(1), ms = ARG(3), mask = ARG(4), i, result;
    uint32_t handles[64];
    rt_object *objs[64];
    const uint32_t required = QS_KEY | QS_MOUSEMOVE | QS_MOUSEBUTTON | QS_POSTMESSAGE | QS_PAINT | QS_SENDMESSAGE | QS_RAWINPUT;
    rt_wait_hook hook;
    mqueue *q;
    if (count > 63 || (count != 0 && hs == 0))
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0xFFFFFFFFu);
        return;
    }
    if (ARG(2) != 0) rt_fatal("MsgWaitForMultipleObjects: a wait for all objects and input is not supported");
    if ((mask & required) != required)
        rt_fatal("MsgWaitForMultipleObjects: wake mask 0x%x is not supported (the runtime needs all of 0x%x)", mask, required);
    for (i = 0; i < count; i++) handles[i] = rd32(hs + 4u * i);
    if (count != 0 && !rt_handles_get_waitable(count, handles, objs))
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0xFFFFFFFFu);
        return;
    }
    if (on_main()) rt_cocoa_pump();
    q = my_queue();
    lock();
    q->last_check = now_ms();
    if (q->changed & mask)
    {
        unlock();
        for (i = 0; i < count; i++) rt_object_unref(objs[i]);
        RET(count);
        return;
    }
    objs[count] = q->event;
    rt_object_ref(q->event);
    unlock();
    atomic_init(&hook.woken, false);
    hook.notify = rt_cocoa_wake;
    hook.block = main_block;
    result = rt_wait_object_list(cpu, count + 1u, objs, false, ms, false, on_main() ? &hook : NULL);
    for (i = 0; i <= count; i++) rt_object_unref(objs[i]);
    RET(result);
}

/* ---------------------------------------------------------------- TranslateMessage, DispatchMessage */

static bool translate(const qmsg *msg)
{
    qmsg last, c;
    mqueue *q;
    int i;
    bool ok = true;
    if (msg->msg != WM_KEYDOWN && msg->msg != WM_SYSKEYDOWN) return msg->msg == WM_KEYUP || msg->msg == WM_SYSKEYUP;
    lock();
    q = queue_of(self_tid(), true);
    if (!q->last_key_valid || q->last_key.hwnd != msg->hwnd || q->last_key.msg != msg->msg ||
        q->last_key.wparam != msg->wparam || q->last_key.lparam != msg->lparam)
    {
        /* Not the key message that PeekMessage gave: the runtime has no
           characters for it (a message that the program made itself). */
        unlock();
        return false;
    }
    last = q->last_key;
    q->last_key_valid = false;
    if (last.nchars == 0)
    {
        unlock();
        return false;
    }
    /* The characters come before any other posted message, in order. */
    for (i = last.nchars - 1; i >= 0; i--)
    {
        memset(&c, 0, sizeof c);
        c.hwnd = last.hwnd;
        c.msg = last.msg == WM_SYSKEYDOWN ? WM_SYSCHAR : WM_CHAR;
        c.wparam = last.chars[i];
        c.lparam = last.lparam;
        c.time = last.time;
        c.x = last.x;
        c.y = last.y;
        c.uchar = true;
        if (!ring_push(&q->posted, &c, true)) ok = false;
    }
    if (ok) queue_signal(q, QS_POSTMESSAGE | QS_ALLPOSTMESSAGE);
    unlock();
    if (!ok) rt_log("USER32: the message queue is full; TranslateMessage discarded characters");
    return true;
}

static qmsg read_msg(uint32_t p)
{
    qmsg m;
    memset(&m, 0, sizeof m);
    m.hwnd = rd32(p);
    m.msg = rd32(p + 4);
    m.wparam = rd32(p + 8);
    m.lparam = rd32(p + 12);
    m.time = rd32(p + 16);
    m.x = (int32_t)rd32(p + 20);
    m.y = (int32_t)rd32(p + 24);
    return m;
}

void rt_user32_translate(_cpu *cpu, uint32_t pmsg)
{
    qmsg m = read_msg(pmsg);
    (void)cpu;
    translate(&m);
}

/* BOOL TranslateMessage(const MSG *) */
void rt_TranslateMessage(_cpu *cpu)
{
    qmsg m;
    if (ARG(0) == 0)
    {
        RET(0);
        return;
    }
    m = read_msg(ARG(0));
    RET(translate(&m) ? 1u : 0u);
}

uint32_t rt_user32_dispatch(_cpu *cpu, uint32_t pmsg, bool unicode)
{
    qmsg m = read_msg(pmsg);
    uint32_t tid;
    wnd *w;
    if (m.msg == WM_TIMER && m.lparam != 0)
        rt_fatal("DispatchMessage: WM_TIMER with a timer procedure (the program has no SetTimer import)");
    if (m.hwnd == 0) return 0;
    lock();
    w = get(m.hwnd);
    tid = w != NULL ? w->tid : 0;
    unlock();
    if (w == NULL)
    {
        rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    if (tid != self_tid())
    {
        rt_set_last_error(ERROR_WINDOW_OF_OTHER_THREAD);
        return 0;
    }
    return call_proc(cpu, m.hwnd, m.msg, m.wparam, m.lparam, unicode);
}

/* LRESULT DispatchMessageA(const MSG *) */
void rt_DispatchMessageA(_cpu *cpu)
{
    if (ARG(0) == 0)
    {
        RET(0);
        return;
    }
    RET(rt_user32_dispatch(cpu, ARG(0), false));
}

/* LRESULT DispatchMessageW(const MSG *) */
void rt_DispatchMessageW(_cpu *cpu)
{
    if (ARG(0) == 0)
    {
        RET(0);
        return;
    }
    RET(rt_user32_dispatch(cpu, ARG(0), true));
}

void rt_user32_post_quit(uint32_t code)
{
    mqueue *q;
    lock();
    q = queue_of(self_tid(), true);
    q->quit = true;
    q->quit_code = code;
    queue_signal(q, QS_POSTMESSAGE | QS_ALLPOSTMESSAGE);
    unlock();
}

/* void PostQuitMessage(int code) */
void rt_PostQuitMessage(_cpu *cpu)
{
    rt_user32_post_quit(ARG(0));
}

/* ---------------------------------------------------------------- SendMessage */

/* Send to one window or to all top-level windows (HWND_BROADCAST). */
static uint32_t send_any(_cpu *cpu, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp, bool unicode, uint32_t smto,
                         uint32_t timeout, bool *ok)
{
    uint32_t tid;
    wnd *w;
    if (hwnd == HWND_BROADCAST || hwnd == 0xFFFFFFFFu)
    {
        uint32_t list[MAX_WINDOWS], tids[MAX_WINDOWS];
        int i, n = 0;
        lock();
        for (i = 1; i < MAX_WINDOWS; i++)
        {
            if (windows[i].hwnd != 0 && windows[i].parent == 0 && !windows[i].destroying)
            {
                list[n] = windows[i].hwnd;
                tids[n++] = windows[i].tid;
            }
        }
        unlock();
        for (i = 0; i < n; i++)
        {
            if (!rt_user32_is_window(list[i])) continue;
            if (tids[i] == self_tid())
                call_proc(cpu, list[i], msg, wp, lp, unicode);
            else
                send_other(cpu, list[i], tids[i], msg, wp, lp, unicode, smto, timeout, NULL);
        }
        if (ok != NULL) *ok = true;
        return 1;
    }
    lock();
    w = get(hwnd);
    tid = w != NULL ? w->tid : 0;
    unlock();
    if (w == NULL)
    {
        rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        if (ok != NULL) *ok = false;
        return 0;
    }
    if (ok != NULL) *ok = true;
    if (tid == self_tid()) return call_proc(cpu, hwnd, msg, wp, lp, unicode);
    return send_other(cpu, hwnd, tid, msg, wp, lp, unicode, smto, timeout, ok);
}

/* LRESULT SendMessageA(HWND, UINT, WPARAM, LPARAM) */
void rt_SendMessageA(_cpu *cpu)
{
    RET(send_any(cpu, ARG(0), ARG(1), ARG(2), ARG(3), false, 0, 0xFFFFFFFFu, NULL));
}

static void send_timeout(_cpu *cpu, bool unicode)
{
    uint32_t hwnd = ARG(0), flags = ARG(4), timeout = ARG(5), result = ARG(6), r;
    bool ok;
    if (flags & ~(SMTO_BLOCK | SMTO_ABORTIFHUNG | SMTO_NOTIMEOUTIFNOTHUNG | SMTO_ERRORONEXIT))
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    /* SMTO_NOTIMEOUTIFNOTHUNG: no time limit while the target checks its
       queue; the hang test (SMTO_ABORTIFHUNG rule) still applies. */
    if (flags & SMTO_NOTIMEOUTIFNOTHUNG) timeout = 0xFFFFFFFFu;
    r = send_any(cpu, hwnd, ARG(1), ARG(2), ARG(3), unicode, flags, timeout, &ok);
    if (!ok)
    {
        RET(0);
        return;
    }
    if (result != 0) wr32(result, r);
    RET(1);
}

/* LRESULT SendMessageTimeoutA(HWND, UINT msg, WPARAM, LPARAM, UINT flags,
   UINT timeout, PDWORD_PTR result) */
void rt_SendMessageTimeoutA(_cpu *cpu) { send_timeout(cpu, false); }
void rt_SendMessageTimeoutW(_cpu *cpu) { send_timeout(cpu, true); }

/* UINT RegisterWindowMessageA(LPCSTR): 0xC000 to 0xFFFF, the same value
   for the same name (case is ignored). */
void rt_RegisterWindowMessageA(_cpu *cpu)
{
    char *name;
    int i;
    if (ARG(0) == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    name = guest_text(ARG(0), false);
    if (name[0] == 0 || strlen(name) > 255)
    {
        free(name);
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    lock();
    for (i = 0; i < n_window_messages; i++)
    {
        if (strcasecmp(window_messages[i], name) == 0)
        {
            unlock();
            free(name);
            RET(0xC000u + (uint32_t)i);
            return;
        }
    }
    if (n_window_messages >= MAX_WINDOW_MESSAGES)
        rt_fatal("RegisterWindowMessageA: more than %d registered messages", MAX_WINDOW_MESSAGES);
    window_messages[n_window_messages++] = name;
    i = n_window_messages - 1;
    unlock();
    RET(0xC000u + (uint32_t)i);
}

/* ---------------------------------------------------------------- keyboard */

typedef struct keymap {
    uint8_t vk, scan, ext;
} keymap;

/* macOS virtual key code (kVK_*) to the Windows key at the same position
   of a US keyboard. */
static const keymap keys[128] = {
    [0x00] = { 'A', 0x1E, 0 }, [0x01] = { 'S', 0x1F, 0 }, [0x02] = { 'D', 0x20, 0 },
    [0x03] = { 'F', 0x21, 0 }, [0x04] = { 'H', 0x23, 0 }, [0x05] = { 'G', 0x22, 0 },
    [0x06] = { 'Z', 0x2C, 0 }, [0x07] = { 'X', 0x2D, 0 }, [0x08] = { 'C', 0x2E, 0 },
    [0x09] = { 'V', 0x2F, 0 }, [0x0A] = { 0xE2, 0x56, 0 }, [0x0B] = { 'B', 0x30, 0 },
    [0x0C] = { 'Q', 0x10, 0 }, [0x0D] = { 'W', 0x11, 0 }, [0x0E] = { 'E', 0x12, 0 },
    [0x0F] = { 'R', 0x13, 0 }, [0x10] = { 'Y', 0x15, 0 }, [0x11] = { 'T', 0x14, 0 },
    [0x12] = { '1', 0x02, 0 }, [0x13] = { '2', 0x03, 0 }, [0x14] = { '3', 0x04, 0 },
    [0x15] = { '4', 0x05, 0 }, [0x16] = { '6', 0x07, 0 }, [0x17] = { '5', 0x06, 0 },
    [0x18] = { 0xBB, 0x0D, 0 }, [0x19] = { '9', 0x0A, 0 }, [0x1A] = { '7', 0x08, 0 },
    [0x1B] = { 0xBD, 0x0C, 0 }, [0x1C] = { '8', 0x09, 0 }, [0x1D] = { '0', 0x0B, 0 },
    [0x1E] = { 0xDD, 0x1B, 0 }, [0x1F] = { 'O', 0x18, 0 }, [0x20] = { 'U', 0x16, 0 },
    [0x21] = { 0xDB, 0x1A, 0 }, [0x22] = { 'I', 0x17, 0 }, [0x23] = { 'P', 0x19, 0 },
    [0x24] = { VK_RETURN, 0x1C, 0 }, [0x25] = { 'L', 0x26, 0 }, [0x26] = { 'J', 0x24, 0 },
    [0x27] = { 0xDE, 0x28, 0 }, [0x28] = { 'K', 0x25, 0 }, [0x29] = { 0xBA, 0x27, 0 },
    [0x2A] = { 0xDC, 0x2B, 0 }, [0x2B] = { 0xBC, 0x33, 0 }, [0x2C] = { 0xBF, 0x35, 0 },
    [0x2D] = { 'N', 0x31, 0 }, [0x2E] = { 'M', 0x32, 0 }, [0x2F] = { 0xBE, 0x34, 0 },
    [0x30] = { VK_TAB, 0x0F, 0 }, [0x31] = { ' ', 0x39, 0 }, [0x32] = { 0xC0, 0x29, 0 },
    [0x33] = { VK_BACK, 0x0E, 0 }, [0x35] = { VK_ESCAPE, 0x01, 0 },
    [0x36] = { 0x5C, 0x5C, 1 },          /* right Command: VK_RWIN */
    [0x37] = { 0x5B, 0x5B, 1 },          /* left Command: VK_LWIN */
    [0x38] = { VK_SHIFT, 0x2A, 0 }, [0x39] = { VK_CAPITAL, 0x3A, 0 }, [0x3A] = { VK_MENU, 0x38, 0 },
    [0x3B] = { VK_CONTROL, 0x1D, 0 }, [0x3C] = { VK_SHIFT, 0x36, 0 }, [0x3D] = { VK_MENU, 0x38, 1 },
    [0x3E] = { VK_CONTROL, 0x1D, 1 },
    [0x41] = { 0x6E, 0x53, 0 }, [0x43] = { 0x6A, 0x37, 0 }, [0x45] = { 0x6B, 0x4E, 0 },
    [0x47] = { VK_CLEAR, 0x4C, 0 },      /* keypad Clear (the Num Lock position) */
    [0x4B] = { 0x6F, 0x35, 1 }, [0x4C] = { VK_RETURN, 0x1C, 1 }, [0x4E] = { 0x6D, 0x4A, 0 },
    [0x51] = { 0xBB, 0x0D, 0 },          /* keypad = */
    [0x52] = { 0x60, 0x52, 0 }, [0x53] = { 0x61, 0x4F, 0 }, [0x54] = { 0x62, 0x50, 0 },
    [0x55] = { 0x63, 0x51, 0 }, [0x56] = { 0x64, 0x4B, 0 }, [0x57] = { 0x65, 0x4C, 0 },
    [0x58] = { 0x66, 0x4D, 0 }, [0x59] = { 0x67, 0x47, 0 }, [0x5B] = { 0x68, 0x48, 0 },
    [0x5C] = { 0x69, 0x49, 0 },
    [0x60] = { 0x74, 0x3F, 0 }, [0x61] = { 0x75, 0x40, 0 }, [0x62] = { 0x76, 0x41, 0 },
    [0x63] = { 0x72, 0x3D, 0 }, [0x64] = { 0x77, 0x42, 0 }, [0x65] = { 0x78, 0x43, 0 },
    [0x67] = { 0x7A, 0x57, 0 }, [0x69] = { 0x7C, 0x64, 0 }, [0x6B] = { 0x7D, 0x65, 0 },
    [0x6D] = { 0x79, 0x44, 0 }, [0x6F] = { 0x7B, 0x58, 0 }, [0x71] = { 0x7E, 0x66, 0 },
    [0x72] = { 0x2D, 0x52, 1 },          /* Help: VK_INSERT */
    [0x73] = { 0x24, 0x47, 1 }, [0x74] = { 0x21, 0x49, 1 }, [0x75] = { 0x2E, 0x53, 1 },
    [0x76] = { 0x73, 0x3E, 0 }, [0x77] = { 0x23, 0x4F, 1 }, [0x78] = { 0x71, 0x3C, 0 },
    [0x79] = { 0x22, 0x51, 1 }, [0x7A] = { 0x70, 0x3B, 0 }, [0x7B] = { 0x25, 0x4B, 1 },
    [0x7C] = { 0x27, 0x4D, 1 }, [0x7D] = { 0x28, 0x50, 1 }, [0x7E] = { 0x26, 0x48, 1 },
};

/* The specific key (left or right) for the generic modifier codes. */
static uint32_t side_vk(const keymap *k)
{
    if (k->vk == VK_SHIFT) return k->scan == 0x36 ? VK_RSHIFT : VK_LSHIFT;
    if (k->vk == VK_CONTROL) return k->ext ? VK_RCONTROL : VK_LCONTROL;
    if (k->vk == VK_MENU) return k->ext ? VK_RMENU : VK_LMENU;
    return k->vk;
}

static bool alt_down(void) { return (input_down[VK_LMENU] | input_down[VK_RMENU]) != 0; }
static bool ctrl_down(void) { return (input_down[VK_LCONTROL] | input_down[VK_RCONTROL]) != 0; }

#define VK_PROCESSKEY 0xE5u

static void key_input(bool down, bool is_repeat, uint16_t code, const uint16_t *chars, int nchars, bool ime)
{
    const keymap *k;
    qmsg m;
    uint32_t side, prev, lp, target;
    bool sys;
    if (code >= 128 || keys[code].vk == 0)
    {
        static bool logged[256];
        if (!logged[code & 0xFFu])
        {
            logged[code & 0xFFu] = true;
            rt_log("USER32: macOS key code 0x%02x has no Windows key; the key is ignored", code);
        }
        return;
    }
    k = &keys[code];
    rt_rawinput_key(k->vk, k->scan, k->ext != 0, down);
    side = side_vk(k);
    lock();
    prev = input_down[side] ? 1u : 0u;
    input_down[side] = down ? 1 : 0;
    if (down && !prev) async_pressed[side] = 1;
    if (side != k->vk)
    {
        /* The generic code is down while one of the two keys is down. */
        uint32_t a = k->vk == VK_SHIFT ? VK_LSHIFT : k->vk == VK_CONTROL ? VK_LCONTROL : VK_LMENU;
        input_down[k->vk] = (input_down[a] | input_down[a + 1u]) ? 1 : 0;
        if (down && input_down[k->vk]) async_pressed[k->vk] = 1;
    }
    {
        mqueue *q = queue_of(main_tid, true);
        target = key_target(q);
        /* With no focus window, the active window gets system key
           messages. */
        sys = (alt_down() && !ctrl_down()) || k->vk == VK_F10 || (q->focus == 0 && target != 0);
    }
    lp = 1u | ((uint32_t)k->scan << 16) | ((uint32_t)k->ext << 24);
    if (alt_down()) lp |= 1u << 29;
    if (down ? (prev || is_repeat) : true) lp |= 1u << 30;
    if (!down) lp |= 1u << 31;
    memset(&m, 0, sizeof m);
    m.hwnd = target;
    m.msg = down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP);
    m.wparam = ime && down ? VK_PROCESSKEY : k->vk;
    m.lparam = lp;
    m.time = now_ms();
    m.x = mouse_x;
    m.y = mouse_y;
    if (down && chars != NULL && !ime)
    {
        int i, n = nchars;
        if (n > (int)(sizeof m.chars / sizeof m.chars[0]))
        {
            rt_log("USER32: a key gives %d characters; the first %d are used", n, (int)(sizeof m.chars / sizeof m.chars[0]));
            n = (int)(sizeof m.chars / sizeof m.chars[0]);
        }
        for (i = 0; i < n; i++) m.chars[i] = chars[i];
        m.nchars = (uint8_t)n;
    }
    unlock();
    if (target != 0) post_input(&m, QS_KEY);
}

void rt_user32_on_key(uint32_t hwnd, bool down, bool is_repeat, uint16_t mac_keycode, const uint16_t *chars, int nchars,
                      bool ime)
{
    (void)hwnd;
    key_input(down, is_repeat, mac_keycode, chars, nchars, ime);
}

/* NSEvent device-dependent modifier bits and their macOS key codes. */
static const struct { uint32_t bit; uint16_t code; } mod_keys[] = {
    { 0x00000001u, 0x3B }, /* left Control */
    { 0x00000002u, 0x38 }, /* left Shift */
    { 0x00000004u, 0x3C }, /* right Shift */
    { 0x00000008u, 0x37 }, /* left Command */
    { 0x00000010u, 0x36 }, /* right Command */
    { 0x00000020u, 0x3A }, /* left Option */
    { 0x00000040u, 0x3D }, /* right Option */
    { 0x00002000u, 0x3E }, /* right Control */
};
#define MAC_CAPS_LOCK 0x00010000u

void rt_user32_on_modifiers(uint32_t hwnd, uint32_t flags)
{
    uint32_t changed;
    size_t i;
    (void)hwnd;
    lock();
    changed = flags ^ last_mods;
    last_mods = flags;
    unlock();
    for (i = 0; i < sizeof mod_keys / sizeof mod_keys[0]; i++)
        if (changed & mod_keys[i].bit) key_input((flags & mod_keys[i].bit) != 0, false, mod_keys[i].code, NULL, 0, false);
    /* macOS reports one event for each Caps Lock change: Windows has a
       press and a release. */
    if (changed & MAC_CAPS_LOCK)
    {
        key_input(true, false, 0x39, NULL, 0, false);
        key_input(false, false, 0x39, NULL, 0, false);
    }
}

static uint16_t key_state(uint32_t vk)
{
    uint16_t r;
    if (vk == VK_NUMLOCK) return 1; /* Mac keypads always give digits: Num Lock is on */
    if (vk == VK_SCROLL) return 0;  /* Mac keyboards have no Scroll Lock */
    lock();
    r = (uint16_t)(((sync_state[vk] & 0x80) ? 0xFF80u : 0) | (vk == VK_CAPITAL ? 0 : (sync_state[vk] & 0x01)));
    unlock();
    if (vk == VK_CAPITAL && rt_cocoa_caps_lock()) r |= 1;
    return r;
}

/* SHORT GetKeyState(int vk) */
void rt_GetKeyState(_cpu *cpu)
{
    RET((uint32_t)(int32_t)(int16_t)key_state(ARG(0) & 0xFFu));
}

/* SHORT GetAsyncKeyState(int vk): 0x8000 while the key is down, bit 0 if
   the key was pressed since the last call. */
void rt_GetAsyncKeyState(_cpu *cpu)
{
    uint32_t vk = ARG(0) & 0xFFu;
    uint16_t r = 0;
    lock();
    if (input_down[vk]) r |= 0x8000u;
    if (async_pressed[vk]) r |= 1u;
    async_pressed[vk] = 0;
    unlock();
    RET((uint32_t)(int32_t)(int16_t)r);
}

/* ---------------------------------------------------------------- mouse */

static uint32_t mouse_keys(void)
{
    uint32_t mk = 0;
    if (input_down[VK_LBUTTON]) mk |= MK_LBUTTON;
    if (input_down[VK_RBUTTON]) mk |= MK_RBUTTON;
    if (input_down[VK_MBUTTON]) mk |= MK_MBUTTON;
    if (input_down[VK_XBUTTON1]) mk |= MK_XBUTTON1;
    if (input_down[VK_XBUTTON2]) mk |= MK_XBUTTON2;
    if (input_down[VK_LSHIFT] | input_down[VK_RSHIFT]) mk |= MK_SHIFT;
    if (ctrl_down()) mk |= MK_CONTROL;
    return mk;
}

/* Raw input button flags (RI_MOUSE_*) of a mouse message. */
static uint32_t raw_buttons(uint32_t msg, int32_t data)
{
    switch (msg)
    {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: return 0x0001u;
    case WM_LBUTTONUP: return 0x0002u;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: return 0x0004u;
    case WM_RBUTTONUP: return 0x0008u;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: return 0x0010u;
    case WM_MBUTTONUP: return 0x0020u;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: return data == 2 ? 0x0100u : 0x0040u;
    case WM_XBUTTONUP: return data == 2 ? 0x0200u : 0x0080u;
    case WM_MOUSEWHEEL: return 0x0400u;
    case WM_MOUSEHWHEEL: return 0x0800u;
    default: return 0;
    }
}

void rt_user32_on_mouse(uint32_t hwnd, uint32_t msg, int32_t x, int32_t y, int32_t data)
{
    qmsg m;
    int32_t ox, oy, sx, sy;
    uint32_t target, vk = 0, bits;
    bool down = false;
    mqueue *q;
    wnd *w;
    lock();
    w = get(hwnd);
    if (w == NULL)
    {
        unlock();
        return;
    }
    client_origin(hwnd, &ox, &oy);
    sx = ox + x;
    sy = oy + y;
    mouse_x = sx;
    mouse_y = sy;
    switch (msg)
    {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: vk = VK_LBUTTON; down = true; break;
    case WM_LBUTTONUP: vk = VK_LBUTTON; break;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: vk = VK_RBUTTON; down = true; break;
    case WM_RBUTTONUP: vk = VK_RBUTTON; break;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: vk = VK_MBUTTON; down = true; break;
    case WM_MBUTTONUP: vk = VK_MBUTTON; break;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: vk = data == 2 ? VK_XBUTTON2 : VK_XBUTTON1; down = true; break;
    case WM_XBUTTONUP: vk = data == 2 ? VK_XBUTTON2 : VK_XBUTTON1; break;
    default: break;
    }
    if (vk != 0)
    {
        if (down && !input_down[vk]) async_pressed[vk] = 1;
        input_down[vk] = down ? 1 : 0;
    }
    q = queue_of(main_tid, true);
    target = hwnd;
    if (q->capture != 0 && get(q->capture) != NULL)
        target = q->capture;
    else if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL)
    {
        /* The wheel goes to the focus window. */
        uint32_t k = key_target(q);
        if (k != 0) target = k;
    }
    /* A double click needs CS_DBLCLKS in the class of the window. */
    if ((msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK || msg == WM_MBUTTONDBLCLK || msg == WM_XBUTTONDBLCLK) &&
        !(classes[get(target)->cls].style & CS_DBLCLKS))
        msg -= 2u; /* xBUTTONDBLCLK - 2 = xBUTTONDOWN */
    memset(&m, 0, sizeof m);
    m.hwnd = target;
    m.msg = msg;
    m.time = now_ms();
    m.x = sx;
    m.y = sy;
    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL)
    {
        /* The wheel messages have screen coordinates. */
        m.wparam = mouse_keys() | ((uint32_t)(uint16_t)(int16_t)data << 16);
        m.lparam = make_lparam(sx, sy);
    }
    else
    {
        int32_t tx, ty;
        client_origin(target, &tx, &ty);
        m.wparam = mouse_keys();
        if (msg >= WM_XBUTTONDOWN && msg <= WM_XBUTTONDBLCLK) m.wparam |= (uint32_t)data << 16;
        m.lparam = make_lparam(sx - tx, sy - ty);
    }
    bits = msg == WM_MOUSEMOVE ? QS_MOUSEMOVE : QS_MOUSEBUTTON;
    unlock();
    {
        uint32_t rb = raw_buttons(msg, data);
        if (rb != 0) rt_rawinput_mouse(0, 0, rb, (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) ? data : 0);
    }
    post_input(&m, bits);
}

void rt_user32_on_mouse_motion(double dx, double dy)
{
    rt_rawinput_mouse(dx, dy, 0, 0);
}

/* An input message of raw input (WM_INPUT) for the main thread. */
void rt_user32_post_raw(uint32_t hwnd, uint32_t wparam, uint32_t lparam)
{
    qmsg m;
    memset(&m, 0, sizeof m);
    m.hwnd = hwnd;
    m.msg = WM_INPUT;
    m.wparam = wparam;
    m.lparam = lparam;
    m.time = now_ms();
    lock();
    m.x = mouse_x;
    m.y = mouse_y;
    unlock();
    post_input(&m, QS_RAWINPUT);
}

/* The window that gets raw input without a target window: the focus
   window of the main thread, else its active window. */
uint32_t rt_user32_raw_target(void)
{
    uint32_t t;
    lock();
    t = key_target(queue_of(main_tid, true));
    unlock();
    return t;
}

/* The last cursor position that an event reported (screen coordinates). */
void rt_user32_mouse_pos(int32_t *x, int32_t *y)
{
    lock();
    *x = mouse_x;
    *y = mouse_y;
    unlock();
}

/* ---------------------------------------------------------------- native events */

void rt_user32_on_active(uint32_t hwnd, bool active)
{
    main_sys(SYS_ACTIVE, hwnd, active ? 1 : 0, 0);
}

bool rt_user32_app_is_active(void)
{
    bool a;
    lock();
    a = app_active;
    unlock();
    return a;
}

void rt_user32_on_app_active(bool active)
{
    main_sys(SYS_APP_ACTIVE, 0, active ? 1 : 0, 0);
}

void rt_user32_on_close(uint32_t hwnd)
{
    main_sys(SYS_CLOSE, hwnd, 0, 0);
}

void rt_user32_on_frame(uint32_t hwnd, bool resized)
{
    main_sys(SYS_FRAME, hwnd, resized ? 1 : 0, 0);
}

void rt_user32_on_minimized(uint32_t hwnd, bool minimized)
{
    main_sys(SYS_MINIMIZED, hwnd, minimized ? 1 : 0, 0);
}

void rt_user32_on_quit(void)
{
    main_sys(SYS_QUIT, 0, 0, 0);
}

void rt_user32_on_control(uint32_t hwnd, int event, int32_t a, int32_t b)
{
    sysmsg m;
    memset(&m, 0, sizeof m);
    m.kind = SYS_CONTROL;
    m.hwnd = hwnd;
    m.a = event;
    m.b = a;
    m.c = b;
    lock();
    sys_push(queue_of(main_tid, true), &m);
    unlock();
}

static void ime_event(int kind, uint32_t hwnd, const uint16_t *text, int n, int caret)
{
    sysmsg m;
    memset(&m, 0, sizeof m);
    if (n < 0 || n > 4096) rt_fatal("USER32: input method text of %d characters", n);
    m.kind = kind;
    m.hwnd = hwnd;
    m.a = n;
    m.b = caret;
    m.text = xcalloc((size_t)n + 1u, sizeof(uint16_t));
    if (n != 0) memcpy(m.text, text, (size_t)n * sizeof(uint16_t));
    lock();
    sys_push(queue_of(main_tid, true), &m);
    unlock();
}

void rt_user32_on_ime_composition(uint32_t hwnd, const uint16_t *text, int n, int caret)
{
    ime_event(SYS_IME_COMP, hwnd, text, n, caret);
}

void rt_user32_on_ime_result(uint32_t hwnd, const uint16_t *text, int n)
{
    ime_event(SYS_IME_RESULT, hwnd, text, n, 0);
}

/* ---------------------------------------------------------------- input method state (rt_imm32.c) */

/* The input method of a window is active when the window has the input
   context of its thread (wnd.ime) and the context is open
   (mqueue.ime_closed). Only guest windows have an input method: the view
   of a top-level window and the view of a child of a guest class
   (RTView). The views of the dialog controls are AppKit controls. */
static void *ime_view(uint32_t hwnd, bool *active)
{
    void *view = NULL;
    wnd *w;
    *active = false;
    lock();
    w = get(hwnd);
    if (w != NULL && w->ns != NULL && (w->parent == 0 || host_of(w) == NULL))
    {
        mqueue *q = queue_of(w->tid, false);
        *active = w->ime && (q == NULL || !q->ime_closed);
        view = w->parent == 0 ? rt_cocoa_content_view(w->ns) : w->ns;
    }
    unlock();
    return view;
}

/* End the current composition (main thread). */
static void ime_discard(uint32_t target)
{
    free(ime_comp);
    ime_comp = NULL;
    ime_ncomp = 0;
    ime_caret = 0;
    ime_end(target);
}

static void ime_apply(uint32_t hwnd)
{
    bool active;
    void *view = ime_view(hwnd, &active);
    if (view == NULL || !on_main()) return;
    rt_cocoa_ime_enable(view, active);
    if (!active && ime_composing)
    {
        uint32_t target;
        lock();
        target = key_target(queue_of(main_tid, true));
        unlock();
        if (target == hwnd || rt_user32_is_self_or_child(hwnd, target)) ime_discard(target);
    }
}

static mqueue *queue_of_himc(uint32_t himc) /* lock held */
{
    int i;
    if (himc == 0) return NULL;
    for (i = 0; i < MAX_QUEUES; i++)
        if (queues[i].tid != 0 && queues[i].himc == himc) return &queues[i];
    return NULL;
}

uint32_t rt_user32_ime_context(uint32_t hwnd)
{
    uint32_t himc = 0;
    wnd *w;
    lock();
    w = get(hwnd);
    if (w != NULL && w->ime) himc = queue_of(w->tid, true)->himc;
    unlock();
    return himc;
}

uint32_t rt_user32_ime_window_thread_context(uint32_t hwnd)
{
    uint32_t himc = 0;
    wnd *w;
    lock();
    w = get(hwnd);
    if (w != NULL) himc = queue_of(w->tid, true)->himc;
    unlock();
    return himc;
}

bool rt_user32_ime_valid(uint32_t himc)
{
    bool ok;
    lock();
    ok = queue_of_himc(himc) != NULL;
    unlock();
    return ok;
}

bool rt_user32_ime_associate(uint32_t hwnd, uint32_t himc)
{
    bool changed = false;
    wnd *w;
    lock();
    w = get(hwnd);
    if (w == NULL || (himc != 0 && queue_of(w->tid, true)->himc != himc))
    {
        unlock();
        return false;
    }
    changed = w->ime != (himc != 0);
    w->ime = himc != 0;
    unlock();
    if (changed) ime_apply(hwnd);
    return true;
}

bool rt_user32_ime_get_open(uint32_t himc, bool *open)
{
    mqueue *q;
    lock();
    q = queue_of_himc(himc);
    if (q != NULL) *open = !q->ime_closed;
    unlock();
    return q != NULL;
}

bool rt_user32_ime_set_open(uint32_t himc, bool open, bool *changed)
{
    uint32_t list[MAX_WINDOWS], tid;
    int i, n = 0;
    mqueue *q;
    lock();
    q = queue_of_himc(himc);
    if (q == NULL)
    {
        unlock();
        return false;
    }
    *changed = q->ime_closed == open;
    q->ime_closed = !open;
    tid = q->tid;
    if (*changed)
        for (i = 1; i < MAX_WINDOWS; i++)
            if (windows[i].hwnd != 0 && windows[i].tid == tid && windows[i].ns != NULL) list[n++] = windows[i].hwnd;
    unlock();
    for (i = 0; i < n; i++) ime_apply(list[i]);
    return true;
}

void rt_user32_ime_strings(uint32_t himc, const uint16_t **comp, int *ncomp, int *caret, const uint16_t **result,
                           int *nresult)
{
    bool main_ctx;
    lock();
    main_ctx = queue_of_himc(himc) == queue_of(main_tid, false) && himc != 0;
    unlock();
    *comp = main_ctx ? ime_comp : NULL;
    *ncomp = main_ctx ? ime_ncomp : 0;
    *caret = main_ctx ? ime_caret : 0;
    *result = main_ctx ? ime_result : NULL;
    *nresult = main_ctx ? ime_nresult : 0;
}

void rt_user32_ime_cancel(uint32_t himc)
{
    uint32_t target;
    bool active;
    void *view;
    lock();
    if (himc == 0 || queue_of_himc(himc) != queue_of(main_tid, false))
    {
        unlock();
        return;
    }
    target = key_target(queue_of(main_tid, true));
    unlock();
    if (target == 0 || !on_main()) return;
    view = ime_view(target, &active);
    if (view != NULL) rt_cocoa_ime_cancel(view);
    ime_discard(target);
}

/* ---------------------------------------------------------------- game controller (rt_gamepad.m) */

/* The window for controller input and the cursor position in its client
   area, limited to the client area. False if there is no window. */
static bool pad_target(uint32_t *hwnd, int32_t *x, int32_t *y, int32_t *ox, int32_t *oy, int32_t *w, int32_t *h)
{
    uint32_t t = rt_user32_raw_target();
    int32_t cx, cy;
    wnd *win;
    rt_rect c;
    if (t == 0) return false;
    lock();
    win = get(t);
    if (win == NULL)
    {
        unlock();
        return false;
    }
    client_origin(t, ox, oy);
    c = client_of(win);
    unlock();
    *w = c.right - c.left;
    *h = c.bottom - c.top;
    if (*w <= 0 || *h <= 0) return false;
    rt_cocoa_cursor_pos(&cx, &cy);
    *x = cx - *ox;
    *y = cy - *oy;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    if (*x >= *w) *x = *w - 1;
    if (*y >= *h) *y = *h - 1;
    *hwnd = t;
    return true;
}

void rt_user32_pad_move(int32_t dx, int32_t dy)
{
    uint32_t hwnd;
    int32_t x, y, ox, oy, w, h, nx, ny;
    if (!pad_target(&hwnd, &x, &y, &ox, &oy, &w, &h)) return;
    /* dx, dy are a few pixels for each controller sample: no overflow. */
    nx = x + dx;
    ny = y + dy;
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx >= w) nx = w - 1;
    if (ny >= h) ny = h - 1;
    if (nx == x && ny == y) return;
    rt_cocoa_cursor_move(ox + nx, oy + ny);
    rt_user32_on_mouse_motion((double)(nx - x), (double)(ny - y));
    rt_user32_on_mouse(hwnd, WM_MOUSEMOVE, nx, ny, 0);
}

void rt_user32_pad_button(int button, bool down)
{
    static const uint32_t msgs[3][2] = { { WM_LBUTTONUP, WM_LBUTTONDOWN }, { WM_RBUTTONUP, WM_RBUTTONDOWN },
                                         { WM_MBUTTONUP, WM_MBUTTONDOWN } };
    uint32_t hwnd;
    int32_t x, y, ox, oy, w, h;
    if (button < 0 || button > 2) rt_fatal("rt_user32_pad_button: button %d", button);
    if (!pad_target(&hwnd, &x, &y, &ox, &oy, &w, &h)) return;
    rt_user32_on_mouse(hwnd, msgs[button][down ? 1 : 0], x, y, 0);
}

void rt_user32_pad_wheel(int32_t delta)
{
    uint32_t hwnd;
    int32_t x, y, ox, oy, w, h;
    if (!pad_target(&hwnd, &x, &y, &ox, &oy, &w, &h)) return;
    rt_user32_on_mouse(hwnd, WM_MOUSEWHEEL, x, y, delta);
}

void rt_user32_pad_key(uint16_t mac_keycode, bool down)
{
    if (mac_keycode >= 128 || keys[mac_keycode].vk == 0) rt_fatal("rt_user32_pad_key: no key for code 0x%02x", mac_keycode);
    if (rt_user32_raw_target() == 0) return;
    key_input(down, false, mac_keycode, NULL, 0, false);
}

/* ---------------------------------------------------------------- input script (headless) */

/* rt_user32_input_script: timed mouse and key events for a headless run.
   A host thread waits for the time of each event and sends the event to
   the main thread through the main dispatch queue (rt_cocoa_pump runs it),
   where it goes through the same functions as a native event. */

#define SCRIPT_MAX_EVENTS 4096u
#define SCRIPT_MAX_SECONDS 86400.0

/* The key window when the script made the application inactive. */
static uint32_t script_key_window;

enum { SEV_MOVE, SEV_DOWN, SEV_UP, SEV_RDOWN, SEV_RUP, SEV_KEYDOWN, SEV_KEYUP, SEV_APP_INACTIVE, SEV_APP_ACTIVE, SEV_DEMINIATURIZE };

typedef struct script_ev {
    double t;      /* seconds after the start of the script */
    unsigned line; /* line of the script file (log and sort order) */
    int kind;
    int32_t x, y;  /* client coordinates of the active window (mouse) */
    uint16_t code; /* Mac virtual key code (key) */
    uint16_t ch;   /* the character of a key-down (text), 0 if none */
    uint32_t hwnd; /* the window of deminiaturize */
} script_ev;

static script_ev script_evs[SCRIPT_MAX_EVENTS];
static size_t script_n;
static double script_start;

static double script_now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) rt_fatal("clock_gettime failed");
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int script_cmp(const void *a, const void *b)
{
    const script_ev *x = a, *y = b;
    if (x->t != y->t) return x->t < y->t ? -1 : 1;
    if (x->line != y->line) return x->line < y->line ? -1 : 1;
    return x->kind == SEV_DOWN || x->kind == SEV_RDOWN || x->kind == SEV_KEYDOWN ? -1 : 1;
}

static void script_add(double t, unsigned line, int kind, int32_t x, int32_t y, uint16_t code, uint16_t ch)
{
    script_ev *e;
    if (script_n >= SCRIPT_MAX_EVENTS) rt_fatal("input script: more than %u events", SCRIPT_MAX_EVENTS);
    e = &script_evs[script_n++];
    e->t = t;
    e->line = line;
    e->kind = kind;
    e->x = x;
    e->y = y;
    e->code = code;
    e->ch = ch;
    e->hwnd = 0;
}

/* The Mac virtual key code (ANSI keyboard) of a character for the text
   command; -1 for a character that the command does not support. */
static int script_char_key(char c)
{
    static const char letters[] = "asdfhgzxcv\0bqweryt";
    static const uint8_t digits[10] = { 0x1D, 0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19 };
    const char *p;
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c == ' ') return 0x31;
    if (c >= 'a' && c <= 'z')
    {
        p = memchr(letters, c, sizeof letters - 1);
        if (p != NULL) return (int)(p - letters);
        switch (c)
        {
        case 'o': return 0x1F;
        case 'u': return 0x20;
        case 'i': return 0x22;
        case 'p': return 0x23;
        case 'l': return 0x25;
        case 'j': return 0x26;
        case 'k': return 0x28;
        case 'n': return 0x2D;
        case 'm': return 0x2E;
        default: break;
        }
    }
    return -1;
}

/* Main thread. */
static void script_run(void *ctx)
{
    const script_ev *e = ctx;
    static const char *const names[] = { "move", "down", "up", "rdown", "rup", "keydown", "keyup", "appinactive",
                                         "appactive", "deminiaturize" };
    uint32_t hwnd = rt_user32_raw_target();
    if (e->kind == SEV_DEMINIATURIZE)
    {
        /* windowDidDeminiaturize: the user restored the window from the
           Dock. */
        rt_log("input script: %.2f s line %u deminiaturize window 0x%08x", e->t, e->line, e->hwnd);
        if (!rt_user32_is_window(e->hwnd)) rt_fatal("input script line %u: 0x%08x is not a window", e->line, e->hwnd);
        rt_user32_on_minimized(e->hwnd, false);
        return;
    }
    if (e->kind == SEV_APP_INACTIVE || e->kind == SEV_APP_ACTIVE)
    {
        /* An inactive application has no active window. */
        rt_log("input script: %.2f s line %u %s (window 0x%08x)", e->t, e->line, names[e->kind], hwnd);
        if (e->kind == SEV_APP_INACTIVE)
        {
            /* The order of the Cocoa notifications when the user selects
               a different application: windowDidResignKey, then
               applicationDidResignActive. */
            if (hwnd != 0) rt_user32_on_active(hwnd, false);
            script_key_window = hwnd;
            rt_user32_on_app_active(false);
        }
        else
        {
            /* applicationDidBecomeActive. A minimized window does not
               become the key window until it is restored; a window that
               is not minimized becomes the key window again
               (windowDidBecomeKey). */
            uint32_t k = script_key_window;
            wnd *w;
            script_key_window = 0;
            rt_user32_on_app_active(true);
            w = k != 0 ? get(k) : NULL;
            if (w != NULL && !(w->style & WS_MINIMIZE)) rt_user32_on_active(k, true);
        }
        return;
    }
    if (hwnd == 0) rt_fatal("input script line %u: the program has no active window", e->line);
    rt_log("input script: %.2f s line %u %s %d %d 0x%02x (window 0x%08x)", e->t, e->line, names[e->kind], e->x, e->y,
           e->code, hwnd);
    if (e->kind == SEV_KEYDOWN || e->kind == SEV_KEYUP)
    {
        rt_user32_on_key(hwnd, e->kind == SEV_KEYDOWN, false, e->code, e->kind == SEV_KEYDOWN && e->ch != 0 ? &e->ch : NULL,
                         e->kind == SEV_KEYDOWN && e->ch != 0 ? 1 : 0, false);
        return;
    }
    {
        int32_t ox, oy, cx, cy;
        lock();
        client_origin(hwnd, &ox, &oy);
        unlock();
        rt_cocoa_cursor_pos(&cx, &cy);
        if (cx != ox + e->x || cy != oy + e->y)
        {
            rt_cocoa_cursor_move(ox + e->x, oy + e->y);
            rt_user32_on_mouse_motion((double)(ox + e->x - cx), (double)(oy + e->y - cy));
            rt_user32_on_mouse(hwnd, WM_MOUSEMOVE, e->x, e->y, 0);
        }
        switch (e->kind)
        {
        case SEV_DOWN: rt_user32_on_mouse(hwnd, WM_LBUTTONDOWN, e->x, e->y, 0); break;
        case SEV_UP: rt_user32_on_mouse(hwnd, WM_LBUTTONUP, e->x, e->y, 0); break;
        case SEV_RDOWN: rt_user32_on_mouse(hwnd, WM_RBUTTONDOWN, e->x, e->y, 0); break;
        case SEV_RUP: rt_user32_on_mouse(hwnd, WM_RBUTTONUP, e->x, e->y, 0); break;
        default: break;
        }
    }
}

static void *script_thread(void *arg)
{
    size_t i;
    (void)arg;
    for (i = 0; i < script_n; i++)
    {
        double wait = script_start + script_evs[i].t - script_now();
        if (wait > 0.0)
        {
            struct timespec ts, rem;
            ts.tv_sec = (time_t)wait;
            ts.tv_nsec = (long)((wait - (double)ts.tv_sec) * 1e9);
            while (nanosleep(&ts, &rem) != 0)
            {
                if (errno != EINTR) rt_fatal("input script: nanosleep failed: %s", strerror(errno));
                ts = rem;
            }
        }
        dispatch_async_f(dispatch_get_main_queue(), &script_evs[i], script_run);
    }
    rt_log("input script: all %zu events sent", script_n);
    return NULL;
}

/* Script lines (times in seconds after the call; # starts a comment):
     <t> move <x> <y>      move the cursor (client coordinates)
     <t> click <x> <y>     left button down at t, up at t + 0.1
     <t> down <x> <y>      left button down
     <t> up <x> <y>        left button up
     <t> rclick <x> <y>    right button down at t, up at t + 0.1
     <t> key <code>        key down at t, up at t + 0.05 (Mac virtual key
                           code, decimal or 0x hexadecimal)
     <t> text <string>     for each character (letters, digits, space) a
                           key with that character, 0.1 s apart; an
                           upper-case letter has no Shift key event
     <t> appinactive       the application stops being active (as when
                           the user selects a different application)
     <t> appactive         the application becomes active again
     <t> deminiaturize <h> window h (hexadecimal) is restored from the
                           Dock */
void rt_user32_input_script(const char *path)
{
    FILE *f;
    char buf[256];
    unsigned line = 0;
    pthread_t th;
    pthread_attr_t a;
    if (!atomic_load(&headless) || !rt_cocoa_headless()) rt_fatal("input script: only a headless run can use it");
    f = fopen(path, "r");
    if (f == NULL) rt_fatal("input script %s: %s", path, strerror(errno));
    while (fgets(buf, sizeof buf, f) != NULL)
    {
        char cmd[16], *hash;
        double t;
        int x, y, n, used = 0;
        unsigned code;
        line++;
        if (strchr(buf, '\n') == NULL && !feof(f)) rt_fatal("input script line %u: longer than %zu bytes", line, sizeof buf - 2);
        hash = strchr(buf, '#');
        if (hash != NULL) *hash = 0;
        n = sscanf(buf, " %lf %15s %n", &t, cmd, &used);
        if (n == EOF || (n <= 0 && buf[strspn(buf, " \t\r\n")] == 0)) continue;
        if (n != 2 || !(t >= 0.0 && t <= SCRIPT_MAX_SECONDS))
            rt_fatal("input script line %u: expected <seconds 0 to %.0f> <command>", line, SCRIPT_MAX_SECONDS);
        if (strcmp(cmd, "key") == 0)
        {
            char *end;
            unsigned long v;
            errno = 0;
            v = strtoul(buf + used, &end, 0);
            if (errno != 0 || end == buf + used || buf[used + strspn(buf + used, "0123456789abcdefxABCDEFX")] > ' ' || v > 0x7Fu)
                rt_fatal("input script line %u: key needs a Mac virtual key code from 0 to 0x7f", line);
            code = (unsigned)v;
            script_add(t, line, SEV_KEYDOWN, 0, 0, (uint16_t)code, 0);
            script_add(t + 0.05, line, SEV_KEYUP, 0, 0, (uint16_t)code, 0);
            continue;
        }
        if (strcmp(cmd, "text") == 0)
        {
            size_t i, len = strcspn(buf + used, "\r\n");
            if (len == 0) rt_fatal("input script line %u: text needs a string", line);
            for (i = 0; i < len; i++)
            {
                int k = script_char_key(buf[used + i]);
                if (k < 0) rt_fatal("input script line %u: text: the character '%c' is not supported", line, buf[used + i]);
                script_add(t + 0.1 * (double)i, line, SEV_KEYDOWN, 0, 0, (uint16_t)k, (uint16_t)(unsigned char)buf[used + i]);
                script_add(t + 0.1 * (double)i + 0.05, line, SEV_KEYUP, 0, 0, (uint16_t)k, 0);
            }
            continue;
        }
        if (strcmp(cmd, "deminiaturize") == 0)
        {
            char *end;
            unsigned long v;
            errno = 0;
            v = strtoul(buf + used, &end, 16);
            if (errno != 0 || end == buf + used || v == 0 || v > 0xFFFFFFFFul || end[strspn(end, " \t\r\n")] != 0)
                rt_fatal("input script line %u: deminiaturize needs a window handle (hexadecimal)", line);
            script_add(t, line, SEV_DEMINIATURIZE, 0, 0, 0, 0);
            script_evs[script_n - 1].hwnd = (uint32_t)v;
            continue;
        }
        if (strcmp(cmd, "appinactive") == 0 || strcmp(cmd, "appactive") == 0)
        {
            if (buf[used + strspn(buf + used, " \t\r\n")] != 0) rt_fatal("input script line %u: %s has no arguments", line, cmd);
            script_add(t, line, strcmp(cmd, "appinactive") == 0 ? SEV_APP_INACTIVE : SEV_APP_ACTIVE, 0, 0, 0, 0);
            continue;
        }
        if (sscanf(buf + used, " %d %d %n", &x, &y, &n) != 2 || buf[used + n] != 0 || x < 0 || y < 0 || x > 32767 ||
            y > 32767)
            rt_fatal("input script line %u: %s needs two coordinates from 0 to 32767", line, cmd);
        if (strcmp(cmd, "move") == 0)
            script_add(t, line, SEV_MOVE, x, y, 0, 0);
        else if (strcmp(cmd, "down") == 0)
            script_add(t, line, SEV_DOWN, x, y, 0, 0);
        else if (strcmp(cmd, "up") == 0)
            script_add(t, line, SEV_UP, x, y, 0, 0);
        else if (strcmp(cmd, "click") == 0)
        {
            script_add(t, line, SEV_DOWN, x, y, 0, 0);
            script_add(t + 0.1, line, SEV_UP, x, y, 0, 0);
        }
        else if (strcmp(cmd, "rclick") == 0)
        {
            script_add(t, line, SEV_RDOWN, x, y, 0, 0);
            script_add(t + 0.1, line, SEV_RUP, x, y, 0, 0);
        }
        else
            rt_fatal("input script line %u: unknown command %s", line, cmd);
    }
    if (ferror(f)) rt_fatal("input script %s: read error", path);
    fclose(f);
    qsort(script_evs, script_n, sizeof script_evs[0], script_cmp);
    script_start = script_now();
    if (pthread_attr_init(&a) != 0 || pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED) != 0 ||
        pthread_create(&th, &a, script_thread, NULL) != 0)
        rt_fatal("input script: the thread could not start");
    pthread_attr_destroy(&a);
    rt_log("input script %s: %zu events", path, script_n);
}

/* ---------------------------------------------------------------- start-up */

void rt_user32_set_headless(void)
{
    atomic_store(&headless, true);
}

bool rt_user32_headless(void)
{
    return atomic_load(&headless);
}

/* Register the runtime classes. The host calls it on the main thread
   before guest code runs. */
void rt_user32_init(void)
{
    if (!on_main()) rt_fatal("rt_user32_init must run on the main thread");
    lock();
    main_tid = self_tid();
    windows[0].hwnd = DESKTOP_HWND;
    generations[0] = 1;
    unlock();
    rt_dialog_init();
}

/* ---------------------------------------------------------------- MessageBox */

#define MB_TYPEMASK 0x0000000Fu
#define MB_ICONMASK 0x000000F0u
#define MB_DEFMASK 0x00000F00u
#define MB_HELP 0x00004000u

/* Show a message box as a system alert (CFUserNotification), which works
   on any thread. In headless mode the runtime shows no alert: it writes
   the message to the log and stops the process. */
static uint32_t message_box(const char *fn, char *text, char *caption, uint32_t type)
{
    static const struct { const char *label[3]; uint32_t id[3]; int n; } sets[7] = {
        { { "OK", NULL, NULL }, { 1, 0, 0 }, 1 },                    /* MB_OK */
        { { "OK", "Cancel", NULL }, { 1, 2, 0 }, 2 },                /* MB_OKCANCEL */
        { { "Abort", "Retry", "Ignore" }, { 3, 4, 5 }, 3 },          /* MB_ABORTRETRYIGNORE */
        { { "Yes", "No", "Cancel" }, { 6, 7, 2 }, 3 },               /* MB_YESNOCANCEL */
        { { "Yes", "No", NULL }, { 6, 7, 0 }, 2 },                   /* MB_YESNO */
        { { "Retry", "Cancel", NULL }, { 4, 2, 0 }, 2 },             /* MB_RETRYCANCEL */
        { { "Cancel", "Try Again", "Continue" }, { 2, 10, 11 }, 3 }, /* MB_CANCELTRYCONTINUE */
    };
    uint32_t set = type & MB_TYPEMASK, icon = type & MB_ICONMASK, def = (type & MB_DEFMASK) >> 8;
    CFStringRef t, c, b[3] = { NULL, NULL, NULL };
    CFOptionFlags level, resp = 0;
    int order[3], i, n;
    SInt32 rc;

    if (set > 6) rt_fatal("%s: type 0x%x is not supported", fn, type);
    if (type & MB_HELP) rt_fatal("%s: MB_HELP is not supported", fn);
    rt_log("%s: [%s] %s", fn, caption, text);
    if (atomic_load(&headless) || rt_cocoa_headless()) rt_fatal("%s in headless mode: [%s] %s", fn, caption, text);

    n = sets[set].n;
    if ((int)def >= n) def = 0;
    /* The default button comes first (the default button of
       CFUserNotification). */
    order[0] = (int)def;
    for (i = 0, n = 1; i < sets[set].n; i++)
        if (i != (int)def) order[n++] = i;
    t = CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
    c = CFStringCreateWithCString(NULL, caption, kCFStringEncodingUTF8);
    free(text);
    free(caption);
    if (t == NULL || c == NULL) rt_fatal("%s: text conversion failed", fn);
    for (i = 0; i < n; i++)
    {
        b[i] = CFStringCreateWithCString(NULL, sets[set].label[order[i]], kCFStringEncodingUTF8);
        if (b[i] == NULL) rt_fatal("%s: text conversion failed", fn);
    }
    level = icon == 0x10 ? kCFUserNotificationStopAlertLevel
          : icon == 0x30 ? kCFUserNotificationCautionAlertLevel
          : icon == 0x20 || icon == 0x40 ? kCFUserNotificationNoteAlertLevel
                                         : kCFUserNotificationPlainAlertLevel;
    rc = CFUserNotificationDisplayAlert(0, level, NULL, NULL, NULL, c, t, b[0], b[1], b[2], &resp);
    CFRelease(t);
    CFRelease(c);
    for (i = 0; i < n; i++) CFRelease(b[i]);
    if (rc != 0) rt_fatal("%s: the alert could not be shown (error %d)", fn, (int)rc);
    resp &= 3;
    if (resp == kCFUserNotificationDefaultResponse) return sets[set].id[order[0]];
    if (resp == kCFUserNotificationAlternateResponse && n > 1) return sets[set].id[order[1]];
    if (resp == kCFUserNotificationOtherResponse && n > 2) return sets[set].id[order[2]];
    rt_fatal("%s: unexpected alert response %lu", fn, (unsigned long)resp);
}

/* int MessageBoxA(HWND, LPCSTR text, LPCSTR caption, UINT type) */
void rt_MessageBoxA(_cpu *cpu)
{
    char *text = ARG(1) != 0 ? guest_text(ARG(1), false) : xstrdup("");
    char *caption = ARG(2) != 0 ? guest_text(ARG(2), false) : xstrdup("Error");
    RET(message_box("MessageBoxA", text, caption, ARG(3)));
}

/* int MessageBoxW(HWND, LPCWSTR text, LPCWSTR caption, UINT type) */
void rt_MessageBoxW(_cpu *cpu)
{
    char *text = ARG(1) != 0 ? guest_text(ARG(1), true) : xstrdup("");
    char *caption = ARG(2) != 0 ? guest_text(ARG(2), true) : xstrdup("Error");
    RET(message_box("MessageBoxW", text, caption, ARG(3)));
}
