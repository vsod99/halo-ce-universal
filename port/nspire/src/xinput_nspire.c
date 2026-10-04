/*
XINPUT_NSPIRE.C

The Xbox controller on the TI-Nspire CX II's keypad and touchpad
(port/nspire/README.md has the table):

	8 2 4 6, 7 9 1 3   left stick (move; the corners are diagonals)
	touchpad drag      aim, as the desktop's mouse does
	touchpad edges     right stick (turn), pressed
	touchpad centre    right trigger (fire), pressed; enter too
	0  A (jump)        .  B (melee)        (-)  X (action, reload)
	x  Y (weapon)      +  left trigger     -    white (flashlight)
	/  black (grenade) 5  left stick click (crouch)
	tab  right stick click (zoom)          menu  start
	esc, held for a second, quits to the calculator
	doc  watch cutscenes (they are fast-forwarded undrawn otherwise)
	var  write the next frame drawn to halo_frame.tns (soft_capture.c)
	r    start or end recording a demo (nspire_demo.c)

Port 0 is the only controller. There is no debug keyboard.
*/

#include "platform.h"
#include "nspire.h"

#include <keys.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* libndls (its header's BOOL is not the Xbox SDK's) */
typedef struct
{
	unsigned char contact;
	unsigned char proximity;
	unsigned short x;
	unsigned short y;
	unsigned char x_velocity;
	unsigned char y_velocity;
	unsigned short dummy;
	unsigned char pressed;
	unsigned char arrow;
} nspire_touchpad_report;

typedef struct
{
	unsigned short width;
	unsigned short height;
} nspire_touchpad_info;

int isKeyPressed(const t_key *key);
int _is_touchpad(void);
int touchpad_scan(nspire_touchpad_report *report);
nspire_touchpad_info *touchpad_getinfo(void);

#define PORT_COUNT 4
#define KEY(name) isKeyPressed(&KEY_NSPIRE_##name)
/* radians of turn for the width of the touchpad */
#define TOUCHPAD_TURN 2.4f
#define QUIT_HOLD_TICKS 32768ULL

XPP_DEVICE_TYPE XDEVICE_TYPE_GAMEPAD_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_MEMORY_UNIT_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_DEBUG_KEYBOARD_TABLE;

static struct
{
	BOOL open;
	DWORD packet_number;
	XINPUT_GAMEPAD previous;
	BOOL reported;
} controller;

void nspire_capture_poll(void);
#define capture_poll nspire_capture_poll

/* nspire_demo.c */
int nspire_demo_playing(void);
void nspire_demo_toggle(void);
void nspire_demo_quit(void);
void nspire_demo_gamepad(XINPUT_GAMEPAD *pad);
int nspire_demo_look(int looked, float *yaw, float *pitch);
const char *nspire_demo_status(void);

static struct
{
	BOOL touching;
	BOOL record_was_down;
	int last_x, last_y;
	float pending_yaw, pending_pitch;
	unsigned long long escape_since;
} pad_state;

/* ---------- the touchpad as a mouse */

static void touchpad_poll(XINPUT_GAMEPAD *pad)
{
	nspire_touchpad_report report;
	nspire_touchpad_info *info;

	if (!_is_touchpad() || touchpad_scan(&report) != 0)
		return;
	info = touchpad_getinfo();
	if (report.pressed)
	{
		/* a click: the edges turn, the centre fires */
		pad_state.touching = FALSE;
		switch (report.arrow)
		{
		case 1: pad->sThumbRY = 32767; break;
		case 2: pad->sThumbRY = 23170; pad->sThumbRX = 23170; break;
		case 3: pad->sThumbRX = 32767; break;
		case 4: pad->sThumbRY = -23170; pad->sThumbRX = 23170; break;
		case 5: pad->sThumbRY = -32767; break;
		case 6: pad->sThumbRY = -23170; pad->sThumbRX = -23170; break;
		case 7: pad->sThumbRX = -32767; break;
		case 8: pad->sThumbRY = 23170; pad->sThumbRX = -23170; break;
		default: pad->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] = 0xFF; break;
		}
		return;
	}
	if (!report.contact)
	{
		pad_state.touching = FALSE;
		return;
	}
	if (pad_state.touching && info && info->width)
	{
		/* dragging: the view follows the finger */
		pad_state.pending_yaw -= (float)(report.x - pad_state.last_x) * TOUCHPAD_TURN / (float)info->width;
		pad_state.pending_pitch += (float)(report.y - pad_state.last_y) * TOUCHPAD_TURN / (float)info->width;
	}
	pad_state.touching = TRUE;
	pad_state.last_x = report.x;
	pad_state.last_y = report.y;
}

