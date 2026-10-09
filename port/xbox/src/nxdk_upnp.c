/*
NXDK_UPNP.C

Internet play's UPnP on the Xbox: port/linux/src/posix_upnp.c, compiled
here as on a POSIX system (without _WIN32, which the compiler defines)
against lwIP's sockets by their POSIX names (port/xbox/include/lwip_posix),
as miniupnpc (port/third_party/miniupnpc) is; and the getnameinfo miniupnpc
learns this machine's address on the router's network with (the address the
router forwards to), which lwIP lacks.
*/

#include "posix_upnp.c"

/* numeric only, whatever the flags: lwIP looks no addresses' names up */
int getnameinfo(const struct sockaddr *address, socklen_t address_length, char *host, socklen_t host_size,
	char *service, socklen_t service_size, int flags)
{
	unsigned short port;

	(void)flags;
	if (address->sa_family == AF_INET && address_length >= (socklen_t)sizeof(struct sockaddr_in))
	{
		const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)address;

		if (host && host_size && !inet_ntop(AF_INET, &ipv4->sin_addr, host, host_size))
			return EAI_FAIL;
		port = ipv4->sin_port;
	}
	else if (address->sa_family == AF_INET6 && address_length >= (socklen_t)sizeof(struct sockaddr_in6))
	{
		const struct sockaddr_in6 *ipv6 = (const struct sockaddr_in6 *)address;

		if (host && host_size && !inet_ntop(AF_INET6, &ipv6->sin6_addr, host, host_size))
			return EAI_FAIL;
		port = ipv6->sin6_port;
	}
	else
		return EAI_FAMILY;
	if (service && service_size)
		snprintf(service, service_size, "%u", (unsigned int)lwip_ntohs(port));
	return 0;
}

/* miniupnpc's; nxdk's sits beside its _stricmp and _strnicmp, which would
then be linked over port/linux/src/msvc_crt.c's */
char *strdup(const char *text)
{
	size_t size = strlen(text) + 1;
	char *copy = malloc(size);

	if (copy)
		memcpy(copy, text, size);
	return copy;
}

const char *gai_strerror(int error)
{
	switch (error)
	{
	case EAI_NONAME: return "no such name";
	case EAI_SERVICE: return "no such service";
	case EAI_MEMORY: return "out of memory";
	case EAI_FAMILY: return "unknown address family";
	default: return "lookup failed";
	}
}
