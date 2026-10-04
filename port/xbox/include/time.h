/*
TIME.H

nxdk's C library's <time.h>, plus the POSIX localtime_r and gmtime_r
(port/xbox/src/nxdk_posix.c, with C11's localtime_s and gmtime_s).
*/

#ifndef __HALO_XBOX_TIME_H
#define __HALO_XBOX_TIME_H

#include_next <time.h>

struct tm *localtime_r(const time_t *timer, struct tm *result);
struct tm *gmtime_r(const time_t *timer, struct tm *result);

#endif /* __HALO_XBOX_TIME_H */
