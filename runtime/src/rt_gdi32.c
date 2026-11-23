/*
 * GDI32: bitmaps (DIB sections and device-dependent bitmaps), device
 * contexts, device capabilities and the pixel format functions. Also the
 * DIB and PNG decoders for icons, cursors and bitmap files.
 *
 * The program draws with Direct3D, not with GDI: the runtime has no GDI
 * drawing. The DCs only answer GetDeviceCaps and the pixel format calls.
 *
 * The emulated system has no OpenGL: ChoosePixelFormat finds no pixel
 * format, and SetPixelFormat and SwapBuffers fail. This is the result of
 * Windows on a system without an OpenGL driver.
 *
 * Handles: (type << 24) | (generation << 16) | (index + 1), with the GDI
 * type numbers of Windows (1 DC, 5 bitmap, 10 font).
 */
#include "rt.h"
#include "rt_gdi32.h"
#include "rt_user32.h"
#include "rt_window.h"

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define OBJ_DC 1u
#define OBJ_BITMAP 5u
#define OBJ_FONT 10u

#define MAX_GDI 4096
#define MAX_BITMAP_BYTES 0x10000000u /* 256 MiB */
#define MAX_DIM 32768u

#define BI_RGB 0u
#define BI_RLE8 1u
#define BI_RLE4 2u
#define BI_BITFIELDS 3u

typedef struct gdi_obj {
    uint32_t handle; /* 0: free */
    uint32_t type;
    /* OBJ_BITMAP */
    int32_t w, h;          /* h < 0: top-down DIB */
    uint32_t bpp, stride;
    bool dib;              /* DIB section: bits in guest memory */
    uint32_t gbits;        /* DIB section bits (guest) */
    uint8_t *hbits;        /* device-dependent bitmap bits (host) */
    uint8_t header[124];   /* the BITMAPINFOHEADER (V4, V5) of a DIB section */
    uint32_t header_size;
    uint32_t masks[4];     /* R, G, B, A masks (16 and 32 bits) */
    uint32_t palette[256]; /* 0x00RRGGBB */
    uint32_t ncolors;
    /* OBJ_DC */
    uint32_t hwnd;
} gdi_obj;

static pthread_mutex_t gdi_lock = PTHREAD_MUTEX_INITIALIZER;
static gdi_obj objs[MAX_GDI];
static uint8_t gens[MAX_GDI];
static uint32_t dialog_font;

static void lock(void)
{
    if (pthread_mutex_lock(&gdi_lock) != 0) rt_fatal("GDI32 lock failed");
}

static void unlock(void)
{
    if (pthread_mutex_unlock(&gdi_lock) != 0) rt_fatal("GDI32 unlock failed");
}

/* A new object (lock held). Returns NULL if the table is full. */
static gdi_obj *obj_new(uint32_t type)
{
    int i;
    for (i = 0; i < MAX_GDI; i++)
    {
        if (objs[i].handle != 0) continue;
        memset(&objs[i], 0, sizeof objs[i]);
        gens[i] = (uint8_t)(gens[i] + 1u);
        if (gens[i] == 0) gens[i] = 1;
        objs[i].handle = (type << 24) | ((uint32_t)gens[i] << 16) | (uint32_t)(i + 1);
        objs[i].type = type;
        return &objs[i];
    }
    return NULL;
}

/* The object of h (lock held), or NULL. type 0: any type. */
static gdi_obj *obj_get(uint32_t h, uint32_t type)
{
    uint32_t i = (h & 0xFFFFu);
    if (i == 0 || i > MAX_GDI) return NULL;
    i--;
    if (objs[i].handle != h || (type != 0 && objs[i].type != type)) return NULL;
    return &objs[i];
}

static bool mul_ok(uint32_t a, uint32_t b, uint32_t *r)
{
    uint64_t v = (uint64_t)a * b;
    if (v > 0xFFFFFFFFu) return false;
    *r = (uint32_t)v;
    return true;
}

/* The DWORD-aligned row size of a DIB. */
static bool dib_stride(uint32_t w, uint32_t bpp, uint32_t *stride)
{
    uint64_t bits = (uint64_t)w * bpp;
    if (bits > 0xFFFFFFFFu - 31u) return false;
    *stride = (uint32_t)(((bits + 31u) / 32u) * 4u);
    return true;
}

