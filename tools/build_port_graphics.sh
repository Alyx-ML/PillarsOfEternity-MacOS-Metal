#!/bin/sh
# Build the assembly PortGraphics.dll (the port's rows on the Graphics tab of
# the options window) and its translated AOT image
# (work/port_graphics_aot).
#
# tools/port_graphics/PortGraphics.cs adds rows for the graphics settings of
# the port (runtime/src/rt_portgfx.c) to the Graphics page of the game's
# options window. The player loads the assembly into the Unity child domain
# and calls PortGraphics.GraphicsOptions.Tick after each frame
# (rt_mono_port_tick, runtime/src/rt_mono.c).
#
# Usage: build_port_graphics.sh
#
# Prerequisites:
#   1. The .NET SDK in /usr/local/share/dotnet (csc).
#   2. build/aot/mono-aot (tools/build_aot_host.sh without --player).
#   3. The game directory is the parent directory of the port directory.
#
# Steps:
#   1. Compile tools/port_graphics/PortGraphics.cs against the assemblies of
#      PillarsOfEternity_data/Managed to work/aot/user/aot/PortGraphics.dll
#      (guest path C:\Users\Player\aot\PortGraphics.dll).
#   2. Compile the assembly with the Mono full-AOT compiler, with the options
#      of the other AOT images.
#   3. tools/build_aot_image.sh makes the image PortGraphics.dll.dll at
#      0x21E00000. The image of PortAotInstances.dll is below this address,
#      and the image of Assembly-CSharp starts at 0x22000000.
#   4. tools/translate_module.sh and tools/compile_module.sh translate and
#      compile the image.
#   5. Copy the assembly to the work directory and write assembly.name, so
#      that tools/build_aot_host.sh --player copies the assembly to
#      build/player/aot.
# Then link the player with port_graphics_aot in the list of AOT work
# directories of tools/build_aot_host.sh --player.
#
# WARNING: the assembly is compiled against the game's assemblies, and the
# image contains code compiled from them. Do not distribute them, upload them
# or commit them.
set -eu

PORT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
GAME_DIR="$(cd "$PORT_DIR/.." && pwd)"
MANAGED="$GAME_DIR/PillarsOfEternity_data/Managed"
DOTNET_DIR=/usr/local/share/dotnet
NAME=port_graphics_aot
ASSEMBLY=PortGraphics.dll
IMAGE=$ASSEMBLY.dll
IMAGE_BASE=0x21E00000
IMAGE_LIMIT=0x22000000
TOOLS="$PORT_DIR/build/tools"
AOT_DIR="$PORT_DIR/work/aot/user/aot"

if [ ! -x "$PORT_DIR/build/aot/mono-aot" ]; then
    echo "error: build/aot/mono-aot does not exist; run tools/build_aot_host.sh first" >&2
    exit 1
fi
if [ ! -f "$MANAGED/Assembly-CSharp.dll" ]; then
    echo "error: $MANAGED/Assembly-CSharp.dll does not exist" >&2
    exit 1
fi
csc=$(ls -d "$DOTNET_DIR"/sdk/*/Roslyn/bincore/csc.dll 2>/dev/null | sort -V | tail -1)
if [ -z "$csc" ]; then
    echo "error: the .NET SDK in $DOTNET_DIR has no csc" >&2
    exit 1
fi

echo "PROGRESS compile $ASSEMBLY"
mkdir -p "$TOOLS" "$AOT_DIR"
: > "$TOOLS/port_graphics.rsp"
for f in "$MANAGED"/*.dll; do
    printf '"-r:%s"\n' "$f" >> "$TOOLS/port_graphics.rsp"
done
"$DOTNET_DIR/dotnet" "$csc" -nologo -noconfig -nostdlib -target:library -optimize+ -warnaserror+ \
    @"$TOOLS/port_graphics.rsp" -out:"$AOT_DIR/$ASSEMBLY" "$PORT_DIR/tools/port_graphics/PortGraphics.cs"

echo "PROGRESS AOT-compile $ASSEMBLY"
rm -f "$AOT_DIR/$IMAGE.s"
"$PORT_DIR/build/aot/mono-aot" "$GAME_DIR" "$PORT_DIR/work/aot" --guest-env MONO_DEBUG=explicit-null-checks \
    "--aot=full,asmonly,asmwriter,nodebug,outfile=C:\\Users\\Player\\aot\\$IMAGE.s" "C:\\Users\\Player\\aot\\$ASSEMBLY"
if [ ! -s "$AOT_DIR/$IMAGE.s" ]; then
    echo "error: the AOT compiler did not write $AOT_DIR/$IMAGE.s" >&2
    exit 1
fi

echo "PROGRESS make the image"
sh "$PORT_DIR/tools/build_aot_image.sh" $NAME "$AOT_DIR/$IMAGE.s" $IMAGE $IMAGE_BASE
work="$PORT_DIR/work/$NAME"
text=$(sed -n 's/^image base [^,]*, \.text \(0x[0-9a-f]*\)-\(0x[0-9a-f]*\)$/\1 \2/p' "$work/seeds.txt")
if [ -z "$text" ]; then
    echo "error: $work/seeds.txt has no .text range" >&2
    exit 1
fi
size=$(python3 -c 'import struct,sys
d=open(sys.argv[1],"rb").read()
pe=struct.unpack_from("<I",d,0x3c)[0]
print(struct.unpack_from("<I",d,pe+24+56)[0])' "$work/$IMAGE")
if [ $((IMAGE_BASE + size)) -gt $((IMAGE_LIMIT)) ]; then
    echo "error: the image $IMAGE ($size bytes at $IMAGE_BASE) ends above $IMAGE_LIMIT, the image of Assembly-CSharp" >&2
    exit 1
fi

echo "PROGRESS translate and compile the image"
# $text is two addresses: it is not quoted.
CODE_SEEDS=fixed sh "$PORT_DIR/tools/translate_module.sh" $NAME $IMAGE "$(cat "$work/image.sha256")" $text
sh "$PORT_DIR/tools/compile_module.sh" $NAME

cp "$AOT_DIR/$ASSEMBLY" "$work/$ASSEMBLY"
printf '%s\n' $ASSEMBLY > "$work/assembly.name"
echo "PROGRESS done: $work ($IMAGE, SHA-256 $(cat "$work/image.sha256"))"
