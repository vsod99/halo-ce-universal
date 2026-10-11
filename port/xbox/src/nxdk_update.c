/*
NXDK_UPDATE.C

The Xbox's self-updater, as the desktop builds' (port/linux/src/updater.c),
with the console's own screen, files and start.

A build of the main branch made by GitHub Actions knows its build number
(HALO_BUILD_NUMBER, which names its release: build-<number>); other builds
have none and never look. When update.auto is true (the default), a thread
asks GitHub for the latest release once the Xbox has an address. If it is
newer, the game asks at the main menu, on pbkit's text screen (the game
waits, its last frame drawn): A updates, B carries on (the game asks again
at the next start), Y stops the questions (update.auto = false, which the
network settings screen turns back on).

To update, the release's Xbox build (halo-xbox-<release|debug>.zip, its
default.xbe and the files beside it) is downloaded into update.partial\ in
the XBE's folder (D:) over HTTPS (posix_update.c and Mbed TLS, with the
certificate authorities the program carries, port/assets/network/
xbox_roots.pem), unpacked, and put in place of the running XBE and its
files, which become <name>.old (deleted at the next start); then the new
XBE is started. A game started from a disc has nothing to replace, and
never looks.

Tests (port/xbox/README.md): debug.update_answer answers the question;
HALO_TEST_UPDATE_ASSET downloads another of the release's files in place of
the Xbox's (which the update then refuses: it has no default.xbe), and
HALO_TEST_UPDATE_ZIP installs a zip from a path instead of downloading.

Also Mbed TLS's platform on the Xbox (mbedtls_xbox_config.h): its entropy,
its clock, and its connection over lwIP's sockets.
*/

#define _CRT_RAND_S
#include <hal/debug.h>
#include <hal/xbox.h>
#include <pbkit/pbkit.h>
#include <xboxkrnl/xboxkrnl.h>
#include <windows.h>
#include <lwip/sockets.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/net_sockets.h"
#include "mbedtls/platform_time.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "zlib_prefixed.h"

#include "nxdk_platform.h"
#include "port_config.h"
#include "posix.h"
#include "update.h"

/* (given for this file by the build: tools/xbox_build.py) */
#ifndef HALO_BUILD_NUMBER
#define HALO_BUILD_NUMBER 0
#endif
#ifndef HALO_BUILD_FLAVOR
#define HALO_BUILD_FLAVOR "release"
#endif

#define UPDATE_REPOSITORY "OpenCommunityEdition/OpenCE"
#define UPDATE_ASSET "halo-xbox-" HALO_BUILD_FLAVOR ".zip"
#define UPDATE_FOLDER "D:\\"
#define UPDATE_DIRECTORY UPDATE_FOLDER "update.partial"
#define UPDATE_XBE "default.xbe"
#define MAXIMUM_UPDATE_FILES 16
/* (how long the check waits for an address: DHCP, or link-local) */
#define ADDRESS_WAIT_MILLISECONDS 60000
#define CONNECT_TIMEOUT_MILLISECONDS 20000

/* the game's (source/interface/main_menu.c) */
unsigned char main_menu_is_active(void);

/* the certificate authorities (tools/embed_assets.py --xbox) */
extern const unsigned int xbox_certificate_authorities[];
extern const unsigned long xbox_certificate_authorities_size;

enum
{
	_update_idle,
	_update_checking,
	_update_available,
	_update_handled,
};

static volatile LONG update_state;
static long update_latest_build;
/* the running XBE's name in its folder (default.xbe, unless renamed), and
the kernel's name for it (\Device\Harddisk0\Partition1\...: nxdk's D:
for XLaunchXBE is the DVD drive, not the XBE's folder) */
static char update_xbe_name[64];
static char update_image[MAX_PATH];

/* ---------- Mbed TLS's platform (mbedtls_xbox_config.h) */

/* the entropy Mbed TLS seeds its generator with: nxdk's rand_s (a SHA-1 of
the EEPROM, fed back on each call) and the processor's time stamp counter
(the kernel gathers none) */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t size, size_t *length)
{
	size_t done = 0;

	(void)data;
	while (done < size)
	{
		unsigned int value;
		size_t count = size - done < sizeof(value) ? size - done : sizeof(value);

		if (rand_s(&value) != 0)
			return -1;
		value ^= (unsigned int)__builtin_ia32_rdtsc();
		memcpy(output + done, &value, count);
		done += count;
	}
	*length = done;
	return 0;
}