static int mask_shift(uint32_t m)
{
    int s = 0;
    if (m == 0) return 0;
    while (!(m & 1u))
    {
        m >>= 1;
        s++;
    }
    return s;
}

static uint8_t mask_value(uint32_t v, uint32_t m)
{
    uint32_t top;
    int s;
    if (m == 0) return 0;
    s = mask_shift(m);
    top = m >> s;
    v = (v & m) >> s;
    return (uint8_t)((v * 255u + top / 2u) / top);
}

/* ---------------------------------------------------------------- DIB decoding */

typedef struct dib_info {
    uint32_t w, h, bpp, compression, stride, ncolors;
    bool top_down;
    uint32_t masks[4];
    uint32_t palette[256];
    uint32_t header_size, table_size; /* bytes of the header and the color table */
} dib_info;

/* Parse a BITMAPCOREHEADER or BITMAPINFOHEADER (and V4, V5) with its
   color table at p[0..n). */
static bool dib_parse(const uint8_t *p, size_t n, dib_info *d)
{
    uint32_t hs, i, entry;
    int32_t w, h;
    memset(d, 0, sizeof *d);
    if (n < 4) return false;
    memcpy(&hs, p, 4);
    if (hs == 12)
    {
        uint16_t w16, h16, planes, bpp;
        if (n < 12) return false;
        memcpy(&w16, p + 4, 2);
        memcpy(&h16, p + 6, 2);
        memcpy(&planes, p + 8, 2);
        memcpy(&bpp, p + 10, 2);
        w = w16;
        h = (int16_t)h16;
        d->bpp = bpp;
        d->compression = BI_RGB;
        entry = 3;
        if (planes != 1) return false;
    }
    else if (hs == 40 || hs == 52 || hs == 56 || hs == 108 || hs == 124)
    {
        uint16_t planes, bpp;
        if (n < hs) return false;
        memcpy(&w, p + 4, 4);
        memcpy(&h, p + 8, 4);
        memcpy(&planes, p + 12, 2);
        memcpy(&bpp, p + 14, 2);
        memcpy(&d->compression, p + 16, 4);
        memcpy(&d->ncolors, p + 32, 4);
        d->bpp = bpp;
        entry = 4;
        if (planes != 1) return false;
        if (d->compression == BI_BITFIELDS && hs >= 52)
            memcpy(d->masks, p + 40, hs >= 56 ? 16 : 12);
    }
    else
        return false;
    if (w <= 0 || h == 0 || h == INT32_MIN || (uint32_t)w > MAX_DIM || (uint32_t)(h < 0 ? -h : h) > MAX_DIM) return false;
    d->w = (uint32_t)w;
    d->top_down = h < 0;
    d->h = (uint32_t)(h < 0 ? -h : h);
    if (d->bpp != 1 && d->bpp != 4 && d->bpp != 8 && d->bpp != 16 && d->bpp != 24 && d->bpp != 32) return false;
    if (d->compression == BI_RLE4 || d->compression == BI_RLE8)
        rt_fatal("GDI32: run-length encoded bitmaps (compression %u) are not supported", d->compression);
    if (d->compression != BI_RGB && d->compression != BI_BITFIELDS) return false;
    if (d->compression == BI_BITFIELDS && d->bpp != 16 && d->bpp != 32) return false;
    d->header_size = hs;
    if (d->compression == BI_BITFIELDS && hs == 40)
    {
        /* The three masks follow a BITMAPINFOHEADER. */
        if (n < 52) return false;
        memcpy(d->masks, p + 40, 12);
        d->header_size = 52;
    }
    if (d->compression == BI_RGB)
    {
        if (d->bpp == 16)
        {
            d->masks[0] = 0x7C00u;
            d->masks[1] = 0x03E0u;
            d->masks[2] = 0x001Fu;
        }
        else if (d->bpp == 32)
        {
            d->masks[0] = 0x00FF0000u;
            d->masks[1] = 0x0000FF00u;
            d->masks[2] = 0x000000FFu;
            d->masks[3] = 0xFF000000u;
        }
    }
    if (d->bpp <= 8)
    {
        uint32_t max = 1u << d->bpp;
        if (d->ncolors == 0 || d->ncolors > max) d->ncolors = max;
        d->table_size = d->ncolors * entry;
        if (d->header_size + d->table_size > n) return false;
        for (i = 0; i < d->ncolors; i++)
        {
            const uint8_t *c = p + d->header_size + i * entry;
            d->palette[i] = ((uint32_t)c[2] << 16) | ((uint32_t)c[1] << 8) | c[0];
        }
    }
    else
        d->ncolors = 0;
    return dib_stride(d->w, d->bpp, &d->stride);
}

