/*
 * The Cocoa side of the window system (rt_window.m).
 *
 * USER32 (rt_user32.c, rt_dialog.c) owns the Win32 state: window handles,
 * styles, rectangles and message queues. This interface makes and changes
 * the native windows and views, and it reports native events back to
 * USER32 through the rt_user32_on_* functions (rt_user32.h).
 *
 * Coordinates are Win32 screen coordinates in points: the origin is the
 * top left corner of the main display, and y increases downward. One
 * point is one Win32 pixel.
 *
 * Native objects:
 *   - A top-level window is one NSWindow. Its content view is a flipped
 *     view: a CAMetalLayer view for a window of a guest class, or a plain
 *     view for a dialog.
 *   - A child window is one NSView in the content view of its parent. Its
 *     frame is in the client coordinates of the parent.
 *
 * All functions must be called on the main thread, except where a comment
 * says otherwise. They stop the process with a message on another thread.
 */
#ifndef RT_WINDOW_H
#define RT_WINDOW_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct rt_rect {
    int32_t left, top, right, bottom;
} rt_rect;

/* Native window kinds (from the Win32 style). */
#define RT_WIN_TITLED 1u       /* WS_CAPTION: title bar */
#define RT_WIN_CLOSABLE 2u     /* WS_SYSMENU: close button */
#define RT_WIN_MINIMIZABLE 4u  /* WS_MINIMIZEBOX */
#define RT_WIN_RESIZABLE 8u    /* WS_THICKFRAME */
#define RT_WIN_ZOOMABLE 16u    /* WS_MAXIMIZEBOX */
#define RT_WIN_METAL 32u       /* the content view has a CAMetalLayer */
#define RT_WIN_TOPMOST 64u     /* WS_EX_TOPMOST */

/* Any thread. */
bool rt_cocoa_is_main_thread(void);

/* Start the application object (activation policy, main menu). Call it on
   the main thread before any other function of this interface. headless:
   the application never shows a window and never becomes active. */
void rt_cocoa_init(bool headless);
/* Game controller input (rt_gamepad.m). Main thread, once, after
   rt_cocoa_init; not in headless mode. */
void rt_gamepad_init(void);
bool rt_cocoa_headless(void);

/* The height of the title bar of a titled window (any thread, after
   rt_cocoa_init). */
int32_t rt_cocoa_title_height(void);

/* ---------------------------------------------------------------- displays */

#define RT_MAX_DISPLAYS 16

typedef struct rt_display {
    uint32_t id;          /* CGDirectDisplayID */
    rt_rect frame;        /* screen coordinates */
    rt_rect work;         /* the frame without the menu bar and the Dock */
    bool main;
    uint32_t refresh;     /* Hz (the maximum frame rate of the screen) */
    char name[128];       /* UTF-8 name of the display */
} rt_display;

/* The displays, the main display first. Returns the number of displays
   (at least 1; stops the process when no display is available). */
int rt_cocoa_displays(rt_display out[RT_MAX_DISPLAYS]);

/* Any thread. The shortest refresh interval in seconds
   (NSScreen.minimumRefreshInterval: 1/120 on a ProMotion screen) of the
   display that holds the point (x, y) in screen coordinates. Returns 0 when
   no display holds the point. */
double rt_cocoa_refresh_interval_at(int32_t x, int32_t y);

typedef struct rt_display_mode {
    uint32_t width, height, refresh; /* points, Hz */
} rt_display_mode;

/* The modes of display id that the desktop can use, without duplicates,
   sorted by width, height and refresh rate. Returns the number of modes;
   at most max are written. *current is the index of the current mode. */
int rt_cocoa_display_modes(uint32_t id, rt_display_mode *out, int max, int *current);
/* Change the mode of display id for the life of the process (0) or test
   that the mode exists (test). Returns false if the display has no such
   mode or the change failed. */
bool rt_cocoa_set_display_mode(uint32_t id, const rt_display_mode *m, bool test);
/* Restore the mode of every display that rt_cocoa_set_display_mode
   changed. */
void rt_cocoa_restore_display_modes(void);

