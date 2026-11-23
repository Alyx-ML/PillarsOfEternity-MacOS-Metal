/*
 * VERSION functions: GetFileVersionInfoSizeA/W, GetFileVersionInfoA/W and
 * VerQueryValueA/W.
 *
 * The version information of a file is the VS_VERSIONINFO resource
 * (RT_VERSION, ID 1) of a PE file. The runtime reads it from the file
 * without loading the file. The language of the resource is US English if
 * the file has it, else neutral, else the first language.
 *
 * As on Windows XP, the size from GetFileVersionInfoSizeW is two times the
 * resource size plus 4, and GetFileVersionInfoW writes the "FE2X"
 * signature after the resource when the buffer has space for it.
 * Only 32-bit (Unicode) version resources are supported.
 *
 * The ANSI functions get the same Unicode resource. VerQueryValueA
 * converts a text value to Windows-1252 into the second half of the
 * buffer, as Windows does: at block + wLength + 4 + (offset of the value
 * in the block) / 2. That is why the buffer size is two times the
 * resource size plus 4.
 */
#include "rt_file.h"
#include "rt_text.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ERROR_BAD_FORMAT 11u
#define ERROR_FILENAME_EXCED_RANGE 206u
#define ERROR_RESOURCE_DATA_NOT_FOUND 1812u
#define ERROR_RESOURCE_TYPE_NOT_FOUND 1813u
#define ERROR_RESOURCE_NAME_NOT_FOUND 1814u

#define RT_VERSION 16u
#define VS_VERSION_INFO 1u
#define LANG_EN_US 0x409u

/* The largest version resource that the runtime reads (the wLength field
   of VS_VERSIONINFO is 16 bits). */
#define MAX_VERSION_SIZE 0xFFFFu

typedef struct pe_file {
    int fd;
    uint64_t size;
} pe_file;

/* Read n bytes at off. Returns false if the range is outside the file or
   the read fails. */
