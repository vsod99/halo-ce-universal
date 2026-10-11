/*
NXDK_P2P.C

The process and desktop half of port/linux/src/posix.h for the Xbox, which
internet play uses (port/linux/src/p2p.c; Windows' is
port/windows/src/win32_p2p.c; its UPnP is nxdk_upnp.c). The Xbox runs one
program, with no command line (a test's stands in for one), links or
desktop: invites are typed on the on-screen keyboard or found in the server
browser, no second copy of the game hands one over (no user secret), and
there is no Discord. What the machine is known by (its hardware id, which
p2p.c hashes) is its EEPROM's serial number and Ethernet address.
*/

#include <xboxkrnl/xboxkrnl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "posix.h"

/* ---------- the process */

/* the Xbox starts programs with none: the HALO_COMMAND_LINE line of
D:\environment.txt (nxdk_libc.c's getenv) stands for one, arguments
separated by spaces, after the program's (index 0), so that a test can open
an invite (tools/xbox_dev.py link --internet) */
int posix_command_line_argument(int index, char *buffer, posix_ulong size)
{
	const char *cursor = getenv("HALO_COMMAND_LINE");
	int current = 1;

	if (!size)
		return 0;
	buffer[0] = 0;
	if (index == 0)
	{
		snprintf(buffer, size, "D:\\default.xbe");
		return 1;
	}
	for (; cursor && *cursor; current++)
	{
		posix_ulong length = 0;

		while (*cursor == ' ')
			cursor++;
		if (!*cursor)
			break;
		for (; *cursor && *cursor != ' '; cursor++)
		{
			if (current == index && length + 1 < size)
				buffer[length++] = *cursor;
		}
		if (current == index)
		{
			buffer[length] = 0;
			return 1;
		}
	}
	return 0;
}

posix_ulong posix_process_id(void)
{
	return 1;
}

int posix_register_url_scheme(const char *scheme, const char *description)
{
	(void)scheme;
	(void)description;
	return 0;
}

/* (only for handing an invite to the copy already running) */
int posix_user_secret(unsigned char *secret, int size)
{
	(void)secret;
	(void)size;
	return 0;
}

/* ---------- the hardware id */

/* the factory section's serial number (12 digits) and Ethernet address,
which differ between consoles and survive a new hard disk; called by
p2p.c's hardware_id_source on _WIN32, which the Xbox's compiler defines */
int posix_hardware_id_source(char *text, int size)
{
	char serial[13];
	unsigned char ethernet[6];
	ULONG type, length = 0;
	int index;

	memset(serial, 0, sizeof(serial));
	if (!NT_SUCCESS(ExQueryNonVolatileSetting(XC_FACTORY_SERIAL_NUMBER, &type, serial, sizeof(serial) - 1,
			&length)) ||
		!NT_SUCCESS(ExQueryNonVolatileSetting(XC_FACTORY_ETHERNET_ADDR, &type, ethernet, sizeof(ethernet),
			&length)))
	{
		return 0;
	}
	/* (only digits, which a blank EEPROM's are not) */
	for (index = 0; serial[index]; index++)
	{
		if (serial[index] < '0' || serial[index] > '9')
			return 0;
	}
	if (!index)
		return 0;
	return snprintf(text, (size_t)size, "xbox %s %02x%02x%02x%02x%02x%02x", serial, ethernet[0], ethernet[1],
		ethernet[2], ethernet[3], ethernet[4], ethernet[5]) < size;
}

/* ---------- Discord: none */

int posix_discord_connect(void)
{
	return -1;
}

int posix_discord_write(int handle, const void *buffer, int length)
{
	(void)handle;
	(void)buffer;
	(void)length;
	return -1;
}

int posix_discord_read(int handle, void *buffer, int length)
{
	(void)handle;
	(void)buffer;
	(void)length;
	return -1;
}

void posix_discord_close(int handle)
{
	(void)handle;
}

/* ---------- DTLS for WebRTC: none

Internet play with browsers (port/linux/src/p2p_webrtc.c) needs a DTLS
server (posix_dtls.c, over mbedtls), which the Xbox leaves out for its
memory: with no certificate, p2p_webrtc.c offers browsers nothing and keeps
to other native builds' plain UDP tunnels, as it does on a machine whose
certificate could not be made. */

int posix_dtls_fingerprint(unsigned char *fingerprint)
{
	(void)fingerprint;
	return 0;
}

int posix_dtls_open(void)
{
	return -1;
}

void posix_dtls_close(int handle)
{
	(void)handle;
}

void posix_dtls_input(int handle, const void *data, int size)
{
	(void)handle;
	(void)data;
	(void)size;
}

int posix_dtls_receive(int handle, posix_ulong now, void *buffer, int size)
{
	(void)handle;
	(void)now;
	(void)buffer;
	(void)size;
	return -1;
}

int posix_dtls_send(int handle, const void *data, int size)
{
	(void)handle;
	(void)data;
	(void)size;
	return 0;
}

int posix_dtls_output(int handle, void *buffer, int size)
{
	(void)handle;
	(void)buffer;
	(void)size;
	return 0;
}

int posix_dtls_peer_fingerprint(int handle, unsigned char *fingerprint)
{
	(void)handle;
	(void)fingerprint;
	return 0;
}

/* (STUN's integrity, for a browser's connection, which there never is: a
digest that matches nothing) */
void posix_hmac_sha1(const void *key, int key_size, const void *data, int size, unsigned char *digest)
{
	(void)key;
	(void)key_size;
	(void)data;
	(void)size;
	memset(digest, 0, 20);
}
