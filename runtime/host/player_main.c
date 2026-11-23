/*
 * Host of the translated game program (PillarsOfEternity.exe, the Unity
 * player): it maps the translated modules and runs the entry point of the
 * program on the main thread.
 *
 *   PillarsOfEternity <game directory> <work directory> [<player argument>...]
 *
 * Guest file system:
 *   C:\PillarsOfEternity   the game directory (the player's files)
 *   C:\Users\Player        <work directory>/user
 *   C:\ProgramData         <work directory>/programdata
 *   C:\PortRuntime         the directory "aot" next to this executable
 *
 * Environment variables (each "1" or "0"; another value stops the host):
 *   RT_HEADLESS  1: no window is shown and no frame is presented on the
 *                screen; the program draws its frames on the GPU as
 *                usual. The program must run in a window
 *                (-screen-fullscreen 0), because a display mode change
 *                stops the process in this mode. A message box stops the
 *                process with its text.
 *   RT_MUTE      1: the audio output sends silence (rt_audio_set_muted).
 *   RT_INPUT     the path of an input script (rt_user32_input_script) for a
 *                headless run: timed mouse and key events.
 *   RT_CAPTURE   a number of seconds from 1 to 3600: the back buffer of a
 *                presented frame goes to
 *                <work directory>/capture/<start time>/NNNNNN.png, at most
 *                one frame for each interval (rt_d3d9_capture_init).
 *
 * Host files: <work directory>/pipelines.cache keeps the recipes of the
 * Metal render pipelines of the Direct3D 9 runtime between runs
 * (rt_d3d9_pipeline_cache_init).
 *
 * Modules:
 *   - PillarsOfEternity.exe of the game directory at 0x26000000, the
 *     address of the translation. The base relocations are applied
 *     (rt_module_load_program).
 *   - PillarsOfEternity_data/Mono/mono.dll of the game directory at its
 *     image base. The program loads it with LoadLibrary. The runtime
 *     cannot run code that the Mono JIT writes, so Mono runs in AOT-only
 *     mode (rt_mono_set_aot_only).
 *   - PillarsOfEternity_data/Plugins/ShroudUnityPlugin.dll (the cloth
 *     simulation plugin) of the game directory at 0x11000000, the address
 *     of its translation (work/shroud). Its preferred base 0x10000000 is the
 *     base of mono.dll, so the base relocations are applied
 *     (rt_module_load_pe_at). Mono loads it with LoadLibrary for the
 *     P/Invoke calls of ShroudObjectManager.
 *   - The translated AOT image of each linked assembly (aot_images.h), from
 *     the directory "aot" next to this executable. Mono loads the AOT image
 *     of an assembly as "<assembly path>.dll" with LoadLibrary.
 *   - The AOT instance assembly PortAotInstances.dll
 *     (tools/build_aot_instances.sh) and its AOT image are in
 *     C:\PortRuntime. The other AOT images are in the Managed directory of
 *     the game. Mono opens the assembly when the program starts Mono
 *     (rt_mono_use_aot_instances). If its image is not in aot_images.h, the
 *     host stops.
 *   - The assembly PortGraphics.dll (tools/build_port_graphics.sh: the
 *     port's rows on the Graphics tab of the options window) and its AOT
 *     image are also in C:\PortRuntime. After Mono has loaded
 *     Assembly-CSharp, the first tick opens the assembly
 *     (rt_mono_use_port_graphics). If its image is not in aot_images.h, the
 *     host stops. The settings of these rows are in
 *     <work directory>/user/PortGraphics.cfg (rt_portgfx_init).
 *
 * WARNING: the executable contains the translated code of the game. Do not
 * distribute it, upload it or commit it.
 */
#include "aot_images.h"
#include "rt.h"
#include "rt_file.h"
#include "rt_module.h"
#include "rt_thread.h"
#include "rt_user32.h"
#include "rt_window.h"

