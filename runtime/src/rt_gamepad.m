/*
 * Game controller support: the GameController framework of macOS.
 *
 * The game has no controller support. This file gives it two layers:
 *   - Here, on the main thread: the left stick moves the cursor, A is the
 *     left mouse button, B is the right mouse button, and up and down on
 *     the directional pad turn the mouse wheel (zoom, and lists in the
 *     game's windows). The events go through the same functions as native
 *     mouse events (rt_user32_pad_*).
 *   - The managed layer (tools/port_graphics/PortController.cs) reads the
 *     state of all controls with PortPadRead once for each frame. It
 *     presses the keys that the player bound to the game's commands
 *     (PortPadKey) and shows the radial ability menu. While the menu is
 *     open, it takes the left stick, A and B (PortPadCapture): the cursor
 *     does not move and A and B do not click.
 *
 * The controller is GCController.current with the extended gamepad
 * profile (Xbox, PlayStation, Nintendo Switch Pro and MFi controllers).
 * A controller without that profile is not used. While a
 * controller is connected, a timer on the main dispatch queue reads it
 * RT_PAD_HZ times a second (the framework sends no event while a stick
 * stays in one position). A controller that disconnects releases the
 * buttons and keys that it holds.
 *
 * The state for PortPadRead (rt_pad_state) has its own lock: the managed
 * layer reads it on the game's main thread, and the timer writes it on the
 * main dispatch queue.
 */
#import <AppKit/AppKit.h>
#import <GameController/GameController.h>
#import <Foundation/Foundation.h>

#include "rt.h"
#include "rt_user32.h"
#include "rt_window.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define RT_PAD_HZ 120.0
/* Stick dead zone and the cursor speed at full deflection: the height of
   the client area each second. The speed grows with the square of the
   deflection, for fine aiming near the center. */
#define PAD_DEAD_ZONE 0.18
#define PAD_CURSOR_SPEED 1.1
/* Wheel repeat of the directional pad: the first repeat after
   PAD_REPEAT_DELAY seconds, then one notch each PAD_REPEAT_STEP seconds. */
#define PAD_REPEAT_DELAY 0.35
#define PAD_REPEAT_STEP 0.12
/* Wheel notches each second at full deflection of the right stick in the
   snap mode (PortPadSnap); the rate grows with the deflection. */
#define PAD_SNAP_WHEEL_RATE 14.0
/* Glyphs (PortPadGlyph): 0..15 the buttons of the PAD_* bits, then the
   left stick, the right stick and the directional pad. */
#define PAD_GLYPHS 19
#define PAD_GLYPH_MIN 8
#define PAD_GLYPH_MAX 256

/* Bits of PortPadRead (tools/port_graphics/PortController.cs has the same
   values). */
enum {
    PAD_A = 1u << 0, PAD_B = 1u << 1, PAD_X = 1u << 2, PAD_Y = 1u << 3,
    PAD_LB = 1u << 4, PAD_RB = 1u << 5, PAD_LT = 1u << 6, PAD_RT = 1u << 7,
    PAD_UP = 1u << 8, PAD_DOWN = 1u << 9, PAD_LEFT = 1u << 10, PAD_RIGHT = 1u << 11,
    PAD_MENU = 1u << 12, PAD_VIEW = 1u << 13, PAD_L3 = 1u << 14, PAD_R3 = 1u << 15,
};

typedef struct rt_pad_state {
    bool connected;
    uint32_t buttons;
    float lx, ly, rx, ry, lt, rt; /* sticks -1..1 (y up), triggers 0..1 */
} rt_pad_state;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static rt_pad_state g_pub;  /* g_lock */
static bool g_capture;      /* g_lock: the managed menu has the stick, A and B */
static bool g_snap;         /* g_lock: the snap mode of the managed layer */
static NSString *g_glyph_names[PAD_GLYPHS]; /* g_lock: SF Symbols of the current controller */
static uint32_t g_glyph_gen;               /* g_lock: changes with g_glyph_names */

