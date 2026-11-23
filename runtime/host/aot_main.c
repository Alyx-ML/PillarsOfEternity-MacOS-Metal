/*
 * Command-line host of the translated mono.dll: it runs mono_main of the
 * game's Mono runtime with the arguments of the command line. The main use
 * is the full ahead-of-time (AOT) compilation of the managed assemblies:
 *
 *   mono-aot <game directory> <work directory> --aot=full,asmonly,...
 *            C:\PillarsOfEternity\PillarsOfEternity_data\Managed\mscorlib.dll
 *
 * Guest file system:
 *   C:\PillarsOfEternity   the game directory (the player's files)
 *   C:\Users\Player        <work directory>/user
 *   C:\ProgramData         <work directory>/programdata
 * The host sets MONO_PATH to the Managed directory of the game, so Mono
 * finds mscorlib.dll and the other assemblies there.
 *
 * The host runs headless: MessageBoxA stops the process with its text.
 *
 * Translated AOT images: the option
 *
 *   --aot-image <guest path> <host path>
 *
 * (before the Mono arguments, once for each image) maps the AOT image DLL
 * at <host path> into the guest before Mono starts. Mono loads the AOT
 * image of an assembly as "<assembly path>.dll" with LoadLibrary, so the
 * guest path is that name, for example
 * C:\PillarsOfEternity\PillarsOfEternity_data\Managed\mscorlib.dll.dll.
 * The file name of the guest path selects the translation in the table of
 * linked images (aot_images.h); the DLL must have the SHA-256 of that
 * translation. Mono uses full-AOT images only in AOT-only mode, so the Mono
 * arguments must include --full-aot.
 *
 * Guest environment: the option --guest-env NAME=VALUE (before the Mono
 * arguments, once for each variable) sets a variable of the guest
 * environment, for example MONO_DEBUG=explicit-null-checks for the AOT
 * compiler.
 *
 * Same output on each Mac: the AOT compiler writes the guest addresses of
 * some of its heap blocks into the assembler output (the message strings of
 * marshaling wrappers that throw MarshalDirectiveException; Mono embeds the
 * pointer for CEE_MONO_LDPTR). The converter of the release checks the
 * SHA-256 of each AOT image, so the heap addresses must be the same in each
 * run. The host therefore:
 *   - places all guest allocations without a requested address in the
 *     __GUEST segment (rt_mem_use_guest_space_only). The free gaps of the
 *     guest window are around host mappings, which address space layout
 *     randomization moves in each run;
 *   - gives the guest the fixed computer name HOST_COMPUTER_NAME and
 *     HOST_PROCESSORS processors (rt_set_fixed_host_identity). The guest
 *     environment holds the computer name and the processor count, and its
 *     heap copy has their length.
 *
 * WARNING: the executable contains the translated code of mono.dll. Do not
 * distribute it, upload it or commit it.
 */
#include "aot_images.h"
#include "rt.h"
#include "rt_file.h"
#include "rt_module.h"
#include "rt_thread.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MONO_SHA256 "4e926907c947611b9fd6c88e9d8902d187a9a89c796e935937fd9b303419674d"
#define MONO_GUEST_DIR RT_GUEST_GAME_DIR "\\PillarsOfEternity_data\\Mono"
#define MANAGED_GUEST_DIR RT_GUEST_GAME_DIR "\\PillarsOfEternity_data\\Managed"
#define MONO_CONFIG_GUEST_DIR MONO_GUEST_DIR "\\etc"
/* The program module: the Mono command-line driver of a Windows Mono
   installation is mono.exe next to mono.dll. */
#define EXE_GUEST_PATH MONO_GUEST_DIR "\\mono.exe"

extern const uint8_t mono_code_map[];

/* The fixed host identity of the AOT compiler (see above). */
#define HOST_COMPUTER_NAME "PILLARS-MAC"
#define HOST_PROCESSORS 8u

#define MAX_AOT_IMAGES 64
#define MAX_GUEST_ENV 16

/* Map the AOT image DLL host_path at the guest path guest_path. Mono loads
   it later with LoadLibrary. */
static void load_aot_image(const char *guest_path, const char *host_path)
{
    const char *name = strrchr(guest_path, '\\');
    unsigned i;
    name = name != NULL ? name + 1 : guest_path;
    for (i = 0; i < aot_image_count; i++)
        if (strcasecmp(aot_images[i].name, name) == 0) break;
    if (i == aot_image_count) rt_fatal("no translation of the AOT image %s is linked into this host", name);
    rt_module_load_pe(guest_path, host_path, aot_images[i].sha256, aot_images[i].code_map);
}

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

/* A copy of the UTF-8 string s in guest memory. */
static uint32_t guest_str(const char *s)
{
    size_t n = strlen(s) + 1;
    uint32_t g;
    if (n > 0x100000u) rt_fatal("argument too long");
    g = rt_heap_alloc((uint32_t)n);
    if (g == 0) rt_fatal("no guest memory for the arguments");
    memcpy(G2H(g), s, n);
    return g;
}

static void real_dir(const char *in, char out[PATH_MAX], const char *what)
{
    if (realpath(in, out) == NULL) rt_fatal("the %s %s does not exist", what, in);
}