mbedtls_ms_time_t mbedtls_ms_time(void)
{
	return (mbedtls_ms_time_t)GetTickCount();
}

/* Mbed TLS's connection (net_sockets.h, which the Xbox does not build):
lwIP's TCP, the host found by its DNS (nxdk_net.c) */
void mbedtls_net_init(mbedtls_net_context *context)
{
	context->fd = -1;
}

int mbedtls_net_connect(mbedtls_net_context *context, const char *host, const char *port, int protocol)
{
	struct sockaddr_in address;
	posix_ulong ipv4 = posix_resolve_ipv4(host);
	int flags, error = 0;
	socklen_t length = sizeof(error);
	fd_set writable;
	struct timeval timeout;

	if (protocol != MBEDTLS_NET_PROTO_TCP)
		return MBEDTLS_ERR_NET_SOCKET_FAILED;
	if (!ipv4)
		return MBEDTLS_ERR_NET_UNKNOWN_HOST;
	context->fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
	if (context->fd < 0)
		return MBEDTLS_ERR_NET_SOCKET_FAILED;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = lwip_htons((u16_t)atoi(port));
	address.sin_addr.s_addr = ipv4;
	/* (without a connection's minutes of retries: a time limit) */
	flags = lwip_fcntl(context->fd, F_GETFL, 0);
	lwip_fcntl(context->fd, F_SETFL, flags | O_NONBLOCK);
	if (lwip_connect(context->fd, (struct sockaddr *)&address, sizeof(address)) != 0 && errno != EINPROGRESS)
		return MBEDTLS_ERR_NET_CONNECT_FAILED;
	FD_ZERO(&writable);
	FD_SET(context->fd, &writable);
	timeout.tv_sec = CONNECT_TIMEOUT_MILLISECONDS / 1000;
	timeout.tv_usec = 0;
	if (lwip_select(context->fd + 1, NULL, &writable, NULL, &timeout) <= 0 ||
		lwip_getsockopt(context->fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error)
	{
		return MBEDTLS_ERR_NET_CONNECT_FAILED;
	}
	lwip_fcntl(context->fd, F_SETFL, flags);
	return 0;
}

int mbedtls_net_send(void *context, const unsigned char *buffer, size_t length)
{
	int sent = lwip_send(((mbedtls_net_context *)context)->fd, buffer, length, 0);

	if (sent >= 0)
		return sent;
	if (errno == EAGAIN || errno == EINTR)
		return MBEDTLS_ERR_SSL_WANT_WRITE;
	return errno == ECONNRESET || errno == EPIPE ? MBEDTLS_ERR_NET_CONN_RESET : MBEDTLS_ERR_NET_SEND_FAILED;
}

int mbedtls_net_recv_timeout(void *context, unsigned char *buffer, size_t length, uint32_t milliseconds)
{
	int socket = ((mbedtls_net_context *)context)->fd;
	fd_set readable;
	struct timeval timeout;
	int received, ready;

	FD_ZERO(&readable);
	FD_SET(socket, &readable);
	timeout.tv_sec = milliseconds / 1000;
	timeout.tv_usec = (milliseconds % 1000) * 1000;
	ready = lwip_select(socket + 1, &readable, NULL, NULL, milliseconds ? &timeout : NULL);
	if (ready == 0)
		return MBEDTLS_ERR_SSL_TIMEOUT;
	if (ready < 0)
		return MBEDTLS_ERR_NET_RECV_FAILED;
	received = lwip_recv(socket, buffer, length, 0);
	if (received >= 0)
		return received;
	if (errno == EAGAIN || errno == EINTR)
		return MBEDTLS_ERR_SSL_WANT_READ;
	return errno == ECONNRESET || errno == EPIPE ? MBEDTLS_ERR_NET_CONN_RESET : MBEDTLS_ERR_NET_RECV_FAILED;
}

void mbedtls_net_free(mbedtls_net_context *context)
{
	if (context->fd >= 0)
	{
		lwip_shutdown(context->fd, SHUT_RDWR);
		lwip_close(context->fd);
	}
	context->fd = -1;
}

/* the certificate authorities the program carries (posix_update.c) */
int xbox_update_certificates(mbedtls_x509_crt *certificates)
{
	int result = mbedtls_x509_crt_parse(certificates, (const unsigned char *)xbox_certificate_authorities,
		xbox_certificate_authorities_size);

	if (result < 0)
		platform_log("update: the certificate authorities did not parse (-0x%04x)", (unsigned int)-result);
	return result >= 0 && certificates->version;
}

/* ---------- files */

static void update_path(char *path, size_t size, const char *name)
{
	snprintf(path, size, UPDATE_FOLDER "%s", name);
}

static void update_partial_path(char *path, size_t size, const char *name)
{
	snprintf(path, size, UPDATE_DIRECTORY "\\%s", name);
}

/* the file's new copy in place of it; it becomes <name>.old */
static int update_replace(const char *name)
{
	char path[MAX_PATH], new_path[MAX_PATH], old_path[MAX_PATH];
	int existed;

	update_path(path, sizeof(path), name);
	update_partial_path(new_path, sizeof(new_path), name);
	snprintf(old_path, sizeof(old_path), "%s.old", path);
	SetFileAttributesA(old_path, FILE_ATTRIBUTE_NORMAL);
	DeleteFileA(old_path);
	existed = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
	if (existed && !MoveFileA(path, old_path))
		return 0;
	if (!MoveFileA(new_path, path))
	{
		if (existed)
			MoveFileA(old_path, path);
		return 0;
	}
	return 1;
}

/* the files a previous update left behind: <name>.old beside the XBE, and
update.partial\ */
static void update_clean_up(void)
{
	WIN32_FIND_DATAA found;
	HANDLE search;
	char path[MAX_PATH];

	/* (every name, the .old ones picked here: the kernel's masks do not
	take "*.old") */
	search = FindFirstFileA(UPDATE_FOLDER "*", &found);
	if (search != INVALID_HANDLE_VALUE)
	{
		do
		{
			size_t length = strlen(found.cFileName);

			if (length <= 4 || _stricmp(found.cFileName + length - 4, ".old") ||
				(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			{
				continue;
			}
			update_path(path, sizeof(path), found.cFileName);
			SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
			if (DeleteFileA(path))
				platform_log("update: %s deleted (the version before the update)", found.cFileName);
		} while (FindNextFileA(search, &found));
		FindClose(search);
	}
	search = FindFirstFileA(UPDATE_DIRECTORY "\\*", &found);
	if (search != INVALID_HANDLE_VALUE)
	{
		do
		{
			update_partial_path(path, sizeof(path), found.cFileName);
			DeleteFileA(path);
		} while (FindNextFileA(search, &found));
		FindClose(search);
	}
	RemoveDirectoryA(UPDATE_DIRECTORY);
}

/* ---------- the zip file (the release's), stored or deflated entries */

static unsigned long zip_word(const unsigned char *bytes)
{
	return (unsigned long)bytes[0] | (unsigned long)bytes[1] << 8;
}

static unsigned long zip_long(const unsigned char *bytes)
{
	return zip_word(bytes) | zip_word(bytes + 2) << 16;
}

static int zip_read_at(HANDLE zip, unsigned long offset, void *buffer, unsigned long size)
{
	DWORD read;

	return SetFilePointer(zip, (LONG)offset, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
		ReadFile(zip, buffer, size, &read, NULL) && read == size;
}

/* one entry, from its local header at local_offset, into path */
static int zip_extract_entry(HANDLE zip, unsigned long local_offset, int method, unsigned long packed_size,
	unsigned long size, unsigned long crc, const char *path)
{
	unsigned char header[30];
	unsigned char *input = NULL, *output = NULL;
	unsigned long remaining = packed_size, written = 0, checked = z_crc32(0L, Z_NULL, 0);
	HANDLE file = INVALID_HANDLE_VALUE;
	z_stream stream;
	int inflating = 0, succeeded = 0;

	if (!zip_read_at(zip, local_offset, header, sizeof(header)) || zip_long(header) != 0x04034b50 ||
		SetFilePointer(zip, (LONG)(local_offset + sizeof(header) + zip_word(header + 26) + zip_word(header + 28)),
			NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
	{
		return 0;
	}
	input = malloc(65536);
	output = malloc(65536);
	file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (!input || !output || file == INVALID_HANDLE_VALUE)
		goto done;
	memset(&stream, 0, sizeof(stream));
	if (method == 8)
	{
		if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
			goto done;
		inflating = 1;
	}
	for (;;)
	{
		const unsigned char *data;
		DWORD count, put;
		int result = Z_OK;

		/* (the next of the packed bytes, once the last are used) */
		if (remaining && (!inflating || !stream.avail_in))
		{
			DWORD read = 0;

			count = remaining < 65536 ? remaining : 65536;
			if (!ReadFile(zip, input, count, &read, NULL) || read != count)
				goto done;
			remaining -= count;
			stream.next_in = input;
			stream.avail_in = count;
		}
		else if (!inflating)
		{
			break;
		}
		if (inflating)
		{
			stream.next_out = output;
			stream.avail_out = 65536;
			result = inflate(&stream, Z_NO_FLUSH);
			if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR)
				goto done;
			/* (no progress, with nothing left to give it: cut short) */
			if (result == Z_BUF_ERROR && !stream.avail_in && !remaining)
				goto done;
			data = output;
			count = 65536 - stream.avail_out;
		}
		else
		{
			data = input;
			count = stream.avail_in;
			stream.avail_in = 0;
		}
		if (written + count > size)
			goto done;
		if (count && (!WriteFile(file, data, count, &put, NULL) || put != count))
			goto done;
		checked = z_crc32(checked, data, count);
		written += count;
		if (result == Z_STREAM_END)
			break;
	}
	succeeded = written == size && checked == crc;
done:
	if (inflating)
		inflateEnd(&stream);
	if (file != INVALID_HANDLE_VALUE)
		CloseHandle(file);
	if (!succeeded)
		DeleteFileA(path);
	free(input);
	free(output);
	return succeeded;
}

/* a name a release's flat folder can have: no folders, nothing odd */
static int zip_name_allowed(const char *name)
{
	size_t index;

	if (!name[0] || name[0] == '.' || strlen(name) > 42)
		return 0;
	for (index = 0; name[index]; index++)
	{
		char c = name[index];

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
			c == '-' || c == '_'))
		{
			return 0;
		}
	}
	return 1;
}

/* the zip's files into update.partial\, their names in names; 1 when every
one is there and default.xbe is among them */
static int zip_extract(const char *zip_path, char names[][64], int *name_count, char *error, size_t error_size)
{
	HANDLE zip = CreateFileA(zip_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
		NULL);
	unsigned char tail[65536 + 22];
	unsigned long size, tail_size, directory_offset = 0, entries = 0, entry, offset;
	int found = 0, has_xbe = 0, index, pass;

	*name_count = 0;
	if (zip == INVALID_HANDLE_VALUE)
	{
		snprintf(error, error_size, "the download cannot be read");
		return 0;
	}
	size = GetFileSize(zip, NULL);
	tail_size = size < sizeof(tail) ? size : sizeof(tail);
	if (size < 22 || !zip_read_at(zip, size - tail_size, tail, tail_size))
		goto fail;
	for (index = (int)tail_size - 22; index >= 0; index--)
	{
		if (zip_long(tail + index) == 0x06054b50)
		{
			entries = zip_word(tail + index + 10);
			directory_offset = zip_long(tail + index + 16);
			found = 1;
			break;
		}
	}
	if (!found || entries > MAXIMUM_UPDATE_FILES)
		goto fail;
	/* (the names first, all of them, then the files: nothing is unpacked
	from a zip that is not an Xbox build) */
	for (pass = 0; pass < 2; pass++)
	{
		offset = directory_offset;
		for (entry = 0; entry < entries; entry++)
		{
			unsigned char header[46];
			char name[64];
			unsigned long name_length, extra_length, comment_length;
			int method;
			char path[MAX_PATH];

			if (!zip_read_at(zip, offset, header, sizeof(header)) || zip_long(header) != 0x02014b50)
				goto fail;
			name_length = zip_word(header + 28);
			extra_length = zip_word(header + 30);
			comment_length = zip_word(header + 32);
			if (name_length >= sizeof(name) || !zip_read_at(zip, offset + sizeof(header), name, name_length))
				goto fail;
			name[name_length] = 0;
			offset += sizeof(header) + name_length + extra_length + comment_length;
			method = (int)zip_word(header + 10);
			if (!pass)
			{
				if (!zip_name_allowed(name) || (method != 0 && method != 8))
				{
					snprintf(error, error_size, "the download has a file this build does not take (%s)", name);
					CloseHandle(zip);
					return 0;
				}
				if (!_stricmp(name, UPDATE_XBE))
					has_xbe = 1;
				continue;
			}
			update_partial_path(path, sizeof(path), name);
			if (!zip_extract_entry(zip, zip_long(header + 42), method, zip_long(header + 20), zip_long(header + 24),
				zip_long(header + 16), path))
			{
				snprintf(error, error_size, "%s in the download is damaged, or the disk is full", name);
				CloseHandle(zip);
				return 0;
			}
			snprintf(names[*name_count], 64, "%s", name);
			(*name_count)++;
		}
		if (!has_xbe)
		{
			snprintf(error, error_size, "the download has no " UPDATE_XBE);
			CloseHandle(zip);
			return 0;
		}
	}
	CloseHandle(zip);
	return 1;
fail:
	CloseHandle(zip);
	snprintf(error, error_size, "the download is not a zip file this build can read");
	return 0;
}

/* ---------- the screen */

/* the buttons down on any controller: A, B, Y (bits 0, 1, 2) */
static unsigned int update_buttons(void)
{
	unsigned char report[XBOX_GAMEPAD_REPORT_SIZE];
	unsigned long reports;
	unsigned long connected = xbox_gamepads_poll();
	unsigned int buttons = 0;
	int port;

	for (port = 0; port < 4; port++)
	{
		/* (the buttons' word, then the analog buttons: A, B, X, Y, ...) */
		if ((connected & (1UL << port)) && xbox_gamepad_report(port, report, &reports))
		{
			if (report[2] > 30)
				buttons |= 1;
			if (report[2 + 1] > 30)
				buttons |= 2;
			if (report[2 + 3] > 30)
				buttons |= 4;
		}
	}
	return buttons;
}

/* waits for one of the buttons (pressed after the screen showed) */
static unsigned int update_wait_button(unsigned int wanted)
{
	unsigned int buttons;

	while (update_buttons() & wanted)
		Sleep(20);
	while (!((buttons = update_buttons()) & wanted))
		Sleep(20);
	return buttons & wanted;
}

static void update_screen_show(void)
{
	xbox_gpu_wait_idle();
	debugClearScreen();
	pb_show_debug_screen();
}

/* back to the game's frames, once the buttons are up (or the game would
take the press as its own) */
static void update_screen_hide(void)
{
	while (update_buttons())
		Sleep(20);
	pb_show_front_screen();
}

struct update_progress
{
	DWORD last_shown;
};

static void update_download_progress(void *context, unsigned long long received, unsigned long long total)
{
	struct update_progress *progress = context;

	if (GetTickCount() - progress->last_shown < 500)
		return;
	progress->last_shown = GetTickCount();
	debugClearScreen();
	if (total)
	{
		debugPrint("\n\n\n  Downloading build %ld: %lu of %lu KB (%lu%%)\n", update_latest_build,
			(unsigned long)(received / 1024), (unsigned long)(total / 1024), (unsigned long)(received * 100 / total));
	}
	else
	{
		debugPrint("\n\n\n  Downloading build %ld: %lu KB\n", update_latest_build, (unsigned long)(received / 1024));
	}
}

/* ---------- checking */

/* the build number of GitHub's latest release, 0 if there is none */
static long update_latest_release(void)
{
	char path[MAX_PATH];
	char error[512] = "";
	char text[16384];
	const char *tag;
	FILE *file;
	size_t length;
	long build = 0;

	update_path(path, sizeof(path), "update-check.json");
	if (!update_download("https://api.github.com/repos/" UPDATE_REPOSITORY "/releases/latest", path, NULL, NULL,
		error, sizeof(error)))
	{
		platform_log("update: could not check for a new version: %s", error);
		return 0;
	}
	file = fopen(path, "rb");
	length = file ? fread(text, 1, sizeof(text) - 1, file) : 0;
	if (file)
		fclose(file);
	DeleteFileA(path);
	text[length] = 0;
	/* "tag_name": "build-<number>" (near the start) */
	tag = strstr(text, "\"tag_name\"");
	if (tag)
	{
		tag = strchr(tag + 10, '"');
		if (tag && !strncmp(tag, "\"build-", 7))
			build = strtol(tag + 7, NULL, 10);
	}
	return build;
}

static DWORD WINAPI update_check_thread(LPVOID parameter)
{
	DWORD started = GetTickCount();
	long latest;

	(void)parameter;
	while (!posix_local_ipv4_address())
	{
		if (GetTickCount() - started > ADDRESS_WAIT_MILLISECONDS)
		{
			platform_log("update: no network address; not checking for a new version");
			InterlockedExchange(&update_state, _update_handled);
			return 0;
		}
		Sleep(500);
	}
	latest = update_latest_release();
	if (latest > HALO_BUILD_NUMBER)
	{
		platform_log("update: build %ld is available (this is build %d)", latest, HALO_BUILD_NUMBER);
		update_latest_build = latest;
		InterlockedExchange(&update_state, _update_available);
	}
	else
	{
		if (latest)
			platform_log("update: this is the latest build (%d)", HALO_BUILD_NUMBER);
		InterlockedExchange(&update_state, _update_handled);
	}
	return 0;
}

/* ---------- updating */

/* downloads, unpacks and puts in place the new version, and starts it;
returns (with why in error) only when it could not */
static void update_install(char *error, size_t error_size)
{
	const char *test_zip = getenv("HALO_TEST_UPDATE_ZIP");
	const char *asset = getenv("HALO_TEST_UPDATE_ASSET");
	char zip_path[MAX_PATH], url[256];
	char names[MAXIMUM_UPDATE_FILES][64];
	int name_count, index;
	struct update_progress progress = {0};

	if (!asset || !asset[0])
		asset = UPDATE_ASSET;
	if (!CreateDirectoryA(UPDATE_DIRECTORY, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
	{
		snprintf(error, error_size, "cannot make " UPDATE_DIRECTORY);
		return;
	}
	if (test_zip && test_zip[0])
	{
		/* (a test's zip in place of the download) */
		platform_log("update: installing %s (HALO_TEST_UPDATE_ZIP)", test_zip);
		snprintf(zip_path, sizeof(zip_path), "%s", test_zip);
	}
	else
	{
		update_partial_path(zip_path, sizeof(zip_path), "update.zip");
		snprintf(url, sizeof(url), "https://github.com/" UPDATE_REPOSITORY "/releases/download/build-%ld/%s",
			update_latest_build, asset);
		platform_log("update: downloading %s", url);
		if (!update_download(url, zip_path, update_download_progress, &progress, error, (int)error_size))
			return;
		platform_log("update: downloaded");
	}
	if (!zip_extract(zip_path, names, &name_count, error, error_size))
		return;
	if (!test_zip || !test_zip[0])
		DeleteFileA(zip_path);
	/* the XBE last, under the running one's name (default.xbe unless the
	player renamed it); the other files beside it first */
	for (index = 0; index < name_count; index++)
	{
		if (!_stricmp(names[index], UPDATE_XBE))
			continue;
		if (!update_replace(names[index]))
		{
			snprintf(error, error_size, "cannot replace %s", names[index]);
			return;
		}
	}
	if (_stricmp(update_xbe_name, UPDATE_XBE))
	{
		char from[MAX_PATH], to[MAX_PATH];

		update_partial_path(from, sizeof(from), UPDATE_XBE);
		update_partial_path(to, sizeof(to), update_xbe_name);
		if (!MoveFileA(from, to))
		{
			snprintf(error, error_size, "cannot rename the new " UPDATE_XBE " to %s", update_xbe_name);
			return;
		}
	}
	if (!update_replace(update_xbe_name))
	{
		snprintf(error, error_size, "cannot replace %s", update_xbe_name);
		return;
	}
	platform_log("update: build %ld is in place; starting %s", update_latest_build, update_image);
	debugClearScreen();
	debugPrint("\n\n\n  Starting build %ld...\n", update_latest_build);
	XLaunchXBE(update_image);
	snprintf(error, error_size, "cannot start %s", update_image);
}

static void update_failed(const char *error, int test)
{
	platform_log("update: failed: %s", error);
	debugClearScreen();
	debugPrint("\n\n\n"
		"  The update failed:\n"
		"  %s\n\n"
		"  This version carries on. Press A.\n", error);
	if (!test)
		update_wait_button(1);
	update_clean_up();
}

/* ---------- the game's */

/* each frame, after it is shown (d3d8_nv2a.c's Present): starts the check
at the first, and asks the player at the main menu once a new version is
found */
void xbox_update_frame(void)
{
	static int started;
	char message[400];
	char error[512] = "";
	unsigned int answer;
	const char *test_answer;

	if (!started)
	{
		char image[MAX_PATH];
		size_t length = 0;
		const char *slash;

		started = 1;
		if (XeImageFileName->Buffer)
		{
			length = XeImageFileName->Length < sizeof(image) - 1 ? XeImageFileName->Length : sizeof(image) - 1;
			memcpy(image, XeImageFileName->Buffer, length);
		}
		image[length] = 0;
		slash = strrchr(image, '\\');
		/* (not from a disc, which has nothing to replace) */
		if (!slash || !_strnicmp(image, "\\Device\\CdRom0\\", 15) || strlen(slash + 1) >= sizeof(update_xbe_name))
			return;
		snprintf(update_xbe_name, sizeof(update_xbe_name), "%s", slash + 1);
		snprintf(update_image, sizeof(update_image), "%s", image);
		update_clean_up();
		/* (not for builds without a number, the player's no, or runs nobody
		is watching, but for a test with its answer) */
		if (HALO_BUILD_NUMBER <= 0 || !config_boolean("update.auto") ||
			(!config_string("debug.update_answer")[0] && (config_real("debug.exit_after") > 0.0 ||
				config_string("debug.network_test")[0])))
		{
			return;
		}
		InterlockedExchange(&update_state, _update_checking);
		{
			HANDLE thread = CreateThread(NULL, 128 * 1024, update_check_thread, NULL, 0, NULL);

			if (thread)
				CloseHandle(thread);
			else
				InterlockedExchange(&update_state, _update_handled);
		}
		return;
	}
	if (update_state != _update_available || !main_menu_is_active())
		return;
	InterlockedExchange(&update_state, _update_handled);
	test_answer = config_string("debug.update_answer");
	update_screen_show();
	if (test_answer[0])
	{
		/* (an automated test's answer: debug.update_answer) */
		platform_log("update: answering %s (debug.update_answer)", test_answer);
		answer = !strcmp(test_answer, "yes") ? 1 : !strcmp(test_answer, "never") ? 4 : 2;
	}
	else
	{
		snprintf(message, sizeof(message),
			"\n\n\n"
			"  A new version of Halo is available\n"
			"  (build %ld; this is build %d).\n\n"
			"  A  Update now: the game downloads it\n"
			"     and starts again\n"
			"  B  Not now\n"
			"  Y  Do not ask again\n", update_latest_build, HALO_BUILD_NUMBER);
		debugPrint("%s", message);
		answer = update_wait_button(1 | 2 | 4);
	}
	if (answer & 1)
	{
		debugClearScreen();
		debugPrint("\n\n\n  Downloading build %ld...\n", update_latest_build);
		update_install(error, sizeof(error));
		update_failed(error, test_answer[0] != 0);
	}
	else if (answer & 4)
	{
		if (config_write_boolean("update.auto", 0))
			platform_log("update: update.auto = false written to config.toml");
		else
			platform_log("update: could not write update.auto to config.toml");
		debugClearScreen();
		debugPrint("\n\n\n"
			"  The game will not look for new versions.\n\n"
			"  To look again, turn on CHECK FOR UPDATES\n"
			"  in the profile's network settings.\n\n"
			"  Press A.\n");
		if (!test_answer[0])
			update_wait_button(1);
	}
	update_screen_hide();
}
