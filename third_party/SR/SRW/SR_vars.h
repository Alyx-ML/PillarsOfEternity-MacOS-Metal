/**
 *
 *  Copyright (C) 2016-2025 Roman Pauer
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy of
 *  this software and associated documentation files (the "Software"), to deal in
 *  the Software without restriction, including without limitation the rights to
 *  use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 *  of the Software, and to permit persons to whom the Software is furnished to do
 *  so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 */

#if !defined(_SR_VARS_H_INCLUDED_)
#define _SR_VARS_H_INCLUDED_

#include <udis86.h>

#include "SR_defs.h"

#if defined(DEFINE_VARIABLES)
	#define EXTERNAL_VARIABLE
#else
	#define EXTERNAL_VARIABLE extern
#endif

#ifdef __cplusplus
extern "C" {
#endif

EXTERNAL_VARIABLE section_data section[MAX_SECTIONS];   // array of sections
EXTERNAL_VARIABLE unsigned int num_sections;            // number of sections
EXTERNAL_VARIABLE uint8_t *SR_CodeBase;                 // allocated data

EXTERNAL_VARIABLE uint_fast32_t EIPObjectNum;           // initial section
EXTERNAL_VARIABLE uint_fast32_t EIP;                    // initial offset

EXTERNAL_VARIABLE ud_t ud_obj;                          // udis86 object

EXTERNAL_VARIABLE int list_invalid_code_fixups;
EXTERNAL_VARIABLE char *invalid_code_fixups_name;

EXTERNAL_VARIABLE int list_data_to_code_fixups;
EXTERNAL_VARIABLE char *data_to_code_fixups_name;

EXTERNAL_VARIABLE int add_tracing_to_code;

EXTERNAL_VARIABLE int esp_dword_aligned;
EXTERNAL_VARIABLE int ebp_dword_aligned;

/* macOS ARM64 port: address-preserving llasm output (command-line option
   --preserve_addresses=<code map name>). The runtime maps the PE image at its original
   base, so data references and code addresses used as values keep their
   original numeric values. Indirect transfers go through the runtime
   function rt_code_target, which maps an original code address to its
   translated procedure with the table code_map_name. */
EXTERNAL_VARIABLE int preserve_addresses;
EXTERNAL_VARIABLE char *code_map_name;

/* macOS ARM64 port: exports are not code entry points (command-line option
   --no_export_entries). An export in a code section gets a label, but SRW
   does not disassemble from it. Use it for an image with data exports in
   its code section (Mono AOT images): code_entry_points.sci then lists the
   code entry points. */
EXTERNAL_VARIABLE int no_export_entries;

/* macOS ARM64 port: code entry points from code_entry_points.sci (one
   "loc_<hex address>" for each line). SRW disassembles from each address
   and starts a procedure there, as for an export in a code section. Use
   it for code that the program reaches only through computed addresses.
   fixup_interpret_as_code.sci does not do this: it only identifies
   relocation targets that are code. */
typedef struct {
    unsigned int SecNum;
    uint_fast32_t RelAdr;
} code_entry_point;
EXTERNAL_VARIABLE code_entry_point *code_entry_points;
EXTERNAL_VARIABLE size_t num_code_entry_points;

/* macOS ARM64 port: the int3 instruction (byte 0xCC) ends the control flow
   (command-line option --int3_noreturn). SRW does not disassemble the
   bytes after it, unless another path reaches them, and writes no tail
   call to the next address. Use it for code that puts int3 only at points
   that execution does not pass (Mono AOT code puts int3 after calls that
   do not return), with data after it. The runtime stops the process at an
   int3 instruction (X86_InterruptProcedure), so the end of the procedure
   is not reached. */
EXTERNAL_VARIABLE int int3_noreturn;

EXTERNAL_VARIABLE Pvoid_t import_list;

#ifdef __cplusplus
}
#endif

#if defined(USE_JUDY)
	#include "judy_helpers.h"
#else
	#include "container_helpers.h"
#endif

int SR_get_section_reladr(uint_fast32_t Address, uint_fast32_t *SecNum, uint_fast32_t *RelAdr);

void SR_get_label(char *cbuf, uint_fast32_t Address);

int SRW_LoadFile(const char *fname);

int SR_LoadSCI(void);

int SR_initial_disassembly(void);
int SR_apply_fixup_info(void);
int SR_full_disassembly(void);
int SR_write_output(const char *fname);



output_data *SR_disassemble_offset_init_output(unsigned int SecNum, uint_fast32_t offset, unsigned int length);

void SR_disassemble_add_address(unsigned int Entry, uint32_t address);
void SR_disassemble_align_fixup(const fixup_data *fixup);
int SR_disassemble_convert_cjump(char *dst, const char *modifier, uint_fast32_t address, extrn_data *extrn);
int SR_disassemble_convert_fixup(const char *ostr, char *dst, fixup_data *fixup, extrn_data *extrn, int Entry, uint_fast32_t offset, int decoded_length);
int SR_disassemble_find_noret(uint_fast32_t address);

extrn_data *SR_disassemble_find_proc(unsigned int Entry, uint_fast32_t address);

int SR_disassemble_offset_windows(unsigned int Entry, uint_fast32_t offset);
int SR_disassemble_offset_win32(unsigned int Entry, uint_fast32_t offset);
int SR_disassemble_region_llasm(unsigned int Entry, region_data *region);
int SR_disassemble_region_x64(unsigned int Entry, region_data *region);

int SR_disassemble_llasm_instruction(unsigned int Entry, output_data *output, uint_fast32_t flags_to_write, uint_fast32_t *pflags_write, uint_fast32_t *pflags_read, int *plast_instruction);
int SR_disassemble_x64_instruction(unsigned int Entry, output_data *output, region_data *region);

#undef EXTERNAL_VARIABLE

#endif
