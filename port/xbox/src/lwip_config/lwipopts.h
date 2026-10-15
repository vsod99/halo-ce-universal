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
each machine, and the datagram ones that find and advertise games, about ten
besides the machines' (the default is 4). The socket table is the one fixed
count here (nxdk's lwIP takes its pools from the heap, MEMP_MEM_MALLOC): 400
entries, about 10 KB, for a host's 127 other machines and the test clients
that stand in for them, three sockets each
(port/linux/game/network_test_clients.c); 32 ran out at the seventh. */
#define MEMP_NUM_NETCONN 400
/* what lwIP holds of the kernel's pool (nxdk's mem_malloc), counted for
the frame rate's log line (nxdk_net.c xbox_net_lwip_kb): a size before each
allocation. A host's buffers for many clients grow there, beside what the
kernel itself needs of the pool for the map's reads. */
#define MEM_STATS 1
/* and their datagram ones beside DHCP's, DNS's and the log's (nxdk_net.c)
(the default is 4) */
#define MEMP_NUM_UDP_PCB 8
/* FIONREAD (Winsock's ioctlsocket), SO_RCVBUF and SO_REUSEADDR */
#define LWIP_SO_RCVBUF 1
#define SO_REUSE 1
/* SO_RCVTIMEO and SO_SNDTIMEO: UPnP's requests to the router
(nxdk_upnp.c) give up on one that does not answer */
#define LWIP_SO_RCVTIMEO 1
#define LWIP_SO_SNDTIMEO 1
/* the frames the NIC receives, copied out of its buffers into these
(xbox_nvnetdrv_init, nxdk_net.c), a frame to a buffer: as many as a TCP
window, which a game that reads nothing for a while (loading a map) leaves
lwIP holding, and the datagrams that come meanwhile; when they run out, a
frame is dropped and lwIP lets go of what it holds out of order. lwIP's
default is 16 of 1488 bytes. (nvnetdrv_lwip.c also takes the count for the
NIC's send descriptors.) */
#define PBUF_POOL_SIZE 48
#define PBUF_POOL_BUFSIZE 1536

/* the platform layer calls lwip_socket and the rest by those names */
#define LWIP_COMPAT_SOCKETS 0
#define LWIP_POSIX_SOCKETS_IO_NAMES 0

#endif
