/*
 * The converter (see rt_convert.h).
 *
 * Layout of the Recompiler directory (made by tools/package_release.sh):
 *   bin/SRW, bin/llasm, bin/llvm-split, bin/llvm-link, bin/clang,
 *   bin/lld
 *   lib/                      the LLVM, lld, z3 and zstd libraries,
 *                             libpillars_rt.a (the runtime objects) and
 *                             libclang_rt.osx.a
 *   lib/objects/              guest_space.o (the __GUEST segment),
 *                             aot_main.o and aot_images_aot.o (the AOT
 *                             compiler host), player_main.o,
 *                             aot_images_player.o and player_info.plist
 *                             (the game program)
 *   include/llasm-support     the .llinc files of the SR llasm support and
 *                             sse_inline.bc (the SSE helpers as bitcode)
 *   include/runtime           the .llinc files of runtime/llasm
 *   modules/<module>          the analysis files of each module (SR.cfg,
 *                             *.sci, extern.llinc)
 *   managed/                  PortAotInstances.dll and PortGraphics.dll,
 *                             the assemblies of the port
 *   manifest.txt              the lines
 *     content <sha256>        a value of all other files of the directory
 *                             (the loader uses the manifest in its cache key)
 *     input <path> <sha256>   a game file (path relative to the game folder)
 *     pe <module> <path> <rebase> <sha256> <parts> <all|codemap>
 *                             a PE module of the game: the game file, the
 *                             new image base (hex, 0 = the original base),
 *                             the SHA-256 of the SRW input (after the
 *                             rebase), the llvm-split parts, and the
 *                             symbols that the combined object keeps
 *     aot <module> <assembly> <base> <managed|port> <options|-> <parts>
 *                             a managed assembly (the game's Managed folder
 *                             or managed/), the image base, the extra
 *                             options of the AOT compiler
 *     image <module> <sha256> the SHA-256 of the tested AOT image DLL
 *     pin <module> <file> <sha256>
 *                             an SRW output file of the tested translation
 *
 * The commands are those of the development build: tools/translate_module.sh
 * (the last SRW pass and llasm), tools/compile_module.sh,
 * tools/build_aot_image.sh and tools/build_aot_host.sh.
 *
 * The work is a set of units:
 *   FRONT     one module: SRW, the pin check, llasm, llvm-split
 *   PART      one part: llvm-link with the SSE helpers, clang
 *   COMBINE   one module: ld -r of its parts
 *   AOT_HOST  the link of the AOT compiler host (after COMBINE of mono)
 *   AOT       one assembly: the AOT compiler, the image build, the check
 *             of the image (after AOT_HOST); then FRONT of the image
 *   PLAYER    the link of the game program (after COMBINE of all modules)
 * Worker threads (one tool process each) take the units from two queues;
 * the queue of the units other than PART comes first. A failed unit stops
 * all tool processes; the first error is the result.
 *
 * The tools run with a minimal environment. The converter reads the output
 * of each tool into memory (it writes no log files): a failed tool stops
 * the conversion with the end of its output in the error message. Intermediate files are removed when the next step no
 * longer needs them.
 */
#include "rt_convert.h"
#include "rt_aot_asm.h"

#include <CommonCrypto/CommonDigest.h>
#include <copyfile.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <removefile.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define PATH_LEN 1024
#define ERR_LEN 4096
#define MAX_JOBS 16u
#define MAX_INPUTS 64u
#define MAX_MODULES 48u
#define MAX_PINS 256u
#define MAX_PARTS 64u
#define MAX_MANIFEST (256u << 10)
#define LOG_TAIL 1500
#define TRIPLE "arm64-apple-macosx14.0.0"
#define MIN_MACOS "14.0.0"
#define DATALAYOUT "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
/* The work files. In the release test on 2026-11-23 the work directory
   had at most 4.1 GB (measured each 15 seconds); the limit is about twice
   that value. */
#define MIN_FREE_BYTES (UINT64_C(8) << 30)
/* Memory for one tool process (UNCONFIRMED: the largest clang part was not
   measured). */
#define BYTES_PER_JOB (UINT64_C(2) << 30)
#define GAME_EXE "PillarsOfEternity"
#define GUEST_MANAGED "C:\\PillarsOfEternity\\PillarsOfEternity_data\\Managed\\"
#define GUEST_USER_AOT "C:\\Users\\Player\\aot\\"
#define PORT_ASSEMBLY_1 "PortAotInstances.dll"
#define PORT_ASSEMBLY_2 "PortGraphics.dll"

/* ---------------------------------------------------------------- manifest */

typedef struct input_file {
    char path[256];
    char sha256[65];
} input_file;

typedef enum { MOD_PE, MOD_AOT } module_kind;

typedef struct module {
    char name[64];
    module_kind kind;
    unsigned parts;
    bool all_symbols;      /* COMBINE keeps all symbols (mono) */
    /* MOD_PE */
    char game_path[256];   /* relative to the game folder */
    uint32_t rebase;       /* 0: the original base */
    char input_sha[65];    /* the SRW input */
    /* MOD_AOT */
    char assembly[128];    /* without ".dll" */
    char base[16];         /* "0x..." as in the manifest */
    bool port;             /* from managed/ */
    char options[256];     /* extra AOT compiler options, "" for none */
    char image_sha[65];
    /* both */
    char srw_input[192];   /* the file name in the module directory */
    char dir[PATH_LEN];
    char object[PATH_LEN]; /* the combined object */
    unsigned parts_done;
} module;

typedef struct pin {
    char module[64];
    char file[128];
    char sha256[65];
} pin;

typedef struct manifest {
    input_file inputs[MAX_INPUTS];
    unsigned ninputs;
    module modules[MAX_MODULES];
    unsigned nmodules;
    pin pins[MAX_PINS];
    unsigned npins;
    bool have_content;
} manifest;

/* The options of the clang driver for "-target arm64-apple-macosx14.0.0
   -O2 -fPIC -c -x ir" (tools/compile_module.sh), without the options that
   depend on the build computer (SDK, host CPU tuning, directories). The
   driver is not used, because it runs xcrun to find an SDK. */
static const char *const cc1_args[] = {
    "-cc1", "-triple", TRIPLE, "-O2", "-emit-obj", "-disable-free", "-disable-llvm-verifier", "-discard-value-names",
    "-mrelocation-model", "pic", "-pic-level", "2", "-mframe-pointer=non-leaf-no-reserve", "-ffp-contract=on",
    "-fno-rounding-math", "-funwind-tables=1", "-target-cpu", "apple-m1", "-target-feature", "+v8.4a",
    "-target-feature", "+aes", "-target-feature", "+altnzcv", "-target-feature", "+ccdp", "-target-feature", "+ccpp",
    "-target-feature", "+complxnum", "-target-feature", "+crc", "-target-feature", "+dotprod", "-target-feature",
    "+flagm", "-target-feature", "+fp-armv8", "-target-feature", "+fp16fml", "-target-feature", "+fptoint",
    "-target-feature", "+fullfp16", "-target-feature", "+jsconv", "-target-feature", "+lse", "-target-feature",
    "+neon", "-target-feature", "+pauth", "-target-feature", "+perfmon", "-target-feature", "+predres",
    "-target-feature", "+ras", "-target-feature", "+rcpc", "-target-feature", "+rdm", "-target-feature", "+sb",
    "-target-feature", "+sha2", "-target-feature", "+sha3", "-target-feature", "+specrestrict", "-target-feature",
    "+ssbs", "-target-abi", "darwinpcs", "-stack-protector", "1", "-fmax-type-align=16", "-vectorize-loops",
    "-vectorize-slp",
};
#define NCC1 (sizeof cc1_args / sizeof cc1_args[0])

/* The frameworks and libraries of tools/build_aot_host.sh. */
static const char *const link_libs[] = {
    "-framework", "CoreFoundation", "-framework", "CoreServices", "-framework", "Security", "-framework", "CFNetwork",
    "-framework", "IOKit", "-framework", "AppKit", "-framework", "Carbon", "-framework", "Metal",
    "-framework", "QuartzCore", "-framework", "CoreGraphics", "-framework", "CoreText", "-framework", "ImageIO",
    "-framework", "AudioToolbox", "-framework", "MetalFX", "-framework", "CoreAudio", "-framework", "GameController",
    "-lresolv", "-lSystem",
};
#define NLINK_LIBS (sizeof link_libs / sizeof link_libs[0])

/* ---------------------------------------------------------------- errors */

/* The state of one worker thread. */
typedef struct wk {
    const rt_convert_job *job;
    const manifest *mf;
    char err[ERR_LEN];
} wk;

