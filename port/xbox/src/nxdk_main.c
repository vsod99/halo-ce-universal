/*
NXDK_MAIN.C

The Xbox port's start-up and exit. nxdk's start-up runs the constructors
before the game's own main() (source/shell/shell_xbox.c), which allocates
its memory first thing (cache/physical_memory_map.c): so the log, the hard
disk and the Xbox's own settings are ready by then.

The game sets the x87's precision to 53 bits itself (_control87 with
CW_DEFAULT, source/math/real_math.c; port/linux/src/msvc_crt.c), as every
port's doubles need.
*/

#include <hal/debug.h>
#include <hal/xbox.h>
#include <nxdk/mount.h>
#include <pbkit/pbkit.h>
#include <xboxkrnl/xboxkrnl.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nxdk_platform.h"
#include "xbox_log.h"

/* (before the other units' constructors: the log is used from the start;
nxdk runs a C initializer, .CRT$XI*, ahead of the constructors, .CRT$XC*) */
static int xbox_startup(void)
{
	int serial = xbox_log_initialize(0);

	/* (kept for the network, debug.log_address: nxdk_net.c) */
	xbox_log_set_tap(xbox_network_log_tap);
	platform_log("Halo for the original Xbox (port/xbox), log on %s", serial ? "COM2" : "the screen only");
	xbox_memory_report("the program loaded");
	/* E:, the hard disk's first partition: the settings, saves and cache
	(port/linux/src/xbox_files.c, port/xbox/src/sdl_files.c) */
	if (!nxIsDriveMounted('E') && !nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\"))
		platform_log("cannot mount the hard disk as E:");
	else if (!CreateDirectoryA("E:\\halo", NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
		platform_log("cannot make E:\\halo (%lu)", (unsigned long)GetLastError());
	/* the DVD drive: the Halo disc's maps, when the XBE's folder (D:) has
	none (nxdk_posix.c). (Read in xemu, whose drive shows an image's game
	partition; not yet tried with a retail disc on a console) */
	if (!nxIsDriveMounted(XBOX_DISC_DRIVE) && !nxMountDrive(XBOX_DISC_DRIVE, "\\Device\\CdRom0\\"))
		platform_log("cannot mount the DVD drive as %c:", XBOX_DISC_DRIVE);
	return 0;
}

__attribute__((section(".CRT$XIU"), used)) static int (*const xbox_startup_entry)(void) = xbox_startup;

/* (not CopyFileA: it gives the copy the original's attributes, and a
disc's files are read-only) */
static BOOL copy_file(const char *from, const char *to)
{
	HANDLE source, target;
	char *buffer;
	DWORD read, written;
	BOOL copied = FALSE;

	SetFileAttributesA(to, FILE_ATTRIBUTE_NORMAL);
	source = CreateFileA(from, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (source == INVALID_HANDLE_VALUE)
		return FALSE;
	target = CreateFileA(to, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	buffer = malloc(64 * 1024);
	if (target != INVALID_HANDLE_VALUE && buffer)
	{
		while (ReadFile(source, buffer, 64 * 1024, &read, NULL))
		{
			if (!read)
			{
				copied = TRUE;
				break;
			}
			if (!WriteFile(target, buffer, read, &written, NULL) || written != read)
				break;
		}
	}
	free(buffer);
	if (target != INVALID_HANDLE_VALUE)
		CloseHandle(target);
	CloseHandle(source);
	return copied;
}

/* a test of the maps from the disc in xemu, which starts the XBE from the
image in its drive: HALO_TEST_FROM_HARD_DISK=1 copies the XBE and its
environment to a folder of the hard disk and starts it from there, so D:
has no maps and the image stands for the Halo disc (port/xbox/README.md) */
__attribute__((constructor)) static void xbox_test_from_hard_disk(void)
{
	static const char *const files[] = {"default.xbe", "environment.txt", "brokers.txt"};
	const char *test = getenv("HALO_TEST_FROM_HARD_DISK");
	char from[64], to[64];
	int index;

	if (!test || strcmp(test, "1") != 0 || !XeImageFileName->Buffer ||
		_strnicmp(XeImageFileName->Buffer, "\\Device\\CdRom0\\", 15) != 0)
	{
		return;
	}
	CreateDirectoryA("E:\\halo\\hard_disk_test", NULL);
	for (index = 0; index < (int)(sizeof(files) / sizeof(files[0])); index++)
	{
		snprintf(from, sizeof(from), "D:\\%s", files[index]);
		snprintf(to, sizeof(to), "E:\\halo\\hard_disk_test\\%s", files[index]);
		if (!copy_file(from, to))
		{
			platform_log("hard disk test: cannot copy %s to %s (%lu)", from, to, (unsigned long)GetLastError());
			return;
		}
	}
	platform_log("hard disk test: starting E:\\halo\\hard_disk_test\\default.xbe");
	XLaunchXBE("E:\\halo\\hard_disk_test\\default.xbe");
	platform_log("hard disk test: cannot start it");
}

/* the folder the XBE was started from, as the dashboards name it (E:\, F:\,
G:\ for the hard disk's partitions; else the kernel's own name) */
static void image_folder(char *folder, size_t size)
{
	static const struct
	{
		const char *device;
		const char *drive;
	} drives[] =
	{
		{"\\Device\\Harddisk0\\Partition1\\", "E:\\"},
		{"\\Device\\Harddisk0\\Partition6\\", "F:\\"},
		{"\\Device\\Harddisk0\\Partition7\\", "G:\\"},
	};
	char name[MAX_PATH];
	size_t length = 0;
	char *slash;
	int index;

	if (XeImageFileName->Buffer)
	{
		length = XeImageFileName->Length < sizeof(name) - 1 ? XeImageFileName->Length : sizeof(name) - 1;
		memcpy(name, XeImageFileName->Buffer, length);
	}
	name[length] = '\0';
	slash = strrchr(name, '\\');
	if (slash)
		slash[1] = '\0';
	for (index = 0; index < (int)(sizeof(drives) / sizeof(drives[0])); index++)
	{
		size_t prefix = strlen(drives[index].device);

		if (!_strnicmp(name, drives[index].device, prefix))
		{
			snprintf(folder, size, "%s%s", drives[index].drive, name + prefix);
			return;
		}
	}
	snprintf(folder, size, "%s", name);
}

/* whether B or BACK is down on any controller */
static int back_pressed(void)
{
	unsigned char report[XBOX_GAMEPAD_REPORT_SIZE];
	unsigned long reports;
	unsigned long connected = xbox_gamepads_poll();
	int port;

	for (port = 0; port < 4; port++)
	{
		/* (the buttons' word, then the analog buttons: A, B, ...) */
		if ((connected & (1UL << port)) && xbox_gamepad_report(port, report, &reports) &&
			((report[0] & 0x20) || report[2 + 1] > 30))
		{
			return 1;
		}
	}
	return 0;
}

/* the screen shown, before the game starts, when neither the XBE's folder
nor the drive has the maps (port/linux/src/xbox_files.c asks, through
platform_offer_game_data): pbkit's text screen, which the game's frames
replace once the maps are found */
int xbox_wait_for_maps(void)
{
	char folder[MAX_PATH];
	DWORD last_look;

	if (!xbox_gpu_ready())
		return xbox_maps_found();
	image_folder(folder, sizeof(folder));
	platform_log("maps: none beside the XBE (%s) or in the drive; asking for the Halo disc", folder);
	debugClearScreen();
	pb_show_debug_screen();
	debugPrint("\n\n\n"
		"  Halo's maps were not found.\n\n"
		"  Put the Halo: Combat Evolved disc in the drive,\n"
		"  or copy the disc's maps folder to\n"
		"  %s\n\n\n"
		"  Press B to go back to the dashboard.\n", folder);
	last_look = GetTickCount();
	for (;;)
	{
		if (back_pressed())
		{
			platform_log("maps: none; back to the dashboard");
			XLaunchXBE(NULL);
			return 0;
		}
		/* (a look at an empty drive is slow: once a second) */
		if (GetTickCount() - last_look >= 1000)
		{
			last_look = GetTickCount();
			if (xbox_maps_found())
				break;
		}
		Sleep(50);
	}
	platform_log("maps: found; starting");
	pb_show_front_screen();
	return 1;
}

/* the menus' Quit (port/linux/game/menu_functions.c): back to the dashboard */
void platform_request_quit(void)
{
	platform_log("quitting to the dashboard");
	XLaunchXBE(NULL);
}
