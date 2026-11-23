/*
 * Translated Mono AOT images that are linked into the host.
 *
 * tools/build_aot_host.sh generates the table (build/aot/images/aot_images.c)
 * from the work directories of tools/build_aot_image.sh. Each entry is one
 * AOT image DLL: the file name that Mono loads ("<assembly file name>.dll"),
 * the SHA-256 of the DLL that was translated, and the code map of the
 * translation.
 */
#ifndef AOT_IMAGES_H
#define AOT_IMAGES_H

#include <stdint.h>

typedef struct aot_image {
    const char *name;          /* for example "mscorlib.dll.dll" */
    const char *sha256;        /* lower-case hex */
    const uint8_t *code_map;   /* <work-dir-name>_code_map */
} aot_image;

/* aot_images is NULL when aot_image_count is 0. */
extern const aot_image *const aot_images;
extern const unsigned aot_image_count;

#endif
