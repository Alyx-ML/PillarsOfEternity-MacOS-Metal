/*
 * Graphics settings of the port, and the system DLL "portgfx".
 *
 * The port adds rows to the Graphics tab of the game's options window
 * (tools/port_graphics/PortGraphics.cs). Each row is one setting of the
 * Direct3D 9 runtime:
 *
 *   id  key                    values  effect
 *   0   upscaling              0, 1    MetalFX spatial upscaling at present
 *   1   sharpness              0..8    RCAS sharpening at present: 0 = off,
 *                                      else attenuation (8 - value) / 4 stops
 *   2   hdr                    0, 1    HDR output with GT7 tone mapping
 *   3   anisotropic            0, 1    16x anisotropic filtering
 *   4   supersampled_shadows   0, 1    4x4 storage of the shadow buffers
 *   5   float_render_targets   0, 1    RGBA32Float storage of 8-bit targets
 *
 * Settings 0 to 3 take effect at the next frame. Settings 4 and 5 change the
 * storage of render targets when the runtime makes them; a target cannot
 * change its storage, so these two take effect at the next start of the
 * program. rt_portgfx_get gives the value in effect, PortGfxGet the saved
 * value.
 *
 * The settings file (rt_portgfx_init) has one "key value" line for each
 * setting; lines that start with '#' are comments. A file that is not there
 * gives the initial values (all effects on, sharpness 7 = 0.25 stops, the
 * values of the runtime before this file existed). A line that cannot be
 * read stops the program with a message. PortGfxSet writes the complete
 * file (a temporary file, then rename); if the file cannot be written, the
 * setting keeps its old value and PortGfxSet returns 0.
 *
 * Calls into the managed code: the managed half of the rows runs in the
 * current domain of the main thread (the domain of the game's scripts).
 * rt_portgfx_frame (each Present) marks a frame, and
 * the next PeekMessage of the main thread (rt_portgfx_pump) calls
 * rt_mono_port_tick once: the managed Tick runs between two frames of the
 * player loop, on the main thread.
 */
#include "rt.h"
#include "rt_thread.h"
#include "rt_portgfx_defs.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The table of the settings is rt_gfx_defs (shared with the launcher
   window). */
_Static_assert(RT_GFX_DEF_COUNT == RT_GFX_COUNT, "rt_portgfx_defs.h needs one entry for each RT_GFX_* id");

static char cfg_path[1024];
static bool cfg_ready;
/* saved: the values of the file. active: the values in effect. */
static _Atomic int saved[RT_GFX_COUNT], active[RT_GFX_COUNT];
static atomic_bool frame_mark;
/* PortGfxSet runs on the main thread; the lock keeps the file and saved[]
   in step if another thread calls it. */
static pthread_mutex_t cfg_lock = PTHREAD_MUTEX_INITIALIZER;

static void cfg_lock_take(void)
{
    int r = pthread_mutex_lock(&cfg_lock);
    if (r != 0) rt_fatal("graphics settings lock failed (%d)", r);
}

static void cfg_lock_drop(void)
{
    int r = pthread_mutex_unlock(&cfg_lock);
    if (r != 0) rt_fatal("graphics settings unlock failed (%d)", r);
}

void rt_portgfx_init(const char *path)
{
    FILE *f;
    char line[256];
    unsigned n = 0;
    int i;
    if (cfg_ready) rt_fatal("rt_portgfx_init: called a second time");
    if ((size_t)snprintf(cfg_path, sizeof cfg_path, "%s", path) >= sizeof cfg_path)
        rt_fatal("the graphics settings path %s is too long", path);
    for (i = 0; i < RT_GFX_COUNT; i++) atomic_store(&saved[i], rt_gfx_defs[i].initial);
    f = fopen(cfg_path, "r");
    if (f == NULL)
    {
        if (errno != ENOENT) rt_fatal("the graphics settings file %s cannot be read: %s", cfg_path, strerror(errno));
        rt_log("graphics settings: %s is not there; the initial values are used", cfg_path);
    }
    else
    {
        while (fgets(line, sizeof line, f) != NULL)
        {
            char key[64], extra[2];
            int value, k;
            n++;
            if (strchr(line, '\n') == NULL && !feof(f))
                rt_fatal("graphics settings: %s line %u is too long", cfg_path, n);
            if (line[0] == '#' || line[strspn(line, " \t\r\n")] == 0) continue;
            if (sscanf(line, "%63s %d %1s", key, &value, extra) != 2)
                rt_fatal("graphics settings: %s line %u is not \"key value\"", cfg_path, n);
            for (k = 0; k < RT_GFX_COUNT; k++)
                if (strcmp(key, rt_gfx_defs[k].key) == 0) break;
            if (k == RT_GFX_COUNT) rt_fatal("graphics settings: %s line %u has the unknown key %s", cfg_path, n, key);
            if (value < rt_gfx_defs[k].min || value > rt_gfx_defs[k].max)
                rt_fatal("graphics settings: %s line %u: %s must be %d to %d", cfg_path, n, key, rt_gfx_defs[k].min,
                         rt_gfx_defs[k].max);
            atomic_store(&saved[k], value);
        }
        if (ferror(f)) rt_fatal("the graphics settings file %s cannot be read", cfg_path);
        fclose(f);
    }
    for (i = 0; i < RT_GFX_COUNT; i++) atomic_store(&active[i], atomic_load(&saved[i]));
    rt_log("graphics settings: upscaling %d, sharpness %d, hdr %d, anisotropic %d, supersampled shadows %d, "
           "float render targets %d",
           atomic_load(&active[RT_GFX_UPSCALING]), atomic_load(&active[RT_GFX_SHARPNESS]),
           atomic_load(&active[RT_GFX_HDR]), atomic_load(&active[RT_GFX_ANISOTROPIC]),
           atomic_load(&active[RT_GFX_SHADOWS]), atomic_load(&active[RT_GFX_FLOAT_TARGETS]));
    cfg_ready = true;
}

