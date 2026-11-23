/*
 * GDI32 functions for other parts of the runtime (rt_gdi32.c).
 */
#ifndef RT_GDI32_H
#define RT_GDI32_H

#include <stdbool.h>
#include <stdint.h>

#define ERROR_INVALID_PIXEL_FORMAT 2000u

/* True if hdc is a device context handle. Any thread. */
bool rt_gdi32_is_dc(uint32_t hdc);

#endif
