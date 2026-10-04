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

#endif /* __HALO_XBOX_NXDK_PLATFORM_H */
