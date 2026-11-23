/*
 * The Cocoa side of the window system (the interface is rt_window.h).
 *
 * The runtime does not call -[NSApplication run]. The main thread runs
 * the guest, and it handles native events when the guest checks its
 * message queue (rt_cocoa_pump) or waits (rt_cocoa_wait). Native events
 * become rt_user32_on_* calls; USER32 puts them in the message queues.
 *
 * Coordinates: Win32 screen coordinates are Core Graphics global display
 * coordinates (origin at the top left corner of the main display, y down,
 * points). AppKit screen coordinates have the origin at the bottom left
 * corner of the main display and y up.
 */
#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/runtime.h>

#include "rt.h"
#include "rt_user32.h"
#include "rt_window.h"

#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Win32 message numbers that this file reports. */
#define WM_MOUSEMOVE 0x0200u
#define WM_LBUTTONDOWN 0x0201u
#define WM_LBUTTONUP 0x0202u
#define WM_LBUTTONDBLCLK 0x0203u
#define WM_RBUTTONDOWN 0x0204u
#define WM_RBUTTONUP 0x0205u
#define WM_RBUTTONDBLCLK 0x0206u
#define WM_MBUTTONDOWN 0x0207u
#define WM_MBUTTONUP 0x0208u
#define WM_MBUTTONDBLCLK 0x0209u
#define WM_MOUSEWHEEL 0x020Au
#define WM_XBUTTONDOWN 0x020Bu
#define WM_XBUTTONUP 0x020Cu
#define WM_XBUTTONDBLCLK 0x020Du
#define WM_MOUSEHWHEEL 0x020Eu

#define MAC_KEY_ESCAPE 0x35u
#define MAC_KEY_BACKSPACE 0x33u

/* The subtype of the application-defined event that rt_cocoa_wake posts. */
#define WAKE_SUBTYPE 0x5254

static bool g_headless;
static atomic_int g_title_h;
/* Win32 units for each point (the backing scale of the main display at
   start, 2 on a Retina display). The program sees the screen, its windows
   and the cursor in backing pixels: the game renders and lays out its UI at
   the full resolution of a Retina display, with no upscale. Set once by
   rt_cocoa_init, before other threads run. */
static int g_px = 1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool g_wake_posted;

static void glock(void)
{
    if (pthread_mutex_lock(&g_lock) != 0) rt_fatal("Cocoa lock failed");
}

static void gunlock(void)
{
    if (pthread_mutex_unlock(&g_lock) != 0) rt_fatal("Cocoa unlock failed");
}

bool rt_cocoa_is_main_thread(void)
{
    return pthread_main_np() != 0;
}

static void need_main(const char *fn)
{
    if (!pthread_main_np()) rt_fatal("%s: Cocoa windows can be changed only on the main thread", fn);
}

static int32_t round32(CGFloat v)
{
    double r = round((double)v);
    if (r > 1e9) return 1000000000;
    if (r < -1e9) return -1000000000;
    return (int32_t)r;
}

static CGFloat main_height(void)
{
    return CGDisplayBounds(CGMainDisplayID()).size.height;
}

/* Win32 units <-> points (g_px). */
static CGFloat pt(int32_t v)
{
    return (CGFloat)v / (CGFloat)g_px;
}

static int32_t px(CGFloat v)
{
    return round32(v * (CGFloat)g_px);
}

/* Win32 screen rectangle -> AppKit screen rectangle. */
static NSRect to_ns(rt_rect r)
{
    return NSMakeRect(pt(r.left), main_height() - pt(r.bottom), pt(r.right - r.left), pt(r.bottom - r.top));
}

/* AppKit screen rectangle -> Win32 screen rectangle. */
static rt_rect from_ns(NSRect f)
{
    rt_rect r;
    r.left = px(f.origin.x);
    r.top = px(main_height() - NSMaxY(f));
    r.right = r.left + px(f.size.width);
    r.bottom = r.top + px(f.size.height);
    return r;
}

static NSString *ns_string(const char *s)
{
    NSString *v = s != NULL ? [NSString stringWithUTF8String:s] : @"";
    if (v == nil) rt_fatal("Cocoa: a string is not valid UTF-8");
    return v;
}

static char *c_string(NSString *s)
{
    const char *u = s != nil ? s.UTF8String : "";
    char *r = strdup(u != NULL ? u : "");
    if (r == NULL) rt_fatal("out of host memory (string)");
    return r;
}

/* ---------------------------------------------------------------- the hwnd of a native view */

static char k_hwnd_key, k_model_key, k_list_key;