static bool pe_read(const pe_file *f, uint64_t off, void *buf, size_t n)
{
    size_t done = 0;
    if (off > f->size || n > f->size - off) return false;
    while (done < n)
    {
        ssize_t r = pread(f->fd, (uint8_t *)buf + done, n - done, (off_t)(off + done));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        done += (size_t)r;
    }
    return true;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

typedef struct section_map {
    uint32_t count;
    uint8_t *table; /* count entries of 40 bytes */
} section_map;

/* File offset of rva for len bytes. Returns false if no section has it. */
static bool rva_to_off(const section_map *m, uint32_t rva, uint32_t len, uint64_t *off)
{
    uint32_t i;
    for (i = 0; i < m->count; i++)
    {
        const uint8_t *s = m->table + 40u * i;
        uint32_t vsize = le32(s + 8), va = le32(s + 12), raw = le32(s + 16), ptr = le32(s + 20);
        uint32_t span = vsize > raw ? raw : vsize;
        if (span == 0) span = raw;
        if (rva >= va && rva - va < span && len <= span - (rva - va))
        {
            *off = (uint64_t)ptr + (rva - va);
            return true;
        }
    }
    return false;
}

/* Find an entry of the resource directory at dir_off (a file offset; the
   resource section starts at base_off). want_id: the ID to find; for the
   language level (want_id < 0) the preferred language. Returns the
   OffsetToData field, or false. */
static bool find_entry(const pe_file *f, uint64_t base_off, uint32_t res_size, uint32_t dir_rel, long want_id,
                       uint32_t *data)
{
    uint8_t hdr[16], e[8];
    uint32_t named, ids, i, first = 0, neutral = 0;
    bool have_first = false, have_neutral = false;
    if (dir_rel > res_size || res_size - dir_rel < 16 || !pe_read(f, base_off + dir_rel, hdr, 16)) return false;
    named = le16(hdr + 12);
    ids = le16(hdr + 14);
    if ((uint64_t)(named + ids) * 8u > res_size - dir_rel - 16u) return false;
    for (i = named; i < named + ids; i++)
    {
        uint32_t id, off;
        if (!pe_read(f, base_off + dir_rel + 16u + 8u * i, e, 8)) return false;
        id = le32(e);
        off = le32(e + 4);
        if (want_id >= 0)
        {
            if (id == (uint32_t)want_id)
            {
                *data = off;
                return true;
            }
            continue;
        }
        if (id == LANG_EN_US)
        {
            *data = off;
            return true;
        }
        if (id == 0 && !have_neutral)
        {
            neutral = off;
            have_neutral = true;
        }
        if (!have_first)
        {
            first = off;
            have_first = true;
        }
    }
    if (want_id >= 0) return false;
    if (have_neutral || have_first)
    {
        *data = have_neutral ? neutral : first;
        return true;
    }
    return false;
}

/* Open the file of the guest path g (wide or ANSI). Returns 0 or a Win32
   error. */
static uint32_t open_guest(uint32_t g, bool wide, pe_file *f)
{
    char in[RT_HOST_PATH], full[RT_HOST_PATH], host[RT_HOST_PATH];
    bool exists;
    struct stat st;
    uint32_t e;
    if (g == 0) return ERROR_INVALID_PARAMETER;
    if ((wide ? rt_gstr_w(in, sizeof(in), g) : rt_gstr_a(in, sizeof(in), g)) != RT_STR_OK) return ERROR_FILENAME_EXCED_RANGE;
    e = rt_path_full(in, full, sizeof(full));
    if (e == 0) e = rt_path_host(full, host, sizeof(host), &exists);
    if (e != 0) return e;
    if (!exists) return ERROR_FILE_NOT_FOUND;
    f->fd = open(host, O_RDONLY | O_CLOEXEC);
    if (f->fd < 0) return rt_errno_to_win32(errno, ERROR_FILE_NOT_FOUND);
    if (fstat(f->fd, &st) != 0) rt_fatal("version: fstat failed: %s", strerror(errno));
    if (!S_ISREG(st.st_mode))
    {
        (void)(close(f->fd) != 0);
        return S_ISDIR(st.st_mode) ? ERROR_ACCESS_DENIED : ERROR_BAD_FORMAT;
    }
    f->size = (uint64_t)st.st_size;
    return 0;
}

/* Find the version resource of the open PE file f: file offset and size.
   Returns 0 or a Win32 error code. */
static uint32_t find_version(const pe_file *f, uint64_t *off, uint32_t *size)
{
    uint8_t mz[64], nt[24], opt[240], dentry[16];
    uint32_t lfanew, nsect, optsize, ndirs, dir_off, res_rva, res_size, ent;
    uint64_t res_off, data_off;
    section_map m = { 0, NULL };
    uint32_t e = 0;

    if (!pe_read(f, 0, mz, 64) || mz[0] != 'M' || mz[1] != 'Z') return ERROR_BAD_FORMAT;
    lfanew = le32(mz + 0x3C);
    if (!pe_read(f, lfanew, nt, 24) || memcmp(nt, "PE\0\0", 4) != 0) return ERROR_BAD_FORMAT;
    nsect = le16(nt + 6);
    optsize = le16(nt + 20);
    if (optsize < 2 || optsize > sizeof(opt) || !pe_read(f, (uint64_t)lfanew + 24u, opt, optsize)) return ERROR_BAD_FORMAT;
    if (le16(opt) == 0x10B)
        dir_off = 96;
    else if (le16(opt) == 0x20B)
        dir_off = 112;
    else
        return ERROR_BAD_FORMAT;
    if (optsize < dir_off) return ERROR_BAD_FORMAT;
    ndirs = le32(opt + dir_off - 4);
    if (ndirs < 3 || optsize < dir_off + 24u) return ERROR_RESOURCE_DATA_NOT_FOUND;
    res_rva = le32(opt + dir_off + 16);
    res_size = le32(opt + dir_off + 20);
    if (res_rva == 0 || res_size < 16) return ERROR_RESOURCE_DATA_NOT_FOUND;

    m.count = nsect;
    m.table = malloc(40u * (nsect == 0 ? 1u : nsect));
    if (m.table == NULL) rt_fatal("out of host memory (version resource)");
    if (!pe_read(f, (uint64_t)lfanew + 24u + optsize, m.table, 40u * nsect)) e = ERROR_BAD_FORMAT;
    else if (!rva_to_off(&m, res_rva, res_size, &res_off)) e = ERROR_BAD_FORMAT;
    /* Three levels: type, name, language. */
    else if (!find_entry(f, res_off, res_size, 0, RT_VERSION, &ent) || !(ent & 0x80000000u))
        e = ERROR_RESOURCE_TYPE_NOT_FOUND;
    else if (!find_entry(f, res_off, res_size, ent & 0x7FFFFFFFu, VS_VERSION_INFO, &ent) || !(ent & 0x80000000u))
        e = ERROR_RESOURCE_NAME_NOT_FOUND;
    else if (!find_entry(f, res_off, res_size, ent & 0x7FFFFFFFu, -1, &ent) || (ent & 0x80000000u))
        e = ERROR_RESOURCE_NAME_NOT_FOUND;
    else if (ent > res_size || res_size - ent < 16 || !pe_read(f, res_off + ent, dentry, 16))
        e = ERROR_BAD_FORMAT;
    else
    {
        uint32_t rva = le32(dentry), len = le32(dentry + 4);
        if (len < 6 || len > MAX_VERSION_SIZE || !rva_to_off(&m, rva, len, &data_off))
            e = ERROR_BAD_FORMAT;
        else
        {
            *off = data_off;
            *size = len;
        }
    }
    free(m.table);
    return e;
}

/* DWORD GetFileVersionInfoSizeA/W(LPCSTR file, LPDWORD handle) */
static void version_info_size(_cpu *cpu, bool wide)
{
    uint32_t hptr = ARG(1), size = 0, e;
    uint64_t off;
    pe_file f;
    if (hptr != 0) wr32(hptr, 0);
    e = open_guest(ARG(0), wide, &f);
    if (e == 0)
    {
        e = find_version(&f, &off, &size);
        (void)(close(f.fd) != 0);
    }
    if (e != 0)
    {
        rt_set_last_error(e);
        RET(0);
        return;
    }
    RET(size * 2u + 4u);
}

void rt_GetFileVersionInfoSizeA(_cpu *cpu) { version_info_size(cpu, false); }
void rt_GetFileVersionInfoSizeW(_cpu *cpu) { version_info_size(cpu, true); }

/* BOOL GetFileVersionInfoA/W(LPCSTR file, DWORD handle, DWORD len, LPVOID
   data) */
static void version_info(_cpu *cpu, bool wide)
{
    uint32_t len = ARG(2), data = ARG(3), size = 0, e, n;
    uint64_t off = 0;
    pe_file f;
    uint8_t *buf = NULL;
    if (data == 0 || len == 0)
    {
        rt_set_last_error(ERROR_INVALID_DATA);
        RET(0);
        return;
    }
    e = open_guest(ARG(0), wide, &f);
    if (e == 0)
    {
        e = find_version(&f, &off, &size);
        if (e == 0)
        {
            n = len < size ? len : size;
            buf = malloc(n);
            if (buf == NULL) rt_fatal("out of host memory (version resource)");
            if (!pe_read(&f, off, buf, n)) e = ERROR_BAD_FORMAT;
        }
        (void)(close(f.fd) != 0);
    }
    if (e != 0)
    {
        free(buf);
        rt_set_last_error(e);
        RET(0);
        return;
    }
    n = len < size ? len : size;
    if (n >= 6 && le16(buf + 4) > 1) rt_fatal("GetFileVersionInfoW: 16-bit version resources are not supported");
    memcpy(G2H(data), buf, n);
    free(buf);
    /* The "FE2X" signature after the resource (wLength bytes). */
    if (n >= 2)
    {
        uint32_t wlen = rd16(data);
        if (len >= wlen + 4u) memcpy(G2H(data + wlen), "FE2X", 4);
    }
    RET(1);
}

void rt_GetFileVersionInfoA(_cpu *cpu) { version_info(cpu, false); }
void rt_GetFileVersionInfoW(_cpu *cpu) { version_info(cpu, true); }

static uint32_t align4(uint32_t x) { return (x + 3u) & ~3u; }

typedef struct vnode {
    uint32_t at, length, value_len, type, key, value, children;
} vnode;

/* Read the node at at, inside [at, end). Returns false for a bad node. */
static bool node_read(uint32_t at, uint32_t end, vnode *n)
{
    uint32_t k, vbytes;
    if (end - at < 6) return false;
    n->at = at;
    n->length = rd16(at);
    n->value_len = rd16(at + 2);
    n->type = rd16(at + 4);
    if (n->length < 6 || n->length > end - at || n->type > 1) return false;
    n->key = at + 6;
    for (k = n->key; k + 2 <= at + n->length && rd16(k) != 0; k += 2) {}
    if (k + 2 > at + n->length) return false;
    n->value = align4(k + 2 - at) + at;
    vbytes = n->type == 1 ? n->value_len * 2u : n->value_len;
    if (n->value > at + n->length || vbytes > at + n->length - n->value)
    {
        /* A node without a value can end at its key. */
        if (vbytes != 0) return false;
        n->value = at + n->length;
    }
    n->children = align4(n->value + vbytes - at) + at;
    if (n->children > at + n->length) n->children = at + n->length;
    return true;
}

static bool key_is(const vnode *n, const char *want)
{
    char key[256];
    uint32_t units = (n->value - n->key) / 2u;
    if (units == 0) return want[0] == 0;
    if (rt_gstrn_w(key, sizeof(key), n->key, units) != RT_STR_OK) return false;
    return strcasecmp(key, want) == 0;
}

/* BOOL VerQueryValueA/W(LPCVOID block, LPCSTR sub_block, LPVOID *buf,
   PUINT len) */
static void query_value(_cpu *cpu, bool wide)
{
    uint32_t block = ARG(0), sub = ARG(1), pbuf = ARG(2), plen = ARG(3);
    char path[512], *p, *save = NULL;
    vnode n;
    if (block == 0 || sub == 0 || pbuf == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if ((wide ? rt_gstr_w(path, sizeof(path), sub) : rt_gstr_a(path, sizeof(path), sub)) != RT_STR_OK)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (!node_read(block, block + rd16(block), &n))
    {
        rt_set_last_error(ERROR_INVALID_DATA);
        RET(0);
        return;
    }
    for (p = strtok_r(path, "\\", &save); p != NULL; p = strtok_r(NULL, "\\", &save))
    {
        uint32_t c = n.children, end = n.at + n.length;
        bool found = false;
        vnode child;
        while (c < end && !found)
        {
            if (!node_read(c, end, &child)) break;
            if (key_is(&child, p))
                found = true;
            else
                c = align4(c + child.length - block) + block;
        }
        if (!found)
        {
            rt_set_last_error(ERROR_RESOURCE_TYPE_NOT_FOUND);
            RET(0);
            return;
        }
        n = child;
    }
    if (!wide && n.type == 1)
    {
        /* A text value: the ANSI form in the second half of the buffer. */
        uint32_t a = block + rd16(block) + 4u + (n.value - block) / 2u, i;
        if (!rt_va_is_committed(a, n.value_len ? n.value_len : 1u))
        {
            rt_set_last_error(ERROR_INVALID_DATA);
            RET(0);
            return;
        }
        for (i = 0; i < n.value_len; i++)
        {
            uint16_t u = rd16(n.value + 2u * i);
            int b = rt_unicode_to_cp1252(u);
            wr8(a + i, b < 0 ? (uint8_t)'?' : (uint8_t)b);
        }
        wr32(pbuf, a);
        if (plen != 0) wr32(plen, n.value_len);
        RET(1);
        return;
    }
    wr32(pbuf, n.value);
    if (plen != 0) wr32(plen, n.value_len);
    RET(1);
}

void rt_VerQueryValueA(_cpu *cpu) { query_value(cpu, false); }
void rt_VerQueryValueW(_cpu *cpu) { query_value(cpu, true); }
