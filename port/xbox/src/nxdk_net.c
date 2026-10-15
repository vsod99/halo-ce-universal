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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lwip/api.h>
#include <lwip/dhcp.h>
#include <lwip/dns.h>
#include <lwip/netif.h>
#include <lwip/netifapi.h>
#include <lwip/pbuf.h>
#include <lwip/stats.h>
#include <lwip/sockets.h>
#include <lwip/tcpip.h>

#include "posix.h"
#include "port_config.h"
#include "nxdk_platform.h"
#include "xbox_log.h"
#include "nvnetdrv.h"

/* (Winsock's error codes are nxdk's winerror.h's) */
#define WINSOCK_SOL_SOCKET 0xffff
#define WINSOCK_SOCKADDR_IN_SIZE 16

/* nvnetdrv_lwip.c's interface; the driver reads it */
err_t nvnetif_init(struct netif *netif);
struct netif *g_pnetif;
static struct netif ethernet;
static BOOL network_started;
static unsigned char ethernet_address[6];

/* ---------- the frames received */

/* The NIC's receive buffers, which nxdk's driver lends lwIP for as long as
lwIP holds a frame (nvnetdrv_lwip.c: zero-copy), are taken back at once
here: each frame is copied into lwIP's pool (lwipopts.h) as it comes. Lent,
they ran out while a game read nothing for a while (a large map loading, 14
s) and the host's backlog came in at once, and xemu's NIC, whose ring was
full, never received again (it does not look at its ring again when the
driver gives buffers back); a console's drops frames until it has some. */
enum
{
	/* the NIC's ring: only between the frame's arrival and its copy now
	(nxdk's default, a TCP window's, was 64) */
	ETHERNET_RECEIVE_BUFFERS = 32,
};

static void ethernet_received(void *buffer, uint16_t length)
{
	struct pbuf *frame = pbuf_alloc(PBUF_RAW, (u16_t)(length + ETH_PAD_SIZE), PBUF_POOL);

	if (frame)
		pbuf_take_at(frame, buffer, length, ETH_PAD_SIZE);
	nvnetdrv_rx_release(buffer);
	/* (the pool spent: dropped, as a full ring would, and lwIP lets go of
	what it holds out of order) */
	if (!frame)
	{
		LINK_STATS_INC(link.memerr);
		LINK_STATS_INC(link.drop);
		return;
	}
	LINK_STATS_INC(link.recv);
	if (ethernet.input(frame, &ethernet) != ERR_OK)
		pbuf_free(frame);
}

/* (nvnetdrv_lwip.c's call to nvnetdrv_init, by tools/xbox_build.py: its own
callback and ring size not taken) */
int xbox_nvnetdrv_init(size_t rx_buffer_count, nvnetdrv_rx_callback_t rx_callback, size_t tx_queue_size)
{
	(void)rx_buffer_count;
	(void)rx_callback;
	return nvnetdrv_init(ETHERNET_RECEIVE_BUFFERS, ethernet_received, tx_queue_size);
}

/* ---------- starting the network */

static void tcpip_ready(void *context)
{
	KeSetEvent((KEVENT *)context, IO_NO_INCREMENT, FALSE);
}

/* (lwIP's thread, each time the interface's address or state changes) */
static void ethernet_status_changed(struct netif *netif)
{
	posix_ulong address = ip4_addr_get_u32(netif_ip4_addr(netif));
	const ip_addr_t *dns = dns_getserver(0);
	posix_ulong dns_address = dns && IP_IS_V4(dns) ? ip4_addr_get_u32(ip_2_ip4(dns)) : 0;

	if (address)
		platform_log("network: Ethernet address %lu.%lu.%lu.%lu (%s), DNS %lu.%lu.%lu.%lu", address & 0xff,
			(address >> 8) & 0xff, (address >> 16) & 0xff, address >> 24,
			(address & 0xffff) == 0xfea9 ? "link-local" : "DHCP", dns_address & 0xff, (dns_address >> 8) & 0xff,
			(dns_address >> 16) & 0xff, dns_address >> 24);
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
	netif_set_status_callback(&ethernet, ethernet_status_changed);
	netifapi_netif_set_default(&ethernet);
	netifapi_netif_set_up(&ethernet);
	/* (the address comes when it comes: nothing waits for it; with no DHCP
	server, a link-local one, lwipopts.h) */
	netifapi_dhcp_start(&ethernet);
	platform_log("network: Ethernet %02x:%02x:%02x:%02x:%02x:%02x, its address from DHCP",
		ethernet_address[0], ethernet_address[1], ethernet_address[2], ethernet_address[3], ethernet_address[4],
		ethernet_address[5]);
}