static void set_view_info(NSView *v, uint32_t hwnd)
{
    objc_setAssociatedObject(v, &k_hwnd_key, @(hwnd), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

static uint32_t view_hwnd(id v)
{
    NSNumber *n = v != nil ? objc_getAssociatedObject(v, &k_hwnd_key) : nil;
    return n != nil ? n.unsignedIntValue : 0;
}


/* ---------------------------------------------------------------- cursor state */

@interface RTCursor : NSObject
{
@public
    NSData *rgba;
    uint32_t w, h;
    int32_t hot_x, hot_y;
    uint32_t idc;      /* a system cursor, else 0 */
    NSCursor *cursor;  /* made on the main thread */
}
@end

@implementation RTCursor
@end

static RTCursor *g_want_cursor; /* g_lock */
static bool g_want_visible;     /* g_lock */
static bool g_cursor_dirty;     /* g_lock */
static bool g_cursor_hidden;    /* main thread */

static bool g_clip_set;  /* g_lock */
static rt_rect g_clip;   /* g_lock */
static int32_t g_virtual_x, g_virtual_y; /* headless cursor position (g_lock) */

static NSCursor *system_cursor(uint32_t idc)
{
    switch (idc)
    {
    case 32512: return NSCursor.arrowCursor;
    case 32513: return NSCursor.IBeamCursor;
    case 32515: return NSCursor.crosshairCursor;
    case 32642:
        if (@available(macOS 15.0, *))
            return [NSCursor frameResizeCursorFromPosition:NSCursorFrameResizePositionTopLeft
                                              inDirections:NSCursorFrameResizeDirectionsAll];
        return nil;
    case 32643:
        if (@available(macOS 15.0, *))
            return [NSCursor frameResizeCursorFromPosition:NSCursorFrameResizePositionTopRight
                                              inDirections:NSCursorFrameResizeDirectionsAll];
        return nil;
    case 32644:
        if (@available(macOS 15.0, *))
            return [NSCursor frameResizeCursorFromPosition:NSCursorFrameResizePositionRight
                                              inDirections:NSCursorFrameResizeDirectionsAll];
        return NSCursor.resizeLeftRightCursor;
    case 32645:
        if (@available(macOS 15.0, *))
            return [NSCursor frameResizeCursorFromPosition:NSCursorFrameResizePositionTop
                                              inDirections:NSCursorFrameResizeDirectionsAll];
        return NSCursor.resizeUpDownCursor;
    case 32648: return NSCursor.operationNotAllowedCursor;
    case 32649: return NSCursor.pointingHandCursor;
    default: return nil;
    }
}

static bool system_cursor_exists(uint32_t idc)
{
    switch (idc)
    {
    case 32512: case 32513: case 32515: case 32644: case 32645: case 32648: case 32649: return true;
    case 32642: case 32643:
        if (@available(macOS 15.0, *)) return true;
        return false;
    default: return false;
    }
}

/* main thread */
static NSCursor *ns_cursor(RTCursor *c)
{
    if (c->cursor != nil) return c->cursor;
    if (c->idc != 0)
        c->cursor = system_cursor(c->idc);
    else
    {
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
                                                                        pixelsWide:(NSInteger)c->w
                                                                        pixelsHigh:(NSInteger)c->h
                                                                     bitsPerSample:8
                                                                   samplesPerPixel:4
                                                                          hasAlpha:YES
                                                                          isPlanar:NO
                                                                    colorSpaceName:NSDeviceRGBColorSpace
                                                                      bitmapFormat:NSBitmapFormatAlphaNonpremultiplied
                                                                       bytesPerRow:(NSInteger)(c->w * 4u)
                                                                      bitsPerPixel:32];
        NSImage *img;
        if (rep == nil || rep.bitmapData == NULL) rt_fatal("Cocoa: a cursor image could not be made");
        memcpy(rep.bitmapData, c->rgba.bytes, (size_t)c->w * c->h * 4u);
        img = [[NSImage alloc] initWithSize:NSMakeSize((CGFloat)c->w, (CGFloat)c->h)];
        [img addRepresentation:rep];
        c->cursor = [[NSCursor alloc] initWithImage:img hotSpot:NSMakePoint((CGFloat)c->hot_x, (CGFloat)c->hot_y)];
    }
    if (c->cursor == nil) rt_fatal("Cocoa: a cursor could not be made");
    return c->cursor;
}

/* main thread: show the cursor that USER32 asked for. */
static void apply_cursor(bool force)
{
    RTCursor *c;
    bool visible, dirty;
    glock();
    c = g_want_cursor;
    visible = g_want_visible;
    dirty = g_cursor_dirty;
    g_cursor_dirty = false;
    gunlock();
    if (!dirty && !force) return;
    if (g_headless) return;
    if (c != nil && visible)
    {
        if (g_cursor_hidden)
        {
            [NSCursor unhide];
            g_cursor_hidden = false;
        }
        [ns_cursor(c) set];
    }
    else if (!g_cursor_hidden)
    {
        [NSCursor hide];
        g_cursor_hidden = true;
    }
}

void *rt_cocoa_cursor_create(const uint8_t *rgba, uint32_t w, uint32_t h, int32_t hot_x, int32_t hot_y)
{
    RTCursor *c;
    if (w == 0 || h == 0 || w > 1024 || h > 1024) rt_fatal("Cocoa: a cursor of %ux%u is not valid", w, h);
    c = [RTCursor new];
    c->rgba = [NSData dataWithBytes:rgba length:(NSUInteger)w * h * 4u];
    c->w = w;
    c->h = h;
    c->hot_x = hot_x < 0 ? 0 : hot_x >= (int32_t)w ? (int32_t)w - 1 : hot_x;
    c->hot_y = hot_y < 0 ? 0 : hot_y >= (int32_t)h ? (int32_t)h - 1 : hot_y;
    return (__bridge_retained void *)c;
}

void *rt_cocoa_cursor_system(uint32_t idc)
{
    RTCursor *c;
    if (!system_cursor_exists(idc)) return NULL;
    c = [RTCursor new];
    c->idc = idc;
    return (__bridge_retained void *)c;
}

void rt_cocoa_cursor_release(void *c)
{
    RTCursor *obj;
    if (c == NULL) return;
    obj = (__bridge_transfer RTCursor *)c;
    if (pthread_main_np()) return; /* obj is released here */
    /* The NSCursor is released on the main thread. */
    dispatch_async(dispatch_get_main_queue(), ^{
        (void)obj;
    });
}

void rt_cocoa_cursor_set(void *c, bool visible)
{
    glock();
    g_want_cursor = (__bridge RTCursor *)c;
    g_want_visible = visible;
    g_cursor_dirty = true;
    gunlock();
    if (pthread_main_np())
        apply_cursor(false);
    else
        rt_cocoa_wake();
}

void rt_cocoa_cursor_pos(int32_t *x, int32_t *y)
{
    CGEventRef e;
    CGPoint p;
    if (g_headless)
    {
        glock();
        *x = g_virtual_x;
        *y = g_virtual_y;
        gunlock();
        return;
    }
    e = CGEventCreate(NULL);
    if (e == NULL) rt_fatal("Cocoa: the cursor position is not available (CGEventCreate failed)");
    p = CGEventGetLocation(e);
    CFRelease(e);
    *x = (int32_t)floor(p.x * g_px);
    *y = (int32_t)floor(p.y * g_px);
}

static void clamp_to_clip(int32_t *x, int32_t *y)
{
    if (*x < g_clip.left) *x = g_clip.left;
    if (*x >= g_clip.right) *x = g_clip.right > g_clip.left ? g_clip.right - 1 : g_clip.left;
    if (*y < g_clip.top) *y = g_clip.top;
    if (*y >= g_clip.bottom) *y = g_clip.bottom > g_clip.top ? g_clip.bottom - 1 : g_clip.top;
}

void rt_cocoa_cursor_move(int32_t x, int32_t y)
{
    if (g_headless)
    {
        glock();
        g_virtual_x = x;
        g_virtual_y = y;
        if (g_clip_set) clamp_to_clip(&g_virtual_x, &g_virtual_y);
        gunlock();
        return;
    }
    /* The center of the Win32 pixel, so that rt_cocoa_cursor_pos gives the
       same pixel back. */
    if (CGWarpMouseCursorPosition(CGPointMake(((CGFloat)x + 0.5) / g_px, ((CGFloat)y + 0.5) / g_px)) != kCGErrorSuccess)
        rt_fatal("Cocoa: the cursor could not be moved (CGWarpMouseCursorPosition failed)");
    if (CGAssociateMouseAndMouseCursorPosition(true) != kCGErrorSuccess)
        rt_fatal("Cocoa: CGAssociateMouseAndMouseCursorPosition failed");
}

/* Keep the cursor in the clip rectangle (main thread, from the event
   monitors; and from rt_cocoa_cursor_clip). */
static void enforce_clip(void)
{
    int32_t x, y, cx, cy;
    bool set;
    if (g_headless) return;
    if (!NSApp.isActive) return;
    glock();
    set = g_clip_set;
    gunlock();
    if (!set) return;
    rt_cocoa_cursor_pos(&x, &y);
    cx = x;
    cy = y;
    glock();
    clamp_to_clip(&cx, &cy);
    gunlock();
    if (cx != x || cy != y) rt_cocoa_cursor_move(cx, cy);
}

void rt_cocoa_cursor_clip(const rt_rect *rect)
{
    glock();
    g_clip_set = rect != NULL;
    if (rect != NULL) g_clip = *rect;
    if (g_headless && rect != NULL) clamp_to_clip(&g_virtual_x, &g_virtual_y);
    gunlock();
    if (pthread_main_np())
        enforce_clip();
    else
        dispatch_async(dispatch_get_main_queue(), ^{
            enforce_clip();
        });
}

/* ---------------------------------------------------------------- displays */

typedef struct disp_info {
    uint32_t id;
    CGFloat inset_l, inset_t, inset_r, inset_b; /* menu bar and Dock */
    uint32_t refresh;
    double min_interval; /* NSScreen.minimumRefreshInterval (seconds) */
    char name[128];
} disp_info;

static disp_info g_disp[RT_MAX_DISPLAYS]; /* g_lock */
static int g_ndisp;                       /* g_lock */

/* main thread */
static void refresh_display_cache(void)
{
    disp_info d[RT_MAX_DISPLAYS];
    int n = 0;
    @autoreleasepool
    {
        for (NSScreen *s in NSScreen.screens)
        {
            NSNumber *num = s.deviceDescription[@"NSScreenNumber"];
            NSRect f = s.frame, v = s.visibleFrame;
            const char *name;
            if (num == nil || n == RT_MAX_DISPLAYS) continue;
            memset(&d[n], 0, sizeof d[n]);
            d[n].id = num.unsignedIntValue;
            d[n].inset_l = v.origin.x - f.origin.x;
            d[n].inset_r = NSMaxX(f) - NSMaxX(v);
            d[n].inset_t = NSMaxY(f) - NSMaxY(v);
            d[n].inset_b = v.origin.y - f.origin.y;
            d[n].refresh = s.maximumFramesPerSecond > 0 ? (uint32_t)s.maximumFramesPerSecond : 0;
            d[n].min_interval = s.minimumRefreshInterval > 0.0 ? s.minimumRefreshInterval : 0.0;
            name = s.localizedName.UTF8String;
            snprintf(d[n].name, sizeof d[n].name, "%s", name != NULL ? name : "");
            n++;
        }
    }
    glock();
    memcpy(g_disp, d, sizeof d);
    g_ndisp = n;
    gunlock();
}

double rt_cocoa_refresh_interval_at(int32_t x, int32_t y)
{
    CGDirectDisplayID id;
    uint32_t count = 0;
    double r = 0.0;
    if (CGGetDisplaysWithPoint(CGPointMake(pt(x), pt(y)), 1, &id, &count) != kCGErrorSuccess || count == 0) return 0.0;
    glock();
    for (int i = 0; i < g_ndisp; i++)
        if (g_disp[i].id == id) r = g_disp[i].min_interval;
    gunlock();
    return r;
}

int rt_cocoa_displays(rt_display out[RT_MAX_DISPLAYS])
{
    disp_info d[RT_MAX_DISPLAYS];
    uint32_t main_id = CGMainDisplayID();
    int n, i, k = 0;
    if (pthread_main_np()) refresh_display_cache();
    glock();
    memcpy(d, g_disp, sizeof d);
    n = g_ndisp;
    gunlock();
    /* The main display first. */
    for (int pass = 0; pass < 2; pass++)
    {
        for (i = 0; i < n; i++)
        {
            CGRect b;
            rt_display *o;
            if ((d[i].id == main_id) != (pass == 0)) continue;
            if (!CGDisplayIsActive(d[i].id)) continue;
            b = CGDisplayBounds(d[i].id);
            o = &out[k++];
            memset(o, 0, sizeof *o);
            o->id = d[i].id;
            o->frame.left = px(b.origin.x);
            o->frame.top = px(b.origin.y);
            o->frame.right = o->frame.left + px(b.size.width);
            o->frame.bottom = o->frame.top + px(b.size.height);
            o->work.left = o->frame.left + px(d[i].inset_l);
            o->work.top = o->frame.top + px(d[i].inset_t);
            o->work.right = o->frame.right - px(d[i].inset_r);
            o->work.bottom = o->frame.bottom - px(d[i].inset_b);
            o->main = d[i].id == main_id;
            o->refresh = d[i].refresh;
            memcpy(o->name, d[i].name, sizeof o->name);
        }
    }
    if (k == 0) rt_fatal("Cocoa: no display is available");
    return k;
}

static uint32_t display_refresh(uint32_t id)
{
    uint32_t r = 0;
    int i;
    glock();
    for (i = 0; i < g_ndisp; i++)
        if (g_disp[i].id == id) r = g_disp[i].refresh;
    gunlock();
    return r;
}

static uint32_t mode_refresh(CGDisplayModeRef m, uint32_t screen_refresh)
{
    double r = CGDisplayModeGetRefreshRate(m);
    /* A mode without a refresh rate (a built-in panel) runs at the rate
       of the screen. */
    if (r <= 0) return screen_refresh;
    return (uint32_t)lround(r);
}

static int mode_cmp(const void *a, const void *b)
{
    const rt_display_mode *x = a, *y = b;
    if (x->width != y->width) return x->width < y->width ? -1 : 1;
    if (x->height != y->height) return x->height < y->height ? -1 : 1;
    if (x->refresh != y->refresh) return x->refresh < y->refresh ? -1 : 1;
    return 0;
}

int rt_cocoa_display_modes(uint32_t id, rt_display_mode *out, int max, int *current)
{
    CFArrayRef all = CGDisplayCopyAllDisplayModes(id, NULL);
    CGDisplayModeRef cur = CGDisplayCopyDisplayMode(id);
    uint32_t sr = display_refresh(id);
    rt_display_mode cm = { 0, 0, 0 };
    CFIndex i, n;
    int k = 0, j;
    *current = -1;
    if (all == NULL || cur == NULL) rt_fatal("Cocoa: the modes of display %u are not available", id);
    cm.width = (uint32_t)CGDisplayModeGetWidth(cur) * (uint32_t)g_px;
    cm.height = (uint32_t)CGDisplayModeGetHeight(cur) * (uint32_t)g_px;
    cm.refresh = mode_refresh(cur, sr);
    n = CFArrayGetCount(all);
    for (i = 0; i < n && k < max; i++)
    {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(all, i);
        rt_display_mode v;
        bool dup = false;
        if (!CGDisplayModeIsUsableForDesktopGUI(m)) continue;
        v.width = (uint32_t)CGDisplayModeGetWidth(m) * (uint32_t)g_px;
        v.height = (uint32_t)CGDisplayModeGetHeight(m) * (uint32_t)g_px;
        v.refresh = mode_refresh(m, sr);
        for (j = 0; j < k; j++)
            if (mode_cmp(&out[j], &v) == 0) dup = true;
        if (!dup) out[k++] = v;
    }
    /* The current mode is in the list. */
    {
        bool found = false;
        for (j = 0; j < k; j++)
            if (mode_cmp(&out[j], &cm) == 0) found = true;
        if (!found && k < max) out[k++] = cm;
    }
    qsort(out, (size_t)k, sizeof *out, mode_cmp);
    for (j = 0; j < k; j++)
        if (mode_cmp(&out[j], &cm) == 0) *current = j;
    CFRelease(all);
    CGDisplayModeRelease(cur);
    return k;
}

static atomic_bool g_modes_changed;

bool rt_cocoa_set_display_mode(uint32_t id, const rt_display_mode *m, bool test)
{
    CFArrayRef all = CGDisplayCopyAllDisplayModes(id, NULL);
    CGDisplayModeRef cur = CGDisplayCopyDisplayMode(id), best = NULL;
    uint32_t sr = display_refresh(id), cur_refresh;
    double cur_scale;
    CFIndex i, n;
    bool ok;
    if (all == NULL || cur == NULL) rt_fatal("Cocoa: the modes of display %u are not available", id);
    cur_refresh = mode_refresh(cur, sr);
    cur_scale = (double)CGDisplayModeGetPixelWidth(cur) / (double)CGDisplayModeGetWidth(cur);
    n = CFArrayGetCount(all);
    for (i = 0; i < n; i++)
    {
        CGDisplayModeRef c = (CGDisplayModeRef)CFArrayGetValueAtIndex(all, i);
        uint32_t r = mode_refresh(c, sr);
        if (!CGDisplayModeIsUsableForDesktopGUI(c)) continue;
        if (CGDisplayModeGetWidth(c) * (size_t)g_px != m->width || CGDisplayModeGetHeight(c) * (size_t)g_px != m->height) continue;
        if (m->refresh != 0 && r != m->refresh) continue;
        if (best == NULL)
        {
            best = c;
            continue;
        }
        {
            /* Prefer the backing scale of the current mode, then the
               refresh rate of the current mode, then the highest rate. */
            double s = (double)CGDisplayModeGetPixelWidth(c) / (double)CGDisplayModeGetWidth(c);
            double bs = (double)CGDisplayModeGetPixelWidth(best) / (double)CGDisplayModeGetWidth(best);
            uint32_t br = mode_refresh(best, sr);
            if ((s == cur_scale) != (bs == cur_scale))
            {
                if (s == cur_scale) best = c;
                continue;
            }
            if ((r == cur_refresh) != (br == cur_refresh))
            {
                if (r == cur_refresh) best = c;
                continue;
            }
            if (r > br) best = c;
        }
    }
    if (best == NULL)
    {
        CFRelease(all);
        CGDisplayModeRelease(cur);
        return false;
    }
    if (test)
    {
        CFRelease(all);
        CGDisplayModeRelease(cur);
        return true;
    }
    if (g_headless) rt_fatal("Cocoa: a display mode change in headless mode is not supported (it changes the Mac display)");
    if (CGDisplayModeGetWidth(best) == CGDisplayModeGetWidth(cur) && CGDisplayModeGetHeight(best) == CGDisplayModeGetHeight(cur) &&
        CGDisplayModeGetPixelWidth(best) == CGDisplayModeGetPixelWidth(cur) && mode_refresh(best, sr) == cur_refresh)
        ok = true; /* the display has this mode now */
    else
    {
        /* CGDisplaySetDisplayMode changes the mode for the life of the
           process (kCGConfigureForAppOnly). */
        ok = CGDisplaySetDisplayMode(id, best, NULL) == kCGErrorSuccess;
        if (ok) atomic_store(&g_modes_changed, true);
    }
    CFRelease(all);
    CGDisplayModeRelease(cur);
    if (ok && pthread_main_np()) refresh_display_cache();
    return ok;
}

void rt_cocoa_restore_display_modes(void)
{
    if (atomic_exchange(&g_modes_changed, false)) CGRestorePermanentDisplayConfiguration();
    if (pthread_main_np()) refresh_display_cache();
}

int32_t rt_cocoa_title_height(void)
{
    return atomic_load(&g_title_h) * g_px;
}

int rt_cocoa_pixel_scale(void)
{
    return g_px;
}

bool rt_cocoa_headless(void)
{
    return g_headless;
}

/* ---------------------------------------------------------------- views */

@class RTWindow;

@interface RTView : NSView <NSTextInputClient>
@property (nonatomic) uint32_t hwnd;
@property (nonatomic) BOOL metal;
@property (nonatomic) BOOL rtEnabled;
@property (nonatomic) BOOL imeEnabled;
@end

@interface RTWindow : NSWindow <NSWindowDelegate>
@property (nonatomic) uint32_t hwnd;
@property (nonatomic) BOOL rtEnabled;
@property (nonatomic) int suppress;
@property (nonatomic) unsigned kind;
/* Native full screen (see "Full screen"): fsMoving while AppKit moves the
   window into or out of its full-screen space. A frame, kind or hide that
   the program sets while the window is in full screen waits in fsPending*
   until the window is out of full screen. */
@property (nonatomic) BOOL fsMoving;
@property (nonatomic) BOOL fsPendingFrameSet;
@property (nonatomic) NSRect fsPendingFrame;
@property (nonatomic) BOOL fsPendingKindSet;
@property (nonatomic) unsigned fsPendingKind;
@property (nonatomic) BOOL fsPendingHide;
@end

/* Wheel motion that is not yet a whole wheel unit (main thread). */
static double g_wheel_v, g_wheel_h;
static __weak RTView *g_down_view;
static NSTimeInterval g_last_move_time;
static NSPoint g_last_move_loc;

static void utf16_of(NSString *s, uint16_t *buf, int max, int *n)
{
    NSUInteger len = s.length;
    if (len > (NSUInteger)max) len = (NSUInteger)max;
    [s getCharacters:(unichar *)buf range:NSMakeRange(0, len)];
    *n = (int)len;
}

@implementation RTView
{
    NSMutableAttributedString *_marked;
    NSRange _markedSel;
    BOOL _inKey;
    BOOL _keyIme;
    NSString *_keyChars;
    NSTrackingArea *_track;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (self != nil)
    {
        _rtEnabled = YES;
        _marked = [[NSMutableAttributedString alloc] init];
        _markedSel = NSMakeRange(0, 0);
    }
    return self;
}

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { (void)event; return YES; }

- (CALayer *)makeBackingLayer
{
    if (self.metal)
    {
        CAMetalLayer *l = [CAMetalLayer layer];
        l.contentsScale = self.window != nil ? self.window.backingScaleFactor : NSScreen.mainScreen.backingScaleFactor;
        return l;
    }
    return [super makeBackingLayer];
}

- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    if (self.metal && self.window != nil) self.layer.contentsScale = self.window.backingScaleFactor;
}

