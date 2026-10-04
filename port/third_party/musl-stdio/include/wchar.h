/*
WCHAR.H

The <wchar.h> the musl files in ../src see: the types alone (vfprintf's
%lc and %ls convert through wctomb, musl_stdio.h). Written for this port,
not musl's.
*/

#ifndef __HALO_MUSL_STDIO_WCHAR_H
#define __HALO_MUSL_STDIO_WCHAR_H

#include <stddef.h>

typedef unsigned int wint_t;

#endif /* __HALO_MUSL_STDIO_WCHAR_H */
