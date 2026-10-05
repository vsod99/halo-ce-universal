/*
NXDK_NET.C

The socket half of port/linux/src/posix.h over lwIP, which the Xbox's
Winsock layer (port/linux/src/xnet.c, as on Linux) calls: the game's system
link, and its split screen games, which join their own host through
127.0.0.1.

The network starts with the constructors, as the GPU's and the USB stack's
do: the Ethernet driver's rings must be contiguous memory, from the low 64
MB the game state's virtual memory would otherwise use up
(port/xbox/README.md). lwIP is built here with the loopback interface beside
the Ethernet one (lwip_config/lwipopts.h). The address comes from DHCP, in
the background, or with no DHCP server a link-local one (169.254.x.x), as a
retail Xbox's system link has it: until then (or with no cable), the machine
has none, and XNet reports no link, but 127.0.0.1 works from the start.

Winsock's sockaddr_in begins with a 16-bit family, lwIP's with a length and
an 8-bit family; SOL_SOCKET is 0xffff in Winsock and 0xfff in lwIP. The
rest (families, types, protocols and the option numbers) are the same.
*/

#include <xboxkrnl/xboxkrnl.h>
#include <errno.h>
#include <string.h>

#include <lwip/dhcp.h>
#include <lwip/netif.h>
#include <lwip/netifapi.h>
#include <lwip/sockets.h>
#include <lwip/tcpip.h>

#include "posix.h"
#include "nxdk_platform.h"

/* (Winsock's error codes are nxdk's winerror.h's) */
#define WINSOCK_SOL_SOCKET 0xffff
#define WINSOCK_SOCKADDR_IN_SIZE 16

/* nvnetdrv_lwip.c's interface; the driver reads it */
err_t nvnetif_init(struct netif *netif);
struct netif *g_pnetif;
static struct netif ethernet;
static BOOL network_started;
static unsigned char ethernet_address[6];

/* ---------- starting the network */

static void tcpip_ready(void *context)
{
	KeSetEvent((KEVENT *)context, IO_NO_INCREMENT, FALSE);
}

__attribute__((constructor)) static void network_start(void)
{
	KEVENT ready;
	ip4_addr_t none;

	KeInitializeEvent(&ready, SynchronizationEvent, FALSE);
	tcpip_init(tcpip_ready, &ready);
	KeWaitForSingleObject(&ready, Executive, KernelMode, FALSE, NULL);
	network_started = TRUE;

	ip4_addr_set_zero(&none);
	if (netifapi_netif_add(&ethernet, &none, &none, &none, NULL, nvnetif_init, tcpip_input) != ERR_OK)
	{
		platform_log("network: no Ethernet (127.0.0.1 alone)");
		return;
	}
	g_pnetif = &ethernet;
	memcpy(ethernet_address, ethernet.hwaddr, sizeof(ethernet_address));
	netifapi_netif_set_default(&ethernet);
	netifapi_netif_set_up(&ethernet);
	/* (the address comes when it comes: nothing waits for it; with no DHCP
	server, a link-local one, lwipopts.h) */
	netifapi_dhcp_start(&ethernet);
	platform_log("network: Ethernet %02x:%02x:%02x:%02x:%02x:%02x, its address from DHCP",
		ethernet_address[0], ethernet_address[1], ethernet_address[2], ethernet_address[3], ethernet_address[4],
		ethernet_address[5]);
}

const unsigned char *xbox_net_ethernet_address(void)
{
	return ethernet_address;
}

int xbox_net_link_up(void)
{
	return g_pnetif && netif_is_up(g_pnetif) && netif_is_link_up(g_pnetif);
}

posix_ulong posix_local_ipv4_address(void)
{
	static int reported = -1;
	posix_ulong address = 0;
	int state;

	if (g_pnetif && netif_is_up(g_pnetif) && netif_is_link_up(g_pnetif))
		address = ip4_addr_get_u32(netif_ip4_addr(g_pnetif));
	/* (the first answer, and each change between no link, a link with no
	address yet and an address) */
	state = !g_pnetif || !netif_is_link_up(g_pnetif) ? 0 : address ? 2 : 1;
	if (state != reported)
	{
		reported = state;
		if (state == 2)
			platform_log("network: address %lu.%lu.%lu.%lu", address & 0xff, (address >> 8) & 0xff,
				(address >> 16) & 0xff, address >> 24);
		else
			platform_log("network: %s", state ? "link up, no address yet" : "no link");
	}
	return address;
}