int rt_portgfx_get(int id)
{
    if (id < 0 || id >= RT_GFX_COUNT) rt_fatal("rt_portgfx_get: setting %d does not exist", id);
    return atomic_load(&active[id]);
}

/* Write all saved values with saved[id] = value. Returns false (and logs the
   cause) if the file cannot be written. */
static bool write_file(int id, int value)
{
    char tmp[1100];
    FILE *f;
    int i;
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", cfg_path) >= sizeof tmp)
        rt_fatal("the graphics settings path %s is too long", cfg_path);
    f = fopen(tmp, "w");
    if (f == NULL)
    {
        rt_log("graphics settings: %s cannot be written: %s", tmp, strerror(errno));
        return false;
    }
    fputs(RT_GFX_FILE_COMMENT, f);
    for (i = 0; i < RT_GFX_COUNT; i++) fprintf(f, "%s %d\n", rt_gfx_defs[i].key, i == id ? value : atomic_load(&saved[i]));
    if (ferror(f) || fclose(f) != 0)
    {
        rt_log("graphics settings: %s cannot be written: %s", tmp, strerror(errno));
        (void)remove(tmp);
        return false;
    }
    if (rename(tmp, cfg_path) != 0)
    {
        rt_log("graphics settings: %s cannot be renamed to %s: %s", tmp, cfg_path, strerror(errno));
        (void)remove(tmp);
        return false;
    }
    return true;
}

/* int __stdcall PortGfxGet(int id): the saved value. */
void rt_PortGfxGet(_cpu *cpu)
{
    uint32_t id = ARG(0);
    if (!cfg_ready) rt_fatal("PortGfxGet: the graphics settings are not loaded");
    if (id >= RT_GFX_COUNT) rt_fatal("PortGfxGet: setting %u does not exist", id);
    RET((uint32_t)atomic_load(&saved[id]));
}

/* int __stdcall PortGfxSet(int id, int value): save the value; a setting
   without "restart" also takes effect. Returns 1, or 0 if the file cannot
   be written (the setting keeps its old value). */
void rt_PortGfxSet(_cpu *cpu)
{
    uint32_t id = ARG(0);
    int32_t value = (int32_t)ARG(1);
    bool ok;
    if (!cfg_ready) rt_fatal("PortGfxSet: the graphics settings are not loaded");
    if (id >= RT_GFX_COUNT) rt_fatal("PortGfxSet: setting %u does not exist", id);
    if (value < rt_gfx_defs[id].min || value > rt_gfx_defs[id].max)
        rt_fatal("PortGfxSet: %s must be %d to %d (%d)", rt_gfx_defs[id].key, rt_gfx_defs[id].min, rt_gfx_defs[id].max, value);
    cfg_lock_take();
    ok = atomic_load(&saved[id]) == value || write_file((int)id, value);
    if (ok)
    {
        atomic_store(&saved[id], value);
        if (!rt_gfx_defs[id].restart) atomic_store(&active[id], value);
    }
    cfg_lock_drop();
    if (ok)
        rt_log("graphics settings: %s %d%s", rt_gfx_defs[id].key, value,
               rt_gfx_defs[id].restart && value != atomic_load(&active[id]) ? " (takes effect at the next start)" : "");
    RET(ok ? 1u : 0u);
}

void rt_portgfx_frame(void)
{
    atomic_store(&frame_mark, true);
}

void rt_portgfx_pump(_cpu *cpu)
{
    static bool busy;
    if (busy || !atomic_exchange(&frame_mark, false)) return;
    busy = true;
    rt_mono_port_tick(cpu);
    busy = false;
}

/* void __stdcall PortGfxFail(const char *message): the managed rows found an
   error (an exception in Tick or in a handler); stop with the message. */
void rt_PortGfxFail(_cpu *cpu)
{
    char msg[2048];
    uint32_t g = ARG(0), i;
    if (g == 0) rt_fatal("PortGraphics: an error without a message");
    for (i = 0; i + 1 < sizeof msg && rd8(g + i) != 0; i++) msg[i] = (char)rd8(g + i);
    msg[i] = 0;
    rt_fatal("PortGraphics: %s%s", msg, rd8(g + i) != 0 ? " [...]" : "");
}
