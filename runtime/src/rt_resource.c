/*
 * PE resources of the mapped modules (FindResource rules).
 *
 * The resource directory (data directory 2) has three levels: type, name
 * and language. A level has the entries with string names first (sorted,
 * upper case) and then the entries with integer ids (sorted). A string
 * name of the guest is compared without case; "#123" is the id 123.
 *
 * Language: the neutral language (0), then US English (0x409), then the
 * first language of the resource. The emulated system has the US English
 * user interface language.
 */
#include "rt.h"
#include "rt_module.h"
#include "rt_text.h"
#include "rt_user32.h"

#include <stdlib.h>
#include <string.h>

#define MAX_RES_NAME 256u

typedef struct res_image {
    uint32_t base, size;    /* module base and SizeOfImage */
    uint32_t dir, dir_size; /* resource directory (guest address, size) */
} res_image;

/* The bytes [off, off + n) of the resource section are in the image. */
static bool in_dir(const res_image *im, uint32_t off, uint32_t n)
{
    return off <= im->dir_size && n <= im->dir_size - off;
}

static bool image_of(uint32_t module, res_image *im)
{
    uint32_t pe, opt, magic, ndirs, rva, size;
    im->size = rt_module_image_size(module);
    im->base = module != 0 ? module : rt_exe_module();
    if (im->size < 0x200u) return false;
    if (rd16(im->base) != 0x5A4Du) return false;
    pe = rd32(im->base + 0x3Cu);
    if (pe > im->size - 0x78u || rd32(im->base + pe) != 0x00004550u) return false;
    opt = pe + 24u;
    magic = rd16(im->base + opt);
    if (magic != 0x10Bu) return false; /* PE32 */
    ndirs = rd32(im->base + opt + 92u);
    if (ndirs < 3u || opt + 96u + 24u > im->size) return false;
    rva = rd32(im->base + opt + 96u + 16u);
    size = rd32(im->base + opt + 96u + 20u);
    if (rva == 0 || size < 16u || rva > im->size || size > im->size - rva) return false;
    im->dir = im->base + rva;
    im->dir_size = size;
    return true;
}

/* A resource name from the guest: an integer id (*id) or a string (UTF-16
   upper case in name, *n units). */
typedef struct res_key {
    bool is_id;
    uint32_t id;
    uint16_t name[MAX_RES_NAME];
    uint32_t n;
} res_key;

static uint16_t upper16(uint16_t c)
{
    if (c >= 'a' && c <= 'z') return (uint16_t)(c - 32u);
    if (c >= 0xE0u && c <= 0xFEu && c != 0xF7u) return (uint16_t)(c - 32u);
    return c;
}

static bool make_key(uint32_t g, bool wide, res_key *k)
{
    uint32_t i;
    memset(k, 0, sizeof *k);
    if (g < 0x10000u)
    {
        k->is_id = true;
        k->id = g;
        return true;
    }
    for (i = 0;; i++)
    {
        uint16_t c;
        if (i >= MAX_RES_NAME) return false;
        c = wide ? rd16(g + 2u * i) : rt_cp1252_to_unicode(rd8(g + i));
        if (c == 0) break;
        k->name[i] = upper16(c);
    }
    k->n = i;
    if (k->n > 1 && k->name[0] == '#')
    {
        /* "#123": the integer id 123. */
        uint32_t v = 0;
        for (i = 1; i < k->n; i++)
        {
            if (k->name[i] < '0' || k->name[i] > '9') return true;
            v = v * 10u + (uint32_t)(k->name[i] - '0');
            if (v > 0xFFFFu) return true;
        }
        k->is_id = true;
        k->id = v;
    }
    return true;
}

/* The entry of key in the directory at offset dir. Returns the
   OffsetToData field, or false. */
static bool find_entry(const res_image *im, uint32_t dir, const res_key *k, uint32_t *target)
{
    uint32_t named, ids, i, first, n;
    if (!in_dir(im, dir, 16u)) return false;
    named = rd16(im->dir + dir + 12u);
    ids = rd16(im->dir + dir + 14u);
    if (!in_dir(im, dir + 16u, 8u * (named + ids))) return false;
    first = k->is_id ? named : 0;
    n = k->is_id ? ids : named;
    for (i = first; i < first + n; i++)
    {
        uint32_t e = im->dir + dir + 16u + 8u * i, name = rd32(e);
        if (k->is_id)
        {
            if ((name & 0x80000000u) == 0 && name == k->id)
            {
                *target = rd32(e + 4u);
                return true;
            }
        }
        else if (name & 0x80000000u)
        {
            uint32_t so = name & 0x7FFFFFFFu, len, j;
            if (!in_dir(im, so, 2u)) return false;
            len = rd16(im->dir + so);
            if (!in_dir(im, so + 2u, 2u * len)) return false;
            if (len != k->n) continue;
            for (j = 0; j < len; j++)
                if (upper16(rd16(im->dir + so + 2u + 2u * j)) != k->name[j]) break;
            if (j == len)
            {
                *target = rd32(e + 4u);
                return true;
            }
        }
    }
    return false;
}

bool rt_resource_find(uint32_t module, uint32_t type, uint32_t name, bool wide, uint32_t *data, uint32_t *size)
{
    static const uint32_t langs[2] = { 0u, 0x409u };
    res_image im;
    res_key k;
    uint32_t t, nm, lang = 0, entry, rva, sz;
    int i;
    bool found = false;
    if (!image_of(module, &im)) return false;
    if (!make_key(type, wide, &k) || !find_entry(&im, 0, &k, &t) || !(t & 0x80000000u)) return false;
    if (!make_key(name, wide, &k) || !find_entry(&im, t & 0x7FFFFFFFu, &k, &nm) || !(nm & 0x80000000u)) return false;
    nm &= 0x7FFFFFFFu;
    for (i = 0; i < 2 && !found; i++)
    {
        res_key lk;
        memset(&lk, 0, sizeof lk);
        lk.is_id = true;
        lk.id = langs[i];
        found = find_entry(&im, nm, &lk, &lang);
    }
    if (!found)
    {
        /* The first language. */
        if (!in_dir(&im, nm, 16u) || rd16(im.dir + nm + 12u) + rd16(im.dir + nm + 14u) == 0 || !in_dir(&im, nm + 16u, 8u))
            return false;
        lang = rd32(im.dir + nm + 16u + 4u);
    }
    if (lang & 0x80000000u) return false; /* a fourth level is not valid */
    entry = lang;
    if (!in_dir(&im, entry, 16u)) return false;
    rva = rd32(im.dir + entry);
    sz = rd32(im.dir + entry + 4u);
    if (rva > im.size || sz > im.size - rva) return false;
    *data = im.base + rva;
    *size = sz;
    return true;
}