- (void)updateTrackingAreas
{
    if (_track != nil) [self removeTrackingArea:_track];
    _track = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                          options:NSTrackingMouseMoved | NSTrackingCursorUpdate |
                                                  NSTrackingActiveAlways | NSTrackingInVisibleRect
                                            owner:self
                                         userInfo:nil];
    [self addTrackingArea:_track];
    [super updateTrackingAreas];
}

- (void)cursorUpdate:(NSEvent *)event
{
    (void)event;
    apply_cursor(true);
}

/* Mouse events are handled in -[RTWindow sendEvent:]. */
- (void)mouseDown:(NSEvent *)event { (void)event; }
- (void)rightMouseDown:(NSEvent *)event { (void)event; }
- (void)otherMouseDown:(NSEvent *)event { (void)event; }
- (void)mouseMoved:(NSEvent *)event { (void)event; }
- (void)scrollWheel:(NSEvent *)event { (void)event; }

/* The characters of a key-down without the input method. Command keys
   have no characters. With Option (Alt) and no Control, the characters
   are those of the key without Option (Alt+key gives WM_SYSCHAR of the
   key, as on Windows). */
static NSString *plain_chars(NSEvent *e)
{
    NSEventModifierFlags f = e.modifierFlags;
    if (f & NSEventModifierFlagCommand) return @"";
    if ((f & NSEventModifierFlagOption) && !(f & NSEventModifierFlagControl))
    {
        NSString *s = [e charactersByApplyingModifiers:f & ~NSEventModifierFlagOption];
        return s != nil ? s : @"";
    }
    return e.characters != nil ? e.characters : @"";
}

/* The Win32 characters of the AppKit characters of a key. */
static void win_chars(NSString *s, uint16_t code, uint16_t *out, int *n)
{
    uint16_t buf[16];
    int len, i, k = 0;
    utf16_of(s, buf, 16, &len);
    for (i = 0; i < len; i++)
    {
        uint16_t c = buf[i];
        if (c >= 0xF700 && c <= 0xF8FF) continue; /* function keys (NSF1FunctionKey and others) */
        if (c == 0x7F && code == MAC_KEY_BACKSPACE) c = 0x08; /* Backspace: VK_BACK gives 0x08 */
        if (c == 0x03) c = 0x0D;                              /* keypad Enter */
        if (c == 0x19) c = 0x09;                              /* Shift+Tab (back tab) */
        out[k++] = c;
    }
    *n = k;
}

- (void)keyDown:(NSEvent *)event
{
    uint16_t chars[16];
    int n = 0;
    if (!self.metal || !self.rtEnabled) return; /* dialog panes: AppKit controls handle the keys */
    /* F12 (kVK_F12): a screenshot to the screenshot folder of macOS. The
       game also gets the key (it has no binding for F12 by default, and a
       binding that the player made continues to work). */
    if (event.keyCode == 0x6F && !event.isARepeat) rt_d3d9_request_screenshot();
    if (self.imeEnabled)
    {
        BOOL had = self.hasMarkedText;
        _keyIme = NO;
        _keyChars = nil;
        _inKey = YES;
        [self interpretKeyEvents:@[ event ]];
        _inKey = NO;
        if (had || _keyIme || self.hasMarkedText)
        {
            rt_user32_on_key(self.hwnd, true, event.isARepeat, event.keyCode, NULL, 0, true);
            return;
        }
        win_chars(_keyChars != nil ? _keyChars : plain_chars(event), event.keyCode, chars, &n);
        _keyChars = nil;
    }
    else
        win_chars(plain_chars(event), event.keyCode, chars, &n);
    rt_user32_on_key(self.hwnd, true, event.isARepeat, event.keyCode, chars, n, false);
}

- (void)keyUp:(NSEvent *)event
{
    if (!self.metal || !self.rtEnabled) return;
    rt_user32_on_key(self.hwnd, false, false, event.keyCode, NULL, 0, false);
}

- (void)flagsChanged:(NSEvent *)event
{
    if (!self.metal || !self.rtEnabled) return;
    rt_user32_on_modifiers(self.hwnd, (uint32_t)event.modifierFlags);
}

/* NSTextInputClient */

- (BOOL)hasMarkedText { return _marked.length > 0; }
- (NSRange)markedRange { return _marked.length > 0 ? NSMakeRange(0, _marked.length) : NSMakeRange(NSNotFound, 0); }
- (NSRange)selectedRange { return _marked.length > 0 ? _markedSel : NSMakeRange(0, 0); }
- (NSArray<NSAttributedStringKey> *)validAttributesForMarkedText { return @[]; }

- (void)setMarkedText:(id)string selectedRange:(NSRange)sel replacementRange:(NSRange)rep
{
    NSString *s = [string isKindOfClass:[NSAttributedString class]] ? [(NSAttributedString *)string string] : (NSString *)string;
    uint16_t *buf;
    NSUInteger len;
    (void)rep;
    if (!self.imeEnabled) return;
    _keyIme = YES;
    [_marked setAttributedString:[[NSAttributedString alloc] initWithString:s]];
    _markedSel = sel;
    len = s.length;
    if (len > 4096) len = 4096;
    buf = malloc((len + 1u) * sizeof(uint16_t));
    if (buf == NULL) rt_fatal("out of host memory (composition)");
    [s getCharacters:(unichar *)buf range:NSMakeRange(0, len)];
    rt_user32_on_ime_composition(self.hwnd, buf, (int)len, sel.location <= len ? (int)sel.location : (int)len);
    free(buf);
}

- (void)unmarkText
{
    /* The input method commits the marked text. */
    if (_marked.length > 0) [self insertText:[_marked.string copy] replacementRange:NSMakeRange(NSNotFound, 0)];
}

- (void)insertText:(id)string replacementRange:(NSRange)rep
{
    NSString *s = [string isKindOfClass:[NSAttributedString class]] ? [(NSAttributedString *)string string] : (NSString *)string;
    (void)rep;
    if (_marked.length > 0 || (!_inKey && self.imeEnabled))
    {
        /* The result of a composition (or text from the input method
           outside a key event, for example the character viewer). */
        NSUInteger len = s.length;
        uint16_t *buf;
        if (len > 4096) len = 4096;
        buf = malloc((len + 1u) * sizeof(uint16_t));
        if (buf == NULL) rt_fatal("out of host memory (composition)");
        [s getCharacters:(unichar *)buf range:NSMakeRange(0, len)];
        [_marked setAttributedString:[[NSAttributedString alloc] init]];
        _keyIme = YES;
        rt_user32_on_ime_result(self.hwnd, buf, (int)len);
        free(buf);
        return;
    }
    _keyChars = [s copy];
}

- (void)doCommandBySelector:(SEL)sel
{
    (void)sel; /* a key without text (arrows, Return): the key itself */
}

- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actual
{
    (void)range;
    (void)actual;
    return nil;
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point
{
    (void)point;
    return NSNotFound;
}

- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actual
{
    /* The candidate window goes below the bottom left corner of the view. */
    NSRect r = [self convertRect:NSMakeRect(0, self.bounds.size.height, 1, 1) toView:nil];
    (void)range;
    (void)actual;
    return self.window != nil ? [self.window convertRectToScreen:r] : NSZeroRect;
}

- (void)imeCancel
{
    [self.inputContext discardMarkedText];
    [_marked setAttributedString:[[NSAttributedString alloc] init]];
}

@end

/* ---------------------------------------------------------------- windows */

static void update_presentation(RTWindow *w);
static void leave_full_screen(RTWindow *w);
static NSWindowStyleMask style_of(unsigned kind);
static void apply_kind_extras(RTWindow *w, unsigned kind);
static int g_fs_windows;

@implementation RTWindow

- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }

/* Win32 windows can be at any position, also below the menu bar. */
- (NSRect)constrainFrameRect:(NSRect)frame toScreen:(NSScreen *)screen
{
    (void)screen;
    return frame;
}

- (BOOL)windowShouldClose:(NSWindow *)sender
{
    (void)sender;
    rt_user32_on_close(self.hwnd);
    return NO;
}

- (void)windowDidBecomeKey:(NSNotification *)n
{
    (void)n;
    rt_user32_on_active(self.hwnd, true);
    update_presentation(self);
}

- (void)windowDidResignKey:(NSNotification *)n
{
    (void)n;
    rt_user32_on_active(self.hwnd, false);
    update_presentation(self);
}

- (void)windowDidMove:(NSNotification *)n
{
    (void)n;
    if (self.suppress == 0) rt_user32_on_frame(self.hwnd, false);
}

- (void)windowDidResize:(NSNotification *)n
{
    (void)n;
    if (self.suppress == 0) rt_user32_on_frame(self.hwnd, true);
}

- (void)windowDidMiniaturize:(NSNotification *)n
{
    (void)n;
    rt_user32_on_minimized(self.hwnd, true);
}