/* radians of yaw and pitch dragged since the last call; the game adds these
to the facing change of the player on gamepad 0 (source/game/player_control.c) */
int halo_linux_mouse_look(short gamepad_index, float *yaw, float *pitch)
{
	int looked;

	*yaw = 0.0f;
	*pitch = 0.0f;
	if (gamepad_index != 0)
		return FALSE;
	looked = pad_state.pending_yaw != 0.0f || pad_state.pending_pitch != 0.0f;
	*yaw = pad_state.pending_yaw;
	*pitch = pad_state.pending_pitch;
	pad_state.pending_yaw = 0.0f;
	pad_state.pending_pitch = 0.0f;
	/* (a demo keeps the drag, or gives its own) */
	return nspire_demo_look(looked, yaw, pitch);
}

/* aiming by touch keeps the controller's aim assist, which the touchpad's
coarse drags need */
int halo_linux_mouse_aiming(short gamepad_index)
{
	(void)gamepad_index;
	return FALSE;
}

/* ---------- the keypad */

static BYTE analog(int down)
{
	return down ? 0xFF : 0x00;
}

static void keypad_poll(XINPUT_GAMEPAD *pad)
{
	int x = 0, y = 0;

	if (KEY(8) || KEY(7) || KEY(9)) y++;
	if (KEY(2) || KEY(1) || KEY(3)) y--;
	if (KEY(6) || KEY(9) || KEY(3)) x++;
	if (KEY(4) || KEY(7) || KEY(1)) x--;
	if (x || y)
	{
		float length = (x && y) ? 0.70710678f : 1.0f;

		pad->sThumbLX = (SHORT)(x * 32767 * length);
		pad->sThumbLY = (SHORT)(y * 32767 * length);
	}

	if (KEY(MENU)) pad->wButtons |= XINPUT_GAMEPAD_START;
	if (KEY(5)) pad->wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
	if (KEY(TAB)) pad->wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;

	pad->bAnalogButtons[XINPUT_GAMEPAD_A] |= analog(KEY(0));
	pad->bAnalogButtons[XINPUT_GAMEPAD_B] |= analog(KEY(PERIOD));
	pad->bAnalogButtons[XINPUT_GAMEPAD_X] |= analog(KEY(NEGATIVE));
	pad->bAnalogButtons[XINPUT_GAMEPAD_Y] |= analog(KEY(MULTIPLY));
	pad->bAnalogButtons[XINPUT_GAMEPAD_WHITE] |= analog(KEY(MINUS));
	pad->bAnalogButtons[XINPUT_GAMEPAD_BLACK] |= analog(KEY(DIVIDE));
	pad->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] |= analog(KEY(PLUS));
	pad->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] |= analog(KEY(ENTER));

	/* r: a demo */
	if (KEY(R) && !pad_state.record_was_down)
		nspire_demo_toggle();
	pad_state.record_was_down = KEY(R) != 0;

	/* esc held quits: there is no other way out */
	capture_poll();
	if (KEY(ESC))
	{
		unsigned long long now = nspire_ticks();

		if (!pad_state.escape_since)
			pad_state.escape_since = now;
		else if (now - pad_state.escape_since >= QUIT_HOLD_TICKS)
		{
			extern void nspire_checkpoint_persist(void);

			nspire_log("esc held: quitting");
			nspire_demo_quit();
			/* (the last checkpoint kept, for the next start to go back to;
			not a demo's, which would take the place of the level's own) */
			if (!nspire_demo_playing())
				nspire_checkpoint_persist();
			exit(EXIT_SUCCESS);
		}
	}
	else
	{
		pad_state.escape_since = 0;
	}
}

/* ---------- cutscenes fast-forwarded

b30's opening cutscene cannot be skipped, and drawn it runs for many
minutes: while one plays, frames go undrawn (the game still runs every
tick) and the screen says so once a second. doc switches to watching them. */

static struct
{
	BOOL watch;
	BOOL doc_was_down;
	unsigned long long shown;
} fast_forward;

