#!/bin/sh
# Build build/aot/mono-aot: the translated mono.dll with the Win32 runtime
# and a command-line host that runs Mono (the AOT compiler, or managed code
# from translated AOT images).
# With --player, build build/player/PillarsOfEternity: the translated game
# program (work/player), mono.dll and the AOT images with the runtime and
# the player host (runtime/host/player_main.c).
#
# Usage: build_aot_host.sh [--player] [<AOT work-dir-name>...]
#   Each AOT work-dir-name is a directory under work/ that
#   tools/build_aot_image.sh, tools/translate_module.sh and
#   tools/compile_module.sh completed (for example "mscorlib_aot"). The host
#   links the translated code of each AOT image and can map the image with
#   --aot-image (runtime/host/aot_main.c).
#
# Prerequisites:
#   1. tools/translate_module.sh and tools/compile_module.sh completed for
#      mono (work/mono/parts/*.o), and for player with --player
#      (work/player/parts/*.o) and for shroud with --player
#      (work/shroud/parts/*.o, ShroudUnityPlugin.dll).
#   2. LLVM from Homebrew (/opt/homebrew/opt/llvm).
#
# Steps:
#   1. tools/gen_thunks.py makes the import thunks, rt_imports.h and the
#      import table from runtime/imports.spec. tools/gen_com_thunks.py makes
#      the COM method thunks and vtables from runtime/com.spec.
#   2. Compile each runtime C and Objective-C file and the host main file
#      (aot_main.c, or player_main.c with --player) with -Werror.
#   3. Compile the SR llasm support helpers.
#   4. Translate the runtime llasm files (import thunks, guest calls, COM
#      thunks) with tools/build_llasm_rt.sh.
#   5. Write the table of AOT images (build/aot/images/aot_images.c, see
#      runtime/host/aot_images.h) from image.name and image.sha256 of each
#      AOT work directory, and compile it.
#   6. Combine the parts of each translated module (mono and each AOT
#      image) into one relocatable object (build/aot/modules/<name>.o).
#      llvm-split (tools/compile_module.sh) changes the private symbols of
#      a module to hidden external symbols, so that the parts can call each
#      other. llasm gives the same names to helpers of each module
#      (__ctor, initialize_pointers, ptr_prev_init_pointers,
#      __return_procedure). The relocatable link changes the hidden
#      symbols back to local symbols, so that the modules link together.
#      SRW also gives each export of an AOT image a global procedure name
#      (for example "plt" in each image). The runtime finds AOT code only
#      with the code map, so the link of an AOT module (and of the player
#      module) keeps only <name>_code_map and the shared
#      ptr_initialize_pointers as external symbols.
#   7. Link the runtime, the host and the module objects into one
#      executable. The layout of the guest window (guest address =
#      host address - 0xFFE00000 before the slide, see runtime/src/rt_mem.c):
#        0x00200000  __TEXT (the header, all code, constants),
#                    __DATA_CONST, __DATA. The linker stops with an error if
#                    they overlap __GUEST. With mscorlib and the 27 game
#                    assemblies, they end at 0x0D85C000.
#        0x0FF00000  __GUEST (runtime/host/guest_space.s): zero-fill guest
#                    space for mono.dll (0x10000000) and the AOT images
#                    (from 0x20000000), up to 0x30000000.
#        0x30000000  __LINKEDIT.
#      Thus all of the program image and the fixed-base guest images are in
#      the window, and host memory that is made before rt_mem_init is above
#      the image.
#      With --player, the code below __GUEST is too large for this layout
#      with all of __DATA there too (the player adds 43 MB of code and
#      9.4 MB of data). The link moves the sections __DATA,__data (the data
#      of the translated modules and of the runtime), __DATA,__bss and
#      __DATA_CONST,__const (28 MB of constant data of the translated
#      modules on 2026-11-15) to the segment __XDATA at 0x30000000, directly
#      after __GUEST. The other sections of __DATA_CONST (__got and the
#      Objective-C sections, which dyld and the Objective-C runtime find by
#      their segment names) stay below __GUEST. The code
#      reaches the data with ADRP (a range of 4 GiB). The program file is
#      in __GUEST at 0x26000000. guest_space.o is first on the link line, so that __GUEST
#      comes before __LINKEDIT, and -segaddr sets its address. All code
#      stays in __TEXT: the linker adds branch islands only inside one
#      segment (a test link with the code in a different segment failed
#      with "B/BL out of range").
#
#   8. With --player, copy the AOT image DLLs to build/player/aot, where
#      the player host finds them. If an AOT work directory has the file
#      assembly.name, also copy the assembly that it names (the AOT instance
#      assembly of tools/build_aot_instances.sh).
#
# WARNING: the executables contain the translated code of mono.dll, of the
# AOT images and (with --player) of the game program. Do not distribute
# them, upload them or commit them.
set -eu

