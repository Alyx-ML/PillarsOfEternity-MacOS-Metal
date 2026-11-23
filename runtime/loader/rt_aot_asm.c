/*
 * The conversion of the Mono AOT assembler output (see rt_aot_asm.h).
 *
 * Pass 1 finds the PLT label of mono_string_new_wrapper: a line ".Lp_<n>:"
 * that is at most 3 lines before the line
 * "plt__jit_icall_mono_string_new_wrapper:". Pass 2 writes the output. It
 * keeps the last ".byte" lines in a buffer, because the 9 bytes
 * "131,236,12,104,b0,b1,b2,b3,232" (sub esp, 12; push imm32; call) before a
 * call operand ".long .Lp_<n> - . -4" can continue from one ".byte" line
 * to the next.
 */
#include "rt_aot_asm.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE (1u << 20)
#define MAX_BYTES_PER_LINE 256u
#define PENDING_LINES 16u
#define PATTERN_LEN 9u
#define PLT_SYMBOL "plt__jit_icall_mono_string_new_wrapper:"
#define MAX_EXPORTS (1u << 20)

typedef struct byte_line {
    char prefix[16];                       /* the white space before ".byte" */
    unsigned n;
    uint8_t v[MAX_BYTES_PER_LINE];
    bool changed;
    char *text;                            /* the line as read (with "\n") */
} byte_line;

typedef struct conv {
    FILE *out;
    byte_line pending[PENDING_LINES];
    unsigned npending;
    char target[64];                       /* ".long .Lp_<n> - . -4", or "" */
    char **exports;
    unsigned nexports, cap_exports;
    rt_aot_asm_stats *stats;
    char *err;
    size_t errn;
    const char *in_path, *out_path;
} conv;

static bool fail(conv *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static bool fail(conv *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, c->errn, fmt, ap);
    va_end(ap);
    return false;
}

static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* The line without the end of line characters, for comparisons. */
static size_t content_len(const char *s, size_t n)
{
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) n--;
    return n;
}

/* Does the directive d start at s, followed by white space or the end of
   the line? */
static bool is_directive(const char *s, const char *d)
{
    size_t n = strlen(d);
    return strncmp(s, d, n) == 0 && (s[n] == ' ' || s[n] == '\t' || s[n] == '\n' || s[n] == '\r' || s[n] == 0);
}

/* Parse "<ws>.byte v,v,...": decimal values 0 to 255 only. */
static bool parse_byte_line(const char *line, byte_line *b)
{
    const char *s = skip_ws(line), *p;
    size_t plen = (size_t)(s - line);
    if (!is_directive(s, ".byte") || plen >= sizeof b->prefix) return false;
    memcpy(b->prefix, line, plen);
    b->prefix[plen] = 0;
    p = skip_ws(s + 5);
    b->n = 0;
    for (;;)
    {
        unsigned v = 0, digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 4)
        {
            v = v * 10u + (unsigned)(*p - '0');
            p++;
            digits++;
        }
        if (digits == 0 || digits == 4 || v > 255 || b->n == MAX_BYTES_PER_LINE) return false;
        b->v[b->n++] = (uint8_t)v;
        if (*p == ',')
        {
            p++;
            continue;
        }
        break;
    }
    p = skip_ws(p);
    return *p == '\n' || *p == '\r' || *p == 0;
}

static bool write_text(conv *c, const char *s)
{
    if (fputs(s, c->out) < 0) return fail(c, "cannot write %s: %s", c->out_path, strerror(errno));
    return true;
}

static bool write_byte_line(conv *c, byte_line *b)
{
    unsigned i;
    bool ok = true;
    if (!b->changed) ok = write_text(c, b->text);
    else
    {
        if (fprintf(c->out, "%s.byte ", b->prefix) < 0) ok = false;
        for (i = 0; ok && i < b->n; i++)
            if (fprintf(c->out, i == 0 ? "%u" : ",%u", b->v[i]) < 0) ok = false;
        /* The line end of the original line (the compiler writes "\r\n"). */
        if (ok && fputs(strchr(b->text, '\r') != NULL ? "\r\n" : "\n", c->out) < 0) ok = false;
        if (!ok) fail(c, "cannot write %s: %s", c->out_path, strerror(errno));
    }
    free(b->text);
    b->text = NULL;
    return ok;
}

static bool flush_pending(conv *c, unsigned keep)
{
    unsigned k, drop;
    if (c->npending <= keep) return true;
    drop = c->npending - keep;
    for (k = 0; k < drop; k++)
        if (!write_byte_line(c, &c->pending[k])) return false;
    for (k = 0; k < keep; k++) c->pending[k] = c->pending[drop + k];
    c->npending = keep;
    return true;
}

