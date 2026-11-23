/*
 * IMM32: the input method manager.
 *
 * The input method is the macOS text input system of the window view
 * (rt_window.m, NSTextInputClient). USER32 keeps the input context state
 * (rt_user32.c): every thread has one input context, a window has the
 * context of its thread or no context, and a context is open or closed.
 * The input method of a window is active while the window has the
 * context and the context is open.
 *
 * Windows reopens a closed input method when the user presses the IME
 * key. On macOS the user selects a keyboard input source: that opens the
 * context of the main thread again.
 */
#include "rt.h"
#include "rt_thread.h"
#include "rt_user32.h"
#include "rt_window.h"

#include <string.h>

#define GCS_COMPSTR 0x0008u
#define GCS_CURSORPOS 0x0080u
#define GCS_RESULTSTR 0x0800u

#define SCS_SETSTR 0x0009u

#define IACE_CHILDREN 0x0001u
#define IACE_DEFAULT 0x0010u
#define IACE_IGNORENOCONTEXT 0x0020u

#define IME_CMODE_ALPHANUMERIC 0x0000u
#define IME_CMODE_NATIVE 0x0001u
#define IME_CMODE_FULLSHAPE 0x0008u
#define IME_SMODE_NONE 0x0000u

#define IMN_SETCONVERSIONMODE 0x0006u
#define IMN_SETOPENSTATUS 0x0008u

#define IMM_ERROR_GENERAL 0xFFFFFFFEu

#define MAX_TREE_DEPTH 64

/* HIMC ImmGetContext(HWND) */
void rt_ImmGetContext(_cpu *cpu)
{
    RET(rt_user32_ime_context(ARG(0)));
}

/* BOOL ImmReleaseContext(HWND, HIMC) */
void rt_ImmReleaseContext(_cpu *cpu)
{
    /* The context belongs to the thread; release does not free it. */
    RET(rt_user32_is_window(ARG(0)) && rt_user32_ime_valid(ARG(1)) ? 1u : 0u);
}

/* HIMC ImmAssociateContext(HWND, HIMC): returns the previous context. */
void rt_ImmAssociateContext(_cpu *cpu)
{
    uint32_t hwnd = ARG(0), himc = ARG(1), prev;
    if (!rt_user32_is_window(hwnd))
    {
        RET(0);
        return;
    }
    prev = rt_user32_ime_context(hwnd);
    if (!rt_user32_ime_associate(hwnd, himc))
    {
        RET(0);
        return;
    }
    RET(prev);
}

/* Apply a context to hwnd and (children) to all its descendants. */
static bool associate_tree(uint32_t hwnd, uint32_t himc, bool use_default, bool children, bool ignore_none, int depth)
{
    uint32_t kids[256], ctx;
    int n, i;
    if (depth > MAX_TREE_DEPTH) rt_fatal("ImmAssociateContextEx: the window tree is deeper than %d windows", MAX_TREE_DEPTH);
    if (!(ignore_none && rt_user32_ime_context(hwnd) == 0))
    {
        ctx = use_default ? rt_user32_ime_window_thread_context(hwnd) : himc;
        if (!rt_user32_ime_associate(hwnd, ctx)) return false;
    }
    if (!children) return true;
    n = rt_user32_children(hwnd, kids, 256);
    for (i = 0; i < n; i++)
        if (rt_user32_is_window(kids[i]) && !associate_tree(kids[i], himc, use_default, true, ignore_none, depth + 1)) return false;
    return true;
}

/* BOOL ImmAssociateContextEx(HWND, HIMC, DWORD flags) */
void rt_ImmAssociateContextEx(_cpu *cpu)
{
    uint32_t hwnd = ARG(0), himc = ARG(1), flags = ARG(2);
    if (flags & ~(IACE_CHILDREN | IACE_DEFAULT | IACE_IGNORENOCONTEXT))
        rt_fatal("ImmAssociateContextEx: flags 0x%x are not supported", flags);
    if (!rt_user32_is_window(hwnd))
    {
        RET(0);
        return;
    }
    if (!(flags & IACE_DEFAULT) && himc != 0 && !rt_user32_ime_valid(himc))
    {
        RET(0);
        return;
    }
    RET(associate_tree(hwnd, himc, (flags & IACE_DEFAULT) != 0, (flags & IACE_CHILDREN) != 0, (flags & IACE_IGNORENOCONTEXT) != 0, 0)
            ? 1u
            : 0u);
}

/* Tell the focus window of the calling thread about a state change of its
   context. */
static void notify_focus(_cpu *cpu, uint32_t himc, uint32_t what)
{
    uint32_t focus = rt_user32_focus();
    if (focus != 0 && rt_user32_ime_context(focus) == himc)
        rt_user32_send(cpu, focus, WM_IME_NOTIFY, what, 0, rt_user32_is_unicode(focus));
}

