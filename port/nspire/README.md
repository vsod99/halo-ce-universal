# TI-Nspire CX II

`ninja nspire` builds the game for the TI-Nspire CX II under Ndless:
`build/nspire/halo.tns`. It plays one campaign level, The Silent
Cartographer (b30), without sound. It is a work in progress: the
software renderer does not draw the level yet (see "Status").

## Requirements

- The Ndless SDK, built (its `toolchain/build_toolchain.sh`, then `make` in
  `ndless-sdk`). `configure.py` looks for it in `../Ndless/ndless-sdk` next
  to this repository; `--ndless-sdk` or `NDLESS_SDK` selects another.
- clang with the ARM target (Apple's clang has it). `--nspire-cc` selects
  another.
- Python 3 and ninja, as for the other ports.
- A calculator with Ndless installed, or the Firebird emulator.

## Build

1. `python3 configure.py`
2. `ninja nspire`
3. `python3 tools/nspire_map.py <maps>/b30.map build/nspire/b30.map.tns`,
   with `maps/` from an Xbox disc image of the game.

## Install and run

1. Send `halo.tns` and `b30.map.tns` to the same folder of the calculator
   (with TI-Nspire Computer Link or the TI-Nspire software).
2. Open `halo.tns`.
3. The program's name picks the level: `halo_<level>.tns` plays that level
   (`halo_a10.tns` with `a10.map.tns`, converted as above); any other name
   plays b30.
4. Hold esc for a second to quit. Cutscenes are fast-forwarded without drawing
   (b30 opens with one that cannot be skipped); press doc to watch them instead.

The game writes `halo_log.txt.tns` (the port's log) and `debug.txt.tns`
(the game's errors) in that folder.

Quitting (esc held) keeps the last checkpoint in `halo_<level>_save.tns`,
and the next start goes back to it once the level has loaded, past the
cutscene that opens it. A save from another map or another map file is not
used. Delete the file to start the level from the beginning (and if a new
build misbehaves just after loading one: a save may not suit another build).

From the command line instead, over USB: TiLP's libraries do not know the
CX II (its USB product id, 0xE022), but libnspire does. With a checkout of
[libnspire](https://github.com/Vogtinator/libnspire) next to this repository,
built with `./configure && make`, `tools/nspire_link.sh` builds
`build/nspire/nspire_link`. Paths on the calculator start at its documents
folder:

    build/nspire/nspire_link ls /
    build/nspire/nspire_link put build/nspire/halo.tns /halo.tns
    build/nspire/nspire_link get /halo_log.txt.tns halo_log.txt.tns /halo_frame.tns halo_frame.tns
    build/nspire/nspire_link rm /halo_b30_save.tns

## Controls

| Calculator | Xbox controller | Game |
| --- | --- | --- |
| 8 2 4 6 (7 9 1 3 diagonally) | left stick | move |
| touchpad (drag) | aim | look |
| touchpad (click an edge) | right stick | turn |
| touchpad (click the centre), enter | right trigger | fire |
| 0 | A | jump |
| . | B | melee |
| (-) | X | action, reload |
| × | Y | change the weapon |
| + | left trigger | throw a grenade |
| - | white | flashlight |
| ÷ | black | change the grenade |
| 5 | left stick click | crouch |
| tab | right stick click | zoom |
| menu | start | pause menu |
| esc (hold) | | quit |
| doc | | watch cutscenes instead of fast-forwarding them |
| var | | write the next frame to `halo_frame.tns`, for the replay below |
| r | | start or end recording a demo (below) |

## Demo

A demo plays the same stretch of the game the same way every run, to time
builds against each other (`src/nspire_demo.c`). During play, `r` saves the
game to `halo_demo_save.tns` and records the controls from then on; `r`
again (or esc held) ends the recording and writes `halo_demo_input.tns`. The
status line says `REC` meanwhile. A copy of the program named
`halo_demo.tns` skips the main menu, goes back to that save in b30, plays the
recording, and quits at its end, logging
`demo: done: N frames, T ticks in M ms, A ms a frame`. While a demo records or
plays, each frame runs a fixed game time, so the ticks come out alike however
fast the build draws. A recording made by one build plays on later ones while
the game state's layout stays the same; the log says if the game went another
way. Runs of one build agree to about 1%. A copy named `q_demo.tns` plays the
demo too, and with a file named `halo_demo_capture.tns` next to it the
demo writes its 100th frame to `halo_frame.tns`, for the replay below (that
run's time is then not comparable).

`tools/nspire_bench.py` plays the demo from the computer with no one at the
calculator: plugged in and showing My Documents, it sends
`build/nspire/halo.tns` (`--upload`, as `q_demo.tns`, `halo.tns` and
`halo_demo.tns`), types q and enter in the file browser through TI's remote
key service (`nspire_link key`; `nspire_link shot` takes a screenshot),
waits for the calculator to leave USB and come back (Halo leaves the bus
for a moment as it quits, `nspire_usb_reattach`, for the computer to see it
again), and fetches the log into `build/nspire/bench/`:

    python3 tools/nspire_bench.py --upload --tag mychange --sections

Compare builds at the same clock, the log's `speed:` line: plugging the
cable in again sets the OS's own (288 MHz), whatever NoverII set.

## How the port operates

### Memory

The CX II has 64 MB, of which a program gets about 25 MB
(`port/nspire/probe` measures it). The game expects the Xbox's memory at
`0x80000000`: its game state and tag cache at fixed addresses there
(`source/cache/physical_memory_map.c`). The OS leaves that range unmapped,
so `src/nspire_memory.c` maps heap memory there with the ARM926's MMU
(coarse page tables of 4 KB pages). The game uses the Xbox's capacities
(`port/linux/include/halo_port_capacity.h`).

### Demand paging

Every campaign map needs about 22 MB of tag data and structure BSP at the
tag cache's addresses, more than the calculator has. `tools/nspire_map.py`
stores them as independently deflated 16 KB blocks (b30: 15.7 MB of tags in
7.5 MB). The loader (`game/cache_files_nspire.c`) keeps the blocks in the
heap. The tag cache's addresses stay unmapped, and `src/nspire_paging.c`
takes the data abort of the first access to a block, inflates the block into
a pool of resident blocks, maps it and retries the access. When the pool is
full the oldest unchanged block goes; a block the game wrote to stays.

### Threads, time, files

- `src/nspire_threads.c`: the game's threads run as coroutines, with the
  part of POSIX threads the shared kernel layer (`port/linux/src/xbox_kernel.c`)
  uses.
- `src/nspire_time.c`: the second timer of the SP804 at `0x900D0000`, at
  32768 Hz.
- `src/posix_nspire.c`: files through Ndless's system calls. A file sent
  from a computer ends in `.tns`; lookups accept the name with it.
- `src/nspire_heap.c`: every allocation is freed at exit; the OS would
  otherwise keep it until the calculator is reset.

### Graphics

`src/d3d8_soft.c` is the Xbox Direct3D device, drawn in software by
`src/soft_rasterizer.c` into the screen through nSDL (`src/posix_video.c`).
The game lays out its usual 640x480. To be fast enough:

- The 3D world is drawn at 160x120 and doubled onto the screen when the
  engine starts on the HUD (`source/render/render.c`); the HUD and overlays
  are drawn at 320x240.
- Vertex programs (`src/soft_vertex.c`) are trimmed to the outputs a draw
  uses, run in 16.16 fixed point where their operations allow (positions too
  large for it, the sky's, over 16, translations with them, when a vertex
  checked against floating point agrees: `NSPIRE_POSITION_SCALING`) (as ARM code
  written for each program, `vjit_routine`; `NSPIRE_VERTEX_CODE 0`
  interprets them), positions first and the rest only for triangles that
  can be seen; a mesh's later passes reuse what its first transformed.
- The pixel combiners are compiled for each draw to what reaches the pixel,
  then written out as ARM code for that setup (`jit_routine`, cached per
  setup; `NSPIRE_COMBINER_CODE 0` interprets them instead); large triangles
  shade once per 2x2 block. Cube maps are a neutral grey.
- Textures are at most 64x64 with mipmaps (the converter), one level a
  triangle.
- Screen-aligned textured rectangles (text, the interface's pieces) are
  filled directly, without triangles (`NSPIRE_RECTANGLES`); those whose
  combiners prove to be texture times colour skip the combiners too. A run
  of draws with the same combiner and texture states keeps the last draw's
  combiners, samplers and code.
- Render targets of at most 64x64 (the motion sensor's, which it then
  pastes on the HUD) are drawn into at their size, without depth, and
  written into their texture, swizzled, when the game turns to another
  target (`drawing_to_texture`; they are made A4R4G4B4, the samplers' one
  format, in `port/linux/src/d3d8_resources.c`). Larger ones (shadows,
  water, the secondary buffer) are skipped.
- A model part that drew no pixel when last drawn (behind a ridge, which
  its box crosses) is skipped for as many frames as it has drawn nothing in
  a row, up to `PART_SKIP_MOST` (3): far scenery coming over a crest shows
  up to that many frames late.
- Small objects are not drawn (`NSPIRE_MINIMUM_OBJECT_PIXELS` and
  `NSPIRE_MINIMUM_SCENERY_PIXELS` in `halo_port_capacity.h`); the game runs
  at most two ticks a frame.

### Measuring the renderer away from the calculator

`var` writes the next frame to `halo_frame.tns` (`src/soft_capture.c`): its
clears and draws with their state, vertex data and textures.
`tools/nspire_replay` draws it again through the same renderer sources on
QEMU's ARM926 board, counting instructions as time, and writes the image:

    brew install qemu
    tools/nspire_replay/build.sh
    tools/nspire_replay/run.sh ~/Downloads/halo_frame.tns

It logs each draw (in thousands of instructions) and the renderer's
sections, and writes `build/nspire/replay/replay_frame.png`, and beside it
`device_frame.png`, the screen as the calculator showed it (captures from
before it was kept have none): the two differ only if the calculator draws
otherwise than the replay. The device logs the captured frame's draws too,
to set against the replay's.

`REPLAY_FLAGS` takes more: `-DREPLAY_TWICE` replays the frame twice and
measures the second (what is kept between frames, as on the calculator, is
made already); `-DREPLAY_FIRST=n -DREPLAY_LAST=m` reports the renderer's
sections for draws n to m alone; `-DREPLAY_STOP=n` stops after draw n;
`-DREPLAY_FIRST_PERSON_FIRST=n -DREPLAY_FIRST_PERSON_LAST=m` marks draws as
the first-person weapon's; `-DCOUNT_SHADES` counts the pixels each pixel
loop takes and the shading each draw does. `-DREPLAY_SKIP=n` leaves draw n out
(to see what it adds), `-DREPLAY_FIND_COUNT=n` logs the number of each
draw of n vertices (the replay counts every draw the capture holds; the
log's draw numbers count only those drawn), and `-DREPLAY_FIRST_PERSON_CACHE`
(in `RENDER_FLAGS` too) skips the marked first-person draws the second
time, pasting them as the calculator does. `RENDER_FLAGS` passes defines to
the renderer's sources instead (`-DDEBUG_PROGRAMS=n` dumps draw n's vertex
program, `-DDEBUG_FIXED_COUNTS` logs each draw's vertices run in fixed and
floating point).

### Where the rest of the time goes

Built with `NSPIRE_SAMPLES` set to how many samples to keep, a power of two
(in `src/nspire_paging.c`; 0, off, by default: its interrupt is a suspect
in a corrupted unit vector, and with it on the calculator has hung on a
black screen after Halo quit, the samples written, needing a reset and
Ndless installed again), a timer
interrupts the game 256 times a second (as a FIQ: Ndless runs programs with
the OS's interrupts masked) and records the pc it was at and its lr (the
handler is in `src/nspire_abort.S`), the last 64 seconds kept. Quitting writes
them to `halo_samples.tns`, and `tools/nspire_samples.py` counts them by
function:

    build/nspire/nspire_link get /halo_samples.tns halo_samples.tns
    python3 tools/nspire_samples.py halo_samples.tns

## Compromises to revisit

Made for speed, to be undone as the port gets faster:

- **One game tick a frame** (`NSPIRE_TICKS_PER_FRAME` in
  `halo_port_capacity.h`, was 2; Xbox up to 7): the game runs at about a
  tenth of its speed, but each frame answers the controls. Raise it once
  object updates and AI cost less.
- Models lit once a draw (`NSPIRE_FLAT_MODEL_LIGHTING`), cube maps grey,
  textures at most 64x64, 3D at 160x120, blended layers and the sky shaded
  by 2x2 blocks.
- Shading by 2x2 blocks in the 3D view for the rest too
  (`soft_rasterizer.c`): large triangles by their own blocks
  (`NSPIRE_OPAQUE_BLOCKS`), the level's lightmap pass by blocks shared by
  all of a draw's triangles (`NSPIRE_SHARED_LAYER_BLOCKS`), and opaque
  draws that are not alpha tested the same where the depths agree
  (`NSPIRE_SHARED_OPAQUE`, `SHARED_DEPTH_TOLERANCE`): a little smearing
  where a block crosses an edge. `SKY_BLOCK_SHIFT` 2 would shade the sky
  by 4x4 (2% of a frame; blockier clouds).
- The first-person weapon and hands shaded by 2x2 blocks, each triangle
  its own however small (`NSPIRE_FIRST_PERSON_BLOCKS`), and drawn every
  every third frame (`NSPIRE_FIRST_PERSON_EVERY`): the frames between paste
  the pixels (colour and depth) they left, so they animate at a third of
  the rate. They were a third of the renderer's work.
- The HUD (`interface_draw_screen`) drawn every third frame
  (`NSPIRE_HUD_EVERY`), the pixels it changed pasted between: its numbers
  are up to two frames late, and under its translucent parts the world is
  as it was when it was drawn.
- Models' detail chosen for the size they are drawn at, an eighth of the
  640 wide the levels were made for (`NSPIRE_MODEL_DETAIL_SCALE`; a
  sixteenth measured no faster: the models are at their least already).
- Objects not drawn lately, but for vehicles, animated (their node
  matrices) every other tick (source/objects/objects.c).
- Small objects and scenery not drawn (`NSPIRE_MINIMUM_OBJECT_PIXELS`,
  `NSPIRE_MINIMUM_SCENERY_PIXELS`).
- The game's object, unit, AI, physics, model and maths code built with
  -O3 (`GAME_SPEED_DIRECTORIES` in tools/nspire_build.py; the renderer and
  the maths library too): empty the set if something in the game behaves
  oddly. All of the game's code but the interface's leaves the frame pointer
  out (the HUD's return-address check needs it); assertions' stack dumps
  stop short.

## Status

- Done: the build, memory, paging, loading b30, input, the game loop,
  the software renderer, checkpoints in memory.
- The demo, at about 350 MHz: 874 ms a frame (977 before part skipping by
  streaks and the HUD's paste); about 1290 ms at the OS's 288 MHz.
- Next: speed. The level's lightmap and diffuse passes draw the same
  triangles twice (the diffuse pass is about 74 ms a frame); merging them is
  the largest single saving known.
