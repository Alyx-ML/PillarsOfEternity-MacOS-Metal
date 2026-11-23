#!/usr/bin/env python3
"""Check that the llasm image data segment keeps the PE data layout.

SRW writes all data sections of a module into one llasm data segment,
"datasegment image_data" (see SR_output.c, SR_write_llasm_image_data).
Code can address data through a base outside the object that it reads, so
the distance between two data addresses must be the same as in the PE
image.

This tool reads the segment from <module>.llasm and its include files,
counts the bytes of each item (db: 1, "db V dup N": N, daddr: 4,
dalign A: padding to a multiple of A) and compares the offset of each
"dlabel loc_XXXXXXXX" with XXXXXXXX minus the address of the first label
minus that label's offset.

Usage: verify_data_layout.py <module>.llasm
Exit status: 0 when all labels are at their PE distance, 1 when a label is
not, 2 for a usage or parse error.
"""
import os
import re
import sys

LABEL_RE = re.compile(r'^dlabel\s+loc_([0-9A-F]{8})\b')
DATA_INCLUDE_RE = re.compile(r'seg[0-9]+_data\.llinc')


def items(path, base_dir):
    with open(path, encoding='latin-1') as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith(';'):
                continue
            name = line.split(None, 1)[1] if line.startswith('include ') else None
            if name is not None and DATA_INCLUDE_RE.fullmatch(name):
                yield from items(os.path.join(base_dir, name), base_dir)
            else:
                yield path, n, line


def main() -> int:
    if len(sys.argv) != 2:
        sys.stderr.write(__doc__)
        return 2
    main_path = sys.argv[1]
    base_dir = os.path.dirname(main_path) or '.'

    in_seg = False
    found = False
    offset = 0
    base = None
    labels = 0
    failures = 0
    for path, n, line in items(main_path, base_dir):
        word = line.split(None, 1)[0]
        if word == 'datasegment':
            in_seg = line.split()[1:2] == ['image_data']
            found = found or in_seg
            continue
        if not in_seg:
            continue
        if word == 'endd':
            in_seg = False
            continue
        params = line.split()[1:]
        if word == 'db':
            if len(params) == 1:
                offset += 1
            elif len(params) == 3 and params[1] == 'dup':
                offset += int(params[2], 0)
            else:
                sys.stderr.write(f'{path}:{n}: unexpected db form: {line}\n')
                return 2
        elif word == 'daddr':
            offset += 4
        elif word == 'dalign':
            a = int(params[0], 0)
            offset += (-offset) % a
        elif word == 'dlabel':
            m = LABEL_RE.match(line)
            if not m:
                continue  # alias labels: same position as the loc_ label next to them
            addr = int(m.group(1), 16)
            labels += 1
            if base is None:
                base = addr - offset
            elif addr - base != offset:
                failures += 1
                if failures <= 20:
                    sys.stderr.write(f'FAIL {path}:{n}: loc_{addr:08X} at segment offset 0x{offset:x}, '
                                     f'expected 0x{addr - base:x}\n')
        else:
            sys.stderr.write(f'{path}:{n}: unexpected item in data segment: {line}\n')
            return 2
    if not found:
        sys.stderr.write(f'{main_path}: no "datasegment image_data"\n')
        return 2
    print(f'image_data: base 0x{base:08x}, size 0x{offset:x}, {labels} labels checked, {failures} failures')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
