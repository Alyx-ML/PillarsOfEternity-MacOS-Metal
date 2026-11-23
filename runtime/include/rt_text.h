/*
 * Text conversion between guest strings and the runtime.
 *
 * The runtime keeps names and paths as UTF-8. Guest strings come in two
 * forms:
 *   - ANSI strings ("A" functions): Windows-1252, the ANSI code page of
 *     the emulated system.
 *   - wide strings ("W" functions): UTF-16LE.
 */
#ifndef RT_TEXT_H
#define RT_TEXT_H

#include "rt.h"

#include <stdbool.h>
#include <stddef.h>

/* Results of the guest string readers. */
#define RT_STR_OK 0
#define RT_STR_TOO_LONG (-1)  /* the string does not fit in the buffer */
#define RT_STR_INVALID (-2)   /* a wide string has an unpaired surrogate */

/* Decode one UTF-8 sequence at s[0..n). Returns the length (>= 1) and the
   code point, or -(length of the invalid sequence) for invalid input. */
int rt_utf8_decode(const uint8_t *s, size_t n, uint32_t *cp);

/* Encode code point cp (at most U+10FFFF, not a surrogate) as UTF-8.
   Returns the number of bytes (1 to 4). */
size_t rt_utf8_encode(uint32_t cp, char out[4]);

/* Read the NUL-terminated ANSI guest string at g as UTF-8 into dst (max
   bytes with the NUL). */
int rt_gstr_a(char *dst, size_t max, uint32_t g);

/* Read the NUL-terminated wide guest string at g as UTF-8 into dst (max
   bytes with the NUL). */
int rt_gstr_w(char *dst, size_t max, uint32_t g);

/* Read count UTF-16 units (no terminator needed) at g as UTF-8. */
int rt_gstrn_w(char *dst, size_t max, uint32_t g, uint32_t count);

/* Number of UTF-16 units before the NUL of the wide guest string at g. */
uint32_t rt_wstrlen(uint32_t g);

/* Number of UTF-16 units of a valid UTF-8 string. Returns false for
   invalid UTF-8. */
bool rt_utf16_length(const char *s, size_t *units);

/* Write the UTF-8 string s as a wide string to the guest buffer buf of n
   UTF-16 units, with the rules of GetCurrentDirectoryW and similar
   functions: if the string and its NUL fit, write them and return the
   length without the NUL; else write nothing and return the required size
   with the NUL. Stops the process for invalid UTF-8 (a runtime defect). */
uint32_t rt_put_wstr(uint32_t buf, uint32_t n, const char *s);

/* Write s as an ANSI guest string with the same rules. Characters that
   Windows-1252 cannot show become '?', as with WideCharToMultiByte and the
   default character. */
uint32_t rt_put_astr(uint32_t buf, uint32_t n, const char *s);

/* Write s as a wide string to a fixed guest field of n units (n >= 1),
   with a NUL. Returns false (and writes an empty string) if s does not fit. */
bool rt_put_wfield(uint32_t buf, uint32_t n, const char *s);

/* The same for an ANSI field. */
bool rt_put_afield(uint32_t buf, uint32_t n, const char *s);

/* Unicode code point to Windows-1252, or -1 if the code page has no such
   character. */
int rt_unicode_to_cp1252(uint32_t cp);

/* OEM code page 437 (the OEM code page of the emulated system). */
uint16_t rt_cp437_to_unicode(uint8_t c);
int rt_unicode_to_cp437(uint32_t cp);

#endif
