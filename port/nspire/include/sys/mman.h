/*
SYS/MMAN.H

newlib has no <sys/mman.h>. The platform files shared with the Linux port
include it; the Nspire has no virtual memory calls, and its memory window
(port/nspire/src/nspire_memory.c) replaces the one xbox_memory.c maps.
*/

#ifndef __HALO_NSPIRE_SYS_MMAN_H
#define __HALO_NSPIRE_SYS_MMAN_H

#include <stddef.h>
#include <sys/types.h>

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED ((void *)-1)

#endif
