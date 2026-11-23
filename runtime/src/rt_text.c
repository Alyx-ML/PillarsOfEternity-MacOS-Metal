/* Text conversion between guest strings and the runtime (see rt_text.h). */
#include "rt_text.h"

/* Windows-1252 0x80-0x9F (the unassigned bytes map to the same C1 control
   code point as in Windows: 0x81, 0x8D, 0x8F, 0x90, 0x9D). */
static const uint16_t cp1252_80[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

uint16_t rt_cp1252_to_unicode(uint8_t c)
{
    return (c >= 0x80 && c <= 0x9F) ? cp1252_80[c - 0x80] : c;
}

int rt_unicode_to_cp1252(uint32_t cp)
{
    unsigned i;
    if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) return (int)cp;
    for (i = 0; i < 32; i++)
        if (cp1252_80[i] == cp) return (int)(0x80 + i);
    return -1;
}

/* OEM code page 437 0x80-0xFF (0x00-0x7F map to the same code points, as
   in MultiByteToWideChar without MB_USEGLYPHCHARS). */
static const uint16_t cp437_80[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

uint16_t rt_cp437_to_unicode(uint8_t c)
{
    return c >= 0x80 ? cp437_80[c - 0x80] : c;
}

int rt_unicode_to_cp437(uint32_t cp)
{
    unsigned i;
    if (cp < 0x80) return (int)cp;
    for (i = 0; i < 128; i++)
        if (cp437_80[i] == cp) return (int)(0x80 + i);
    return -1;
}

int rt_utf8_decode(const uint8_t *s, size_t n, uint32_t *cp)
{
    uint8_t c = s[0];
    int len, i;
    uint32_t v, min;
    if (c < 0x80)
    {
        *cp = c;
        return 1;
    }
    if (c >= 0xC2 && c <= 0xDF) { len = 2; v = c & 0x1Fu; min = 0x80; }
    else if (c >= 0xE0 && c <= 0xEF) { len = 3; v = c & 0x0Fu; min = 0x800; }
    else if (c >= 0xF0 && c <= 0xF4) { len = 4; v = c & 0x07u; min = 0x10000; }
    else return -1;
    for (i = 1; i < len; i++)
    {
        if ((size_t)i >= n || (s[i] & 0xC0) != 0x80) return -i;
        v = (v << 6) | (s[i] & 0x3Fu);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return -len;
    *cp = v;
    return len;
}

size_t rt_utf8_encode(uint32_t cp, char out[4])
{
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) rt_fatal("rt_utf8_encode: bad code point 0x%x", cp);
    if (cp < 0x80)
    {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800)
    {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000)
    {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Append code point cp to dst at *pos. Returns false if it does not fit
   with a NUL after it. */
static bool put_cp(char *dst, size_t max, size_t *pos, uint32_t cp)
{
    char tmp[4];
    size_t n = rt_utf8_encode(cp, tmp);
    if (*pos + n + 1 > max) return false;
    memcpy(dst + *pos, tmp, n);
    *pos += n;
    return true;
}

int rt_gstr_a(char *dst, size_t max, uint32_t g)
{
    const uint8_t *s = G2H(g);
    size_t pos = 0, i;
    if (max == 0) return RT_STR_TOO_LONG;
    dst[0] = 0;
    if (s == NULL) return RT_STR_TOO_LONG;
    for (i = 0; s[i] != 0; i++)
    {
        if (!put_cp(dst, max, &pos, rt_cp1252_to_unicode(s[i])))
        {
            dst[pos] = 0;
            return RT_STR_TOO_LONG;
        }
    }
    dst[pos] = 0;
    return RT_STR_OK;
}

/* Convert UTF-16 units at g; count UINT32_MAX means "until the NUL". */
static int wide_to_utf8(char *dst, size_t max, uint32_t g, uint32_t count)
{
    size_t pos = 0;
    uint32_t i;
    if (max == 0) return RT_STR_TOO_LONG;
    dst[0] = 0;
    if (g == 0) return RT_STR_TOO_LONG;
    for (i = 0; count == UINT32_MAX || i < count; i++)
    {
        uint32_t cp = rd16(g + 2u * i);
        if (count == UINT32_MAX && cp == 0) break;
        if (cp >= 0xD800 && cp <= 0xDBFF)
        {
            uint32_t lo = (count == UINT32_MAX || i + 1 < count) ? rd16(g + 2u * (i + 1)) : 0;
            if (lo < 0xDC00 || lo > 0xDFFF)
            {
                dst[pos] = 0;
                return RT_STR_INVALID;
            }
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i++;
        }
        else if (cp >= 0xDC00 && cp <= 0xDFFF)
        {
            dst[pos] = 0;
            return RT_STR_INVALID;
        }
        if (!put_cp(dst, max, &pos, cp))
        {
            dst[pos] = 0;
            return RT_STR_TOO_LONG;
        }
    }
    dst[pos] = 0;
    return RT_STR_OK;
}

int rt_gstr_w(char *dst, size_t max, uint32_t g)
{
    return wide_to_utf8(dst, max, g, UINT32_MAX);
}

int rt_gstrn_w(char *dst, size_t max, uint32_t g, uint32_t count)
{
    if (count == UINT32_MAX) return RT_STR_TOO_LONG;
    if (count == 0)
    {
        if (max == 0) return RT_STR_TOO_LONG;
        dst[0] = 0;
        return RT_STR_OK;
    }
    return wide_to_utf8(dst, max, g, count);
}

uint32_t rt_wstrlen(uint32_t g)
{
    uint32_t n = 0;
    while (rd16(g + 2u * n) != 0)
    {
        n++;
        if (n == 0x7FFFFFFFu) rt_fatal("wide guest string at 0x%08x has no terminator", g);
    }
    return n;
}

bool rt_utf16_length(const char *s, size_t *units)
{
    const uint8_t *p = (const uint8_t *)s;
    size_t n = strlen(s), i = 0, u = 0;
    while (i < n)
    {
        uint32_t cp;
        int l = rt_utf8_decode(p + i, n - i, &cp);
        if (l < 0) return false;
        u += cp >= 0x10000 ? 2u : 1u;
        i += (size_t)l;
    }
    *units = u;
    return true;
}

/* Write s as UTF-16 at buf (room for its units and a NUL is checked by the
   caller). */
static void write_wide(uint32_t buf, const char *s)
{
    const uint8_t *p = (const uint8_t *)s;
    size_t n = strlen(s), i = 0;
    uint32_t o = buf;
    while (i < n)
    {
        uint32_t cp;
        int l = rt_utf8_decode(p + i, n - i, &cp);
        if (l < 0) rt_fatal("runtime string is not valid UTF-8");
        if (cp >= 0x10000)
        {
            wr16(o, (uint16_t)(0xD800 + ((cp - 0x10000) >> 10)));
            wr16(o + 2, (uint16_t)(0xDC00 + ((cp - 0x10000) & 0x3FF)));
            o += 4;
        }
        else
        {
            wr16(o, (uint16_t)cp);
            o += 2;
        }
        i += (size_t)l;
    }
    wr16(o, 0);
}

uint32_t rt_put_wstr(uint32_t buf, uint32_t n, const char *s)
{
    size_t units;
    if (!rt_utf16_length(s, &units)) rt_fatal("runtime string is not valid UTF-8");
    if (units >= 0x7FFFFFFFu) rt_fatal("runtime string is too long");
    if (units + 1 > n || buf == 0) return (uint32_t)units + 1u;
    write_wide(buf, s);
    return (uint32_t)units;
}

bool rt_put_wfield(uint32_t buf, uint32_t n, const char *s)
{
    size_t units;
    if (n == 0) rt_fatal("rt_put_wfield with a field of 0 units");
    if (!rt_utf16_length(s, &units)) rt_fatal("runtime string is not valid UTF-8");
    if (units + 1 > n)
    {
        wr16(buf, 0);
        return false;
    }
    write_wide(buf, s);
    return true;
}

/* Number of ANSI bytes of s; each code point is one byte. */
static size_t ansi_length(const char *s)
{
    const uint8_t *p = (const uint8_t *)s;
    size_t n = strlen(s), i = 0, c = 0;
    while (i < n)
    {
        uint32_t cp;
        int l = rt_utf8_decode(p + i, n - i, &cp);
        if (l < 0) rt_fatal("runtime string is not valid UTF-8");
        c++;
        i += (size_t)l;
    }
    return c;
}

static void write_ansi(uint32_t buf, const char *s)
{
    const uint8_t *p = (const uint8_t *)s;
    size_t n = strlen(s), i = 0;
    uint32_t o = buf;
    while (i < n)
    {
        uint32_t cp;
        int l = rt_utf8_decode(p + i, n - i, &cp);
        int b;
        if (l < 0) rt_fatal("runtime string is not valid UTF-8");
        b = rt_unicode_to_cp1252(cp);
        wr8(o++, (uint8_t)(b < 0 ? '?' : b));
        i += (size_t)l;
    }
    wr8(o, 0);
}

uint32_t rt_put_astr(uint32_t buf, uint32_t n, const char *s)
{
    size_t len = ansi_length(s);
    if (len >= 0x7FFFFFFFu) rt_fatal("runtime string is too long");
    if (len + 1 > n || buf == 0) return (uint32_t)len + 1u;
    write_ansi(buf, s);
    return (uint32_t)len;
}

bool rt_put_afield(uint32_t buf, uint32_t n, const char *s)
{
    if (n == 0) rt_fatal("rt_put_afield with a field of 0 bytes");
    if (ansi_length(s) + 1 > n)
    {
        wr8(buf, 0);
        return false;
    }
    write_ansi(buf, s);
    return true;
}
