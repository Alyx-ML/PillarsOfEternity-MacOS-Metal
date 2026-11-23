/* The converter: makes the game program from the game files of the player.
 *
 * The converter runs the same steps as the development build with the tools
 * in Contents/Resources/Recompiler (see rt_convert.c for the layout):
 *   1. mono.dll, PillarsOfEternity.exe and ShroudUnityPlugin.dll: SRW
 *      translates each module to llasm with its analysis files. The
 *      SHA-256 values of the SRW output must agree with manifest.txt (the
 *      tested translation). llasm makes LLVM IR, opt checks it, llvm-split
 *      divides it, clang compiles the parts to ARM64 objects, and the
 *      linker combines the parts of each module.
 *   2. The Mono AOT compiler host: the translated mono.dll with the runtime.
 *   3. The AOT compiler host compiles each managed assembly of the game
 *      (and the two assemblies of the port) to x86 code; the image build
 *      (rt_aot_asm.h, the assembler and lld-link) makes a DLL of each
 *      result. The SHA-256 of each DLL must agree with manifest.txt. Each
 *      DLL is then translated as in step 1.
 *   4. Apple's linker of the Command Line Tools links the runtime, the
 *      player host and all translated modules into the game program.
 * The converter runs on a thread that is not the main thread. The steps of
 * different modules run in parallel.
 */
#ifndef RT_CONVERT_H
#define RT_CONVERT_H

#include <stdbool.h>
#include <stddef.h>

/* fraction: 0 to 1. text: a short description of the current step. */
typedef void (*rt_convert_progress_fn)(void *ctx, double fraction, const char *text);

typedef struct rt_convert_tools {
    char ld[1024];         /* Apple's linker (xcrun --find ld) */
    char sdk[1024];        /* the macOS SDK (xcrun --sdk macosx --show-sdk-path) */
    char sdk_version[32];  /* its version (xcrun --sdk macosx --show-sdk-version) */
} rt_convert_tools;

typedef struct rt_convert_job {
    const char *recompiler;        /* the Recompiler directory (a copy without quarantine) */
    const char *game_dir;          /* the checked game directory */
    const char *work_dir;          /* an empty directory for intermediate files */
    const char *out_dir;           /* an empty directory for the game program and its AOT images */
    const rt_convert_tools *tools; /* from rt_convert_find_tools */
    rt_convert_progress_fn progress;
    void *ctx;
    char error[4096];              /* the reason when rt_convert_run returns false */
} rt_convert_job;

/* Run all steps. Returns false with job->error set if a step fails or the
   conversion was cancelled. The result is out_dir/PillarsOfEternity (the
   game program) and out_dir/aot (the AOT images and the assemblies of the
   port). */
bool rt_convert_run(rt_convert_job *job);

/* Stop the conversion: stops the running tools. rt_convert_run then
   returns false. Can be called from any thread. */
void rt_convert_cancel(void);

/* Are the Command Line Tools (or Xcode) installed? xcode-select -p gives
   the developer directory. This check does not start the installer of
   macOS, as xcrun does when no developer directory exists. */
bool rt_convert_have_developer_dir(void);

/* Find the linker and the SDK with xcrun. Call it only when
   rt_convert_have_developer_dir is true. Returns false with a message in
   err. */
bool rt_convert_find_tools(rt_convert_tools *tools, char *err, size_t errn);

/* The SHA-256 of a file as 64 lower-case hex digits. Returns false with a
   message in err if the file cannot be read. */
bool rt_sha256_file(const char *path, char out[65], char *err, size_t errn);

#endif
