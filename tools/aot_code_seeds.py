#!/usr/bin/env python3
"""List the code entry points of a Mono full-AOT image for SRW.

The input is a PE DLL that tools/build_aot_image.sh made from the assembler
output of the Mono AOT compiler (format version 66, the Mono of Unity 4.x).
The DLL exports each global symbol of the output. Mono finds the tables of
the image with these exports (find_symbol through GetProcAddress).
Mono finds most code in the image by computed addresses, not by
relocations or direct calls, so SRW cannot find this code by recursive
descent. This script reads the tables of the image as the Mono AOT loader
(mono/mini/aot-runtime.c of the unity-4.6 branch) reads them and lists
these entry points:

  - the named code of the export table: generic trampolines,
    exception code (restore_context, call_filter, throw_*), rgctx fetch
    trampolines, delegate invoke code and unbox trampolines (ut_N, ut_e_N)
  - each PLT entry (plt + 10 * i)
  - each specific trampoline, static rgctx trampoline and IMT thunk
    (array start + size * i, from mono_aot_file_info)
  - the start of each compiled method (method_addresses)
  - the handler start and the filter start of each exception clause
    (ex_info); Mono enters them through restore_context and call_filter
  - each target of each switch table (MONO_PATCH_INFO_SWITCH entries of
    the GOT patches of the method, method_info and got_info)

Each export must be known code or known data. An
unknown name, an unknown encoding or an entry that is not at an
instruction start stops the script with an error. The instruction starts
come from a linear disassembly (llvm-objdump) of the method area; Mono
puts only code and alignment padding there.

Usage: aot_code_seeds.py <image.dll> <seeds.sci> <report.txt>
  seeds.sci: "loc_X" per line, the format of the SRW files
             code_entry_points.sci and fixup_interpret_as_code.sci
  report.txt: the counts of each kind of entry point
Exit status: 0 on success, 1 on an error.
"""
import os
import re
import struct
import subprocess
import sys

LLVM_OBJDUMP = '/opt/homebrew/opt/llvm/bin/llvm-objdump'

AOT_VERSION = '66'
PLT_ENTRY_SIZE = 10          # jmp *<disp32>(%ebx); <info offset (4 bytes)>
PATCH_SWITCH = 8             # MONO_PATCH_INFO_SWITCH (mono/mini/patch-info.h)
# Patch types whose data is an offset in the method code. Only SWITCH is
# in the GOT of a full-AOT image; the others would be code entries that
# this script does not decode.
PATCH_CODE_RELATIVE = {0: 'BB', 2: 'LABEL', 5: 'METHOD_REL', 22: 'IP', 25: 'BB_OVF'}
OPT_SHARED = 1 << 9          # MONO_OPT_SHARED (mono/mini/optflags-def.h)
CLAUSE_FILTER = 1            # MONO_EXCEPTION_CLAUSE_FILTER
TOKEN_TYPE_DEF = 0x02000000
TOKEN_TYPE_SPEC = 0x1B000000
TABLE_TYPESPEC = 0x1B
TYPE_VAR, TYPE_GENERICINST, TYPE_MVAR = 0x13, 0x15, 0x1E
METHODREF_MIN, METHODREF_WRAPPER_NAME, METHODREF_ARRAY = 240, 250, 251
METHODREF_NO_AOT_TRAMPOLINE, METHODREF_WRAPPER = 252, 253
METHODREF_GINST, METHODREF_METHODSPEC = 254, 255

CODE_NAMES = {
    'plt', 'call_filter', 'restore_context', 'throw_exception', 'rethrow_exception',
    'throw_exception_by_name', 'throw_corlib_exception', 'nullified_class_init_trampoline',
    'generic_class_init_trampoline', 'delegate_invoke_impl_has_target',
}
CODE_PATTERNS = [re.compile(p) for p in (
    r'generic_trampoline_\d+$', r'rgctx_fetch_trampoline_\d+$', r'delegate_invoke_impl_target_\d+$',
    r'ut_\d+$', r'ut_e_\d+$',
)]
ARRAY_NAMES = ['specific_trampolines', 'static_rgctx_trampolines', 'imt_thunks']  # MonoAotTrampoline order
DATA_NAMES = {
    'methods', 'methods_end', 'method_addresses', 'method_offsets', 'method_info', 'method_info_offsets',
    'method_order', 'method_order_end', 'ex_info', 'ex_info_offsets', 'extra_method_table',
    'extra_method_info', 'extra_method_info_offsets', 'class_info', 'class_info_offsets',
    'class_name_table', 'got_info', 'got_info_offsets', 'unwind_info', 'plt_end', 'mono_image_table',
    'mono_aot_got_addr', 'mono_aot_file_info', 'mono_assembly_guid', 'mono_aot_version',
    'mono_aot_opt_flags', 'mono_aot_full_aot', 'mono_runtime_version', 'mem_end',
}
DATA_PATTERNS = [re.compile(r'.+_p$')]   # patch lists of the named code (emit_trampoline)