#include <limits.h>
#include <stdbool.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* SHA-256 of the files of the supported game version. */
#define EXE_SHA256 "053c3fd4de19abced9be429d1d89f779f2da6bc13d9706c9768daf294c01931d"
#define MONO_SHA256 "4e926907c947611b9fd6c88e9d8902d187a9a89c796e935937fd9b303419674d"
#define SHROUD_SHA256 "43ec63429a4c708ed2e55abb23488704592d785c8230e9bf83adc67fece20a26"
/* The guest address of the translation of ShroudUnityPlugin.dll
   (tools/rebase_pe.py, work/shroud). */
#define SHROUD_LOAD_BASE 0x11000000u
/* The guest address of the translation of the program
   (tools/rebase_pe.py, work/player). */
#define EXE_LOAD_BASE 0x26000000u

#define EXE_GUEST_PATH RT_GUEST_GAME_DIR "\\PillarsOfEternity.exe"
#define MONO_GUEST_PATH RT_GUEST_GAME_DIR "\\PillarsOfEternity_data\\Mono\\mono.dll"
#define SHROUD_GUEST_PATH RT_GUEST_GAME_DIR "\\PillarsOfEternity_data\\Plugins\\ShroudUnityPlugin.dll"
#define MANAGED_GUEST_DIR RT_GUEST_GAME_DIR "\\PillarsOfEternity_data\\Managed"
#define AOT_INSTANCES_ASSEMBLY "PortAotInstances.dll"
#define AOT_INSTANCES_IMAGE AOT_INSTANCES_ASSEMBLY ".dll"
/* The port's rows on the Graphics tab (tools/build_port_graphics.sh). */
#define PORT_GRAPHICS_ASSEMBLY "PortGraphics.dll"
#define PORT_GRAPHICS_IMAGE PORT_GRAPHICS_ASSEMBLY ".dll"

extern const uint8_t mono_code_map[];
extern const uint8_t player_code_map[];
extern const uint8_t shroud_code_map[];

/* Append s to the command line buf (capacity cap) with the quoting rules
   of CommandLineToArgvW: quotes around an argument with a space, a tab or
   a quote; 2n backslashes before a quote become n, a quote in the argument
   is \". */
static void append_arg(char *buf, size_t cap, size_t *len, const char *s)
{
    bool quote = s[0] == 0 || strpbrk(s, " \t\"") != NULL;
    size_t need = strlen(s) * 2 + 4, i, bs = 0;
    char *p;
    if (*len + need >= cap) rt_fatal("the command line is too long");
    p = buf + *len;
    if (*len != 0) *p++ = ' ';
    if (quote) *p++ = '"';
    for (i = 0; s[i] != 0; i++)
    {
        if (s[i] == '\\')
        {
            bs++;
            *p++ = '\\';
            continue;
        }
        if (s[i] == '"')
        {
            size_t k;
            for (k = 0; k < bs + 1; k++) *p++ = '\\';
        }
        bs = 0;
        *p++ = s[i];
    }
    if (quote)
    {
        size_t k;
        for (k = 0; k < bs; k++) *p++ = '\\';
        *p++ = '"';
    }
    *p = 0;
    *len = (size_t)(p - buf);
}

static void real_dir(const char *in, char out[PATH_MAX], const char *what)
{
    if (realpath(in, out) == NULL) rt_fatal("the %s %s does not exist", what, in);
}

/* The directory of this executable. */
static void host_dir(char out[PATH_MAX])
{
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    char *slash;
    if (_NSGetExecutablePath(buf, &size) != 0) rt_fatal("the path of the host executable is too long");
    if (realpath(buf, out) == NULL) rt_fatal("the host executable %s was not found", buf);
    slash = strrchr(out, '/');
    if (slash == NULL) rt_fatal("the host executable path %s has no directory", out);
    *slash = 0;
}