/* ---------- errors */

static __thread int last_error;

static int fail(void)
{
	switch (errno)
	{
	case EINTR: last_error = WSAEINTR; break;
	case EBADF: last_error = WSAEBADF; break;
	case EACCES: case EPERM: last_error = WSAEACCES; break;
	case EFAULT: last_error = WSAEFAULT; break;
	case EMFILE: case ENFILE: last_error = WSAEMFILE; break;
	case EWOULDBLOCK: last_error = WSAEWOULDBLOCK; break;
#if EAGAIN != EWOULDBLOCK
	case EAGAIN: last_error = WSAEWOULDBLOCK; break;
#endif
	case EINPROGRESS: last_error = WSAEINPROGRESS; break;
	case EALREADY: last_error = WSAEALREADY; break;
	case ENOTSOCK: last_error = WSAENOTSOCK; break;
	case EDESTADDRREQ: last_error = WSAEDESTADDRREQ; break;
	case EMSGSIZE: last_error = WSAEMSGSIZE; break;
	case EPROTOTYPE: last_error = WSAEPROTOTYPE; break;
	case ENOPROTOOPT: last_error = WSAENOPROTOOPT; break;
	case EPROTONOSUPPORT: last_error = WSAEPROTONOSUPPORT; break;
	case EOPNOTSUPP: last_error = WSAEOPNOTSUPP; break;
	case EAFNOSUPPORT: last_error = WSAEAFNOSUPPORT; break;
	case EADDRINUSE: last_error = WSAEADDRINUSE; break;
	case EADDRNOTAVAIL: last_error = WSAEADDRNOTAVAIL; break;
	case ENETDOWN: last_error = WSAENETDOWN; break;
	case ENETUNREACH: last_error = WSAENETUNREACH; break;
	case ENETRESET: last_error = WSAENETRESET; break;
	case ECONNABORTED: last_error = WSAECONNABORTED; break;
	/* the game takes Winsock's WSAECONNRESET as the connection lost */
	case ECONNRESET: case EPIPE: last_error = WSAECONNRESET; break;
	case ENOBUFS: case ENOMEM: last_error = WSAENOBUFS; break;
	case EISCONN: last_error = WSAEISCONN; break;
	case ENOTCONN: last_error = WSAENOTCONN; break;
	case ETIMEDOUT: last_error = WSAETIMEDOUT; break;
	case ECONNREFUSED: last_error = WSAECONNREFUSED; break;
	case EHOSTUNREACH: last_error = WSAEHOSTUNREACH; break;
	default: last_error = WSAEINVAL; break;
	}
	return -1;
}

static int succeed(int result)
{
	if (result < 0)
		return fail();
	last_error = 0;
	return result;
}

int posix_socket_last_error(void)
{
	return last_error;
}

/* ---------- addresses */

/* a Winsock sockaddr_in as lwIP's; NULL (WSAEFAULT) if it is not one */
static const struct sockaddr *lwip_address(const void *address, int length, struct sockaddr_in *lwip)
{
	unsigned short family;

	if (!address || length < WINSOCK_SOCKADDR_IN_SIZE)
	{
		last_error = WSAEFAULT;
		return NULL;
	}
	memcpy(&family, address, sizeof(family));
	memcpy(lwip, address, sizeof(*lwip));
	lwip->sin_len = sizeof(*lwip);
	lwip->sin_family = (sa_family_t)family;
	return (const struct sockaddr *)lwip;
}

/* lwIP's sockaddr_in back as Winsock's, in the caller's buffer */
static void winsock_address(const struct sockaddr_in *lwip, void *address, int *length)
{
	unsigned short family = lwip->sin_family;
	unsigned char winsock[WINSOCK_SOCKADDR_IN_SIZE];

	if (!address || !length)
		return;
	memcpy(winsock, lwip, sizeof(winsock));
	memcpy(winsock, &family, sizeof(family));
	memcpy(address, winsock, *length < (int)sizeof(winsock) ? (size_t)*length : sizeof(winsock));
	*length = sizeof(winsock);
}

/* ---------- sockets */

int posix_socket(int family, int type, int protocol)
{
	if (!network_started)
	{
		last_error = WSAENETDOWN;
		return -1;
	}
	return succeed(lwip_socket(family, type, protocol));
}