static bool fail(wk *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static bool fail(wk *w, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(w->err, sizeof w->err, fmt, ap);
    va_end(ap);
    return false;
}

static bool join(char *out, size_t n, const char *dir, const char *name)
{
    int r = snprintf(out, n, "%s/%s", dir, name);
    return r > 0 && (size_t)r < n;
}

static bool fmt_path(char *out, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static bool fmt_path(char *out, size_t n, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vsnprintf(out, n, fmt, ap);
    va_end(ap);
    return r > 0 && (size_t)r < n;
}

bool rt_sha256_file(const char *path, char out[65], char *err, size_t errn)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_CTX ctx;
    unsigned char *buf;
    size_t n;
    int i;
    FILE *f = fopen(path, "rb");
    if (f == NULL)
    {
        snprintf(err, errn, "cannot open %s: %s", path, strerror(errno));
        return false;
    }
    buf = malloc(1u << 20);
    if (buf == NULL)
    {
        fclose(f);
        snprintf(err, errn, "out of memory");
        return false;
    }
    CC_SHA256_Init(&ctx);
    while ((n = fread(buf, 1, 1u << 20, f)) > 0) CC_SHA256_Update(&ctx, buf, (CC_LONG)n);
    i = ferror(f);
    fclose(f);
    free(buf);
    if (i != 0)
    {
        snprintf(err, errn, "read of %s failed", path);
        return false;
    }
    CC_SHA256_Final(digest, &ctx);
    for (i = 0; i < CC_SHA256_DIGEST_LENGTH; i++)
    {
        out[2 * i] = hex[digest[i] >> 4];
        out[2 * i + 1] = hex[digest[i] & 15];
    }
    out[64] = 0;
    return true;
}

/* ---------------------------------------------------------------- child processes */

static pthread_mutex_t child_lock = PTHREAD_MUTEX_INITIALIZER;
static pid_t children[MAX_JOBS + 1];
static unsigned nchildren;
static atomic_bool cancelled;

static void register_child(pid_t pid)
{
    pthread_mutex_lock(&child_lock);
    if (nchildren < MAX_JOBS + 1) children[nchildren++] = pid;
    pthread_mutex_unlock(&child_lock);
    /* A cancel between posix_spawn and the registration did not see this
       child. */
    if (atomic_load(&cancelled)) kill(pid, SIGKILL);
}

static void unregister_child(pid_t pid)
{
    unsigned i;
    pthread_mutex_lock(&child_lock);
    for (i = 0; i < nchildren; i++)
    {
        if (children[i] != pid) continue;
        children[i] = children[--nchildren];
        break;
    }
    pthread_mutex_unlock(&child_lock);
}

void rt_convert_cancel(void)
{
    unsigned i;
    atomic_store(&cancelled, true);
    pthread_mutex_lock(&child_lock);
    for (i = 0; i < nchildren; i++) kill(children[i], SIGKILL);
    pthread_mutex_unlock(&child_lock);
}

/* Run tool with argv (argv[0] is set to the tool path) in cwd and wait for
   it. The standard output and the standard error go to a pipe; the last
   LOG_TAIL bytes stay in memory for the error message. A tool that fails
   (or, with quiet, writes any output) is an error. */
static bool run_tool(wk *w, const char *what, const char *tool, const char **argv, const char *cwd, bool quiet)
{
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    char env_home[PATH_LEN + 8], env_tmp[PATH_LEN + 8], tail[LOG_TAIL + 1], chunk[4096];
    const char *home = getenv("HOME");
    char *envp[5];
    int n = 0, r, status = 0, fd[2];
    size_t kept = 0, total = 0;
    ssize_t got_bytes;
    pid_t pid, got;

    if (atomic_load(&cancelled)) return fail(w, "the conversion was cancelled");
    envp[n++] = (char *)"PATH=/usr/bin:/bin";
    envp[n++] = (char *)"LC_ALL=C";
    if (home != NULL && (size_t)snprintf(env_home, sizeof env_home, "HOME=%s", home) < sizeof env_home)
        envp[n++] = env_home;
    if ((size_t)snprintf(env_tmp, sizeof env_tmp, "TMPDIR=%s", w->job->work_dir) >= sizeof env_tmp)
        return fail(w, "the work directory path is too long");
    envp[n++] = env_tmp;
    envp[n] = NULL;
    argv[0] = tool;

    if (pipe(fd) != 0) return fail(w, "pipe failed: %s", strerror(errno));
    if (posix_spawn_file_actions_init(&fa) != 0)
    {
        close(fd[0]);
        close(fd[1]);
        return fail(w, "posix_spawn_file_actions_init failed");
    }
    if (posix_spawnattr_init(&attr) != 0)
    {
        posix_spawn_file_actions_destroy(&fa);
        close(fd[0]);
        close(fd[1]);
        return fail(w, "posix_spawnattr_init failed");
    }
    r = posix_spawn_file_actions_addchdir_np(&fa, cwd);
    if (r == 0) r = posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (r == 0) r = posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
    if (r == 0) r = posix_spawn_file_actions_adddup2(&fa, fd[1], 2);
    /* Only the descriptors 0, 1 and 2 go to the tool. */
    if (r == 0) r = posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
    if (r == 0) r = posix_spawn(&pid, tool, &fa, &attr, (char *const *)argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(fd[1]);
    if (r != 0)
    {
        close(fd[0]);
        return fail(w, "cannot start %s: %s", tool, strerror(r));
    }
    register_child(pid);
    /* Read until the tool closes its output; keep the last LOG_TAIL bytes. */
    for (;;)
    {
        got_bytes = read(fd[0], chunk, sizeof chunk);
        if (got_bytes < 0 && errno == EINTR) continue;
        if (got_bytes <= 0) break;
        total += (size_t)got_bytes;
        if ((size_t)got_bytes >= LOG_TAIL)
        {
            memcpy(tail, chunk + (size_t)got_bytes - LOG_TAIL, LOG_TAIL);
            kept = LOG_TAIL;
        }
        else
        {
            size_t add = (size_t)got_bytes, drop = kept + add > LOG_TAIL ? kept + add - LOG_TAIL : 0;
            memmove(tail, tail + drop, kept - drop);
            kept -= drop;
            memcpy(tail + kept, chunk, add);
            kept += add;
        }
    }
    r = got_bytes < 0 ? errno : 0;
    close(fd[0]);
    tail[kept] = 0;
    do got = waitpid(pid, &status, 0);
    while (got < 0 && errno == EINTR);
    unregister_child(pid);
    if (got != pid) return fail(w, "waitpid for %s failed: %s", what, strerror(errno));
    if (atomic_load(&cancelled)) return fail(w, "the conversion was cancelled");
    if (r != 0) return fail(w, "the output of %s cannot be read: %s", what, strerror(r));
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        if (WIFSIGNALED(status)) return fail(w, "%s stopped with signal %d. The end of its output:\n%s", what, WTERMSIG(status), tail);
        return fail(w, "%s failed with status %d. The end of its output:\n%s", what, WEXITSTATUS(status), tail);
    }
    if (quiet && total > 0) return fail(w, "%s wrote messages (the tested build had none). The end of its output:\n%s", what, tail);
    return true;
}

/* Run a system tool and read its standard output (one line, without the
   line end) into out. */
static bool run_capture(const char *tool, const char *const *args, char *out, size_t n, char *err, size_t errn)
{
    posix_spawn_file_actions_t fa;
    const char *argv[8];
    char *envp[3], env_home[PATH_LEN + 8];
    const char *home = getenv("HOME");
    int fd[2], r, status = 0, k = 0, i;
    size_t len = 0;
    ssize_t got;
    pid_t pid;

    argv[0] = tool;
    for (i = 0; args[i] != NULL && i < 6; i++) argv[i + 1] = args[i];
    argv[i + 1] = NULL;
    envp[k++] = (char *)"PATH=/usr/bin:/bin";
    if (home != NULL && (size_t)snprintf(env_home, sizeof env_home, "HOME=%s", home) < sizeof env_home)
        envp[k++] = env_home;
    envp[k] = NULL;
    if (pipe(fd) != 0)
    {
        snprintf(err, errn, "pipe failed: %s", strerror(errno));
        return false;
    }
    if (posix_spawn_file_actions_init(&fa) != 0)
    {
        close(fd[0]);
        close(fd[1]);
        snprintf(err, errn, "posix_spawn_file_actions_init failed");
        return false;
    }
    r = posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (r == 0) r = posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
    if (r == 0) r = posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    if (r == 0) r = posix_spawn_file_actions_addclose(&fa, fd[0]);
    if (r == 0) r = posix_spawn(&pid, tool, &fa, NULL, (char *const *)argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    close(fd[1]);
    if (r != 0)
    {
        close(fd[0]);
        snprintf(err, errn, "cannot start %s: %s", tool, strerror(r));
        return false;
    }
    while (len + 1 < n && (got = read(fd[0], out + len, n - 1 - len)) != 0)
    {
        if (got < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        len += (size_t)got;
    }
    close(fd[0]);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    out[len] = 0;
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) out[--len] = 0;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || len == 0 || len + 1 >= n || strchr(out, '\n') != NULL)
    {
        snprintf(err, errn, "%s %s did not give a result (status %d)", tool, args[0], status);
        return false;
    }
    return true;
}

bool rt_convert_have_developer_dir(void)
{
    static const char *const args[] = { "-p", NULL };
    char dir[PATH_LEN], err[256];
    struct stat st;
    return run_capture("/usr/bin/xcode-select", args, dir, sizeof dir, err, sizeof err) && stat(dir, &st) == 0 &&
           S_ISDIR(st.st_mode);
}

bool rt_convert_find_tools(rt_convert_tools *t, char *err, size_t errn)
{
    static const char *const find_ld[] = { "--find", "ld", NULL };
    static const char *const sdk_path[] = { "--sdk", "macosx", "--show-sdk-path", NULL };
    static const char *const sdk_ver[] = { "--sdk", "macosx", "--show-sdk-version", NULL };
    struct stat st;
    if (!run_capture("/usr/bin/xcrun", find_ld, t->ld, sizeof t->ld, err, errn) ||
        !run_capture("/usr/bin/xcrun", sdk_path, t->sdk, sizeof t->sdk, err, errn) ||
        !run_capture("/usr/bin/xcrun", sdk_ver, t->sdk_version, sizeof t->sdk_version, err, errn))
        return false;
    if (stat(t->ld, &st) != 0 || !S_ISREG(st.st_mode))
    {
        snprintf(err, errn, "the linker %s does not exist", t->ld);
        return false;
    }
    if (stat(t->sdk, &st) != 0 || !S_ISDIR(st.st_mode))
    {
        snprintf(err, errn, "the SDK %s does not exist", t->sdk);
        return false;
    }
    if (strspn(t->sdk_version, "0123456789.") != strlen(t->sdk_version))
    {
        snprintf(err, errn, "the SDK version \"%s\" is not a version number", t->sdk_version);
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------- files */

static bool is_sha(const char *s)
{
    return strlen(s) == 64 && strspn(s, "0123456789abcdef") == 64;
}

static bool is_name(const char *s)
{
    return s[0] != 0 && s[0] != '.' && strchr(s, '/') == NULL && strchr(s, '\\') == NULL;
}

static module *find_module(manifest *mf, const char *name)
{
    unsigned i;
    for (i = 0; i < mf->nmodules; i++)
        if (strcmp(mf->modules[i].name, name) == 0) return &mf->modules[i];
    return NULL;
}

static bool load_manifest(wk *w, manifest *mf)
{
    char path[PATH_LEN], line[1024];
    FILE *f;
    unsigned lineno = 0, i;
    long size;
    memset(mf, 0, sizeof *mf);
    if (!join(path, sizeof path, w->job->recompiler, "manifest.txt")) return fail(w, "the converter path is too long");
    f = fopen(path, "r");
    if (f == NULL) return fail(w, "cannot open %s: %s", path, strerror(errno));
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || size > (long)MAX_MANIFEST || fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        return fail(w, "%s is damaged", path);
    }
    while (fgets(line, sizeof line, f) != NULL)
    {
        char a[16], b[256], c[256], d[256], e[256], g[256], h[16], extra[2];
        unsigned parts;
        int k;
        bool ok = false;
        lineno++;
        if (strchr(line, '\n') == NULL && !feof(f)) break;
        if (line[0] == '#' || line[0] == '\n') continue;
        k = sscanf(line, "%15s %255s %255s %255s %255s %255s %15s %1s", a, b, c, d, e, g, h, extra);
        if (k == 2 && strcmp(a, "content") == 0 && is_sha(b))
        {
            mf->have_content = true;
            ok = true;
        }
        else if (k == 3 && strcmp(a, "input") == 0 && is_sha(c) && strlen(b) < sizeof mf->inputs[0].path &&
                 b[0] != '/' && strstr(b, "..") == NULL && mf->ninputs < MAX_INPUTS)
        {
            input_file *in = &mf->inputs[mf->ninputs++];
            memcpy(in->path, b, strlen(b) + 1);
            memcpy(in->sha256, c, 65);
            ok = true;
        }
        else if (k == 7 && strcmp(a, "pe") == 0 && is_name(b) && strlen(b) < 64 && find_module(mf, b) == NULL &&
                 strlen(c) < 256 && c[0] != '/' && strstr(c, "..") == NULL && is_sha(e) && sscanf(g, "%u", &parts) == 1 &&
                 parts >= 1 && parts <= MAX_PARTS && (strcmp(h, "all") == 0 || strcmp(h, "codemap") == 0) &&
                 mf->nmodules < MAX_MODULES)
        {
            module *m = &mf->modules[mf->nmodules++];
            const char *base = strrchr(c, '/');
            char *endp;
            unsigned long rb = strtoul(d, &endp, 16);
            if (*endp != 0 || rb > 0x7FFF0000ul || (rb & 0xFFFFul) != 0) break;
            m->kind = MOD_PE;
            memcpy(m->name, b, strlen(b) + 1);
            memcpy(m->game_path, c, strlen(c) + 1);
            m->rebase = (uint32_t)rb;
            memcpy(m->input_sha, e, 65);
            m->parts = parts;
            m->all_symbols = strcmp(h, "all") == 0;
            snprintf(m->srw_input, sizeof m->srw_input, "%s", base != NULL ? base + 1 : c);
            ok = is_name(m->srw_input);
        }
        else if (k == 7 && strcmp(a, "aot") == 0 && is_name(b) && strlen(b) < 64 && find_module(mf, b) == NULL &&
                 is_name(c) && strlen(c) < 120 && strncmp(d, "0x", 2) == 0 && strlen(d) == 10 &&
                 strspn(d + 2, "0123456789ABCDEFabcdef") == 8 && strcmp(d + 6, "0000") == 0 &&
                 (strcmp(e, "managed") == 0 || strcmp(e, "port") == 0) && sscanf(h, "%u", &parts) == 1 && parts >= 1 &&
                 parts <= MAX_PARTS && mf->nmodules < MAX_MODULES)
        {
            module *m = &mf->modules[mf->nmodules++];
            m->kind = MOD_AOT;
            memcpy(m->name, b, strlen(b) + 1);
            memcpy(m->assembly, c, strlen(c) + 1);
            memcpy(m->base, d, strlen(d) + 1);
            m->port = strcmp(e, "port") == 0;
            if (strcmp(g, "-") != 0) snprintf(m->options, sizeof m->options, "%s", g);
            ok = strspn(m->options, "abcdefghijklmnopqrstuvwxyz0123456789=,-") == strlen(m->options);
            m->parts = parts;
            snprintf(m->srw_input, sizeof m->srw_input, "%s.dll.dll", c);
        }
        else if (k == 3 && strcmp(a, "image") == 0 && is_sha(c))
        {
            module *m = find_module(mf, b);
            if (m != NULL && m->kind == MOD_AOT && m->image_sha[0] == 0)
            {
                memcpy(m->image_sha, c, 65);
                ok = true;
            }
        }
        else if (k == 4 && strcmp(a, "pin") == 0 && is_sha(d) && is_name(c) && strlen(c) < 128 &&
                 find_module(mf, b) != NULL && mf->npins < MAX_PINS)
        {
            pin *p = &mf->pins[mf->npins++];
            memcpy(p->module, b, strlen(b) + 1);
            memcpy(p->file, c, strlen(c) + 1);
            memcpy(p->sha256, d, 65);
            ok = true;
        }
        if (!ok)
        {
            fclose(f);
            return fail(w, "%s line %u is damaged", path, lineno);
        }
    }
    if (ferror(f) || !feof(f))
    {
        fclose(f);
        return fail(w, "%s is damaged (line %u)", path, lineno + 1);
    }
    fclose(f);
    if (!mf->have_content) return fail(w, "%s has no content line", path);
    if (find_module(mf, "mono") == NULL || find_module(mf, "player") == NULL)
        return fail(w, "%s has no module mono or player", path);
    for (i = 0; i < mf->nmodules; i++)
    {
        module *m = &mf->modules[i];
        unsigned k, npins = 0;
        if (m->kind == MOD_AOT && m->image_sha[0] == 0) return fail(w, "%s has no image value for %s", path, m->name);
        for (k = 0; k < mf->npins; k++) npins += strcmp(mf->pins[k].module, m->name) == 0;
        if (npins == 0) return fail(w, "%s has no SRW output values for the module %s", path, m->name);
    }
    return true;
}

static bool copy_file(wk *w, const char *src, const char *dst)
{
    if (copyfile(src, dst, NULL, COPYFILE_DATA | COPYFILE_EXCL) != 0)
        return fail(w, "cannot copy %s to %s: %s", src, dst, strerror(errno));
    return true;
}

/* Copy each regular file of src_dir into dst_dir. */
static bool copy_dir_files(wk *w, const char *src_dir, const char *dst_dir, unsigned *count)
{
    DIR *d = opendir(src_dir);
    struct dirent *e;
    *count = 0;
    if (d == NULL) return fail(w, "cannot open %s: %s", src_dir, strerror(errno));
    while ((e = readdir(d)) != NULL)
    {
        char src[PATH_LEN], dst[PATH_LEN];
        struct stat st;
        if (e->d_name[0] == '.') continue;
        if (!join(src, sizeof src, src_dir, e->d_name) || !join(dst, sizeof dst, dst_dir, e->d_name))
        {
            closedir(d);
            return fail(w, "a path is too long");
        }
        if (lstat(src, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (!copy_file(w, src, dst))
        {
            closedir(d);
            return false;
        }
        (*count)++;
    }
    closedir(d);
    return true;
}

static bool remove_file(wk *w, const char *path)
{
    if (unlink(path) != 0) return fail(w, "cannot remove %s: %s", path, strerror(errno));
    return true;
}

/* Append text to the file at path. */
static bool append_text(wk *w, const char *path, const char *text)
{
    size_t len = strlen(text), done = 0;
    int fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd < 0) return fail(w, "%s cannot be opened: %s", path, strerror(errno));
    while (done < len)
    {
        ssize_t n = write(fd, text + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0)
        {
            int e = errno;
            (void)close(fd);
            return fail(w, "%s cannot be written: %s", path, n < 0 ? strerror(e) : "no progress");
        }
        done += (size_t)n;
    }
    if (close(fd) != 0) return fail(w, "%s cannot be closed: %s", path, strerror(errno));
    return true;
}

static bool remove_in(wk *w, const char *dir, const char *name)
{
    char p[PATH_LEN];
    if (!join(p, sizeof p, dir, name)) return fail(w, "a path is too long");
    return remove_file(w, p);
}

static bool remove_tree(wk *w, const char *path)
{
    if (removefile(path, NULL, REMOVEFILE_RECURSIVE) != 0) return fail(w, "cannot remove %s: %s", path, strerror(errno));
    return true;
}

static bool make_dir(wk *w, const char *path)
{
    if (mkdir(path, 0755) != 0) return fail(w, "cannot make %s: %s", path, strerror(errno));
    return true;
}

static bool check_sha(wk *w, const char *path, const char *want, const char *what)
{
    char sum[65], err[PATH_LEN + 64];
    if (!rt_sha256_file(path, sum, err, sizeof err)) return fail(w, "%s", err);
    if (strcmp(sum, want) != 0)
        return fail(w, "%s is not the tested %s: %s has SHA-256 %s, expected %s", path, what, path, sum, want);
    return true;
}

static unsigned job_count(void)
{
    int ncpu = 0;
    uint64_t mem = 0;
    size_t len = sizeof ncpu;
    unsigned jobs;
    if (sysctlbyname("hw.activecpu", &ncpu, &len, NULL, 0) != 0 || ncpu < 1) ncpu = 1;
    len = sizeof mem;
    if (sysctlbyname("hw.memsize", &mem, &len, NULL, 0) != 0) mem = 0;
    jobs = (unsigned)ncpu;
    if (mem / BYTES_PER_JOB < jobs) jobs = (unsigned)(mem / BYTES_PER_JOB);
    if (jobs > MAX_JOBS) jobs = MAX_JOBS;
    return jobs < 1 ? 1 : jobs;
}

/* ---------------------------------------------------------------- PE rebase */

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

typedef struct pe_section {
    uint32_t va, span, rsize, rptr;
} pe_section;

/* The file offset of [rva, rva + len), or SIZE_MAX. */
static size_t pe_offset(const pe_section *s, unsigned n, size_t file_size, uint32_t rva, uint32_t len)
{
    unsigned i;
    for (i = 0; i < n; i++)
    {
        uint64_t end = (uint64_t)rva + len;
        if (s[i].va <= rva && end <= (uint64_t)s[i].va + s[i].span)
        {
            uint64_t off;
            if (end > (uint64_t)s[i].va + s[i].rsize) return SIZE_MAX;
            off = (uint64_t)s[i].rptr + (rva - s[i].va);
            if (off + len > file_size) return SIZE_MAX;
            return (size_t)off;
        }
    }
    return SIZE_MAX;
}

/* Write a copy of the PE32 module src that is rebased to new_base, as
   tools/rebase_pe.py: add (new base - old base) to each 32-bit word that a
   base relocation of type HIGHLOW (3) points to, and write the new base in
   the optional header. Type ABSOLUTE (0) is padding; any other type, two
   relocations of one word or a table out of range is an error. */
static bool rebase_pe(wk *w, const char *src, const char *dst, uint32_t new_base)
{
    FILE *f;
    uint8_t *d = NULL, *seen = NULL;
    long size;
    size_t n, pe, opt, pos, end;
    pe_section sec[96];
    unsigned nsect, i;
    uint32_t old_base, image_size, ndirs, reloc_rva, reloc_size, delta;
    uint16_t opt_size;
    bool ok = false;

    f = fopen(src, "rb");
    if (f == NULL) return fail(w, "cannot open %s: %s", src, strerror(errno));
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 64 || size > (256L << 20) || fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        return fail(w, "%s is not a PE module of the expected size", src);
    }
    n = (size_t)size;
    d = malloc(n);
    if (d == NULL || fread(d, 1, n, f) != n)
    {
        fclose(f);
        free(d);
        return fail(w, "cannot read %s", src);
    }
    fclose(f);
#define BAD(...)                         \
    do                                   \
    {                                    \
        fail(w, __VA_ARGS__);            \
        goto out;                        \
    } while (0)
    if (d[0] != 'M' || d[1] != 'Z') BAD("%s has no MZ header", src);
    pe = get32(d + 0x3C);
    if (pe > n - 24 || memcmp(d + pe, "PE\0\0", 4) != 0) BAD("%s has no PE header", src);
    nsect = get16(d + pe + 6);
    opt_size = get16(d + pe + 20);
    opt = pe + 24;
    if (opt + 96 > n || get16(d + opt) != 0x10B) BAD("%s is not a PE32 module", src);
    old_base = get32(d + opt + 28);
    image_size = get32(d + opt + 56);
    ndirs = get32(d + opt + 92);
    if ((new_base & 0xFFFFu) != 0 || new_base == 0 || (uint64_t)new_base + image_size > 0x80000000u)
        BAD("the image of %s does not fit at 0x%08x", src, new_base);
    if (ndirs <= 5 || opt + 96 + 8 * 6 > n) BAD("%s has no base relocation directory", src);
    reloc_rva = get32(d + opt + 96 + 8 * 5);
    reloc_size = get32(d + opt + 96 + 8 * 5 + 4);
    if (reloc_rva == 0 || reloc_size == 0) BAD("%s has no base relocations", src);
    if (nsect > sizeof sec / sizeof sec[0] || opt + opt_size + 40u * nsect > n) BAD("%s has a damaged section table", src);
    for (i = 0; i < nsect; i++)
    {
        const uint8_t *h = d + opt + opt_size + 40u * i;
        uint32_t vsize = get32(h + 8), rsize = get32(h + 16);
        sec[i].va = get32(h + 12);
        sec[i].rsize = rsize;
        sec[i].rptr = get32(h + 20);
        sec[i].span = vsize > rsize ? vsize : rsize;
    }
    seen = calloc((size_t)image_size / 8 + 1, 1);
    if (seen == NULL) BAD("out of memory");
    delta = new_base - old_base;
    pos = pe_offset(sec, nsect, n, reloc_rva, reloc_size);
    if (pos == SIZE_MAX) BAD("%s: the relocation table is not in the file", src);
    end = pos + reloc_size;
    while (pos < end)
    {
        uint32_t page, block, k;
        if (end - pos < 8) BAD("%s: truncated relocation block at file offset 0x%zx", src, pos);
        page = get32(d + pos);
        block = get32(d + pos + 4);
        if (block < 8 || (block & 1) != 0 || block > end - pos) BAD("%s: bad relocation block at file offset 0x%zx", src, pos);
        for (k = 0; k < (block - 8) / 2; k++)
        {
            uint16_t entry = get16(d + pos + 8 + 2 * k);
            unsigned kind = entry >> 12;
            uint32_t rva = page + (entry & 0xFFFu);
            size_t fo;
            if (kind == 0) continue;
            if (kind != 3) BAD("%s: relocation type %u at RVA 0x%x is not supported", src, kind, rva);
            if (rva >= image_size) BAD("%s: relocation at RVA 0x%x is outside the image", src, rva);
            if (seen[rva / 8] & (1u << (rva % 8))) BAD("%s: two relocations at RVA 0x%x", src, rva);
            seen[rva / 8] |= (uint8_t)(1u << (rva % 8));
            fo = pe_offset(sec, nsect, n, rva, 4);
            if (fo == SIZE_MAX) BAD("%s: RVA 0x%x is not in the file data of a section", src, rva);
            put32(d + fo, get32(d + fo) + delta);
        }
        pos += block;
    }
    put32(d + opt + 28, new_base);
#undef BAD
    f = fopen(dst, "wb");
    if (f == NULL)
    {
        fail(w, "cannot make %s: %s", dst, strerror(errno));
        goto out;
    }
    if (fwrite(d, 1, n, f) != n)
    {
        fclose(f);
        fail(w, "cannot write %s", dst);
        goto out;
    }
    if (fclose(f) != 0)
    {
        fail(w, "cannot write %s: %s", dst, strerror(errno));
        goto out;
    }
    ok = true;
out:
    free(seen);
    free(d);
    return ok;
}

/* ---------------------------------------------------------------- units */

static bool tool_path(wk *w, char *out, const char *rel)
{
    if (!join(out, PATH_LEN, w->job->recompiler, rel)) return fail(w, "the converter path is too long");
    return true;
}

/* The common options of Apple's linker for this target. */
static unsigned ld_base_args(const rt_convert_tools *t, const char **argv)
{
    unsigned k = 1;
    argv[k++] = "-arch";
    argv[k++] = "arm64";
    argv[k++] = "-platform_version";
    argv[k++] = "macos";
    argv[k++] = MIN_MACOS;
    argv[k++] = t->sdk_version;
    argv[k++] = "-syslibroot";
    argv[k++] = t->sdk;
    return k;
}

static bool check_pins(wk *w, const module *m)
{
    unsigned i;
    for (i = 0; i < w->mf->npins; i++)
    {
        char p[PATH_LEN];
        const pin *pn = &w->mf->pins[i];
        if (strcmp(pn->module, m->name) != 0) continue;
        if (!join(p, sizeof p, m->dir, pn->file)) return fail(w, "a path is too long");
        if (!check_sha(w, p, pn->sha256, "translation")) return false;
    }
    return true;
}

/* FRONT: the SRW input, SRW, the pin check, llasm, llvm-split. */
static bool unit_front(wk *w, module *m)
{
    char src[PATH_LEN], dst[PATH_LEN], tool[PATH_LEN], inc1[PATH_LEN], inc2[PATH_LEN], map_opt[128],
        llasm_name[96], ll_name[96], parts_dir[PATH_LEN], prefix[128], jobs_arg[16];
    const char *argv[24];
    unsigned i, nfiles;

    if (m->kind == MOD_PE)
    {
        if (!make_dir(w, m->dir)) return false;
        if (!join(src, sizeof src, w->job->game_dir, m->game_path) || !join(dst, sizeof dst, m->dir, m->srw_input))
            return fail(w, "a path is too long");
        if (m->rebase != 0)
        {
            if (!rebase_pe(w, src, dst, m->rebase)) return false;
        }
        else if (!copy_file(w, src, dst))
            return false;
        if (!check_sha(w, dst, m->input_sha, "input of the translation")) return false;
    }
    if (!fmt_path(src, sizeof src, "%s/modules/%s", w->job->recompiler, m->name)) return fail(w, "a path is too long");
    if (!copy_dir_files(w, src, m->dir, &nfiles)) return false;
    if (nfiles == 0) return fail(w, "%s holds no analysis files", src);

    /* SRW: the address-preserving pass of tools/translate_module.sh. */
    snprintf(llasm_name, sizeof llasm_name, "%s.llasm", m->name);
    snprintf(ll_name, sizeof ll_name, "%s.ll", m->name);
    snprintf(map_opt, sizeof map_opt, "--preserve_addresses=%s_code_map", m->name);
    if (!tool_path(w, tool, "bin/SRW")) return false;
    i = 1;
    if (m->kind == MOD_AOT)
    {
        argv[i++] = "--no_export_entries";
        argv[i++] = "--int3_noreturn";
    }
    argv[i++] = map_opt;
    argv[i++] = m->srw_input;
    argv[i++] = llasm_name;
    argv[i] = NULL;
    if (!run_tool(w, "SRW", tool, argv, m->dir, false)) return false;
    if (!check_pins(w, m)) return false;
    if (m->kind == MOD_PE && !remove_in(w, m->dir, m->srw_input)) return false;

    /* llasm with the options of tools/translate_module.sh. */
    if (!tool_path(w, tool, "bin/llasm") ||
        !tool_path(w, inc1, "include/llasm-support") || !tool_path(w, inc2, "include/runtime"))
        return false;
    i = 1;
    argv[i++] = "-m64";
    argv[i++] = "-inline-idiv";
    argv[i++] = "-inline-float";
    argv[i++] = "-pic";
    argv[i++] = "-ptrofs";
    argv[i++] = "-O";
    argv[i++] = "-I";
    argv[i++] = inc1;
    argv[i++] = "-I";
    argv[i++] = inc2;
    argv[i++] = "-I";
    argv[i++] = ".";
    argv[i++] = llasm_name;
    argv[i++] = "-o";
    argv[i++] = ll_name;
    argv[i] = NULL;
    if (!run_tool(w, "llasm", tool, argv, m->dir, false)) return false;
    if (!remove_in(w, m->dir, llasm_name)) return false;
    for (i = 0; i < w->mf->npins; i++)
        if (strcmp(w->mf->pins[i].module, m->name) == 0 && strcmp(w->mf->pins[i].file, llasm_name) != 0 &&
            !remove_in(w, m->dir, w->mf->pins[i].file))
            return false;

    /* The target and the data layout go at the end of the module (LLVM
       assembly allows them anywhere at the top level). llvm-split then
       reads the text directly: the module is parsed once, not three times
       (opt to bitcode, then llvm-split), on the longest path of the
       conversion. */
    if (!fmt_path(src, sizeof src, "%s/%s", m->dir, ll_name)) return fail(w, "a path is too long");
    if (!append_text(w, src, "\ntarget datalayout = \"" DATALAYOUT "\"\ntarget triple = \"" TRIPLE "\"\n")) return false;

    /* llvm-split into parts/<module>0 ... parts/<module>N-1. */
    if (!join(parts_dir, sizeof parts_dir, m->dir, "parts") || !make_dir(w, parts_dir)) return false;
    if (!tool_path(w, tool, "bin/llvm-split")) return false;
    snprintf(jobs_arg, sizeof jobs_arg, "%u", m->parts);
    snprintf(prefix, sizeof prefix, "parts/%s", m->name);
    argv[1] = "-j";
    argv[2] = jobs_arg;
    argv[3] = ll_name;
    argv[4] = "-o";
    argv[5] = prefix;
    argv[6] = NULL;
    if (!run_tool(w, "llvm-split", tool, argv, m->dir, true)) return false;
    if (!remove_in(w, m->dir, ll_name)) return false;
    for (i = 0; i < m->parts; i++)
    {
        struct stat st;
        if (!fmt_path(src, sizeof src, "%s/%s%u", parts_dir, m->name, i)) return fail(w, "a path is too long");
        if (stat(src, &st) != 0 || !S_ISREG(st.st_mode)) return fail(w, "llvm-split did not write %s", src);
    }
    return true;
}

/* PART: llvm-link copies the SSE helpers that the part calls into the part
   as internal functions (so that clang can inline them), then clang
   compiles the part (tools/compile_module.sh). */
static bool unit_part(wk *w, module *m, unsigned part)
{
    char parts_dir[PATH_LEN], in[PATH_LEN], sse[PATH_LEN], obj[PATH_LEN], tool[PATH_LEN],
        sse_bc[PATH_LEN];
    const char *argv[NCC1 + 8];
    unsigned k;

    if (!join(parts_dir, sizeof parts_dir, m->dir, "parts") ||
        !fmt_path(in, sizeof in, "%s/%s%u", parts_dir, m->name, part) ||
        !fmt_path(sse, sizeof sse, "%s/%s%u.sse.bc", parts_dir, m->name, part) ||
        !fmt_path(obj, sizeof obj, "%s/%s%u.o", parts_dir, m->name, part) ||
        !tool_path(w, sse_bc, "include/llasm-support/sse_inline.bc") || !tool_path(w, tool, "bin/llvm-link"))
        return fail(w, "a path is too long");
    argv[1] = "--only-needed";
    argv[2] = "--internalize";
    argv[3] = in;
    argv[4] = sse_bc;
    argv[5] = "-o";
    argv[6] = sse;
    argv[7] = NULL;
    if (!run_tool(w, "llvm-link", tool, argv, parts_dir, true)) return false;
    if (!remove_file(w, in)) return false;
    if (!tool_path(w, tool, "bin/clang")) return false;
    for (k = 0; k < NCC1; k++) argv[1 + k] = cc1_args[k];
    argv[1 + NCC1] = "-o";
    argv[2 + NCC1] = obj;
    argv[3 + NCC1] = "-x";
    argv[4 + NCC1] = "ir";
    argv[5 + NCC1] = sse;
    argv[6 + NCC1] = NULL;
    if (!run_tool(w, "clang", tool, argv, parts_dir, true)) return false;
    return remove_file(w, sse);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* COMBINE: one relocatable object for the module (tools/build_aot_host.sh,
   step 6). The parts are in the order of the development build (sorted by
   name). */
static bool unit_combine(wk *w, module *m)
{
    char (*names)[96], (*paths)[PATH_LEN], parts_dir[PATH_LEN], sym1[96];
    const char **argv;
    unsigned i, k;
    bool ok;

    names = calloc(m->parts, sizeof *names);
    paths = calloc(m->parts, sizeof *paths);
    argv = calloc(m->parts + 24, sizeof *argv);
    if (names == NULL || paths == NULL || argv == NULL)
    {
        free(names);
        free(paths);
        free(argv);
        return fail(w, "out of memory");
    }
    ok = join(parts_dir, sizeof parts_dir, m->dir, "parts");
    for (i = 0; ok && i < m->parts; i++) snprintf(names[i], sizeof names[i], "%s%u", m->name, i);
    qsort(names, m->parts, sizeof *names, cmp_str);
    for (i = 0; ok && i < m->parts; i++) ok = fmt_path(paths[i], PATH_LEN, "%s/%s.o", parts_dir, names[i]);
    if (!ok)
    {
        free(names);
        free(paths);
        free(argv);
        return fail(w, "a path is too long");
    }
    k = ld_base_args(w->job->tools, argv);
    argv[k++] = "-r";
    argv[k++] = "-o";
    argv[k++] = m->object;
    for (i = 0; i < m->parts; i++) argv[k++] = paths[i];
    if (!m->all_symbols)
    {
        snprintf(sym1, sizeof sym1, "_%s_code_map", m->name);
        argv[k++] = "-exported_symbol";
        argv[k++] = sym1;
        argv[k++] = "-exported_symbol";
        argv[k++] = "_ptr_initialize_pointers";
    }
    argv[k] = NULL;
    ok = run_tool(w, "ld -r", w->job->tools->ld, argv, m->dir, false);
    if (ok) ok = remove_tree(w, parts_dir);
    free(names);
    free(paths);
    free(argv);
    return ok;
}

/* The link of the AOT compiler host or of the game program
   (tools/build_aot_host.sh, step 7). guest_space.o comes first, so that
   __GUEST comes before __LINKEDIT. */
static bool link_program(wk *w, bool player, const char *out)
{
    char obj_dir[PATH_LEN], guest[PATH_LEN], images[PATH_LEN], host_main[PATH_LEN], rtlib[PATH_LEN], crt[PATH_LEN],
        tmp[PATH_LEN], plist[PATH_LEN];
    const char **argv;
    const char *cs[8];
    unsigned k, i;
    bool ok;

    if (!tool_path(w, obj_dir, "lib/objects") || !join(guest, sizeof guest, obj_dir, "guest_space.o") ||
        !join(images, sizeof images, obj_dir, player ? "aot_images_player.o" : "aot_images_aot.o") ||
        !join(host_main, sizeof host_main, obj_dir, player ? "player_main.o" : "aot_main.o") ||
        !tool_path(w, rtlib, "lib/libpillars_rt.a") || !tool_path(w, crt, "lib/libclang_rt.osx.a") ||
        !join(plist, sizeof plist, obj_dir, "player_info.plist") || !fmt_path(tmp, sizeof tmp, "%s.tmp", out))
        return fail(w, "a path is too long");
    argv = calloc(w->mf->nmodules + NLINK_LIBS + 48, sizeof *argv);
    if (argv == NULL) return fail(w, "out of memory");
    k = ld_base_args(w->job->tools, argv);
    argv[k++] = "-dynamic";
    /* The player has millions of local symbols of the translated code
       (about 110 MB); nothing reads them at run time (rt_mem.c uses dladdr
       only for the image base). */
    if (player) argv[k++] = "-x";
    argv[k++] = "-o";
    argv[k++] = tmp;
    argv[k++] = guest;
    argv[k++] = images;
    argv[k++] = "-force_load";
    argv[k++] = rtlib;
    argv[k++] = host_main;
    /* The modules in the order of the development build: mono, the AOT
       images (manifest order), player, then the other PE modules. */
    argv[k++] = find_module((manifest *)w->mf, "mono")->object;
    if (player)
    {
        for (i = 0; i < w->mf->nmodules; i++)
            if (w->mf->modules[i].kind == MOD_AOT) argv[k++] = w->mf->modules[i].object;
        argv[k++] = find_module((manifest *)w->mf, "player")->object;
        for (i = 0; i < w->mf->nmodules; i++)
        {
            const module *m = &w->mf->modules[i];
            if (m->kind == MOD_PE && strcmp(m->name, "mono") != 0 && strcmp(m->name, "player") != 0) argv[k++] = m->object;
        }
        argv[k++] = "-rename_section";
        argv[k++] = "__DATA";
        argv[k++] = "__data";
        argv[k++] = "__XDATA";
        argv[k++] = "__data";
        argv[k++] = "-rename_section";
        argv[k++] = "__DATA";
        argv[k++] = "__bss";
        argv[k++] = "__XDATA";
        argv[k++] = "__bss";
        argv[k++] = "-rename_section";
        argv[k++] = "__DATA_CONST";
        argv[k++] = "__const";
        argv[k++] = "__XDATA";
        argv[k++] = "__const";
        argv[k++] = "-segaddr";
        argv[k++] = "__XDATA";
        argv[k++] = "0x12FE00000";
        /* The name of the game in the Dock and in the menu bar. */
        argv[k++] = "-sectcreate";
        argv[k++] = "__TEXT";
        argv[k++] = "__info_plist";
        argv[k++] = plist;
    }
    argv[k++] = "-segaddr";
    argv[k++] = "__GUEST";
    argv[k++] = "0x10FD00000";
    for (i = 0; i < NLINK_LIBS; i++) argv[k++] = link_libs[i];
    argv[k++] = crt;
    argv[k] = NULL;
    unlink(tmp);
    ok = run_tool(w, player ? "the link of the game" : "the link of the AOT compiler", w->job->tools->ld, argv,
                  w->job->work_dir, false);
    free(argv);
    if (!ok) return false;
    cs[1] = "--force";
    cs[2] = "--sign";
    cs[3] = "-";
    cs[4] = tmp;
    cs[5] = NULL;
    if (!run_tool(w, "codesign", "/usr/bin/codesign", cs, w->job->work_dir, false)) return false;
    if (rename(tmp, out) != 0) return fail(w, "cannot rename %s: %s", tmp, strerror(errno));
    return true;
}

static bool aot_host_path(wk *w, char *out)
{
    if (!join(out, PATH_LEN, w->job->work_dir, "mono-aot")) return fail(w, "a path is too long");
    return true;
}

/* AOT: the AOT compiler, the image build (tools/build_aot_image.sh) and the
   check of the image. */
static bool unit_aot(wk *w, module *m)
{
    char run[PATH_LEN], user[PATH_LEN], aot_dir[PATH_LEN], src[PATH_LEN], dst[PATH_LEN], host[PATH_LEN],
        aot_opt[512], source[256], out_s[PATH_LEN], image_s[PATH_LEN], exports[PATH_LEN], tool[PATH_LEN], base_opt[32],
        lib[PATH_LEN];
    const char *argv[24];
    const char *const managed[] = { PORT_ASSEMBLY_1, PORT_ASSEMBLY_2 };
    rt_aot_asm_stats st;
    unsigned i;

    if (!make_dir(w, m->dir) || !join(run, sizeof run, m->dir, "run") || !make_dir(w, run) ||
        !join(user, sizeof user, run, "user") || !make_dir(w, user) || !join(aot_dir, sizeof aot_dir, user, "aot") ||
        !make_dir(w, aot_dir))
        return false;
    /* The work directory of the AOT compiler holds the assemblies of the
       port, as in the development build: both for a game assembly, the one
       for an assembly of the port. */
    for (i = 0; i < 2; i++)
    {
        char name[160];
        snprintf(name, sizeof name, "%s.dll", m->assembly);
        if (m->port && strcmp(managed[i], name) != 0) continue;
        if (!fmt_path(src, sizeof src, "%s/managed/%s", w->job->recompiler, managed[i]) ||
            !join(dst, sizeof dst, aot_dir, managed[i]))
            return fail(w, "a path is too long");
        if (!copy_file(w, src, dst)) return false;
    }
    if (!aot_host_path(w, host)) return false;
    if (!fmt_path(aot_opt, sizeof aot_opt, "--aot=full,asmonly,asmwriter,nodebug%s%s,outfile=" GUEST_USER_AOT "%s.s",
                  m->options[0] != 0 ? "," : "", m->options, m->srw_input) ||
        !fmt_path(source, sizeof source, "%s%s.dll", m->port ? GUEST_USER_AOT : GUEST_MANAGED, m->assembly))
        return fail(w, "the AOT compiler options are too long");
    argv[1] = w->job->game_dir;
    argv[2] = run;
    argv[3] = "--guest-env";
    argv[4] = "MONO_DEBUG=explicit-null-checks";
    argv[5] = aot_opt;
    argv[6] = source;
    argv[7] = NULL;
    if (!run_tool(w, "the AOT compiler", host, argv, m->dir, false)) return false;

    /* The image (tools/build_aot_image.sh). */
    if (!fmt_path(out_s, sizeof out_s, "%s/%s.s", aot_dir, m->srw_input) || !join(image_s, sizeof image_s, m->dir, "image.s") ||
        !join(exports, sizeof exports, m->dir, "exports.rsp"))
        return fail(w, "a path is too long");
    if (!rt_aot_asm_convert(out_s, image_s, exports, &st, w->err, sizeof w->err)) return false;
    if (!remove_tree(w, run)) return false;
    if (!tool_path(w, tool, "bin/clang")) return false;
    i = 1;
    argv[i++] = "-cc1as";
    argv[i++] = "-triple";
    argv[i++] = "i686-pc-windows-msvc19.33.0";
    argv[i++] = "-filetype";
    argv[i++] = "obj";
    argv[i++] = "-main-file-name";
    argv[i++] = "image.s";
    argv[i++] = "-target-cpu";
    argv[i++] = "pentium4";
    argv[i++] = "-mrelocation-model";
    argv[i++] = "static";
    argv[i++] = "-o";
    argv[i++] = "image.obj";
    argv[i++] = "image.s";
    argv[i] = NULL;
    if (!run_tool(w, "the assembler", tool, argv, m->dir, true)) return false;
    if (!remove_file(w, image_s)) return false;
    if (!tool_path(w, tool, "bin/lld")) return false;
    snprintf(base_opt, sizeof base_opt, "/base:%s", m->base);
    if (!fmt_path(dst, sizeof dst, "/out:%s", m->srw_input)) return fail(w, "a path is too long");
    i = 1;
    argv[i++] = "-flavor";
    argv[i++] = "link";
    argv[i++] = "/dll";
    argv[i++] = "/noentry";
    argv[i++] = "/machine:x86";
    argv[i++] = "/safeseh:no";
    argv[i++] = "/nodefaultlib";
    argv[i++] = "/Brepro";
    argv[i++] = base_opt;
    argv[i++] = "@exports.rsp";
    argv[i++] = dst;
    argv[i++] = "image.obj";
    argv[i] = NULL;
    if (!run_tool(w, "lld-link", tool, argv, m->dir, true)) return false;
    /* lld-link also writes the import library <image without .dll>.lib. */
    if (!fmt_path(lib, sizeof lib, "%s/%s.dll.lib", m->dir, m->assembly))
        return fail(w, "a path is too long");
    if (!remove_file(w, lib) || !remove_in(w, m->dir, "image.obj") || !remove_file(w, exports)) return false;
    if (!join(dst, sizeof dst, m->dir, m->srw_input)) return fail(w, "a path is too long");
    return check_sha(w, dst, m->image_sha, "AOT image");
}

/* ---------------------------------------------------------------- scheduler */

typedef enum { U_FRONT, U_PART, U_COMBINE, U_AOT_HOST, U_AOT, U_PLAYER } unit_kind;

typedef struct unit {
    unit_kind kind;
    unsigned module;
    unsigned part;
} unit;

typedef struct queue {
    unit *u;
    unsigned head, tail, cap;
} queue;

typedef struct sched {
    const rt_convert_job *job;
    manifest *mf;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    queue high, low;
    unsigned running, combined;
    bool failed, finished;
    double done_w, total_w;
    char error[ERR_LEN];
} sched;

static bool push(queue *q, unit u)
{
    if (q->tail == q->cap) return false;
    q->u[q->tail++] = u;
    return true;
}

static double unit_weight(const sched *s, unit u)
{
    const module *m = &s->mf->modules[u.module];
    switch (u.kind)
    {
    case U_FRONT: return m->kind == MOD_PE ? (strcmp(m->name, "player") == 0 ? 40.0 : 6.0) : 2.0;
    case U_PART: return strcmp(m->name, "player") == 0 ? 3.0 : 1.0;
    case U_COMBINE: return 0.5;
    case U_AOT_HOST: return 2.0;
    case U_AOT: return 1.0;
    case U_PLAYER: return 4.0;
    }
    return 1.0;
}

static const char *unit_text(const sched *s, unit u)
{
    const module *m = &s->mf->modules[u.module];
    switch (u.kind)
    {
    case U_FRONT:
        if (strcmp(m->name, "player") == 0) return "Translating the game program";
        return m->kind == MOD_PE ? "Translating the game libraries" : "Translating the managed code of the game";
    case U_PART: return "Compiling the game code";
    case U_COMBINE: return "Combining the game code";
    case U_AOT_HOST: return "Linking the compiler for the managed code";
    case U_AOT: return "Compiling the managed code of the game";
    case U_PLAYER: return "Linking the game";
    }
    return "";
}

static bool run_unit(wk *w, module *m, unit u)
{
    char host[PATH_LEN], out[PATH_LEN];
    switch (u.kind)
    {
    case U_FRONT: return unit_front(w, m);
    case U_PART: return unit_part(w, m, u.part);
    case U_COMBINE: return unit_combine(w, m);
    case U_AOT_HOST: return aot_host_path(w, host) && link_program(w, false, host);
    case U_AOT: return unit_aot(w, m);
    case U_PLAYER:
        if (!join(out, sizeof out, w->job->out_dir, GAME_EXE)) return fail(w, "a path is too long");
        return link_program(w, true, out);
    }
    return fail(w, "unknown unit");
}

/* The follow-up units of a completed unit. Lock held. */
static bool unit_done(sched *s, unit u)
{
    module *m = &s->mf->modules[u.module];
    unsigned i;
    bool ok = true;
    switch (u.kind)
    {
    case U_FRONT:
        for (i = 0; ok && i < m->parts; i++) ok = push(&s->low, (unit){ U_PART, u.module, i });
        break;
    case U_PART:
        if (++m->parts_done == m->parts) ok = push(&s->high, (unit){ U_COMBINE, u.module, 0 });
        break;
    case U_COMBINE:
        if (strcmp(m->name, "mono") == 0) ok = push(&s->high, (unit){ U_AOT_HOST, u.module, 0 });
        if (ok && ++s->combined == s->mf->nmodules) ok = push(&s->high, (unit){ U_PLAYER, u.module, 0 });
        break;
    case U_AOT_HOST:
        for (i = 0; ok && i < s->mf->nmodules; i++)
            if (s->mf->modules[i].kind == MOD_AOT) ok = push(&s->high, (unit){ U_AOT, i, 0 });
        break;
    case U_AOT: ok = push(&s->high, (unit){ U_FRONT, u.module, 0 }); break;
    case U_PLAYER: s->finished = true; break;
    }
    return ok;
}

static void set_failed(sched *s, const char *msg)
{
    if (s->failed) return;
    s->failed = true;
    snprintf(s->error, sizeof s->error, "%s", msg);
    rt_convert_cancel();
}

static void *worker(void *arg)
{
    sched *s = arg;
    wk w;
    memset(&w, 0, sizeof w);
    w.job = s->job;
    w.mf = s->mf;
    pthread_mutex_lock(&s->lock);
    for (;;)
    {
        unit u;
        bool ok;
        double f;
        while (!s->failed && !s->finished && s->high.head == s->high.tail && s->low.head == s->low.tail)
        {
            if (s->running == 0)
            {
                set_failed(s, "the conversion stopped with no unit to run (an error of the converter)");
                break;
            }
            pthread_cond_wait(&s->cond, &s->lock);
        }
        if (s->failed || s->finished) break;
        u = s->high.head != s->high.tail ? s->high.u[s->high.head++] : s->low.u[s->low.head++];
        s->running++;
        f = s->done_w / s->total_w;
        if (s->job->progress != NULL) s->job->progress(s->job->ctx, f > 0.99 ? 0.99 : f, unit_text(s, u));
        pthread_mutex_unlock(&s->lock);
        ok = !atomic_load(&cancelled) ? run_unit(&w, &s->mf->modules[u.module], u) : fail(&w, "the conversion was cancelled");
        pthread_mutex_lock(&s->lock);
        s->running--;
        if (!ok) set_failed(s, w.err);
        else
        {
            s->done_w += unit_weight(s, u);
            if (!unit_done(s, u)) set_failed(s, "the unit queue of the converter is full");
        }
        pthread_cond_broadcast(&s->cond);
    }
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    return NULL;
}

static bool check_inputs(wk *w)
{
    unsigned i;
    for (i = 0; i < w->mf->ninputs; i++)
    {
        char p[PATH_LEN];
        if (!join(p, sizeof p, w->job->game_dir, w->mf->inputs[i].path)) return fail(w, "a path is too long");
        if (!check_sha(w, p, w->mf->inputs[i].sha256, "game file")) return false;
    }
    return true;
}

/* Copy the AOT images and the assemblies of the port to out_dir/aot, where
   the game program finds them (runtime/host/player_main.c). */
static bool copy_aot_files(wk *w)
{
    char dir[PATH_LEN], src[PATH_LEN], dst[PATH_LEN];
    unsigned i;
    if (!join(dir, sizeof dir, w->job->out_dir, "aot") || !make_dir(w, dir)) return false;
    for (i = 0; i < w->mf->nmodules; i++)
    {
        const module *m = &w->mf->modules[i];
        if (m->kind != MOD_AOT) continue;
        if (!join(src, sizeof src, m->dir, m->srw_input) || !join(dst, sizeof dst, dir, m->srw_input))
            return fail(w, "a path is too long");
        if (!copy_file(w, src, dst)) return false;
    }
    if (!fmt_path(src, sizeof src, "%s/managed/" PORT_ASSEMBLY_1, w->job->recompiler) ||
        !join(dst, sizeof dst, dir, PORT_ASSEMBLY_1) || !copy_file(w, src, dst) ||
        !fmt_path(src, sizeof src, "%s/managed/" PORT_ASSEMBLY_2, w->job->recompiler) ||
        !join(dst, sizeof dst, dir, PORT_ASSEMBLY_2) || !copy_file(w, src, dst))
        return w->err[0] != 0 ? false : fail(w, "a path is too long");
    return true;
}

bool rt_convert_run(rt_convert_job *job)
{
    sched *s;
    wk w;
    struct statfs fs;
    pthread_t threads[MAX_JOBS];
    unsigned jobs = job_count(), started = 0, i, nunits = 0;
    bool ok;

    job->error[0] = 0;
    atomic_store(&cancelled, false);
    memset(&w, 0, sizeof w);
    w.job = job;
    if (statfs(job->work_dir, &fs) != 0)
    {
        snprintf(job->error, sizeof job->error, "cannot read the free space of %s", job->work_dir);
        return false;
    }
    if ((uint64_t)fs.f_bavail * fs.f_bsize < MIN_FREE_BYTES)
    {
        snprintf(job->error, sizeof job->error,
                 "The conversion needs %llu GB of free disk space for temporary files. Free space: %.1f GB.",
                 (unsigned long long)(MIN_FREE_BYTES >> 30), (double)((uint64_t)fs.f_bavail * fs.f_bsize) / 1e9);
        return false;
    }
    s = calloc(1, sizeof *s);
    if (s == NULL || (s->mf = calloc(1, sizeof *s->mf)) == NULL)
    {
        free(s);
        snprintf(job->error, sizeof job->error, "out of memory");
        return false;
    }
    w.mf = s->mf;
    ok = load_manifest(&w, s->mf);
    if (ok)
    {
        if (job->progress != NULL) job->progress(job->ctx, 0, "Checking the game files");
        ok = check_inputs(&w);
    }
    for (i = 0; ok && i < s->mf->nmodules; i++)
    {
        module *m = &s->mf->modules[i];
        ok = join(m->dir, sizeof m->dir, job->work_dir, m->name) &&
             fmt_path(m->object, sizeof m->object, "%s/%s.o", job->work_dir, m->name);
        if (!ok) fail(&w, "the work directory path is too long");
        nunits += m->parts + 3;
    }
    if (!ok)
    {
        snprintf(job->error, sizeof job->error, "%s", w.err);
        free(s->mf);
        free(s);
        return false;
    }
    s->job = job;
    s->high.cap = s->low.cap = nunits + 8;
    s->high.u = calloc(s->high.cap, sizeof *s->high.u);
    s->low.u = calloc(s->low.cap, sizeof *s->low.u);
    if (s->high.u == NULL || s->low.u == NULL || pthread_mutex_init(&s->lock, NULL) != 0 ||
        pthread_cond_init(&s->cond, NULL) != 0)
    {
        free(s->high.u);
        free(s->low.u);
        free(s->mf);
        free(s);
        snprintf(job->error, sizeof job->error, "out of memory");
        return false;
    }
    /* The total weight for the progress bar. */
    for (i = 0; i < s->mf->nmodules; i++)
    {
        module *m = &s->mf->modules[i];
        s->total_w += unit_weight(s, (unit){ U_FRONT, i, 0 }) + m->parts * unit_weight(s, (unit){ U_PART, i, 0 }) +
                      unit_weight(s, (unit){ U_COMBINE, i, 0 });
        if (m->kind == MOD_AOT) s->total_w += unit_weight(s, (unit){ U_AOT, i, 0 });
        else push(&s->high, (unit){ U_FRONT, i, 0 });
    }
    s->total_w += unit_weight(s, (unit){ U_AOT_HOST, 0, 0 }) + unit_weight(s, (unit){ U_PLAYER, 0, 0 });

    pthread_mutex_lock(&s->lock);
    for (i = 0; i < jobs; i++)
    {
        pthread_attr_t attr;
        if (pthread_attr_init(&attr) != 0) break;
        if (pthread_attr_setstacksize(&attr, 8u << 20) != 0 || pthread_create(&threads[started], &attr, worker, s) != 0)
        {
            pthread_attr_destroy(&attr);
            break;
        }
        pthread_attr_destroy(&attr);
        started++;
    }
    if (started == 0) set_failed(s, "no worker thread can be made");
    pthread_mutex_unlock(&s->lock);
    for (i = 0; i < started; i++) pthread_join(threads[i], NULL);

    ok = !s->failed && s->finished;
    if (!ok) snprintf(job->error, sizeof job->error, "%s", s->error[0] != 0 ? s->error : "the conversion did not complete");
    else
    {
        ok = copy_aot_files(&w);
        if (!ok) snprintf(job->error, sizeof job->error, "%s", w.err);
    }
    if (ok && job->progress != NULL) job->progress(job->ctx, 1.0, "Done");
    pthread_cond_destroy(&s->cond);
    pthread_mutex_destroy(&s->lock);
    free(s->high.u);
    free(s->low.u);
    free(s->mf);
    free(s);
    return ok;
}