PORT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
LLVM=/opt/homebrew/opt/llvm/bin
TRIPLE=arm64-apple-macosx14.0.0
PLAYER=0
if [ "${1:-}" = --player ]; then
    PLAYER=1
    shift
fi
if [ $PLAYER -eq 1 ]; then
    ROOT="$PORT_DIR/build/player"
    OUT="$ROOT/PillarsOfEternity"
    HOST_MAIN=player_main
else
    ROOT="$PORT_DIR/build/aot"
    OUT="$ROOT/mono-aot"
    HOST_MAIN=aot_main
fi
OBJ="$ROOT/obj"
IMAGES="$ROOT/images"
MODULES="$ROOT/modules"

# Compile one file. The port directory can contain spaces, so the include
# paths are separate quoted arguments.
compile() {
    "$LLVM/clang" -target $TRIPLE -O2 -g \
        -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion -Werror -DPTROFS_64BIT \
        -I"$PORT_DIR/runtime/include" -I"$PORT_DIR/third_party/SR/SR/llasm-support" "$@"
}

if ! ls "$PORT_DIR"/work/mono/parts/mono*.o > /dev/null 2>&1; then
    echo "error: work/mono/parts has no objects; run tools/compile_module.sh mono first" >&2
    exit 1
fi
if [ $PLAYER -eq 1 ] && ! ls "$PORT_DIR"/work/player/parts/player*.o > /dev/null 2>&1; then
    echo "error: work/player/parts has no objects; run tools/compile_module.sh player first" >&2
    exit 1
fi
for name in "$@"; do
    case "$name" in
        *[!A-Za-z0-9_]*|[0-9]*|"") echo "error: \"$name\" is not a valid work directory name (C identifier)" >&2; exit 1 ;;
    esac
    dir="$PORT_DIR/work/$name"
    for f in image.name image.sha256; do
        if [ ! -s "$dir/$f" ]; then
            echo "error: $dir/$f does not exist; run tools/build_aot_image.sh $name first" >&2
            exit 1
        fi
    done
    if ! ls "$dir/parts/$name"*.o > /dev/null 2>&1; then
        echo "error: $dir/parts has no objects; run tools/compile_module.sh $name first" >&2
        exit 1
    fi
    if ! grep -q -x '[0-9a-f]\{64\}' "$dir/image.sha256"; then
        echo "error: $dir/image.sha256 is not a SHA-256 value" >&2
        exit 1
    fi
    if ! grep -q -x '[A-Za-z0-9_.-]*\.dll' "$dir/image.name"; then
        echo "error: $dir/image.name is not a DLL file name" >&2
        exit 1
    fi
    if [ -e "$dir/assembly.name" ]; then
        if ! grep -q -x '[A-Za-z0-9_.-]*\.dll' "$dir/assembly.name" || [ ! -s "$dir/$(cat "$dir/assembly.name")" ]; then
            echo "error: $dir/assembly.name does not name an assembly in $dir" >&2
            exit 1
        fi
    fi
done

echo "PROGRESS generate the import thunks"
python3 -I "$PORT_DIR/tools/gen_thunks.py" "$PORT_DIR/runtime/imports.spec" "$PORT_DIR/runtime/llasm/thunks.llasm" \
    "$PORT_DIR/runtime/include/rt_imports.h" "$PORT_DIR/runtime/src/rt_import_table.c"
python3 -I "$PORT_DIR/tools/gen_com_thunks.py" "$PORT_DIR/runtime/com.spec" "$PORT_DIR/runtime/llasm/com_thunks.llasm" \
    "$PORT_DIR/runtime/include/rt_com_thunks.h" "$PORT_DIR/runtime/src/rt_com_vtables.c"

rm -rf "$OBJ"
mkdir -p "$OBJ"

