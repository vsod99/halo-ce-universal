/*
NXDK_PLATFORM.H

What the units that talk to nxdk (nxdk_*.c, built with nxdk's Windows and
kernel headers instead of the Xbox SDK's: tools/xbox_build.py) share with
the rest of the platform layer, declared without either's types.
*/

#ifndef __HALO_XBOX_NXDK_PLATFORM_H
#define __HALO_XBOX_NXDK_PLATFORM_H

/* port/linux/src/xbox_kernel.c: a line to the log (xbox_log.c) */
void platform_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* the game state's virtual memory at its fixed address
(cache/physical_memory_map.c; nxdk_memory.c) */
void *xbox_game_state_allocate(unsigned long address, unsigned long size);

/* the controllers (nxdk_gamepads.c, for xinput_xbox.c) */
/* (a report is an XINPUT_GAMEPAD's bytes) */
#define XBOX_GAMEPAD_REPORT_SIZE 18
/* notices controllers plugged in and out; the ports with one, a bit each */
unsigned long xbox_gamepads_poll(void);
/* the latest report of the controller in a port (0-3) and how many it has
sent; false if there is none */
int xbox_gamepad_report(int port, unsigned char report[XBOX_GAMEPAD_REPORT_SIZE], unsigned long *reports);
void xbox_gamepad_rumble(int port, unsigned short left, unsigned short right);

/* the network (nxdk_net.c): the Ethernet address, 6 bytes (zeros with no
Ethernet), which XNet's XNADDR carries (p2p_none.c) */
const unsigned char *xbox_net_ethernet_address(void);
/* whether the cable is plugged in (XNet's link status: port/linux/src/xnet.c) */
int xbox_net_link_up(void);

/* the GPU through pbkit (nxdk_nv2a.c, for d3d8_nv2a.c) */
/* context DMAs over all of the low 64 MB for the color and depth buffers */
#define XBOX_GPU_DMA_COLOR 18
#define XBOX_GPU_DMA_ZETA 19
/* and over the visibility tests' reports: 16 bytes each (a time stamp,
the pixel count, a word the GPU zeroes), for indices 0 to 0xfff */
#define XBOX_GPU_DMA_REPORT 20
#define XBOX_GPU_REPORT_BYTES (4096UL * 16)
/* after them, the fence: the count the GPU writes once it has drawn all
before it (d3d8_nv2a.c) */
#define XBOX_GPU_FENCE_OFFSET XBOX_GPU_REPORT_BYTES
/* whether pbkit started (with the video mode) */
int xbox_gpu_ready(void);
void xbox_gpu_screen(unsigned long *width, unsigned long *height, unsigned long *pitch);
/* the reports' memory, or NULL */
volatile unsigned long *xbox_gpu_reports(void);
/* the physical address of the buffer the next frame is drawn into */
unsigned long xbox_gpu_back_buffer(void);
/* room for that many words of methods at the returned address; the end
of what was written goes to xbox_gpu_end, which sends it to the GPU */
unsigned long *xbox_gpu_begin(unsigned long dwords);
void xbox_gpu_end(unsigned long *end);
/* the GPU told of everything written (xbox_gpu_end does so only now and
then), before polling for what it writes back */
void xbox_gpu_kick(void);
int xbox_gpu_busy(void);
void xbox_gpu_wait_idle(void);
/* the count the GPU last wrote at XBOX_GPU_FENCE_OFFSET, and a wait,
yielding to the other threads, until it reaches that count */
unsigned long xbox_gpu_fence(void);
void xbox_gpu_wait_fence(unsigned long fence);
/* shows the back buffer at the next vertical blank; the next one then
becomes the back buffer */
void xbox_gpu_present(void);
/* waits for a vertical blank; their count */
unsigned long xbox_gpu_wait_vertical_blank(void);
unsigned long xbox_gpu_vertical_blank_count(void);

#endif /* __HALO_XBOX_NXDK_PLATFORM_H */