int posix_socket_close(int socket)
{
	return succeed(lwip_close(socket));
}

int posix_socket_bind(int socket, const void *address, int address_length)
{
	struct sockaddr_in local;
	const struct sockaddr *converted = lwip_address(address, address_length, &local);

	if (!converted)
		return -1;
	return succeed(lwip_bind(socket, converted, sizeof(local)));
}

int posix_socket_connect(int socket, const void *address, int address_length)
{
	struct sockaddr_in remote;
	const struct sockaddr *converted = lwip_address(address, address_length, &remote);
	int result;

	if (!converted)
		return -1;
	/* a non-blocking connect under way is Winsock's WSAEWOULDBLOCK, which
	the game waits on (port/linux/src/posix_net.c) */
	result = lwip_connect(socket, converted, sizeof(remote));
	if (result < 0 && errno == EINPROGRESS)
	{
		last_error = WSAEWOULDBLOCK;
		return -1;
	}
	return succeed(result);
}

int posix_socket_listen(int socket, int backlog)
{
	return succeed(lwip_listen(socket, backlog));
}

int posix_socket_accept(int socket, void *address, int *address_length)
{
	struct sockaddr_in remote;
	socklen_t length = sizeof(remote);
	int result = lwip_accept(socket, (struct sockaddr *)&remote, &length);

	if (result >= 0)
		winsock_address(&remote, address, address_length);
	return succeed(result);
}

int posix_socket_send(int socket, const void *buffer, int length, int flags)
{
	return succeed((int)lwip_send(socket, buffer, (size_t)length, flags));
}

int posix_socket_sendto(int socket, const void *buffer, int length, int flags,
	const void *address, int address_length)
{
	struct sockaddr_in remote;
	const struct sockaddr *converted = NULL;

	if (address)
	{
		converted = lwip_address(address, address_length, &remote);
		if (!converted)
			return -1;
	}
	return succeed((int)lwip_sendto(socket, buffer, (size_t)length, flags, converted,
		converted ? sizeof(remote) : 0));
}

int posix_socket_recv(int socket, void *buffer, int length, int flags)
{
	return succeed((int)lwip_recv(socket, buffer, (size_t)length, flags));
}

int posix_socket_recvfrom(int socket, void *buffer, int length, int flags,
	void *address, int *address_length)
{
	struct sockaddr_in remote;
	struct iovec vector;
	struct msghdr message;
	int result;

	vector.iov_base = buffer;
	vector.iov_len = (size_t)length;
	memset(&message, 0, sizeof(message));
	memset(&remote, 0, sizeof(remote));
	message.msg_name = &remote;
	message.msg_namelen = sizeof(remote);
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	result = (int)lwip_recvmsg(socket, &message, flags);
	if (result >= 0 && address && address_length)
		winsock_address(&remote, address, address_length);
	/* a datagram larger than the buffer is an error in Winsock */
	if (result >= 0 && (message.msg_flags & MSG_TRUNC))
	{
		last_error = WSAEMSGSIZE;
		return -1;
	}
	return succeed(result);
}

int posix_socket_shutdown(int socket, int how)
{
	return succeed(lwip_shutdown(socket, how));
}

int posix_socket_set_nonblocking(int socket, int nonblocking)
{
	u32_t value = nonblocking != 0;

	return succeed(lwip_ioctl(socket, (long)FIONBIO, &value));
}

int posix_socket_set_nodelay(int socket)
{
	int value = 1;

	return succeed(lwip_setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)));
}

int posix_socket_bytes_available(int socket, posix_ulong *count)
{
	int available = 0;
	int result = lwip_ioctl(socket, (long)FIONREAD, &available);

	if (result >= 0)
		*count = (posix_ulong)available;
	return succeed(result);
}

int posix_socket_setsockopt(int socket, int level, int name, const void *value, int length)
{
	if (level == WINSOCK_SOL_SOCKET)
	{
		/* lwIP has no send buffer to size (sends wait on its window) */
		if (name == SO_SNDBUF)
		{
			last_error = 0;
			return 0;
		}
		level = SOL_SOCKET;
	}
	else if (level != IPPROTO_IP && level != IPPROTO_TCP && level != IPPROTO_UDP)
	{
		/* Xbox-only options such as SO_ENCRYPT have nothing to do here */
		last_error = 0;
		return 0;
	}
	return succeed(lwip_setsockopt(socket, level, name, value, (socklen_t)length));
}

