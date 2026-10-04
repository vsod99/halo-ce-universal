/*
SYS/TYPES.H

MSVC's <sys/types.h>, which nxdk's C library (pdclib) does not have: the
types port/linux/include/sys/stat.h, zlib and libtiff take from it.
*/

#ifndef __HALO_XBOX_SYS_TYPES_H
#define __HALO_XBOX_SYS_TYPES_H

#include <time.h>

typedef unsigned short _ino_t;
typedef unsigned int _dev_t;
typedef long _off_t;
typedef _ino_t ino_t;
typedef _dev_t dev_t;
typedef _off_t off_t;

#endif /* __HALO_XBOX_SYS_TYPES_H */
