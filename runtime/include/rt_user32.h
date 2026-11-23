/*
 * USER32 internal interface. rt_user32.c keeps the windows, classes and
 * message queues; rt_dialog.c (dialogs and the system control classes),
 * rt_gdi32.c, rt_imm32.c and rt_window.m use the functions here.
 *
 * Window procedures:
 *   - A window of a guest class has a guest window procedure.
 *   - A window of a runtime class (dialogs, controls) has a host window
 *     procedure (rt_host_proc). unicode tells which form of a message the
 *     sender used (SendMessageA or SendMessageW).
 *
 * Messages that Windows sends (SendMessage semantics) call the window
 * procedure directly. Only the thread that owns a window can send a
 * message to it; a send from another thread stops the process.
 */
#ifndef RT_USER32_H
#define RT_USER32_H

#include "rt.h"
#include "rt_window.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------- messages */

#define WM_NULL 0x0000u
#define WM_CREATE 0x0001u
#define WM_DESTROY 0x0002u
#define WM_MOVE 0x0003u
#define WM_SIZE 0x0005u
#define WM_ACTIVATE 0x0006u
#define WM_SETFOCUS 0x0007u
#define WM_KILLFOCUS 0x0008u
#define WM_ENABLE 0x000Au
#define WM_SETREDRAW 0x000Bu
#define WM_SETTEXT 0x000Cu
#define WM_GETTEXT 0x000Du
#define WM_GETTEXTLENGTH 0x000Eu
#define WM_PAINT 0x000Fu
#define WM_CLOSE 0x0010u
#define WM_QUERYENDSESSION 0x0011u
#define WM_QUIT 0x0012u
#define WM_QUERYOPEN 0x0013u
#define WM_ERASEBKGND 0x0014u
#define WM_SHOWWINDOW 0x0018u
#define WM_ACTIVATEAPP 0x001Cu
#define WM_SETCURSOR 0x0020u
#define WM_MOUSEACTIVATE 0x0021u
#define WM_GETMINMAXINFO 0x0024u
#define WM_NEXTDLGCTL 0x0028u
#define WM_SETFONT 0x0030u
#define WM_GETFONT 0x0031u
#define WM_WINDOWPOSCHANGING 0x0046u
#define WM_WINDOWPOSCHANGED 0x0047u
#define WM_COPYDATA 0x004Au
#define WM_NOTIFY 0x004Eu
#define WM_INPUTLANGCHANGE 0x0051u
#define WM_CONTEXTMENU 0x007Bu
#define WM_STYLECHANGING 0x007Cu
#define WM_STYLECHANGED 0x007Du
#define WM_GETICON 0x007Fu
#define WM_SETICON 0x0080u
#define WM_NCCREATE 0x0081u
#define WM_NCDESTROY 0x0082u
#define WM_NCCALCSIZE 0x0083u
#define WM_NCHITTEST 0x0084u
#define WM_NCPAINT 0x0085u
#define WM_NCACTIVATE 0x0086u
#define WM_GETDLGCODE 0x0087u
#define WM_INPUT_DEVICE_CHANGE 0x00FEu
#define WM_INPUT 0x00FFu
#define WM_KEYDOWN 0x0100u
#define WM_KEYUP 0x0101u
#define WM_CHAR 0x0102u
#define WM_DEADCHAR 0x0103u
#define WM_SYSKEYDOWN 0x0104u
#define WM_SYSKEYUP 0x0105u
#define WM_SYSCHAR 0x0106u
#define WM_SYSDEADCHAR 0x0107u
#define WM_UNICHAR 0x0109u
#define WM_IME_STARTCOMPOSITION 0x010Du
#define WM_IME_ENDCOMPOSITION 0x010Eu
#define WM_IME_COMPOSITION 0x010Fu
#define WM_INITDIALOG 0x0110u
#define WM_COMMAND 0x0111u
#define WM_SYSCOMMAND 0x0112u
#define WM_TIMER 0x0113u
#define WM_CHANGEUISTATE 0x0127u
#define WM_UPDATEUISTATE 0x0128u
#define WM_QUERYUISTATE 0x0129u
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
#define WM_ENTERMENULOOP 0x0211u
#define WM_CAPTURECHANGED 0x0215u
#define WM_DEVICECHANGE 0x0219u
#define WM_ENTERSIZEMOVE 0x0231u
#define WM_EXITSIZEMOVE 0x0232u
#define WM_IME_SETCONTEXT 0x0281u
#define WM_IME_NOTIFY 0x0282u
#define WM_IME_CHAR 0x0286u
#define WM_IME_REQUEST 0x0288u
#define WM_IME_KEYDOWN 0x0290u
#define WM_IME_KEYUP 0x0291u
#define WM_MOUSELEAVE 0x02A3u
#define WM_USER 0x0400u