int posix_socket_getsockopt(int socket, int level, int name, void *value, int *length)
{
	socklen_t socket_length = (socklen_t)*length;
	int result;

	if (level == WINSOCK_SOL_SOCKET)
	{
		/* (what a send may queue: setting it does nothing) */
		if (name == SO_SNDBUF && *length >= (int)sizeof(int))
		{
			*(int *)value = TCP_SND_BUF;
			*length = sizeof(int);
			last_error = 0;
			return 0;
		}
		level = SOL_SOCKET;
	}
	result = lwip_getsockopt(socket, level, name, value, &socket_length);
	*length = (int)socket_length;
	return succeed(result);
}

int posix_socket_getsockname(int socket, void *address, int *address_length)
{
	struct sockaddr_in local;
	socklen_t length = sizeof(local);
	int result = lwip_getsockname(socket, (struct sockaddr *)&local, &length);

	if (result >= 0)
		winsock_address(&local, address, address_length);
	return succeed(result);
}

int posix_socket_getpeername(int socket, void *address, int *address_length)
{
	struct sockaddr_in remote;
	socklen_t length = sizeof(remote);
	int result = lwip_getpeername(socket, (struct sockaddr *)&remote, &length);

	if (result >= 0)
		winsock_address(&remote, address, address_length);
	return succeed(result);
}

/* poll with select's readiness, as posix_net.c does it: read for data, the
end or an error, write for room (a connect done) or an error, error for a
connect that failed */
int posix_socket_select(int *read, int *read_count, int *write, int *write_count,
	int *error, int *error_count, posix_long timeout_seconds, posix_long timeout_microseconds, int infinite)
{
	enum { MAXIMUM_DESCRIPTORS = MEMP_NUM_NETCONN * 3 };
	static const short events[3] = { POLLIN, POLLOUT, 0 };
	static const short ready[3] = { POLLIN | POLLHUP | POLLERR, POLLOUT | POLLERR, POLLERR };
	struct pollfd descriptors[MAXIMUM_DESCRIPTORS];
	int *lists[3] = { read, write, error };
	int *counts[3] = { read_count, write_count, error_count };
	long long milliseconds = (long long)timeout_seconds * 1000 + ((long long)timeout_microseconds + 999) / 1000;
	int total = 0;
	int list, index;
	int result;

	for (list = 0; list < 3; list++)
	{
		if (!lists[list] || !counts[list])
			continue;
		for (index = 0; index < *counts[list]; index++)
		{
			if (total == MAXIMUM_DESCRIPTORS)
			{
				last_error = WSAEINVAL;
				return -1;
			}
			descriptors[total].fd = lists[list][index];
			descriptors[total].events = events[list];
			descriptors[total].revents = 0;
			total++;
		}
	}
	result = lwip_poll(descriptors, (nfds_t)total, infinite ? -1 :
		(int)(milliseconds < 0 ? 0 : milliseconds > 0x7fffffff ? 0x7fffffff : milliseconds));
	for (index = 0; index < total && result > 0; index++)
	{
		if (descriptors[index].revents & POLLNVAL)
		{
			errno = EBADF;
			result = -1;
		}
	}
	if (result < 0)
		return fail();
	result = 0;
	total = 0;
	for (list = 0; list < 3; list++)
	{
		int kept = 0;

		if (!lists[list] || !counts[list])
			continue;
		for (index = 0; index < *counts[list]; index++, total++)
		{
			int descriptor = lists[list][index];
			int pending = 0;
			socklen_t length = sizeof(pending);

			if (!(descriptors[total].revents & ready[list]))
				continue;
			/* writeable is connected to the game: a connect that failed
			is not (posix_net.c) */
			if (list == 1 && lwip_getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &pending, &length) == 0 && pending)
			{
				errno = pending;
				fail();
				continue;
			}
			/* and in the error set only a connect that failed */
			if (list == 2 && !(lwip_getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &pending, &length) == 0 && pending))
				continue;
			lists[list][kept++] = descriptor;
		}
		*counts[list] = kept;
		result += kept;
	}
	if (result > 0)
		last_error = 0;
	return result;
}