/* The call operand of mono_string_new_wrapper follows the pending lines:
   check the 9 bytes before it. */
static void check_call(conv *c)
{
    uint8_t *pos[PATTERN_LEN];
    bool *changed[PATTERN_LEN];
    static const int want[PATTERN_LEN] = { 131, 236, 12, 104, -1, -1, -1, -1, 232 };
    unsigned got = 0, k, i;
    uint32_t v;
    for (k = c->npending; k > 0 && got < PATTERN_LEN; k--)
    {
        byte_line *b = &c->pending[k - 1];
        for (i = b->n; i > 0 && got < PATTERN_LEN; i--)
        {
            pos[PATTERN_LEN - 1 - got] = &b->v[i - 1];
            changed[PATTERN_LEN - 1 - got] = &b->changed;
            got++;
        }
    }
    if (got < PATTERN_LEN) return;
    for (i = 0; i < PATTERN_LEN; i++)
        if (want[i] >= 0 && *pos[i] != want[i]) return;
    v = (uint32_t)*pos[4] | (uint32_t)*pos[5] << 8 | (uint32_t)*pos[6] << 16 | (uint32_t)*pos[7] << 24;
    if (v >= RT_AOT_MONO_BASE && v < RT_AOT_MONO_END)
    {
        c->stats->pointers_kept++;
        return;
    }
    for (i = 4; i < 8; i++)
    {
        *pos[i] = 0;
        *changed[i] = true;
    }
    c->stats->pointers_zeroed++;
}