/* ---------------------------------------------------------------- windows and views */

/* Make a hidden top-level window for hwnd with the client rectangle
   client (screen coordinates). title is UTF-8. Returns the native window
   reference. */
void *rt_cocoa_window_create(uint32_t hwnd, rt_rect client, unsigned kind, const char *title);
void rt_cocoa_window_destroy(void *w);
void rt_cocoa_window_show(void *w, bool visible, bool activate);
void rt_cocoa_window_minimize(void *w, bool minimize);
void rt_cocoa_window_set_client(void *w, rt_rect client);
void rt_cocoa_window_set_title(void *w, const char *title);
void rt_cocoa_window_set_kind(void *w, unsigned kind);
/* The client rectangle (screen coordinates) of a native window. */
rt_rect rt_cocoa_window_client(void *w);
bool rt_cocoa_window_is_key(void *w);
/* Make the native window w the key window and activate the application. */
void rt_cocoa_activate(void *w);
/* Enable or disable mouse and key input to the native window. */
void rt_cocoa_window_enable(void *w, bool enabled);
/* The CAMetalLayer of the content view (for the renderer); NULL if the
   window has no Metal layer. */
void *rt_cocoa_window_layer(void *w);
/* The content view of a native window, or the native view of a child. */
void *rt_cocoa_content_view(void *native);

/* Native child views. kind: one of RT_VIEW_*. parent: the content view of
   a native window or another native view. frame: client coordinates of
   the parent. text: UTF-8. */
#define RT_VIEW_PANE 1        /* plain view (a child dialog) */
#define RT_VIEW_METAL 2       /* CAMetalLayer view (a child of a guest class) */
#define RT_VIEW_PUSH 3        /* push button */
#define RT_VIEW_DEFPUSH 4     /* default push button (Return) */
#define RT_VIEW_CHECK 5       /* check box */
#define RT_VIEW_RADIO 6       /* radio button */
#define RT_VIEW_GROUP 7       /* group box */
#define RT_VIEW_LABEL 8       /* static text */
#define RT_VIEW_IMAGE 9       /* static icon or bitmap */
#define RT_VIEW_EDIT 10       /* one-line edit field */
#define RT_VIEW_EDIT_MULTI 11 /* multi-line edit field */
#define RT_VIEW_POPUP 12      /* drop-down list (combo box) */
#define RT_VIEW_TABS 13       /* tab strip */
#define RT_VIEW_LIST 14       /* report list view */

/* Text alignment for RT_VIEW_LABEL and the edit views. */
#define RT_ALIGN_LEFT 0
#define RT_ALIGN_CENTER 1
#define RT_ALIGN_RIGHT 2

void *rt_cocoa_view_create(uint32_t hwnd, void *parent, int kind, rt_rect frame, const char *text, int align, bool read_only);
void rt_cocoa_view_destroy(void *v);
void rt_cocoa_view_set_frame(void *v, rt_rect frame);
void rt_cocoa_view_set_visible(void *v, bool visible);
void rt_cocoa_view_set_enabled(void *v, bool enabled);
void rt_cocoa_view_set_text(void *v, const char *text);
/* The text of an edit view (UTF-8, malloc; the caller frees it). */
char *rt_cocoa_view_text(void *v);
void rt_cocoa_view_set_check(void *v, int state); /* 0, 1, 2 (indeterminate) */
int rt_cocoa_view_check(void *v);
void rt_cocoa_view_focus(void *v);
/* The image of an RT_VIEW_IMAGE view: rgba (straight alpha, row 0 on top)
   or NULL for no image. */
void rt_cocoa_view_set_image(void *v, const uint8_t *rgba, uint32_t w, uint32_t h);
/* A system icon: 32513 hand/error, 32514 question, 32515 exclamation,
   32516 asterisk/information, 32512 application, 32517 Windows logo. */
void rt_cocoa_view_set_system_icon(void *v, uint32_t id);

/* RT_VIEW_POPUP items. */
void rt_cocoa_popup_insert(void *v, int index, const char *text);
void rt_cocoa_popup_remove(void *v, int index);
void rt_cocoa_popup_clear(void *v);
void rt_cocoa_popup_select(void *v, int index); /* -1: no selection */
int rt_cocoa_popup_selected(void *v);

