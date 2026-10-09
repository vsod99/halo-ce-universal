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

The link (`build/xbox/halo.exe`, with its DWARF for gdb and a map,
`halo.exe.map`) is made into the XBE without the debug information, which
cxbe would otherwise load into memory.

The menus are the PC version's (`port/assets/menus`), with their pictures
at the size the original game drew them: `tools/xbox_menu_art.py` (Pillow)
writes those copies to `port/assets/menus/xbox`, which the build embeds in
the originals' place (the originals are drawn for a desktop, about 245 MB
as textures; the copies 25 MB, decoded when first drawn and at most 8 MB
kept). The build stops when a copy is missing or was made from another
original (`sources.json` there): run the tool again after the menus'
pictures change.

## The platform layer (`src/`)

| Unit | What |
| --- | --- |
| `nxdk_main.c` | Start-up, before the game's `main`: the log, the hard disk as `E:`, `E:\halo`; Quit goes to the dashboard |
| `nxdk_memory.c` | `XPhysicalAlloc` and page protection from the kernel's contiguous memory; the game state as virtual memory at 0x40000000 (`cache/physical_memory_map.c` takes the caches first) |
| `nxdk_posix.c` | File descriptors and the file half of `port/linux/src/posix.h` over nxdk's Windows API; `D:` is the XBE's folder (maps), `E:/halo` the settings and saves |
| `nxdk_libc.c` | What pdclib lacks or gets wrong: printf's floating point, `strtod`, `fmod`, `scalbn`, `lrint` (musl's: `port/third_party/musl-stdio`) |
| `sdl_files.c` | The SDL file functions `port_config.c` and `menu_files.c` call (`include/SDL3/SDL.h`) |
| `d3d8_nv2a.c` | The game's Direct3D 8 as NV2A push buffer methods: its vertex shaders (NV2A microcode) and pixel shaders (combiner values) as they are, the simple render states as their own methods, the rest before each draw; textures, vertex buffers and surfaces at their physical addresses; the menus' pictures (PNGs, `port/linux/src/png_decode.c`) for their placeholder textures. `debug.gpu_trace_frame` logs a frame's draws |
| `nxdk_nv2a.c` | pbkit: the video mode, the push buffer, the three screen buffers flipped at the vertical blank (started before the game takes its memory), and context DMAs over the low 64 MB for the game's own surfaces |
| `nxdk_gamepads.c` | The controllers through nxdk's USB host stack (started before the game takes its memory: its pool is contiguous); each port's latest report |
| `xinput_xbox.c` | The SDK's `XInput*` over them (a report is an `XINPUT_GAMEPAD`), rumble; the menus' text fields on the game's on-screen keyboard (`platform_text_field_on_screen`) |
| `xbox_platform.c` | The desktop's hooks as the Xbox answers them; no high-res HUD or text |
| `nxdk_audio.c` | The sound output for Linux's DirectSound mixer (`port/linux/src/dsound_sdl.c`): 48 kHz stereo through the AC'97 controller, a thread above the game's keeping about 64 ms of its descriptors mixed ahead (the ring set aside before the game takes its memory); the log has the mixing's share of the processor every 30 s. The APU is not used yet |
| `nxdk_net.c` | The sockets under Linux's Winsock layer (`port/linux/src/xnet.c`) over lwIP, which the build compiles with the loopback interface (`lwip_config/lwipopts.h`): nxdk's Ethernet driver (started before the game takes its memory), the address from DHCP or a link-local one in the background (claimed once the first DHCP request goes unanswered, there 7-8 s after start-up; logged when it comes), XNet's link the cable's and its address pending until then (the game's transport waits up to 10 s for it, as with the console's own XNet). A split screen game joins its own host through 127.0.0.1. Names are looked up with lwIP's DNS (the DHCP server's; none with a link-local address) |
| `nxdk_p2p.c` | Internet play's (`port/linux/src/p2p*.c`) process half of `posix.h`: the hardware id from the EEPROM's serial number and Ethernet address; no command line (a test's `HALO_COMMAND_LINE` stands in), links, second copy or Discord. The brokers' list is `D:\brokers.txt`, beside the XBE (the build puts it there) |
| `nxdk_upnp.c` | Internet play's UPnP: `port/linux/src/posix_upnp.c` and miniupnpc compiled as on a POSIX system against lwIP's sockets by their POSIX names (`include/lwip_posix`) |

Threads, mutexes, condition variables and clocks are the Windows build's
(`port/windows/src/win32_threads.c`, nxdk has those Windows calls), and the
texture layout without OpenGL is `texture_layout.c`. The game's own log
(debug.txt, on the read-only D: here) goes to COM2 with the rest, its lines
marked `debug.txt:`.

`debug.start_map` (`--env HALO_START_MAP=b30`) starts a map the menus do not
reach a few seconds into the main menu: a campaign level at normal, any
other map (`bloodgulch`, `bloodgulch:ctf`) as a one-player split screen game.
The map, and carousel, which a multiplayer lobby loads first, must be in
`[game] maps`.

## The loop

    python3 tools/xbox_dev.py run port/xbox/probe            # opens xemu's window
    python3 tools/xbox_dev.py run port/xbox/probe --gdb      # waits for gdb on :1234
    python3 tools/xbox_dev.py run build/xbox/halo            # the game (ninja xbox)

The game's run packs the maps `[game]` names in `xemu.local.toml` (`ui`
alone by default: the main menu) beside the XBE, as `D:\maps`. Under xemu
with Cerbios (128 MB) it takes its memory, reads `ui.map` and its menus,
writes `E:\halo\config.toml`, and draws the main menu on the NV2A.

