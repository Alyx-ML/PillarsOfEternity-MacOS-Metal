/*
 * The dialog manager and the dialog controls.
 *
 * Dialog boxes come from DIALOG and DIALOGEX resources. The dialog class
 * ("#32770") and the control classes (Button, Static, Edit, ComboBox,
 * SysTabControl32, SysListView32) are runtime classes: USER32 keeps the
 * window state and the message flow, and each control has a native
 * AppKit view (rt_window.m). The controls keep their Win32 state (check
 * state, items, selection) in this file and show it in the view.
 *
 * Supported subsets (other styles and messages stop the process with a
 * message):
 *   - Button: push, default push, check box, 3-state, radio, group box.
 *   - Static: text, icon, bitmap.
 *   - Edit: one line and multi-line, read-only; text messages only.
 *   - ComboBox: CBS_DROPDOWNLIST.
 *   - SysTabControl32: one row of tabs at the top, text only.
 *   - SysListView32: LVS_REPORT with text cells.
 *
 * Dialog units: x = dlu * base_x / 4, y = dlu * base_y / 8. The base
 * units come from the system font of the native controls with the point
 * size of the dialog font (GdiGetCharDimensions rules: the average width
 * of the 52 letters and the font height).
 */
#include "rt.h"
#include "rt_text.h"
#include "rt_thread.h"
#include "rt_user32.h"
#include "rt_window.h"

#include <CoreText/CoreText.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ERROR_INVALID_WINDOW_HANDLE 1400u
#define ERROR_CONTROL_ID_NOT_FOUND 1421u
#define ERROR_RESOURCE_DATA_NOT_FOUND 1812u
#define ERROR_RESOURCE_NAME_NOT_FOUND 1814u

#define RTNV 0x52544E56u /* WM_NULL wp: make the native view */

#define DWLP_MSGRESULT 0u
#define DWLP_DLGPROC 4u
#define DWLP_USER 8u
#define DLGWINDOWEXTRA 30u

#define DS_ABSALIGN 0x01u
#define DS_SETFONT 0x40u
#define DS_MODALFRAME 0x80u
#define DS_CONTROL 0x400u
#define DS_CENTER 0x800u
#define DS_CENTERMOUSE 0x1000u

#define DM_GETDEFID 0x0400u
#define DM_SETDEFID 0x0401u
#define DC_HASDEFID 0x534Bu

#define IDOK 1u
#define IDCANCEL 2u

#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SWP_NOMOVE 0x0002u
#define SWP_NOZORDER 0x0004u
#define SWP_NOACTIVATE 0x0010u

#define VK_TAB 0x09u
#define VK_RETURN 0x0Du
#define VK_ESCAPE 0x1Bu

#define DLGC_WANTARROWS 0x0001u
#define DLGC_WANTTAB 0x0002u
#define DLGC_WANTALLKEYS 0x0004u
#define DLGC_HASSETSEL 0x0008u
#define DLGC_DEFPUSHBUTTON 0x0010u
#define DLGC_UNDEFPUSHBUTTON 0x0020u
#define DLGC_RADIOBUTTON 0x0040u
#define DLGC_WANTCHARS 0x0080u
#define DLGC_STATIC 0x0100u
#define DLGC_BUTTON 0x2000u

#define DLG_MAGIC 0x31474C44u /* "DLG1": the create parameter of a dialog {magic, unicode} */
#define MAX_ITEMS 32767
#define MAX_DIALOG_CONTROLS 255

/* ---------------------------------------------------------------- base units and text widths */

static CTFontRef control_font(double size)
{
    CTFontRef f = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, (CGFloat)size, NULL);
    if (f == NULL) rt_fatal("dialog: the system font is not available (CoreText)");
    return f;
}

/* The dialog base units of a dialog font of pt points (0: the dialog has
   no font; Windows then uses the system font, 8 x 16). */
static void base_units(uint32_t pt, int *bx, int *by)
{
    static const char letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    UniChar chars[52];
    CGGlyph glyphs[52];
    CTFontRef f;
    double total;
    int i;
    if (pt == 0)
    {
        *bx = 8;
        *by = 16;
        return;
    }
    if (pt > 72) rt_fatal("dialog: a dialog font of %u points is not supported", pt);
    f = control_font(round((double)pt * 96.0 / 72.0));
    for (i = 0; i < 52; i++) chars[i] = (UniChar)letters[i];
    if (!CTFontGetGlyphsForCharacters(f, chars, glyphs, 52)) rt_fatal("dialog: the system font has no Latin letters");
    total = CTFontGetAdvancesForGlyphs(f, kCTFontOrientationHorizontal, glyphs, NULL, 52);
    *bx = ((int)total / 26 + 1) / 2;
    *by = (int)ceil(CTFontGetAscent(f) + CTFontGetDescent(f));
    CFRelease(f);
    if (*bx <= 0 || *by <= 0) rt_fatal("dialog: the dialog base units are not valid");
}

/* The width of text in the font of the native controls. */
static int text_width(const char *text)
{
    CTFontRef f = control_font(RT_CONTROL_FONT_SIZE);
    CFStringRef s = CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
    CFAttributedStringRef as;
    CTLineRef line;
    double w;
    const void *keys[1], *vals[1];
    CFDictionaryRef attrs;
    if (s == NULL) rt_fatal("dialog: a text is not valid UTF-8");
    keys[0] = kCTFontAttributeName;
    vals[0] = f;
    attrs = CFDictionaryCreate(NULL, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    as = attrs != NULL ? CFAttributedStringCreate(NULL, s, attrs) : NULL;
    line = as != NULL ? CTLineCreateWithAttributedString(as) : NULL;
    if (line == NULL) rt_fatal("dialog: a text could not be measured (CoreText)");
    w = CTLineGetTypographicBounds(line, NULL, NULL, NULL);
    CFRelease(line);
    CFRelease(as);
    CFRelease(attrs);
    CFRelease(s);
    CFRelease(f);
    return (int)ceil(w);
}

static int32_t dlu_x(int32_t v, int bx)
{
    int64_t p = (int64_t)v * bx;
    return (int32_t)(p >= 0 ? (p + 2) / 4 : -((-p + 2) / 4));
}

static int32_t dlu_y(int32_t v, int by)
{
    int64_t p = (int64_t)v * by;
    return (int32_t)(p >= 0 ? (p + 4) / 8 : -((-p + 4) / 8));
}

/* Text without the '&' mnemonic markers ("&&" is one '&'). */
static char *strip_mnemonic(const char *s)
{
    size_t n = strlen(s), i, o = 0;
    char *r = malloc(n + 1u);
    if (r == NULL) rt_fatal("out of host memory (dialog text)");
    for (i = 0; i < n; i++)
    {
        if (s[i] == '&')
        {
            if (s[i + 1] == '&') r[o++] = '&';
            if (s[i + 1] != 0) i++;
            else break;
            if (s[i] == '&') continue;
        }
        r[o++] = s[i];
    }
    r[o] = 0;
    return r;
}

static char *xstrdup(const char *s)
{
    char *r = strdup(s != NULL ? s : "");
    if (r == NULL) rt_fatal("out of host memory (dialog)");
    return r;
}

/* "\r\n" -> "\n" (native multi-line text) and back. */
static char *to_lf(const char *s)
{
    size_t n = strlen(s), i, o = 0;
    char *r = malloc(n + 1u);
    if (r == NULL) rt_fatal("out of host memory (dialog text)");
    for (i = 0; i < n; i++)
        if (!(s[i] == '\r' && s[i + 1] == '\n')) r[o++] = s[i];
    r[o] = 0;
    return r;
}

static char *to_crlf(const char *s)
{
    size_t n = strlen(s), i, o = 0, lf = 0;
    char *r;
    for (i = 0; i < n; i++)
        if (s[i] == '\n') lf++;
    r = malloc(n + lf + 1u);
    if (r == NULL) rt_fatal("out of host memory (dialog text)");
    for (i = 0; i < n; i++)
    {
        if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) r[o++] = '\r';
        r[o++] = s[i];
    }
    r[o] = 0;
    return r;
}

/* Copy s to a guest buffer of cap characters (A or W) with a NUL;
   returns the number of characters without the NUL. The text is cut at
   the buffer size, as Windows does. */
static uint32_t copy_out(const char *s, uint32_t buf, uint32_t cap, bool wide)
{
    size_t i = 0, len = strlen(s);
    uint32_t o = 0;
    if (cap == 0 || buf == 0) return 0;
    while (i < len)
    {
        uint32_t cp;
        int k = rt_utf8_decode((const uint8_t *)s + i, len - i, &cp);
        if (k <= 0) rt_fatal("dialog: a text is not valid UTF-8");
        i += (size_t)k;
        if (wide)
        {
            uint32_t units = cp >= 0x10000u ? 2u : 1u;
            if (o + units >= cap) break;
            if (units == 2)
            {
                wr16(buf + 2u * o++, (uint16_t)(0xD800u + ((cp - 0x10000u) >> 10)));
                wr16(buf + 2u * o++, (uint16_t)(0xDC00u + ((cp - 0x10000u) & 0x3FFu)));
            }
            else
                wr16(buf + 2u * o++, (uint16_t)cp);
        }
        else
        {
            int c = rt_unicode_to_cp1252(cp);
            if (o + 1u >= cap) break;
            wr8(buf + o++, (uint8_t)(c < 0 ? '?' : c));
        }
    }
    if (wide)
        wr16(buf + 2u * o, 0);
    else
        wr8(buf + o, 0);
    return o;
}

/* The length of s in characters of the form (A or W). */
static uint32_t text_len(const char *s, bool wide)
{
    size_t i = 0, len = strlen(s);
    uint32_t n = 0;
    while (i < len)
    {
        uint32_t cp;
        int k = rt_utf8_decode((const uint8_t *)s + i, len - i, &cp);
        if (k <= 0) rt_fatal("dialog: a text is not valid UTF-8");
        i += (size_t)k;
        n += wide && cp >= 0x10000u ? 2u : 1u;
    }
    return n;
}

/* ---------------------------------------------------------------- control state */

enum { CTL_BUTTON = 1, CTL_STATIC, CTL_EDIT, CTL_COMBO, CTL_TAB, CTL_LIST };

typedef struct item {
    char *text;
    uint32_t data;
    int width; /* tabs: the width of the tab */
} item;

typedef struct lv_row {
    char **cells; /* one for each column (NULL: empty) */
    uint32_t param, state;
} lv_row;

typedef struct lv_col {
    char *title;
    int width, align;
} lv_col;

typedef struct ctl {
    int type;
    uint32_t style;
    uint32_t font;
    void *view;
    /* button */
    int check;
    bool pushed;
    /* static */
    uint32_t image, image_type;
    /* combo box, tabs */
    item *items;
    int count, cap, cursel;
    /* list view */
    lv_col *cols;
    int ncols;
    lv_row *rows;
    int nrows, rcap, sel;
    uint32_t ex_style;
} ctl;

typedef struct modal_state {
    bool ended;
    uint32_t result;
} modal_state;

