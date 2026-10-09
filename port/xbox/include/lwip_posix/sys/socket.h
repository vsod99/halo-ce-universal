/*
SYS/SOCKET.H

BSD sockets for the Xbox's UPnP (port/xbox/src/nxdk_upnp.c, with
port/third_party/miniupnpc), which is written for them: lwIP's sockets by
their POSIX names. The rest of the build calls lwIP by its own names
(lwip_config/lwipopts.h: LWIP_COMPAT_SOCKETS 0), so the names are these
headers' (lwIP's own list, lwip/sockets.h), for the UPnP units alone. The
other POSIX networking headers include this one.
*/

#ifndef __HALO_XBOX_LWIP_POSIX_SOCKET_H
#define __HALO_XBOX_LWIP_POSIX_SOCKET_H

#include <string.h>
#include <sys/types.h>

#include <lwip/inet.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#define accept(s, addr, addrlen) lwip_accept(s, addr, addrlen)
#define bind(s, name, namelen) lwip_bind(s, name, namelen)
#define shutdown(s, how) lwip_shutdown(s, how)
#define getpeername(s, name, namelen) lwip_getpeername(s, name, namelen)
#define getsockname(s, name, namelen) lwip_getsockname(s, name, namelen)
#define setsockopt(s, level, name, value, length) lwip_setsockopt(s, level, name, value, length)
#define getsockopt(s, level, name, value, length) lwip_getsockopt(s, level, name, value, length)
#define connect(s, name, namelen) lwip_connect(s, name, namelen)
#define listen(s, backlog) lwip_listen(s, backlog)
#define recv(s, mem, len, flags) lwip_recv(s, mem, len, flags)
#define recvfrom(s, mem, len, flags, from, fromlen) lwip_recvfrom(s, mem, len, flags, from, fromlen)
#define send(s, data, size, flags) lwip_send(s, data, size, flags)
#define sendto(s, data, size, flags, to, tolen) lwip_sendto(s, data, size, flags, to, tolen)
#define socket(domain, type, protocol) lwip_socket(domain, type, protocol)
#define select(maxfdp1, readset, writeset, exceptset, timeout) \
	lwip_select(maxfdp1, readset, writeset, exceptset, timeout)
#define poll(fds, nfds, timeout) lwip_poll(fds, nfds, timeout)
/* (miniupnpc passes SIOCGIFADDR's a length too) */
#define ioctl(s, cmd, argp, ...) lwip_ioctl(s, cmd, argp)
#define fcntl(s, cmd, value) lwip_fcntl(s, cmd, value)
#define inet_ntop(af, src, dst, size) lwip_inet_ntop(af, src, dst, size)
#define inet_pton(af, src, dst) lwip_inet_pton(af, src, dst)
#define getaddrinfo(name, service, hints, result) lwip_getaddrinfo(name, service, hints, result)
#define freeaddrinfo(list) lwip_freeaddrinfo(list)
/* (miniupnpc's closesocket is close, which is the file descriptors'
otherwise: port/xbox/src/nxdk_posix.c) */
#define close(s) lwip_close(s)

/* no local (Unix) sockets: socket() fails for them (miniupnpc asks the
minissdpd daemon through one, and then does its own SSDP search) */
#define AF_UNIX 1

/* named only by miniupnpc's branches for IPv6 and for a multicast interface
given by name, which posix_upnp.c asks for neither of (lwIP has neither):
lwIP rejects these options and ioctl, and there are no interface names */
#define IPV6_MULTICAST_IF 17
#define IPV6_MULTICAST_HOPS 18
#define SIOCGIFADDR 0x8915
#define ifr_addr ifr_name
static inline unsigned int if_nametoindex(const char *name)
{
	(void)name;
	return 0;
}

/* getnameinfo, numeric only (NI_NUMERICHOST; port/xbox/src/nxdk_upnp.c),
which lwIP lacks */
#define NI_MAXHOST 64
#define NI_MAXSERV 8
#define NI_NUMERICHOST 1
#define NI_NUMERICSERV 2
int getnameinfo(const struct sockaddr *address, socklen_t address_length, char *host, socklen_t host_size,
	char *service, socklen_t service_size, int flags);
const char *gai_strerror(int error);

#endif
