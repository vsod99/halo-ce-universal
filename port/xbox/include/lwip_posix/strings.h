/* POSIX <strings.h> for the Xbox's UPnP and its self-updater's download
(port/linux/src/posix_update.c): nxdk's (ffs), strncasecmp and strcasecmp,
which nxdk's C library calls _strnicmp and _stricmp, and strcasestr, which
it has not */

#ifndef __HALO_XBOX_LWIP_POSIX_STRINGS_H
#define __HALO_XBOX_LWIP_POSIX_STRINGS_H

#include_next <strings.h>
#include <string.h>

#define strncasecmp _strnicmp
#define strcasecmp _stricmp

static __inline__ char *strcasestr(const char *text, const char *wanted)
{
	size_t length = strlen(wanted);

	for (; *text; text++)
	{
		if (!_strnicmp(text, wanted, length))
			return (char *)text;
	}
	return length ? NULL : (char *)text;
}

#endif