class Error(Exception):
    pass


class Image:
    """The sections of a 32-bit PE file, addressed by virtual address."""

    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = f.read()
        d = self.data
        if d[:2] != b'MZ':
            raise Error(f'{path} is not a PE file')
        pe = struct.unpack_from('<I', d, 0x3C)[0]
        if d[pe:pe + 4] != b'PE\0\0':
            raise Error(f'{path} is not a PE file')
        machine, nsec = struct.unpack_from('<HH', d, pe + 4)
        opt_size = struct.unpack_from('<H', d, pe + 20)[0]
        opt = pe + 24
        if machine != 0x14C or struct.unpack_from('<H', d, opt)[0] != 0x10B:
            raise Error(f'{path} is not a 32-bit x86 PE file')
        self.base = struct.unpack_from('<I', d, opt + 28)[0]
        export_rva = struct.unpack_from('<I', d, opt + 96)[0]
        self.sections = []
        for i in range(nsec):
            o = opt + opt_size + 40 * i
            name = d[o:o + 8].rstrip(b'\0').decode('ascii')
            vsize, va, rsize, rptr = struct.unpack_from('<IIII', d, o + 8)
            ch = struct.unpack_from('<I', d, o + 36)[0]
            self.sections.append((name, self.base + va, max(vsize, rsize), rsize, rptr, ch))
        self.exports = self._exports(export_rva)

    def section(self, name):
        for s in self.sections:
            if s[0] == name:
                return s
        raise Error(f'the image has no {name} section')

    def read(self, va, n):
        for _, start, vsize, rsize, rptr, _ in self.sections:
            if start <= va and va + n <= start + vsize:
                o = va - start
                out = bytearray(n)
                avail = max(0, min(n, rsize - o))
                out[:avail] = self.data[rptr + o:rptr + o + avail]
                return bytes(out)
        raise Error(f'0x{va:08x} + {n} is outside the sections')

    def u32(self, va):
        return struct.unpack('<I', self.read(va, 4))[0]

    def cstr(self, va):
        out = bytearray()
        while True:
            b = self.read(va + len(out), 1)[0]
            if b == 0:
                return out.decode('ascii')
            out.append(b)
            if len(out) > 4096:
                raise Error(f'string at 0x{va:08x} is not terminated')

    def _exports(self, rva):
        if rva == 0:
            raise Error('the image has no export directory')
        va = self.base + rva
        nfunc, nname, afunc, aname, aord = struct.unpack('<IIIII', self.read(va + 20, 20))
        out = {}
        for i in range(nname):
            name = self.cstr(self.base + self.u32(self.base + aname + 4 * i))
            idx = struct.unpack('<H', self.read(self.base + aord + 2 * i, 2))[0]
            if idx >= nfunc:
                raise Error(f'export {name}: bad ordinal')
            out[name] = self.base + self.u32(self.base + afunc + 4 * idx)
        return out