/* BOOL ImmSetOpenStatus(HIMC, BOOL) */
void rt_ImmSetOpenStatus(_cpu *cpu)
{
    uint32_t himc = ARG(0);
    bool open = ARG(1) != 0, changed = false;
    if (!rt_user32_ime_valid(himc))
    {
        RET(0);
        return;
    }
    /* Closing the input method ends the current composition. */
    if (!open) rt_user32_ime_cancel(himc);
    if (!rt_user32_ime_set_open(himc, open, &changed))
    {
        RET(0);
        return;
    }
    if (changed) notify_focus(cpu, himc, IMN_SETOPENSTATUS);
    RET(1);
}

/* BOOL ImmGetConversionStatus(HIMC, LPDWORD conversion, LPDWORD sentence) */
void rt_ImmGetConversionStatus(_cpu *cpu)
{
    uint32_t himc = ARG(0), pconv = ARG(1), psent = ARG(2);
    if (!rt_user32_ime_valid(himc))
    {
        RET(0);
        return;
    }
    /* The conversion mode is a property of the input source: an input
       method that types characters that are not ASCII is in the native
       (full-width) mode; a keyboard layout is in the alphanumeric mode. */
    if (pconv != 0)
        wr32(pconv, rt_cocoa_input_source_native() ? (IME_CMODE_NATIVE | IME_CMODE_FULLSHAPE) : IME_CMODE_ALPHANUMERIC);
    if (psent != 0) wr32(psent, IME_SMODE_NONE);
    RET(1);
}

/* LONG ImmGetCompositionStringW(HIMC, DWORD index, LPVOID buf, DWORD len):
   the size in bytes, or the number of bytes copied. */
void rt_ImmGetCompositionStringW(_cpu *cpu)
{
    uint32_t himc = ARG(0), index = ARG(1), buf = ARG(2), len = ARG(3), bytes, i;
    const uint16_t *comp, *result, *text;
    int ncomp, caret, nresult, n;
    if (!rt_user32_ime_valid(himc))
    {
        RET(0);
        return;
    }
    rt_user32_ime_strings(himc, &comp, &ncomp, &caret, &result, &nresult);
    switch (index)
    {
    case GCS_CURSORPOS:
        RET((uint32_t)caret);
        return;
    case GCS_COMPSTR:
        text = comp;
        n = ncomp;
        break;
    case GCS_RESULTSTR:
        text = result;
        n = nresult;
        break;
    default:
        /* The reading strings, the attributes and the clauses of the
           composition: the macOS text input system does not give them. */
        rt_fatal("ImmGetCompositionStringW: index 0x%x is not supported (only GCS_COMPSTR, GCS_RESULTSTR, GCS_CURSORPOS)", index);
    }
    if (n < 0 || n > 4096) rt_fatal("ImmGetCompositionStringW: composition of %d characters", n);
    bytes = (uint32_t)n * 2u;
    if (len == 0)
    {
        RET(bytes);
        return;
    }
    if (buf == 0)
    {
        RET(IMM_ERROR_GENERAL);
        return;
    }
    if (bytes > len) bytes = len & ~1u;
    for (i = 0; i < bytes / 2u; i++) wr16(buf + 2u * i, text[i]);
    RET(bytes);
}

/* BOOL ImmSetCompositionStringW(HIMC, DWORD index, LPVOID comp, DWORD
   comp_len, LPVOID read, DWORD read_len) */
void rt_ImmSetCompositionStringW(_cpu *cpu)
{
    uint32_t himc = ARG(0), index = ARG(1), comp = ARG(2), comp_len = ARG(3), read = ARG(4), read_len = ARG(5);
    if (!rt_user32_ime_valid(himc))
    {
        RET(0);
        return;
    }
    if (index != SCS_SETSTR) rt_fatal("ImmSetCompositionStringW: index 0x%x is not supported (only SCS_SETSTR)", index);
    if ((comp != 0 && comp_len != 0) || (read != 0 && read_len != 0))
        rt_fatal("ImmSetCompositionStringW: text for the composition is not supported (the macOS input method owns the "
                 "marked text; only an empty string is supported)");
    /* An empty composition string: discard the composition. */
    rt_user32_ime_cancel(himc);
    RET(1);
}

/* ---------------------------------------------------------------- input source changes */

static void source_changed(_cpu *cpu, void *ctx)
{
    uint32_t focus = rt_user32_focus(), himc;
    bool changed = false;
    (void)ctx;
    if (focus == 0) return;
    himc = rt_user32_ime_window_thread_context(focus);
    if (himc == 0) return;
    /* The user selected an input source: as the IME key on Windows, this
       opens the input method. */
    if (!rt_user32_ime_set_open(himc, true, &changed)) return;
    if (changed) notify_focus(cpu, himc, IMN_SETOPENSTATUS);
    notify_focus(cpu, himc, IMN_SETCONVERSIONMODE);
}

void rt_imm32_input_source_changed(void)
{
    rt_user32_main_call(source_changed, NULL);
}