typedef struct dialog {
    uint32_t font;
    bool unicode; /* charset of the dialog procedure */
    bool ended;
    uint32_t result;
    modal_state *modal;
    uint32_t disabled_owner; /* the owner that DialogBox disabled */
    void *view;              /* a child dialog: its pane */
} dialog;

static ctl *ctl_of(uint32_t hwnd)
{
    return rt_user32_class_data(hwnd);
}

static void send_command(_cpu *cpu, uint32_t hwnd, uint32_t code)
{
    uint32_t parent = rt_user32_parent(hwnd), id = rt_user32_id(hwnd);
    if (parent == 0) return;
    rt_user32_send(cpu, parent, WM_COMMAND, (id & 0xFFFFu) | (code << 16), hwnd, rt_user32_is_unicode(parent));
}

/* Send WM_NOTIFY with a structure of size bytes that starts with NMHDR;
   fill(ptr) writes the rest. */
static uint32_t send_notify(_cpu *cpu, uint32_t hwnd, uint32_t code, uint32_t size, void (*fill)(uint32_t, const void *),
                            const void *ctx)
{
    uint32_t parent = rt_user32_parent(hwnd), id = rt_user32_id(hwnd), nm, r;
    if (parent == 0) return 0;
    nm = rt_heap_calloc(1, size);
    if (nm == 0) rt_fatal("out of guest memory (WM_NOTIFY)");
    wr32(nm, hwnd);
    wr32(nm + 4, id);
    wr32(nm + 8, code);
    if (fill != NULL) fill(nm, ctx);
    r = rt_user32_send(cpu, parent, WM_NOTIFY, id, nm, rt_user32_is_unicode(parent));
    rt_heap_free(nm);
    return r;
}

static void items_insert(ctl *c, int index, const char *text, uint32_t data, int width)
{
    if (c->count >= MAX_ITEMS) rt_fatal("dialog: a control has more than %d items", MAX_ITEMS);
    if (c->count == c->cap)
    {
        int ncap = c->cap == 0 ? 16 : c->cap * 2;
        item *n = realloc(c->items, (size_t)ncap * sizeof *n);
        if (n == NULL) rt_fatal("out of host memory (control items)");
        c->items = n;
        c->cap = ncap;
    }
    memmove(&c->items[index + 1], &c->items[index], (size_t)(c->count - index) * sizeof *c->items);
    c->items[index].text = xstrdup(text);
    c->items[index].data = data;
    c->items[index].width = width;
    c->count++;
}

static void items_delete(ctl *c, int index)
{
    free(c->items[index].text);
    memmove(&c->items[index], &c->items[index + 1], (size_t)(c->count - index - 1) * sizeof *c->items);
    c->count--;
}

static void items_clear(ctl *c)
{
    int i;
    for (i = 0; i < c->count; i++) free(c->items[i].text);
    c->count = 0;
}

static void ctl_free(ctl *c)
{
    int i, j;
    items_clear(c);
    free(c->items);
    for (i = 0; i < c->nrows; i++)
    {
        for (j = 0; j < c->ncols; j++) free(c->rows[i].cells[j]);
        free(c->rows[i].cells);
    }
    free(c->rows);
    for (i = 0; i < c->ncols; i++) free(c->cols[i].title);
    free(c->cols);
    free(c);
}

/* ---------------------------------------------------------------- native views */

static void apply_static_image(ctl *c)
{
    uint8_t *rgba = NULL;
    uint32_t w = 0, h = 0, sys = 0;
    bool alpha;
    if (c->view == NULL) return;
    if (c->image == 0)
    {
        rt_cocoa_view_set_image(c->view, NULL, 0, 0);
        return;
    }
    if (c->image_type == 0)
    {
        if (!rt_gdi_bitmap_rgba(c->image, &rgba, &w, &h, &alpha))
        {
            rt_cocoa_view_set_image(c->view, NULL, 0, 0);
            return;
        }
        rt_cocoa_view_set_image(c->view, rgba, w, h);
    }
    else
    {
        if (!rt_user32_icon_pixels(c->image, &rgba, &w, &h, &sys))
        {
            rt_cocoa_view_set_image(c->view, NULL, 0, 0);
            return;
        }
        if (rgba == NULL && sys != 0)
            rt_cocoa_view_set_system_icon(c->view, sys);
        else
            rt_cocoa_view_set_image(c->view, rgba, w, h);
    }
    free(rgba);
}

static int view_kind_of(const ctl *c, int *align, bool *ro)
{
    uint32_t s = c->style;
    *align = RT_ALIGN_LEFT;
    *ro = false;
    switch (c->type)
    {
    case CTL_BUTTON:
        switch (s & 0xFu)
        {
        case 0: return RT_VIEW_PUSH;
        case 1: return RT_VIEW_DEFPUSH;
        case 2: case 3: case 5: case 6: return RT_VIEW_CHECK;
        case 4: case 9: return RT_VIEW_RADIO;
        case 7: return RT_VIEW_GROUP;
        default: break;
        }
        break;
    case CTL_STATIC:
        switch (s & 0x1Fu)
        {
        case 0: case 0xB: case 0xC: return RT_VIEW_LABEL;
        case 1: *align = RT_ALIGN_CENTER; return RT_VIEW_LABEL;
        case 2: *align = RT_ALIGN_RIGHT; return RT_VIEW_LABEL;
        case 3: case 0xE: return RT_VIEW_IMAGE;
        default: break;
        }
        break;
    case CTL_EDIT:
        *align = (s & 3u) == 1 ? RT_ALIGN_CENTER : (s & 3u) == 2 ? RT_ALIGN_RIGHT : RT_ALIGN_LEFT;
        *ro = (s & 0x800u) != 0;
        return (s & 4u) ? RT_VIEW_EDIT_MULTI : RT_VIEW_EDIT;
    case CTL_COMBO: return RT_VIEW_POPUP;
    case CTL_TAB: return RT_VIEW_TABS;
    case CTL_LIST: return RT_VIEW_LIST;
    default: break;
    }
    rt_fatal("dialog: control type %d with style 0x%08x has no native view", c->type, s);
}

/* The text that the view shows for the window text. */
static char *view_text(uint32_t hwnd, const ctl *c)
{
    char *t = rt_user32_text(hwnd), *r;
    if (t == NULL) t = xstrdup("");
    if (c->type == CTL_BUTTON || (c->type == CTL_STATIC && !(c->style & 0x80u))) /* SS_NOPREFIX */
        r = strip_mnemonic(t);
    else if (c->type == CTL_EDIT && (c->style & 4u))
        r = to_lf(t);
    else
        return t;
    free(t);
    return r;
}

static void make_view(uint32_t hwnd, ctl *c)
{
    void *parent = rt_user32_native_view(rt_user32_parent(hwnd));
    rt_rect fr = rt_user32_child_rect(hwnd);
    int align, kind, i, j;
    bool ro;
    char *text;
    if (parent == NULL || c->view != NULL) return;
    kind = view_kind_of(c, &align, &ro);
    text = view_text(hwnd, c);
    c->view = rt_cocoa_view_create(hwnd, parent, kind, fr, c->type == CTL_COMBO || c->type == CTL_TAB ? "" : text, align, ro);
    free(text);
    rt_user32_set_native_view(hwnd, c->view);
    switch (c->type)
    {
    case CTL_BUTTON:
        if (kind == RT_VIEW_CHECK || kind == RT_VIEW_RADIO) rt_cocoa_view_set_check(c->view, c->check);
        break;
    case CTL_STATIC:
        if (kind == RT_VIEW_IMAGE) apply_static_image(c);
        break;
    case CTL_COMBO:
        for (i = 0; i < c->count; i++) rt_cocoa_popup_insert(c->view, i, c->items[i].text);
        rt_cocoa_popup_select(c->view, c->cursel);
        break;
    case CTL_TAB:
        for (i = 0; i < c->count; i++) rt_cocoa_tabs_insert(c->view, i, c->items[i].text, c->items[i].width);
        rt_cocoa_tabs_select(c->view, c->cursel);
        break;
    case CTL_LIST:
        for (j = 0; j < c->ncols; j++) rt_cocoa_list_insert_column(c->view, j, c->cols[j].title, c->cols[j].width, c->cols[j].align);
        for (i = 0; i < c->nrows; i++)
        {
            rt_cocoa_list_insert_row(c->view, i);
            for (j = 0; j < c->ncols; j++)
                if (c->rows[i].cells[j] != NULL) rt_cocoa_list_set_cell(c->view, i, j, c->rows[i].cells[j]);
        }
        rt_cocoa_list_set_grid(c->view, (c->ex_style & 1u) != 0, (c->ex_style & 0x20u) != 0);
        break;
    default: break;
    }
    rt_cocoa_view_set_enabled(c->view, !(rt_user32_style(hwnd) & WS_DISABLED));
    rt_cocoa_view_set_visible(c->view, (rt_user32_style(hwnd) & WS_VISIBLE) != 0);
}

/* ---------------------------------------------------------------- Button */

static void button_set_check(ctl *c, int state)
{
    c->check = state;
    if (c->view != NULL) rt_cocoa_view_set_check(c->view, state);
}

static bool is_radio(uint32_t style)
{
    return (style & 0xFu) == 4u || (style & 0xFu) == 9u;
}

/* An automatic radio button was clicked: check it and clear the other
   automatic radio buttons of its group (the controls from the previous
   WS_GROUP control to the next one, in creation order). */
static void radio_group_check(uint32_t hwnd)
{
    uint32_t kids[256];
    int n = rt_user32_children(rt_user32_parent(hwnd), kids, 256), i, me = -1, start, end;
    for (i = 0; i < n; i++)
        if (kids[i] == hwnd) me = i;
    if (me < 0) return;
    start = me;
    while (start > 0 && !(rt_user32_style(kids[start]) & WS_GROUP)) start--;
    end = me + 1;
    while (end < n && !(rt_user32_style(kids[end]) & WS_GROUP)) end++;
    for (i = start; i < end; i++)
    {
        ctl *o = ctl_of(kids[i]);
        if (o == NULL || o->type != CTL_BUTTON || (o->style & 0xFu) != 9u) continue;
        button_set_check(o, kids[i] == hwnd ? 1 : 0);
    }
}

static void button_click(_cpu *cpu, uint32_t hwnd, ctl *c)
{
    switch (c->style & 0xFu)
    {
    case 3: button_set_check(c, c->check ? 0 : 1); break;      /* BS_AUTOCHECKBOX */
    case 6: button_set_check(c, (c->check + 1) % 3); break;     /* BS_AUTO3STATE */
    case 9: radio_group_check(hwnd); break;                     /* BS_AUTORADIOBUTTON */
    default: break;
    }
    send_command(cpu, hwnd, 0); /* BN_CLICKED */
}