int main(int argc, char **argv)
{
    char game[PATH_MAX], work[PATH_MAX], user[PATH_MAX + 16], data[PATH_MAX + 16], mono_host[PATH_MAX + 64];
    char cmdline[32768];
    size_t len = 0;
    rt_thread *t;
    uint32_t base, mono_main, gargv, code;
    int i, margc, first, naot = 0, nenv = 0;
    const char *aot_guest[MAX_AOT_IMAGES], *aot_host[MAX_AOT_IMAGES];
    char env_name[MAX_GUEST_ENV][256];
    const char *env_value[MAX_GUEST_ENV];

    /* Host options come before the Mono arguments. */
    first = 3;
    while (first < argc)
    {
        if (strcmp(argv[first], "--aot-image") == 0)
        {
            if (first + 2 >= argc)
            {
                fprintf(stderr, "error: --aot-image needs a guest path and a host path\n");
                return 2;
            }
            if (naot == MAX_AOT_IMAGES)
            {
                fprintf(stderr, "error: more than %d AOT images\n", MAX_AOT_IMAGES);
                return 2;
            }
            aot_guest[naot] = argv[first + 1];
            aot_host[naot] = argv[first + 2];
            naot++;
            first += 3;
        }
        else if (strcmp(argv[first], "--guest-env") == 0)
        {
            const char *eq;
            size_t nlen;
            if (first + 1 >= argc)
            {
                fprintf(stderr, "error: --guest-env needs NAME=VALUE\n");
                return 2;
            }
            eq = strchr(argv[first + 1], '=');
            nlen = eq != NULL ? (size_t)(eq - argv[first + 1]) : 0;
            if (nlen == 0 || nlen >= sizeof(env_name[0]))
            {
                fprintf(stderr, "error: --guest-env %s is not NAME=VALUE\n", argv[first + 1]);
                return 2;
            }
            if (nenv == MAX_GUEST_ENV)
            {
                fprintf(stderr, "error: more than %d guest environment variables\n", MAX_GUEST_ENV);
                return 2;
            }
            memcpy(env_name[nenv], argv[first + 1], nlen);
            env_name[nenv][nlen] = '\0';
            env_value[nenv] = eq + 1;
            nenv++;
            first += 2;
        }
        else
            break;
    }
    if (argc < 3 || first >= argc)
    {
        fprintf(stderr,
                "usage: %s <game directory> <work directory> [--aot-image <guest path> <host path>]... "
                "[--guest-env NAME=VALUE]... <mono argument>...\n"
                "example: %s ~/Games/PillarsOfEternity work/aot --aot=full,asmonly,asmwriter,nodebug,outfile=C:\\\\Users\\\\Player\\\\mscorlib.dll.dll.s "
                MANAGED_GUEST_DIR "\\\\mscorlib.dll\n",
                argv[0], argv[0]);
        return 2;
    }
    real_dir(argv[1], game, "game directory");
    real_dir(argv[2], work, "work directory");
    if ((size_t)snprintf(user, sizeof(user), "%s/user", work) >= sizeof(user) ||
        (size_t)snprintf(data, sizeof(data), "%s/programdata", work) >= sizeof(data) ||
        (size_t)snprintf(mono_host, sizeof(mono_host), "%s/PillarsOfEternity_data/Mono/mono.dll", game) >= sizeof(mono_host))
        rt_fatal("host path too long");

    rt_user32_set_headless();
    rt_set_fixed_host_identity(HOST_COMPUTER_NAME, HOST_PROCESSORS);
    rt_mem_init();
    rt_mem_use_guest_space_only();
    if (ptr_initialize_pointers == NULL) rt_fatal("the translated modules have no pointer initialization");
    ptr_initialize_pointers(rt_ptrofs);
    rt_modules_init(EXE_GUEST_PATH);
    /* The translated images must be at their original bases. Map them
       before the main thread and the heap allocate guest memory: the
       allocator puts those blocks in the lowest free ranges above the
       program image, and with a large program image they reach the image
       bases. */
    base = rt_module_load_pe(MONO_GUEST_DIR "\\mono.dll", mono_host, MONO_SHA256, mono_code_map);
    for (i = 0; i < naot; i++) load_aot_image(aot_guest[i], aot_host[i]);
    t = rt_thread_init_main();
    rt_fs_init(game, user, data);
    rt_std_handles_init();

    /* argv[0] of mono_main is the program; the other arguments come from
       the host command line. */
    append_arg(cmdline, sizeof(cmdline), &len, EXE_GUEST_PATH);
    for (i = first; i < argc; i++) append_arg(cmdline, sizeof(cmdline), &len, argv[i]);
    rt_set_command_line(cmdline);
    rt_env_set("MONO_PATH", MANAGED_GUEST_DIR);
    for (i = 0; i < nenv; i++) rt_env_set(env_name[i], env_value[i]);

    rt_mono_dll_loaded(base);
    rt_module_attach_static(t->cpu, base);

    /* void mono_set_dirs(const char *assembly_dir, const char *config_dir) */
    rt_call(t->cpu, rt_module_export(base, "mono_set_dirs"), false, 0, 2,
            (const uint32_t[]){ guest_str(MANAGED_GUEST_DIR), guest_str(MONO_CONFIG_GUEST_DIR) });

    /* int mono_main(int argc, char *argv[]): the strings are UTF-8, the
       convention of the Mono API. */
    margc = argc - first + 1;
    gargv = rt_heap_calloc((uint32_t)margc + 1u, 4);
    if (gargv == 0) rt_fatal("no guest memory for the arguments");
    wr32(gargv, guest_str(EXE_GUEST_PATH));
    for (i = first; i < argc; i++) wr32(gargv + 4u * (uint32_t)(i - first + 1), guest_str(argv[i]));
    mono_main = rt_module_export(base, "mono_main");
    code = rt_call(t->cpu, mono_main, false, 0, 2, (const uint32_t[]){ (uint32_t)margc, gargv });
    rt_log("mono_main returned %d", (int)code);
    rt_process_exit(code);
}