/* Main dispatch queue only. */
static dispatch_source_t g_timer;
static bool g_started;
static uint32_t g_prev;           /* buttons of the last sample */
static bool g_left_down, g_right_down;
static double g_frac_x, g_frac_y; /* cursor motion below one pixel */
static double g_repeat_at;        /* next wheel repeat, 0 = none */
static double g_last;             /* time of the last sample */
static bool g_share_prev;         /* the Share button in the last sample */
static double g_wheel_acc;        /* snap mode: wheel notches below one */
static __weak GCController *g_named; /* the controller of g_glyph_names */
static bool g_named_set;

static void pad_lock(void)
{
    if (pthread_mutex_lock(&g_lock) != 0) rt_fatal("gamepad: lock failed");
}

static void pad_unlock(void)
{
    if (pthread_mutex_unlock(&g_lock) != 0) rt_fatal("gamepad: unlock failed");
}

static double now_s(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) rt_fatal("clock_gettime failed");
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static float stick(float v)
{
    double a = fabs((double)v), r;
    if (!isfinite(a) || a <= PAD_DEAD_ZONE) return 0.0f;
    r = (fmin(a, 1.0) - PAD_DEAD_ZONE) / (1.0 - PAD_DEAD_ZONE);
    return (float)(v < 0 ? -r : r);
}

static bool pressed(GCControllerButtonInput *b) { return b != nil && b.pressed; }

/* Release the mouse buttons that the controller holds. */
static void release_mouse(void)
{
    if (g_left_down) rt_user32_pad_button(0, false);
    if (g_right_down) rt_user32_pad_button(1, false);
    g_left_down = g_right_down = false;
}

static NSString *symbol_of(GCControllerElement *e)
{
    NSString *n = e != nil ? e.sfSymbolsName : nil;
    return n != nil && n.length > 0 ? [n copy] : nil;
}

/* Read the SF Symbols names of the controls of the current controller
   when the controller changes (a controller with other glyphs, or none). */
static void refresh_glyph_names(GCController *c, GCExtendedGamepad *p)
{
    NSString *n[PAD_GLYPHS];
    unsigned i;
    bool same = true;
    if (g_named_set && g_named == c) return;
    g_named = c;
    g_named_set = true;
    memset(n, 0, sizeof n);
    if (p != nil)
    {
        n[0] = symbol_of(p.buttonA);
        n[1] = symbol_of(p.buttonB);
        n[2] = symbol_of(p.buttonX);
        n[3] = symbol_of(p.buttonY);
        n[4] = symbol_of(p.leftShoulder);
        n[5] = symbol_of(p.rightShoulder);
        n[6] = symbol_of(p.leftTrigger);
        n[7] = symbol_of(p.rightTrigger);
        n[8] = symbol_of(p.dpad.up);
        n[9] = symbol_of(p.dpad.down);
        n[10] = symbol_of(p.dpad.left);
        n[11] = symbol_of(p.dpad.right);
        n[12] = symbol_of(p.buttonMenu);
        n[13] = symbol_of(p.buttonOptions);
        n[14] = symbol_of(p.leftThumbstickButton);
        n[15] = symbol_of(p.rightThumbstickButton);
        n[16] = symbol_of(p.leftThumbstick);
        n[17] = symbol_of(p.rightThumbstick);
        n[18] = symbol_of(p.dpad);
    }
    pad_lock();
    for (i = 0; i < PAD_GLYPHS; i++)
    {
        if (!(n[i] == g_glyph_names[i] || (n[i] != nil && [n[i] isEqualToString:g_glyph_names[i]]))) same = false;
        g_glyph_names[i] = n[i];
    }
    if (!same) g_glyph_gen++;
    pad_unlock();
}

