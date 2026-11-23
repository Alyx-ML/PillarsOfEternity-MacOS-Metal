#!/bin/sh
# Translate one PE module to LLVM IR.
#
# Usage: translate_module.sh <work-dir-name> <pe-file> <sha256> <code-start> <code-end>
#   work-dir-name: directory under work/ (for example "engine")
#   pe-file: file name of the PE module in the work directory
#   sha256: expected SHA-256 of the PE module
#   code-start, code-end: virtual address range of the code section (hex)
# The output is work/<work-dir-name>/<work-dir-name>.ll.
#
# The script repeats these steps until no new information is found:
#   1. SRW translates the DLL to llasm.
#   2. fix_region_flags.py adds instruction_flags.sci entries for flag
#      values that are live across region starts.
#   3. find_code_seeds.py adds code addresses that only data references.
# Then it checks the flag entries against the SRW control flow and runs
# SRW one more time with --preserve_addresses. That output keeps the
# original x86 addresses (the runtime maps the PE image at its original
# base) and has a code map, <work-dir-name>_code_map, from original code
# addresses to translated procedures. The script generates the import
# declarations for that output, checks the code map and runs llasm.
# The analysis passes use the label output of SRW, because
# find_code_seeds.py reads code addresses from its label operands and data
# segments.
# The llasm options are the upstream SR options for arm64-darwin with
# offset pointers (games/Septerra Core/SR-Septerra/SConstruct).
#
# The work directory must contain the PE file (or a link to it) and the
# .sci files.
#
# Environment: CODE_SEEDS=fixed skips step 3, and SRW runs with
# --no_export_entries (exports are labels, not code entry points) and
# --int3_noreturn (int3 ends the control flow: Mono AOT code puts int3
# after calls that do not return, and data can follow it). Then
# code_entry_points.sci must list all code entry points (SRW starts a
# procedure at each address of the list).
# tools/build_aot_image.sh writes this list for a Mono AOT image, which has
# data (strings, method information) in its code section: step 3 would
# take data addresses as code.
#
# Environment: IMPORTS=list lets the module have imports that have no entry
# in runtime/imports.spec. gen_extern_llinc.py then writes the names of
# these imports to missing_imports.txt in the work directory. The link of a
# host with the module stops with an undefined symbol for each of these
# imports until the runtime implements it. The default, IMPORTS=check,
# stops the script at a missing entry.
set -eu

PORT_DIR=$(cd "$(dirname "$0")/.." && pwd)
if [ $# -ne 5 ]; then
    echo "usage: translate_module.sh <work-dir-name> <pe-file> <sha256> <code-start> <code-end>" >&2
    exit 2
fi
NAME=$1
PE=$2
EXPECTED_SHA256=$3
CODE_START=$4
CODE_END=$5
WORK="$PORT_DIR/work/$NAME"
SRW="$PORT_DIR/build/srw/SRW"
LLASM="$PORT_DIR/build/llasm/llasm"
TOOLS="$PORT_DIR/tools"
PY="python3 -I"
MAX_PASSES=32
CODE_SEEDS=${CODE_SEEDS:-scan}
case "$CODE_SEEDS" in
    scan|fixed) ;;
    *) echo "error: CODE_SEEDS must be scan or fixed, not $CODE_SEEDS" >&2; exit 2 ;;
esac
SRW_OPTS=
if [ "$CODE_SEEDS" = fixed ]; then SRW_OPTS="--no_export_entries --int3_noreturn"; fi
IMPORTS=${IMPORTS:-check}
case "$IMPORTS" in
    check) IMPORT_OPTS= ;;
    list) IMPORT_OPTS=--missing=missing_imports.txt ;;
    *) echo "error: IMPORTS must be check or list, not $IMPORTS" >&2; exit 2 ;;
esac

cd "$WORK"

actual=$(shasum -a 256 $PE | cut -d' ' -f1)
if [ "$actual" != "$EXPECTED_SHA256" ]; then
    echo "error: $PE has SHA-256 $actual, expected $EXPECTED_SHA256" >&2
    exit 1
fi

pass=1
while [ $pass -le $MAX_PASSES ]; do
    echo "PROGRESS pass $pass: SRW"
    srw_rc=0
    "$SRW" $SRW_OPTS $PE $NAME.llasm > out.log 2> err.log || srw_rc=$?
    changed=0
    if [ $srw_rc -ne 0 ]; then
        if grep -q '^Error: unknown flags on start of region' err.log; then
            fix_rc=0
            $PY "$TOOLS/fix_region_flags.py" $PE err.log instruction_flags.sci disasm.cache || fix_rc=$?
            if [ $fix_rc -eq 0 ]; then changed=1; else
                echo "error: fix_region_flags.py failed with status $fix_rc" >&2; exit 1; fi
        else
            echo "error: SRW failed with status $srw_rc:" >&2
            grep '^Error' err.log | head -20 >&2
            exit 1
        fi
    elif [ "$CODE_SEEDS" = scan ]; then
        seed_rc=0
        $PY "$TOOLS/find_code_seeds.py" seg01_code.llinc fixup_interpret_as_code.sci $CODE_START $CODE_END \
            $NAME.llasm seg*_data.llinc || seed_rc=$?
        if [ $seed_rc -eq 0 ]; then changed=1
        elif [ $seed_rc -ne 1 ]; then echo "error: find_code_seeds.py failed with status $seed_rc" >&2; exit 1; fi
    fi
    if [ $changed -eq 0 ]; then break; fi
    pass=$((pass + 1))
done
if [ $pass -gt $MAX_PASSES ]; then
    echo "error: no convergence after $MAX_PASSES passes" >&2
    exit 1
fi

echo "PROGRESS verify flag entries"
if [ -f reviewed_flag_edges.sci ]; then
    $PY "$TOOLS/verify_region_flags.py" seg01_code.llinc instruction_flags.sci disasm.cache reviewed_flag_edges.sci 2> verify.log
else
    $PY "$TOOLS/verify_region_flags.py" seg01_code.llinc instruction_flags.sci disasm.cache 2> verify.log
fi

echo "PROGRESS address-preserving SRW pass"
"$SRW" $SRW_OPTS --preserve_addresses=${NAME}_code_map $PE $NAME.llasm > out.log 2> err.log || {
    echo "error: address-preserving SRW pass failed:" >&2
    grep '^Error' err.log | head -20 >&2
    exit 1
}
# The seg*_data.llinc files of the analysis passes stay in the work
# directory; the address-preserving output does not include them.

echo "PROGRESS declare imports"
$PY "$TOOLS/gen_extern_llinc.py" $IMPORT_OPTS "$PORT_DIR/runtime/imports.spec" $NAME.llasm extern.llinc

echo "PROGRESS verify code map"
$PY "$TOOLS/verify_code_map.py" $NAME.llasm ${NAME}_code_map $CODE_START $CODE_END \
    instruction_replacements.sci seg*_code.llinc

echo "PROGRESS llasm"
"$LLASM" -m64 -inline-idiv -inline-float -pic -ptrofs -O -I "$PORT_DIR/third_party/SR/SR/llasm-support" -I "$PORT_DIR/runtime/llasm" -I . \
    $NAME.llasm -o $NAME.ll > llasm.out 2> llasm.err || {
    echo "error: llasm failed:" >&2
    grep -v '^Replacing' llasm.err | head -20 >&2
    exit 1
}
echo "PROGRESS done: $(ls -l $NAME.ll | awk '{print $5}') bytes of LLVM IR"
