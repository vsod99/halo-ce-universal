/*
XINPUT_XBOX.C

The Xbox SDK's controller calls over the console's own controllers
(nxdk_gamepads.c, nxdk's USB driver). A controller's report is the SDK's
XINPUT_GAMEPAD as it is, so XInputGetState copies it; XInputSetState sets
the motors. No debug keyboard (the console's, a USB keyboard, would need
nxdk's HID driver), so no console. The desktop ports' keyboard, mouse and
binding hooks the game calls answer as having none.
*/

#include "platform.h"
#include "halo_keyboard.h"
#include "nxdk_platform.h"

#include <stdio.h>
#include <string.h>

#define PORT_COUNT 4

/* (a controller's report is the SDK's gamepad state) */
typedef char gamepad_report_size_check[sizeof(XINPUT_GAMEPAD) == XBOX_GAMEPAD_REPORT_SIZE ? 1 : -1];

XPP_DEVICE_TYPE XDEVICE_TYPE_GAMEPAD_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_MEMORY_UNIT_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_DEBUG_KEYBOARD_TABLE;

struct controller
{
	BOOL open;
};

static struct controller controllers[PORT_COUNT];
/* the ports XGetDeviceChanges has told the game have a controller */
static DWORD reported_gamepads;

/* ---------- XAPI */

VOID WINAPI XInitDevices(DWORD preallocation_type_count, PXDEVICE_PREALLOC_TYPE preallocation_types)
{
	(void)preallocation_type_count;
	(void)preallocation_types;
}

BOOL WINAPI XGetDeviceChanges(PXPP_DEVICE_TYPE device_type, PDWORD insertions, PDWORD removals)
{
	*insertions = 0;
	*removals = 0;
	if (device_type == XDEVICE_TYPE_GAMEPAD)
	{
		DWORD connected = xbox_gamepads_poll();

		*insertions = connected & ~reported_gamepads;
		*removals = reported_gamepads & ~connected;
		reported_gamepads = connected;
	}
	return *insertions || *removals;
}

HANDLE WINAPI XInputOpen(PXPP_DEVICE_TYPE device_type, DWORD port, DWORD slot,
	PXINPUT_POLLING_PARAMETERS polling_parameters)
{
	(void)slot;
	(void)polling_parameters;
	if (device_type == XDEVICE_TYPE_GAMEPAD && port < PORT_COUNT && (reported_gamepads & (1UL << port)))
	{
		controllers[port].open = TRUE;
		return (HANDLE)&controllers[port];
	}
	SetLastError(ERROR_DEVICE_NOT_CONNECTED);
	return NULL;
}

VOID WINAPI XInputClose(HANDLE device)
{
	struct controller *controller = (struct controller *)device;

	if (controller)
	{
		xbox_gamepad_rumble((int)(controller - controllers), 0, 0);
		controller->open = FALSE;
	}
}

static int controller_port(HANDLE device)
{
	int port;

	for (port = 0; port < PORT_COUNT; port++)
	{
		if (device == (HANDLE)&controllers[port] && controllers[port].open)
			return port;
	}
	return -1;
}

DWORD WINAPI XInputGetState(HANDLE device, PXINPUT_STATE state)
{
	int port = controller_port(device);
	unsigned long reports;

	memset(state, 0, sizeof(*state));
	if (port < 0 || !xbox_gamepad_report(port, (unsigned char *)&state->Gamepad, &reports))
		return ERROR_DEVICE_NOT_CONNECTED;
	state->dwPacketNumber = reports;
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputSetState(HANDLE device, PXINPUT_FEEDBACK feedback)
{
	int port = controller_port(device);

	if (!feedback)
		return ERROR_INVALID_PARAMETER;
	feedback->Header.dwStatus = ERROR_SUCCESS;
	if (port < 0)
		return ERROR_DEVICE_NOT_CONNECTED;
	xbox_gamepad_rumble(port, feedback->Rumble.wLeftMotorSpeed, feedback->Rumble.wRightMotorSpeed);
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputDebugInitKeyboardQueue(PXINPUT_DEBUG_KEYQUEUE_PARAMETERS parameters)
{
	(void)parameters;
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputDebugGetKeystroke(PXINPUT_DEBUG_KEYSTROKE keystroke)
{
	memset(keystroke, 0, sizeof(*keystroke));
	return ERROR_HANDLE_EOF;
}

/* ---------- the desktop ports' keyboard and mouse */

unsigned long halo_keyboard_actions(short controller_index)
{
	(void)controller_index;
	return 0;
}

int halo_linux_mouse_look(short gamepad_index, float *yaw, float *pitch)
{
	(void)gamepad_index;
	*yaw = 0.0f;
	*pitch = 0.0f;
	return 0;
}

int halo_linux_mouse_aiming(short gamepad_index)
{
	(void)gamepad_index;
	return 0;
}

/* the controls menu's names of inputs (port/linux/game/menu_functions.c) */
void halo_input_name(int input, char *name, size_t size)
{
	snprintf(name, size, "%d", input);
}

/* the controls menu's capture of a new binding: none comes */
void platform_binding_capture_begin(void)
{
}

int platform_binding_capture_poll(int *input)
{
	(void)input;
	return 0;
}

/* the menus' text fields (port/linux/game/menu_functions.c): typed on the
game's own on-screen keyboard */
int platform_text_field_on_screen(void)
{
	return TRUE;
}

/* the desktop's typing into a text field: none */
void platform_text_field(int typing, int password)
{
	(void)typing;
	(void)password;
}

void platform_text_typing(int typing)
{
	(void)typing;
}

/* automated network tests (port/linux/game/network_test.c) */
void test_input_hold_action(int hold)
{
	(void)hold;
}
