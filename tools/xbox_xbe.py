"""The memory an XBE asks the kernel for (``python tools/xbox_xbe.py XBE
128|64``).

cxbe always sets the XBE header's "limit development kit run time memory to
64 MB" initialization flag (nxdk tools/cxbe/Xbe.h), with which the kernel
gives a 128 MB console's program only 64 MB, and the game needs 128
(port/xbox/README.md): the build clears it on every XBE it makes
(tools/xbox_build.py), and tools/xbox_dev.py sets it again for a run on a
64 MB console's settings.
"""

import sys
from pathlib import Path

XBE_INIT_FLAGS_OFFSET = 0x124
XBE_INIT_FLAG_LIMIT_64MB = 0x4


def set_memory_limit(xbe: Path, limit_64mb: bool) -> None:
    data = bytearray(xbe.read_bytes())
    if data[:4] != b"XBEH":
        sys.exit(f"{xbe} is not an XBE")
    flags = int.from_bytes(data[XBE_INIT_FLAGS_OFFSET:XBE_INIT_FLAGS_OFFSET + 4], "little")
    flags = flags | XBE_INIT_FLAG_LIMIT_64MB if limit_64mb else flags & ~XBE_INIT_FLAG_LIMIT_64MB
    data[XBE_INIT_FLAGS_OFFSET:XBE_INIT_FLAGS_OFFSET + 4] = flags.to_bytes(4, "little")
    xbe.write_bytes(bytes(data))


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[2] not in ("128", "64"):
        sys.exit("usage: xbox_xbe.py XBE 128|64")
    set_memory_limit(Path(sys.argv[1]), sys.argv[2] == "64")
