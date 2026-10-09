/*
LWIPOPTS.H

lwIP's options for the Xbox build (nxdk_net.c), which compiles lwIP itself
(tools/xbox_build.py): nxdk's own (lib/net/nforceif/include/lwipopts.h),
and what the game's system link needs of them besides.
*/

#ifndef __HALO_XBOX_LWIPOPTS_H
#define __HALO_XBOX_LWIPOPTS_H

#include_next <lwipopts.h>

/* a host joins its own game through 127.0.0.1, and a split screen game is
nothing but that: the loopback interface beside the Ethernet one, and
datagrams to this machine's own address looped back too */
#undef LWIP_SINGLE_NETIF
#define LWIP_SINGLE_NETIF 0
#undef LWIP_HAVE_LOOPIF
#define LWIP_HAVE_LOOPIF 1
#define LWIP_NETIF_LOOPBACK 1

/* with no DHCP server (two consoles and a cable, or xemu without its NAT),
a link-local address, as a retail Xbox's system link has: claimed once the
first DHCP request goes unanswered (2 s), so it is there 7-8 s after
start-up, inside the 10 s the game's transport waits for an address
(transport_endpoint_set_winsock.c). After two tries it came after 12 s,
too late; claimed from the first request, it beat xemu's NAT's DHCP answer,
and the transport kept the link-local address it no longer had. */
#define LWIP_DHCP_AUTOIP_COOP 1
#define LWIP_DHCP_AUTOIP_COOP_TRIES 1
/* the address in the log when it comes or changes (nxdk_net.c) */
#define LWIP_NETIF_STATUS_CALLBACK 1

/* the sockets of a system link game: its listening one, a connection for
each machine, and the datagram ones that find and advertise games (the
default is 4) */
#define MEMP_NUM_NETCONN 32
/* FIONREAD (Winsock's ioctlsocket), SO_RCVBUF and SO_REUSEADDR */
#define LWIP_SO_RCVBUF 1
#define SO_REUSE 1
/* the platform layer calls lwip_socket and the rest by those names */
#define LWIP_COMPAT_SOCKETS 0
#define LWIP_POSIX_SOCKETS_IO_NAMES 0

#endif
