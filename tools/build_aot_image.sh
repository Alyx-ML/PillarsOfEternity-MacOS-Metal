#!/bin/sh
# Make an x86 PE DLL from the assembler output of the Mono full-AOT
# compiler, and list its code entry points for SRW.
#
# Usage: build_aot_image.sh <work-dir-name> <aot.s> <image name> <image base>
#   work-dir-name: directory under work/ (for example "mscorlib_aot")
#   aot.s: output of mono-aot --aot=full,asmonly,asmwriter,nodebug (without
#          "static")
#   image name: file name that Mono loads, "<assembly file name>.dll" (for
#               example "mscorlib.dll.dll")
#   image base: preferred base address of the DLL (hex, 64 KiB aligned). The
#               runtime maps the image at this address.
#
# Steps:
#   1. build/tools/aot_asm (tools/aot_asm_main.c with
#      runtime/loader/rt_aot_asm.c, the function that the converter of the
#      release also uses) changes the assembler output to COFF syntax, sets
#      the compiler-heap addresses of the MarshalDirectiveException messages
#      to 0 and writes the export list (see runtime/loader/rt_aot_asm.h).
#      The other code and data bytes do not change.
#   2. The assembler of clang assembles the result for i686-pc-windows-msvc.
#   3. lld-link makes a DLL with no imports and no entry point. It exports
#      each global symbol of the output with its C name ("_methods" is
#      exported as "methods"), because Mono finds the tables of a DLL image
#      with GetProcAddress. Mono AOT code registers no SEH frame handlers,
#      so the DLL has no safe exception handler table (/safeseh:no).
#      /Brepro writes a fixed value in place of the time stamp, so that the
#      same input gives the same DLL (the converter of the release checks
#      the SHA-256 of each image).
#   4. tools/aot_code_seeds.py writes code_entry_points.sci (the code entry
#      points) and seeds.txt (counts). SRW starts a procedure at each entry
#      point. The script copies the list to fixup_interpret_as_code.sci, so
#      that SRW also identifies a relocation to one of these addresses (for
#      example in method_addresses) as a code address.
#   5. The script writes the SRW configuration files that do not exist yet,
#      an empty instruction_flags.sci,
#      image.sha256 (the SHA-256 of the DLL) and image.name (the image name).
#      tools/build_aot_host.sh reads these two files.
# Then translate the DLL with:
#   CODE_SEEDS=fixed tools/translate_module.sh <work-dir-name> <image name> \
#       <sha256> <text start> <text end>
# (the script prints this command).
#
# WARNING: the DLL contains code compiled from the game's assemblies. Do not
# distribute it, upload it or commit it.
set -eu

PORT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
LLVM=/opt/homebrew/opt/llvm/bin
LLD=/opt/homebrew/opt/lld/bin

if [ $# -ne 4 ]; then
    echo "usage: build_aot_image.sh <work-dir-name> <aot.s> <image name> <image base>" >&2
    exit 2
fi
NAME=$1
SRC=$2
IMAGE=$3
BASE=$4
WORK="$PORT_DIR/work/$NAME"

if [ ! -f "$SRC" ]; then
    echo "error: $SRC does not exist" >&2
    exit 1
fi
SRC="$(cd "$(dirname "$SRC")" && pwd)/$(basename "$SRC")"
case "$BASE" in
    0x*0000) ;;
    *) echo "error: the image base $BASE is not a 64 KiB aligned hex address" >&2; exit 1 ;;
esac
mkdir -p "$WORK"
cd "$WORK"

echo "PROGRESS convert to COFF syntax"
mkdir -p "$PORT_DIR/build/tools"
"$LLVM/clang" -target arm64-apple-macosx14.0.0 -O2 -std=c2x -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion \
    -Werror -I"$PORT_DIR/runtime/loader" "$PORT_DIR/runtime/loader/rt_aot_asm.c" "$PORT_DIR/tools/aot_asm_main.c" \
    -o "$PORT_DIR/build/tools/aot_asm.$$"
# Image builds can run in parallel: each links its own file, and the rename
# replaces the tool in one step.
mv -f "$PORT_DIR/build/tools/aot_asm.$$" "$PORT_DIR/build/tools/aot_asm"
"$PORT_DIR/build/tools/aot_asm" "$SRC" image.s exports.rsp

echo "PROGRESS assemble"
# The assembler of clang with the options of the clang driver for
# "-c -target i686-pc-windows-msvc" without the debug options (an assembler
# source without -g has no debug information). The converter of the
# release (runtime/loader/rt_convert.c) runs the same command.
"$LLVM/clang" -cc1as -triple i686-pc-windows-msvc19.33.0 -filetype obj -main-file-name image.s -target-cpu pentium4 \
    -mrelocation-model static -o image.obj image.s

echo "PROGRESS link $IMAGE"
rm -f exports.txt
"$LLD/lld-link" /dll /noentry /machine:x86 /safeseh:no /nodefaultlib /Brepro /base:$BASE \
    @exports.rsp /out:"$IMAGE" image.obj
rm -f "${IMAGE%.dll}.lib"

echo "PROGRESS code entry points"
python3 -I "$PORT_DIR/tools/aot_code_seeds.py" "$IMAGE" code_entry_points.sci seeds.txt
cp code_entry_points.sci fixup_interpret_as_code.sci

# SRW configuration: the code of Mono keeps esp and ebp 4-byte aligned.
[ -f SR.cfg ] || printf 'esp_dword_aligned=yes\nebp_dword_aligned=yes\n' > SR.cfg
for f in external_procedures.sci global_aliases.sci ignored_areas.sci instruction_replacements.sci; do
    [ -f $f ] || : > $f
done
# tools/translate_module.sh finds the instruction_flags.sci entries for the
# code of this DLL. Entries for an earlier DLL have addresses of other
# instructions, so the list starts empty for each new DLL.
: > instruction_flags.sci

shasum -a 256 "$IMAGE" | cut -d' ' -f1 > image.sha256
printf '%s\n' "$IMAGE" > image.name
text=$(sed -n 's/^image base [^,]*, \.text \(0x[0-9a-f]*\)-\(0x[0-9a-f]*\)$/\1 \2/p' seeds.txt)
if [ -z "$text" ]; then
    echo "error: seeds.txt has no .text range" >&2
    exit 1
fi
cat seeds.txt
echo "PROGRESS done. Translate with:"
echo "  CODE_SEEDS=fixed tools/translate_module.sh $NAME $IMAGE $(cat image.sha256) $text"
