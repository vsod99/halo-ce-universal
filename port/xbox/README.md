# Original Xbox

Work in progress: a build of the game for the original Xbox, from the same
sources as the native ports, that plays with them over system link and
internet play. It needs a modified console with 128 MB of memory: the native
builds' game state (`port/linux/include/halo_port_capacity.h`), which every
machine in a game must share, does not fit in 64 MB beside the tag, texture
and sound caches.

The build uses [nxdk](https://github.com/XboxDev/nxdk) (clang, lld, no
Microsoft SDK). Until the console is ready, everything runs in
[xemu](https://xemu.app).

## Setup (macOS)

1. `brew install llvm lld coreutils` and `brew install --cask xemu`.
2. `git clone --recursive https://github.com/XboxDev/nxdk ~/source/repos/nxdk`.
3. Copy `port/xbox/xemu.example.toml` to `port/xbox/xemu.local.toml` (not
   committed) and fill it in. The MCPX boot ROM and the flash BIOS must be
   dumped from your own console. The hard disk can be xemu's blank image
   ([xemu-hdd-image](https://github.com/xemu-project/xemu-hdd-image/releases)).
4. `python3 tools/xbox_dev.py doctor` checks it.

## The game's build

`python3 configure.py` adds the Xbox build when it finds nxdk (the path in
`xemu.local.toml`, or `--nxdk`); `tools/xbox_build.py` writes it.

    ninja xbox-game      # the game's units alone
    ninja xbox           # build/xbox/halo/bin/default.xbe

The game's units are the other ports': nxdk's clang targets the same
Microsoft ABI as the Windows build, so they take `include/halo_xbox_prefix.h`
(after the Windows prefix) and the Linux build's C runtime wrappers over
nxdk's C library, pdclib, with `include/` standing in for the headers pdclib
lacks. The platform layer is the Linux build's units that implement the Xbox
SDK (`port.json`) and `src/`'s own; `src/nxdk_*.c` alone see nxdk's
Windows and kernel headers.

## The loop

    python3 tools/xbox_dev.py run port/xbox/probe            # opens xemu's window
    python3 tools/xbox_dev.py run port/xbox/probe --gdb      # waits for gdb on :1234

(`--headless` passes `-display none`, but xemu 0.8 then never starts the
machine: its own window drives the emulation. Runs open the window.)

The console's memory is the BIOS's as much as the configuration's: a
modified retail BIOS such as Complex 4627 gives the kernel 64 MB even with
`memory = "128"`. Testing the 128 MB layout needs a BIOS that sets up
128 MB, as the console's upgrade does.

`run` makes the program (`make` in its folder, nxdk's Makefile), clears the
XBE's "limit to 64 MB" flag (cxbe always sets it, and the kernel would give
the program only 64 MB of a 128 MB console; `memory = "64"` keeps it), packs
`bin/` into `build/xbox/<name>.iso`, and boots it in xemu with a generated
configuration: no welcome screen or boot animation, the configured memory,
NAT networking, the ISO in the drive.

A program logs with `xbox_log` (`common/xbox_log.c`) to COM2 of the debug
kits' SuperIO chip, which xemu emulates (`-device lpc47m157`); the runner
reads it from a socket, prints it, and saves it in
`build/xbox/runs/<time>/log.txt` with xemu's own output (`xemu.txt`) and
COM1's (`com1.bin`, the kernel debugger's when there is one). A run
succeeds when the program prints `== XBOX DONE ==` (`xbox_log_done`), and
fails at xemu's exit or the timeout (`--timeout`, 60 s), so it can be
scripted: the exit status is 0 only for a finished run.

A retail console has no SuperIO chip: there, `xbox_log` finds no serial
port and writes to the screen only. The plan is for the game to log over
the network as well, which works on both.

xemu's QMP has no `screendump`, since xemu draws with its own renderer.
Pictures of frames will come from the program, read back from its frame
buffer and sent over the network, the same on xemu and the console.

## Programs

| Folder | What it finds out |
| --- | --- |
| `probe` | Phase 0: whether an nxdk program can have the tag cache at physical 0x3A6000 and the native game state at 0x1A00000, where nxdk's start-up puts its image, stack and heap, and how much memory is left beside the caches |

The probe's results (xemu 0.8.136):

- With Complex 4627 (64 MB): the tag cache and the game state are had at
  exactly their addresses, and nxdk's image, stack and heap stay below
  0x130000. 64,876 KB are free at start, 21,868 KB once those and the sound
  cache are held: too little for the 22,528 KB texture cache, before any of
  the game's code, heap, Direct3D or frame buffers.
- With Cerbios 3.1.0 beta (128 MB, `/LIMITMEM` cleared by the runner): the
  kernel counts 131,072 KB, but gives contiguous memory
  (`MmAllocateContiguousMemoryEx`, what `XPhysicalAlloc` is) only from the
  low 64 MB; not one 64 KB step of the upper half can be had that way.
  Ordinary virtual memory (`NtAllocateVirtualMemory`) does take pages from
  the upper half, once the low half's free pages are used up.
- So the layout that fits 128 MB: the tag cache at physical 0x3A6000, the
  texture and sound caches contiguous anywhere in the low half (the GPU and
  the sound hardware read them), and the 16 MB game state, which only the
  CPU reads, as virtual memory at a fixed address (0x40000000) instead of at
  physical 0x1A00000. All of it fits, with 63 MB free after a video mode.
- The kernel gives virtual memory the low half's free pages first, so the
  port must allocate everything contiguous first (the caches, and a pool for
  Direct3D's frame buffers, push buffer and vertex buffers) and only then
  the game state and heap, or the low half runs out.

Cerbios's hybrid kernel runs its debugger on COM1 when it sees the SuperIO
chip, so programs log on COM2; the runner saves COM1 as `com1.bin`.
