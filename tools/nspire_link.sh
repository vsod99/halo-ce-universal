#!/bin/sh
# nspire_link.sh: builds tools/nspire_link.c against libnspire (a checkout
# next to this repository, built with ./configure && make) into
# build/nspire/nspire_link. Run from the repository root.
set -e
LIBNSPIRE=${LIBNSPIRE:-$(pwd)/../libnspire}
USB_CFLAGS=$(pkg-config --cflags libusb-1.0)
USB_LIBS=$(pkg-config --libs libusb-1.0)
mkdir -p build/nspire
clang++ -x c -O2 -I"$LIBNSPIRE/src/api" $USB_CFLAGS tools/nspire_link.c -x none \
	"$LIBNSPIRE/src/.libs/libnspire.a" $USB_LIBS -o build/nspire/nspire_link
echo "built build/nspire/nspire_link"
