/* POSIX <strings.h> for the Xbox's UPnP: nxdk's (ffs), and strncasecmp,
which nxdk's C library calls _strnicmp */

#ifndef __HALO_XBOX_LWIP_POSIX_STRINGS_H
#define __HALO_XBOX_LWIP_POSIX_STRINGS_H

#include_next <strings.h>
#include <string.h>

#define strncasecmp _strnicmp
#define strcasecmp _stricmp

#endif
