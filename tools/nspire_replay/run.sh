#!/bin/sh
# run.sh <halo_frame.tns>: replays a captured frame under QEMU (instruction
# counts as time) and converts the result to build/nspire/replay/replay_frame.png
set -e
OUT=build/nspire/replay
cp "$1" $OUT/halo_frame.tns
cd $OUT
qemu-system-arm -M versatilepb -cpu arm926 -m 256 -nographic -semihosting -icount shift=0 -kernel replay.elf
rm -f device_frame.png
sips -s format png replay_frame.ppm --out replay_frame.png >/dev/null 2>&1 || true
echo "image: $OUT/replay_frame.png"
if [ -f device_frame.ppm ]; then
	sips -s format png device_frame.ppm --out device_frame.png >/dev/null 2>&1 || true
	rm -f device_frame.ppm
	echo "the device's own: $OUT/device_frame.png"
fi