static uint32_t button_msg(_cpu *cpu, uint32_t hwnd, ctl *c, uint32_t msg, uint32_t wp, uint32_t lp, bool *done)
{
    uint32_t t = c->style & 0xFu;
    (void)lp;
    *done = true;
    switch (msg)
    {
    case 0x00F0: /* BM_GETCHECK */
        return (t == 2 || t == 3 || t == 4 || t == 5 || t == 6 || t == 9) ? (uint32_t)c->check : 0u;
    case 0x00F1: /* BM_SETCHECK */
        if (t == 2 || t == 3 || t == 4 || t == 9)
            button_set_check(c, wp != 0 ? 1 : 0);
        else if (t == 5 || t == 6)
            button_set_check(c, wp > 2 ? 1 : (int)wp);
        return 0;
    case 0x00F2: /* BM_GETSTATE */
        return (uint32_t)c->check | (c->pushed ? 4u : 0u) | (rt_user32_focus() == hwnd ? 8u : 0u);
    case 0x00F3: /* BM_SETSTATE */
        c->pushed = wp != 0;
        return 0;
    case 0x00F5: /* BM_CLICK */
        button_click(cpu, hwnd, c);
        return 0;
    case WM_GETDLGCODE:
        if (t == 0) return DLGC_BUTTON | DLGC_UNDEFPUSHBUTTON;
        if (t == 1) return DLGC_BUTTON | DLGC_DEFPUSHBUTTON;
        if (t == 7) return DLGC_STATIC;
        return DLGC_BUTTON | (is_radio(c->style) ? DLGC_RADIOBUTTON : 0u);
    default:
        if (msg >= 0x00F0 && msg <= 0x00F8) rt_fatal("Button: message 0x%04x is not supported", msg);
        *done = false;
        return 0;
    }
}

/* ---------------------------------------------------------------- Static */

#define IMAGE_BITMAP 0u
#define IMAGE_ICON 1u
#define IMAGE_CURSOR 2u

static uint32_t static_msg(ctl *c, uint32_t msg, uint32_t wp, uint32_t lp, bool *done)
{
    uint32_t prev;
    *done = true;
    switch (msg)
    {
    case 0x0170: /* STM_SETICON */
        prev = c->image_type != IMAGE_BITMAP ? c->image : 0;
        c->image = wp;
        c->image_type = IMAGE_ICON;
        apply_static_image(c);
        return prev;
    case 0x0171: /* STM_GETICON */
        return c->image_type != IMAGE_BITMAP ? c->image : 0;
    case 0x0172: /* STM_SETIMAGE */
        if (wp > IMAGE_CURSOR) rt_fatal("Static: STM_SETIMAGE with image type %u is not supported", wp);
        prev = c->image;
        c->image = lp;
        c->image_type = wp;
        apply_static_image(c);
        return prev;
    case 0x0173: /* STM_GETIMAGE */
        return c->image_type == wp ? c->image : 0;
    case WM_GETDLGCODE:
        return DLGC_STATIC;
    default:
        *done = false;
        return 0;
    }
}

/* ---------------------------------------------------------------- Edit */

static void edit_sync(uint32_t hwnd, ctl *c)
{
    char *t, *w;
    if (c->view == NULL) return;
    t = rt_cocoa_view_text(c->view);
    w = (c->style & 4u) ? to_crlf(t) : xstrdup(t);
    rt_user32_set_text(hwnd, w);
    free(w);
    free(t);
}

/* ---------------------------------------------------------------- ComboBox */

#define CB_ERR 0xFFFFFFFFu
#define COMBO_HEIGHT 22 /* the height of the closed drop-down list */

static void combo_text(uint32_t hwnd, ctl *c)
{
    rt_user32_set_text(hwnd, c->cursel >= 0 && c->cursel < c->count ? c->items[c->cursel].text : "");
}

static int combo_find(const ctl *c, int start, const char *s, bool exact)
{
    int i, k, n = c->count;
    size_t len = strlen(s);
    if (n == 0) return -1;
    for (k = 1; k <= n; k++)
    {
        i = (start < 0 || start >= n) ? (k - 1) : (start + k) % n;
        if (exact ? strcasecmp(c->items[i].text, s) == 0 : strncasecmp(c->items[i].text, s, len) == 0) return i;
    }
    return -1;
}

static uint32_t combo_msg(uint32_t hwnd, ctl *c, uint32_t msg, uint32_t wp, uint32_t lp, bool unicode, bool *done)
{
    char *s;
    int i;
    *done = true;
    switch (msg)
    {
    case 0x0143: /* CB_ADDSTRING */
    case 0x014A: /* CB_INSERTSTRING */
        s = lp != 0 ? rt_user32_guest_text(lp, unicode) : xstrdup("");
        if (c->count >= MAX_ITEMS)
        {
            free(s);
            return 0xFFFFFFFEu; /* CB_ERRSPACE */
        }
        if (msg == 0x0143)
        {
            i = c->count;
            if (c->style & 0x100u) /* CBS_SORT */
                for (i = 0; i < c->count && strcasecmp(c->items[i].text, s) <= 0; i++) {}
        }
        else
        {
            i = (int32_t)wp;
            if (i == -1) i = c->count;
            if (i < 0 || i > c->count)
            {
                free(s);
                return CB_ERR;
            }
        }
        items_insert(c, i, s, 0, 0);
        if (c->cursel >= i) c->cursel++;
        if (c->view != NULL)
        {
            rt_cocoa_popup_insert(c->view, i, s);
            rt_cocoa_popup_select(c->view, c->cursel);
        }
        free(s);
        return (uint32_t)i;
    case 0x0144: /* CB_DELETESTRING */
        i = (int32_t)wp;
        if (i < 0 || i >= c->count) return CB_ERR;
        items_delete(c, i);
        if (c->cursel == i)
            c->cursel = -1;
        else if (c->cursel > i)
            c->cursel--;
        if (c->view != NULL)
        {
            rt_cocoa_popup_remove(c->view, i);
            rt_cocoa_popup_select(c->view, c->cursel);
        }
        combo_text(hwnd, c);
        return (uint32_t)c->count;
    case 0x0146: /* CB_GETCOUNT */
        return (uint32_t)c->count;
    case 0x0147: /* CB_GETCURSEL */
        return c->cursel >= 0 ? (uint32_t)c->cursel : CB_ERR;
    case 0x0148: /* CB_GETLBTEXT */
        i = (int32_t)wp;
        if (i < 0 || i >= c->count || lp == 0) return CB_ERR;
        return copy_out(c->items[i].text, lp, text_len(c->items[i].text, unicode) + 1u, unicode);
    case 0x0149: /* CB_GETLBTEXTLEN */
        i = (int32_t)wp;
        if (i < 0 || i >= c->count) return CB_ERR;
        return text_len(c->items[i].text, unicode);
    case 0x014B: /* CB_RESETCONTENT */
        items_clear(c);
        c->cursel = -1;
        if (c->view != NULL) rt_cocoa_popup_clear(c->view);
        combo_text(hwnd, c);
        return 1;
    case 0x014C: /* CB_FINDSTRING */
    case 0x0158: /* CB_FINDSTRINGEXACT */
    case 0x014D: /* CB_SELECTSTRING */
        s = lp != 0 ? rt_user32_guest_text(lp, unicode) : xstrdup("");
        i = combo_find(c, (int32_t)wp, s, msg == 0x0158);
        free(s);
        if (msg == 0x014D && i >= 0)
        {
            c->cursel = i;
            if (c->view != NULL) rt_cocoa_popup_select(c->view, i);
            combo_text(hwnd, c);
        }
        return i >= 0 ? (uint32_t)i : CB_ERR;
    case 0x014E: /* CB_SETCURSEL */
        i = (int32_t)wp;
        c->cursel = (i >= 0 && i < c->count) ? i : -1;
        if (c->view != NULL) rt_cocoa_popup_select(c->view, c->cursel);
        combo_text(hwnd, c);
        return c->cursel >= 0 ? (uint32_t)c->cursel : CB_ERR;
    case 0x0150: /* CB_GETITEMDATA */
        i = (int32_t)wp;
        return i >= 0 && i < c->count ? c->items[i].data : CB_ERR;
    case 0x0151: /* CB_SETITEMDATA */
        i = (int32_t)wp;
        if (i < 0 || i >= c->count) return CB_ERR;
        c->items[i].data = lp;
        return 1;
    case 0x0157: /* CB_GETDROPPEDSTATE */
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    default:
        if (msg >= 0x0140 && msg <= 0x0165) rt_fatal("ComboBox: message 0x%04x is not supported", msg);
        *done = false;
        return 0;
    }
}

/* ---------------------------------------------------------------- SysTabControl32 */

#define TAB_PAD 20 /* the width of a tab: the text and this padding */

static void tab_item_rect(const ctl *c, int index, rt_rect *r)
{
    int32_t x = 2;
    int i;
    for (i = 0; i < index; i++) x += c->items[i].width;
    r->left = x;
    r->top = 0;
    r->right = x + c->items[index].width;
    r->bottom = RT_TAB_STRIP_HEIGHT;
}

/* TCITEMA/W: mask 0, dwState 4, dwStateMask 8, pszText 12, cchTextMax 16,
   iImage 20, lParam 24. */
