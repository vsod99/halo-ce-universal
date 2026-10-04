#!/usr/bin/env python3
"""Where the Nspire build spends its time: halo_samples.tns (the pc the game
was at 256 times a second, and its lr, kept by the sampling timer's FIQ
handler in port/nspire/src/nspire_abort.S) counted by the function of
build/nspire/halo.elf it fell in, and the time in runtime helpers (soft
floating point, memcpy) by their callers.

    build/nspire/nspire_link get /halo_samples.tns halo_samples.tns
    python3 tools/nspire_samples.py halo_samples.tns [--top 40] [--elf build/nspire/halo.elf]
"""

import argparse
import bisect
import collections
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NM = os.path.join(ROOT, "..", "Ndless", "ndless-sdk", "toolchain", "install", "bin", "arm-none-eabi-nm")


def functions(elf):
    """(address, name) of each function, by address"""
    output = subprocess.run([NM, "-n", "-C", elf], check=True, capture_output=True, text=True).stdout
    result = []
    for line in output.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[1] in "TtWw":
            result.append((int(parts[0], 16), parts[2]))
    return result


def is_helper(name):
    """runtime helpers (soft floating point, memcpy and the like): the time in
    them is charged to their callers as well"""
    return name.startswith("__") or name.startswith(".") or name in (
        "memcpy", "memset", "memmove", "memcmp", "strlen", "strcmp", "_stricmp", "sqrtf", "__ieee754_sqrtf")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("samples")
    parser.add_argument("--elf", default=os.path.join(ROOT, "build", "nspire", "halo.elf"))
    parser.add_argument("--top", type=int, default=40)
    parser.add_argument("--skip", type=float, default=0.0, help="leave out this fraction of the oldest samples")
    arguments = parser.parse_args()

    data = open(arguments.samples, "rb").read()
    magic, base, count = struct.unpack_from("<III", data)
    if magic == 0x504D5348:
        pairs = [(pc, 0) for pc in struct.unpack_from("<%dI" % count, data, 12)]
    elif magic == 0x324D5348:
        words = struct.unpack_from("<%dI" % (count * 2), data, 12)
        pairs = list(zip(words[0::2], words[1::2]))
    else:
        sys.exit("not a samples file")
    pairs = pairs[int(len(pairs) * arguments.skip):]
    symbols = functions(arguments.elf)
    addresses = [address for address, _ in symbols]
    end = addresses[-1] + 0x10000 if addresses else 0

    def name_of(address):
        offset = address - base
        if offset < 0 or offset >= end:
            return None
        index = bisect.bisect_right(addresses, offset) - 1
        return symbols[index][1] if index >= 0 else "?"

    counts = collections.Counter()
    helper_callers = collections.Counter()
    charged = collections.Counter()
    for pc, lr in pairs:
        # (the FIQ's lr is the interrupted pc + 4)
        name = name_of(pc - 4)
        if name is None:
            name = "(outside the program: the OS, or code made at run time, %07x)" % ((pc - 4) >> 20 << 20)
        counts[name] += 1
        caller = name_of(lr - 4) if lr else None
        if is_helper(name) and caller:
            helper_callers[caller] += 1
            charged[caller] += 1
        else:
            charged[name] += 1
    total = max(len(pairs), 1)
    print("%d samples, the program at %08x" % (len(pairs), base))
    print("\nby function:")
    for name, hits in counts.most_common(arguments.top):
        print("%6.2f%% %6d  %s" % (100.0 * hits / total, hits, name))
    if helper_callers:
        print("\nwho calls the helpers (soft floating point, memcpy...):")
        for name, hits in helper_callers.most_common(arguments.top):
            print("%6.2f%% %6d  %s" % (100.0 * hits / total, hits, name))
        print("\nby function, the helpers it calls included:")
        for name, hits in charged.most_common(arguments.top):
            print("%6.2f%% %6d  %s" % (100.0 * hits / total, hits, name))


if __name__ == "__main__":
    main()
