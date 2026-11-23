/*
 * make_icon: draw the application icon of the port into an .iconset folder.
 *
 * Usage: make_icon <folder.iconset>
 *
 * The icon is art of the port, not art of the game. It uses the colors of
 * the launcher window (runtime/loader/rt_launcher.m): a dark ground and
 * gold. Two gold pillars stand on a horizon line, and a shaft of warm light
 * rises in the gap between them.
 *
 * The tool draws the shapes with Core Graphics for each size of a macOS
 * icon (16 to 1024 pixels), so each size is sharp. The shapes are on a
 * 1024 x 1024 grid with the body of the icon at 100..924 (the macOS icon
 * grid). Sizes below 64 pixels omit the flutes of the pillars. The tool
 * writes icon_<n>x<n>.png and icon_<n>x<n>@2x.png; iconutil makes the
 * .icns file from the folder (tools/package_release.sh).
 *
 * Errors: the tool writes the cause to stderr and exits with status 1.
 */
#include <ApplicationServices/ApplicationServices.h>
#include <ImageIO/ImageIO.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define GRID 1024.0

typedef struct rgba {
    CGFloat r, g, b, a;
} rgba;

static rgba hex(unsigned v, CGFloat a)
{
    rgba c = {(CGFloat)((v >> 16) & 0xFF) / 255.0, (CGFloat)((v >> 8) & 0xFF) / 255.0, (CGFloat)(v & 0xFF) / 255.0, a};
    return c;
}

static CGGradientRef gradient(CGColorSpaceRef cs, const rgba *stops, const CGFloat *at, size_t n)
{
    CGFloat comp[4 * 8];
    size_t i;
    CGGradientRef g;
    if (n > 8) return NULL;
    for (i = 0; i < n; i++)
    {
        comp[4 * i] = stops[i].r;
        comp[4 * i + 1] = stops[i].g;
        comp[4 * i + 2] = stops[i].b;
        comp[4 * i + 3] = stops[i].a;
    }
    g = CGGradientCreateWithColorComponents(cs, comp, at, n);
    return g;
}

/* A pillar: base, shaft (narrower at the top) and capital. lit < 0: the
   light is on the right side of the pillar (the left pillar); lit > 0: on
   the left side. */
static int pillar(CGContextRef cx, CGColorSpaceRef cs, CGFloat center, CGFloat lit, int flutes)
{
    const rgba stops[3] = {hex(0xF0D184, 1), hex(0xB8913A, 1), hex(0x5E4514, 1)};
    const CGFloat at[3] = {0, 0.45, 1};
    CGGradientRef g = gradient(cs, stops, at, 3);
    CGMutablePathRef p = CGPathCreateMutable();
    CGFloat bright = lit < 0 ? 1 : -1; /* side of the bright edge, from the center */
    int i;
    if (g == NULL || p == NULL)
    {
        if (g != NULL) CGGradientRelease(g);
        if (p != NULL) CGPathRelease(p);
        return -1;
    }
    /* base */
    CGPathAddRect(p, NULL, CGRectMake(center - 78, 258, 156, 34));
    CGPathAddRect(p, NULL, CGRectMake(center - 66, 292, 132, 14));
    /* shaft */
    CGPathMoveToPoint(p, NULL, center - 52, 306);
    CGPathAddLineToPoint(p, NULL, center + 52, 306);
    CGPathAddLineToPoint(p, NULL, center + 45, 690);
    CGPathAddLineToPoint(p, NULL, center - 45, 690);
    CGPathCloseSubpath(p);
    /* capital */
    CGPathAddRect(p, NULL, CGRectMake(center - 62, 690, 124, 16));
    CGPathAddRect(p, NULL, CGRectMake(center - 74, 706, 148, 26));
    CGPathAddRect(p, NULL, CGRectMake(center - 84, 732, 168, 20));
    CGContextSaveGState(cx);
    CGContextAddPath(cx, p);
    CGContextClip(cx);
    CGContextDrawLinearGradient(cx, g, CGPointMake(center + bright * 84, 0), CGPointMake(center - bright * 84, 0), 0);
    if (flutes)
    {
        /* three flutes on the shaft */
        for (i = -1; i <= 1; i++)
        {
            CGFloat x = center + (CGFloat)i * 26;
            CGContextSetRGBFillColor(cx, 0.16, 0.11, 0.03, 0.30);
            CGContextFillRect(cx, CGRectMake(x - 3, 318, 6, 360));
            CGContextSetRGBFillColor(cx, 1, 0.94, 0.75, 0.18);
            CGContextFillRect(cx, CGRectMake(x + 3 * bright - (bright > 0 ? 0 : 3), 318, 3, 360));
        }
    }
    CGContextRestoreGState(cx);
    CGPathRelease(p);
    CGGradientRelease(g);
    return 0;
}