static uint32_t tab_msg(_cpu *cpu, uint32_t hwnd, ctl *c, uint32_t msg, uint32_t wp, uint32_t lp, bool *done)
{
    int i;
    bool wide;
    (void)cpu;
    *done = true;
    switch (msg)
    {
    case 0x1304: /* TCM_GETITEMCOUNT */
        return (uint32_t)c->count;
    case 0x1307: /* TCM_INSERTITEMA */
    case 0x133E: /* TCM_INSERTITEMW */
    {
        uint32_t mask = lp != 0 ? rd32(lp) : 0;
        char *s;
        wide = msg == 0x133E;
        if (lp == 0) return 0xFFFFFFFFu;
        if ((mask & 2u) && (int32_t)rd32(lp + 20) != -1) rt_fatal("SysTabControl32: tab images are not supported");
        i = (int32_t)wp;
        if (i < 0) return 0xFFFFFFFFu;
        if (i > c->count) i = c->count;
        s = (mask & 1u) && rd32(lp + 12) != 0 ? rt_user32_guest_text(rd32(lp + 12), wide) : xstrdup("");
        {
            char *plain = strip_mnemonic(s);
            items_insert(c, i, plain, (mask & 8u) ? rd32(lp + 24) : 0, text_width(plain) + TAB_PAD);
            if (c->view != NULL) rt_cocoa_tabs_insert(c->view, i, plain, c->items[i].width);
            free(plain);
        }
        free(s);
        if (c->cursel >= i)
            c->cursel++;
        else if (c->cursel < 0)
            c->cursel = 0; /* the first tab is selected */
        if (c->view != NULL) rt_cocoa_tabs_select(c->view, c->cursel);
        return (uint32_t)i;
    }
    case 0x1305: /* TCM_GETITEMA */
    case 0x133C: /* TCM_GETITEMW */
    {
        uint32_t mask;
        i = (int32_t)wp;
        if (lp == 0 || i < 0 || i >= c->count) return 0;
        mask = rd32(lp);
        if (mask & 1u) copy_out(c->items[i].text, rd32(lp + 12), rd32(lp + 16), msg == 0x133C);
        if (mask & 8u) wr32(lp + 24, c->items[i].data);
        if (mask & 2u) wr32(lp + 20, 0xFFFFFFFFu);
        if (mask & 0x10u) wr32(lp + 4, i == c->cursel ? 1u : 0u); /* TCIS_BUTTONPRESSED */
        return 1;
    }
    case 0x1308: /* TCM_DELETEITEM */
        i = (int32_t)wp;
        if (i < 0 || i >= c->count) return 0;
        items_delete(c, i);
        if (c->cursel == i)
            c->cursel = -1;
        else if (c->cursel > i)
            c->cursel--;
        if (c->view != NULL)
        {
            int k;
            rt_cocoa_tabs_clear(c->view);
            for (k = 0; k < c->count; k++) rt_cocoa_tabs_insert(c->view, k, c->items[k].text, c->items[k].width);
            rt_cocoa_tabs_select(c->view, c->cursel);
        }
        return 1;
    case 0x1309: /* TCM_DELETEALLITEMS */
        items_clear(c);
        c->cursel = -1;
        if (c->view != NULL) rt_cocoa_tabs_clear(c->view);
        return 1;
    case 0x130A: /* TCM_GETITEMRECT */
    {
        rt_rect r;
        i = (int32_t)wp;
        if (lp == 0 || i < 0 || i >= c->count) return 0;
        tab_item_rect(c, i, &r);
        wr32(lp, (uint32_t)r.left);
        wr32(lp + 4, (uint32_t)r.top);
        wr32(lp + 8, (uint32_t)r.right);
        wr32(lp + 12, (uint32_t)r.bottom);
        return 1;
    }
    case 0x130B: /* TCM_GETCURSEL */
    case 0x132F: /* TCM_GETCURFOCUS */
        return (uint32_t)c->cursel;
    case 0x130C: /* TCM_SETCURSEL */
    case 0x1330: /* TCM_SETCURFOCUS */
    {
        int prev = c->cursel;
        i = (int32_t)wp;
        if (i < 0 || i >= c->count) return 0xFFFFFFFFu;
        c->cursel = i;
        if (c->view != NULL) rt_cocoa_tabs_select(c->view, i);
        return msg == 0x130C ? (uint32_t)prev : 0u;
    }
    case 0x1328: /* TCM_ADJUSTRECT */
    {
        /* The display area: below the tab strip, inside the border. */
        int32_t l, t, r, b, top = RT_TAB_STRIP_HEIGHT + 2;
        if (lp == 0) return 0;
        l = (int32_t)rd32(lp);
        t = (int32_t)rd32(lp + 4);
        r = (int32_t)rd32(lp + 8);
        b = (int32_t)rd32(lp + 12);
        if (wp == 0)
        {
            l += 4;
            t += top;
            r -= 4;
            b -= 4;
        }
        else
        {
            l -= 4;
            t -= top;
            r += 4;
            b += 4;
        }
        wr32(lp, (uint32_t)l);
        wr32(lp + 4, (uint32_t)t);
        wr32(lp + 8, (uint32_t)r);
        wr32(lp + 12, (uint32_t)b);
        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    default:
        if (msg >= 0x1300 && msg <= 0x13FF)
            rt_fatal("SysTabControl32: message 0x%04x is not supported (window 0x%08x)", msg, hwnd);
        *done = false;
        return 0;
    }
}

/* ---------------------------------------------------------------- SysListView32 */

#define LVIF_TEXT 0x1u
#define LVIF_IMAGE 0x2u
#define LVIF_PARAM 0x4u
#define LVIF_STATE 0x8u
#define LVIS_FOCUSED 0x1u
#define LVIS_SELECTED 0x2u
#define LPSTR_TEXTCALLBACK 0xFFFFFFFFu

/* Extended styles that change nothing for the native list: grid lines and
   full row select are shown; the others are visual details of Windows. */
#define LV_EX_SUPPORTED (0x1u /* GRIDLINES */ | 0x20u /* FULLROWSELECT */ | 0x400u /* INFOTIP */ | 0x4000u /* LABELTIP */ | \
                         0x8000u /* BORDERSELECT */ | 0x10000u /* DOUBLEBUFFER */)

static char *lv_text(uint32_t p, bool wide)
{
    if (p == LPSTR_TEXTCALLBACK) rt_fatal("SysListView32: LPSTR_TEXTCALLBACK is not supported");
    return p != 0 ? rt_user32_guest_text(p, wide) : xstrdup("");
}

static void lv_set_cell(ctl *c, int row, int col, char *text)
{
    free(c->rows[row].cells[col]);
    c->rows[row].cells[col] = text;
    if (c->view != NULL) rt_cocoa_list_set_cell(c->view, row, col, text);
}

/* LVITEM: mask 0, iItem 4, iSubItem 8, state 12, stateMask 16, pszText 20,
   cchTextMax 24, iImage 28, lParam 32. */
static void lv_apply_item(ctl *c, int row, uint32_t lp, bool wide)
{
    uint32_t mask = rd32(lp), sub = rd32(lp + 8);
    if (mask & ~(LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM | LVIF_STATE)) rt_fatal("SysListView32: item mask 0x%x is not supported", mask);
    if ((mask & LVIF_IMAGE) && (int32_t)rd32(lp + 28) >= 0) rt_fatal("SysListView32: item images are not supported");
    if (mask & LVIF_TEXT)
    {
        if ((int)sub >= c->ncols) rt_fatal("SysListView32: subitem %u has no column", sub);
        lv_set_cell(c, row, (int)sub, lv_text(rd32(lp + 20), wide));
    }
    if ((mask & LVIF_PARAM) && sub == 0) c->rows[row].param = rd32(lp + 32);
    if ((mask & LVIF_STATE) && sub == 0)
    {
        uint32_t sm = rd32(lp + 16), st = rd32(lp + 12);
        c->rows[row].state = (c->rows[row].state & ~sm) | (st & sm);
        if ((sm & LVIS_SELECTED) && (st & LVIS_SELECTED))
            rt_fatal("SysListView32: a selection that the program sets is not supported");
    }
}

static uint32_t list_insert_item(ctl *c, uint32_t lp, bool wide)
{
    int row;
    if (lp == 0) return 0xFFFFFFFFu;
    if (rd32(lp + 8) != 0) return 0xFFFFFFFFu; /* LVM_INSERTITEM needs iSubItem 0 */
    if (c->nrows >= MAX_ITEMS) rt_fatal("SysListView32: more than %d items", MAX_ITEMS);
    row = (int32_t)rd32(lp + 4);
    if (row < 0 || row > c->nrows) row = c->nrows;
    if (c->nrows == c->rcap)
    {
        int ncap = c->rcap == 0 ? 16 : c->rcap * 2;
        lv_row *n = realloc(c->rows, (size_t)ncap * sizeof *n);
        if (n == NULL) rt_fatal("out of host memory (list view)");
        c->rows = n;
        c->rcap = ncap;
    }
    memmove(&c->rows[row + 1], &c->rows[row], (size_t)(c->nrows - row) * sizeof *c->rows);
    c->rows[row].cells = calloc((size_t)(c->ncols > 0 ? c->ncols : 1), sizeof(char *));
    if (c->rows[row].cells == NULL) rt_fatal("out of host memory (list view)");
    c->rows[row].param = 0;
    c->rows[row].state = 0;
    c->nrows++;
    if (c->sel >= row) c->sel++;
    if (c->view != NULL) rt_cocoa_list_insert_row(c->view, row);
    if (c->ncols == 0 && (rd32(lp) & LVIF_TEXT)) rt_fatal("SysListView32: an item with text needs a column");
    lv_apply_item(c, row, lp, wide);
    return (uint32_t)row;
}

static void list_delete_row(ctl *c, int row)
{
    int j;
    for (j = 0; j < c->ncols; j++) free(c->rows[row].cells[j]);
    free(c->rows[row].cells);
    memmove(&c->rows[row], &c->rows[row + 1], (size_t)(c->nrows - row - 1) * sizeof *c->rows);
    c->nrows--;
    if (c->sel == row)
        c->sel = -1;
    else if (c->sel > row)
        c->sel--;
    if (c->view != NULL) rt_cocoa_list_delete_row(c->view, row);
}