- (void)windowDidDeminiaturize:(NSNotification *)n
{
    (void)n;
    rt_user32_on_minimized(self.hwnd, false);
}

- (NSApplicationPresentationOptions)window:(NSWindow *)window
      willUseFullScreenPresentationOptions:(NSApplicationPresentationOptions)proposed
{
    (void)window;
    (void)proposed;
    return NSApplicationPresentationFullScreen | NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar;
}

- (void)windowDidEnterFullScreen:(NSNotification *)n
{
    (void)n;
    self.fsMoving = NO;
    if (self.fsPendingFrameSet || self.fsPendingKindSet || self.fsPendingHide) leave_full_screen(self);
}

- (void)windowDidFailToEnterFullScreen:(NSWindow *)window
{
    (void)window;
    rt_fatal("Cocoa: the game window could not move into a full-screen space of macOS");
}

- (void)windowDidExitFullScreen:(NSNotification *)n
{
    (void)n;
    self.fsMoving = NO;
    self.collectionBehavior = NSWindowCollectionBehaviorFullScreenNone;
    g_fs_windows--;
    self.suppress++;
    if (self.fsPendingKindSet)
    {
        NSRect content = [self contentRectForFrameRect:self.frame];
        self.styleMask = style_of(self.fsPendingKind);
        [self setFrame:[self frameRectForContentRect:content] display:YES];
        apply_kind_extras(self, self.fsPendingKind);
        self.fsPendingKindSet = NO;
    }
    if (self.fsPendingFrameSet)
    {
        [self setFrame:self.fsPendingFrame display:YES];
        self.fsPendingFrameSet = NO;
    }
    self.suppress--;
    if (self.fsPendingHide)
    {
        self.fsPendingHide = NO;
        [self orderOut:nil];
    }
    update_presentation(self);
}

- (void)windowDidFailToExitFullScreen:(NSWindow *)window
{
    (void)window;
    rt_fatal("Cocoa: the game window could not leave its full-screen space");
}

- (void)windowDidChangeBackingProperties:(NSNotification *)n
{
    (void)n;
    if ([self.contentView isKindOfClass:[RTView class]] && ((RTView *)self.contentView).metal)
        self.contentView.layer.contentsScale = self.backingScaleFactor;
}

/* The RTView under the event location (NULL if the location is on a
   control or outside the content view). */
- (RTView *)rtViewAt:(NSEvent *)e
{
    NSView *cv = self.contentView;
    NSView *hit;
    if (cv == nil || cv.superview == nil) return nil;
    hit = [cv hitTest:[cv.superview convertPoint:e.locationInWindow fromView:nil]];
    return [hit isKindOfClass:[RTView class]] ? (RTView *)hit : nil;
}

static void report_mouse(RTView *v, NSEvent *e, uint32_t msg, int32_t data)
{
    NSPoint p;
    if (v == nil || !v.rtEnabled) return;
    p = [v convertPoint:e.locationInWindow fromView:nil];
    rt_user32_on_mouse(v.hwnd, msg, (int32_t)floor(p.x * g_px), (int32_t)floor(p.y * g_px), data);
}

static uint32_t button_msg(NSEvent *e, int phase, int32_t *data)
{
    /* phase: 0 down, 1 up */
    NSInteger b = e.buttonNumber;
    bool dbl = phase == 0 && e.clickCount >= 2 && e.clickCount % 2 == 0;
    *data = 0;
    switch (b)
    {
    case 0: return phase == 0 ? (dbl ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN) : WM_LBUTTONUP;
    case 1: return phase == 0 ? (dbl ? WM_RBUTTONDBLCLK : WM_RBUTTONDOWN) : WM_RBUTTONUP;
    case 2: return phase == 0 ? (dbl ? WM_MBUTTONDBLCLK : WM_MBUTTONDOWN) : WM_MBUTTONUP;
    case 3: case 4:
        *data = b == 3 ? 1 : 2;
        return phase == 0 ? (dbl ? WM_XBUTTONDBLCLK : WM_XBUTTONDOWN) : WM_XBUTTONUP;
    default: return 0;
    }
}

static void report_wheel(RTView *v, NSEvent *e)
{
    double dy, dx;
    if (e.hasPreciseScrollingDeltas)
    {
        /* Points: one wheel notch (120) for each 10 points of scrolling
           (a design value of the runtime). */
        dy = e.scrollingDeltaY / 10.0;
        dx = e.scrollingDeltaX / 10.0;
    }
    else
    {
        dy = e.scrollingDeltaY;
        dx = e.scrollingDeltaX;
    }
    g_wheel_v += dy * 120.0;
    g_wheel_h -= dx * 120.0; /* WM_MOUSEHWHEEL: positive is to the right */
    while (fabs(g_wheel_v) >= 1.0)
    {
        double part = g_wheel_v > 32767.0 ? 32767.0 : g_wheel_v < -32768.0 ? -32768.0 : trunc(g_wheel_v);
        g_wheel_v -= part;
        report_mouse(v, e, WM_MOUSEWHEEL, (int32_t)part);
    }
    while (fabs(g_wheel_h) >= 1.0)
    {
        double part = g_wheel_h > 32767.0 ? 32767.0 : g_wheel_h < -32768.0 ? -32768.0 : trunc(g_wheel_h);
        g_wheel_h -= part;
        report_mouse(v, e, WM_MOUSEHWHEEL, (int32_t)part);
    }
}

- (void)sendEvent:(NSEvent *)e
{
    NSEventType t = e.type;
    bool mouse = false, key = false;
    switch (t)
    {
    case NSEventTypeLeftMouseDown: case NSEventTypeRightMouseDown: case NSEventTypeOtherMouseDown:
    case NSEventTypeLeftMouseUp: case NSEventTypeRightMouseUp: case NSEventTypeOtherMouseUp:
    case NSEventTypeLeftMouseDragged: case NSEventTypeRightMouseDragged: case NSEventTypeOtherMouseDragged:
    case NSEventTypeMouseMoved: case NSEventTypeScrollWheel:
        mouse = true;
        break;
    case NSEventTypeKeyDown: case NSEventTypeKeyUp: case NSEventTypeFlagsChanged:
        key = true;
        break;
    default: break;
    }
    if ((mouse || key) && !self.rtEnabled) return; /* a disabled window gets no input */
    if (mouse)
    {
        RTView *v = [self rtViewAt:e];
        int32_t data;
        uint32_t msg;
        switch (t)
        {
        case NSEventTypeLeftMouseDown: case NSEventTypeRightMouseDown: case NSEventTypeOtherMouseDown:
            msg = button_msg(e, 0, &data);
            g_down_view = v;
            if (msg != 0) report_mouse(v, e, msg, data);
            break;
        case NSEventTypeLeftMouseUp: case NSEventTypeRightMouseUp: case NSEventTypeOtherMouseUp:
            msg = button_msg(e, 1, &data);
            if (msg != 0) report_mouse(v != nil ? v : g_down_view, e, msg, data);
            break;
        case NSEventTypeLeftMouseDragged: case NSEventTypeRightMouseDragged: case NSEventTypeOtherMouseDragged:
            report_mouse(v != nil ? v : g_down_view, e, WM_MOUSEMOVE, 0);
            break;
        case NSEventTypeMouseMoved:
            /* One event for each tracking area: report one of them. */
            if (e.timestamp == g_last_move_time && NSEqualPoints(e.locationInWindow, g_last_move_loc)) break;
            g_last_move_time = e.timestamp;
            g_last_move_loc = e.locationInWindow;
            report_mouse(v, e, WM_MOUSEMOVE, 0);
            break;
        case NSEventTypeScrollWheel:
            if (v != nil) report_wheel(v, e);
            break;
        default: break;
        }
    }
    if (t == NSEventTypeKeyDown && e.keyCode == MAC_KEY_ESCAPE && [self.contentView isKindOfClass:[RTView class]] &&
        !((RTView *)self.contentView).metal)
    {
        /* Escape in a dialog window: IDCANCEL. */
        rt_user32_on_control(self.hwnd, RT_CTL_CANCEL, 0, 0);
        return;
    }
    [super sendEvent:e];
}

@end

static NSWindowStyleMask style_of(unsigned kind)
{
    NSWindowStyleMask m = 0;
    if (kind & RT_WIN_TITLED)
    {
        m |= NSWindowStyleMaskTitled;
        if (kind & RT_WIN_CLOSABLE) m |= NSWindowStyleMaskClosable;
        if (kind & RT_WIN_MINIMIZABLE) m |= NSWindowStyleMaskMiniaturizable;
        if (kind & RT_WIN_RESIZABLE) m |= NSWindowStyleMaskResizable;
    }
    else
        m = NSWindowStyleMaskBorderless;
    return m;
}

static void apply_kind_extras(RTWindow *w, unsigned kind)
{
    NSButton *zoom = [w standardWindowButton:NSWindowZoomButton];
    if (zoom != nil) zoom.enabled = (kind & RT_WIN_ZOOMABLE) != 0;
    w.level = (kind & RT_WIN_TOPMOST) ? NSFloatingWindowLevel : NSNormalWindowLevel;
    w.kind = kind;
}

/* ---- Full screen

   A Win32 full-screen window is a borderless window that covers its
   display. When such a window is the key window, it moves into its own
   full-screen space of macOS (toggleFullScreen:), as a native full-screen
   application: Command-Tab and Mission Control show the other
   applications in their own spaces, and macOS turns on Game Mode for the
   game (LSSupportsGameMode and the game category in the property list of
   the program). In the space, the menu bar and the Dock stay hidden
   (window:willUseFullScreenPresentationOptions:), also when the cursor is
   at the top edge, where the game pans the camera.
   A display with a camera housing (safeAreaInsets.top > 0) puts a
   full-screen window below the housing, so the window would not have the
   Win32 size. On such a display the window stays a borderless window
   that covers the display, and the menu bar and the Dock are hidden with
   presentation options; macOS does not use Game Mode for it.
   The window leaves full screen when the program gives it a frame that
   does not cover the display, a title bar or SW_HIDE; the change is made
   when the window is out of full screen (windowDidExitFullScreen:). A
   window in full screen is not miniaturized when the program minimizes
   it (the game does that when it is no longer the active application):
   the space keeps the last frame, and SC_RESTORE restores the game when
   the application is active again (rt_user32_on_app_active). */
static int g_fs_windows; /* windows in full screen or moving into it (main thread) */

static bool frame_covers(NSRect f, NSRect s)
{
    return NSMinX(f) <= NSMinX(s) && NSMinY(f) <= NSMinY(s) && NSMaxX(f) >= NSMaxX(s) && NSMaxY(f) >= NSMaxY(s);
}

static bool in_full_screen(RTWindow *w)
{
    return w.fsMoving || (w.styleMask & NSWindowStyleMaskFullScreen) != 0;
}

static bool space_capable(NSScreen *s)
{
    return s != nil && s.safeAreaInsets.top <= 0;
}

static void leave_full_screen(RTWindow *w)
{
    if (w.fsMoving || !(w.styleMask & NSWindowStyleMaskFullScreen)) return; /* windowDidEnterFullScreen: leaves */
    w.fsMoving = YES;
    [w toggleFullScreen:nil];
}

/* A key borderless window that covers its display moves into full screen,
   or, on a display with a camera housing, hides the menu bar and the Dock
   (a Win32 full-screen window covers them). */
static void update_presentation(RTWindow *w)
{
    NSApplicationPresentationOptions want = NSApplicationPresentationDefault;
    bool covers;
    if (g_headless) return;
    if (in_full_screen(w)) return; /* the space has its options */
    covers = w.isVisible && !(w.styleMask & NSWindowStyleMaskTitled) && w.screen != nil &&
             frame_covers(w.frame, w.screen.frame);
    if (covers && w.isKeyWindow && space_capable(w.screen))
    {
        w.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        w.fsMoving = YES;
        g_fs_windows++;
        [w toggleFullScreen:nil];
        return;
    }
    if (g_fs_windows > 0) return; /* options cannot change while a space has the full-screen options */
    if (covers && w.isKeyWindow) want = NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar;
    if (!w.isKeyWindow && NSApp.keyWindow != nil && NSApp.keyWindow != w) return; /* the key window decides */
    if (NSApp.presentationOptions != want) NSApp.presentationOptions = want;
}

