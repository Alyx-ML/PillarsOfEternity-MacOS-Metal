/*
 * SHELL32 CommandLineToArgvW and SHLWAPI path functions: PathCanonicalizeW,
 * PathFileExistsW and PathIsDirectoryW.
 *
 * CommandLineToArgvW and PathCanonicalizeW follow the rules of the Windows
 * functions as the Wine project documents them (dlls/shell32/shell32_main.c
 * and dlls/shlwapi/path.c, which are tested against Windows). The
 * runtime code is a new implementation of these rules.
 */
#include "rt.h"
#include "rt_file.h"
#include "rt_module.h"
#include "rt_text.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_PATH 260u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define ERROR_FILENAME_EXCED_RANGE 206u
#define MAX_CMDLINE 32768u /* characters, the Windows command line limit */

/* ---------------------------------------------------------------- CommandLineToArgvW */

static bool is_blank(uint16_t c) { return c == ' ' || c == '\t'; }

/* LPWSTR *CommandLineToArgvW(LPCWSTR cmdline, int *argc). The result is
   one LocalAlloc block: the argument pointers, a NULL pointer and the
   argument strings. An empty command line gives the program path. */
void rt_CommandLineToArgvW(_cpu *cpu)
{
    uint32_t cmd = ARG(0), pargc = ARG(1), len, i, argc, qcount, bcount, blk, ptrs, d, total;
    uint16_t *src;
    const uint16_t *s;
    uint16_t *out, *o;
    uint32_t *starts;

    if (pargc == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (cmd == 0) rt_fatal("CommandLineToArgvW with a NULL command line");
    len = rt_wstrlen(cmd);
    if (len >= MAX_CMDLINE) rt_fatal("CommandLineToArgvW: the command line has more than %u characters", MAX_CMDLINE - 1u);
    if (len == 0)
    {
        /* The program path is the only argument. */
        const char *exe = rt_exe_guest_path();
        size_t units;
        if (!rt_utf16_length(exe, &units) || units >= MAX_PATH) rt_fatal("CommandLineToArgvW: bad program path");
        blk = rt_heap_alloc(8u + 2u * ((uint32_t)units + 1u));
        if (blk == 0)
        {
            rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
            RET(0);
            return;
        }
        rt_put_wstr(blk + 8u, (uint32_t)units + 1u, exe);
        wr32(blk, blk + 8u);
        wr32(blk + 4u, 0);
        wr32(pargc, 1);
        RET(blk);
        return;
    }
    src = malloc(2u * ((size_t)len + 1u));
    out = malloc(2u * ((size_t)len + 1u));
    starts = malloc(sizeof(*starts) * ((size_t)len + 2u));
    if (src == NULL || out == NULL || starts == NULL) rt_fatal("out of host memory");
    for (i = 0; i < len; i++) src[i] = rd16(cmd + 2u * i);
    src[len] = 0;

    /* The program name: up to the next quote if it starts with a quote,
       else up to the first blank. No escape characters. */
    s = src;
    o = out;
    argc = 0;
    starts[argc++] = 0;
    if (*s == '"')
    {
        s++;
        while (*s)
        {
            if (*s == '"')
            {
                s++;
                break;
            }
            *o++ = *s++;
        }
    }
    else
    {
        while (*s && !is_blank(*s)) *o++ = *s++;
    }
    *o++ = 0;
    while (is_blank(*s)) s++;
    if (*s)
    {
        starts[argc++] = (uint32_t)(o - out);
        qcount = bcount = 0;
        while (*s)
        {
            if (is_blank(*s) && qcount == 0)
            {
                *o++ = 0;
                bcount = 0;
                do s++;
                while (is_blank(*s));
                if (*s) starts[argc++] = (uint32_t)(o - out);
            }
            else if (*s == '\\')
            {
                *o++ = *s++;
                bcount++;
            }
            else if (*s == '"')
            {
                if ((bcount & 1u) == 0)
                {
                    /* An even number of backslashes: half of them, and the
                       quote starts or ends a quoted part. */
                    o -= bcount / 2u;
                    qcount++;
                }
                else
                {
                    /* An odd number: half of them and a literal quote. */
                    o -= bcount / 2u + 1u;
                    *o++ = '"';
                }
                s++;
                bcount = 0;
                /* Each third consecutive quote is a literal quote. */
                while (*s == '"')
                {
                    if (++qcount == 3)
                    {
                        *o++ = '"';
                        qcount = 0;
                    }
                    s++;
                }
                if (qcount == 2) qcount = 0;
            }
            else
            {
                *o++ = *s++;
                bcount = 0;
            }
        }
        *o++ = 0;
    }

    ptrs = 4u * (argc + 1u);
    total = (uint32_t)(o - out);
    blk = rt_heap_alloc(ptrs + 2u * total);
    if (blk == 0)
    {
        free(src);
        free(out);
        free(starts);
        rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        RET(0);
        return;
    }
    d = blk + ptrs;
    for (i = 0; i < total; i++) wr16(d + 2u * i, out[i]);
    for (i = 0; i < argc; i++) wr32(blk + 4u * i, d + 2u * starts[i]);
    wr32(blk + 4u * argc, 0);
    wr32(pargc, argc);
    free(src);
    free(out);
    free(starts);
    RET(blk);
}

/* ---------------------------------------------------------------- PathCanonicalizeW */

/* PathIsUNCServerShareW: "\\server\share" with no other backslash. */
static bool is_unc_server_share(const uint16_t *p)
{
    bool seen = false;
    if (p[0] != '\\' || p[1] != '\\') return false;
    for (p += 2; *p; p++)
    {
        if (*p == '\\')
        {
            if (seen) return false;
            seen = true;
        }
    }
    return seen;
}

/* BOOL PathCanonicalizeW(LPWSTR dst, LPCWSTR src): removes "." and ".."
   parts. dst has MAX_PATH characters. A source of MAX_PATH or more
   characters fails with ERROR_FILENAME_EXCED_RANGE (UNCONFIRMED for the
   shlwapi version of Windows XP: the rule is from later versions). */
void rt_PathCanonicalizeW(_cpu *cpu)
{
    uint32_t gdst = ARG(0), gsrc = ARG(1), len, i;
    uint16_t path[MAX_PATH], buf[MAX_PATH + 2];
    const uint16_t *s;
    uint16_t *d;

    if (gdst == 0 || gsrc == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    wr16(gdst, 0);
    len = rt_wstrlen(gsrc);
    if (len >= MAX_PATH)
    {
        rt_set_last_error(ERROR_FILENAME_EXCED_RANGE);
        RET(0);
        return;
    }
    for (i = 0; i < len; i++) path[i] = rd16(gsrc + 2u * i);
    path[len] = 0;
    d = buf;
    s = path;
    if (*s == 0)
    {
        wr16(gdst, '\\');
        wr16(gdst + 2, 0);
        RET(1);
        return;
    }
    /* The root. */
    if (*s == '\\')
        *d++ = *s++;
    else if (s[0] != 0 && s[1] == ':')
    {
        *d++ = *s++;
        *d++ = *s++;
        if (*s == '\\') *d++ = *s++;
    }
    while (*s)
    {
        if (*s == '.')
        {
            if (s[1] == '\\' && (s == path || s[-1] == '\\' || s[-1] == ':'))
                s += 2; /* ".\" */
            else if (s[1] == '.' && (d == buf || d[-1] == '\\'))
            {
                /* "\.." goes up one directory, over the root if no "\"
                   follows "X:". It is ignored if it would remove a UNC
                   server name or the first "\\". */
                if (d != buf)
                {
                    *d = 0;
                    if (d > buf + 1 && d[-1] == '\\' && (d[-2] != '\\' || d > buf + 2))
                    {
                        if (d[-2] == ':' && (d > buf + 3 || d[-3] == ':'))
                        {
                            d -= 2;
                            while (d > buf && *d != '\\') d--;
                            if (*d == '\\')
                                d++;
                            else
                                d = buf;
                        }
                        else if (d[-2] != ':' && !is_unc_server_share(buf))
                            d -= 2;
                    }
                    while (d > buf && *d != '\\') d--;
                    if (d == buf)
                    {
                        *d++ = '\\';
                        s++;
                    }
                }
                s += 2;
                /* After a "\" that was put back for the root, s skipped
                   one more character. If the source ended with "..", s is
                   now one past its NUL (still inside path). */
                if (s > path + len) break;
            }
            else
                *d++ = *s++;
        }
        else
            *d++ = *s++;
    }
    /* A drive alone gets a backslash. */
    if (d - buf == 2 && d[-1] == ':') *d++ = '\\';
    *d = 0;
    for (i = 0; buf[i] != 0; i++) wr16(gdst + 2u * i, buf[i]);
    wr16(gdst + 2u * i, 0);
    RET(1);
}

/* ---------------------------------------------------------------- PathFileExistsW, PathIsDirectoryW */

/* stat of the guest path g (wide). Returns false if the path is not valid
   or the file does not exist. */
static bool guest_stat(uint32_t g, struct stat *st)
{
    char in[RT_PATH_BYTES], full[RT_PATH_BYTES], host[RT_HOST_PATH];
    bool exists = false;
    if (g == 0 || rt_gstr_w(in, sizeof(in), g) != RT_STR_OK || in[0] == 0) return false;
    if (rt_path_full(in, full, sizeof(full)) != 0) return false;
    if (rt_path_host(full, host, sizeof(host), &exists) != 0 || !exists) return false;
    return stat(host, st) == 0;
}

/* BOOL PathFileExistsW(LPCWSTR) */
void rt_PathFileExistsW(_cpu *cpu)
{
    struct stat st;
    RET(guest_stat(ARG(0), &st) ? 1u : 0u);
}

/* BOOL PathIsDirectoryW(LPCWSTR): FILE_ATTRIBUTE_DIRECTORY for a
   directory, else FALSE. */
void rt_PathIsDirectoryW(_cpu *cpu)
{
    struct stat st;
    RET(guest_stat(ARG(0), &st) && S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : 0u);
}