static uint32_t list_msg(ctl *c, uint32_t msg, uint32_t wp, uint32_t lp, bool *done)
{
    int i;
    *done = true;
    switch (msg)
    {
    case 0x1004: /* LVM_GETITEMCOUNT */
        return (uint32_t)c->nrows;
    case 0x1005: /* LVM_GETITEMA */
    case 0x104B: /* LVM_GETITEMW */
    {
        uint32_t mask, sub;
        if (lp == 0) return 0;
        i = (int32_t)rd32(lp + 4);
        mask = rd32(lp);
        sub = rd32(lp + 8);
        if (i < 0 || i >= c->nrows || (int)sub >= (c->ncols > 0 ? c->ncols : 1)) return 0;
        if (mask & LVIF_TEXT)
            copy_out(c->ncols > 0 && c->rows[i].cells[sub] != NULL ? c->rows[i].cells[sub] : "", rd32(lp + 20), rd32(lp + 24),
                     msg == 0x104B);
        if (mask & LVIF_PARAM) wr32(lp + 32, c->rows[i].param);
        if (mask & LVIF_STATE)
            wr32(lp + 12, (c->rows[i].state | (i == c->sel ? LVIS_SELECTED | LVIS_FOCUSED : 0u)) & rd32(lp + 16));
        if (mask & LVIF_IMAGE) wr32(lp + 28, 0xFFFFFFFFu);
        return 1;
    }
    case 0x1006: /* LVM_SETITEMA */
    case 0x104C: /* LVM_SETITEMW */
        if (lp == 0) return 0;
        i = (int32_t)rd32(lp + 4);
        if (i < 0 || i >= c->nrows) return 0;
        lv_apply_item(c, i, lp, msg == 0x104C);
        return 1;
    case 0x1007: /* LVM_INSERTITEMA */
    case 0x104D: /* LVM_INSERTITEMW */
        return list_insert_item(c, lp, msg == 0x104D);
    case 0x1008: /* LVM_DELETEITEM */
        i = (int32_t)wp;
        if (i < 0 || i >= c->nrows) return 0;
        list_delete_row(c, i);
        return 1;
    case 0x1009: /* LVM_DELETEALLITEMS */
        while (c->nrows > 0) list_delete_row(c, c->nrows - 1);
        return 1;
    case 0x100C: /* LVM_GETNEXTITEM */
    {
        int start = (int32_t)wp, k;
        uint32_t flags = lp & 0xFFFFu;
        if (flags & ~(LVIS_FOCUSED | LVIS_SELECTED)) rt_fatal("SysListView32: LVM_GETNEXTITEM flags 0x%x are not supported", flags);
        for (k = start + 1; k < c->nrows; k++)
        {
            if (k < 0) continue;
            if (flags == 0 || (k == c->sel)) return (uint32_t)k;
        }
        return 0xFFFFFFFFu;
    }
    case 0x101B: /* LVM_INSERTCOLUMNA */
    case 0x1061: /* LVM_INSERTCOLUMNW */
    {
        /* LVCOLUMN: mask 0, fmt 4, cx 8, pszText 12, cchTextMax 16,
           iSubItem 20. */
        uint32_t mask;
        lv_col col;
        int r;
        if (lp == 0) return 0xFFFFFFFFu;
        mask = rd32(lp);
        i = (int32_t)wp;
        if (i < 0) return 0xFFFFFFFFu;
        if (i < c->ncols) rt_fatal("SysListView32: a column before other columns is not supported (only at the end)");
        i = c->ncols;
        col.title = (mask & 4u) ? lv_text(rd32(lp + 12), msg == 0x1061) : xstrdup("");
        col.width = (mask & 2u) ? (int32_t)rd32(lp + 8) : 50;
        col.align = (mask & 1u) ? ((rd32(lp + 4) & 3u) == 1 ? RT_ALIGN_RIGHT : (rd32(lp + 4) & 3u) == 2 ? RT_ALIGN_CENTER : RT_ALIGN_LEFT)
                                : RT_ALIGN_LEFT;
        if ((mask & 8u) && (int32_t)rd32(lp + 20) != i) rt_fatal("SysListView32: a column subitem that is not its index is not supported");
        {
            lv_col *n = realloc(c->cols, (size_t)(c->ncols + 1) * sizeof *n);
            if (n == NULL) rt_fatal("out of host memory (list view)");
            c->cols = n;
        }
        c->cols[c->ncols++] = col;
        for (r = 0; r < c->nrows; r++)
        {
            char **cells = realloc(c->rows[r].cells, (size_t)c->ncols * sizeof(char *));
            if (cells == NULL) rt_fatal("out of host memory (list view)");
            cells[c->ncols - 1] = NULL;
            c->rows[r].cells = cells;
        }
        if (c->view != NULL) rt_cocoa_list_insert_column(c->view, i, col.title, col.width, col.align);
        return (uint32_t)i;
    }
    case 0x101D: /* LVM_GETCOLUMNWIDTH */
        i = (int32_t)wp;
        return i >= 0 && i < c->ncols ? (uint32_t)c->cols[i].width : 0u;
    case 0x102D: /* LVM_GETITEMTEXTA */
    case 0x1073: /* LVM_GETITEMTEXTW */
    {
        uint32_t sub;
        i = (int32_t)wp;
        if (lp == 0 || i < 0 || i >= c->nrows) return 0;
        sub = rd32(lp + 8);
        if ((int)sub >= c->ncols) return 0;
        return copy_out(c->rows[i].cells[sub] != NULL ? c->rows[i].cells[sub] : "", rd32(lp + 20), rd32(lp + 24), msg == 0x1073);
    }
    case 0x102E: /* LVM_SETITEMTEXTA */
    case 0x1074: /* LVM_SETITEMTEXTW */
    {
        uint32_t sub;
        i = (int32_t)wp;
        if (lp == 0 || i < 0 || i >= c->nrows) return 0;
        sub = rd32(lp + 8);
        if ((int)sub >= c->ncols) return 0;
        lv_set_cell(c, i, (int)sub, lv_text(rd32(lp + 20), msg == 0x1074));
        return 1;
    }
    case 0x102F: /* LVM_SETITEMCOUNT: a size hint for a list with items */
        return 1;
    case 0x1036: /* LVM_SETEXTENDEDLISTVIEWSTYLE */
    {
        uint32_t prev = c->ex_style, mask = wp != 0 ? wp : 0xFFFFFFFFu;
        uint32_t next = (c->ex_style & ~mask) | (lp & mask);
        if (next & ~LV_EX_SUPPORTED) rt_fatal("SysListView32: extended styles 0x%x are not supported", next & ~LV_EX_SUPPORTED);
        c->ex_style = next;
        if (c->view != NULL) rt_cocoa_list_set_grid(c->view, (next & 1u) != 0, (next & 0x20u) != 0);
        return prev;
    }
    case 0x1037: /* LVM_GETEXTENDEDLISTVIEWSTYLE */
        return c->ex_style;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    default:
        if (msg >= 0x1000 && msg <= 0x10FF) rt_fatal("SysListView32: message 0x%04x is not supported", msg);
        *done = false;
        return 0;
    }
}

/* ---------------------------------------------------------------- control window procedure */

static void check_style(int type, uint32_t style, uint32_t exstyle)
{
    (void)exstyle;
    switch (type)
    {
    case CTL_BUTTON:
    {
        uint32_t t = style & 0xFu;
        if (t == 8 || t > 9) rt_fatal("Button: button type %u (style 0x%08x) is not supported", t, style);
        if (style & (0x40u | 0x80u)) rt_fatal("Button: BS_ICON and BS_BITMAP are not supported");
        if (style & 0x1000u) rt_fatal("Button: BS_PUSHLIKE is not supported");
        break;
    }
    case CTL_STATIC:
    {
        uint32_t t = style & 0x1Fu;
        if (!(t <= 3 || t == 0xB || t == 0xC || t == 0xE)) rt_fatal("Static: static type 0x%x is not supported", t);
        if (style & 0x100u) rt_fatal("Static: SS_NOTIFY is not supported (the native label reports no clicks)");
        break;
    }
    case CTL_EDIT:
        if (style & (0x08u | 0x10u | 0x20u | 0x2000u))
            rt_fatal("Edit: ES_UPPERCASE, ES_LOWERCASE, ES_PASSWORD and ES_NUMBER are not supported (style 0x%08x)", style);
        break;
    case CTL_COMBO:
        if ((style & 3u) != 3u) rt_fatal("ComboBox: only CBS_DROPDOWNLIST is supported (style 0x%08x)", style);
        if (style & 0x30u) rt_fatal("ComboBox: owner-drawn combo boxes are not supported");
        break;
    case CTL_TAB:
        if (style & (0x0002u | 0x0080u | 0x0100u | 0x0200u | 0x2000u))
            rt_fatal("SysTabControl32: style 0x%08x is not supported (bottom, vertical, buttons, multi-line, owner draw)", style);
        break;
    case CTL_LIST:
        if ((style & 3u) != 1u) rt_fatal("SysListView32: only LVS_REPORT is supported (style 0x%08x)", style);
        if (style & (0x0010u | 0x0020u | 0x0200u | 0x0400u | 0x1000u | 0x4000u))
            rt_fatal("SysListView32: style 0x%08x is not supported (sort, edit labels, owner draw, owner data, no header)", style);
        break;
    default: break;
    }
}

