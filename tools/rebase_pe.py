#!/usr/bin/env python3
"""Write a copy of a 32-bit PE module that is rebased to a new image base.

Usage: rebase_pe.py <input> <sha256> <new-base> <output>

The script checks the SHA-256 of the input. It then adds (new base - old
base) to each 32-bit word that a base relocation of type HIGHLOW (3)
points to, and writes the new base in the optional header. Relocations of
type ABSOLUTE (0) are padding. Any other relocation type stops the script.
The relocation table stays the same (it has RVAs), so the copy can be
rebased again.

Use this script when the preferred base of a module is not free in the
guest window (see runtime/src/rt_mem.c). For example, the host program
image covers the base 0x00400000 of PillarsOfEternity.exe. SRW translates
the copy at the new base, and the runtime maps the copy at that base.
"""
import hashlib
import struct
import sys

IMAGE_REL_BASED_ABSOLUTE = 0
IMAGE_REL_BASED_HIGHLOW = 3
DIR_BASERELOC = 5


def fail(msg):
    sys.stderr.write("error: %s\n" % msg)
    sys.exit(1)


def main():
    if len(sys.argv) != 5:
        sys.stderr.write("usage: rebase_pe.py <input> <sha256> <new-base> <output>\n")
        sys.exit(2)
    src, want_sha, base_text, dst = sys.argv[1:]
    data = bytearray(open(src, "rb").read())
    sha = hashlib.sha256(data).hexdigest()
    if sha != want_sha.lower():
        fail("%s has SHA-256 %s, expected %s" % (src, sha, want_sha))
    new_base = int(base_text, 16)
    if new_base & 0xFFFF or not 0 < new_base < 0x80000000:
        fail("new base 0x%x is not a 64 KiB aligned address below 0x80000000" % new_base)

    if data[:2] != b"MZ":
        fail("no MZ header")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        fail("no PE header")
    nsect, = struct.unpack_from("<H", data, pe + 6)
    opt_size, = struct.unpack_from("<H", data, pe + 20)
    opt = pe + 24
    if struct.unpack_from("<H", data, opt)[0] != 0x10B:
        fail("not a PE32 (32-bit) module")
    old_base, = struct.unpack_from("<I", data, opt + 28)
    image_size, = struct.unpack_from("<I", data, opt + 56)
    ndirs, = struct.unpack_from("<I", data, opt + 92)
    if new_base + image_size > 0x80000000:
        fail("the image (0x%x bytes) does not fit below 0x80000000 at 0x%x" % (image_size, new_base))
    if ndirs <= DIR_BASERELOC:
        fail("no base relocation directory")
    reloc_rva, reloc_size = struct.unpack_from("<II", data, opt + 96 + 8 * DIR_BASERELOC)
    if reloc_rva == 0 or reloc_size == 0:
        fail("the module has no base relocations")

    sections = []
    for i in range(nsect):
        h = opt + opt_size + 40 * i
        vsize, va, rsize, rptr = struct.unpack_from("<IIII", data, h + 8)
        sections.append((va, max(vsize, rsize), rsize, rptr))

    def file_offset(rva, length):
        for va, span, rsize, rptr in sections:
            if va <= rva and rva + length <= va + span:
                if rva + length > va + rsize:
                    fail("RVA 0x%x is not in the file data of its section" % rva)
                return rptr + rva - va
        fail("RVA 0x%x is not in a section" % rva)

    delta = (new_base - old_base) & 0xFFFFFFFF
    pos = file_offset(reloc_rva, reloc_size)
    end = pos + reloc_size
    count = 0
    seen = set()
    while pos < end:
        if end - pos < 8:
            fail("truncated relocation block at file offset 0x%x" % pos)
        page, block = struct.unpack_from("<II", data, pos)
        if block < 8 or block & 1 or pos + block > end:
            fail("bad relocation block size %u at file offset 0x%x" % (block, pos))
        for k in range((block - 8) // 2):
            entry, = struct.unpack_from("<H", data, pos + 8 + 2 * k)
            kind, off = entry >> 12, entry & 0xFFF
            if kind == IMAGE_REL_BASED_ABSOLUTE:
                continue
            if kind != IMAGE_REL_BASED_HIGHLOW:
                fail("relocation type %u at RVA 0x%x is not supported" % (kind, page + off))
            rva = page + off
            if rva in seen:
                fail("two relocations at RVA 0x%x" % rva)
            seen.add(rva)
            fo = file_offset(rva, 4)
            v, = struct.unpack_from("<I", data, fo)
            struct.pack_into("<I", data, fo, (v + delta) & 0xFFFFFFFF)
            count += 1
        pos += block
    struct.pack_into("<I", data, opt + 28, new_base)
    open(dst, "wb").write(data)
    print("rebased %s from 0x%08x to 0x%08x: %u relocations, SHA-256 %s"
          % (src, old_base, new_base, count, hashlib.sha256(data).hexdigest()))


if __name__ == "__main__":
    main()
