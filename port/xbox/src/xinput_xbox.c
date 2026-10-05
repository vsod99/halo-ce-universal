/*
XINPUT_XBOX.C

The Xbox SDK's controller calls over the console's own controllers
(nxdk_gamepads.c, nxdk's USB driver). A controller's report is the SDK's
XINPUT_GAMEPAD as it is, so XInputGetState copies it; XInputSetState sets
the motors. No debug keyboard (the console's, a USB keyboard, would need
nxdk's HID driver), so no console. The desktop ports' keyboard, mouse and
binding hooks the game calls answer as having none.

debug.test_input "press:SECONDS:BUTTON[,BUTTON...];..." presses buttons on
the first controller at those seconds after the controllers are first read,
each held 0.12 s and those of one time 0.52 s apart (as
`tools/xbox_dev.py run --input` does it, which needs no access to the
Mac's screen); the desktop ports' "bot" and "look" scripts are not here.
*/

#include "platform.h"
#include "halo_keyboard.h"
#include "nxdk_platform.h"
#include "port_config.h"

#include <stdio.h>
#include <stdlib.h>
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

/* ---------- scripted presses (debug.test_input "press:...") */

#define TEST_PRESS_HOLD_MS 120
#define TEST_PRESS_GAP_MS 520
#define TEST_PRESS_MAXIMUM 64

struct test_press
{
	unsigned long at_ms;
	WORD buttons;
	signed char analog;
	SHORT thumb_lx, thumb_ly;
};

static struct test_press test_presses[TEST_PRESS_MAXIMUM];
static int test_press_count = -1;
static unsigned long test_press_start_ms;

static int test_press_parse_button(const char *name, size_t length, struct test_press *press)
{
	static const struct
	{
		const char *name;
		WORD buttons;
		signed char analog;
		SHORT thumb_lx, thumb_ly;
	} names[] =
	{
		{ "a", 0, XINPUT_GAMEPAD_A, 0, 0 },
		{ "b", 0, XINPUT_GAMEPAD_B, 0, 0 },
		{ "x", 0, XINPUT_GAMEPAD_X, 0, 0 },
		{ "y", 0, XINPUT_GAMEPAD_Y, 0, 0 },
		{ "black", 0, XINPUT_GAMEPAD_BLACK, 0, 0 },
		{ "white", 0, XINPUT_GAMEPAD_WHITE, 0, 0 },
		{ "lt", 0, XINPUT_GAMEPAD_LEFT_TRIGGER, 0, 0 },
		{ "rt", 0, XINPUT_GAMEPAD_RIGHT_TRIGGER, 0, 0 },
		{ "up", XINPUT_GAMEPAD_DPAD_UP, -1, 0, 0 },
		{ "down", XINPUT_GAMEPAD_DPAD_DOWN, -1, 0, 0 },
		{ "left", XINPUT_GAMEPAD_DPAD_LEFT, -1, 0, 0 },
		{ "right", XINPUT_GAMEPAD_DPAD_RIGHT, -1, 0, 0 },
		{ "start", XINPUT_GAMEPAD_START, -1, 0, 0 },
		{ "back", XINPUT_GAMEPAD_BACK, -1, 0, 0 },
		{ "lup", 0, -1, 0, 32767 },
		{ "ldown", 0, -1, 0, -32767 },
		{ "lleft", 0, -1, -32767, 0 },
		{ "lright", 0, -1, 32767, 0 },
	};
	size_t index;

	for (index = 0; index < sizeof(names) / sizeof(names[0]); index++)
	{
		if (strlen(names[index].name) == length && !strncmp(names[index].name, name, length))
		{
			press->buttons = names[index].buttons;
			press->analog = names[index].analog;
			press->thumb_lx = names[index].thumb_lx;
			press->thumb_ly = names[index].thumb_ly;
			return 1;
		}
	}
	platform_log("debug.test_input: no button \"%.*s\"", (int)length, name);
	return 0;
}

/* "press:32:a;35:down,a": at 32 s A, at 35 s down and half a second on A */
static void test_press_load(void)
{
	const char *setting = config_string("debug.test_input");
	const char *item;

	test_press_count = 0;
	test_press_start_ms = GetTickCount();
	if (strncmp(setting, "press:", 6))
		return;
	for (item = setting + 6; *item; )
	{
		const char *end = item + strcspn(item, ";");
		const char *colon = memchr(item, ':', (size_t)(end - item));
		unsigned long at_ms;
		const char *name;

		if (!colon)
		{
			platform_log("debug.test_input: \"%.*s\" is not SECONDS:BUTTON", (int)(end - item), item);
			break;
		}
		at_ms = (unsigned long)(atof(item) * 1000.0);
		for (name = colon + 1; name < end && test_press_count < TEST_PRESS_MAXIMUM; )
		{
			size_t length = strcspn(name, ",;");
			struct test_press *press = &test_presses[test_press_count];

			if (length > (size_t)(end - name))
				length = (size_t)(end - name);
			if (test_press_parse_button(name, length, press))
			{
				press->at_ms = at_ms;
				test_press_count++;
				at_ms += TEST_PRESS_GAP_MS;
			}
			name += length;
			if (*name == ',')
				name++;
		}
		item = *end ? end + 1 : end;
	}
	platform_log("debug.test_input: %d scripted presses", test_press_count);
}

static void test_press_apply(XINPUT_GAMEPAD *pad)
{
	unsigned long now;
	int index;

	if (test_press_count < 0)
		test_press_load();
	if (!test_press_count)
		return;
	now = GetTickCount() - test_press_start_ms;
	for (index = 0; index < test_press_count; index++)
	{
		const struct test_press *press = &test_presses[index];

		if (now < press->at_ms || now >= press->at_ms + TEST_PRESS_HOLD_MS)
			continue;
		pad->wButtons |= press->buttons;
		if (press->analog >= 0)
			pad->bAnalogButtons[press->analog] = 255;
		if (press->thumb_lx)
			pad->sThumbLX = press->thumb_lx;
		if (press->thumb_ly)
			pad->sThumbLY = press->thumb_ly;
	}
}

DWORD WINAPI XInputGetState(HANDLE device, PXINPUT_STATE state)
{
	int port = controller_port(device);
	unsigned long reports;

	memset(state, 0, sizeof(*state));
	if (port < 0 || !xbox_gamepad_report(port, (unsigned char *)&state->Gamepad, &reports))
		return ERROR_DEVICE_NOT_CONNECTED;
	if (port == 0)
	{
		XINPUT_GAMEPAD before = state->Gamepad;

		test_press_apply(&state->Gamepad);
		/* (a changed report is a new packet, as the hardware's) */
		if (memcmp(&before, &state->Gamepad, sizeof(before)))
			reports += 0x10000;
	}
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
