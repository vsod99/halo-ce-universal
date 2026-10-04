# The hardware probe (port/nspire/README.md): make [NDLESS_SDK=<path>]
NDLESS_SDK ?= $(abspath ../../../../Ndless/ndless-sdk)
TOOLS = PATH="$(NDLESS_SDK)/bin:$(NDLESS_SDK)/toolchain/install/bin:$$PATH"

all: halo_probe.tns

probe.o: probe.c
	$(TOOLS) nspire-gcc -Wall -marm -O2 -c $< -o $@

halo_probe.elf: probe.o
	$(TOOLS) nspire-ld $^ -o $@

halo_probe.tns: halo_probe.elf
	$(TOOLS) genzehn --input $< --output $@.zehn --name "halo_probe"
	$(TOOLS) make-prg $@.zehn $@
	rm $@.zehn

clean:
	rm -f probe.o halo_probe.elf halo_probe.tns
