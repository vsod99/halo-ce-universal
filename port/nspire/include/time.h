/*
TIME.H

newlib's <time.h> for the game on the TI-Nspire, with MSVC's 32-bit time_t.

This newlib's time_t is 64 bits, and the game, written for MSVC, keeps
times in longs (errors.c passes a long's address to time and localtime) and
in structures laid out for 32 bits. For the game's code time_t is a long,
and the calls that take one go to 32-bit wrappers (posix_nspire.c). The
platform layer keeps newlib's own.
*/

#ifndef __HALO_NSPIRE_TIME_H
#define __HALO_NSPIRE_TIME_H

#include_next <time.h>

#ifndef HALO_LINUX_PLATFORM_LAYER

struct tm;

long halo_nspire_time(long *result);
struct tm *halo_nspire_localtime(const long *time);
struct tm *halo_nspire_gmtime(const long *time);
long halo_nspire_mktime(struct tm *time);
char *halo_nspire_ctime(const long *time);

#define time_t long
#define time(result) halo_nspire_time(result)
#define localtime(value) halo_nspire_localtime(value)
#define gmtime(value) halo_nspire_gmtime(value)
#define mktime(value) halo_nspire_mktime(value)
#define ctime(value) halo_nspire_ctime(value)

#endif

#endif