xemu's keyboard is the controller in port 1 (arrows the D-pad, A B X Y
those letters, Return Start, E S D F and I J K L the sticks, W and O the
triggers). `--press SECONDS:BUTTON,...` presses its buttons that long after
the program's first log line, e.g. `--press 18:down,a` opens Multiplayer;
it needs the terminal to have macOS's Accessibility permission. The menus
repeat a button held over about a quarter second.

With `--gdb`, lldb attaches with `gdb-remote 1234`; the kernel's own
breakpoints and assertions go to its debugger on COM1 instead (a run whose
`com1.bin` grows to megabytes stopped in one), so set a breakpoint where it
stopped and symbolize the stack with `llvm-symbolizer --obj=build/xbox/halo.exe`.

(`--headless` passes `-display none`, but xemu 0.8 then never starts the
machine: its own window drives the emulation. Runs open the window.)

The console's memory is the BIOS's as much as the configuration's: a
modified retail BIOS such as Complex 4627 gives the kernel 64 MB even with
`memory = "128"`. Testing the 128 MB layout needs a BIOS that sets up
128 MB, as the console's upgrade does.

`run` makes the program (`make` in its folder, nxdk's Makefile; `ninja
xbox` for the game), clears the
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

xemu's QMP has no `screendump`, since xemu draws with its own renderer:
`--shot SECONDS` saves xemu's window that long after the program's first
log line as `shot-SECONDS.png` in the run's folder (the terminal needs
macOS's Screen Recording permission). `--wav` records the sound card as
`sound.wav` there instead of playing it (xemu's `-audio driver=wav`;
xemu writes it at 44.1 kHz). On the console, pictures of frames
will come from the program over the network.

### System link between two Xboxes

    python3 tools/xbox_dev.py link bloodgulch
    python3 tools/xbox_dev.py link bloodgulch --env HALO_NETWORK_TEST_SHOOT=2 \
        --host-env HALO_NETWORK_TEST_KILL=20 --host-env HALO_NETWORK_TEST_VEHICLE=40

boots two xemus: one hosting the map as a system link game
(`debug.network_test host:MAP[:VARIANT]`, started 15 s after hosting:
`HALO_NETWORK_TEST_START`), the other joining the first game it finds
(`debug.network_test join`); `--env` is for both, `--host-env` and
`--join-env` for one. Their network cards are joined by a cable through
the runner: each xemu's UDP backend sends its Ethernet frames to the
runner, which passes them to the other and saves them as `cable.pcap`
(Wireshark, `tcpdump -r`), with a count of each machine's frames by
protocol and port at the end. There is no DHCP server on the cable, so the
machines take link-local addresses (169.254.x.x) as two consoles and a
cable do. Each machine gets an APFS clone of the hard disk (xemu locks the
image) and an EEPROM copy with its own Ethernet address (the factory
section's checksum made again). The logs come prefixed `[host]` and
`[join]`, and are saved in `build/xbox/runs/<time>/{host,join}`.

The network tests' tick lines show each machine's view of every player
(position, health, weapons, score, kills and deaths), to compare.

### Internet play between two Xboxes

    python3 tools/xbox_dev.py link bloodgulch --internet --forward

The same two machines without the cable: each on its own xemu NAT, which
reaches the internet through the Mac, as two players' homes. The host's
invite (logged as it starts hosting) is the joining machine's command line
(`HALO_COMMAND_LINE`, which the runner sets when it sees the invite and
then starts that machine), so it reaches the host through the public MQTT
brokers, the tunnel connects, and the host's game is in its system link list.
xemu's NAT gives each destination its own port, which no two machines get
through (`p2p.c` says so in the log), so `--forward` has the host's router
forward its tunnel port, as a player would: xemu forwards a port of the Mac
to the host's `network.tunnel_port`, and the runner is the host's only STUN
server, which tells it that port of 10.0.2.2 is its internet address (the
joining machine's NAT takes 10.0.2.2 to the Mac). Without `--forward` the
brokers and STUN are the public ones and the machines find each other but
cannot connect. UPnP is off unless `--upnp`: the machines would ask the
Mac's network's router to forward ports to xemu's address.

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