/* A switch of the environment: unset, empty or "0" is off, "1" is on. */
static bool env_switch(const char *name)
{
    const char *v = getenv(name);
    if (v == NULL || v[0] == 0 || strcmp(v, "0") == 0) return false;
    if (strcmp(v, "1") == 0) return true;
    rt_fatal("%s=%s: the value must be 0 or 1", name, v);
}

/* RT_CAPTURE: the frame capture directory of this run. */
static void capture_setup(const char *work)
{
    const char *v = getenv("RT_CAPTURE");
    char dir[PATH_MAX], *end;
    unsigned long s;
    time_t now;
    struct tm tm;
    if (v == NULL || v[0] == 0) return;
    errno = 0;
    s = strtoul(v, &end, 10);
    if (errno != 0 || *end != 0 || s < 1 || s > 3600) rt_fatal("RT_CAPTURE=%s: the value must be 1 to 3600 seconds", v);
    now = time(NULL);
    if (now == (time_t)-1 || localtime_r(&now, &tm) == NULL) rt_fatal("RT_CAPTURE: the time is not available");
    if ((size_t)snprintf(dir, sizeof dir, "%s/capture", work) >= sizeof dir) rt_fatal("RT_CAPTURE: path too long");
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) rt_fatal("RT_CAPTURE: %s could not be made: %s", dir, strerror(errno));
    if ((size_t)snprintf(dir, sizeof dir, "%s/capture/%04d%02d%02d-%02d%02d%02d", work, tm.tm_year + 1900, tm.tm_mon + 1,
                         tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec) >= sizeof dir)
        rt_fatal("RT_CAPTURE: path too long");
    if (mkdir(dir, 0755) != 0) rt_fatal("RT_CAPTURE: %s could not be made: %s", dir, strerror(errno));
    rt_d3d9_capture_init(dir, (unsigned)s);
    rt_log("frame capture: %s, one frame each %lu s", dir, s);
}