class Reader:
    """decode_value and the reference decoders of aot-runtime.c, which only
    skip the encoded data."""

    def __init__(self, img, va):
        self.img = img
        self.p = va

    def value(self):
        b = self.img.read(self.p, 1)[0]
        if b & 0x80 == 0:
            self.p += 1
            return b
        if b & 0x40 == 0:
            v = ((b & 0x3F) << 8) | self.img.read(self.p + 1, 1)[0]
            self.p += 2
            return v
        if b != 0xFF:
            x = self.img.read(self.p, 4)
            self.p += 4
            return ((x[0] & 0x1F) << 24) | (x[1] << 16) | (x[2] << 8) | x[3]
        x = self.img.read(self.p + 1, 4)
        self.p += 5
        return (x[0] << 24) | (x[1] << 16) | (x[2] << 8) | x[3]

    def klass_ref(self, depth=0):
        if depth > 64:
            raise Error(f'class reference at 0x{self.p:08x} is nested too deeply')
        token = self.value()
        if token == 0:
            return
        table = token >> 24
        if table == 0:
            self.value()                                    # image index
        elif table == TABLE_TYPESPEC:
            if token == TOKEN_TYPE_SPEC:
                kind = self.value()
                if kind == TYPE_GENERICINST:
                    self.klass_ref(depth + 1)
                    self.generic_inst(depth + 1)
                elif kind in (TYPE_VAR, TYPE_MVAR):
                    self.value()                            # parameter number
                    if self.value():                        # is_method
                        self.method_ref(depth + 1)
                    else:
                        self.klass_ref(depth + 1)
                else:
                    raise Error(f'type specification kind 0x{kind:x} at 0x{self.p:08x} is not known')
            else:
                self.value()                                # image index
        elif token == TOKEN_TYPE_DEF:                       # array
            self.value()                                    # image index
            self.value()                                    # rank
            self.klass_ref(depth + 1)
        else:
            raise Error(f'class reference token 0x{token:08x} at 0x{self.p:08x} is not known')

    def generic_inst(self, depth):
        for _ in range(self.value()):
            self.klass_ref(depth + 1)

    def method_ref(self, depth):
        value = self.value()
        index = value >> 24
        if index == METHODREF_NO_AOT_TRAMPOLINE:
            value = self.value()
            index = value >> 24
        if index in (METHODREF_WRAPPER, METHODREF_WRAPPER_NAME):
            # Wrapper references do not occur in class references.
            raise Error(f'wrapper method reference at 0x{self.p:08x} in a class reference')
        if index == METHODREF_METHODSPEC:
            self.value()
            self.value()
        elif index == METHODREF_GINST:
            self.klass_ref(depth + 1)
            self.value()
            self.value()
            if self.value():
                self.generic_inst(depth + 1)
            if self.value():
                self.generic_inst(depth + 1)
        elif index == METHODREF_ARRAY:
            self.klass_ref(depth + 1)
            self.value()
        elif index >= METHODREF_MIN:
            raise Error(f'method reference kind {index} at 0x{self.p:08x} is not known')


def instruction_starts(dll, lo, hi):
    """Instruction start addresses in [lo, hi) from a linear disassembly."""
    out = subprocess.run([LLVM_OBJDUMP, '-d', '--no-show-raw-insn', f'--start-address=0x{lo:x}',
                          f'--stop-address=0x{hi:x}', dll], capture_output=True, text=True, check=False)
    if out.returncode != 0:
        raise Error(f'llvm-objdump failed: {out.stderr.strip()}')
    starts = set()
    bad = 0
    line_re = re.compile(r'^\s*([0-9a-f]+):\s*(\S.*)$')
    for line in out.stdout.splitlines():
        m = line_re.match(line)
        if not m:
            continue
        if m.group(2).startswith('<unknown>'):
            bad += 1
        starts.add(int(m.group(1), 16))
    return starts, bad


