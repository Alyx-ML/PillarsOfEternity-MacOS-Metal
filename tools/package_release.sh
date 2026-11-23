#!/bin/sh
# Make the release package build/release/PillarsOfEternity-Mac-ARM64.zip.
#
# The release package holds no game code. It holds the loader with the
# launcher window, the runtime (as a static archive and host objects) and
# the converter. On the first start, the converter translates the game
# files of the player (see runtime/loader/rt_convert.h).
#
# Prerequisites:
#   1. The development build is complete for mono, player, shroud and each
#      AOT module of tools/aot_modules.txt (tools/translate_module.sh,
#      tools/build_aot_image.sh). The SRW output files in work/<module> are
#      the tested translation; their SHA-256 values go into the manifest.
#   2. ldc2 (Homebrew: brew install ldc), for tools/build_toolchain.sh.
#   3. LLVM and lld from Homebrew (/opt/homebrew/opt/llvm, /opt/homebrew/opt/lld).
#   4. The game folder is the parent of the port folder (for the SHA-256
#      values of the managed assemblies).
#
# Result:
#   build/release/dist/Pillars of Eternity.app
#   build/release/PillarsOfEternity-Mac-ARM64.zip
#
# The script stops with an error if a check fails:
#   - the application holds a file with game code (.o other than the
#     runtime host objects, .ll, .llasm, .bc other than the SSE helper
#     bitcode, .exe, .dll other than the two assemblies of the port) or a
#     symbol of the translated code,
#   - a file holds a path of the build computer (its home folder or the
#     port folder),
#   - a program or library needs a macOS version newer than MIN_VERSION.
set -eu

PORT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
GAME_DIR="$(cd "$PORT_DIR/.." && pwd)"
LLVM_PREFIX=/opt/homebrew/opt/llvm
LLD_PREFIX=/opt/homebrew/opt/lld
LLVM="$LLVM_PREFIX/bin"
TRIPLE=arm64-apple-macosx14.0.0
OUT="$PORT_DIR/build/release"
DIST="$OUT/dist"
APP="$DIST/Pillars of Eternity.app"
REC="$APP/Contents/Resources/Recompiler"
OBJ="$OUT/obj"
HOSTOBJ="$REC/lib/objects"
MODULE_LIST="$PORT_DIR/tools/aot_modules.txt"
ZIP="$OUT/PillarsOfEternity-Mac-ARM64.zip"
# The minimum macOS version of the application. The converter tools of
# Homebrew (LLVM, lld) need macOS 27.2; tools/build_toolchain.sh builds SRW
# and llasm for the same version.
MIN_VERSION=27.2

die() {
    echo "error: $*" >&2
    exit 1
}

for f in "$LLVM/opt" "$LLVM/llvm-split" "$LLVM/llvm-link" "$LLVM/clang-23" "$LLVM/llvm-ar" "$LLVM/llvm-nm" \
    "$LLD_PREFIX/bin/lld" "$MODULE_LIST" "$PORT_DIR/work/aot/user/aot/PortAotInstances.dll" \
    "$PORT_DIR/work/aot/user/aot/PortGraphics.dll"; do
    [ -f "$f" ] || die "$f does not exist"
done
command -v ldc2 > /dev/null || die "ldc2 was not found; install LDC (brew install ldc)"

# The modules of the game program. Each line of tools/aot_modules.txt:
# <work directory> <assembly> <image base>.
AOT_NAMES="$(awk 'NF { print $1 }' "$MODULE_LIST")"
for m in mono player shroud $AOT_NAMES; do
    w="$PORT_DIR/work/$m"
    [ -f "$w/$m.llasm" ] || die "work/$m/$m.llasm does not exist; translate the module first"
    [ -s "$w/parts/list" ] || die "work/$m/parts/list does not exist; compile the module first"
done

rm -rf "$OUT"
mkdir -p "$OBJ" "$APP/Contents/MacOS" "$REC/bin" "$REC/lib" "$HOSTOBJ" "$REC/include/llasm-support" \
    "$REC/include/runtime" "$REC/modules" "$REC/managed" "$APP/Contents/Resources/Licenses"

