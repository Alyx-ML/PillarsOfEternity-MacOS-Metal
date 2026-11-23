/*
 * Code addresses of address-preserving translated modules (SRW
 * --preserve_addresses).
 *
 * A translated module keeps the original x86 addresses: return addresses,
 * function pointers in guest memory and code addresses in registers are
 * the addresses of the original PE image. Each return, indirect call and
 * indirect jump of the translated code calls rt_code_target to get the
 * translated procedure for such an address.
 */
#ifndef RT_CODE_H
#define RT_CODE_H

#include "rt.h"

/* Register the code map of a module that is mapped at guest address
   [image_base, image_base + image_size). The map is the data that SRW
   writes as the "<name>_code_map" segment (4-byte entry count, then pairs
   of original address and translated procedure address, in increasing
   address order). Stops the process if the map is not valid or if the
   image overlaps a registered module. Call it before guest code of the
   module runs. */
void rt_code_register(const char *name, uint32_t image_base, uint32_t image_size, const uint8_t *map);

/* The translated procedure (guest procedure address) for an original code
   address. An address in a registered module image must be a procedure
   entry of the code map. An address that is outside all registered module
   images must be in the program image (a runtime procedure, for example an
   import thunk or the return procedure of a call from C). Any other address
   stops the process with an error message. */
uint32_t rt_code_target(uint32_t address);

/* Register position-independent code of a module image that the module
   copies to memory that it allocates and then calls: the len bytes at
   guest address template. A control transfer to an address outside all
   module images whose bytes equal the template runs the runtime procedure
   proc (a guest procedure address in the program image), which does what
   the template code does. Stops the process if the table is full. */
void rt_code_register_copy(const char *name, uint32_t template_addr, uint32_t len, uint32_t proc);

/* Is the original code address a procedure entry of a registered module?
   Returns the translated procedure, or 0. */
uint32_t rt_code_find(uint32_t address);

#endif