static RTWindow *as_window(void *w, const char *fn)
{
    id o = (__bridge id)w;
    if (![o isKindOfClass:[RTWindow class]]) rt_fatal("%s: the native object is not a window", fn);
    return (RTWindow *)o;
}

void *rt_cocoa_window_create(uint32_t hwnd, rt_rect client, unsigned kind, const char *title)
{
    RTWindow *w;
    RTView *v;
    need_main("rt_cocoa_window_create");
    w = [[RTWindow alloc] initWithContentRect:to_ns(client)
                                    styleMask:style_of(kind)
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    if (w == nil) rt_fatal("Cocoa: a window could not be made");
    w.hwnd = hwnd;
    w.rtEnabled = YES;
    w.releasedWhenClosed = NO;
    w.restorable = NO;
    w.tabbingMode = NSWindowTabbingModeDisallowed;
    w.delegate = w;
    w.acceptsMouseMovedEvents = YES;
    w.autorecalculatesKeyViewLoop = YES;
    w.title = ns_string(title);
    w.collectionBehavior = NSWindowCollectionBehaviorFullScreenNone;
    v = [[RTView alloc] initWithFrame:NSMakeRect(0, 0, (CGFloat)(client.right - client.left), (CGFloat)(client.bottom - client.top))];
    v.hwnd = hwnd;
    v.metal = (kind & RT_WIN_METAL) != 0;
    v.wantsLayer = YES;
    if (v.metal) w.backgroundColor = NSColor.blackColor;
    set_view_info(v, hwnd);
    w.contentView = v;
    [w makeFirstResponder:v];
    apply_kind_extras(w, kind);
    /* initWithContentRect places the window on the screen with the
       content rectangle; keep the exact Win32 position. */
    w.suppress++;
    [w setFrame:[w frameRectForContentRect:to_ns(client)] display:NO];
    w.suppress--;
    return (__bridge_retained void *)w;
}

void rt_cocoa_window_destroy(void *native)
{
    RTWindow *w;
    need_main("rt_cocoa_window_destroy");
    w = (__bridge_transfer RTWindow *)native;
    if (![w isKindOfClass:[RTWindow class]]) rt_fatal("rt_cocoa_window_destroy: the native object is not a window");
    w.delegate = nil;
    if (in_full_screen(w)) g_fs_windows--; /* closing the window removes its space */
    [w orderOut:nil];
    [w close];
    if (!g_headless && NSApp.keyWindow == nil && g_fs_windows == 0) NSApp.presentationOptions = NSApplicationPresentationDefault;
}

void rt_cocoa_window_show(void *native, bool visible, bool activate)
{
    RTWindow *w = as_window(native, "rt_cocoa_window_show");
    need_main("rt_cocoa_window_show");
    if (!visible)
    {
        if (in_full_screen(w))
        {
            w.fsPendingHide = YES;
            leave_full_screen(w);
            return;
        }
        [w orderOut:nil];
        return;
    }
    if (g_headless) rt_fatal("Cocoa: a window cannot be shown in headless mode");
    w.fsPendingHide = NO;
    if (activate)
    {
        [w makeKeyAndOrderFront:nil];
        [NSApp activate];
    }
    else
        [w orderFront:nil];
    update_presentation(w);
}

void rt_cocoa_window_minimize(void *native, bool minimize)
{
    RTWindow *w = as_window(native, "rt_cocoa_window_minimize");
    need_main("rt_cocoa_window_minimize");
    if (minimize && !w.miniaturized)
    {
        if (g_headless) return; /* a headless window is never on the screen */
        if (in_full_screen(w)) return; /* the space keeps the window (see "Full screen") */
        [w miniaturize:nil];
    }
    else if (!minimize && w.miniaturized)
        [w deminiaturize:nil];
}

void rt_cocoa_window_set_client(void *native, rt_rect client)
{
    RTWindow *w = as_window(native, "rt_cocoa_window_set_client");
    need_main("rt_cocoa_window_set_client");
    if (in_full_screen(w))
    {
        NSRect f = [w frameRectForContentRect:to_ns(client)];
        if (w.screen != nil && frame_covers(f, w.screen.frame) && !w.fsPendingKindSet)
        {
            w.fsPendingFrameSet = NO; /* the space already covers the display */
            return;
        }
        w.fsPendingFrame = f;
        w.fsPendingFrameSet = YES;
        leave_full_screen(w);
        return;
    }
    w.suppress++;
    [w setFrame:[w frameRectForContentRect:to_ns(client)] display:YES];
    w.suppress--;
    update_presentation(w);
}

void rt_cocoa_window_set_title(void *native, const char *title)
{
    RTWindow *w = as_window(native, "rt_cocoa_window_set_title");
    need_main("rt_cocoa_window_set_title");
    w.title = ns_string(title);
}

void rt_cocoa_window_set_kind(void *native, unsigned kind)
{
    RTWindow *w = as_window(native, "rt_cocoa_window_set_kind");
    NSRect content;
    need_main("rt_cocoa_window_set_kind");
    if ((kind & RT_WIN_METAL) != (w.kind & RT_WIN_METAL))
        rt_fatal("Cocoa: the content view of a window cannot change between Metal and a dialog pane");
    if (in_full_screen(w))
    {
        if (style_of(kind) == style_of(w.kind) && !w.fsPendingKindSet)
        {
            apply_kind_extras(w, kind);
            return;
        }
        w.fsPendingKind = kind;
        w.fsPendingKindSet = YES;
        leave_full_screen(w);
        return;
    }
    content = [w contentRectForFrameRect:w.frame];
    w.suppress++;
    w.styleMask = style_of(kind);
    [w setFrame:[w frameRectForContentRect:content] display:YES];
    w.suppress--;
    apply_kind_extras(w, kind);
    [w makeFirstResponder:w.firstResponder != nil ? w.firstResponder : w.contentView];
    update_presentation(w);
}

rt_rect rt_cocoa_window_client(void *native)
{
    RTWindow *w = as_window(native, "rt_cocoa_window_client");
    need_main("rt_cocoa_window_client");
    return from_ns([w contentRectForFrameRect:w.frame]);
}

bool rt_cocoa_window_is_key(void *native)
{
    RTWindow *w = as_window(native, "rt_cocoa_window_is_key");
    need_main("rt_cocoa_window_is_key");
    return w.isKeyWindow;
}

void rt_cocoa_activate(void *native)
{
    RTWindow *w = as_window(native, "rt_cocoa_activate");
    need_main("rt_cocoa_activate");
    if (g_headless) return; /* a headless application never becomes active */
    [w makeKeyAndOrderFront:nil];
    [NSApp activate];
}

void rt_cocoa_window_enable(void *native, bool enabled)
{
    RTWindow *w = as_window(native, "rt_cocoa_window_enable");
    need_main("rt_cocoa_window_enable");
    w.rtEnabled = enabled;
    if ([w.contentView isKindOfClass:[RTView class]]) ((RTView *)w.contentView).rtEnabled = enabled;
}

void *rt_cocoa_window_layer(void *native)
{
    id o = (__bridge id)native;
    NSView *v;
    need_main("rt_cocoa_window_layer");
    if ([o isKindOfClass:[NSWindow class]])
        v = ((NSWindow *)o).contentView;
    else if ([o isKindOfClass:[NSView class]])
        v = (NSView *)o;
    else
        rt_fatal("rt_cocoa_window_layer: the native object is not a window or a view");
    if (![v isKindOfClass:[RTView class]] || !((RTView *)v).metal) return NULL;
    return (__bridge void *)v.layer;
}

void *rt_cocoa_content_view(void *native)
{
    id o = (__bridge id)native;
    need_main("rt_cocoa_content_view");
    if ([o isKindOfClass:[NSWindow class]]) return (__bridge void *)((NSWindow *)o).contentView;
    if ([o isKindOfClass:[NSView class]]) return native;
    rt_fatal("rt_cocoa_content_view: the native object is not a window or a view");
}

/* ---------------------------------------------------------------- controls */


@interface RTList : NSObject <NSTableViewDataSource, NSTableViewDelegate>
@property (nonatomic, strong) NSMutableArray<NSMutableArray<NSString *> *> *rows;
@property (nonatomic, weak) NSTableView *table;
@end

@implementation RTList
- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv
{
    (void)tv;
    return (NSInteger)self.rows.count;
}

- (NSView *)tableView:(NSTableView *)tv viewForTableColumn:(NSTableColumn *)col row:(NSInteger)row
{
    NSTextField *f = [tv makeViewWithIdentifier:@"rtcell" owner:self];
    NSInteger c = col.identifier.integerValue;
    NSMutableArray<NSString *> *r;
    if (f == nil)
    {
        f = [NSTextField labelWithString:@""];
        f.identifier = @"rtcell";
        f.font = [NSFont systemFontOfSize:RT_CONTROL_FONT_SIZE];
        f.lineBreakMode = NSLineBreakByTruncatingTail;
    }
    f.alignment = col.headerCell.alignment;
    r = row >= 0 && (NSUInteger)row < self.rows.count ? self.rows[(NSUInteger)row] : nil;
    f.stringValue = r != nil && c >= 0 && (NSUInteger)c < r.count ? r[(NSUInteger)c] : @"";
    return f;
}
@end

@interface RTControlTarget : NSObject <NSTextFieldDelegate, NSTextViewDelegate>
@end

static RTControlTarget *g_target;

/* Check boxes and radio buttons show the state that USER32 keeps; a
   click only reports the event (the dialog code changes the state). */
static void restore_models(NSView *parent)
{
    for (NSView *s in parent.subviews)
    {
        NSNumber *m = objc_getAssociatedObject(s, &k_model_key);
        if (m != nil && [s isKindOfClass:[NSButton class]]) ((NSButton *)s).state = m.integerValue;
    }
}

@implementation RTControlTarget
- (void)clicked:(id)sender
{
    NSView *v = sender;
    if (objc_getAssociatedObject(v, &k_model_key) != nil && v.superview != nil) restore_models(v.superview);
    rt_user32_on_control(view_hwnd(sender), RT_CTL_CLICKED, 0, 0);
}

- (void)selChanged:(id)sender
{
    rt_user32_on_control(view_hwnd(sender), RT_CTL_SELCHANGE, (int32_t)((NSPopUpButton *)sender).indexOfSelectedItem, 0);
}

- (void)tabChanged:(id)sender
{
    rt_user32_on_control(view_hwnd(sender), RT_CTL_TAB, (int32_t)((NSSegmentedControl *)sender).selectedSegment, 0);
}

- (void)listClicked:(id)sender
{
    NSTableView *t = sender;
    rt_user32_on_control(view_hwnd(t), RT_CTL_LIST_CLICK, (int32_t)t.clickedRow, (int32_t)t.clickedColumn);
}

- (void)listDoubleClicked:(id)sender
{
    NSTableView *t = sender;
    rt_user32_on_control(view_hwnd(t), RT_CTL_LIST_DBLCLICK, (int32_t)t.clickedRow, (int32_t)t.clickedColumn);
}

- (void)controlTextDidChange:(NSNotification *)n
{
    rt_user32_on_control(view_hwnd(n.object), RT_CTL_EDIT_CHANGE, 0, 0);
}

- (void)textDidChange:(NSNotification *)n
{
    rt_user32_on_control(view_hwnd(n.object), RT_CTL_EDIT_CHANGE, 0, 0);
}
@end

/* A Win32 tab control: the tab strip at the top and a bordered body. */
@interface RTTabView : NSView
@property (nonatomic, strong) NSSegmentedControl *seg;
@property (nonatomic, strong) NSBox *body;
@end

@implementation RTTabView
- (BOOL)isFlipped { return YES; }

- (void)layout
{
    NSSize b = self.bounds.size;
    CGFloat w = 0, h = RT_TAB_STRIP_HEIGHT;
    NSInteger i;
    for (i = 0; i < self.seg.segmentCount; i++) w += [self.seg widthForSegment:i];
    self.seg.frame = NSMakeRect(2, 0, w, h);
    self.body.frame = NSMakeRect(0, h - 1, b.width, b.height - h + 1 > 0 ? b.height - h + 1 : 0);
    [super layout];
}
@end

static NSTextAlignment ns_align(int align)
{
    return align == RT_ALIGN_CENTER ? NSTextAlignmentCenter : align == RT_ALIGN_RIGHT ? NSTextAlignmentRight : NSTextAlignmentLeft;
}

/* Child controls keep the text size of AppKit; on a Retina display their
   frames are half the size in points of their Win32 size (g_px). */
static NSRect view_rect(rt_rect r)
{
    return NSMakeRect(pt(r.left), pt(r.top), pt(r.right - r.left), pt(r.bottom - r.top));
}

static NSView *as_view(void *v, const char *fn)
{
    id o = (__bridge id)v;
    if (![o isKindOfClass:[NSView class]]) rt_fatal("%s: the native object is not a view", fn);
    return (NSView *)o;
}

/* The control in a scroll view (list, multi-line edit). */
static NSView *inner(NSView *v)
{
    if ([v isKindOfClass:[NSScrollView class]]) return ((NSScrollView *)v).documentView;
    if ([v isKindOfClass:[RTTabView class]]) return ((RTTabView *)v).seg;
    return v;
}

void *rt_cocoa_view_create(uint32_t hwnd, void *parent, int kind, rt_rect frame, const char *text, int align, bool read_only)
{
    NSView *p = as_view(parent, "rt_cocoa_view_create"), *v = nil;
    NSFont *font = [NSFont systemFontOfSize:RT_CONTROL_FONT_SIZE];
    NSRect r = view_rect(frame);
    NSString *s = ns_string(text);
    bool back = false;
    need_main("rt_cocoa_view_create");
    if (g_target == nil) g_target = [RTControlTarget new];
    switch (kind)
    {
    case RT_VIEW_PANE:
    case RT_VIEW_METAL:
    {
        RTView *rv = [[RTView alloc] initWithFrame:r];
        rv.hwnd = hwnd;
        rv.metal = kind == RT_VIEW_METAL;
        rv.wantsLayer = YES;
        v = rv;
        break;
    }
    case RT_VIEW_PUSH:
    case RT_VIEW_DEFPUSH:
    {
        NSButton *b = [NSButton buttonWithTitle:s target:g_target action:@selector(clicked:)];
        b.bezelStyle = NSBezelStylePush;
        b.controlSize = NSControlSizeSmall;
        b.font = font;
        if (kind == RT_VIEW_DEFPUSH) b.keyEquivalent = @"\r";
        v = b;
        break;
    }
    case RT_VIEW_CHECK:
    case RT_VIEW_RADIO:
    {
        NSButton *b = [NSButton buttonWithTitle:s target:g_target action:@selector(clicked:)];
        [b setButtonType:kind == RT_VIEW_CHECK ? NSButtonTypeSwitch : NSButtonTypeRadio];
        b.controlSize = NSControlSizeSmall;
        b.font = font;
        b.state = NSControlStateValueOff;
        objc_setAssociatedObject(b, &k_model_key, @(NSControlStateValueOff), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        v = b;
        break;
    }
    case RT_VIEW_GROUP:
    {
        NSBox *b = [[NSBox alloc] initWithFrame:r];
        b.boxType = NSBoxPrimary;
        b.title = s;
        b.titleFont = font;
        b.titlePosition = s.length > 0 ? NSAtTop : NSNoTitle;
        v = b;
        back = true;
        break;
    }
    case RT_VIEW_LABEL:
    {
        NSTextField *f = [NSTextField wrappingLabelWithString:s];
        f.font = font;
        f.alignment = ns_align(align);
        f.selectable = NO;
        v = f;
        break;
    }
    case RT_VIEW_IMAGE:
    {
        NSImageView *iv = [[NSImageView alloc] initWithFrame:r];
        iv.imageScaling = NSImageScaleProportionallyDown;
        iv.editable = NO;
        v = iv;
        break;
    }
    case RT_VIEW_EDIT:
    {
        NSTextField *f = [NSTextField textFieldWithString:s];
        f.font = font;
        f.alignment = ns_align(align);
        f.editable = !read_only;
        f.selectable = YES;
        f.delegate = g_target;
        f.usesSingleLineMode = YES;
        f.cell.scrollable = YES;
        v = f;
        break;
    }
    case RT_VIEW_EDIT_MULTI:
    {
        NSScrollView *sv = [[NSScrollView alloc] initWithFrame:r];
        NSTextView *tv = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, r.size.width, r.size.height)];
        sv.hasVerticalScroller = YES;
        sv.borderType = NSBezelBorder;
        tv.font = font;
        tv.string = s;
        tv.editable = !read_only;
        tv.selectable = YES;
        tv.richText = NO;
        tv.alignment = ns_align(align);
        tv.delegate = g_target;
        tv.verticallyResizable = YES;
        tv.autoresizingMask = NSViewWidthSizable;
        tv.textContainer.widthTracksTextView = YES;
        sv.documentView = tv;
        set_view_info(tv, hwnd);
        v = sv;
        break;
    }
    case RT_VIEW_POPUP:
    {
        NSPopUpButton *pb = [[NSPopUpButton alloc] initWithFrame:r pullsDown:NO];
        pb.controlSize = NSControlSizeSmall;
        pb.font = font;
        [pb removeAllItems];
        pb.target = g_target;
        pb.action = @selector(selChanged:);
        v = pb;
        break;
    }
    case RT_VIEW_TABS:
    {
        RTTabView *tv = [[RTTabView alloc] initWithFrame:r];
        NSSegmentedControl *sc = [[NSSegmentedControl alloc] initWithFrame:NSZeroRect];
        NSBox *body = [[NSBox alloc] initWithFrame:NSZeroRect];
        sc.segmentCount = 0;
        sc.trackingMode = NSSegmentSwitchTrackingSelectOne;
        sc.controlSize = NSControlSizeSmall;
        sc.font = font;
        sc.target = g_target;
        sc.action = @selector(tabChanged:);
        body.boxType = NSBoxPrimary;
        body.titlePosition = NSNoTitle;
        tv.seg = sc;
        tv.body = body;
        [tv addSubview:body];
        [tv addSubview:sc];
        set_view_info(sc, hwnd);
        v = tv;
        break;
    }
    case RT_VIEW_LIST:
    {
        NSScrollView *sv = [[NSScrollView alloc] initWithFrame:r];
        NSTableView *tv = [[NSTableView alloc] initWithFrame:NSMakeRect(0, 0, r.size.width, r.size.height)];
        RTList *list = [RTList new];
        list.rows = [NSMutableArray array];
        list.table = tv;
        tv.dataSource = list;
        tv.delegate = list;
        tv.target = g_target;
        tv.action = @selector(listClicked:);
        tv.doubleAction = @selector(listDoubleClicked:);
        tv.rowHeight = 16;
        tv.style = NSTableViewStylePlain;
        tv.usesAlternatingRowBackgroundColors = NO;
        tv.columnAutoresizingStyle = NSTableViewNoColumnAutoresizing;
        tv.allowsColumnReordering = NO; /* the column index is the Win32 subitem */
        sv.documentView = tv;
        sv.hasVerticalScroller = YES;
        sv.hasHorizontalScroller = YES;
        sv.borderType = NSBezelBorder;
        objc_setAssociatedObject(sv, &k_list_key, list, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        set_view_info(tv, hwnd);
        v = sv;
        break;
    }
    default:
        rt_fatal("rt_cocoa_view_create: view kind %d is not known", kind);
    }
    v.frame = r;
    set_view_info(v, hwnd);
    if (back)
        [p addSubview:v positioned:NSWindowBelow relativeTo:nil];
    else
        [p addSubview:v];
    return (__bridge_retained void *)v;
}

void rt_cocoa_view_destroy(void *native)
{
    NSView *v;
    need_main("rt_cocoa_view_destroy");
    v = (__bridge_transfer NSView *)native;
    if (![v isKindOfClass:[NSView class]]) rt_fatal("rt_cocoa_view_destroy: the native object is not a view");
    if (v.window != nil && [v.window.firstResponder isKindOfClass:[NSView class]] &&
        [(NSView *)v.window.firstResponder isDescendantOf:v])
        [v.window makeFirstResponder:nil];
    if (g_down_view == (RTView *)v) g_down_view = nil;
    [v removeFromSuperview];
}

void rt_cocoa_view_set_frame(void *native, rt_rect frame)
{
    NSView *v = as_view(native, "rt_cocoa_view_set_frame");
    need_main("rt_cocoa_view_set_frame");
    v.frame = view_rect(frame);
}

void rt_cocoa_view_set_visible(void *native, bool visible)
{
    NSView *v = as_view(native, "rt_cocoa_view_set_visible");
    need_main("rt_cocoa_view_set_visible");
    v.hidden = !visible;
}

void rt_cocoa_view_set_enabled(void *native, bool enabled)
{
    NSView *v = as_view(native, "rt_cocoa_view_set_enabled"), *c = inner(v);
    need_main("rt_cocoa_view_set_enabled");
    if ([v isKindOfClass:[RTView class]])
        ((RTView *)v).rtEnabled = enabled;
    else if ([c isKindOfClass:[NSControl class]])
        ((NSControl *)c).enabled = enabled;
    else if ([c isKindOfClass:[NSTextView class]])
        ((NSTextView *)c).editable = enabled; /* a disabled edit field cannot change */
    else if ([v isKindOfClass:[NSBox class]])
        ((NSCell *)((NSBox *)v).titleCell).enabled = enabled;
}

void rt_cocoa_view_set_text(void *native, const char *text)
{
    NSView *v = as_view(native, "rt_cocoa_view_set_text"), *c = inner(v);
    NSString *s = ns_string(text);
    need_main("rt_cocoa_view_set_text");
    if ([c isKindOfClass:[NSButton class]])
        ((NSButton *)c).title = s;
    else if ([c isKindOfClass:[NSTextField class]])
        ((NSTextField *)c).stringValue = s;
    else if ([c isKindOfClass:[NSTextView class]])
        ((NSTextView *)c).string = s;
    else if ([v isKindOfClass:[NSBox class]])
    {
        ((NSBox *)v).title = s;
        ((NSBox *)v).titlePosition = s.length > 0 ? NSAtTop : NSNoTitle;
    }
    /* Panes, images, lists, tabs and drop-down lists show no window text. */
}

char *rt_cocoa_view_text(void *native)
{
    NSView *v = as_view(native, "rt_cocoa_view_text"), *c = inner(v);
    need_main("rt_cocoa_view_text");
    if ([c isKindOfClass:[NSTextField class]]) return c_string(((NSTextField *)c).stringValue);
    if ([c isKindOfClass:[NSTextView class]]) return c_string(((NSTextView *)c).string);
    if ([c isKindOfClass:[NSButton class]]) return c_string(((NSButton *)c).title);
    return c_string(@"");
}

void rt_cocoa_view_set_check(void *native, int state)
{
    NSView *v = as_view(native, "rt_cocoa_view_set_check");
    NSButton *b;
    NSControlStateValue s;
    need_main("rt_cocoa_view_set_check");
    if (![v isKindOfClass:[NSButton class]]) rt_fatal("rt_cocoa_view_set_check: the view is not a button");
    b = (NSButton *)v;
    s = state == 1 ? NSControlStateValueOn : state == 2 ? NSControlStateValueMixed : NSControlStateValueOff;
    if (state == 2) b.allowsMixedState = YES;
    objc_setAssociatedObject(b, &k_model_key, @(s), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    b.state = s;
}

int rt_cocoa_view_check(void *native)
{
    NSView *v = as_view(native, "rt_cocoa_view_check");
    NSControlStateValue s;
    need_main("rt_cocoa_view_check");
    if (![v isKindOfClass:[NSButton class]]) return 0;
    s = ((NSButton *)v).state;
    return s == NSControlStateValueOn ? 1 : s == NSControlStateValueMixed ? 2 : 0;
}

void rt_cocoa_view_focus(void *native)
{
    NSView *v = as_view(native, "rt_cocoa_view_focus"), *c = inner(v);
    need_main("rt_cocoa_view_focus");
    if (c.window != nil && c.window.firstResponder != c) [c.window makeFirstResponder:c];
}

void rt_cocoa_view_set_image(void *native, const uint8_t *rgba, uint32_t w, uint32_t h)
{
    NSView *v = as_view(native, "rt_cocoa_view_set_image");
    NSBitmapImageRep *rep;
    NSImage *img;
    need_main("rt_cocoa_view_set_image");
    if (![v isKindOfClass:[NSImageView class]]) rt_fatal("rt_cocoa_view_set_image: the view is not an image view");
    if (rgba == NULL || w == 0 || h == 0)
    {
        ((NSImageView *)v).image = nil;
        return;
    }
    if (w > 4096 || h > 4096) rt_fatal("rt_cocoa_view_set_image: an image of %ux%u is too large", w, h);
    rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
                                                  pixelsWide:(NSInteger)w
                                                  pixelsHigh:(NSInteger)h
                                               bitsPerSample:8
                                             samplesPerPixel:4
                                                    hasAlpha:YES
                                                    isPlanar:NO
                                              colorSpaceName:NSDeviceRGBColorSpace
                                                bitmapFormat:NSBitmapFormatAlphaNonpremultiplied
                                                 bytesPerRow:(NSInteger)(w * 4u)
                                                bitsPerPixel:32];
    if (rep == nil || rep.bitmapData == NULL) rt_fatal("Cocoa: an image could not be made");
    memcpy(rep.bitmapData, rgba, (size_t)w * h * 4u);
    img = [[NSImage alloc] initWithSize:NSMakeSize((CGFloat)w, (CGFloat)h)];
    [img addRepresentation:rep];
    ((NSImageView *)v).image = img;
}

void rt_cocoa_view_set_system_icon(void *native, uint32_t id)
{
    NSView *v = as_view(native, "rt_cocoa_view_set_system_icon");
    NSImage *img = nil;
    need_main("rt_cocoa_view_set_system_icon");
    if (![v isKindOfClass:[NSImageView class]]) rt_fatal("rt_cocoa_view_set_system_icon: the view is not an image view");
    switch (id)
    {
    case 32512: case 32517: img = NSApp.applicationIconImage; break; /* IDI_APPLICATION, IDI_WINLOGO */
    case 32513: img = [NSImage imageWithSystemSymbolName:@"xmark.octagon.fill" accessibilityDescription:@"Error"]; break;
    case 32514: img = [NSImage imageWithSystemSymbolName:@"questionmark.circle.fill" accessibilityDescription:@"Question"]; break;
    case 32515: img = [NSImage imageWithSystemSymbolName:@"exclamationmark.triangle.fill" accessibilityDescription:@"Warning"]; break;
    case 32516: img = [NSImage imageWithSystemSymbolName:@"info.circle.fill" accessibilityDescription:@"Information"]; break;
    case 32518: img = [NSImage imageWithSystemSymbolName:@"lock.shield.fill" accessibilityDescription:@"Shield"]; break;
    default: rt_fatal("rt_cocoa_view_set_system_icon: system icon %u is not known", id);
    }
    if (img == nil) rt_fatal("Cocoa: system icon %u is not available", id);
    if (img.isTemplate)
    {
        NSImageSymbolConfiguration *cfg = [NSImageSymbolConfiguration configurationWithPointSize:24 weight:NSFontWeightRegular];
        img = [img imageWithSymbolConfiguration:cfg];
    }
    ((NSImageView *)v).imageScaling = NSImageScaleProportionallyUpOrDown;
    ((NSImageView *)v).image = img;
}

static NSPopUpButton *as_popup(void *native, const char *fn)
{
    NSView *v = as_view(native, fn);
    need_main(fn);
    if (![v isKindOfClass:[NSPopUpButton class]]) rt_fatal("%s: the view is not a drop-down list", fn);
    return (NSPopUpButton *)v;
}

void rt_cocoa_popup_insert(void *native, int index, const char *text)
{
    NSPopUpButton *p = as_popup(native, "rt_cocoa_popup_insert");
    NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:ns_string(text) action:NULL keyEquivalent:@""];
    NSInteger n = p.menu.numberOfItems;
    NSInteger sel = p.indexOfSelectedItem;
    if (index < 0 || index > n) index = (int)n;
    /* NSMenu allows items with the same title (insertItemWithTitle: does
       not). */
    [p.menu insertItem:it atIndex:index];
    if (sel < 0)
        [p selectItem:nil];
    else
        [p selectItemAtIndex:sel >= index ? sel + 1 : sel];
}

