/*
 * Command-line tool of tools/build_aot_image.sh: the conversion of the
 * Mono AOT assembler output (runtime/loader/rt_aot_asm.h), the same
 * function that the converter of the release uses.
 *
 * Usage: aot_asm <compiler output .s> <output .s> <export list>
 * Prints the counts of the conversion. Exit status 1 on an error.
 */
#include "rt_aot_asm.h"

#include <stdio.h>

int main(int argc, char **argv)
{
    rt_aot_asm_stats st;
    char err[2048];
    if (argc != 4)
    {
        fprintf(stderr, "usage: aot_asm <compiler output .s> <output .s> <export list>\n");
        return 2;
    }
    if (!rt_aot_asm_convert(argv[1], argv[2], argv[3], &st, err, sizeof err))
    {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    printf("removed %u .local and .size lines, %u exports, %u message addresses set to 0, %u kept (in mono.dll)\n",
           st.lines_removed, st.exports, st.pointers_zeroed, st.pointers_kept);
    return 0;
}