static uint32_t ctl_proc(_cpu *cpu, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp, bool unicode, int type)
{
    ctl *c = ctl_of(hwnd);
    uint32_t r;
    bool done = false;
    if (msg == WM_NCCREATE)
    {
        uint32_t style = rd32(lp + 32), exstyle = rd32(lp + 44);
        check_style(type, style, exstyle);
        c = calloc(1, sizeof *c);
        if (c == NULL) rt_fatal("out of host memory (control)");
        c->type = type;
        c->style = style;
        c->cursel = -1;
        c->sel = -1;
        rt_user32_set_class_data(hwnd, c);
        return rt_user32_default_proc(cpu, hwnd, msg, wp, lp, unicode);
    }
    if (c == NULL) return rt_user32_default_proc(cpu, hwnd, msg, wp, lp, unicode);
    switch (msg)
    {
    case WM_NULL:
        if (wp == RTNV)
        {
            make_view(hwnd, c);
            return 0;
        }
        return 0;
    case WM_CREATE:
        if (type == CTL_COMBO)
        {
            /* The window has the height of the closed list; the template
               height is the height of the open list. */
            rt_rect fr = rt_user32_child_rect(hwnd);
            rt_user32_set_pos(cpu, hwnd, 0, 0, 0, fr.right - fr.left, COMBO_HEIGHT, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_SETTEXT:
        r = rt_user32_default_proc(cpu, hwnd, msg, wp, lp, unicode);
        if (c->view != NULL && type != CTL_COMBO && type != CTL_TAB && type != CTL_LIST)
        {
            char *t = view_text(hwnd, c);
            rt_cocoa_view_set_text(c->view, t);
            free(t);
        }
        return r;
    case WM_GETTEXT:
    case WM_GETTEXTLENGTH:
        if (type == CTL_EDIT) edit_sync(hwnd, c);
        return rt_user32_default_proc(cpu, hwnd, msg, wp, lp, unicode);
    case WM_ENABLE:
        if (c->view != NULL) rt_cocoa_view_set_enabled(c->view, wp != 0);
        return 0;
    case WM_SETFONT:
        c->font = wp;
        return 0;
    case WM_GETFONT:
        return c->font;
    case WM_NCDESTROY:
        rt_user32_set_class_data(hwnd, NULL);
        ctl_free(c);
        return rt_user32_default_proc(cpu, hwnd, msg, wp, lp, unicode);
    default:
        break;
    }
    switch (type)
    {
    case CTL_BUTTON: r = button_msg(cpu, hwnd, c, msg, wp, lp, &done); break;
    case CTL_STATIC: r = static_msg(c, msg, wp, lp, &done); break;
    case CTL_EDIT:
        if (msg >= 0x00B0 && msg <= 0x00DA) rt_fatal("Edit: message 0x%04x is not supported", msg);
        if (msg == WM_GETDLGCODE)
        {
            done = true;
            r = DLGC_WANTCHARS | DLGC_HASSETSEL | DLGC_WANTARROWS | ((c->style & 4u) ? DLGC_WANTALLKEYS : 0u);
        }
        else
            r = 0;
        break;
    case CTL_COMBO: r = combo_msg(hwnd, c, msg, wp, lp, unicode, &done); break;
    case CTL_TAB: r = tab_msg(cpu, hwnd, c, msg, wp, lp, &done); break;
    case CTL_LIST: r = list_msg(c, msg, wp, lp, &done); break;
    default: r = 0; break;
    }
    if (done) return r;
    return rt_user32_default_proc(cpu, hwnd, msg, wp, lp, unicode);
}

static uint32_t button_proc(_cpu *cpu, uint32_t h, uint32_t m, uint32_t wp, uint32_t lp, bool u) { return ctl_proc(cpu, h, m, wp, lp, u, CTL_BUTTON); }
static uint32_t static_proc(_cpu *cpu, uint32_t h, uint32_t m, uint32_t wp, uint32_t lp, bool u) { return ctl_proc(cpu, h, m, wp, lp, u, CTL_STATIC); }
static uint32_t edit_proc(_cpu *cpu, uint32_t h, uint32_t m, uint32_t wp, uint32_t lp, bool u) { return ctl_proc(cpu, h, m, wp, lp, u, CTL_EDIT); }
static uint32_t combo_proc(_cpu *cpu, uint32_t h, uint32_t m, uint32_t wp, uint32_t lp, bool u) { return ctl_proc(cpu, h, m, wp, lp, u, CTL_COMBO); }
static uint32_t tab_proc(_cpu *cpu, uint32_t h, uint32_t m, uint32_t wp, uint32_t lp, bool u) { return ctl_proc(cpu, h, m, wp, lp, u, CTL_TAB); }
static uint32_t list_proc(_cpu *cpu, uint32_t h, uint32_t m, uint32_t wp, uint32_t lp, bool u) { return ctl_proc(cpu, h, m, wp, lp, u, CTL_LIST); }

/* ---------------------------------------------------------------- native control events */

typedef struct lv_event {
    int row, col;
    uint32_t new_state, old_state, changed, param;
} lv_event;

/* NMITEMACTIVATE (48 bytes) and NMLISTVIEW (44 bytes) after NMHDR:
   iItem 12, iSubItem 16, uNewState 20, uOldState 24, uChanged 28,
   ptAction 32, lParam 40 (uKeyFlags 44). */
static void fill_lv(uint32_t nm, const void *ctx)
{
    const lv_event *e = ctx;
    wr32(nm + 12, (uint32_t)e->row);
    wr32(nm + 16, (uint32_t)e->col);
    wr32(nm + 20, e->new_state);
    wr32(nm + 24, e->old_state);
    wr32(nm + 28, e->changed);
    wr32(nm + 40, e->param);
}

static void list_select(_cpu *cpu, uint32_t hwnd, ctl *c, int row)
{
    int old = c->sel;
    lv_event e;
    if (old == row) return;
    c->sel = row;
    if (old >= 0 && old < c->nrows)
    {
        e = (lv_event){ old, 0, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIF_STATE, c->rows[old].param };
        send_notify(cpu, hwnd, 0xFFFFFF9Bu /* LVN_ITEMCHANGED */, 44, fill_lv, &e);
    }
    if (row >= 0 && row < c->nrows)
    {
        e = (lv_event){ row, 0, LVIS_SELECTED | LVIS_FOCUSED, 0, LVIF_STATE, c->rows[row].param };
        send_notify(cpu, hwnd, 0xFFFFFF9Bu, 44, fill_lv, &e);
    }
}

void rt_dialog_control_event(_cpu *cpu, uint32_t hwnd, int event, int32_t a, int32_t b)
{
    ctl *c;
    if (event == RT_CTL_CANCEL)
    {
        /* Escape in a dialog window: IDCANCEL. */
        uint32_t cancel = rt_user32_child_by_id(hwnd, IDCANCEL);
        if (!rt_user32_is_window(hwnd)) return;
        rt_user32_send(cpu, hwnd, WM_COMMAND, IDCANCEL, cancel, rt_user32_is_unicode(hwnd));
        return;
    }
    c = rt_user32_is_window(hwnd) ? ctl_of(hwnd) : NULL;
    if (c == NULL) return;
    switch (event)
    {
    case RT_CTL_CLICKED:
        if (c->type == CTL_BUTTON) button_click(cpu, hwnd, c);
        break;
    case RT_CTL_SELCHANGE:
        if (c->type != CTL_COMBO) break;
        c->cursel = a >= 0 && a < c->count ? a : -1;
        combo_text(hwnd, c);
        send_command(cpu, hwnd, 1); /* CBN_SELCHANGE */
        send_command(cpu, hwnd, 9); /* CBN_SELENDOK */
        break;
    case RT_CTL_TAB:
        if (c->type != CTL_TAB || a == c->cursel) break;
        if (send_notify(cpu, hwnd, 0xFFFFFDD8u /* TCN_SELCHANGING */, 12, NULL, NULL) != 0)
        {
            /* The program keeps the current tab. */
            if (c->view != NULL) rt_cocoa_tabs_select(c->view, c->cursel);
            break;
        }
        c->cursel = a >= 0 && a < c->count ? a : -1;
        send_notify(cpu, hwnd, 0xFFFFFDD9u /* TCN_SELCHANGE */, 12, NULL, NULL);
        break;
    case RT_CTL_LIST_CLICK:
    case RT_CTL_LIST_DBLCLICK:
    {
        lv_event e;
        if (c->type != CTL_LIST) break;
        if (a >= c->nrows) a = -1;
        if (a >= 0) list_select(cpu, hwnd, c, a);
        e = (lv_event){ a, a >= 0 ? b : 0, 0, 0, 0, a >= 0 ? c->rows[a].param : 0 };
        if (e.col < 0) e.col = 0;
        send_notify(cpu, hwnd, event == RT_CTL_LIST_CLICK ? 0xFFFFFFFEu /* NM_CLICK */ : 0xFFFFFFFDu /* NM_DBLCLK */, 48, fill_lv,
                    &e);
        break;
    }
    case RT_CTL_EDIT_CHANGE:
        if (c->type != CTL_EDIT) break;
        edit_sync(hwnd, c);
        send_command(cpu, hwnd, 0x0400); /* EN_UPDATE */
        send_command(cpu, hwnd, 0x0300); /* EN_CHANGE */
        break;
    default:
        rt_fatal("dialog: native control event %d is not known", event);
    }
}

/* ---------------------------------------------------------------- dialog window procedure */

static dialog *dlg_of(uint32_t hwnd)
{
    return rt_user32_class_data(hwnd);
}

/* Messages whose DLGPROC result is the message result. */
static bool direct_result(uint32_t msg)
{
    return msg == WM_INITDIALOG || (msg >= 0x0132 && msg <= 0x0138) /* WM_CTLCOLOR* */ || msg == 0x0039 /* WM_COMPAREITEM */ ||
           msg == 0x002E /* WM_VKEYTOITEM */ || msg == 0x002F /* WM_CHARTOITEM */ || msg == 0x0037 /* WM_QUERYDRAGICON */;
}

/* The first control of the dialog that can get the focus. */
static uint32_t first_tabstop(uint32_t hdlg)
{
    uint32_t kids[256];
    int n = rt_user32_children(hdlg, kids, 256), i;
    for (i = 0; i < n; i++)
    {
        uint32_t s = rt_user32_style(kids[i]);
        if ((s & WS_TABSTOP) && (s & WS_VISIBLE) && !(s & WS_DISABLED)) return kids[i];
    }
    return 0;
}

/* The next (or previous) control with WS_TABSTOP after from. */
static uint32_t next_tabstop(uint32_t hdlg, uint32_t from, bool prev)
{
    uint32_t kids[256];
    int n = rt_user32_children(hdlg, kids, 256), i, at = -1, k;
    for (i = 0; i < n; i++)
        if (kids[i] == from) at = i;
    for (k = 1; k <= n; k++)
    {
        int j = at < 0 ? (prev ? n - k : k - 1) : ((at + (prev ? -k : k)) % n + n) % n;
        uint32_t s = rt_user32_style(kids[j]);
        if ((s & WS_TABSTOP) && (s & WS_VISIBLE) && !(s & WS_DISABLED)) return kids[j];
    }
    return 0;
}

static uint32_t default_button_id(uint32_t hdlg)
{
    uint32_t kids[256];
    int n = rt_user32_children(hdlg, kids, 256), i;
    for (i = 0; i < n; i++)
    {
        ctl *c = ctl_of(kids[i]);
        if (c != NULL && c->type == CTL_BUTTON && (c->style & 0xFu) == 1u) return rt_user32_id(kids[i]);
    }
    return 0;
}

static uint32_t def_dlg(_cpu *cpu, uint32_t hwnd, dialog *d, uint32_t msg, uint32_t wp, uint32_t lp, bool unicode)
{
    switch (msg)
    {
    case WM_SETFONT:
        d->font = wp;
        return 0;
    case WM_GETFONT:
        return d->font;
    case WM_CLOSE:
    {
        uint32_t cancel = rt_user32_child_by_id(hwnd, IDCANCEL);
        if (cancel != 0 && (rt_user32_style(cancel) & WS_DISABLED)) return 0;
        rt_user32_post_message(hwnd, WM_COMMAND, IDCANCEL, cancel);
        return 0;
    }
    case DM_GETDEFID:
    {
        uint32_t id = default_button_id(hwnd);
        return id != 0 ? (id & 0xFFFFu) | (DC_HASDEFID << 16) : 0u;
    }
    case DM_SETDEFID:
        rt_fatal("DM_SETDEFID is not supported (the native default button cannot change)");
    case WM_NEXTDLGCTL:
    {
        uint32_t to = lp != 0 ? wp : next_tabstop(hwnd, rt_user32_focus(), wp != 0);
        if (to != 0) rt_user32_set_focus(cpu, to);
        return 0;
    }
    case WM_GETDLGCODE:
        return 0;
    case WM_ERASEBKGND:
        return 1;
    default:
        return rt_user32_default_proc(cpu, hwnd, msg, wp, lp, unicode);
    }
}

static uint32_t dialog_proc(_cpu *cpu, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp, bool unicode)
{
    dialog *d = dlg_of(hwnd);
    uint32_t proc, r;
    if (msg == WM_NCCREATE)
    {
        uint32_t cp = rd32(lp);
        d = calloc(1, sizeof *d);
        if (d == NULL) rt_fatal("out of host memory (dialog)");
        rt_user32_set_class_data(hwnd, d);
        /* The DLGPROC is set after the window exists (create_dialog): a
           dialog procedure gets no WM_NCCREATE and no WM_CREATE. */
        if (cp != 0 && rd32(cp) == DLG_MAGIC)
            d->unicode = rd32(cp + 4) != 0;
        else
            rt_fatal("dialog: the dialog class can be made only by CreateDialogParam or DialogBoxParam");
    }
    if (d == NULL) return rt_user32_default_proc(cpu, hwnd, msg, wp, lp, unicode);
    if (msg == WM_NULL && wp == RTNV)
    {
        /* A child dialog (DS_CONTROL page): a plain pane. */
        void *parent = rt_user32_native_view(rt_user32_parent(hwnd));
        if (parent != NULL && d->view == NULL)
        {
            d->view = rt_cocoa_view_create(hwnd, parent, RT_VIEW_PANE, rt_user32_child_rect(hwnd), "", RT_ALIGN_LEFT, false);
            rt_user32_set_native_view(hwnd, d->view);
            rt_cocoa_view_set_visible(d->view, (rt_user32_style(hwnd) & WS_VISIBLE) != 0);
        }
        return 0;
    }
    if (!rt_user32_get_extra(hwnd, DWLP_DLGPROC, &proc)) proc = 0;
    if (proc != 0)
    {
        r = rt_user32_call_proc(cpu, proc, hwnd, msg, wp, lp, unicode, d->unicode);
        if (!rt_user32_is_window(hwnd) || dlg_of(hwnd) != d) return r;
        if (r != 0)
        {
            uint32_t res = 0;
            if (direct_result(msg)) return r;
            (void)rt_user32_get_extra(hwnd, DWLP_MSGRESULT, &res);
            return res;
        }
    }
    if (msg == WM_NCDESTROY)
    {
        rt_user32_set_class_data(hwnd, NULL);
        free(d);
        return rt_user32_default_proc(cpu, hwnd, msg, wp, lp, unicode);
    }
    return def_dlg(cpu, hwnd, d, msg, wp, lp, unicode);
}

/* ---------------------------------------------------------------- dialog templates */

typedef struct reader {
    uint32_t base, size, pos;
    bool bad;
} reader;

static uint32_t r16(reader *r)
{
    uint32_t v;
    if (r->pos + 2u > r->size || r->pos + 2u < r->pos)
    {
        r->bad = true;
        return 0;
    }
    v = rd16(r->base + r->pos);
    r->pos += 2;
    return v;
}

static uint32_t r32(reader *r)
{
    uint32_t lo = r16(r);
    return lo | (r16(r) << 16);
}

/* A string or an ordinal: *ord gets the ordinal (0 for a string). Returns
   the UTF-8 string (malloc) or NULL for an ordinal. */
static char *r_sz_or_ord(reader *r, uint32_t *ord)
{
    uint32_t first = r16(r), start, n = 0, i;
    char *s;
    size_t o = 0;
    *ord = 0;
    if (r->bad) return NULL;
    if (first == 0) return xstrdup("");
    if (first == 0xFFFFu)
    {
        *ord = r16(r);
        return NULL;
    }
    start = r->pos - 2u;
    r->pos = start;
    for (;;)
    {
        uint32_t c = r16(r);
        if (r->bad) return NULL;
        if (c == 0) break;
        n++;
        if (n > 4096)
        {
            r->bad = true;
            return NULL;
        }
    }
    s = malloc(3u * (size_t)n + 1u);
    if (s == NULL) rt_fatal("out of host memory (dialog template)");
    for (i = 0; i < n; i++)
    {
        uint32_t c = rd16(r->base + start + 2u * i);
        if (c >= 0xD800u && c < 0xDC00u && i + 1 < n)
        {
            uint32_t lo = rd16(r->base + start + 2u * (i + 1));
            if (lo >= 0xDC00u && lo < 0xE000u)
            {
                c = 0x10000u + ((c - 0xD800u) << 10) + (lo - 0xDC00u);
                i++;
            }
        }
        if (c >= 0xD800u && c < 0xE000u) c = 0xFFFDu;
        o += rt_utf8_encode(c, s + o);
    }
    s[o] = 0;
    return s;
}

typedef struct titem {
    uint32_t style, exstyle, id, cls_ord, title_ord, extra;
    int16_t x, y, cx, cy;
    char *cls, *title;
} titem;

typedef struct tmpl {
    uint32_t style, exstyle, cls_ord, menu_ord;
    int16_t x, y, cx, cy;
    char *menu, *cls, *title;
    uint32_t font_pt;
    int n;
    titem *items;
} tmpl;

static void tmpl_free(tmpl *t)
{
    int i;
    for (i = 0; i < t->n; i++)
    {
        free(t->items[i].cls);
        free(t->items[i].title);
    }
    free(t->items);
    free(t->menu);
    free(t->cls);
    free(t->title);
}

static bool parse_template(uint32_t base, uint32_t size, tmpl *t)
{
    reader r = { base, size, 0, false };
    bool ex;
    uint32_t ord, i;
    memset(t, 0, sizeof *t);
    ex = size >= 4 && rd16(base) == 1 && rd16(base + 2) == 0xFFFFu;
    if (ex)
    {
        r.pos = 4;
        (void)r32(&r); /* helpID */
        t->exstyle = r32(&r);
        t->style = r32(&r);
    }
    else
    {
        t->style = r32(&r);
        t->exstyle = r32(&r);
    }
    t->n = (int)r16(&r);
    t->x = (int16_t)r16(&r);
    t->y = (int16_t)r16(&r);
    t->cx = (int16_t)r16(&r);
    t->cy = (int16_t)r16(&r);
    t->menu = r_sz_or_ord(&r, &t->menu_ord);
    t->cls = r_sz_or_ord(&r, &t->cls_ord);
    t->title = r_sz_or_ord(&r, &ord);
    if (t->title == NULL) t->title = xstrdup("");
    if (t->style & DS_SETFONT)
    {
        char *face;
        t->font_pt = r16(&r);
        if (ex)
        {
            (void)r16(&r); /* weight */
            (void)r16(&r); /* italic, charset */
        }
        face = r_sz_or_ord(&r, &ord);
        free(face);
    }
    if (r.bad || t->n > MAX_DIALOG_CONTROLS) return false;
    t->items = calloc((size_t)(t->n > 0 ? t->n : 1), sizeof *t->items);
    if (t->items == NULL) rt_fatal("out of host memory (dialog template)");
    for (i = 0; i < (uint32_t)t->n; i++)
    {
        titem *it = &t->items[i];
        r.pos = (r.pos + 3u) & ~3u;
        if (ex)
        {
            (void)r32(&r); /* helpID */
            it->exstyle = r32(&r);
            it->style = r32(&r);
        }
        else
        {
            it->style = r32(&r);
            it->exstyle = r32(&r);
        }
        it->x = (int16_t)r16(&r);
        it->y = (int16_t)r16(&r);
        it->cx = (int16_t)r16(&r);
        it->cy = (int16_t)r16(&r);
        it->id = ex ? r32(&r) : r16(&r);
        it->cls = r_sz_or_ord(&r, &it->cls_ord);
        it->title = r_sz_or_ord(&r, &it->title_ord);
        if (r.bad) return false;
        {
            uint32_t extra = r16(&r);
            it->extra = extra != 0 ? base + r.pos - 2u : 0;
            r.pos += extra;
        }
        if (r.bad || r.pos > size) return false;
    }
    return true;
}

static const char *ordinal_class(uint32_t ord)
{
    switch (ord)
    {
    case 0x80: return "Button";
    case 0x81: return "Edit";
    case 0x82: return "Static";
    case 0x83: return "ListBox";
    case 0x84: return "ScrollBar";
    case 0x85: return "ComboBox";
    default: return NULL;
    }
}

static bool supported_class(const char *name)
{
    static const char *const names[] = { "Button", "Edit", "Static", "ComboBox", "SysTabControl32", "SysListView32" };
    size_t i;
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcasecmp(names[i], name) == 0) return true;
    return false;
}

/* Make a dialog from the template of module (CreateDialogIndirectParam
   rules). Returns the dialog window or 0. */
static uint32_t create_dialog(_cpu *cpu, uint32_t hinst, uint32_t data, uint32_t size, uint32_t owner, uint32_t proc,
                              uint32_t param, bool unicode, modal_state *modal)
{
    tmpl t;
    int bx, by, i;
    uint32_t style, exstyle, hwnd, cp, font, focus;
    int32_t x, y, w, h;
    bool child, titled;
    dialog *d;
    if (!parse_template(data, size, &t))
    {
        tmpl_free(&t);
        rt_set_last_error(ERROR_RESOURCE_DATA_NOT_FOUND);
        return 0;
    }
    if (t.menu == NULL || t.menu[0] != 0) rt_fatal("dialog: dialog menus are not supported");
    if (t.cls == NULL || t.cls[0] != 0) rt_fatal("dialog: a dialog with its own window class is not supported");
    if (t.style & DS_CENTERMOUSE) rt_fatal("dialog: DS_CENTERMOUSE is not supported");
    base_units(t.font_pt, &bx, &by);
    style = t.style & ~WS_VISIBLE;
    exstyle = t.exstyle;
    if (t.style & DS_MODALFRAME) exstyle |= WS_EX_DLGMODALFRAME;
    if (t.style & DS_CONTROL)
    {
        style &= ~(WS_CAPTION | WS_SYSMENU);
        exstyle |= WS_EX_CONTROLPARENT;
    }
    child = (style & WS_CHILD) != 0;
    titled = !child && (style & WS_CAPTION) == WS_CAPTION;
    w = dlu_x(t.cx, bx);
    h = dlu_y(t.cy, by) + (titled ? rt_cocoa_title_height() : 0);
    x = dlu_x(t.x, bx);
    y = dlu_y(t.y, by);
    if (child)
    {
        if (!rt_user32_is_window(owner))
        {
            tmpl_free(&t);
            rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
            return 0;
        }
    }
    else
    {
        rt_display disp[RT_MAX_DISPLAYS];
        rt_rect work;
        int nd = rt_cocoa_displays(disp);
        (void)nd;
        work = disp[0].work;
        if (owner != 0 && !rt_user32_is_window(owner)) owner = 0;
        if (t.style & DS_CENTER)
        {
            x = work.left + ((work.right - work.left) - w) / 2;
            y = work.top + ((work.bottom - work.top) - h) / 2;
        }
        else if (!(t.style & DS_ABSALIGN) && owner != 0)
        {
            /* Relative to the client area of the owner. */
            int32_t ox = 0, oy = 0;
            rt_user32_client_origin(owner, &ox, &oy);
            x += ox;
            y += oy;
        }
        /* Windows keeps a dialog box in the work area. */
        if (x + w > work.right) x = work.right - w;
        if (y + h > work.bottom) y = work.bottom - h;
        if (x < work.left) x = work.left;
        if (y < work.top) y = work.top;
    }
    cp = rt_heap_calloc(1, 8);
    if (cp == 0) rt_fatal("out of guest memory (dialog)");
    wr32(cp, DLG_MAGIC);
    wr32(cp + 4, unicode ? 1u : 0u);
    hwnd = rt_user32_create(cpu, exstyle, "#32770", t.title, style, x, y, w, h, owner, 0, hinst, cp, unicode);
    rt_heap_free(cp);
    if (hwnd == 0)
    {
        tmpl_free(&t);
        return 0;
    }
    d = dlg_of(hwnd);
    if (d == NULL) rt_fatal("dialog: the dialog state of 0x%08x is missing", hwnd);
    d->modal = modal;
    rt_user32_set_extra(hwnd, DWLP_DLGPROC, proc);
    font = rt_gdi_dialog_font();
    if (t.style & DS_SETFONT) rt_user32_send(cpu, hwnd, WM_SETFONT, font, 0, unicode);
    for (i = 0; i < t.n; i++)
    {
        titem *it = &t.items[i];
        const char *cls = it->cls != NULL ? it->cls : ordinal_class(it->cls_ord);
        uint32_t c, image = 0, image_type = 0;
        if (cls == NULL) rt_fatal("dialog: control class ordinal 0x%x is not known", it->cls_ord);
        if (!supported_class(cls)) rt_fatal("dialog: control class %s is not supported", cls);
        if (it->title == NULL)
        {
            /* An ordinal title: the icon or bitmap of a static control. */
            uint32_t ss = it->style & 0x1Fu;
            if (strcasecmp(cls, "Static") != 0 || (ss != 3u && ss != 0xEu))
                rt_fatal("dialog: an ordinal title for a %s control is not supported", cls);
            image_type = ss == 3u ? IMAGE_ICON : IMAGE_BITMAP;
            image = ss == 3u ? rt_user32_load_icon(hinst, it->title_ord) : rt_user32_load_bitmap(hinst, it->title_ord);
        }
        c = rt_user32_create(cpu, it->exstyle | WS_EX_NOPARENTNOTIFY, cls, it->title != NULL ? it->title : "",
                             it->style | WS_CHILD, dlu_x(it->x, bx), dlu_y(it->y, by), dlu_x(it->cx, bx), dlu_y(it->cy, by), hwnd,
                             it->id & 0xFFFFu, hinst, it->extra, unicode);
        if (c == 0)
        {
            rt_user32_destroy(cpu, hwnd);
            tmpl_free(&t);
            return 0;
        }
        rt_user32_send(cpu, c, WM_SETFONT, font, 0, unicode);
        if (image != 0) rt_user32_send(cpu, c, 0x0172 /* STM_SETIMAGE */, image_type, image, unicode);
    }
    focus = first_tabstop(hwnd);
    if (rt_user32_send(cpu, hwnd, WM_INITDIALOG, focus, param, unicode) != 0 && rt_user32_is_window(hwnd) && focus != 0 &&
        rt_user32_is_window(focus))
        rt_user32_set_focus(cpu, focus);
    if (rt_user32_is_window(hwnd) && (t.style & WS_VISIBLE) && modal == NULL)
    {
        d = dlg_of(hwnd);
        if (d != NULL && !d->ended) rt_user32_show(cpu, hwnd, SW_SHOWNORMAL);
    }
    tmpl_free(&t);
    return rt_user32_is_window(hwnd) ? hwnd : 0;
}

static bool find_template(uint32_t hinst, uint32_t name, bool wide, uint32_t *data, uint32_t *size)
{
    if (!rt_resource_find(hinst, RT_RES_DIALOG, name, wide, data, size))
    {
        rt_set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND);
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------- dialog functions */

static void create_dialog_param(_cpu *cpu, bool wide)
{
    uint32_t data, size;
    if (!find_template(ARG(0), ARG(1), wide, &data, &size))
    {
        RET(0);
        return;
    }
    RET(create_dialog(cpu, ARG(0), data, size, ARG(2), ARG(3), ARG(4), wide, NULL));
}

/* HWND CreateDialogParamA(HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM) */
void rt_CreateDialogParamA(_cpu *cpu) { create_dialog_param(cpu, false); }
/* HWND CreateDialogParamW(HINSTANCE, LPCWSTR, HWND, DLGPROC, LPARAM) */
void rt_CreateDialogParamW(_cpu *cpu) { create_dialog_param(cpu, true); }

static bool is_dialog_message(_cpu *cpu, uint32_t hdlg, uint32_t pmsg, bool unicode);

static void dialog_box_param(_cpu *cpu, bool wide)
{
    uint32_t data, size, hwnd, owner = ARG(2), msg, result = 0;
    modal_state m = { false, 0 };
    dialog *d;
    if (!find_template(ARG(0), ARG(1), wide, &data, &size))
    {
        RET(0xFFFFFFFFu);
        return;
    }
    if (owner != 0 && !rt_user32_is_window(owner))
    {
        rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        RET(0);
        return;
    }
    /* The owner of a dialog box is a top-level window. */
    while (owner != 0 && (rt_user32_style(owner) & WS_CHILD)) owner = rt_user32_parent(owner);
    hwnd = create_dialog(cpu, ARG(0), data, size, owner, ARG(3), ARG(4), wide, &m);
    if (hwnd == 0)
    {
        RET(0xFFFFFFFFu);
        return;
    }
    d = dlg_of(hwnd);
    if (owner != 0 && !(rt_user32_style(owner) & WS_DISABLED))
    {
        rt_user32_enable(cpu, owner, false);
        if (d != NULL) d->disabled_owner = owner;
    }
    if (!m.ended && rt_user32_is_window(hwnd)) rt_user32_show(cpu, hwnd, SW_SHOWNORMAL);
    msg = rt_heap_alloc(28);
    if (msg == 0) rt_fatal("out of guest memory (dialog box)");
    while (!m.ended && rt_user32_is_window(hwnd))
    {
        if (!rt_user32_get_message(cpu, msg, wide))
        {
            /* WM_QUIT ends the dialog box; the next loop gets it again. */
            rt_user32_post_quit(rd32(msg + 8));
            break;
        }
        if (!is_dialog_message(cpu, hwnd, msg, wide))
        {
            rt_user32_translate(cpu, msg);
            rt_user32_dispatch(cpu, msg, wide);
        }
    }
    rt_heap_free(msg);
    result = m.ended ? m.result : 0;
    d = rt_user32_is_window(hwnd) ? dlg_of(hwnd) : NULL;
    if (d != NULL)
    {
        d->modal = NULL;
        if (d->disabled_owner != 0 && rt_user32_is_window(d->disabled_owner)) rt_user32_enable(cpu, d->disabled_owner, true);
        d->disabled_owner = 0;
    }
    else if (owner != 0 && rt_user32_is_window(owner))
        rt_user32_enable(cpu, owner, true);
    if (rt_user32_is_window(hwnd)) rt_user32_destroy(cpu, hwnd);
    RET(result);
}

/* INT_PTR DialogBoxParamA(HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM) */
void rt_DialogBoxParamA(_cpu *cpu) { dialog_box_param(cpu, false); }
/* INT_PTR DialogBoxParamW(HINSTANCE, LPCWSTR, HWND, DLGPROC, LPARAM) */
void rt_DialogBoxParamW(_cpu *cpu) { dialog_box_param(cpu, true); }

/* BOOL EndDialog(HWND, INT_PTR) */
void rt_EndDialog(_cpu *cpu)
{
    uint32_t hwnd = ARG(0);
    dialog *d = rt_user32_is_window(hwnd) ? dlg_of(hwnd) : NULL;
    if (d == NULL)
    {
        rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        RET(0);
        return;
    }
    d->ended = true;
    d->result = ARG(1);
    if (d->modal != NULL)
    {
        d->modal->ended = true;
        d->modal->result = ARG(1);
    }
    if (d->disabled_owner != 0)
    {
        if (rt_user32_is_window(d->disabled_owner)) rt_user32_enable(cpu, d->disabled_owner, true);
        d->disabled_owner = 0;
    }
    rt_user32_show(cpu, hwnd, SW_HIDE);
    RET(1);
}

/* HWND GetDlgItem(HWND, int) */
void rt_GetDlgItem(_cpu *cpu)
{
    uint32_t c;
    if (!rt_user32_is_window(ARG(0)))
    {
        rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        RET(0);
        return;
    }
    c = rt_user32_child_by_id(ARG(0), ARG(1) & 0xFFFFu);
    if (c == 0) rt_set_last_error(ERROR_CONTROL_ID_NOT_FOUND);
    RET(c);
}

static uint32_t dlg_item(uint32_t hdlg, uint32_t id)
{
    uint32_t c = rt_user32_is_window(hdlg) ? rt_user32_child_by_id(hdlg, id & 0xFFFFu) : 0;
    if (c == 0) rt_set_last_error(rt_user32_is_window(hdlg) ? ERROR_CONTROL_ID_NOT_FOUND : ERROR_INVALID_WINDOW_HANDLE);
    return c;
}

/* UINT IsDlgButtonChecked(HWND, int) */
void rt_IsDlgButtonChecked(_cpu *cpu)
{
    uint32_t c = dlg_item(ARG(0), ARG(1));
    RET(c != 0 ? rt_user32_send(cpu, c, 0x00F0 /* BM_GETCHECK */, 0, 0, false) : 0u);
}

/* BOOL CheckDlgButton(HWND, int, UINT) */
void rt_CheckDlgButton(_cpu *cpu)
{
    uint32_t c = dlg_item(ARG(0), ARG(1));
    if (c == 0)
    {
        RET(0);
        return;
    }
    rt_user32_send(cpu, c, 0x00F1 /* BM_SETCHECK */, ARG(2), 0, false);
    RET(1);
}

/* LRESULT SendDlgItemMessageW(HWND, int, UINT, WPARAM, LPARAM) */
void rt_SendDlgItemMessageW(_cpu *cpu)
{
    uint32_t c = dlg_item(ARG(0), ARG(1));
    RET(c != 0 ? rt_user32_send(cpu, c, ARG(2), ARG(3), ARG(4), true) : 0u);
}

static void set_dlg_item_text(_cpu *cpu, bool wide)
{
    uint32_t c = dlg_item(ARG(0), ARG(1));
    RET(c != 0 ? rt_user32_send(cpu, c, WM_SETTEXT, 0, ARG(2), wide) : 0u);
}

/* BOOL SetDlgItemTextA(HWND, int, LPCSTR) */
void rt_SetDlgItemTextA(_cpu *cpu) { set_dlg_item_text(cpu, false); }
/* BOOL SetDlgItemTextW(HWND, int, LPCWSTR) */
void rt_SetDlgItemTextW(_cpu *cpu) { set_dlg_item_text(cpu, true); }

/* Keyboard navigation of a dialog box for a message of the dialog or one
   of its controls (the native AppKit controls handle their own keys; this
   handles the key messages of the runtime windows). */
static bool is_dialog_message(_cpu *cpu, uint32_t hdlg, uint32_t pmsg, bool unicode)
{
    uint32_t hwnd = rd32(pmsg), msg = rd32(pmsg + 4), wp = rd32(pmsg + 8);
    if (!rt_user32_is_window(hdlg) || hwnd == 0 || !rt_user32_is_self_or_child(hdlg, hwnd)) return false;
    if (msg == WM_KEYDOWN && (wp == VK_TAB || wp == VK_RETURN || wp == VK_ESCAPE))
    {
        uint32_t code = rt_user32_send(cpu, hwnd, WM_GETDLGCODE, wp, pmsg, unicode);
        if (!(code & DLGC_WANTALLKEYS) && !(wp == VK_TAB && (code & DLGC_WANTTAB)))
        {
            if (wp == VK_TAB)
            {
                uint32_t to = next_tabstop(hdlg, rt_user32_focus(), false);
                if (to != 0) rt_user32_set_focus(cpu, to);
            }
            else if (wp == VK_ESCAPE)
                rt_user32_send(cpu, hdlg, WM_COMMAND, IDCANCEL, rt_user32_child_by_id(hdlg, IDCANCEL), unicode);
            else
            {
                uint32_t def = rt_user32_send(cpu, hdlg, DM_GETDEFID, 0, 0, unicode);
                uint32_t id = (def >> 16) == DC_HASDEFID ? (def & 0xFFFFu) : IDOK;
                rt_user32_send(cpu, hdlg, WM_COMMAND, id, rt_user32_child_by_id(hdlg, id), unicode);
            }
            return true;
        }
    }
    rt_user32_translate(cpu, pmsg);
    rt_user32_dispatch(cpu, pmsg, unicode);
    return true;
}

/* BOOL IsDialogMessageW(HWND, LPMSG) */
void rt_IsDialogMessageW(_cpu *cpu)
{
    if (ARG(1) == 0)
    {
        RET(0);
        return;
    }
    RET(is_dialog_message(cpu, ARG(0), ARG(1), true) ? 1u : 0u);
}

/* ---------------------------------------------------------------- start-up */

void rt_dialog_init(void)
{
    rt_user32_register_runtime_class("#32770", dialog_proc, DLGWINDOWEXTRA);
    rt_user32_register_runtime_class("Button", button_proc, 0);
    rt_user32_register_runtime_class("Static", static_proc, 0);
    rt_user32_register_runtime_class("Edit", edit_proc, 0);
    rt_user32_register_runtime_class("ComboBox", combo_proc, 0);
    rt_user32_register_runtime_class("SysTabControl32", tab_proc, 0);
    rt_user32_register_runtime_class("SysListView32", list_proc, 0);
}