/* ---------- test clients' addresses */

/* debug.network_test_clients (port/linux/game/network_test_clients.c): each
test client joins its own host from an address of its own, 127.0.0.2 and on
(the host tells machines apart by their addresses). lwIP's loopback
interface takes datagrams to 127.0.0.1 alone, so each address is an
interface of its own, which lwIP routes first (the newest) and loops back
(ip4_output_if: a datagram to an interface's own address); the host's
answers come from that address, which the client talks to. */
enum
{
	/* a game's machines (HALO_PORT_MAXIMUM_NETWORK_MACHINES) */
	MAXIMUM_LOOPBACK_ADDRESSES = 128,
};

/* (made as they are asked for: none in a game without test clients) */
static struct netif *loopback_addresses[MAXIMUM_LOOPBACK_ADDRESSES];
static int loopback_address_count;

/* (nothing goes out of it but to its own address, looped back before this) */
static err_t loopback_address_output(struct netif *netif, struct pbuf *p, const ip4_addr_t *address)
{
	(void)netif;
	(void)p;
	(void)address;
	return ERR_OK;
}

static err_t loopback_address_init(struct netif *netif)
{
	netif->name[0] = 't';
	netif->name[1] = 'c';
	netif->mtu = 0;
	netif->output = loopback_address_output;
	return ERR_OK;
}