static void sample(void)
{
    GCController *c = GCController.current;
    GCExtendedGamepad *p = c != nil ? c.extendedGamepad : nil;
    rt_pad_state s;
    uint32_t b = 0, changed;
    double t = now_s(), dt;
    bool capture, snap;
    memset(&s, 0, sizeof s);
    /* An inactive application gets no controller input: the last values
       would hold the buttons down. */
    if (!rt_user32_app_is_active()) p = nil;
    refresh_glyph_names(c, c != nil ? c.extendedGamepad : nil);
    if (p != nil)
    {
        s.connected = true;
        if (pressed(p.buttonA)) b |= PAD_A;
        if (pressed(p.buttonB)) b |= PAD_B;
        if (pressed(p.buttonX)) b |= PAD_X;
        if (pressed(p.buttonY)) b |= PAD_Y;
        if (pressed(p.leftShoulder)) b |= PAD_LB;
        if (pressed(p.rightShoulder)) b |= PAD_RB;
        if (pressed(p.leftTrigger)) b |= PAD_LT;
        if (pressed(p.rightTrigger)) b |= PAD_RT;
        if (pressed(p.dpad.up)) b |= PAD_UP;
        if (pressed(p.dpad.down)) b |= PAD_DOWN;
        if (pressed(p.dpad.left)) b |= PAD_LEFT;
        if (pressed(p.dpad.right)) b |= PAD_RIGHT;
        if (pressed(p.buttonMenu)) b |= PAD_MENU;
        if (pressed(p.buttonOptions)) b |= PAD_VIEW;
        if (pressed(p.leftThumbstickButton)) b |= PAD_L3;
        if (pressed(p.rightThumbstickButton)) b |= PAD_R3;
        s.buttons = b;
        s.lx = stick(p.leftThumbstick.xAxis.value);
        s.ly = stick(p.leftThumbstick.yAxis.value);
        s.rx = stick(p.rightThumbstick.xAxis.value);
        s.ry = stick(p.rightThumbstick.yAxis.value);
        s.lt = p.leftTrigger.value;
        s.rt = p.rightTrigger.value;
    }
    /* The Share button (PlayStation Create, Xbox Share, Switch Capture)
       saves a screenshot, like F12. The extended gamepad profile has no
       such button, so it comes from the physical input profile. When macOS
       binds the button to its own screenshot gesture, the system takes the
       screenshot and the port does not take a second one. */
    {
        GCControllerButtonInput *sb = p != nil ? c.physicalInputProfile.buttons[GCInputButtonShare] : nil;
        bool share = sb != nil && !sb.boundToSystemGesture && sb.pressed;
        if (share && !g_share_prev) rt_d3d9_request_screenshot();
        g_share_prev = share;
    }
    pad_lock();
    g_pub = s;
    capture = g_capture;
    snap = g_snap;
    pad_unlock();

    dt = g_last > 0.0 ? fmin(t - g_last, 0.1) : 0.0;
    g_last = t;
    changed = b ^ g_prev;
    g_prev = b;
    if (capture || !s.connected)
    {
        release_mouse();
        g_frac_x = g_frac_y = 0.0;
    }
    else
    {
        if ((s.lx != 0.0f || s.ly != 0.0f) && dt > 0.0)
        {
            /* The speed in pixels of the 1080-line reference; the window
               scales it (rt_user32_pad_move limits the position). */
            double speed = PAD_CURSOR_SPEED * 1080.0 * dt;
            double mx = (double)s.lx * fabs((double)s.lx) * speed, my = -(double)s.ly * fabs((double)s.ly) * speed;
            int32_t ix, iy;
            g_frac_x += mx;
            g_frac_y += my;
            ix = (int32_t)trunc(g_frac_x);
            iy = (int32_t)trunc(g_frac_y);
            g_frac_x -= ix;
            g_frac_y -= iy;
            if (ix != 0 || iy != 0) rt_user32_pad_move(ix, iy);
        }
        else
            g_frac_x = g_frac_y = 0.0;
        if ((b & PAD_A) != 0 && !g_left_down)
        {
            g_left_down = true;
            rt_user32_pad_button(0, true);
        }
        else if ((b & PAD_A) == 0 && g_left_down)
        {
            g_left_down = false;
            rt_user32_pad_button(0, false);
        }
        if ((b & PAD_B) != 0 && !g_right_down)
        {
            g_right_down = true;
            rt_user32_pad_button(1, true);
        }
        else if ((b & PAD_B) == 0 && g_right_down)
        {
            g_right_down = false;
            rt_user32_pad_button(1, false);
        }
    }
    /* Snap mode: the managed layer moves the cursor with the directional
       pad, and the right stick turns the wheel (lists in the windows). */
    if (snap)
    {
        g_repeat_at = 0.0;
        if (capture || s.ry == 0.0f || dt <= 0.0)
            g_wheel_acc = 0.0;
        else
        {
            g_wheel_acc += (double)s.ry * fabs((double)s.ry) * PAD_SNAP_WHEEL_RATE * dt;
            while (g_wheel_acc >= 1.0)
            {
                rt_user32_pad_wheel(120);
                g_wheel_acc -= 1.0;
            }
            while (g_wheel_acc <= -1.0)
            {
                rt_user32_pad_wheel(-120);
                g_wheel_acc += 1.0;
            }
        }
        return;
    }
    g_wheel_acc = 0.0;
    /* The wheel works in the menu too (it does nothing there). */
    if (changed & b & (PAD_UP | PAD_DOWN))
    {
        rt_user32_pad_wheel((b & PAD_UP) ? 120 : -120);
        g_repeat_at = t + PAD_REPEAT_DELAY;
    }
    else if ((b & (PAD_UP | PAD_DOWN)) == 0)
        g_repeat_at = 0.0;
    else if (g_repeat_at > 0.0 && t >= g_repeat_at)
    {
        rt_user32_pad_wheel((b & PAD_UP) ? 120 : -120);
        g_repeat_at = t + PAD_REPEAT_STEP;
    }
}

