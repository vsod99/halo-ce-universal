/*
STDIO.H

nxdk's C library's <stdio.h>, plus the POSIX descriptor functions it lacks
(port/xbox/src/nxdk_posix.c): its streams have no file descriptors, so a
stream is not made from one (zlib's gzdopen and the game's ufdopen, which
nothing calls, would fail).
*/

#ifndef __HALO_XBOX_STDIO_H
#define __HALO_XBOX_STDIO_H

#include_next <stdio.h>

FILE *fdopen(int descriptor, const char *mode);
int fileno(FILE *stream);

#endif /* __HALO_XBOX_STDIO_H */