#define HTCLIENT 1u
#define MA_ACTIVATE 1u

/* Window styles. */
#define WS_OVERLAPPED 0x00000000u
#define WS_TABSTOP 0x00010000u
#define WS_MAXIMIZEBOX 0x00010000u
#define WS_GROUP 0x00020000u
#define WS_MINIMIZEBOX 0x00020000u
#define WS_THICKFRAME 0x00040000u
#define WS_SYSMENU 0x00080000u
#define WS_HSCROLL 0x00100000u
#define WS_VSCROLL 0x00200000u
#define WS_DLGFRAME 0x00400000u
#define WS_BORDER 0x00800000u
#define WS_CAPTION 0x00C00000u
#define WS_MAXIMIZE 0x01000000u
#define WS_CLIPCHILDREN 0x02000000u
#define WS_CLIPSIBLINGS 0x04000000u
#define WS_DISABLED 0x08000000u
#define WS_VISIBLE 0x10000000u
#define WS_MINIMIZE 0x20000000u
#define WS_CHILD 0x40000000u
#define WS_POPUP 0x80000000u
#define WS_EX_DLGMODALFRAME 0x00000001u
#define WS_EX_NOPARENTNOTIFY 0x00000004u
#define WS_EX_TOPMOST 0x00000008u
#define WS_EX_ACCEPTFILES 0x00000010u
#define WS_EX_TRANSPARENT 0x00000020u
#define WS_EX_TOOLWINDOW 0x00000080u
#define WS_EX_WINDOWEDGE 0x00000100u
#define WS_EX_CLIENTEDGE 0x00000200u
#define WS_EX_CONTEXTHELP 0x00000400u
#define WS_EX_CONTROLPARENT 0x00010000u
#define WS_EX_STATICEDGE 0x00020000u
#define WS_EX_APPWINDOW 0x00040000u

/* Window procedure of a runtime class. */
typedef uint32_t (*rt_host_proc)(_cpu *cpu, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp, bool unicode);

/* Register a runtime window class (system class: any module can use it). */
void rt_user32_register_runtime_class(const char *name, rt_host_proc proc, uint32_t wnd_extra);

/* Make a window. cls: UTF-8 class name. title: UTF-8 or NULL. id: the
   control id of a child window. title_res: a resource id given as the
   title (0 if none). Returns the window handle, or 0 (the last error is
   set). */
uint32_t rt_user32_create(_cpu *cpu, uint32_t exstyle, const char *cls, const char *title, uint32_t style,
                          int32_t x, int32_t y, int32_t w, int32_t h, uint32_t parent, uint32_t id,
                          uint32_t hinst, uint32_t param, bool unicode);
bool rt_user32_destroy(_cpu *cpu, uint32_t hwnd);

/* Send a message (SendMessage; HWND_BROADCAST: all top-level windows).
   Returns 0 if hwnd is not a window. */
uint32_t rt_user32_send(_cpu *cpu, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp, bool unicode);
/* DefWindowProcA or DefWindowProcW. */
uint32_t rt_user32_default_proc(_cpu *cpu, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp, bool unicode);

/* GetMessage semantics for the dialog loop: wait for a message for any
   window of the calling thread, remove it and write the MSG at pmsg.
   Returns false for WM_QUIT. */
bool rt_user32_get_message(_cpu *cpu, uint32_t pmsg, bool unicode);
void rt_user32_translate(_cpu *cpu, uint32_t pmsg);
uint32_t rt_user32_dispatch(_cpu *cpu, uint32_t pmsg, bool unicode);
/* PostQuitMessage for the calling thread. */
void rt_user32_post_quit(uint32_t code);

/* Window state. All functions return 0 or false for a handle that is not
   a window. */
bool rt_user32_is_window(uint32_t hwnd);
uint32_t rt_user32_parent(uint32_t hwnd);  /* child: the parent; else 0 */
uint32_t rt_user32_style(uint32_t hwnd);
uint32_t rt_user32_id(uint32_t hwnd);
bool rt_user32_is_unicode(uint32_t hwnd);
/* The children of parent in creation order. Returns the number (at most
   max, at most 256, are written; more children stop the process). */