/* RT_VIEW_TABS: the tab strip is at x = 2, y = 0 with the height
   RT_TAB_STRIP_HEIGHT; each tab has the width that USER32 gives (the
   item rectangles of TCM_GETITEMRECT come from the same values). Below
   the strip is a bordered body. */
#define RT_TAB_STRIP_HEIGHT 22
/* The font size of the native controls (points): the Win32 dialog font,
   8 points at 96 dots per inch. */
#define RT_CONTROL_FONT_SIZE 11.0
void rt_cocoa_tabs_insert(void *v, int index, const char *text, int width);
void rt_cocoa_tabs_clear(void *v);
void rt_cocoa_tabs_select(void *v, int index);
int rt_cocoa_tabs_selected(void *v);

/* RT_VIEW_LIST columns, rows and cells. */
void rt_cocoa_list_insert_column(void *v, int index, const char *title, int width, int align);
void rt_cocoa_list_insert_row(void *v, int index);
void rt_cocoa_list_delete_row(void *v, int index);
void rt_cocoa_list_clear(void *v);
void rt_cocoa_list_set_cell(void *v, int row, int column, const char *text);
void rt_cocoa_list_set_grid(void *v, bool grid, bool full_row_select);

/* ---------------------------------------------------------------- events */

/* Handle the native events that wait now (at most one time in each
   millisecond). Native events become rt_user32_on_* calls. */
void rt_cocoa_pump(void);
/* Handle native events until *woken becomes true or the deadline (in
   CLOCK_UPTIME_RAW nanoseconds) passes. Returns false at the deadline. */
bool rt_cocoa_wait(atomic_bool *woken, bool infinite, uint64_t deadline_ns);
/* Wake rt_cocoa_wait on the main thread. Any thread; does not block. */
void rt_cocoa_wake(void);

/* ---------------------------------------------------------------- cursor */

/* A native cursor from RGBA pixels (straight alpha, row 0 on top). Any
   thread; the cursor is made on the main thread. Returns the reference. */
void *rt_cocoa_cursor_create(const uint8_t *rgba, uint32_t w, uint32_t h, int32_t hot_x, int32_t hot_y);
/* A system cursor for an IDC_* value (32512 to 32651). NULL if macOS has
   no such cursor. Any thread. */
void *rt_cocoa_cursor_system(uint32_t idc);
void rt_cocoa_cursor_release(void *c);
/* Show cursor c (NULL: no cursor) over the windows of the application.
   Any thread. */
void rt_cocoa_cursor_set(void *c, bool visible);
/* The cursor position (screen coordinates). Any thread. */
void rt_cocoa_cursor_pos(int32_t *x, int32_t *y);
/* Move the cursor. Any thread. */
void rt_cocoa_cursor_move(int32_t x, int32_t y);
/* Keep the cursor in rect while the application is active (NULL: no
   limit). Any thread. */
void rt_cocoa_cursor_clip(const rt_rect *rect);

/* ---------------------------------------------------------------- keyboard and text input */

bool rt_cocoa_caps_lock(void);
/* Enable the input method (marked text) for the view of hwnd. */
void rt_cocoa_ime_enable(void *native, bool enabled);
/* Cancel the marked text of the view. */
void rt_cocoa_ime_cancel(void *native);
/* The current keyboard input source types characters that are not ASCII
   (an input method). Any thread (the main thread updates the value when
   the source changes). */
bool rt_cocoa_input_source_native(void);
/* The caret blink interval in milliseconds (any thread). */
uint32_t rt_cocoa_caret_blink_ms(void);

/* ---------------------------------------------------------------- clipboard */

/* The text of the general pasteboard (UTF-8, malloc; NULL if it has no
   text). Any thread. */
char *rt_cocoa_clipboard_text(void);
bool rt_cocoa_clipboard_has_text(void);
/* Replace the contents of the general pasteboard (text NULL: empty).
   Any thread. */
void rt_cocoa_clipboard_set_text(const char *text);

#endif
