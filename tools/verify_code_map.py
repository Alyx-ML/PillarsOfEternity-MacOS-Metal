#!/usr/bin/env python3
"""Check the address-preserving llasm output of SRW (--preserve_addresses).

The runtime maps the PE image at its original base. The translated code
uses original addresses for data and for code addresses that it keeps as
values, and every indirect transfer goes through rt_code_target, which uses
the code map. This script checks:

  1. The code map ("datasegment <name>_seg") has the entry count in its
     first 4 bytes, and then that number of entries: 4 bytes of original
     address and one "daddr" label.
  2. The addresses increase strictly and are in the code range.
  3. The label of each entry is "loc_<address>" of that same address.
  4. Each procedure of the code files is in the map once, and each map
     entry is a procedure. A procedure name is "loc_X", or an export or
     alias name that a "define loc_X name" line in the main file connects
     to its address.
  5. No translated instruction uses a "loc_" label as a value (an operand
     other than the target of tcall, ctcallz or ctcallnz). A label value
     is the address of the translated procedure, not the original address.
     The only exception is a line that instruction_replacements.sci
     supplies (jump table replacements that select a direct target).

Usage: verify_code_map.py <main.llasm> <code map name> <code-start> <code-end>
                          <instruction_replacements.sci> <code.llinc>...
Exit status: 0 pass, 1 check failed, 2 usage or parse error.
"""
import re
import sys

PROC_RE = re.compile(r'^proc\s+(\S+)')
DEFINE_RE = re.compile(r'^define\s+loc_([0-9A-F]+)\s+(\S+)\s*$')
DB_RE = re.compile(r'^db\s+(\d+)\s*$')
DADDR_RE = re.compile(r'^daddr\s+(\S+)\s*$')
LABEL_RE = re.compile(r'\bloc_[0-9A-F]+\b')
TRANSFER_RE = re.compile(r'^(tcall\s+loc_[0-9A-F]+|ctcall(z|nz)\s+\S+,\s*loc_[0-9A-F]+)\s*$')


def fail(msg: str) -> None:
    sys.stderr.write(f'error: {msg}\n')


def read_code_map(path: str, name: str):
    """Return the list of (address, label) entries of the code map."""
    header = f'datasegment {name}_seg'
    with open(path, encoding='latin-1') as f:
        lines = [l.strip() for l in f]
    try:
        start = lines.index(header) + 1
    except ValueError:
        try:
            start = lines.index(header + ' constant') + 1
        except ValueError:
            raise SystemExit(f'error: {path} has no "{header}"') from None
    if lines[start] != f'dlabel {name} global':
        raise SystemExit(f'error: code map does not start with "dlabel {name} global"')
    pos = start + 1

    def byte4() -> int:
        nonlocal pos
        value = 0
        for i in range(4):
            m = DB_RE.match(lines[pos])
            if not m or int(m.group(1)) > 255:
                raise SystemExit(f'error: {path}: expected "db <byte>", found "{lines[pos]}"')
            value |= int(m.group(1)) << (8 * i)
            pos += 1
        return value

    count = byte4()
    entries = []
    for _ in range(count):
        address = byte4()
        m = DADDR_RE.match(lines[pos])
        if not m:
            raise SystemExit(f'error: {path}: expected "daddr <label>", found "{lines[pos]}"')
        entries.append((address, m.group(1)))
        pos += 1
    if lines[pos] != 'endd':
        raise SystemExit(f'error: code map has more data than its count {count}')
    return entries


def main() -> int:
    if len(sys.argv) < 7:
        sys.stderr.write(__doc__)
        return 2
    main_file, name = sys.argv[1], sys.argv[2]
    code_start, code_end = int(sys.argv[3], 16), int(sys.argv[4], 16)
    sci_file, code_files = sys.argv[5], sys.argv[6:]

    entries = read_code_map(main_file, name)

    aliases = {}
    with open(main_file, encoding='latin-1') as f:
        for line in f:
            m = DEFINE_RE.match(line)
            if m:
                aliases[m.group(2)] = int(m.group(1), 16)

    allowed = set()
    with open(sci_file, encoding='latin-1') as f:
        for line in f:
            for part in line.rstrip('\n').split('|')[1:]:
                allowed.add(part.strip())

    failures = 0
    previous = None
    for address, label in entries:
        if previous is not None and address <= previous:
            fail(f'code map address 0x{address:08X} is not above 0x{previous:08X}')
            failures += 1
        previous = address
        if not code_start <= address < code_end:
            fail(f'code map address 0x{address:08X} is outside the code range')
            failures += 1
        if label != f'loc_{address:X}':
            fail(f'code map entry 0x{address:08X} has label {label}')
            failures += 1
    mapped = {address for address, _ in entries}

    procs = set()
    value_uses = 0
    for path in code_files:
        with open(path, encoding='latin-1') as f:
            for number, raw in enumerate(f, 1):
                line = raw.strip()
                if not line or line.startswith(';'):
                    continue
                m = PROC_RE.match(line)
                if m:
                    proc = m.group(1)
                    if proc.startswith('loc_'):
                        address = int(proc[4:], 16)
                    elif proc in aliases:
                        address = aliases[proc]
                    else:
                        fail(f'{path}:{number}: procedure {proc} has no "define loc_X {proc}"')
                        failures += 1
                        continue
                    if address in procs:
                        fail(f'{path}:{number}: second procedure at 0x{address:08X}')
                        failures += 1
                    procs.add(address)
                    continue
                if LABEL_RE.search(line) and not TRANSFER_RE.match(line):
                    if line in allowed:
                        value_uses += 1
                    else:
                        fail(f'{path}:{number}: label used as a value: {line}')
                        failures += 1

    for address in sorted(procs - mapped):
        fail(f'procedure at 0x{address:08X} is not in the code map')
        failures += 1
    for address in sorted(mapped - procs):
        fail(f'code map entry 0x{address:08X} is not a procedure')
        failures += 1

    print(f'code map {name}: {len(entries)} entries, {len(procs)} procedures, '
          f'{value_uses} replacement label values, {failures} failures')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
