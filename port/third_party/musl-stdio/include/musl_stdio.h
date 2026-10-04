/*
MUSL_STDIO.H

Force-included ahead of each of the musl files in ../src (-include), which
are musl's unchanged. Every name they define or call that a C library also
has gets a halo_musl_ prefix, so they sit beside the platform's own C
library (nxdk's pdclib on the Xbox) and the platform layer chooses what to
call them for (port/xbox/src/nxdk_libc.c).
*/

#ifndef __HALO_MUSL_STDIO_H
#define __HALO_MUSL_STDIO_H

#include <stddef.h>

/* stdio */
#define vfprintf halo_musl_vfprintf
#define vsnprintf halo_musl_vsnprintf
#define fwrite halo_musl_fwrite
#define fwrite_unlocked halo_musl_fwrite_unlocked
#define __fwritex halo_musl_fwritex
#define __towrite halo_musl_towrite
#define __towrite_needs_stdio_exit halo_musl_towrite_needs_stdio_exit
#define __stdio_exit_needed halo_musl_stdio_exit_needed
#define __uflow halo_musl_uflow

/* strtod and the scanner behind it */
#define strtof halo_musl_strtof
#define strtod halo_musl_strtod
#define strtold halo_musl_strtold
#define __floatscan halo_musl_floatscan
#define __shlim halo_musl_shlim
#define __shgetc halo_musl_shgetc

/* the exact maths they need, musl's too (a C library's may be wrong:
pdclib's fmod takes one partial remainder, its scalbn asserts) */
#define fmod halo_musl_fmod
#define fmodf halo_musl_fmodf
#define scalbn halo_musl_scalbn
#define scalbnf halo_musl_scalbnf
#define frexp halo_musl_frexp
#define frexpf halo_musl_frexpf

/* what the C library may not have (support.c) */
#define strnlen halo_musl_strnlen
#define wctomb halo_musl_wctomb
size_t halo_musl_strnlen(const char *string, size_t maximum);
int halo_musl_wctomb(char *s, wchar_t wc);

/* musl's <limits.h>: the positional arguments printf takes (%1$d) */
#define NL_ARGMAX 9

/* musl's visibility and aliases: nothing here is exported anyway, and no
alias is called */
#define hidden
#define weak_alias(old, new)

#endif /* __HALO_MUSL_STDIO_H */
