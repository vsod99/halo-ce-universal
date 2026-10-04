/*
FCNTL.H

The <fcntl.h> nxdk's C library (pdclib) does not have, with MSVC's values,
under port/linux/include/fcntl.h (which adds the MSVC names and sends
open() to the platform layer). The Xbox build's own posix layer implements
open() (port/xbox/src).
*/

#ifndef __HALO_XBOX_FCNTL_H
#define __HALO_XBOX_FCNTL_H

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR 0x0002
#define O_ACCMODE (O_RDONLY | O_WRONLY | O_RDWR)
#define O_APPEND 0x0008
#define O_CREAT 0x0100
#define O_TRUNC 0x0200
#define O_EXCL 0x0400
/* no processes to inherit files */
#define O_CLOEXEC 0

int open(const char *path, int flags, ...);

#endif /* __HALO_XBOX_FCNTL_H */
