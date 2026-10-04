/*
XNET_NULL.C

Winsock and XNet for the original Xbox until its network card has a socket
layer (port/xbox/README.md, phase 4). Start-up succeeds, the Ethernet link
is down and no address comes, so the game's transport layer reports the
network unavailable; a socket cannot be made.
*/

#include "platform.h"

#include <string.h>

static int winsock_last_error;

int WSAAPI WSAGetLastError(void)
{
	return winsock_last_error;
}

void WSAAPI WSASetLastError(int error)
{
	winsock_last_error = error;
}

int WSAAPI WSAStartup(WORD version_requested, LPWSADATA data)
{
	if (data)
	{
		memset(data, 0, sizeof(*data));
		data->wVersion = version_requested;
		data->wHighVersion = MAKEWORD(2, 2);
	}
	return 0;
}

int WSAAPI WSACleanup(void)
{
	return 0;
}

static int network_down(void)
{
	winsock_last_error = WSAENETDOWN;
	return SOCKET_ERROR;
}

SOCKET WSAAPI halo_ws_socket(int family, int type, int protocol)
{
	(void)family;
	(void)type;
	(void)protocol;
	winsock_last_error = WSAENETDOWN;
	return INVALID_SOCKET;
}

int WSAAPI halo_ws_closesocket(SOCKET socket)
{
	(void)socket;
	return 0;
}

int WSAAPI halo_ws_bind(SOCKET socket, const struct sockaddr *address, int address_length)
{
	(void)socket;
	(void)address;
	(void)address_length;
	return network_down();
}

int WSAAPI halo_ws_connect(SOCKET socket, const struct sockaddr *address, int address_length)
{
	(void)socket;
	(void)address;
	(void)address_length;
	return network_down();
}

int WSAAPI halo_ws_listen(SOCKET socket, int backlog)
{
	(void)socket;
	(void)backlog;
	return network_down();
}

SOCKET WSAAPI halo_ws_accept(SOCKET socket, struct sockaddr *address, int *address_length)
{
	(void)socket;
	(void)address;
	(void)address_length;
	winsock_last_error = WSAENETDOWN;
	return INVALID_SOCKET;
}

int WSAAPI halo_ws_send(SOCKET socket, const char *buffer, int length, int flags)
{
	(void)socket;
	(void)buffer;
	(void)length;
	(void)flags;
	return network_down();
}

int WSAAPI halo_ws_sendto(SOCKET socket, const char *buffer, int length, int flags,
	const struct sockaddr *address, int address_length)
{
	(void)socket;
	(void)buffer;
	(void)length;
	(void)flags;
	(void)address;
	(void)address_length;
	return network_down();
}

int WSAAPI halo_ws_recv(SOCKET socket, char *buffer, int length, int flags)
{
	(void)socket;
	(void)buffer;
	(void)length;
	(void)flags;
	return network_down();
}

int WSAAPI halo_ws_recvfrom(SOCKET socket, char *buffer, int length, int flags,
	struct sockaddr *address, int *address_length)
{
	(void)socket;
	(void)buffer;
	(void)length;
	(void)flags;
	(void)address;
	(void)address_length;
	return network_down();
}

int WSAAPI halo_ws_shutdown(SOCKET socket, int how)
{
	(void)socket;
	(void)how;
	return network_down();
}

int WSAAPI halo_ws_ioctlsocket(SOCKET socket, long command, u_long *argument)
{
	(void)socket;
	(void)command;
	(void)argument;
	return network_down();
}

int WSAAPI halo_ws_setsockopt(SOCKET socket, int level, int name, const char *value, int length)
{
	(void)socket;
	(void)level;
	(void)name;
	(void)value;
	(void)length;
	return network_down();
}

int WSAAPI halo_ws_getsockopt(SOCKET socket, int level, int name, char *value, int *length)
{
	(void)socket;
	(void)level;
	(void)name;
	(void)value;
	(void)length;
	return network_down();
}

int WSAAPI halo_ws_getsockname(SOCKET socket, struct sockaddr *address, int *address_length)
{
	(void)socket;
	(void)address;
	(void)address_length;
	return network_down();
}

int WSAAPI halo_ws_getpeername(SOCKET socket, struct sockaddr *address, int *address_length)
{
	(void)socket;
	(void)address;
	(void)address_length;
	return network_down();
}

int WSAAPI halo_ws_select(int descriptor_count, halo_ws_fd_set *read_set, halo_ws_fd_set *write_set,
	halo_ws_fd_set *error_set, const struct halo_ws_timeval *timeout)
{
	(void)descriptor_count;
	(void)timeout;
	if (read_set)
		read_set->fd_count = 0;
	if (write_set)
		write_set->fd_count = 0;
	if (error_set)
		error_set->fd_count = 0;
	return 0;
}

int PASCAL __WSAFDIsSet(SOCKET socket, halo_ws_fd_set *set)
{
	u_int index;

	for (index = 0; index < set->fd_count; index++)
	{
		if (set->fd_array[index] == socket)
			return 1;
	}
	return 0;
}

/* the Xbox is little-endian, as the network is not */
u_long WSAAPI halo_ws_htonl(u_long value)
{
	return __builtin_bswap32(value);
}

u_long WSAAPI halo_ws_ntohl(u_long value)
{
	return __builtin_bswap32(value);
}

u_short WSAAPI halo_ws_htons(u_short value)
{
	return (u_short)((value >> 8) | (value << 8));
}

u_short WSAAPI halo_ws_ntohs(u_short value)
{
	return (u_short)((value >> 8) | (value << 8));
}

/* ---------- XNet */

INT WSAAPI XNetStartup(const XNetStartupParams *parameters)
{
	(void)parameters;
	return 0;
}

INT WSAAPI XNetCleanup(void)
{
	return 0;
}

/* not cryptographic: nothing without a network needs it to be */
INT WSAAPI XNetRandom(BYTE *buffer, UINT size)
{
	static unsigned long state = 0x2342CE01UL;
	UINT index;

	for (index = 0; index < size; index++)
	{
		state = state * 1103515245UL + 12345UL;
		buffer[index] = (BYTE)(state >> 16);
	}
	return 0;
}

INT WSAAPI XNetCreateKey(XNKID *key_identifier, XNKEY *key)
{
	XNetRandom((BYTE *)key_identifier, sizeof(*key_identifier));
	XNetRandom((BYTE *)key, sizeof(*key));
	return 0;
}

INT WSAAPI XNetRegisterKey(const XNKID *key_identifier, const XNKEY *key)
{
	(void)key_identifier;
	(void)key;
	return 0;
}

INT WSAAPI XNetUnregisterKey(const XNKID *key_identifier)
{
	(void)key_identifier;
	return 0;
}

INT WSAAPI XNetXnAddrToInAddr(const XNADDR *address, const XNKID *key_identifier, IN_ADDR *result)
{
	(void)address;
	(void)key_identifier;
	memset(result, 0, sizeof(*result));
	return WSAENETDOWN;
}

DWORD WSAAPI XNetGetTitleXnAddr(XNADDR *address)
{
	memset(address, 0, sizeof(*address));
	return XNET_GET_XNADDR_NONE;
}

DWORD WSAAPI XNetGetEthernetLinkStatus(void)
{
	return 0;
}