static int draw(CGContextRef cx, CGColorSpaceRef cs, size_t px)
{
    const CGRect body = CGRectMake(100, 100, 824, 824);
    const rgba ground[2] = {hex(0x2A2318, 1), hex(0x0B0A08, 1)};
    const rgba glow[2] = {hex(0xE8C36A, 0.42), hex(0xE8C36A, 0)};
    const rgba shaft[3] = {hex(0xFFF4D6, 0), hex(0xFFF4D6, 0.96), hex(0xFFF4D6, 0)};
    const CGFloat at2[2] = {0, 1}, at3[3] = {0, 0.42, 1};
    CGGradientRef gGround = gradient(cs, ground, at2, 2), gGlow = gradient(cs, glow, at2, 2),
                  gShaft = gradient(cs, shaft, at3, 3);
    CGPathRef shape = CGPathCreateWithRoundedRect(body, 185, 185, NULL);
    int rc = -1;
    if (gGround == NULL || gGlow == NULL || gShaft == NULL || shape == NULL) goto done;
    CGContextScaleCTM(cx, (CGFloat)px / GRID, (CGFloat)px / GRID);

    /* drop shadow of the body */
    CGContextSaveGState(cx);
    CGContextSetShadowWithColor(cx, CGSizeMake(0, -10 * (CGFloat)px / GRID), 26 * (CGFloat)px / GRID,
                                CGColorGetConstantColor(kCGColorBlack));
    CGContextSetRGBFillColor(cx, 0.04, 0.035, 0.03, 0.6);
    CGContextAddPath(cx, shape);
    CGContextFillPath(cx);
    CGContextRestoreGState(cx);

    CGContextSaveGState(cx);
    CGContextAddPath(cx, shape);
    CGContextClip(cx);
    CGContextDrawLinearGradient(cx, gGround, CGPointMake(0, 924), CGPointMake(0, 100), 0);
    /* the light behind the pillars */
    CGContextDrawRadialGradient(cx, gGlow, CGPointMake(512, 500), 0, CGPointMake(512, 500), 380, 0);
    /* the horizon line and its reflection */
    CGContextSetRGBFillColor(cx, 0.72, 0.57, 0.23, 0.85);
    CGContextFillRect(cx, CGRectMake(196, 252, 632, 5));
    CGContextSetRGBFillColor(cx, 0.72, 0.57, 0.23, 0.18);
    CGContextFillRect(cx, CGRectMake(250, 232, 524, 3));
    /* the shaft of light in the gap */
    CGContextSaveGState(cx);
    CGContextAddEllipseInRect(cx, CGRectMake(512 - 34, 258, 68, 560));
    CGContextClip(cx);
    CGContextDrawLinearGradient(cx, gShaft, CGPointMake(0, 258), CGPointMake(0, 818), 0);
    CGContextRestoreGState(cx);
    if (pillar(cx, cs, 356, -1, px >= 64) != 0 || pillar(cx, cs, 668, 1, px >= 64) != 0)
    {
        CGContextRestoreGState(cx);
        goto done;
    }
    CGContextRestoreGState(cx);

    /* a thin rim of light at the edge of the body */
    CGContextSaveGState(cx);
    CGContextAddPath(cx, shape);
    CGContextSetRGBStrokeColor(cx, 0.83, 0.69, 0.35, 0.22);
    CGContextSetLineWidth(cx, px >= 64 ? 3 : 6);
    CGContextStrokePath(cx);
    CGContextRestoreGState(cx);
    rc = 0;
done:
    if (gGround != NULL) CGGradientRelease(gGround);
    if (gGlow != NULL) CGGradientRelease(gGlow);
    if (gShaft != NULL) CGGradientRelease(gShaft);
    if (shape != NULL) CGPathRelease(shape);
    return rc;
}

static int write_png(const char *folder, const char *name, size_t px)
{
    char path[1024];
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef cx = NULL;
    CGImageRef img = NULL;
    CFURLRef url = NULL;
    CGImageDestinationRef dst = NULL;
    int rc = -1;
    if ((size_t)snprintf(path, sizeof path, "%s/%s", folder, name) >= sizeof path)
    {
        fprintf(stderr, "make_icon: the path %s/%s is too long\n", folder, name);
        goto done;
    }
    if (cs == NULL) goto fail;
    cx = CGBitmapContextCreate(NULL, px, px, 8, 0, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    if (cx == NULL) goto fail;
    CGContextClearRect(cx, CGRectMake(0, 0, (CGFloat)px, (CGFloat)px));
    if (draw(cx, cs, px) != 0) goto fail;
    img = CGBitmapContextCreateImage(cx);
    url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    if (img == NULL || url == NULL) goto fail;
    dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    if (dst == NULL) goto fail;
    CGImageDestinationAddImage(dst, img, NULL);
    if (!CGImageDestinationFinalize(dst)) goto fail;
    rc = 0;
    goto done;
fail:
    fprintf(stderr, "make_icon: %s (%zu pixels) cannot be drawn or written\n", path, px);
done:
    if (dst != NULL) CFRelease(dst);
    if (url != NULL) CFRelease(url);
    if (img != NULL) CGImageRelease(img);
    if (cx != NULL) CGContextRelease(cx);
    if (cs != NULL) CGColorSpaceRelease(cs);
    return rc;
}

int main(int argc, char **argv)
{
    static const size_t sizes[] = {16, 32, 128, 256, 512};
    char name[64];
    size_t i;
    if (argc != 2)
    {
        fprintf(stderr, "usage: make_icon <folder.iconset>\n");
        return 1;
    }
    if (mkdir(argv[1], 0755) != 0 && errno != EEXIST)
    {
        fprintf(stderr, "make_icon: %s cannot be made: %s\n", argv[1], strerror(errno));
        return 1;
    }
    for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
    {
        snprintf(name, sizeof name, "icon_%zux%zu.png", sizes[i], sizes[i]);
        if (write_png(argv[1], name, sizes[i]) != 0) return 1;
        snprintf(name, sizeof name, "icon_%zux%zu@2x.png", sizes[i], sizes[i]);
        if (write_png(argv[1], name, 2 * sizes[i]) != 0) return 1;
    }
    return 0;
}