/* One pixel of row data (bottom-up or top-down as stored) as RGBA. */
static void dib_pixel(const dib_info *d, const uint8_t *row, uint32_t x, uint8_t out[4], bool *alpha_seen)
{
    uint32_t v;
    switch (d->bpp)
    {
    case 1: case 4: case 8:
    {
        uint32_t bit = x * d->bpp, idx = (row[bit / 8u] >> (8u - d->bpp - bit % 8u)) & ((1u << d->bpp) - 1u);
        uint32_t c = idx < d->ncolors ? d->palette[idx] : 0;
        out[0] = (uint8_t)(c >> 16);
        out[1] = (uint8_t)(c >> 8);
        out[2] = (uint8_t)c;
        out[3] = 255;
        return;
    }
    case 16:
        v = (uint32_t)row[2u * x] | ((uint32_t)row[2u * x + 1u] << 8);
        break;
    case 24:
        out[0] = row[3u * x + 2u];
        out[1] = row[3u * x + 1u];
        out[2] = row[3u * x];
        out[3] = 255;
        return;
    default:
        memcpy(&v, row + 4u * x, 4);
        break;
    }
    out[0] = mask_value(v, d->masks[0]);
    out[1] = mask_value(v, d->masks[1]);
    out[2] = mask_value(v, d->masks[2]);
    if (d->bpp == 32 && d->masks[3] != 0)
    {
        out[3] = mask_value(v, d->masks[3]);
        if (out[3] != 0) *alpha_seen = true;
    }
    else
        out[3] = 255;
}

/* Convert DIB pixel rows to RGBA (top row first). bits has rows of
   d->stride bytes. */
static uint8_t *dib_rows_to_rgba(const dib_info *d, const uint8_t *bits, size_t nbits, uint32_t rows, bool *alpha_seen)
{
    uint32_t size, x, y;
    uint8_t *rgba;
    if (!mul_ok(d->stride, rows, &size) || size > nbits) return NULL;
    if (!mul_ok(d->w * 4u, rows, &size) || size > MAX_BITMAP_BYTES) return NULL;
    rgba = malloc(size != 0 ? size : 1u);
    if (rgba == NULL) rt_fatal("out of host memory (bitmap)");
    *alpha_seen = false;
    for (y = 0; y < rows; y++)
    {
        const uint8_t *row = bits + (size_t)(d->top_down ? y : rows - 1u - y) * d->stride;
        for (x = 0; x < d->w; x++) dib_pixel(d, row, x, rgba + 4u * ((size_t)y * d->w + x), alpha_seen);
    }
    return rgba;
}

