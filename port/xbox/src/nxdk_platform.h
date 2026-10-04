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

/* the GPU through pbkit (nxdk_nv2a.c, for d3d8_nv2a.c) */
/* context DMAs over all of the low 64 MB for the color and depth buffers */
#define XBOX_GPU_DMA_COLOR 18
#define XBOX_GPU_DMA_ZETA 19
/* whether pbkit started (with the video mode) */
int xbox_gpu_ready(void);
void xbox_gpu_screen(unsigned long *width, unsigned long *height, unsigned long *pitch);
/* the physical address of the buffer the next frame is drawn into */
unsigned long xbox_gpu_back_buffer(void);
/* room for that many words of methods at the returned address; the end
of what was written goes to xbox_gpu_end, which sends it to the GPU */
unsigned long *xbox_gpu_begin(unsigned long dwords);
void xbox_gpu_end(unsigned long *end);
int xbox_gpu_busy(void);
void xbox_gpu_wait_idle(void);
/* shows the back buffer at the next vertical blank; the next one then
becomes the back buffer */
void xbox_gpu_present(void);
/* waits for a vertical blank; their count */
unsigned long xbox_gpu_wait_vertical_blank(void);
unsigned long xbox_gpu_vertical_blank_count(void);

#endif /* __HALO_XBOX_NXDK_PLATFORM_H */