void rt_cocoa_popup_remove(void *native, int index)
{
    NSPopUpButton *p = as_popup(native, "rt_cocoa_popup_remove");
    NSInteger sel = p.indexOfSelectedItem;
    if (index < 0 || index >= p.menu.numberOfItems) return;
    [p.menu removeItemAtIndex:index];
    if (sel < 0 || sel == index)
        [p selectItem:nil];
    else
        [p selectItemAtIndex:sel > index ? sel - 1 : sel];
}

void rt_cocoa_popup_clear(void *native)
{
    [as_popup(native, "rt_cocoa_popup_clear") removeAllItems];
}

void rt_cocoa_popup_select(void *native, int index)
{
    NSPopUpButton *p = as_popup(native, "rt_cocoa_popup_select");
    if (index < 0 || index >= p.menu.numberOfItems)
        [p selectItem:nil];
    else
        [p selectItemAtIndex:index];
}

int rt_cocoa_popup_selected(void *native)
{
    return (int)as_popup(native, "rt_cocoa_popup_selected").indexOfSelectedItem;
}

static NSSegmentedControl *as_tabs(void *native, const char *fn)
{
    NSView *v = as_view(native, fn);
    need_main(fn);
    if (![v isKindOfClass:[RTTabView class]]) rt_fatal("%s: the view is not a tab control", fn);
    return ((RTTabView *)v).seg;
}

