/* POSIX <sys/un.h> for the Xbox's UPnP: lwIP has no local sockets, and a
connection to one fails (miniupnpc's to the minissdpd daemon), but the
address is still named */

#ifndef __HALO_XBOX_LWIP_POSIX_SYS_UN_H
#define __HALO_XBOX_LWIP_POSIX_SYS_UN_H

#include <sys/socket.h>

struct sockaddr_un
{
	u8_t sun_len;
	sa_family_t sun_family;
	char sun_path[108];
};

#endif