static bool add_export(conv *c, const char *s)
{
    const char *e = s;
    size_t n;
    char *name;
    while (*e != 0 && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r') e++;
    n = (size_t)(e - s);
    if (n == 0) return fail(c, "%s has a .globl directive without a name", c->in_path);
    if (s[0] != '_') return fail(c, "%s: the global symbol %.*s has no C prefix \"_\"", c->in_path, (int)n, s);
    if (c->nexports == c->cap_exports)
    {
        unsigned cap = c->cap_exports == 0 ? 256u : c->cap_exports * 2u;
        char **p;
        if (cap > MAX_EXPORTS) return fail(c, "%s has more than %u global symbols", c->in_path, MAX_EXPORTS);
        p = realloc(c->exports, cap * sizeof *p);
        if (p == NULL) return fail(c, "out of memory");
        c->exports = p;
        c->cap_exports = cap;
    }
    name = malloc(n);
    if (name == NULL) return fail(c, "out of memory");
    memcpy(name, s + 1, n - 1);
    name[n - 1] = 0;
    c->exports[c->nexports++] = name;
    return true;
}

/* One line that is not a parsed ".byte" line. */
static bool other_line(conv *c, char *line, size_t len)
{
    const char *s = skip_ws(line);
    size_t clen = content_len(line, len);
    if (c->target[0] != 0 && clen - (size_t)(s - line) == strlen(c->target) && memcmp(s, c->target, strlen(c->target)) == 0)
        check_call(c);
    if (!flush_pending(c, 0)) return false;
    if (is_directive(s, ".local") || is_directive(s, ".size"))
    {
        c->stats->lines_removed++;
        return true;
    }
    if (is_directive(s, ".section"))
    {
        const char *n = skip_ws(s + 8), *e, *rest;
        size_t nl;
        bool known;
        e = n;
        while (*e != 0 && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r') e++;
        nl = (size_t)(e - n);
        known = (nl == 5 && (strncmp(n, ".text", 5) == 0 || strncmp(n, ".data", 5) == 0)) ||
                (nl == 4 && strncmp(n, ".bss", 4) == 0);
        rest = skip_ws(e);
        if (!known || (*rest != '\n' && *rest != '\r' && *rest != 0))
            return fail(c, "%s has a section other than .text, .data and .bss: %.*s", c->in_path, (int)clen, line);
    }
    if (is_directive(s, ".globl") && !add_export(c, skip_ws(s + 6))) return false;
    if (is_directive(s, ".hword"))
    {
        if (fprintf(c->out, "%.*s.short%s", (int)(s - line), line, s + 6) < 0)
            return fail(c, "cannot write %s: %s", c->out_path, strerror(errno));
        return true;
    }
    return write_text(c, line);
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Pass 1: the label of the PLT entry of mono_string_new_wrapper. */
static bool find_target(conv *c, FILE *in)
{
    char *line = NULL, label[32] = "";
    size_t cap = 0;
    ssize_t len;
    unsigned long lineno = 0, label_line = 0, found = 0;
    while ((len = getline(&line, &cap, in)) >= 0)
    {
        size_t clen = content_len(line, (size_t)len);
        lineno++;
        if ((size_t)len > MAX_LINE)
        {
            free(line);
            return fail(c, "%s line %lu is too long", c->in_path, lineno);
        }
        if (clen > 5 && clen < 5 + 20 && strncmp(line, ".Lp_", 4) == 0 && line[clen - 1] == ':' &&
            strspn(line + 4, "0123456789") == clen - 5)
        {
            memcpy(label, line, clen - 1);
            label[clen - 1] = 0;
            label_line = lineno;
        }
        else if (clen == strlen(PLT_SYMBOL) && memcmp(line, PLT_SYMBOL, clen) == 0)
        {
            if (label[0] == 0 || lineno - label_line > 3)
            {
                free(line);
                return fail(c, "%s line %lu: the PLT entry of mono_string_new_wrapper has no .Lp label", c->in_path, lineno);
            }
            if (found++ != 0)
            {
                free(line);
                return fail(c, "%s has two PLT entries of mono_string_new_wrapper", c->in_path);
            }
            snprintf(c->target, sizeof c->target, ".long %s - . -4", label);
        }
    }
    free(line);
    if (ferror(in)) return fail(c, "cannot read %s: %s", c->in_path, strerror(errno));
    return true;
}

bool rt_aot_asm_convert(const char *in_path, const char *out_path, const char *exports_path, rt_aot_asm_stats *stats,
                        char *err, size_t errn)
{
    conv c;
    FILE *in, *ex = NULL;
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    unsigned long lineno = 0;
    unsigned i;
    bool ok = true;

    memset(&c, 0, sizeof c);
    memset(stats, 0, sizeof *stats);
    c.stats = stats;
    c.err = err;
    c.errn = errn;
    c.in_path = in_path;
    c.out_path = out_path;
    in = fopen(in_path, "r");
    if (in == NULL) return fail(&c, "cannot open %s: %s", in_path, strerror(errno));
    if (!find_target(&c, in))
    {
        fclose(in);
        return false;
    }
    if (fseek(in, 0, SEEK_SET) != 0)
    {
        fclose(in);
        return fail(&c, "cannot read %s again: %s", in_path, strerror(errno));
    }
    c.out = fopen(out_path, "w");
    if (c.out == NULL)
    {
        fclose(in);
        return fail(&c, "cannot make %s: %s", out_path, strerror(errno));
    }
    while (ok && (len = getline(&line, &cap, in)) >= 0)
    {
        byte_line b;
        lineno++;
        if ((size_t)len > MAX_LINE)
        {
            ok = fail(&c, "%s line %lu is too long", in_path, lineno);
            break;
        }
        if (c.target[0] != 0 && parse_byte_line(line, &b))
        {
            b.changed = false;
            b.text = strdup(line);
            if (b.text == NULL)
            {
                ok = fail(&c, "out of memory");
                break;
            }
            if (c.npending == PENDING_LINES && !flush_pending(&c, PENDING_LINES - 1))
            {
                free(b.text);
                ok = false;
                break;
            }
            c.pending[c.npending++] = b;
            continue;
        }
        ok = other_line(&c, line, (size_t)len);
    }
    if (ok && ferror(in)) ok = fail(&c, "cannot read %s: %s", in_path, strerror(errno));
    if (ok) ok = flush_pending(&c, 0);
    for (i = 0; i < c.npending; i++) free(c.pending[i].text);
    free(line);
    fclose(in);
    if (fclose(c.out) != 0 && ok) ok = fail(&c, "cannot write %s: %s", out_path, strerror(errno));

    if (ok)
    {
        qsort(c.exports, c.nexports, sizeof *c.exports, cmp_names);
        ex = fopen(exports_path, "w");
        if (ex == NULL) ok = fail(&c, "cannot make %s: %s", exports_path, strerror(errno));
        for (i = 0; ok && i < c.nexports; i++)
        {
            if (i > 0 && strcmp(c.exports[i], c.exports[i - 1]) == 0) continue;
            if (fprintf(ex, "/export:%s\n", c.exports[i]) < 0) ok = fail(&c, "cannot write %s", exports_path);
            stats->exports++;
        }
        if (ex != NULL && fclose(ex) != 0 && ok) ok = fail(&c, "cannot write %s: %s", exports_path, strerror(errno));
    }
    for (i = 0; i < c.nexports; i++) free(c.exports[i]);
    free(c.exports);
    return ok;
}