void rt_cocoa_tabs_insert(void *native, int index, const char *text, int width)
{
    NSSegmentedControl *t = as_tabs(native, "rt_cocoa_tabs_insert");
    NSInteger n = t.segmentCount, i, sel = t.selectedSegment;
    if (index < 0 || index > n) index = (int)n;
    t.segmentCount = n + 1;
    for (i = n; i > index; i--)
    {
        [t setLabel:[t labelForSegment:i - 1] forSegment:i];
        [t setWidth:[t widthForSegment:i - 1] forSegment:i];
    }
    [t setLabel:ns_string(text) forSegment:index];
    [t setWidth:(width > 0 ? pt(width) : 1) forSegment:index];
    t.selectedSegment = sel < 0 ? -1 : sel >= index ? sel + 1 : sel;
    t.superview.needsLayout = YES;
    [t.superview layoutSubtreeIfNeeded];
}

void rt_cocoa_tabs_clear(void *native)
{
    NSSegmentedControl *t = as_tabs(native, "rt_cocoa_tabs_clear");
    t.segmentCount = 0;
    t.superview.needsLayout = YES;
    [t.superview layoutSubtreeIfNeeded];
}

void rt_cocoa_tabs_select(void *native, int index)
{
    NSSegmentedControl *t = as_tabs(native, "rt_cocoa_tabs_select");
    t.selectedSegment = index >= 0 && index < t.segmentCount ? index : -1;
}

int rt_cocoa_tabs_selected(void *native)
{
    return (int)as_tabs(native, "rt_cocoa_tabs_selected").selectedSegment;
}

static RTList *as_list(void *native, const char *fn)
{
    NSView *v = as_view(native, fn);
    RTList *l;
    need_main(fn);
    l = objc_getAssociatedObject(v, &k_list_key);
    if (l == nil) rt_fatal("%s: the view is not a list", fn);
    return l;
}

void rt_cocoa_list_insert_column(void *native, int index, const char *title, int width, int align)
{
    RTList *l = as_list(native, "rt_cocoa_list_insert_column");
    NSTableView *t = l.table;
    NSInteger n = (NSInteger)t.tableColumns.count, i;
    NSTableColumn *c;
    if (index < 0 || index > n) index = (int)n;
    /* Column identifiers are data indexes: move the data of later columns. */
    for (NSTableColumn *old in t.tableColumns)
        if (old.identifier.integerValue >= index) old.identifier = [NSString stringWithFormat:@"%ld", (long)(old.identifier.integerValue + 1)];
    for (NSMutableArray<NSString *> *row in l.rows)
        if ((NSUInteger)index <= row.count) [row insertObject:@"" atIndex:(NSUInteger)index];
    c = [[NSTableColumn alloc] initWithIdentifier:[NSString stringWithFormat:@"%d", index]];
    c.title = ns_string(title);
    c.headerCell.alignment = ns_align(align);
    c.width = width > 0 ? pt(width) : 0;
    [t addTableColumn:c];
    for (i = (NSInteger)t.tableColumns.count - 1; i > index; i--) [t moveColumn:i - 1 toColumn:i];
    [t reloadData];
}

void rt_cocoa_list_insert_row(void *native, int index)
{
    RTList *l = as_list(native, "rt_cocoa_list_insert_row");
    NSMutableArray<NSString *> *row = [NSMutableArray array];
    NSUInteger i;
    for (i = 0; i < l.table.tableColumns.count; i++) [row addObject:@""];
    if (index < 0 || (NSUInteger)index > l.rows.count) index = (int)l.rows.count;
    [l.rows insertObject:row atIndex:(NSUInteger)index];
    [l.table reloadData];
}

void rt_cocoa_list_delete_row(void *native, int index)
{
    RTList *l = as_list(native, "rt_cocoa_list_delete_row");
    if (index < 0 || (NSUInteger)index >= l.rows.count) return;
    [l.rows removeObjectAtIndex:(NSUInteger)index];
    [l.table reloadData];
}

void rt_cocoa_list_clear(void *native)
{
    RTList *l = as_list(native, "rt_cocoa_list_clear");
    [l.rows removeAllObjects];
    [l.table reloadData];
}

void rt_cocoa_list_set_cell(void *native, int row, int column, const char *text)
{
    RTList *l = as_list(native, "rt_cocoa_list_set_cell");
    NSMutableArray<NSString *> *r;
    if (row < 0 || (NSUInteger)row >= l.rows.count || column < 0) return;
    r = l.rows[(NSUInteger)row];
    while (r.count <= (NSUInteger)column) [r addObject:@""];
    r[(NSUInteger)column] = ns_string(text);
    [l.table reloadDataForRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row]
                       columnIndexes:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0, l.table.tableColumns.count)]];
}

void rt_cocoa_list_set_grid(void *native, bool grid, bool full_row_select)
{
    RTList *l = as_list(native, "rt_cocoa_list_set_grid");
    l.table.gridStyleMask = grid ? (NSTableViewSolidVerticalGridLineMask | NSTableViewSolidHorizontalGridLineMask) : NSTableViewGridNone;
    /* NSTableView always selects full rows; full_row_select only changes
       the highlight of the Win32 list view. */
    (void)full_row_select;
}

/* ---------------------------------------------------------------- keyboard input source */

static atomic_bool g_source_native;

/* The current keyboard input source types characters that are not ASCII
   (an input method such as Japanese Hiragana or Korean): the IME
   conversion mode is "native". Main thread (Text Input Sources). */
static void refresh_input_source(void)
{
    TISInputSourceRef src = TISCopyCurrentKeyboardInputSource();
    bool native = false;
    if (src != NULL)
    {
        CFBooleanRef ascii = (CFBooleanRef)TISGetInputSourceProperty(src, kTISPropertyInputSourceIsASCIICapable);
        native = ascii != NULL && !CFBooleanGetValue(ascii);
        CFRelease(src);
    }
    atomic_store(&g_source_native, native);
}