# Compile one file. The port directory can contain spaces, so the paths
# are separate quoted arguments. -ffile-prefix-map removes the build
# directory from the objects.
compile() {
    "$LLVM/clang" -target $TRIPLE -O2 -ffile-prefix-map="$PORT_DIR"=/pillars-mac-port \
        -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion -Werror -DPTROFS_64BIT \
        -I"$PORT_DIR/runtime/include" -I"$PORT_DIR/third_party/SR/SR/llasm-support" "$@"
}

echo "PROGRESS build SRW and llasm"
sh "$PORT_DIR/tools/build_toolchain.sh" "$OUT/toolchain"

echo "PROGRESS compile the runtime archive"
python3 -I "$PORT_DIR/tools/gen_thunks.py" "$PORT_DIR/runtime/imports.spec" "$PORT_DIR/runtime/llasm/thunks.llasm" \
    "$PORT_DIR/runtime/include/rt_imports.h" "$PORT_DIR/runtime/src/rt_import_table.c"
python3 -I "$PORT_DIR/tools/gen_com_thunks.py" "$PORT_DIR/runtime/com.spec" "$PORT_DIR/runtime/llasm/com_thunks.llasm" \
    "$PORT_DIR/runtime/include/rt_com_thunks.h" "$PORT_DIR/runtime/src/rt_com_vtables.c"
for src in "$PORT_DIR"/runtime/src/*.c; do
    compile -std=c2x -c "$src" -o "$OBJ/$(basename "$src" .c).o"
done
for src in "$PORT_DIR"/runtime/src/*.m; do
    compile -fobjc-arc -c "$src" -o "$OBJ/$(basename "$src" .m).o"
done
for src in "$PORT_DIR"/third_party/SR/SR/llasm-support/*.c; do
    name="$(basename "$src" .c)"
    [ "$name" = llasm_inout ] && continue
    compile -std=c2x -c "$src" -o "$OBJ/support_$name.o"
done
sh "$PORT_DIR/tools/build_llasm_rt.sh" thunks calls com_thunks
for name in thunks calls com_thunks; do
    cp "$PORT_DIR/build/rt/$name.o" "$OBJ/llasm_$name.o"
done
"$LLVM/llvm-ar" rcs "$REC/lib/libpillars_rt.a" "$OBJ"/*.o

echo "PROGRESS compile the host objects"
compile -std=c2x -c "$PORT_DIR/runtime/host/aot_main.c" -o "$HOSTOBJ/aot_main.o"
compile -std=c2x -c "$PORT_DIR/runtime/host/player_main.c" -o "$HOSTOBJ/player_main.o"
"$LLVM/clang" -target $TRIPLE -c "$PORT_DIR/runtime/host/guest_space.s" -o "$HOSTOBJ/guest_space.o"
# The tables of AOT images (as tools/build_aot_host.sh writes them): none
# for the AOT compiler host, all modules for the game program.
{
    echo "/* Generated by tools/package_release.sh. Do not edit. */"
    echo "#include \"aot_images.h\""
    echo "#include <stddef.h>"
    echo "const aot_image *const aot_images = NULL;"
    echo "const unsigned aot_image_count = 0;"
} > "$OBJ/aot_images_aot.c"
{
    echo "/* Generated by tools/package_release.sh. Do not edit. */"
    echo "#include \"aot_images.h\""
    echo "#include <stddef.h>"
    for m in $AOT_NAMES; do echo "extern const uint8_t ${m}_code_map[];"; done
    echo "static const aot_image table[] = {"
    for m in $AOT_NAMES; do
        echo "    { \"$(cat "$PORT_DIR/work/$m/image.name")\", \"$(cat "$PORT_DIR/work/$m/image.sha256")\", ${m}_code_map },"
    done
    echo "};"
    echo "const aot_image *const aot_images = table;"
    echo "const unsigned aot_image_count = $(echo $AOT_NAMES | wc -w | tr -d ' ');"
} > "$OBJ/aot_images_player.c"
for t in aot player; do
    compile -std=c2x -I"$PORT_DIR/runtime/host" -c "$OBJ/aot_images_$t.c" -o "$HOSTOBJ/aot_images_$t.o"
done
cat > "$HOSTOBJ/player_info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleIdentifier</key>
    <string>local.pillars-mac-port.game</string>
    <key>CFBundleName</key>
    <string>Pillars of Eternity</string>
    <key>LSApplicationCategoryType</key>
    <string>public.app-category.role-playing-games</string>
    <key>LSSupportsGameMode</key>
    <true/>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
