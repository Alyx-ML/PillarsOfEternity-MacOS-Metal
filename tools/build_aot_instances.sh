#!/bin/sh
# Build the AOT instance assembly PortAotInstances.dll and its translated AOT
# image (work/port_aot_instances_aot).
#
# In AOT-only mode, Mono cannot run a generic instance that no AOT image
# contains; it throws ExecutionEngineException ("Attempting to JIT compile
# method ... while running with --aot-only"). The full-AOT compiler does not
# find every instance that the game uses (for example instances that only
# reflection makes). tools/aot_instances.txt lists these instances.
# PortAotInstances.dll uses each of them, so its AOT image contains their
# code. The player loads the assembly when the program starts Mono
# (rt_mono_use_aot_instances, runtime/src/rt_mono.c).
#
# Usage: build_aot_instances.sh
#
# Prerequisites:
#   1. The .NET SDK in /usr/local/share/dotnet (csc and a
#      Microsoft.NETCore.App runtime and reference pack of the same version).
#   2. build/aot/mono-aot (tools/build_aot_host.sh without --player).
#   3. The game directory is the parent directory of the port directory.
#
# Steps:
#   1. Compile tools/gen_aot_instances.cs to build/tools/gen_aot_instances.dll.
#   2. Run it: it reads the assemblies of PillarsOfEternity_data/Managed and
#      tools/aot_instances.txt, and writes
#      work/aot/user/aot/PortAotInstances.dll (guest path
#      C:\Users\Player\aot\PortAotInstances.dll).
#   3. Compile the assembly with the Mono full-AOT compiler, with the options
#      of the other AOT images.
#   4. tools/build_aot_image.sh makes the image PortAotInstances.dll.dll at
#      0x21C00000. The images of the test assemblies end below this address,
#      and the image of Assembly-CSharp starts at 0x22000000.
#   5. tools/translate_module.sh and tools/compile_module.sh translate and
#      compile the image.
#   6. Copy the assembly to the work directory and write assembly.name, so
#      that tools/build_aot_host.sh --player copies the assembly to
#      build/player/aot.
# Then link the player with port_aot_instances_aot in the list of AOT work
# directories of tools/build_aot_host.sh --player.
#
# WARNING: the assembly contains references to the game's assemblies, and
# the image contains code compiled from them. Do not distribute them,
# upload them or commit them.
set -eu

PORT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
GAME_DIR="$(cd "$PORT_DIR/.." && pwd)"
DOTNET_DIR=/usr/local/share/dotnet
NAME=port_aot_instances_aot
ASSEMBLY=PortAotInstances.dll
IMAGE=$ASSEMBLY.dll
IMAGE_BASE=0x21C00000
TOOLS="$PORT_DIR/build/tools"
AOT_DIR="$PORT_DIR/work/aot/user/aot"

if [ ! -x "$PORT_DIR/build/aot/mono-aot" ]; then
    echo "error: build/aot/mono-aot does not exist; run tools/build_aot_host.sh first" >&2
    exit 1
fi
ref=$(ls -d "$DOTNET_DIR"/packs/Microsoft.NETCore.App.Ref/*/ref/net* 2>/dev/null | sort -V | tail -1)
csc=$(ls -d "$DOTNET_DIR"/sdk/*/Roslyn/bincore/csc.dll 2>/dev/null | sort -V | tail -1)
if [ -z "$ref" ] || [ -z "$csc" ]; then
    echo "error: the .NET SDK in $DOTNET_DIR has no reference pack or csc" >&2
    exit 1
fi
tfm=$(basename "$ref")
version=$(basename "$(dirname "$(dirname "$ref")")")
if [ ! -d "$DOTNET_DIR/shared/Microsoft.NETCore.App/$version" ]; then
    echo "error: the .NET runtime $version (of the reference pack $ref) is not installed" >&2
    exit 1
fi

echo "PROGRESS compile tools/gen_aot_instances.cs"
mkdir -p "$TOOLS" "$AOT_DIR"
: > "$TOOLS/gen_aot_instances.rsp"
for f in "$ref"/*.dll; do
    printf '"-r:%s"\n' "$f" >> "$TOOLS/gen_aot_instances.rsp"
done
"$DOTNET_DIR/dotnet" "$csc" -nologo -noconfig -nostdlib -target:exe -optimize+ -nullable:enable -warnaserror+ \
    @"$TOOLS/gen_aot_instances.rsp" -out:"$TOOLS/gen_aot_instances.dll" "$PORT_DIR/tools/gen_aot_instances.cs"
printf '{"runtimeOptions":{"tfm":"%s","framework":{"name":"Microsoft.NETCore.App","version":"%s"}}}\n' \
    "$tfm" "$version" > "$TOOLS/gen_aot_instances.runtimeconfig.json"

echo "PROGRESS generate $ASSEMBLY"
"$DOTNET_DIR/dotnet" exec --runtimeconfig "$TOOLS/gen_aot_instances.runtimeconfig.json" "$TOOLS/gen_aot_instances.dll" \
    "$GAME_DIR/PillarsOfEternity_data/Managed" "$PORT_DIR/tools/aot_instances.txt" "$AOT_DIR/$ASSEMBLY"

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

echo "PROGRESS translate and compile the image"
# $text is two addresses: it is not quoted.
CODE_SEEDS=fixed sh "$PORT_DIR/tools/translate_module.sh" $NAME $IMAGE "$(cat "$work/image.sha256")" $text
sh "$PORT_DIR/tools/compile_module.sh" $NAME

cp "$AOT_DIR/$ASSEMBLY" "$work/$ASSEMBLY"
printf '%s\n' $ASSEMBLY > "$work/assembly.name"
echo "PROGRESS done: $work ($IMAGE, SHA-256 $(cat "$work/image.sha256"))"
