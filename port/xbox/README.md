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
2. `git clone --recursive https://github.com/XboxDev/nxdk ~/source/repos/nxdk`,
   and build its libraries optimized (nxdk's makefile names no `-O`, and a
   program's first `make` builds them without; CI does the same):

       cd ~/source/repos/nxdk && NXDK_DIR=$PWD PATH=$PWD/bin:/opt/homebrew/opt/llvm/bin:$PATH \
         make -B CFLAGS=-O2 $(for l in libpdclib libwinapi libxboxrt libnxdk libnxdk_hal libpbkit \
           nxdk_usb libnxdk_automount_d xboxkrnl/libxboxkrnl; do echo $PWD/lib/$l.lib; done)

3. Copy `port/xbox/xemu.example.toml` to `port/xbox/xemu.local.toml` (not
   committed) and fill it in. The MCPX boot ROM and the flash BIOS must be
   dumped from your own console. The hard disk can be xemu's blank image
   ([xemu-hdd-image](https://github.com/xemu-project/xemu-hdd-image/releases)).
4. `python3 tools/xbox_dev.py doctor` checks it.

## Setup (Windows)

The build is Linux's, in WSL; `tools/xbox_dev.py` runs in Windows's Python
beside Windows's xemu (WSL's sockets do not reach Windows's 127.0.0.1, and
xemu draws with the GPU), and runs make, ninja, extract-xiso and
llvm-symbolizer in WSL by `wsl.exe`.

1. In WSL Ubuntu: LLVM 18 (from [apt.llvm.org](https://apt.llvm.org) on
   22.04: `sudo bash llvm.sh 18`) and
   `sudo apt install clang-18 lld-18 llvm-18 ninja-build make cmake g++ bison flex`.
   (If installing them leaves WSL unable to start Windows programs, "Exec
   format error": `echo ':WSLInterop:M::MZ::/init:PF' | sudo tee
   /usr/lib/binfmt.d/WSLInterop.conf`, and `systemd-binfmt` not masked.)
2. nxdk in WSL, at CI's commit (`.github/workflows/build.yml`), its
   libraries optimized as above, with cxbe and extract-xiso:

       git clone https://github.com/XboxDev/nxdk ~/source/repos/nxdk && cd ~/source/repos/nxdk
       git checkout <NXDK_COMMIT> && git submodule update --init --recursive
       NXDK_DIR=$PWD PATH=$PWD/bin:/usr/lib/llvm-18/bin:/usr/bin:/bin \
         make -B CFLAGS=-O2 cxbe extract-xiso $(for l in libpdclib libwinapi libxboxrt libnxdk libnxdk_hal \
           libpbkit nxdk_usb libnxdk_automount_d xboxkrnl/libxboxkrnl; do echo $PWD/lib/$l.lib; done)

3. [xemu](https://github.com/xemu-project/xemu/releases) for Windows.
4. `port/xbox/xemu.local.toml` as in the template, with `wsl` naming the
   distribution: `[tools]`'s nxdk and LLVM paths are WSL's (absolute, no
   `~`), the xemu, console and maps paths Windows's.
5. `python3 configure.py` in WSL (the build's paths are Linux's), then
   `python tools/xbox_dev.py doctor` and `run` from Windows.

`--press`, `--shot` and `--listen` are macOS's; `--input`, `--frames` and
`--wav` do the same from inside the game.

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
| `nxdk_memory.c` | `XPhysicalAlloc` and page protection from the kernel's contiguous memory, and a pool of it set aside before the game state for when the kernel's runs out; the game state as virtual memory at 0x42000000 and the sound cache as virtual memory; Halo Custom Edition's tag cache window at 0x40440000, made of the Xbox tag cache's pages (`cache/physical_memory_map.c` takes the GPU's caches first); the memory left in the log (at start, once the caches are had, and with each `frame N, X fps` line) |
| `nxdk_posix.c` | File descriptors and the file half of `port/linux/src/posix.h` over nxdk's Windows API; `D:` is the XBE's folder (maps), `E:/halo` the settings and saves |
| `nxdk_libc.c` | What pdclib lacks or gets wrong: printf's floating point, `strtod`, `fmod`, `scalbn`, `lrint` (musl's: `port/third_party/musl-stdio`); `memset`, `memcpy` and `memmove` with the string instructions (pdclib's go a byte at a time) |
| `sdl_files.c` | The SDL file functions `port_config.c` and `menu_files.c` call (`include/SDL3/SDL.h`) |
| `d3d8_nv2a.c` | The game's Direct3D 8 as NV2A push buffer methods: its vertex shaders (NV2A microcode) and pixel shaders (combiner values) as they are, the simple render states as their own methods, the rest before each draw; textures, vertex buffers and surfaces at their physical addresses; the menus' pictures (PNGs, `port/linux/src/png_decode.c`) for their placeholder textures. `debug.gpu_trace_frame` logs a frame's draws |
| `nxdk_nv2a.c` | pbkit: the video mode, the push buffer, the three screen buffers flipped at the vertical blank (started before the game takes its memory), and context DMAs over the low 64 MB for the game's own surfaces; the time the processor waits on the GPU (`frame N, X fps, waited Y%`: the frame rate snaps to the vertical blanks, so where the frame rate is under 30 the processor's own time a frame is the frame's time less that share) |
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

The picture is 640x480: 480p where the console's video setting allows it
and the cable carries it (nxdk chooses the mode), else 480i. A widescreen
television stretches it across 16:9, as it did the Xbox's games;
`display.widescreen` (`"auto"`, the default, follows the console's video
setting, which its dashboard sets; `"on"`; `"off"`) widens the 3D view to
match (`halo_screen_pixel_aspect`, `source/render/render_cameras.c`). The
menus and HUD, laid out for 640 columns, stretch with the picture. The log's
`GPU: pbkit started` line gives the console's setting.

The settings screens show the Xbox's own rows (`platform="xbox"` in the
menus' files, `tools/port_settings.py`): Video Setup is WIDESCREEN and the
field of view; there is no window, resolution, frame rate limit,
V-Sync, interpolation, graphics screen, voice chat, invite clipboard or
updater to set.

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
`sound.wav` there (xemu's `-audio driver=wav`; xemu writes it at 44.1
kHz). xemu plays only its APU on the Mac, never the AC'97 controller the
game plays through (its SDL backend crashes for it, `-audio driver=sdl`),
so a run is silent unless `--listen`: it records as `--wav` does, and
`tools/xbox_listen.swift` (built with `swiftc` on first use) plays the file
as xemu writes it. On the console, pictures of frames will come from the
program over the network.

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

### Where the time goes

    python3 tools/xbox_dev.py run build/xbox/halo --profile 80:45 ...

samples the Xbox's processor about every 10 ms for 45 s, 80 s after the
program's first log line: xemu's gdb stub stops the machine, the
instruction pointer and the return addresses on the stack are read (the
game keeps frame pointers), and it runs on. `profile.txt` in the run's
folder has the shares by function (halo.exe's DWARF, and the link map for
nxdk's libraries), by function with what it called, by source folder, and
the callers of the busiest functions; `samples.json` keeps the stacks
(`tools/xbox_dev.py profile RUN` writes the report again). xemu's instructions do not cost a
Pentium III's, so these are shares, not timings. Play tests want a release
build (`python3 configure.py --release`): in a 30-player internet game the
assertions and checked accessors of the default build took the frame rate
from 15-22 fps to 4-6 in xemu.

A profile is best taken in a scene that moves. A demo (`src/xbox_demo.c`)
plays the same stretch of a campaign level every time: during play, Back
held while both sticks are clicked saves the game (the game's core save,
`E:\halo\z\core\demo.bin`, its z: drive) and records the first controller's every reading
until the same buttons again, into `E:\halo\demo.bin` on xemu's hard disk.
It can be played live in xemu's window, or scripted (buttons held together
are separate `--input`s at one time: those of one `--input` are pressed in
turn):

    python3 tools/xbox_dev.py run build/xbox/halo --env HALO_START_MAP=b30 \
        --input '250:back*0.5' --input '250:ls*0.5' --input '250:rs*0.5' \
        --input '253:lup*24' --input '278:rright*0.5' --input '279:lup*18' \
        --input '300:back*0.5' --input '300:ls*0.5' --input '300:rs*0.5'

`debug.demo` `"play"` (`--env HALO_DEMO=play`) starts the level from the
main menu, loads the save once it has loaded, gives the game the readings
in place of the controller's, and quits when they run out, logging `demo:
done: N frames, T ticks in M ms, X ms a frame, Y of the processor's own (W%
waiting on the GPU)`:

    python3 tools/xbox_dev.py run build/xbox/halo --env HALO_DEMO=play --profile 30:40

While a demo records or plays, each frame runs a tick and a half of game
time, and held buttons are timed by those frames, so that the game goes the
same way however long frames take (a recording in xemu goes at its frame
rate's share of the game's speed). The recording loads its own save as it
starts, as the demo played does, and both start from the game's random
seeds as they were (the save leaves them out). Each reading keeps the
seed, and the log names the first reading a playback's differs at, and
whether it ended at the recording's: a demo that went another way is not
comparable.

Demos do not yet always go the same way. A playback can part from the
recording (the session that played up to the save has state of its own
besides the save: frame counts in the objects and lights, the particles),
and two playbacks can part from each other: the sound is mixed in real time
and the game reads when its sounds end (`--env HALO_NO_AUDIO=1` takes that
away), and something else timed by the clock remains. The log's lines say
when; compare builds by profile shares until they agree.

In xemu the frame's time is mostly the wait on xemu's GPU (b30's demo:
75 ms a frame, 60-66% of it waiting), and the same build's frame time
comes out within a few tenths of a millisecond run to run; the processor's
own time moves 10-15% between runs (xemu runs the processor and the GPU on
threads of their own), so a change to the processor's work is judged by
its share in a profile of the same demo rather than by the timing.

### Joining someone's game over the internet

    python3 tools/xbox_dev.py run build/xbox/halo --router-forward \
        --env HALO_COMMAND_LINE=halo://join/<the host's 64 hex digits> --env HALO_NETWORK_TEST=join

The invite is the Xbox's command line; `join` picks the host's game once
it is in the system link list (or leave it out and pick it in xemu). Two
NATs that map each destination to its own port never connect (a carrier's
NAT and many home routers do), so `--router-forward` makes the Xbox a
console whose router forwards it a port: the runner asks the Mac's
network's router (UPnP) to forward a UDP port to the Mac for the run, xemu
forwards it to the Xbox's tunnel port, and the runner is the Xbox's only
STUN server, telling it the router's address and that port are its own.
The forwarding is removed at the end. The host's map must be in `[game]
maps`.

### Halo Custom Edition maps

    [game]
    custom_maps_folder = "~/halo/custom_maps"   # the maps and bitmaps.map, sounds.map, loc.map
    custom_maps = "deathisland,infinity"

The runner packs the maps `[game] custom_maps` names, with Custom Edition's
`bitmaps.map`, `sounds.map` and `loc.map` (every Custom Edition map reads
tags from them), beside the XBE as `D:\custom_maps`, the folder the game
looks in (`docs/custom_edition_caches.md`). A map's level name is
`custom_maps\<name>`: `--env 'HALO_START_MAP=custom_maps\deathisland'`.

A Custom Edition map's tags are linked to run at 0x40440000, up to 23 MB of
them, and the Xbox has no 23 MB to spare. Only one map is loaded at a time,
so when `D:\custom_maps` exists `nxdk_memory.c` maps the 22 MB Xbox tag
cache's own pages a second time at 0x40440000 (the program runs in the
processor's most privileged mode, so it writes the page tables itself, in a
range reserved from the kernel), and 1 MB more for the last megabyte, where
structure BSPs load. The game state moved from 0x40000000 to 0x42000000 to
make room (saved games of earlier Xbox builds no longer load). Sounds
Custom Edition keeps as Ogg Vorbis are encoded again at load into 2 MB at
most; the rest are silent.

Each model part's vertices and strip are read from the map as it is
converted, to the top of the window (free until a structure BSP loads), and
the models' geometry goes between the tags and the lowest structure BSP, at
the Xbox tag cache's own address (which the GPU reads), drawn in place from
headers of the port's own. A structure BSP's vertices are compressed where
they lie in the BSP: the window's last megabyte is had right after the tag
cache's pages, so all 23 MB are one run the GPU reads. Only the BSP's buffer
headers need room beside the models', kept for them, as a BSP that fails to
load is fatal to the game.

With `game.custom_edition_reduce_detail` (Xbox only, on by default), a map
whose models do not fit at full detail is run with each permutation's high
and super high levels of detail drawn from its medium geometry; maps that
fit keep their full detail. When even that does not fit, each model geometry
is read from the map, compressed and given buffers when it is first drawn,
as Halo PC's geometry cache does: into a cache of the game's own kind
(`lruv_cache.c`, the texture cache's), whose blocks go when their room is
wanted, once the GPU has drawn from them (`custom_edition_model_geometry_ready`,
called by `models.c` before a geometry is drawn). The cache is the room
between the tags and the lowest structure BSP when that is 3 MB or more,
else 4 MB the texture cache lends from its top (`texture_cache_lend_memory`);
the structure BSPs' buffer headers are kept room at its start. A geometry
the cache has no room for is not drawn that frame. Each part keeps where its
vertices and strip lie in the map in its tag's empty blocks, and each
geometry its block in its tag's padding, so the cache costs the heap
nothing per part. The log has a line on the cache every 900 frames.

A map is measured from a few reads of its file before a game starts for it
(`custom_edition_cache_measure`; the resource maps' tags measured from their
indexes when an allowance for them leaves no room): one whose tags would
reach its structure BSPs is left out of the map lists, and refused with a
message when a host names it. So are maps that fit but do not run well
enough yet (`xbox_unsupported_maps` in `custom_edition_cache.c`).

| Map | Models, compressed | Room | Runs (xemu, release) |
| --- | --- | --- | --- |
| Death Island, Infinity, Yoyorast Island | 3.4-3.6 MB | 11-13 MB | 25-30 fps |
| Portent | 3.3 MB | 7.6 MB | 19 fps |
| Chronopolis C3 | 7.6 MB | 9.1 MB | 17 fps (xemu's GPU-bound) |
| cmt Snow Grove | 10.5 MB | 10.9 MB | 27 fps |
| Hugeass | 11.3 MB (5.2 at medium) | 7.2 MB | 25-30 fps, medium detail |
| Extinction | 10.4 MB (6.9) | 5.3 MB | 30 fps, read when drawn (cache beside the tags) |
| TSCE | 11.6 MB (9.0) | 0.1 MB (15 MB BSPs) | about 20 fps, read when drawn (cache lent); large textures garbled |
| bigass v3 | 26.7 MB (12.8) | 5.5 MB | 10-22 fps, read when drawn; large textures garbled |
| Coldsnap | 8.4 MB (6.3) | 1.2 MB (12.5 MB BSPs) | runs, under 3 fps: its scripts take the processor (trigger volume tests every tick) |
| Precipice | 4.5 MB (2.6) | 0.2 MB (16 MB BSP) | not listed: its textures thrash the texture cache (3-4 fps) |

A large uncompressed Custom Edition bitmap is swizzled through two heap
buffers its own size (`rasterizer_swizzle.c`); with 2-3 MB of heap free in a
map, those of 1.5 MB or more fail and are drawn unswizzled (TSCE, bigass,
Precipice).

The NV2A has no texture swizzle, so a Custom Edition multipurpose map (Halo
PC's channel order) is read in its own order by the model shader instead:
combiner 0 takes specular from blue and color change from alpha
(`rasterizer_xbox_models.c`); only the auxiliary mask, which would be red,
stays alpha, as a combiner's alpha inputs read blue and alpha only. HUD
meters are still drawn in Halo PC's order (Snow Grove's are gradients): the
meter shader kills by the texture's own alpha, which holds their fill order.

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

How it runs (Oct 9, in xemu): the program itself takes 20 MB of the low
half (its zeroed globals were 19 MB until the Xbox's limits on Custom
Edition maps and the profiler's history were cut, 7 MB), the tag and
texture caches 44 MB, so the low half has about 10 MB left for everything
else the GPU reads: the menus' pictures, render targets, decal vertices.
The game state and the heap, virtual memory, would take those pages first:
with the game state allocated, 3.4 MB of it was left, and the menus'
larger pictures were sometimes not drawn. So `nxdk_memory.c` sets that
memory aside as a pool (9.9 MB) before the game state, and hands its pages
out when the kernel's contiguous memory runs out; the sound cache, which the
processor mixes from, is virtual memory too. In use: the server browser 5.8
MB of the pool (its pictures 2.3 MB), b30 3.2 MB, Blood Gulch 3.9 MB, with
5.5-6.4 MB free besides.

Cerbios's hybrid kernel runs its debugger on COM1 when it sees the SuperIO
chip, so programs log on COM2; the runner saves COM1 as `com1.bin`.