/* whether the last frame went undrawn (game_time.c lets it run more ticks) */
static int fast_forwarding;

int nspire_fast_forwarding(void)
{
	return fast_forwarding;
}

/* var: the next frame to halo_frame.tns (soft_capture.c) */
void soft_capture_request(void);
static BOOL var_was_down;

void nspire_capture_poll(void)
{
	BOOL var = KEY(VAR) != 0;

	if (var && !var_was_down)
		soft_capture_request();
	var_was_down = var;
}

int nspire_fast_forward(int cinematic)
{
	BOOL doc = KEY(DOC) != 0;

	capture_poll();
	unsigned long long now;

	/* (a demo plays the same way every time) */
	if (doc && !fast_forward.doc_was_down && !nspire_demo_playing())
		fast_forward.watch = !fast_forward.watch;
	fast_forward.doc_was_down = doc;
	fast_forwarding = cinematic && !fast_forward.watch;
	if (!fast_forwarding)
		return 0;
	now = nspire_ticks();
	if (!fast_forward.shown || now - fast_forward.shown >= 32768ULL)
	{
		unsigned short *pixels = nspire_video_pixels();

		fast_forward.shown = now;
		if (pixels)
			memset(pixels, 0, NSPIRE_SCREEN_WIDTH * NSPIRE_SCREEN_HEIGHT * sizeof(unsigned short));
		nspire_video_text(8, 100, "Fast-forwarding the cutscene...");
		nspire_video_text(8, 116, "(doc: watch it instead)");
		nspire_video_present();
	}
	/* (the input thread runs while this one waits, and Present, which
	otherwise lets it, is not called) */
	nspire_yield();
	return 1;
}

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
	if (device_type == XDEVICE_TYPE_GAMEPAD && !controller.reported)
	{
		*insertions = XDEVICE_PORT0_MASK;
		controller.reported = TRUE;
	}
	return *insertions != 0;
}

HANDLE WINAPI XInputOpen(PXPP_DEVICE_TYPE device_type, DWORD port, DWORD slot,
	PXINPUT_POLLING_PARAMETERS polling_parameters)
{
	(void)slot;
	(void)polling_parameters;
	if (device_type == XDEVICE_TYPE_GAMEPAD && port == 0)
	{
		controller.open = TRUE;
		return (HANDLE)&controller;
	}
	SetLastError(ERROR_DEVICE_NOT_CONNECTED);
	return NULL;
}

VOID WINAPI XInputClose(HANDLE device)
{
	if (device == (HANDLE)&controller)
		controller.open = FALSE;
}

DWORD WINAPI XInputGetState(HANDLE device, PXINPUT_STATE state)
{
	memset(state, 0, sizeof(*state));
	if (device != (HANDLE)&controller || !controller.open)
		return ERROR_DEVICE_NOT_CONNECTED;
	keypad_poll(&state->Gamepad);
	touchpad_poll(&state->Gamepad);
	nspire_demo_gamepad(&state->Gamepad);
	if (memcmp(&state->Gamepad, &controller.previous, sizeof(state->Gamepad)))
	{
		controller.packet_number++;
		controller.previous = state->Gamepad;
	}
	state->dwPacketNumber = controller.packet_number;
	return ERROR_SUCCESS;
}

/* the controller as last read, for the status line (d3d8_soft.c) */
void nspire_input_describe(char *text, unsigned long size)
{
	const XINPUT_GAMEPAD *pad = &controller.previous;
	unsigned long analog = 0;
	int index;

	for (index = 0; index < 8; index++)
	{
		if (pad->bAnalogButtons[index] > 0x80)
			analog |= 1UL << index;
	}
	snprintf(text, size, "%sL%+d,%+d R%+d,%+d b%02lx%02x", nspire_demo_status(), pad->sThumbLX / 3277, pad->sThumbLY / 3277,
		pad->sThumbRX / 3277, pad->sThumbRY / 3277, analog, pad->wButtons & 0xFF);
}

/* no rumble motor */
DWORD WINAPI XInputSetState(HANDLE device, PXINPUT_FEEDBACK feedback)
{
	if (!feedback)
		return ERROR_INVALID_PARAMETER;
	feedback->Header.dwStatus = ERROR_SUCCESS;
	return device == (HANDLE)&controller ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
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