/* PNG (and the other ImageIO formats) to straight-alpha RGBA. */
static bool png_decode(const uint8_t *p, size_t n, uint8_t **rgba, uint32_t *w, uint32_t *h)
{
    CFDataRef data = CFDataCreate(NULL, p, (CFIndex)n);
    CGImageSourceRef src;
    CGImageRef img;
    CGColorSpaceRef cs;
    CGContextRef ctx;
    size_t iw, ih, i, bytes;
    uint8_t *buf;
    if (data == NULL) rt_fatal("out of host memory (PNG)");
    src = CGImageSourceCreateWithData(data, NULL);
    CFRelease(data);
    if (src == NULL) return false;
    img = CGImageSourceCreateImageAtIndex(src, 0, NULL);
    CFRelease(src);
    if (img == NULL) return false;
    iw = CGImageGetWidth(img);
    ih = CGImageGetHeight(img);
    if (iw == 0 || ih == 0 || iw > MAX_DIM || ih > MAX_DIM || iw * ih * 4u > MAX_BITMAP_BYTES)
    {
        CGImageRelease(img);
        return false;
    }
    bytes = iw * ih * 4u;
    buf = calloc(1, bytes);
    if (buf == NULL) rt_fatal("out of host memory (PNG)");
    cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    if (cs == NULL) rt_fatal("GDI32: the sRGB color space is not available");
    ctx = CGBitmapContextCreate(buf, iw, ih, 8, iw * 4u, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (ctx == NULL) rt_fatal("GDI32: a bitmap context could not be made");
    CGContextDrawImage(ctx, CGRectMake(0, 0, (CGFloat)iw, (CGFloat)ih), img);
    CGContextRelease(ctx);
    CGImageRelease(img);
    /* Straight alpha. */
    for (i = 0; i < iw * ih; i++)
    {
        uint8_t *px = buf + 4u * i;
        if (px[3] != 0 && px[3] != 255)
        {
            px[0] = (uint8_t)((px[0] * 255u + px[3] / 2u) / px[3]);
            px[1] = (uint8_t)((px[1] * 255u + px[3] / 2u) / px[3]);
            px[2] = (uint8_t)((px[2] * 255u + px[3] / 2u) / px[3]);
        }
    }
    *rgba = buf;
    *w = (uint32_t)iw;
    *h = (uint32_t)ih;
    return true;
}

bool rt_image_decode(const uint8_t *p, size_t n, bool icon, uint8_t **rgba, uint32_t *w, uint32_t *h, bool *has_alpha)
{
    dib_info d;
    bool alpha_seen;
    uint32_t rows;
    size_t off;
    if (n >= 8 && memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0)
    {
        *has_alpha = true;
        return png_decode(p, n, rgba, w, h);
    }
    if (!dib_parse(p, n, &d)) return false;
    rows = icon ? d.h / 2u : d.h;
    if (rows == 0) return false;
    off = d.header_size + d.table_size;
    *rgba = dib_rows_to_rgba(&d, p + off, n - off, rows, &alpha_seen);
    if (*rgba == NULL) return false;
    *w = d.w;
    *h = rows;
    *has_alpha = alpha_seen;
    if (icon && !alpha_seen)
    {
        /* The AND mask follows the color rows: 1 is transparent. */
        uint32_t mstride, msize, x, y;
        const uint8_t *mask;
        if (!dib_stride(d.w, 1, &mstride) || !mul_ok(mstride, rows, &msize)) return false;
        off += (size_t)d.stride * rows;
        if (off > n || msize > n - off)
        {
            free(*rgba);
            return false;
        }
        mask = p + off;
        for (y = 0; y < rows; y++)
        {
            const uint8_t *row = mask + (size_t)(rows - 1u - y) * mstride;
            for (x = 0; x < d.w; x++)
                (*rgba)[4u * ((size_t)y * d.w + x) + 3u] = (row[x / 8u] >> (7u - x % 8u)) & 1u ? 0 : 255;
        }
        *has_alpha = true;
    }
    return true;
}

/* ---------------------------------------------------------------- bitmaps */

static uint32_t bitmap_bytes(const gdi_obj *b)
{
    return b->stride * (uint32_t)(b->h < 0 ? -b->h : b->h);
}

/* A new bitmap object (lock held). dib: DIB section with guest bits. */
static gdi_obj *bitmap_new(uint32_t w, uint32_t h, uint32_t bpp, bool dib, bool top_down, uint32_t *stride_out)
{
    uint32_t stride, size;
    gdi_obj *b;
    if (w == 0 || h == 0 || w > MAX_DIM || h > MAX_DIM) return NULL;
    if (dib)
    {
        if (!dib_stride(w, bpp, &stride)) return NULL;
    }
    else
    {
        /* Device-dependent bitmaps have WORD-aligned rows. */
        uint64_t bits = (uint64_t)w * bpp;
        stride = (uint32_t)(((bits + 15u) / 16u) * 2u);
    }
    if (!mul_ok(stride, h, &size) || size > MAX_BITMAP_BYTES) return NULL;
    b = obj_new(OBJ_BITMAP);
    if (b == NULL) return NULL;
    b->w = (int32_t)w;
    b->h = top_down ? -(int32_t)h : (int32_t)h;
    b->bpp = bpp;
    b->stride = stride;
    b->dib = dib;
    if (dib)
    {
        b->gbits = rt_heap_calloc(1, size);
        if (b->gbits == 0)
        {
            b->handle = 0;
            return NULL;
        }
    }
    else
    {
        b->hbits = calloc(1, size);
        if (b->hbits == NULL) rt_fatal("out of host memory (bitmap)");
    }
    if (stride_out != NULL) *stride_out = stride;
    return b;
}

static void bitmap_free(gdi_obj *b)
{
    if (b->gbits != 0) rt_heap_free(b->gbits);
    free(b->hbits);
    b->handle = 0;
}

uint32_t rt_gdi_bitmap_from_rgba(const uint8_t *rgba, uint32_t w, uint32_t h)
{
    gdi_obj *b;
    uint32_t x, y, hd;
    lock();
    b = bitmap_new(w, h, 32, false, false, NULL);
    if (b == NULL)
    {
        unlock();
        return 0;
    }
    for (y = 0; y < h; y++)
    {
        uint8_t *row = b->hbits + (size_t)y * b->stride; /* DDB rows: top first */
        for (x = 0; x < w; x++)
        {
            const uint8_t *s = rgba + 4u * ((size_t)y * w + x);
            row[4u * x] = s[2];
            row[4u * x + 1u] = s[1];
            row[4u * x + 2u] = s[0];
            row[4u * x + 3u] = s[3];
        }
    }
    hd = b->handle;
    unlock();
    return hd;
}

/* A bitmap from a DIB (header, color table and bits). dib_section: a DIB
   section, else a 32-bit device-dependent bitmap. */
uint32_t rt_gdi_bitmap_from_dib(const uint8_t *p, size_t n, const uint8_t *bits, size_t nbits, bool dib_section)
{
    dib_info d;
    gdi_obj *b;
    uint32_t h, size;
    bool alpha;
    if (!dib_parse(p, n, &d)) return 0;
    if (!dib_section)
    {
        uint8_t *rgba = dib_rows_to_rgba(&d, bits, nbits, d.h, &alpha);
        if (rgba == NULL) return 0;
        if (!alpha && d.bpp == 32)
        {
            /* An alpha byte of 0 in every pixel: an opaque image. */
            uint32_t i;
            for (i = 0; i < d.w * d.h; i++) rgba[4u * i + 3u] = 255;
        }
        h = rt_gdi_bitmap_from_rgba(rgba, d.w, d.h);
        free(rgba);
        return h;
    }
    if (!mul_ok(d.stride, d.h, &size) || size > nbits) return 0;
    lock();
    b = bitmap_new(d.w, d.h, d.bpp, true, d.top_down, NULL);
    if (b == NULL)
    {
        unlock();
        return 0;
    }
    memcpy(G2H(b->gbits), bits, size);
    b->header_size = d.header_size > sizeof b->header ? (uint32_t)sizeof b->header : d.header_size;
    memcpy(b->header, p, b->header_size);
    memcpy(b->masks, d.masks, sizeof b->masks);
    memcpy(b->palette, d.palette, sizeof b->palette);
    b->ncolors = d.ncolors;
    h = b->handle;
    unlock();
    return h;
}

bool rt_gdi_bitmap_rgba(uint32_t h, uint8_t **rgba, uint32_t *w, uint32_t *hgt, bool *has_alpha)
{
    gdi_obj *b;
    dib_info d;
    bool alpha = false;
    lock();
    b = obj_get(h, OBJ_BITMAP);
    if (b == NULL)
    {
        unlock();
        return false;
    }
    memset(&d, 0, sizeof d);
    d.w = (uint32_t)b->w;
    d.h = (uint32_t)(b->h < 0 ? -b->h : b->h);
    d.bpp = b->bpp;
    d.stride = b->stride;
    d.ncolors = b->ncolors;
    memcpy(d.masks, b->masks, sizeof d.masks);
    memcpy(d.palette, b->palette, sizeof d.palette);
    if (b->dib)
    {
        d.top_down = b->h < 0;
        *rgba = dib_rows_to_rgba(&d, G2H(b->gbits), bitmap_bytes(b), d.h, &alpha);
    }
    else
    {
        uint32_t x, y;
        *rgba = malloc((size_t)d.w * d.h * 4u);
        if (*rgba == NULL) rt_fatal("out of host memory (bitmap)");
        for (y = 0; y < d.h; y++)
        {
            const uint8_t *row = b->hbits + (size_t)y * b->stride;
            for (x = 0; x < d.w; x++)
            {
                uint8_t *o = *rgba + 4u * ((size_t)y * d.w + x);
                if (b->bpp == 1)
                {
                    uint8_t v = (row[x / 8u] >> (7u - x % 8u)) & 1u ? 255 : 0;
                    o[0] = o[1] = o[2] = v;
                    o[3] = 255;
                }
                else
                {
                    o[0] = row[4u * x + 2u];
                    o[1] = row[4u * x + 1u];
                    o[2] = row[4u * x];
                    o[3] = row[4u * x + 3u];
                    if (o[3] != 0) alpha = true;
                }
            }
        }
    }
    unlock();
    if (*rgba == NULL) return false;
    *w = d.w;
    *hgt = d.h;
    *has_alpha = alpha;
    return true;
}

/* HBITMAP CreateBitmap(int w, int h, UINT planes, UINT bpp, const void *bits) */
void rt_CreateBitmap(_cpu *cpu)
{
    int32_t w = (int32_t)ARG(0), h = (int32_t)ARG(1);
    uint32_t planes = ARG(2), bpp = ARG(3), bits = ARG(4), hd;
    gdi_obj *b;
    if (w <= 0 || h <= 0 || planes != 1)
    {
        if (w == 0 || h == 0) rt_fatal("CreateBitmap: a bitmap of size 0 (the stock 1x1 bitmap) is not supported");
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (bpp != 1 && bpp != 32) rt_fatal("CreateBitmap: %u bits per pixel are not supported (1 and 32 are)", bpp);
    lock();
    b = bitmap_new((uint32_t)w, (uint32_t)h, bpp, false, false, NULL);
    if (b == NULL)
    {
        unlock();
        rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        RET(0);
        return;
    }
    if (bits != 0) memcpy(b->hbits, G2H(bits), bitmap_bytes(b));
    hd = b->handle;
    unlock();
    RET(hd);
}

/* HBITMAP CreateDIBSection(HDC, const BITMAPINFO *, UINT usage, void **bits,
   HANDLE section, DWORD offset) */
void rt_CreateDIBSection(_cpu *cpu)
{
    uint32_t bmi = ARG(1), usage = ARG(2), pbits = ARG(3), hd, hs, avail = 0;
    dib_info d;
    gdi_obj *b;
    if (ARG(4) != 0) rt_fatal("CreateDIBSection: DIB sections in file mappings are not supported");
    if (usage != 0) rt_fatal("CreateDIBSection: DIB_PAL_COLORS is not supported");
    if (bmi == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    hs = rd32(bmi);
    /* The header, the masks and a full color table. */
    avail = hs + 16u + 256u * 4u;
    if (!dib_parse(G2H(bmi), avail, &d))
    {
        if (pbits != 0) wr32(pbits, 0);
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    lock();
    b = bitmap_new(d.w, d.h, d.bpp, true, d.top_down, NULL);
    if (b == NULL)
    {
        unlock();
        if (pbits != 0) wr32(pbits, 0);
        rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        RET(0);
        return;
    }
    b->header_size = d.header_size > sizeof b->header ? (uint32_t)sizeof b->header : d.header_size;
    memcpy(b->header, G2H(bmi), b->header_size);
    memcpy(b->masks, d.masks, sizeof b->masks);
    memcpy(b->palette, d.palette, sizeof b->palette);
    b->ncolors = d.ncolors;
    hd = b->handle;
    if (pbits != 0) wr32(pbits, b->gbits);
    unlock();
    RET(hd);
}

/* int GetObjectA(HGDIOBJ, int size, LPVOID buf) */
void rt_GetObjectA(_cpu *cpu)
{
    uint32_t h = ARG(0), buf = ARG(2), need;
    int32_t size = (int32_t)ARG(1);
    gdi_obj *o;
    lock();
    o = obj_get(h, 0);
    if (o == NULL || o->type == OBJ_DC)
    {
        unlock();
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    if (o->type == OBJ_FONT)
    {
        /* LOGFONTA of the dialog font: 8 points, "MS Shell Dlg". */
        unlock();
        need = 60;
        if (buf == 0)
        {
            RET(need);
            return;
        }
        if (size < 0) size = 0;
        {
            uint8_t lf[60];
            int32_t height = -11;
            memset(lf, 0, sizeof lf);
            memcpy(lf, &height, 4);
            lf[16] = 0x90; /* lfWeight FW_NORMAL (400), low byte */
            lf[17] = 0x01;
            lf[23] = 0;    /* ANSI_CHARSET */
            memcpy(lf + 28, "MS Shell Dlg", 12);
            memcpy(G2H(buf), lf, (uint32_t)size < need ? (uint32_t)size : need);
            RET((uint32_t)size < need ? (uint32_t)size : need);
        }
        return;
    }
    need = o->dib && size >= 84 ? 84u : 24u;
    if (buf == 0)
    {
        unlock();
        RET(o->dib ? 84u : 24u);
        return;
    }
    if (size < 24)
    {
        unlock();
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    wr32(buf, 0);
    wr32(buf + 4, (uint32_t)o->w);
    wr32(buf + 8, (uint32_t)(o->h < 0 ? -o->h : o->h));
    wr32(buf + 12, o->stride);
    wr16(buf + 16, 1);
    wr16(buf + 18, (uint16_t)o->bpp);
    wr32(buf + 20, o->dib ? o->gbits : 0);
    if (need == 84)
    {
        /* DIBSECTION: the BITMAPINFOHEADER, the masks, no section. */
        uint8_t bih[40];
        memset(bih, 0, sizeof bih);
        memcpy(bih, o->header, o->header_size < 40 ? o->header_size : 40);
        bih[0] = 40;
        bih[1] = bih[2] = bih[3] = 0;
        memcpy(G2H(buf + 24), bih, 40);
        wr32(buf + 64, o->masks[0]);
        wr32(buf + 68, o->masks[1]);
        wr32(buf + 72, o->masks[2]);
        wr32(buf + 76, 0);
        wr32(buf + 80, 0);
    }
    unlock();
    RET(need);
}

/* BOOL DeleteObject(HGDIOBJ) */
void rt_DeleteObject(_cpu *cpu)
{
    uint32_t h = ARG(0);
    gdi_obj *o;
    lock();
    o = obj_get(h, 0);
    if (o == NULL || o->type == OBJ_DC)
    {
        unlock();
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (o->type == OBJ_FONT)
    {
        /* The dialog font is a stock object: it is not deleted. */
        unlock();
        RET(1);
        return;
    }
    bitmap_free(o);
    unlock();
    RET(1);
}

uint32_t rt_gdi_dialog_font(void)
{
    uint32_t h;
    lock();
    if (dialog_font == 0)
    {
        gdi_obj *f = obj_new(OBJ_FONT);
        if (f == NULL) rt_fatal("GDI32: the object table is full");
        dialog_font = f->handle;
    }
    h = dialog_font;
    unlock();
    return h;
}

/* ---------------------------------------------------------------- device contexts */

/* HDC GetDC(HWND): a DC for the client area of the window, or for the
   screen (NULL). */
void rt_GetDC(_cpu *cpu)
{
    uint32_t hwnd = ARG(0), h;
    gdi_obj *o;
    if (hwnd != 0 && !rt_user32_is_window(hwnd))
    {
        rt_set_last_error(1400u); /* ERROR_INVALID_WINDOW_HANDLE */
        RET(0);
        return;
    }
    lock();
    o = obj_new(OBJ_DC);
    if (o == NULL)
    {
        unlock();
        rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        RET(0);
        return;
    }
    o->hwnd = hwnd;
    h = o->handle;
    unlock();
    RET(h);
}

/* int ReleaseDC(HWND, HDC) */
void rt_ReleaseDC(_cpu *cpu)
{
    uint32_t hwnd = ARG(0), h = ARG(1);
    gdi_obj *o;
    lock();
    o = obj_get(h, OBJ_DC);
    if (o == NULL || o->hwnd != hwnd)
    {
        unlock();
        RET(0);
        return;
    }
    o->handle = 0;
    unlock();
    RET(1);
}

/* The display of a DC: the display of its window, else the main display. */
static rt_display dc_display(uint32_t hdc, bool *ok)
{
    rt_display d[RT_MAX_DISPLAYS];
    uint32_t hwnd = 0;
    int n, i, best = 0;
    gdi_obj *o;
    lock();
    o = obj_get(hdc, OBJ_DC);
    *ok = o != NULL;
    if (o != NULL) hwnd = o->hwnd;
    unlock();
    n = rt_cocoa_displays(d);
    if (hwnd != 0)
    {
        rt_rect r = rt_user32_screen_rect(hwnd);
        int32_t cx = r.left + (r.right - r.left) / 2, cy = r.top + (r.bottom - r.top) / 2;
        for (i = 0; i < n; i++)
            if (cx >= d[i].frame.left && cx < d[i].frame.right && cy >= d[i].frame.top && cy < d[i].frame.bottom) best = i;
    }
    return d[best];
}

/* int GetDeviceCaps(HDC, int index) */
void rt_GetDeviceCaps(_cpu *cpu)
{
    uint32_t index = ARG(1);
    bool ok;
    rt_display d = dc_display(ARG(0), &ok);
    int32_t w = d.frame.right - d.frame.left, h = d.frame.bottom - d.frame.top;
    if (!ok)
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    switch (index)
    {
    case 0: RET(0x0400u); return;                   /* DRIVERVERSION */
    case 2: RET(1u); return;                        /* TECHNOLOGY: DT_RASDISPLAY */
    case 4: RET((uint32_t)(w * 254 / 960)); return;  /* HORZSIZE (mm at 96 dpi) */
    case 6: RET((uint32_t)(h * 254 / 960)); return;  /* VERTSIZE */
    case 8: RET((uint32_t)w); return;               /* HORZRES */
    case 10: RET((uint32_t)h); return;              /* VERTRES */
    case 12: RET(32u); return;                      /* BITSPIXEL */
    case 14: RET(1u); return;                       /* PLANES */
    case 24: RET(0xFFFFFFFFu); return;              /* NUMCOLORS: more than 256 */
    case 38: RET(0x7E99u); return;                  /* RASTERCAPS of a true-color display */
    case 40: RET(36u); return;                      /* ASPECTX */
    case 42: RET(36u); return;                      /* ASPECTY */
    case 44: RET(51u); return;                      /* ASPECTXY */
    case 88: RET(96u); return;                      /* LOGPIXELSX */
    case 90: RET(96u); return;                      /* LOGPIXELSY */
    case 104: RET(0u); return;                      /* SIZEPALETTE */
    case 106: RET(0u); return;                      /* NUMRESERVED */
    case 108: RET(24u); return;                     /* COLORRES */
    case 110: case 111: case 112: case 113: RET(0u); return; /* printer values */
    case 116: RET(d.refresh); return;               /* VREFRESH */
    case 117: RET((uint32_t)h); return;             /* DESKTOPVERTRES */
    case 118: RET((uint32_t)w); return;             /* DESKTOPHORZRES */
    case 119: RET(1u); return;                      /* BLTALIGNMENT */
    case 120: RET(0u); return;                      /* SHADEBLENDCAPS */
    case 121: RET(0u); return;                      /* COLORMGMTCAPS: CM_NONE */
    default: rt_fatal("GetDeviceCaps: index %u is not supported", index);
    }
}

/* ---------------------------------------------------------------- pixel formats (no OpenGL) */

bool rt_gdi32_is_dc(uint32_t hdc)
{
    bool r;
    lock();
    r = obj_get(hdc, OBJ_DC) != NULL;
    unlock();
    return r;
}

/* int ChoosePixelFormat(HDC, const PIXELFORMATDESCRIPTOR *) */
void rt_ChoosePixelFormat(_cpu *cpu)
{
    bool ok;
    (void)dc_display(ARG(0), &ok);
    rt_set_last_error(ok ? ERROR_INVALID_PIXEL_FORMAT : ERROR_INVALID_HANDLE);
    RET(0);
}

/* BOOL SetPixelFormat(HDC, int format, const PIXELFORMATDESCRIPTOR *) */
void rt_SetPixelFormat(_cpu *cpu)
{
    bool ok;
    (void)dc_display(ARG(0), &ok);
    rt_set_last_error(ok ? ERROR_INVALID_PIXEL_FORMAT : ERROR_INVALID_HANDLE);
    RET(0);
}

/* BOOL SwapBuffers(HDC) */
void rt_SwapBuffers(_cpu *cpu)
{
    bool ok;
    (void)dc_display(ARG(0), &ok);
    rt_set_last_error(ok ? ERROR_INVALID_PIXEL_FORMAT : ERROR_INVALID_HANDLE);
    RET(0);
}
