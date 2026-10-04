/*
XINPUT_NULL.C

The Xbox port's controllers until it reads its own (nxdk's USB controller
driver, next in the plan's first phase): none is plugged in, so the game
waits at its menus, which is what the development loop's first runs need.
The desktop ports' keyboard, mouse and binding hooks the game calls answer
as having none either.
*/

#include "platform.h"
#include "halo_keyboard.h"

#include <stdio.h>
#include <string.h>

XPP_DEVICE_TYPE XDEVICE_TYPE_GAMEPAD_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_MEMORY_UNIT_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_DEBUG_KEYBOARD_TABLE;

/* ---------- XAPI */

VOID WINAPI XInitDevices(DWORD preallocation_type_count, PXDEVICE_PREALLOC_TYPE preallocation_types)
{
	(void)preallocation_type_count;
	(void)preallocation_types;
}

BOOL WINAPI XGetDeviceChanges(PXPP_DEVICE_TYPE device_type, PDWORD insertions, PDWORD removals)
{
	(void)device_type;
	*insertions = 0;
	*removals = 0;
	return FALSE;
}

HANDLE WINAPI XInputOpen(PXPP_DEVICE_TYPE device_type, DWORD port, DWORD slot,
	PXINPUT_POLLING_PARAMETERS polling_parameters)
{
	(void)device_type;
	(void)port;
	(void)slot;
	(void)polling_parameters;
	SetLastError(ERROR_DEVICE_NOT_CONNECTED);
	return NULL;
}

VOID WINAPI XInputClose(HANDLE device)
{
	(void)device;
}

DWORD WINAPI XInputGetState(HANDLE device, PXINPUT_STATE state)
{
	(void)device;
	memset(state, 0, sizeof(*state));
	return ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputSetState(HANDLE device, PXINPUT_FEEDBACK feedback)
{
	(void)device;
	if (!feedback)
		return ERROR_INVALID_PARAMETER;
	feedback->Header.dwStatus = ERROR_SUCCESS;
	return ERROR_DEVICE_NOT_CONNECTED;
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

/* the desktop's typing into a text field (the menus' player name): the Xbox
will use its on-screen keyboard */
void platform_text_field(int typing)
{
	(void)typing;
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