PLIST

echo "PROGRESS build the loader"
compile -std=c2x -c "$PORT_DIR/runtime/loader/rt_convert.c" -o "$OBJ/loader_rt_convert.o"
compile -std=c2x -c "$PORT_DIR/runtime/loader/rt_aot_asm.c" -o "$OBJ/loader_rt_aot_asm.o"
compile -fobjc-arc -c "$PORT_DIR/runtime/loader/rt_loader.m" -o "$OBJ/loader_rt_loader.o"
compile -fobjc-arc -c "$PORT_DIR/runtime/loader/rt_launcher.m" -o "$OBJ/loader_rt_launcher.o"
"$LLVM/clang" -target $TRIPLE -fobjc-arc "$OBJ/loader_rt_loader.o" "$OBJ/loader_rt_launcher.o" \
    "$OBJ/loader_rt_convert.o" "$OBJ/loader_rt_aot_asm.o" -framework Cocoa -framework Foundation \
    -o "$APP/Contents/MacOS/PillarsOfEternity"

# Copy the libraries that a Mach-O file needs (not the system libraries)
# into a directory, and refer to them with @rpath.
# Arguments: <mach-o file> <library directory>
copy_deps() {
    own_id="$(otool -D "$1" | tail -n +2)"
    otool -L "$1" | tail -n +2 | awk '{print $1}' | while read -r dep; do
        case "$dep" in
            /usr/lib/* | /System/*) continue ;;
        esac
        [ "$dep" = "$own_id" ] && continue
        base="$(basename "$dep")"
        if [ ! -f "$2/$base" ]; then
            src=""
            case "$dep" in
                @rpath/* | @loader_path/* | @executable_path/*)
                    for d in "$LLVM_PREFIX/lib" "$LLD_PREFIX/lib"; do
                        if [ -z "$src" ] && [ -f "$d/$base" ]; then src="$d/$base"; fi
                    done
                    ;;
                *) src="$dep" ;;
            esac
            [ -n "$src" ] && [ -f "$src" ] || die "the library $dep of $1 was not found"
            cp -L "$src" "$2/$base"
            chmod u+w "$2/$base"
            install_name_tool -id "@rpath/$base" "$2/$base"
            copy_deps "$2/$base" "$2"
        fi
        install_name_tool -change "$dep" "@rpath/$base" "$1"
    done
}

# Remove each LC_RPATH entry of a file and add one entry.
# Arguments: <mach-o file> <rpath>
set_rpath() {
    otool -l "$1" | awk '/cmd LC_RPATH/{r=1} r&&/ path /{print $2; r=0}' | while read -r p; do
        install_name_tool -delete_rpath "$p" "$1"
    done
    install_name_tool -add_rpath "$2" "$1"
}

echo "PROGRESS copy the converter tools"
cp "$OUT/toolchain/srw/SRW" "$REC/bin/SRW"
cp "$OUT/toolchain/llasm/llasm" "$REC/bin/llasm"
cp -L "$LLVM/opt" "$REC/bin/opt"
cp -L "$LLVM/llvm-split" "$REC/bin/llvm-split"
cp -L "$LLVM/llvm-link" "$REC/bin/llvm-link"
cp -L "$LLVM/clang-23" "$REC/bin/clang"
cp -L "$LLD_PREFIX/bin/lld" "$REC/bin/lld"
chmod u+w "$REC"/bin/*
for t in "$REC"/bin/*; do
    copy_deps "$t" "$REC/lib"
    set_rpath "$t" @loader_path/../lib
done
for l in "$REC"/lib/*.dylib; do
    set_rpath "$l" @loader_path
done
CLANG_RT="$("$LLVM/clang" -target $TRIPLE -print-file-name=lib/darwin/libclang_rt.osx.a)"
[ -f "$CLANG_RT" ] || die "libclang_rt.osx.a was not found"
cp "$CLANG_RT" "$REC/lib/libclang_rt.osx.a"
chmod u+w "$REC/lib/libclang_rt.osx.a"
cp "$PORT_DIR"/third_party/SR/SR/llasm-support/*.llinc "$REC/include/llasm-support/"
# The SSE helpers as bitcode: the converter links them into the parts so
# that clang can inline them (see tools/build_sse_inline.sh).
SSE_BC="$REC/include/llasm-support/sse_inline.bc"
sh "$PORT_DIR/tools/build_sse_inline.sh" "$SSE_BC"
cp "$PORT_DIR"/runtime/llasm/*.llinc "$REC/include/runtime/"
cp "$PORT_DIR/work/aot/user/aot/PortAotInstances.dll" "$PORT_DIR/work/aot/user/aot/PortGraphics.dll" "$REC/managed/"

echo "PROGRESS write the analysis files and the manifest"
MANIFEST="$REC/manifest.txt"
sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
parts() { wc -l < "$PORT_DIR/work/$1/parts/list" | tr -d ' '; }
{
    echo "# Pillars of Eternity Mac Port converter manifest. Generated by tools/package_release.sh."
    for f in PillarsOfEternity.exe PillarsOfEternity_data/Mono/mono.dll PillarsOfEternity_data/Plugins/ShroudUnityPlugin.dll; do
        echo "input $f $(sha "$GAME_DIR/$f")"
    done
    while read -r m asm base; do
        [ -n "$m" ] || continue
        case "$asm" in
            PortAotInstances | PortGraphics) ;;
            *) echo "input PillarsOfEternity_data/Managed/$asm.dll $(sha "$GAME_DIR/PillarsOfEternity_data/Managed/$asm.dll")" ;;
        esac
    done < "$MODULE_LIST"
    # The SRW inputs: mono.dll at its own base, the program and the plugin
    # rebased to the addresses of their translations (runtime/host/player_main.c).
    echo "pe mono PillarsOfEternity_data/Mono/mono.dll 0 $(sha "$PORT_DIR/work/mono/mono.dll") $(parts mono) all"
    echo "pe player PillarsOfEternity.exe 26000000 $(sha "$PORT_DIR/work/player/PillarsOfEternity.exe") $(parts player) codemap"
    echo "pe shroud PillarsOfEternity_data/Plugins/ShroudUnityPlugin.dll 11000000 $(sha "$PORT_DIR/work/shroud/ShroudUnityPlugin.dll") $(parts shroud) codemap"
    while read -r m asm base; do
        [ -n "$m" ] || continue
        case "$asm" in
            PortAotInstances | PortGraphics) src=port ;;
            *) src=managed ;;
        esac
        opts=-
        [ "$asm" = mscorlib ] && opts=ntrampolines=8192,nrgctx-trampolines=8192,nimt-trampolines=16384
        echo "aot $m $asm $base $src $opts $(parts "$m")"
        echo "image $m $(cat "$PORT_DIR/work/$m/image.sha256")"
    done < "$MODULE_LIST"
    # The output of the address-preserving SRW pass: <module>.llasm and the
    # segment files that it includes (the other include files come from
    # the converter).
    for m in mono player shroud $AOT_NAMES; do
        w="$PORT_DIR/work/$m"
        segs="$(sed -n 's/^include \(seg[0-9]*_[a-z]*\.llinc\)\r*$/\1/p' "$w/$m.llasm")"
        [ -n "$segs" ] || die "work/$m/$m.llasm includes no segment file"
        for f in $m.llasm $segs; do
            [ -f "$w/$f" ] || die "work/$m/$f does not exist"
            echo "pin $m $f $(sha "$w/$f")"
        done
    done
} > "$MANIFEST.body"
for m in mono player shroud $AOT_NAMES; do
    mkdir -p "$REC/modules/$m"
    for f in "$PORT_DIR/work/$m"/*.sci "$PORT_DIR/work/$m/SR.cfg" "$PORT_DIR/work/$m/extern.llinc"; do
        [ -f "$f" ] && cp "$f" "$REC/modules/$m/"
    done
    [ -f "$REC/modules/$m/extern.llinc" ] || die "work/$m/extern.llinc does not exist"
done

echo "PROGRESS sign the libraries and the tools"
for f in "$REC"/bin/* "$REC"/lib/*.dylib; do
    codesign --force --sign - "$f"
done
CONTENT="$(cd "$REC" && find . -type f ! -name manifest.txt ! -name manifest.txt.body | LC_ALL=C sort |
    while read -r f; do echo "$(shasum -a 256 "$f" | cut -d' ' -f1) $f"; done | shasum -a 256 | cut -d' ' -f1)"
{
    cat "$MANIFEST.body"
    echo "content $CONTENT"
} > "$MANIFEST"
rm "$MANIFEST.body"

echo "PROGRESS licenses"
LIC="$APP/Contents/Resources/Licenses"
cp "$PORT_DIR/LICENSE" "$LIC/PORT-LICENSE.txt"
cp "$LLVM_PREFIX/LICENSE.TXT" "$LIC/LLVM-LICENSE.txt"
cp "$LLD_PREFIX/LICENSE.TXT" "$LIC/LLD-LICENSE.txt"
cp /opt/homebrew/opt/z3/LICENSE.txt "$LIC/Z3-LICENSE.txt"
cp /opt/homebrew/opt/zstd/LICENSE "$LIC/ZSTD-LICENSE.txt"
cp "$PORT_DIR/third_party/SR/README.md" "$LIC/SR-README.md"
for f in "$REC"/lib/*.dylib; do
    case "$(basename "$f")" in
        libLLVM* | libclang-cpp* | liblld* | libz3* | libzstd*) ;;
        *) die "$(basename "$f") has no license file in Contents/Resources/Licenses" ;;
    esac
done

# Each program and library must run on MIN_VERSION: a file that needs a
# newer version stops the script, so that a build on a newer macOS cannot
# make a release that only that version can open.
TOO_NEW="$(for f in "$APP/Contents/MacOS/PillarsOfEternity" "$REC"/bin/* "$REC"/lib/*.dylib; do
    v="$(otool -l "$f" | awk '/cmd LC_BUILD_VERSION/{b=1} b&&/minos/{print $2; exit}')"
    [ -n "$v" ] || { echo "$(basename "$f") (no minimum version)"; continue; }
    echo "$v" | awk -F. -v min="$MIN_VERSION" -v name="$(basename "$f")" \
        'BEGIN{split(min, M, ".")} { if ($1+0 > M[1]+0 || ($1+0 == M[1]+0 && $2+0 > M[2]+0)) print name " (" $0 ")" }'
done)"
[ -z "$TOO_NEW" ] || die "these files need a macOS version newer than $MIN_VERSION: $(echo $TOO_NEW)"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleExecutable</key>
    <string>PillarsOfEternity</string>
    <key>CFBundleIdentifier</key>
    <string>local.pillars-mac-port</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>Pillars of Eternity</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>0.1</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>LSApplicationCategoryType</key>
    <string>public.app-category.role-playing-games</string>
    <key>LSMinimumSystemVersion</key>
    <string>$MIN_VERSION</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
PLIST
codesign --force --sign - "$APP"

echo "PROGRESS check the package"
# Permitted: sse_inline.bc (only the SR llasm support helpers), the host
# objects of the runtime and the two assemblies of the port (their code is
# the code of the port; they refer to types of the game by name only).
bad="$(find "$DIST" -type f \( -name '*.o' -o -name '*.ll' -o -name '*.llasm' -o -name '*.bc' -o -iname '*.exe' -o -iname '*.dll' \) \
    ! -path "$SSE_BC" ! -path "$HOSTOBJ/*.o" ! -path "$REC/managed/PortAotInstances.dll" ! -path "$REC/managed/PortGraphics.dll" | head -5)"
[ -z "$bad" ] || die "the package holds files with game code: $bad"
if "$LLVM/llvm-nm" --defined-only "$APP/Contents/MacOS/PillarsOfEternity" "$REC/lib/libpillars_rt.a" "$HOSTOBJ"/*.o "$SSE_BC" |
    grep -q -E ' (_loc_[0-9A-Fa-f]|_[a-z_]+_code_map$)'; then
    die "the package defines a symbol of the translated code"
fi
# The home folder and the port folder of the build computer.
paths="$(grep -r -l -a -F -e "$HOME/" -e "$PORT_DIR" "$DIST" | head -5 || true)"
[ -z "$paths" ] || die "files hold paths of the build computer: $paths"

rm -f "$ZIP"
# --norsrc: no resource forks, extended attributes or ACLs, so the archive
# has no __MACOSX folder. The signatures are in the files themselves.
(cd "$DIST" && ditto -c -k --norsrc . "$ZIP")
echo "PROGRESS done: $ZIP (minimum macOS $MIN_VERSION)"
