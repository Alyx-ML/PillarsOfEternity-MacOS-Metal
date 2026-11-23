/*
 * The conversion of the assembler output of the Mono full-AOT compiler for
 * the image build (rt_aot_asm.c). tools/build_aot_image.sh (with
 * tools/aot_asm_main.c) and the converter of the release (rt_convert.c) use
 * this one implementation, so that both make the same image from the same
 * compiler output.
 *
 * The conversion:
 *   1. COFF syntax. The output uses ELF directives: ".local" and ".size"
 *      only give symbol binding and size, which COFF does not need, so
 *      these lines are removed. ".hword" becomes ".short" (the same 2-byte
 *      value). The output must have no section other than .text, .data and
 *      .bss.
 *   2. Message pointers. For CEE_MONO_LDPTR, Mono writes the address of a
 *      C string of the compiler process into the code of a wrapper: the
 *      message of a MarshalDirectiveException ("sub esp, 12; push <address>;
 *      call mono_string_new_wrapper"). An address in the compiler heap has
 *      no meaning in the game process, and it changes from run to run (the
 *      finalizer thread of Mono allocates at a time that depends on thread
 *      scheduling). The conversion replaces each such address that is not
 *      in mono.dll (RT_AOT_MONO_BASE to RT_AOT_MONO_END; mono.dll has the
 *      same base in the game, so its strings stay valid) with 0.
 *      mono_string_new_wrapper(NULL) gives a null string, so the exception
 *      has no message.
 *   3. Exports. Each ".globl" symbol must have the C prefix "_". The export
 *      list has one "/export:<name>" line (the name without the prefix) for
 *      each symbol, in byte order, because Mono finds the tables of an
 *      image with GetProcAddress.
 */
#ifndef RT_AOT_ASM_H
#define RT_AOT_ASM_H

#include <stdbool.h>
#include <stddef.h>

/* The image range of mono.dll (the base and the end of the image). */
#define RT_AOT_MONO_BASE 0x10000000u
#define RT_AOT_MONO_END 0x10230000u

typedef struct rt_aot_asm_stats {
    unsigned lines_removed;   /* .local and .size lines */
    unsigned pointers_zeroed; /* message addresses outside mono.dll */
    unsigned pointers_kept;   /* message addresses in mono.dll */
    unsigned exports;
} rt_aot_asm_stats;

/* Convert the compiler output in_path to out_path (the COFF assembler
   source) and write the export list to exports_path (a response file of
   lld-link). Returns false with a message in err. */
bool rt_aot_asm_convert(const char *in_path, const char *out_path, const char *exports_path, rt_aot_asm_stats *stats,
                        char *err, size_t errn);

#endif