int rt_user32_children(uint32_t parent, uint32_t *out, int max);
/* The first child of parent with control id id, or 0. */
uint32_t rt_user32_child_by_id(uint32_t parent, uint32_t id);
/* True if hwnd is parent or a descendant of parent. */
bool rt_user32_is_self_or_child(uint32_t parent, uint32_t hwnd);
/* The class data of a runtime class window (owned by the class). */
void *rt_user32_class_data(uint32_t hwnd);
void rt_user32_set_class_data(uint32_t hwnd, void *data);
/* The native view of a child window, or the content view of a top-level
   window (NULL if it has no native object yet). Main thread. */
void *rt_user32_native_view(uint32_t hwnd);
/* The window text (UTF-8, malloc; the caller frees it). */
char *rt_user32_text(uint32_t hwnd);
/* Client rectangle (client coordinates: left = top = 0). */
rt_rect rt_user32_client_rect(uint32_t hwnd);
/* The window rectangle in screen coordinates. */
rt_rect rt_user32_screen_rect(uint32_t hwnd);
/* Show or hide (ShowWindow), enable (EnableWindow) and focus. */
bool rt_user32_show(_cpu *cpu, uint32_t hwnd, int cmd);
bool rt_user32_enable(_cpu *cpu, uint32_t hwnd, bool enable);
uint32_t rt_user32_set_focus(_cpu *cpu, uint32_t hwnd);
uint32_t rt_user32_focus(void);
/* The extra window bytes (GetWindowLong with an index >= 0). */
bool rt_user32_get_extra(uint32_t hwnd, uint32_t offset, uint32_t *value);
bool rt_user32_set_extra(uint32_t hwnd, uint32_t offset, uint32_t value);
/* The window procedure field (the DLGPROC of a dialog is in its extra
   bytes, not here). */

/* ---------------------------------------------------------------- native events (main thread) */

/* chars: the characters of a key-down (UTF-16; TranslateMessage gives
   them as WM_CHAR). ime: the input method took the key (VK_PROCESSKEY). */
void rt_user32_on_key(uint32_t hwnd, bool down, bool is_repeat, uint16_t mac_keycode, const uint16_t *chars, int nchars,
                      bool ime);
void rt_user32_on_modifiers(uint32_t hwnd, uint32_t mac_flags);
/* msg: WM_MOUSEMOVE, WM_[LRMX]BUTTON(DOWN|UP|DBLCLK), WM_MOUSE(H)WHEEL.
   x, y: client coordinates of hwnd. data: the wheel delta (120 for one
   notch) or the X button (1 or 2). */
void rt_user32_on_mouse(uint32_t hwnd, uint32_t msg, int32_t x, int32_t y, int32_t data);
/* Relative mouse motion (raw input, rt_rawinput.c). */
void rt_user32_on_mouse_motion(double dx, double dy);
void rt_user32_on_active(uint32_t hwnd, bool active);
void rt_user32_on_app_active(bool active);
/* The application is the active application (any thread). */
bool rt_user32_app_is_active(void);
void rt_user32_on_close(uint32_t hwnd);
void rt_user32_on_frame(uint32_t hwnd, bool resized);
void rt_user32_on_minimized(uint32_t hwnd, bool minimized);
void rt_user32_on_quit(void);
/* Input method: text that is being composed (marked text, UTF-16) and
   the text that the input method gives as the result. */
void rt_user32_on_ime_composition(uint32_t hwnd, const uint16_t *text, int n, int caret);
void rt_user32_on_ime_result(uint32_t hwnd, const uint16_t *text, int n);
/* A native control event (rt_dialog.c handles it at the next message
   check of the main thread). */
#define RT_CTL_CLICKED 1      /* button */
#define RT_CTL_SELCHANGE 2    /* drop-down list: a = new selection */
#define RT_CTL_TAB 3          /* tab strip: a = new tab */
#define RT_CTL_LIST_CLICK 4   /* list: a = row, b = column */
#define RT_CTL_LIST_DBLCLICK 5
#define RT_CTL_EDIT_CHANGE 6  /* edit field text changed */
#define RT_CTL_CANCEL 7       /* Escape in a dialog window */
void rt_user32_on_control(uint32_t hwnd, int event, int32_t a, int32_t b);

/* Run fn(cpu, ctx) on the main thread at its next message check. Any
   thread. */
void rt_user32_main_call(void (*fn)(_cpu *cpu, void *ctx), void *ctx);

/* Raw input (rt_rawinput.c). rt_user32.c calls these on the main thread
   for each native key and mouse event. */
void rt_rawinput_key(uint32_t vk, uint32_t scan, bool ext, bool down);
/* dx, dy: relative motion in points. buttons: RI_MOUSE_* flags. wheel: the
   wheel delta (120 for one notch) with RI_MOUSE_WHEEL or RI_MOUSE_HWHEEL. */