/* the address (network byte order) this machine's own, on an interface of
its own; 0 if there is no room for another */
int xbox_net_add_loopback_address(unsigned long address)
{
	ip4_addr_t ip;
	ip4_addr_t mask;
	ip4_addr_t gateway;
	int index;

	struct netif *netif;

	for (index = 0; index < loopback_address_count; index++)
	{
		if (ip4_addr_get_u32(netif_ip4_addr(loopback_addresses[index])) == address)
			return 1;
	}
	if (!network_started || loopback_address_count >= MAXIMUM_LOOPBACK_ADDRESSES ||
		!(netif = calloc(1, sizeof(*netif))))
	{
		return 0;
	}
	ip4_addr_set_u32(&ip, address);
	ip4_addr_set_u32(&mask, 0xffffffffUL);
	ip4_addr_set_zero(&gateway);
	if (netifapi_netif_add(netif, &ip, &mask, &gateway, NULL, loopback_address_init, tcpip_input) != ERR_OK)
	{
		platform_log("network: could not add the address %lu.%lu.%lu.%lu", address & 0xff, (address >> 8) & 0xff,
			(address >> 16) & 0xff, address >> 24);
		free(netif);
		return 0;
	}
	netifapi_netif_set_link_up(netif);
	netifapi_netif_set_up(netif);
	loopback_addresses[loopback_address_count++] = netif;
	return 1;
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

/* (lwIP's DNS, from the DHCP server: none with a link-local address) */
posix_ulong posix_resolve_ipv4(const char *host)
{
	ip_addr_t address;

	if (!network_started || netconn_gethostbyname_addrtype(host, &address, NETCONN_DNS_IPV4) != ERR_OK ||
		!IP_IS_V4(&address))
	{
		return 0;
	}
	return ip4_addr_get_u32(ip_2_ip4(&address));
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

/* ---------- the log over the network */

/* debug.log_address: the log's lines as UDP datagrams, for a console
without the debug kits' COM2 (port/xbox/common/xbox_log.c), which
tools/xbox_dev.py log shows. Lines go into a ring from the start (the tap),
and a thread of its own sends them once the machine has an address and the
settings say where; with none set, the tap is taken away. A full ring drops
what comes after, and says how much once there is room. */

#define NETWORK_LOG_PORT 2324
#define NETWORK_LOG_RING_BYTES (32 * 1024)
#define NETWORK_LOG_DATAGRAM_BYTES 1400

static struct
{
	char ring[NETWORK_LOG_RING_BYTES];
	/* (bytes ever written and sent: the difference is in the ring) */
	volatile unsigned long written, sent;
	volatile unsigned long dropped;
} network_log;

/* (any thread: the raised IRQL keeps the others out on the one processor) */
void xbox_network_log_tap(const char *text)
{
	unsigned long length = strlen(text);
	unsigned long index;
	KIRQL irql = KeRaiseIrqlToDpcLevel();

	if (network_log.written - network_log.sent + length > NETWORK_LOG_RING_BYTES)
	{
		network_log.dropped += length;
	}
	else
	{
		for (index = 0; index < length; index++)
			network_log.ring[(network_log.written + index) % NETWORK_LOG_RING_BYTES] = text[index];
		network_log.written += length;
	}
	KfLowerIrql(irql);
}

/* the address and port debug.log_address names; false for none (or a
mistake, logged) */
static int network_log_destination(struct sockaddr_in *destination)
{
	const char *setting = config_string("debug.log_address");
	const char *colon = strchr(setting, ':');
	size_t length = colon ? (size_t)(colon - setting) : strlen(setting);
	char host[64];
	ip4_addr_t address;

	if (!*setting)
		return 0;
	if (length >= sizeof(host))
		length = sizeof(host) - 1;
	memcpy(host, setting, length);
	host[length] = '\0';
	memset(destination, 0, sizeof(*destination));
	destination->sin_len = sizeof(*destination);
	destination->sin_family = AF_INET;
	destination->sin_port = lwip_htons(colon ? (u16_t)atoi(colon + 1) : NETWORK_LOG_PORT);
	if (!strcmp(host, "broadcast"))
	{
		destination->sin_addr.s_addr = IPADDR_BROADCAST;
	}
	else if (ip4addr_aton(host, &address))
	{
		destination->sin_addr.s_addr = ip4_addr_get_u32(&address);
	}
	else
	{
		platform_log("network log: debug.log_address \"%s\" is not an IPv4 address or \"broadcast\" (either "
			"with :port)", setting);
		return 0;
	}
	return 1;
}

/* the dropped bytes' count, zeroed */
static unsigned long network_log_take_dropped(void)
{
	KIRQL irql = KeRaiseIrqlToDpcLevel();
	unsigned long dropped = network_log.dropped;

	network_log.dropped = 0;
	KfLowerIrql(irql);
	return dropped;
}

static DWORD WINAPI network_log_thread(LPVOID parameter)
{
	struct sockaddr_in destination;
	char datagram[NETWORK_LOG_DATAGRAM_BYTES + 96];
	int descriptor;
	int yes = 1;

	(void)parameter;
	/* the settings are read once the address comes, by when the game has
	read them (read here first, they would look for the data too early:
	before the screen asking for the disc, nxdk_main.c) */
	while (!posix_local_ipv4_address())
		Sleep(250);
	if (!network_log_destination(&destination))
	{
		xbox_log_set_tap(NULL);
		return 0;
	}
	descriptor = lwip_socket(AF_INET, SOCK_DGRAM, 0);
	if (descriptor < 0)
	{
		xbox_log_set_tap(NULL);
		platform_log("network log: no socket (%d)", errno);
		return 0;
	}
	lwip_setsockopt(descriptor, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
	platform_log("network log: to %s:%u", ip4addr_ntoa((const ip4_addr_t *)&destination.sin_addr),
		(unsigned)lwip_ntohs(destination.sin_port));
	for (;;)
	{
		unsigned long available = network_log.written - network_log.sent;
		unsigned long length = 0;
		unsigned long dropped;

		if (!available)
		{
			Sleep(50);
			continue;
		}
		/* whole lines, as many as fit (a longer one is cut) */
		while (length < available && length < NETWORK_LOG_DATAGRAM_BYTES)
		{
			datagram[length] = network_log.ring[(network_log.sent + length) % NETWORK_LOG_RING_BYTES];
			length++;
		}
		if (length < available)
		{
			unsigned long end = length;

			while (end && datagram[end - 1] != '\n')
				end--;
			if (end)
				length = end;
		}
		network_log.sent += length;
		dropped = length == available ? network_log_take_dropped() : 0;
		if (dropped)
			length += (unsigned long)snprintf(datagram + length, sizeof(datagram) - length,
				"network log: %lu bytes of lines dropped (the ring was full)\n", dropped);
		lwip_sendto(descriptor, datagram, length, 0, (struct sockaddr *)&destination, sizeof(destination));
	}
}

__attribute__((constructor)) static void network_log_start(void)
{
	HANDLE thread;

	/* (the tap is there from the first line: nxdk_main.c) */
	thread = CreateThread(NULL, 16 * 1024, network_log_thread, NULL, 0, NULL);
	if (!thread)
	{
		xbox_log_set_tap(NULL);
		return;
	}
	SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
	CloseHandle(thread);
}