def main() -> int:
    if len(sys.argv) != 4:
        sys.stderr.write(__doc__)
        return 1
    dll, seeds_path, report_path = sys.argv[1:]
    img = Image(dll)
    text = img.section('.text')
    text_lo, text_hi = text[1], text[1] + text[2]

    syms = img.exports
    for name in ('mono_aot_version', 'mono_aot_full_aot', 'mono_aot_opt_flags', 'mono_aot_file_info',
                 'methods', 'methods_end', 'method_addresses', 'method_info', 'method_info_offsets',
                 'ex_info', 'ex_info_offsets', 'got_info', 'got_info_offsets', 'plt', 'plt_end'):
        if name not in syms:
            raise Error(f'the image does not export {name}')

    if img.cstr(syms['mono_aot_version']) != AOT_VERSION:
        raise Error(f'AOT format version {img.cstr(syms["mono_aot_version"])} is not {AOT_VERSION}')
    if img.cstr(syms['mono_aot_full_aot']) != 'TRUE':
        raise Error('the image is not a full-AOT image')
    opts = int(img.cstr(syms['mono_aot_opt_flags']))
    fi = struct.unpack('<13I', img.read(syms['mono_aot_file_info'], 52))
    plt_got_base, got_size, plt_size, nmethods = fi[0:4]
    num_tramp, tramp_size = fi[4:7], fi[10:13]

    entries = {}                                            # address -> kind

    def add(addr, kind):
        if not text_lo <= addr < text_hi:
            raise Error(f'{kind} entry 0x{addr:08x} is outside .text')
        entries.setdefault(addr, kind)

    counts = {}

    def count(kind, n=1):
        counts[kind] = counts.get(kind, 0) + n

    for name, value in sorted(syms.items()):
        if name in CODE_NAMES or any(r.match(name) for r in CODE_PATTERNS):
            add(value, 'named')
            count('named code')
        elif name in ARRAY_NAMES:
            t = ARRAY_NAMES.index(name)
            if num_tramp[t] == 0 or tramp_size[t] == 0:
                raise Error(f'{name}: no entries')
            for i in range(num_tramp[t]):
                add(value + i * tramp_size[t], name)
            count(name, num_tramp[t])
        elif name in DATA_NAMES or any(r.match(name) for r in DATA_PATTERNS):
            pass
        else:
            raise Error(f'export {name} is not known as code or data')

    plt, plt_end = syms['plt'], syms['plt_end']
    if plt_end - plt != plt_size * PLT_ENTRY_SIZE:
        raise Error(f'PLT size {plt_end - plt} is not {plt_size} * {PLT_ENTRY_SIZE}')
    for i in range(plt_size):
        e = plt + i * PLT_ENTRY_SIZE
        if img.read(e, 2) != b'\xff\xa3':
            raise Error(f'PLT entry {i} at 0x{e:08x} is not jmp *disp32(%ebx)')
        add(e, 'plt')
    count('PLT entries', plt_size)

    methods_lo, methods_hi = syms['methods'], syms['methods_end']
    starts, unknown = instruction_starts(dll, methods_lo, methods_hi)
    if unknown:
        raise Error(f'the method area has {unknown} bytes that are not instructions')

    def check_insn(addr, what):
        if addr not in starts:
            raise Error(f'{what} 0x{addr:08x} is not at an instruction start')

    ma, ma_off, mi, mi_off = syms['method_addresses'], syms['ex_info_offsets'], syms['method_info'], syms['method_info_offsets']
    ex = syms['ex_info']
    got_info, got_info_off = syms['got_info'], syms['got_info_offsets']
    got_slots = got_size // 4
    switch_slots = set()
    for i in range(nmethods):
        code = img.u32(ma + 4 * i)
        if code == 0:
            count('methods without code')
            continue
        if not methods_lo <= code < methods_hi:
            raise Error(f'method {i}: code 0x{code:08x} is outside the method area')
        check_insn(code, f'method {i} start')
        add(code, 'method')
        count('methods')

        r = Reader(img, ex + img.u32(ma_off + 4 * i))
        code_len = r.value()
        flags = r.value()
        r.value()                                           # unwind info offset or used registers
        if code + code_len > methods_hi:
            raise Error(f'method {i}: code 0x{code:08x} + {code_len} is outside the method area')

        def code_entry(off, what):
            if not 0 <= off < code_len:
                raise Error(f'method {i}: {what} offset {off} is outside the code ({code_len} bytes)')
            check_insn(code + off, f'method {i} {what}')
            add(code + off, what)
            count(what)

        if flags & 4:
            for _ in range(r.value()):
                cflags = r.value()
                r.value()                                   # exvar_offset
                if cflags == CLAUSE_FILTER:
                    code_entry(r.value(), 'filter')
                elif r.value():
                    r.klass_ref()
                r.value()                                   # try_start
                r.value()                                   # try_end
                code_entry(r.value(), 'handler')

        r = Reader(img, mi + img.u32(mi_off + 4 * i))
        r.klass_ref()
        if opts & OPT_SHARED:
            for _ in range(r.value()):
                r.value()
        for _ in range(r.value()):
            slot = r.value()
            if slot >= got_slots:
                raise Error(f'method {i}: GOT slot {slot} is outside the GOT ({got_slots} slots)')
            g = Reader(img, got_info + img.u32(got_info_off + 4 * slot))
            ptype = g.value()
            if ptype in PATCH_CODE_RELATIVE:
                raise Error(f'method {i}: GOT slot {slot} has patch type {PATCH_CODE_RELATIVE[ptype]}, '
                            'a code offset that this script does not decode')
            if ptype != PATCH_SWITCH:
                continue
            if slot in switch_slots:
                raise Error(f'method {i}: switch GOT slot {slot} is shared with another method')
            switch_slots.add(slot)
            count('switch tables')
            for _ in range(g.value()):
                code_entry(g.value(), 'switch target')

    seeds = sorted(entries)
    with open(seeds_path + '.tmp', 'w', encoding='ascii') as f:
        for a in seeds:
            f.write(f'loc_{a:08X}\n')
    os.replace(seeds_path + '.tmp', seeds_path)
    with open(report_path + '.tmp', 'w', encoding='ascii') as f:
        # The .text end is exclusive, as the code-end argument of the SR tools.
        f.write(f'image base 0x{img.base:08x}, .text 0x{text_lo:08x}-0x{text_hi:08x}\n')
        f.write(f'method area 0x{methods_lo:08x}-0x{methods_hi - 1:08x}, {len(starts)} instructions\n')
        for k in sorted(counts):
            f.write(f'{k}: {counts[k]}\n')
        f.write(f'entry points: {len(seeds)}\n')
    os.replace(report_path + '.tmp', report_path)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Error as e:
        sys.stderr.write(f'error: {e}\n')
        sys.exit(1)