static void timer_event(void *ctx)
{
    (void)ctx;
    @autoreleasepool
    {
        sample();
    }
}

/* Start or stop the timer for the connected controllers. */
static void update_timer(void)
{
    bool any = GCController.controllers.count > 0;
    if (any && g_timer == nil)
    {
        uint64_t ns = (uint64_t)(1e9 / RT_PAD_HZ);
        g_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        if (g_timer == nil) rt_fatal("gamepad: the timer cannot be made");
        dispatch_source_set_timer(g_timer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)ns), ns, ns / 10u);
        dispatch_source_set_event_handler_f(g_timer, timer_event);
        dispatch_resume(g_timer);
        g_last = 0.0;
    }
    else if (!any && g_timer != nil)
    {
        dispatch_source_cancel(g_timer);
        g_timer = nil;
        /* The last sample with no controller releases what it held. */
        sample();
        g_prev = 0;
        g_repeat_at = 0.0;
    }
}

void rt_gamepad_init(void)
{
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    if (!NSThread.isMainThread) rt_fatal("rt_gamepad_init: not on the main thread");
    if (g_started) rt_fatal("rt_gamepad_init: called a second time");
    g_started = true;
    [nc addObserverForName:GCControllerDidConnectNotification
                    object:nil
                     queue:NSOperationQueue.mainQueue
                usingBlock:^(NSNotification *n) {
                  (void)n;
                  update_timer();
                }];
    [nc addObserverForName:GCControllerDidDisconnectNotification
                    object:nil
                     queue:NSOperationQueue.mainQueue
                usingBlock:^(NSNotification *n) {
                  (void)n;
                  update_timer();
                }];
    update_timer();
}

/* ---------------------------------------------------------------- portgfx exports */

/* int __stdcall PortPadRead(float *axes): -1 if no controller is used, else
   the PAD_* bits; axes[0..5] = left x, left y, right x, right y, left
   trigger, right trigger (y up). */
