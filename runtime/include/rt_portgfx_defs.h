/*
 * The graphics settings of the port: the keys, ranges and initial values of
 * the settings file <work directory>/user/PortGraphics.cfg. The runtime
 * (rt_portgfx.c) and the launcher window of the loader
 * (runtime/loader/rt_launcher.m) use this one table, so that both read and
 * write the same file. The index of an entry is the setting id (the RT_GFX_*
 * values of rt.h and the ids of tools/port_graphics/PortGraphics.cs).
 *
 * The file has one "<key> <value>" line for each setting. A line that
 * starts with '#' is a comment.
 */
#ifndef RT_PORTGFX_DEFS_H
#define RT_PORTGFX_DEFS_H

#include <stdbool.h>

typedef struct rt_gfx_def {
    const char *key;
    int min, max, initial;
    bool restart; /* takes effect at the next start of the game */
} rt_gfx_def;

#define RT_GFX_DEF_COUNT 6

static const rt_gfx_def rt_gfx_defs[RT_GFX_DEF_COUNT] = {
    { "upscaling", 0, 1, 1, false },
    { "sharpness", 0, 8, 7, false },
    { "hdr", 0, 1, 1, false },
    { "anisotropic", 0, 1, 1, false },
    { "supersampled_shadows", 0, 1, 1, true },
    { "float_render_targets", 0, 1, 1, true },
};

/* The first line of the file. */
#define RT_GFX_FILE_COMMENT "# Graphics settings of the port (Graphics tab of the options window)\n"

#endif