echo "PROGRESS compile the runtime"
for src in "$PORT_DIR"/runtime/src/*.c "$PORT_DIR/runtime/host/$HOST_MAIN.c"; do
    name="$(basename "$src" .c)"
    compile -std=c2x -c "$src" -o "$OBJ/$name.o"
done
for src in "$PORT_DIR"/runtime/src/*.m; do
    name="$(basename "$src" .m)"
    compile -fobjc-arc -c "$src" -o "$OBJ/$name.o"
done
# Helpers that the translated code calls (x87, SSE, string instructions,
# 64-bit division, fs access): the C files of the SR llasm support. The
# port I/O helpers (llasm_inout.c) are not used: mono.dll has no in or out
# instruction.
for src in "$PORT_DIR"/third_party/SR/SR/llasm-support/*.c; do
    name="$(basename "$src" .c)"
    [ "$name" = llasm_inout ] && continue
    compile -std=c2x -c "$src" -o "$OBJ/support_$name.o"
done

echo "PROGRESS translate the runtime llasm files"
sh "$PORT_DIR/tools/build_llasm_rt.sh" thunks calls com_thunks
for name in thunks calls com_thunks; do
    cp "$PORT_DIR/build/rt/$name.o" "$OBJ/llasm_$name.o"
done

"$LLVM/clang" -target $TRIPLE -c "$PORT_DIR/runtime/host/guest_space.s" -o "$OBJ/guest_space.o"

echo "PROGRESS write the table of AOT images"
rm -rf "$IMAGES"
mkdir -p "$IMAGES"
{
    echo "/* Generated by tools/build_aot_host.sh. Do not edit. */"
    echo "#include \"aot_images.h\""
    echo "#include <stddef.h>"
    for name in "$@"; do
        echo "extern const uint8_t ${name}_code_map[];"
    done
    if [ $# -eq 0 ]; then
        echo "const aot_image *const aot_images = NULL;"
    else
        echo "static const aot_image table[] = {"
        for name in "$@"; do
            echo "    { \"$(cat "$PORT_DIR/work/$name/image.name")\", \"$(cat "$PORT_DIR/work/$name/image.sha256")\", ${name}_code_map },"
        done
        echo "};"
        echo "const aot_image *const aot_images = table;"
    fi
    echo "const unsigned aot_image_count = $#;"
} > "$IMAGES/aot_images.c"
compile -std=c2x -I"$PORT_DIR/runtime/host" -c "$IMAGES/aot_images.c" -o "$IMAGES/aot_images.o"

echo "PROGRESS combine the parts of each translated module"
aot_names="$*"
module_names="mono $aot_names"
[ $PLAYER -eq 1 ] && module_names="$module_names player shroud"
rm -rf "$MODULES"
mkdir -p "$MODULES"
for name in $module_names; do
    parts="$PORT_DIR/work/$name/parts"
    set --
    while read -r p; do
        if [ ! -f "$parts/$p.o" ]; then
            echo "error: $parts/$p.o does not exist; run tools/compile_module.sh $name" >&2
            exit 1
        fi
        set -- "$@" "$parts/$p.o"
    done < "$parts/list"
    if [ $# -eq 0 ]; then
        echo "error: $parts/list is empty" >&2
        exit 1
    fi
    if [ "$name" = mono ]; then
        "$LLVM/clang" -target $TRIPLE -nostdlib -r "$@" -o "$MODULES/$name.o"
    else
        "$LLVM/clang" -target $TRIPLE -nostdlib -r -Wl,-exported_symbol,_${name}_code_map \
            -Wl,-exported_symbol,_ptr_initialize_pointers "$@" -o "$MODULES/$name.o"
    fi
done

echo "PROGRESS link $OUT"
# guest_space.o is first, so that __GUEST comes before __LINKEDIT.
set -- "$OBJ/guest_space.o" "$IMAGES/aot_images.o"
for o in "$OBJ"/*.o; do
    [ "$o" = "$OBJ/guest_space.o" ] && continue
    set -- "$@" "$o"
done
for name in $module_names; do
    set -- "$@" "$MODULES/$name.o"
done
if [ $PLAYER -eq 1 ]; then
    set -- "$@" -Wl,-rename_section,__DATA,__data,__XDATA,__data -Wl,-rename_section,__DATA,__bss,__XDATA,__bss \
        -Wl,-rename_section,__DATA_CONST,__const,__XDATA,__const -Wl,-segaddr,__XDATA,0x12FE00000
fi
"$LLVM/clang" -target $TRIPLE "$@" -Wl,-segaddr,__GUEST,0x10FD00000 \
    -framework CoreFoundation -framework CoreServices -framework Security -framework CFNetwork \
    -framework IOKit -framework AppKit -framework Carbon -framework Metal -framework QuartzCore \
    -framework CoreGraphics -framework CoreText -framework ImageIO -framework AudioToolbox -framework MetalFX \
    -framework CoreAudio -framework GameController -lresolv -o "$OUT"
codesign --force --sign - "$OUT"
if [ $PLAYER -eq 1 ]; then
    rm -rf "$ROOT/aot"
    mkdir -p "$ROOT/aot"
    for name in $aot_names; do
        cp "$PORT_DIR/work/$name/$(cat "$PORT_DIR/work/$name/image.name")" "$ROOT/aot/"
        if [ -e "$PORT_DIR/work/$name/assembly.name" ]; then
            cp "$PORT_DIR/work/$name/$(cat "$PORT_DIR/work/$name/assembly.name")" "$ROOT/aot/"
        fi
    done
fi
echo "PROGRESS done: $OUT"