bool rt_cocoa_input_source_native(void)
{
    return atomic_load(&g_source_native);
}

/* ---------------------------------------------------------------- application and events */

@interface RTAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation RTAppDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender
{
    (void)sender;
    /* Quit (Command-Q, the Dock): the windows get WM_CLOSE; the process
       ends when the guest exits. */
    rt_user32_on_quit();
    return NSTerminateCancel;
}

- (void)applicationDidBecomeActive:(NSNotification *)n
{
    (void)n;
    refresh_input_source(); /* the user can select another source in another application */
    rt_user32_on_app_active(true);
    enforce_clip();
}

- (void)applicationDidResignActive:(NSNotification *)n
{
    (void)n;
    rt_user32_on_app_active(false);
    if (!g_headless && g_fs_windows == 0) NSApp.presentationOptions = NSApplicationPresentationDefault;
}

- (void)applicationDidChangeScreenParameters:(NSNotification *)n
{
    (void)n;
    refresh_display_cache();
}
@end

static RTAppDelegate *g_delegate;
static id g_monitor_local, g_monitor_global, g_monitor_keyup, g_input_source_observer;

/* Relative motion for raw input, and the cursor clip. The local monitor
   gets the events of this application, the global monitor the events of
   the other applications (raw input with RIDEV_INPUTSINK). */
static void mouse_motion_event(NSEvent *e)
{
    rt_user32_on_mouse_motion(e.deltaX * g_px, e.deltaY * g_px);
    enforce_clip();
}

void rt_cocoa_init(bool headless)
{
    NSMenu *bar, *app;
    NSMenuItem *item;
    NSString *name;
    NSRect fr;
    need_main("rt_cocoa_init");
    g_headless = headless;
    [NSApplication sharedApplication];
    {
        NSScreen *m = NSScreen.mainScreen;
        long s = m != nil ? lround(m.backingScaleFactor) : 1;
        g_px = s >= 1 && s <= 4 ? (int)s : 1;
    }
    /* A process without an application bundle starts with the policy
       NSApplicationActivationPolicyProhibited. setActivationPolicy: returns
       NO for the policy that the application has (macOS 27.3). */
    {
        NSApplicationActivationPolicy want =
            headless ? NSApplicationActivationPolicyProhibited : NSApplicationActivationPolicyRegular;
        if (NSApp.activationPolicy != want && ![NSApp setActivationPolicy:want])
            rt_fatal("Cocoa: the activation policy could not be set");
    }
    g_delegate = [RTAppDelegate new];
    NSApp.delegate = g_delegate;
    name = NSProcessInfo.processInfo.processName;
    bar = [[NSMenu alloc] init];
    app = [[NSMenu alloc] initWithTitle:name];
    item = [[NSMenuItem alloc] initWithTitle:@"" action:NULL keyEquivalent:@""];
    /* The standard items of a Mac application. Hide makes the application
       inactive, as Command-Tab does. */
    [app addItemWithTitle:[@"Hide " stringByAppendingString:name] action:@selector(hide:) keyEquivalent:@"h"];
    [[app addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"]
        setKeyEquivalentModifierMask:NSEventModifierFlagOption | NSEventModifierFlagCommand];
    [app addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [app addItem:[NSMenuItem separatorItem]];
    [app addItemWithTitle:[@"Quit " stringByAppendingString:name] action:@selector(terminate:) keyEquivalent:@"q"];
    item.submenu = app;
    [bar addItem:item];
    {
        /* Window > Minimize (Command-M) for a window with a title bar. A
           borderless window (full screen) has no miniaturize button, so
           AppKit disables the item for it. */
        NSMenu *win = [[NSMenu alloc] initWithTitle:@"Window"];
        NSMenuItem *wi = [[NSMenuItem alloc] initWithTitle:@"Window" action:NULL keyEquivalent:@""];
        [win addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
        wi.submenu = win;
        [bar addItem:wi];
        NSApp.windowsMenu = win;
    }
    NSApp.mainMenu = bar;
    [NSApp finishLaunching];
    fr = [NSWindow frameRectForContentRect:NSMakeRect(0, 0, 100, 100)
                                 styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable];
    atomic_store(&g_title_h, round32(fr.size.height - 100.0));
    refresh_display_cache();
    {
        rt_display d[RT_MAX_DISPLAYS];
        rt_cocoa_displays(d);
        /* The headless cursor starts at the center of the main display. */
        g_virtual_x = d[0].frame.left + (d[0].frame.right - d[0].frame.left) / 2;
        g_virtual_y = d[0].frame.top + (d[0].frame.bottom - d[0].frame.top) / 2;
    }
    {
        NSEventMask motion = NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDragged |
                             NSEventMaskOtherMouseDragged;
        g_monitor_local = [NSEvent addLocalMonitorForEventsMatchingMask:motion
                                                                handler:^NSEvent *(NSEvent *e) {
                                                                    mouse_motion_event(e);
                                                                    return e;
                                                                }];
        /* AppKit does not send a key-up with Command to the key window:
           send it here. */
        g_monitor_keyup = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyUp
                                                                handler:^NSEvent *(NSEvent *e) {
                                                                    if ((e.modifierFlags & NSEventModifierFlagCommand) &&
                                                                        [e.window isKindOfClass:[RTWindow class]])
                                                                    {
                                                                        [e.window sendEvent:e];
                                                                        return nil;
                                                                    }
                                                                    return e;
                                                                }];
        if (!headless)
        {
            /* The cursor clip also applies when the cursor is over the
               windows of other applications. */
            g_monitor_global = [NSEvent addGlobalMonitorForEventsMatchingMask:motion
                                                                      handler:^(NSEvent *e) {
                                                                          mouse_motion_event(e);
                                                                      }];
        }
        if (g_monitor_local == nil || g_monitor_keyup == nil || (!headless && g_monitor_global == nil))
            rt_fatal("Cocoa: the event monitors could not be installed");
    }
    refresh_input_source();
    g_input_source_observer =
        [NSNotificationCenter.defaultCenter addObserverForName:NSTextInputContextKeyboardSelectionDidChangeNotification
                                                        object:nil
                                                         queue:nil
                                                    usingBlock:^(NSNotification *note) {
                                                        (void)note;
                                                        refresh_input_source();
                                                        rt_imm32_input_source_changed();
                                                    }];
    if (g_input_source_observer == nil) rt_fatal("Cocoa: the input source observer could not be installed");
    if (!headless)
    {
        /* SetCursorPos: no pause of the mouse after a move of the cursor. */
        CGEventSourceRef src = CGEventSourceCreate(kCGEventSourceStateCombinedSessionState);
        if (src == NULL) rt_fatal("Cocoa: CGEventSourceCreate failed");
        CGEventSourceSetLocalEventsSuppressionInterval(src, 0.0);
        CFRelease(src);
        [NSApp activate];
    }
}

static void dispatch_event(NSEvent *e)
{
    if (e.type == NSEventTypeApplicationDefined && e.subtype == WAKE_SUBTYPE)
    {
        atomic_store(&g_wake_posted, false);
        return;
    }
    [NSApp sendEvent:e];
}

static uint64_t now_ns(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

void rt_cocoa_pump(void)
{
    static uint64_t last;
    uint64_t t = now_ns();
    need_main("rt_cocoa_pump");
    if (t - last < 1000000u) return;
    last = t;
    @autoreleasepool
    {
        apply_cursor(false);
        for (;;)
        {
            NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES];
            if (e == nil) break;
            dispatch_event(e);
        }
        [NSApp updateWindows];
        apply_cursor(false);
    }
}

bool rt_cocoa_wait(atomic_bool *woken, bool infinite, uint64_t deadline_ns)
{
    need_main("rt_cocoa_wait");
    for (;;)
    {
        uint64_t t;
        if (atomic_load(woken)) return true;
        t = now_ns();
        if (!infinite && t >= deadline_ns) return false;
        @autoreleasepool
        {
            NSDate *until = infinite ? NSDate.distantFuture : [NSDate dateWithTimeIntervalSinceNow:(double)(deadline_ns - t) / 1e9];
            NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES];
            if (e != nil) dispatch_event(e);
            apply_cursor(false);
        }
    }
}

void rt_cocoa_wake(void)
{
    if (atomic_exchange(&g_wake_posted, true)) return;
    @autoreleasepool
    {
        NSEvent *e = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                        location:NSZeroPoint
                                   modifierFlags:0
                                       timestamp:0
                                    windowNumber:0
                                         context:nil
                                         subtype:WAKE_SUBTYPE
                                           data1:0
                                           data2:0];
        if (e == nil) rt_fatal("Cocoa: the wake event could not be made");
        [NSApp postEvent:e atStart:NO];
    }
}

/* ---------------------------------------------------------------- keyboard and text input */

bool rt_cocoa_caps_lock(void)
{
    return (CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState) & kCGEventFlagMaskAlphaShift) != 0;
}

static RTView *as_rtview(void *native, const char *fn)
{
    id o = (__bridge id)native;
    if (![o isKindOfClass:[RTView class]]) rt_fatal("%s: the native object is not a runtime view", fn);
    return (RTView *)o;
}

void rt_cocoa_ime_enable(void *native, bool enabled)
{
    RTView *v = as_rtview(native, "rt_cocoa_ime_enable");
    need_main("rt_cocoa_ime_enable");
    if (!enabled && v.imeEnabled) [v imeCancel];
    v.imeEnabled = enabled;
}

void rt_cocoa_ime_cancel(void *native)
{
    RTView *v = as_rtview(native, "rt_cocoa_ime_cancel");
    need_main("rt_cocoa_ime_cancel");
    [v imeCancel];
}

uint32_t rt_cocoa_caret_blink_ms(void)
{
    /* The insertion point blink period of AppKit text views (the
       NSTextInsertionPointBlinkPeriodOn default). 500 ms when the
       default is not set: UNCONFIRMED value of the AppKit default. */
    NSInteger v = [NSUserDefaults.standardUserDefaults integerForKey:@"NSTextInsertionPointBlinkPeriodOn"];
    if (v <= 0 || v > 10000) return 500;
    return (uint32_t)v;
}

/* ---------------------------------------------------------------- clipboard */

static pthread_mutex_t g_pb_lock = PTHREAD_MUTEX_INITIALIZER;
static NSPasteboard *g_private_pb; /* headless: a private pasteboard */

static NSPasteboard *pasteboard(void)
{
    if (!g_headless) return NSPasteboard.generalPasteboard;
    if (g_private_pb == nil) g_private_pb = [NSPasteboard pasteboardWithUniqueName];
    return g_private_pb;
}

static void pb_lock(void)
{
    if (pthread_mutex_lock(&g_pb_lock) != 0) rt_fatal("pasteboard lock failed");
}

static void pb_unlock(void)
{
    if (pthread_mutex_unlock(&g_pb_lock) != 0) rt_fatal("pasteboard unlock failed");
}

char *rt_cocoa_clipboard_text(void)
{
    char *r = NULL;
    pb_lock();
    @autoreleasepool
    {
        NSString *s = [pasteboard() stringForType:NSPasteboardTypeString];
        if (s != nil) r = c_string(s);
    }
    pb_unlock();
    return r;
}

bool rt_cocoa_clipboard_has_text(void)
{
    bool r;
    pb_lock();
    @autoreleasepool
    {
        r = [pasteboard() availableTypeFromArray:@[ NSPasteboardTypeString ]] != nil;
    }
    pb_unlock();
    return r;
}

void rt_cocoa_clipboard_set_text(const char *text)
{
    pb_lock();
    @autoreleasepool
    {
        NSPasteboard *pb = pasteboard();
        [pb clearContents];
        if (text != NULL && ![pb setString:ns_string(text) forType:NSPasteboardTypeString])
            rt_fatal("Cocoa: the pasteboard text could not be set");
    }
    pb_unlock();
}