void rt_PortPadRead(_cpu *cpu)
{
    uint32_t g = ARG(0);
    rt_pad_state s;
    float a[6];
    unsigned i;
    if (g == 0) rt_fatal("PortPadRead: no array");
    pad_lock();
    s = g_pub;
    pad_unlock();
    a[0] = s.lx;
    a[1] = s.ly;
    a[2] = s.rx;
    a[3] = s.ry;
    a[4] = s.lt;
    a[5] = s.rt;
    for (i = 0; i < 6; i++)
    {
        uint32_t v;
        memcpy(&v, &a[i], 4);
        wr32(g + 4u * i, v);
    }
    RET(s.connected ? s.buttons : 0xFFFFFFFFu);
}

/* void __stdcall PortPadCapture(int on): the managed menu takes (1) or
   gives back (0) the left stick, A and B. */
void rt_PortPadCapture(_cpu *cpu)
{
    uint32_t on = ARG(0);
    if (on > 1u) rt_fatal("PortPadCapture: %u is not 0 or 1", on);
    pad_lock();
    g_capture = on != 0;
    pad_unlock();
}

/* void __stdcall PortPadSnap(int on): the managed layer moves the cursor
   between the controls of a window with the directional pad (1), or gives
   the directional pad back to the mouse wheel (0). In the snap mode the
   right stick turns the wheel. */
void rt_PortPadSnap(_cpu *cpu)
{
    uint32_t on = ARG(0);
    if (on > 1u) rt_fatal("PortPadSnap: %u is not 0 or 1", on);
    pad_lock();
    g_snap = on != 0;
    pad_unlock();
}

typedef struct warp_job {
    int32_t x, y, w, h;
} warp_job;

static void warp_job_run(void *ctx)
{
    warp_job *j = ctx;
    rt_user32_pad_warp(j->x, j->y, j->w, j->h);
    free(j);
}

/* void __stdcall PortPadWarp(int x, int y, int width, int height): put the
   cursor at (x, y) of the game's frame of width x height pixels (origin top
   left), on the main thread. */
void rt_PortPadWarp(_cpu *cpu)
{
    int32_t x = (int32_t)ARG(0), y = (int32_t)ARG(1), w = (int32_t)ARG(2), h = (int32_t)ARG(3);
    warp_job *j;
    if (w <= 0 || h <= 0 || w > 65536 || h > 65536) rt_fatal("PortPadWarp: the frame size %dx%d", w, h);
    j = malloc(sizeof *j);
    if (j == NULL) rt_fatal("PortPadWarp: out of memory");
    j->x = x;
    j->y = y;
    j->w = w;
    j->h = h;
    dispatch_async_f(dispatch_get_main_queue(), j, warp_job_run);
}

/* int __stdcall PortPadGlyphs(void): a number that changes when the
   glyphs of PortPadGlyph change (another controller, or none). */
void rt_PortPadGlyphs(_cpu *cpu)
{
    uint32_t g;
    pad_lock();
    g = g_glyph_gen;
    pad_unlock();
    RET(g);
}

/* int __stdcall PortPadGlyph(int glyph, int size, Color32 *pixels): draw
   the SF Symbols glyph of a control of the current controller (0..15 the
   buttons of the PAD_* bits, 16 the left stick, 17 the right stick, 18 the
   directional pad) in white with alpha, centered in size x size pixels,
   rows from the bottom (Texture2D.SetPixels32). Returns 1, or 0 if the
   controller gives no glyph for the control (pixels are then
   transparent). */