int main(int argc, char **argv)
{
    char game[PATH_MAX], work[PATH_MAX], self[PATH_MAX], user[PATH_MAX + 16], data[PATH_MAX + 16];
    char pcache[PATH_MAX + 32], gfxcfg[PATH_MAX + 32];
    char exe_host[PATH_MAX + 64], mono_host[PATH_MAX + 64], shroud_host[PATH_MAX + 64], aot_host[PATH_MAX + 128], aot_guest[RT_PATH_BYTES];
    char aot_dir[PATH_MAX + 16];
    bool have_instances = false, have_graphics = false, headless;
    char cmdline[32768];
    size_t len = 0;
    rt_thread *t;
    uint32_t exe, mono, code;
    unsigned i;
    int a;

    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <game directory> <work directory> [<player argument>...]\n", argv[0]);
        return 2;
    }
    real_dir(argv[1], game, "game directory");
    real_dir(argv[2], work, "work directory");
    host_dir(self);
    if ((size_t)snprintf(user, sizeof(user), "%s/user", work) >= sizeof(user) ||
        (size_t)snprintf(data, sizeof(data), "%s/programdata", work) >= sizeof(data) ||
        (size_t)snprintf(pcache, sizeof(pcache), "%s/pipelines.cache", work) >= sizeof(pcache) ||
        (size_t)snprintf(gfxcfg, sizeof(gfxcfg), "%s/user/PortGraphics.cfg", work) >= sizeof(gfxcfg) ||
        (size_t)snprintf(aot_dir, sizeof(aot_dir), "%s/aot", self) >= sizeof(aot_dir) ||
        (size_t)snprintf(exe_host, sizeof(exe_host), "%s/PillarsOfEternity.exe", game) >= sizeof(exe_host) ||
        (size_t)snprintf(mono_host, sizeof(mono_host), "%s/PillarsOfEternity_data/Mono/mono.dll", game) >= sizeof(mono_host) ||
        (size_t)snprintf(shroud_host, sizeof(shroud_host), "%s/PillarsOfEternity_data/Plugins/ShroudUnityPlugin.dll", game) >=
            sizeof(shroud_host))
        rt_fatal("host path too long");

    rt_mem_init();
    if (ptr_initialize_pointers == NULL) rt_fatal("the translated modules have no pointer initialization");
    ptr_initialize_pointers(rt_ptrofs);
    rt_modules_init(EXE_GUEST_PATH);
    /* The translated images must be at the addresses of their translations.
       Map them before the main thread and the heap allocate guest memory
       (see runtime/host/aot_main.c). */
    exe = rt_module_load_program(EXE_GUEST_PATH, exe_host, EXE_SHA256, player_code_map, EXE_LOAD_BASE);
    mono = rt_module_load_pe(MONO_GUEST_PATH, mono_host, MONO_SHA256, mono_code_map);
    (void)rt_module_load_pe_at(SHROUD_GUEST_PATH, shroud_host, SHROUD_SHA256, shroud_code_map, SHROUD_LOAD_BASE);
    for (i = 0; i < aot_image_count; i++)
    {
        bool instances = strcmp(aot_images[i].name, AOT_INSTANCES_IMAGE) == 0;
        bool graphics = strcmp(aot_images[i].name, PORT_GRAPHICS_IMAGE) == 0;
        have_instances |= instances;
        have_graphics |= graphics;
        if ((size_t)snprintf(aot_host, sizeof(aot_host), "%s/%s", aot_dir, aot_images[i].name) >= sizeof(aot_host) ||
            (size_t)snprintf(aot_guest, sizeof(aot_guest), "%s\\%s",
                             instances || graphics ? RT_GUEST_RUNTIME_DIR : MANAGED_GUEST_DIR,
                             aot_images[i].name) >= sizeof(aot_guest))
            rt_fatal("AOT image path too long");
        rt_module_load_pe(aot_guest, aot_host, aot_images[i].sha256, aot_images[i].code_map);
    }
    if (!have_instances)
        rt_fatal("the host has no AOT image " AOT_INSTANCES_IMAGE "; build it with tools/build_aot_instances.sh "
                 "and link it with tools/build_aot_host.sh --player");
    if (!have_graphics)
        rt_fatal("the host has no AOT image " PORT_GRAPHICS_IMAGE "; build it with tools/build_port_graphics.sh "
                 "and link it with tools/build_aot_host.sh --player");
    rt_mono_dll_loaded(mono);
    rt_mono_set_aot_only(mono);
    rt_mono_use_aot_instances(mono, RT_GUEST_RUNTIME_DIR "\\" AOT_INSTANCES_ASSEMBLY);
    rt_mono_use_port_graphics(mono, RT_GUEST_RUNTIME_DIR "\\" PORT_GRAPHICS_ASSEMBLY);
    t = rt_thread_init_main();
    rt_fs_init(game, user, data);
    rt_fs_mount_runtime(aot_dir);
    rt_std_handles_init();
    rt_d3d9_pipeline_cache_init(pcache);
    rt_portgfx_init(gfxcfg);
    capture_setup(work);

    append_arg(cmdline, sizeof(cmdline), &len, EXE_GUEST_PATH);
    for (a = 3; a < argc; a++) append_arg(cmdline, sizeof(cmdline), &len, argv[a]);
    rt_set_command_line(cmdline);

    if (env_switch("RT_MUTE")) rt_audio_set_muted();
    headless = env_switch("RT_HEADLESS");
    if (headless) rt_user32_set_headless();

    /* The window system: the main thread of the host is the main thread of
       the program, and its message functions run the Cocoa event loop. */
    rt_cocoa_init(headless);
    rt_user32_init();
    if (getenv("RT_INPUT") != NULL && getenv("RT_INPUT")[0] != 0) rt_user32_input_script(getenv("RT_INPUT"));

    /* The entry point of the program (the C runtime startup). It ends the
       process with ExitProcess; a return gives the exit code in eax. */
    code = rt_call_std0(t->cpu, rt_module_entry(exe));
    rt_process_exit(code);
}
