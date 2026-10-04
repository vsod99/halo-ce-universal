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

#endif /* __HALO_XBOX_NXDK_PLATFORM_H */
