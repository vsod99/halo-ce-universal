#!/bin/sh
# build.sh: the replay (replay.elf, in build/nspire/replay), from the renderer's
# sources compiled as the game compiles them. Run from the repository root:
#   tools/nspire_replay/build.sh && tools/nspire_replay/run.sh <halo_frame.tns>
# REPLAY_FLAGS: defines for replay.c; RENDER_FLAGS: for the renderer sources
set -e
ROOT=$(pwd)
OUT=build/nspire/replay
SDK=${NDLESS_SDK:-$ROOT/../Ndless/ndless-sdk}
T=$SDK/toolchain/install
mkdir -p $OUT
# the game's flags for the platform sources (ninja knows them)
FLAGS=$(ninja -t commands build/nspire/obj/port/nspire/src/soft_rasterizer.o | tail -1 |
	sed -e 's/^clang //' -e 's/-MMD -MF [^ ]* //' -e 's/ -c port.*$//')
for f in soft_rasterizer soft_vertex soft_textures nspire_profile; do
	# addresses through the capture's memory
	sed 's/PLATFORM_PHYSICAL_TO_VIRTUAL(/replay_address(/g' port/nspire/src/$f.c > $OUT/$f.c
	eval clang $FLAGS -DNSPIRE_FINE_PROFILE=${FINE_PROFILE:-1} -DCHECK_COMPILED_COMBINERS=${CHECK:-0} -DNSPIRE_COMBINER_CODE=${CODE:-1} -DNSPIRE_VERTEX_CODE=${VCODE:-1} -DNSPIRE_FIXED_FETCH=${FFETCH:-1} -DNSPIRE_FIXED_OUTCODES=${FOUT:-1} -DNSPIRE_REPLAY=1 $RENDER_FLAGS -include $ROOT/tools/nspire_replay/replay.h -Iport/nspire/src -c $OUT/$f.c -o $OUT/$f.o
done
eval clang $FLAGS -DNSPIRE_REPLAY=1 $REPLAY_FLAGS -include $ROOT/tools/nspire_replay/replay.h -c tools/nspire_replay/replay.c -o $OUT/replay.o
clang --target=armv5te-none-eabi -mcpu=arm926ej-s -marm -mfloat-abi=soft -O2 -ffreestanding -fno-builtin \
	-c tools/nspire_replay/baremetal.c -o $OUT/baremetal.o
$T/bin/arm-none-eabi-gcc -mcpu=arm926ej-s -marm -nostdlib -nostartfiles -Wl,-Ttext=0x10000 -Wl,--gc-sections -Wl,-e,_start \
	tools/nspire_replay/start.S $OUT/replay.o $OUT/soft_rasterizer.o $OUT/soft_vertex.o $OUT/soft_textures.o \
	$OUT/nspire_profile.o $OUT/baremetal.o $T/arm-none-eabi/lib/libm.a -lgcc -o $OUT/replay.elf 2>&1 |
	grep -v "warning\|NOTE" || true
echo "built $OUT/replay.elf"