void rt_rawinput_mouse(double dx, double dy, uint32_t buttons, int32_t wheel);
/* For rt_rawinput.c: post WM_INPUT to a window of the main thread. */
void rt_user32_post_raw(uint32_t hwnd, uint32_t wparam, uint32_t lparam);
/* The window for raw input without a target: the focus or active window
   of the main thread (0 if none). */
uint32_t rt_user32_raw_target(void);
/* The cursor position of the last mouse event (screen coordinates). */
void rt_user32_mouse_pos(int32_t *x, int32_t *y);
/* Game controller input (rt_gamepad.m), main thread. The events go to the
   window that gets raw input (rt_user32_raw_target), at the cursor; they
   do nothing when the program has no such window.
   pad_move: move the cursor by (dx, dy) pixels, inside the client area of
   the window. pad_button: 0 left, 1 right, 2 middle. pad_wheel: one notch
   is 120. pad_key: a macOS virtual key code (kVK_*). */
void rt_user32_pad_move(int32_t dx, int32_t dy);
/* Put the cursor at the position (x, y) of a frame of sw x sh pixels
   (origin top left) that covers the client area of the target window; the
   position is limited to the client area. Main thread. */
void rt_user32_pad_warp(int32_t x, int32_t y, int32_t sw, int32_t sh);
void rt_user32_pad_button(int button, bool down);
void rt_user32_pad_wheel(int32_t delta);
void rt_user32_pad_key(uint16_t mac_keycode, bool down);
/* Headless mode (rt_user32_set_headless). */
bool rt_user32_headless(void);
/* Register the runtime classes; main thread, before guest code runs. */
void rt_user32_init(void);
/* Windows destroys the windows and the queue of a thread that ends. */
void rt_user32_thread_exit(uint32_t tid);
/* Call the guest window or dialog procedure proc (charset proc_unicode)
   with a message in the charset unicode of the sender; the text messages
   are converted. */
uint32_t rt_user32_call_proc(_cpu *cpu, uint32_t proc, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp, bool unicode,
                             bool proc_unicode);
/* A guest string (ANSI or wide) as UTF-8 (malloc; NULL for g = 0). The
   process stops for a string that is not valid. */
char *rt_user32_guest_text(uint32_t g, bool wide);
/* Runtime classes: set the window text without messages (UTF-8). */
void rt_user32_set_text(uint32_t hwnd, const char *text);
/* The window rectangle of a child in the client coordinates of its
   parent (screen coordinates for a top-level window). */
rt_rect rt_user32_child_rect(uint32_t hwnd);
/* The screen position of the client origin of hwnd (0, 0 if hwnd is not
   a window). */
void rt_user32_client_origin(uint32_t hwnd, int32_t *x, int32_t *y);
/* SetWindowPos (SWP_* flags). */
bool rt_user32_set_pos(_cpu *cpu, uint32_t hwnd, uint32_t after, int32_t x, int32_t y, int32_t cx, int32_t cy, uint32_t flags);
/* Runtime classes: set the native view of a child window. */
void rt_user32_set_native_view(uint32_t hwnd, void *view);
/* The CAMetalLayer of a window for the renderer. Makes the native window
   if it does not exist yet. Main thread. */
void *rt_user32_metal_layer(uint32_t hwnd);
/* The HMONITOR of the main display (the handle that MonitorFromWindow and
   GetMonitorInfoA use for it). */
uint32_t rt_user32_main_monitor(void);
/* Show the cursor of a cursor handle (WM_SETCURSOR, SetCursor;
   rt_user32_sys.c). Returns the previous cursor handle. */
uint32_t rt_user32_cursor_set(uint32_t hcursor);

/* Called by rt_dialog.c for a control event. */
void rt_dialog_control_event(_cpu *cpu, uint32_t hwnd, int event, int32_t a, int32_t b);
/* Register the dialog and control classes (rt_dialog.c). */
void rt_dialog_init(void);

/* Input contexts (rt_imm32.c). Every thread with a message queue has one
   input context (HIMC); a window has the context of its thread or none.
   The input method of a window is active while the window has the context
   and the context is open. */
/* The context of hwnd (0: none or not a window). */
uint32_t rt_user32_ime_context(uint32_t hwnd);
/* The context of the thread of hwnd (0: not a window). */
uint32_t rt_user32_ime_window_thread_context(uint32_t hwnd);
bool rt_user32_ime_valid(uint32_t himc);
/* Give hwnd the context himc (0: no context). Returns false if hwnd is not
   a window or himc is not the context of the thread of hwnd. */
