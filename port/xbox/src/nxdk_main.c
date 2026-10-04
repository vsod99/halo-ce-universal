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

#include <hal/xbox.h>
#include <nxdk/mount.h>
#include <xboxkrnl/xboxkrnl.h>
#include <windows.h>

#include "nxdk_platform.h"
#include "xbox_log.h"

/* (before the other units' constructors: the log is used from the start;
nxdk runs a C initializer, .CRT$XI*, ahead of the constructors, .CRT$XC*) */
static int xbox_startup(void)
{
	int serial = xbox_log_initialize(0);

	platform_log("Halo for the original Xbox (port/xbox), log on %s", serial ? "COM2" : "the screen only");	/* E:, the hard disk's first partition: the settings, saves and cache
	(port/linux/src/xbox_files.c, port/xbox/src/sdl_files.c) */
	if (!nxIsDriveMounted('E') && !nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\"))
		platform_log("cannot mount the hard disk as E:");
	else if (!CreateDirectoryA("E:\\halo", NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
		platform_log("cannot make E:\\halo (%lu)", (unsigned long)GetLastError());
	return 0;
}

__attribute__((section(".CRT$XIU"), used)) static int (*const xbox_startup_entry)(void) = xbox_startup;

/* the menus' Quit (port/linux/game/menu_functions.c): back to the dashboard */
void platform_request_quit(void)
{
	platform_log("quitting to the dashboard");
	XLaunchXBE(NULL);
}