void rt_PortPadGlyph(_cpu *cpu)
{
    uint32_t id = ARG(0), size = ARG(1), g = ARG(2), y;
    size_t px, row;
    NSString *name;
    NSImage *img;
    CGColorSpaceRef cs;
    CGContextRef cx;
    uint8_t *bits;
    int rc = 0;
    if (id >= PAD_GLYPHS) rt_fatal("PortPadGlyph: glyph %u", id);
    if (size < PAD_GLYPH_MIN || size > PAD_GLYPH_MAX) rt_fatal("PortPadGlyph: size %u", size);
    if (g == 0 || (uint64_t)g + (uint64_t)size * size * 4u > 0x100000000ull) rt_fatal("PortPadGlyph: pixels at 0x%08x", g);
    px = size;
    row = px * 4u;
    pad_lock();
    name = g_glyph_names[id];
    pad_unlock();
    cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    if (cs == NULL) rt_fatal("PortPadGlyph: no sRGB color space");
    cx = CGBitmapContextCreate(NULL, px, px, 8, row, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(cs);
    if (cx == NULL) rt_fatal("PortPadGlyph: the bitmap cannot be made");
    CGContextClearRect(cx, CGRectMake(0, 0, (CGFloat)px, (CGFloat)px));
    @autoreleasepool
    {
        img = name != nil ? [NSImage imageWithSystemSymbolName:name accessibilityDescription:nil] : nil;
        if (img != nil)
        {
            NSImageSymbolConfiguration *cfg =
                [NSImageSymbolConfiguration configurationWithPointSize:(CGFloat)px weight:NSFontWeightSemibold];
            NSImage *sized = [img imageWithSymbolConfiguration:cfg];
            NSSize sz = sized != nil ? sized.size : NSZeroSize;
            if (sized != nil && sz.width > 0 && sz.height > 0)
            {
                /* Fit the glyph into the square, 1 pixel inside the edge. */
                CGFloat box = (CGFloat)px - 2, k = fmin(box / sz.width, box / sz.height);
                NSRect r = NSMakeRect(((CGFloat)px - sz.width * k) / 2, ((CGFloat)px - sz.height * k) / 2, sz.width * k,
                                      sz.height * k);
                NSGraphicsContext *gc = [NSGraphicsContext graphicsContextWithCGContext:cx flipped:NO];
                [NSGraphicsContext saveGraphicsState];
                NSGraphicsContext.currentContext = gc;
                [sized drawInRect:r fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1.0];
                [NSGraphicsContext restoreGraphicsState];
                rc = 1;
            }
        }
    }
    bits = CGBitmapContextGetData(cx);
    if (bits == NULL) rt_fatal("PortPadGlyph: the bitmap has no data");
    /* White with the alpha of the glyph; the bitmap's first row is the top
       row, the first row of the texture is the bottom row. */
    for (y = 0; y < size; y++)
    {
        const uint8_t *src = bits + (px - 1u - y) * row;
        uint8_t line[PAD_GLYPH_MAX * 4];
        size_t x;
        for (x = 0; x < px; x++)
        {
            uint8_t a = src[4u * x + 3u];
            line[4u * x] = 255;
            line[4u * x + 1u] = 255;
            line[4u * x + 2u] = 255;
            line[4u * x + 3u] = a;
        }
        memcpy(G2H(g + y * (uint32_t)row), line, row);
    }
    CGContextRelease(cx);
    RET((uint32_t)rc);
}

/* Unity KeyCode (UnityEngine.KeyCode of Unity 4) to the macOS virtual key
   code of the same key on a US keyboard; -1 if the Mac has no such key. */
static int mac_key_of_unity(uint32_t k)
{
    static const char letters[26] = { 0x00, 0x0B, 0x08, 0x02, 0x0E, 0x03, 0x05, 0x04, 0x22, 0x26, 0x28, 0x25, 0x2E,
                                      0x2D, 0x1F, 0x23, 0x0C, 0x0F, 0x01, 0x11, 0x20, 0x09, 0x0D, 0x07, 0x10, 0x06 };
    static const char digits[10] = { 0x1D, 0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19 };
    static const char keypad[10] = { 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5B, 0x5C };
    static const char fkeys[15] = { 0x7A, 0x78, 0x63, 0x76, 0x60, 0x61, 0x62, 0x64, 0x65, 0x6D, 0x67, 0x6F, 0x69, 0x6B, 0x71 };
    if (k >= 97 && k <= 122) return letters[k - 97];
    if (k >= 48 && k <= 57) return digits[k - 48];
    if (k >= 256 && k <= 265) return keypad[k - 256];
    if (k >= 282 && k <= 296) return fkeys[k - 282];
    switch (k)
    {
    case 8: return 0x33;   /* Backspace */
    case 9: return 0x30;   /* Tab */
    case 13: return 0x24;  /* Return */
    case 27: return 0x35;  /* Escape */
    case 32: return 0x31;  /* Space */
    case 39: return 0x27;  /* Quote */
    case 44: return 0x2B;  /* Comma */
    case 45: return 0x1B;  /* Minus */
    case 46: return 0x2F;  /* Period */
    case 47: return 0x2C;  /* Slash */
    case 59: return 0x29;  /* Semicolon */
    case 61: return 0x18;  /* Equals */
    case 91: return 0x21;  /* LeftBracket */
    case 92: return 0x2A;  /* Backslash */
    case 93: return 0x1E;  /* RightBracket */
    case 96: return 0x32;  /* BackQuote */
    case 127: return 0x75; /* Delete */
    case 266: return 0x41; /* KeypadPeriod */
    case 267: return 0x4B; /* KeypadDivide */
    case 268: return 0x43; /* KeypadMultiply */
    case 269: return 0x4E; /* KeypadMinus */
    case 270: return 0x45; /* KeypadPlus */
    case 271: return 0x4C; /* KeypadEnter */
    case 272: return 0x51; /* KeypadEquals */
    case 273: return 0x7E; /* UpArrow */
    case 274: return 0x7D; /* DownArrow */
    case 275: return 0x7C; /* RightArrow */
    case 276: return 0x7B; /* LeftArrow */
    case 277: return 0x72; /* Insert (Help) */
    case 278: return 0x73; /* Home */
    case 279: return 0x77; /* End */
    case 280: return 0x74; /* PageUp */
    case 281: return 0x79; /* PageDown */
    case 301: return 0x39; /* CapsLock */
    case 303: return 0x3C; /* RightShift */
    case 304: return 0x38; /* LeftShift */
    case 305: return 0x3E; /* RightControl */
    case 306: return 0x3B; /* LeftControl */
    case 307: return 0x3D; /* RightAlt */
    case 308: return 0x3A; /* LeftAlt */
    default: return -1;
    }
}

typedef struct key_job {
    int kind; /* 0 key, 1 mouse button */
    int code;
    bool down;
} key_job;

static void key_job_run(void *ctx)
{
    key_job *j = ctx;
    if (j->kind == 0)
        rt_user32_pad_key((uint16_t)j->code, j->down);
    else
        rt_user32_pad_button(j->code, j->down);
    free(j);
}

/* int __stdcall PortPadKey(int unityKeyCode, int down): press or release
   the key (or the mouse button Mouse0 to Mouse2) on the main thread.
   Returns 1, or 0 if the Mac has no such key. */
void rt_PortPadKey(_cpu *cpu)
{
    uint32_t k = ARG(0), down = ARG(1);
    key_job *j;
    int code, kind = 0;
    if (down > 1u) rt_fatal("PortPadKey: %u is not 0 or 1", down);
    if (k >= 323 && k <= 325)
    {
        kind = 1;
        code = (int)(k - 323); /* Mouse0 left, Mouse1 right, Mouse2 middle */
    }
    else
        code = mac_key_of_unity(k);
    if (code < 0)
    {
        RET(0);
        return;
    }
    j = malloc(sizeof *j);
    if (j == NULL) rt_fatal("PortPadKey: out of memory");
    j->kind = kind;
    j->code = code;
    j->down = down != 0;
    dispatch_async_f(dispatch_get_main_queue(), j, key_job_run);
    RET(1);
}