bool rt_user32_ime_associate(uint32_t hwnd, uint32_t himc);
bool rt_user32_ime_get_open(uint32_t himc, bool *open);
/* Open or close a context. *changed: the state changed. Returns false for
   a handle that is not a context. */
bool rt_user32_ime_set_open(uint32_t himc, bool open, bool *changed);
/* The current composition and the last result of a context (UTF-16 in
   host memory owned by USER32; valid until the next USER32 call). Only
   the context of the main thread has native text input. */
void rt_user32_ime_strings(uint32_t himc, const uint16_t **comp, int *ncomp, int *caret,
                           const uint16_t **result, int *nresult);
/* Discard the current composition of a context (main thread). */
void rt_user32_ime_cancel(uint32_t himc);
/* The user selected another keyboard input source (rt_window.m, main
   thread): rt_imm32.c opens the context of the main thread and tells the
   focus window (WM_IME_NOTIFY). */
void rt_imm32_input_source_changed(void);

/* ---------------------------------------------------------------- icons, cursors and devices (rt_user32_sys.c) */

/* Make a runtime icon or cursor from RGBA pixels (straight alpha, row 0
   on top). Returns the handle or 0 when the table is full. */
uint32_t rt_user32_icon_create(const uint8_t *rgba, uint32_t w, uint32_t h, bool cursor, int32_t hot_x, int32_t hot_y);
/* The pixels of an icon: rgba is malloc (the caller frees it). A system
   icon gives *system_id (IDI_*) and no pixels. Returns false if h is not
   an icon. */
bool rt_user32_icon_pixels(uint32_t h, uint8_t **rgba, uint32_t *w, uint32_t *hgt, uint32_t *system_id);
/* LoadIconA (a shared image) and a bitmap resource (LoadImageA) of a
   module, for the dialog manager. Return 0 with the last error set. */
uint32_t rt_user32_load_icon(uint32_t module, uint32_t name);
uint32_t rt_user32_load_bitmap(uint32_t module, uint32_t name);
/* A device interface arrived or was removed (rt_hid.c; any thread). The
   windows that RegisterDeviceNotificationW registered get WM_DEVICECHANGE
   on the main thread. guid: the interface class. name: UTF-8 path. */
void rt_user32_device_interface_event(bool arrival, const uint8_t guid[16], const char *name);

/* ---------------------------------------------------------------- GDI (rt_gdi32.c) */

/* A bitmap object from RGBA pixels (row 0 on top). Returns the handle or
   0 (no handle or guest memory left). */
uint32_t rt_gdi_bitmap_from_rgba(const uint8_t *rgba, uint32_t w, uint32_t h);
/* The pixels of a bitmap as RGBA (malloc; the caller frees it). A 1-bit
   bitmap gives 0 or 255 in every channel. Returns false if h is not a
   bitmap. *has_alpha: the bitmap has an alpha channel (32 bits). */
bool rt_gdi_bitmap_rgba(uint32_t h, uint8_t **rgba, uint32_t *w, uint32_t *hgt, bool *has_alpha);
/* A bitmap from a DIB: p[0..n) has the header and the color table, bits
   the pixel rows. dib_section: a DIB section (bits in guest memory), else
   a 32-bit device-dependent bitmap. Returns 0 for a bitmap that is not
   valid or when no memory is left. */
uint32_t rt_gdi_bitmap_from_dib(const uint8_t *p, size_t n, const uint8_t *bits, size_t nbits, bool dib_section);
/* Decode a DIB (header, color table, pixel rows) or a PNG image to RGBA
   (malloc, row 0 on top, straight alpha). icon: an icon or cursor image
   (double height with an AND mask). Returns false for data that is not
   valid. */
bool rt_image_decode(const uint8_t *p, size_t n, bool icon, uint8_t **rgba, uint32_t *w, uint32_t *h, bool *has_alpha);
/* The font object of the dialog font (one shared object). */
uint32_t rt_gdi_dialog_font(void);

/* ---------------------------------------------------------------- resources (rt_resource.c) */

#define RT_RES_CURSOR 1u
#define RT_RES_BITMAP 2u
#define RT_RES_ICON 3u
#define RT_RES_DIALOG 5u
#define RT_RES_GROUP_CURSOR 12u
#define RT_RES_GROUP_ICON 14u

/* Find a resource of the module (a mapped PE image) by type and name.
   type and name: an integer id (< 0x10000) or a guest string address
   (wide if wide). The first language is used. Returns the guest address
   of the data and its size, or false if the module has no such resource. */
bool rt_resource_find(uint32_t module, uint32_t type, uint32_t name, bool wide, uint32_t *data, uint32_t *size);

#endif
