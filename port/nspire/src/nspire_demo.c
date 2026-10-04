/*
NSPIRE_DEMO.C

A demo: the same stretch of play, run the same way every time, to time
builds against each other.

Recording: r, during play, saves the game where it stands (a checkpoint,
written to halo_demo_save.tns) and from the next frame keeps every reading
of the controller and the touchpad. r again (or esc held, or the room
running out) ends it, and the readings go to halo_demo_input.tns.

Playing: the program named halo_demo.tns plays b30 with no main menu,
goes back to that checkpoint once the level has loaded, gives the game the
readings kept instead of the keypad's (esc still quits), and quits when
they run out, logging the frames and their time.

While a demo records or plays, every frame runs a fixed time (main.c), so
the game ticks alike however long the frames take to draw. Each reading
keeps the game time it was taken at: one taken at another time when played
means the game went another way, and the log says so.
*/

#include "platform.h"
#include "nspire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEMO_INPUT_FILE "halo_demo_input.tns"
#define DEMO_SAVE_FILE "halo_demo_save.tns"
/* when there is a file of this name, the frame CAPTURE_FRAME of the demo
goes to halo_frame.tns, for tools/nspire_replay (the time is then not
comparable: writing it takes seconds) */
#define DEMO_CAPTURE_FILE "halo_demo_capture.tns"
#define CAPTURE_FRAME 100
#define DEMO_MAGIC 0x314D4448UL /* HDM1 */
#define MAXIMUM_READINGS 4096
#define NONE (-1)

enum
{
	_reading_gamepad = 'G',
	_reading_look = 'M'
};

struct demo_reading
{
	long game_time;
	unsigned char kind;
	unsigned char looked;
	unsigned short unused;
	union
	{
		XINPUT_GAMEPAD pad;
		float look[2];
	} u;
};

enum
{
	_demo_off,
	_demo_waiting_for_save,
	_demo_recording,
	_demo_playing,
	_demo_played
};

static struct
{
	int state;
	BOOL play_requested;
	BOOL loaded;
	long count, next;
	BOOL out_of_step;
	long frames;
	unsigned long long started;
	long first_game_time;
	BOOL capture;
} demo;

static struct demo_reading readings[MAXIMUM_READINGS];

/* (game_time.c: NONE until a game is under way) */
long nspire_game_time_or_none(void);
int nspire_checkpoint_write(const char *path);

static void demo_path(char *path, unsigned long size, const char *name)
{
	snprintf(path, size, "%s/%s", nspire_program_directory(), name);
}

/* ---------- playing */

/* the program's name asked for the demo (nspire_main.c) */
void nspire_demo_request_play(void)
{
	demo.play_requested = TRUE;
	demo.state = _demo_playing;
}

int nspire_demo_playing(void)
{
	return demo.play_requested;
}

/* whether frames run a fixed time (main.c) */
int nspire_demo_active(void)
{
	return demo.state == _demo_recording || demo.state == _demo_playing;
}

static void load_readings(void)
{
	char path[300];
	FILE *file;
	unsigned long magic = 0;

	demo.loaded = TRUE;
	demo_path(path, sizeof(path), DEMO_INPUT_FILE);
	file = fopen(path, "rb");
	if (file && fread(&magic, sizeof(magic), 1, file) == 1 && magic == DEMO_MAGIC)
		demo.count = (long)fread(readings, sizeof(readings[0]), MAXIMUM_READINGS, file);
	if (file)
		fclose(file);
	nspire_log("demo: %ld readings from %s%s", demo.count, path,
		magic == DEMO_MAGIC ? "" : " (none: record one with r first)");
	if (!demo.count)
	{
		nspire_log("demo: nothing to play; quitting");
		exit(EXIT_SUCCESS);
	}
	demo.first_game_time = readings[0].game_time;
	demo_path(path, sizeof(path), DEMO_CAPTURE_FILE);
	file = fopen(path, "rb");
	if (file)
	{
		fclose(file);
		demo.capture = TRUE;
		nspire_log("demo: frame %d to halo_frame.tns (%s is there)", CAPTURE_FRAME, DEMO_CAPTURE_FILE);
	}
}

static void finish_playing(void)
{
	unsigned long milliseconds = (unsigned long)((nspire_ticks() - demo.started) * 1000ULL / 32768ULL);

	demo.state = _demo_played;
	nspire_log("demo: done: %ld frames, %ld ticks in %lu ms, %lu ms a frame%s", demo.frames,
		nspire_game_time_or_none() - demo.first_game_time, milliseconds,
		demo.frames ? milliseconds / (unsigned long)demo.frames : 0UL,
		demo.out_of_step ? " (the game went another way: not comparable)" : "");
	exit(EXIT_SUCCESS);
}

/* the next reading of kind, once the game reaches its time; NULL before */
static const struct demo_reading *next_reading(int kind)
{
	const struct demo_reading *reading;
	long now = nspire_game_time_or_none();

	if (!demo.loaded)
		load_readings();
	if (demo.next >= demo.count)
	{
		if (kind == _reading_gamepad)
			finish_playing();
		return NULL;
	}
	reading = &readings[demo.next];
	if (now == NONE || now < reading->game_time)
		return NULL;
	if (demo.next == 0)
	{
		demo.started = nspire_ticks();
		nspire_log("demo: playing from game time %ld", now);
	}
	if (reading->kind != kind || now != reading->game_time)
	{
		if (!demo.out_of_step)
		{
			nspire_log("demo: out of step at reading %ld: %c at %ld recorded, %c at %ld now", demo.next,
				reading->kind, reading->game_time, kind, now);
			demo.out_of_step = TRUE;
		}
		/* (the readings go on in their order regardless) */
		if (reading->kind != kind)
			return NULL;
	}
	demo.next++;
	return reading;
}

/* ---------- recording */

static void write_readings(void)
{
	char path[300];
	FILE *file;
	unsigned long magic = DEMO_MAGIC;
	int ok;

	demo_path(path, sizeof(path), DEMO_INPUT_FILE);
	file = fopen(path, "wb");
	ok = file && fwrite(&magic, sizeof(magic), 1, file) == 1 &&
		fwrite(readings, sizeof(readings[0]), (size_t)demo.count, file) == (size_t)demo.count;
	if (file && fclose(file) != 0)
		ok = FALSE;
	nspire_log("demo: %ld readings (%ld frames) %s %s", demo.count, demo.frames,
		ok ? "written to" : "could not be written to", path);
}

static void stop_recording(void)
{
	if (demo.state == _demo_recording)
		write_readings();
	demo.state = _demo_off;
}

/* r: start or end a recording (xinput_nspire.c) */
void nspire_demo_toggle(void)
{
	if (demo.play_requested)
		return;
	if (demo.state == _demo_off)
	{
		extern void main_save_map_nonsafe(void);

		if (nspire_game_time_or_none() == NONE)
			return;
		/* (the game saves at the end of this frame: nspire_demo_saved) */
		nspire_log("demo: recording asked for");
		demo.state = _demo_waiting_for_save;
		main_save_map_nonsafe();
	}
	else
	{
		stop_recording();
	}
}

/* a checkpoint was just made (nspire_checkpoint.c) */
void nspire_demo_saved(void)
{
	char path[300];

	if (demo.state != _demo_waiting_for_save)
		return;
	demo_path(path, sizeof(path), DEMO_SAVE_FILE);
	if (!nspire_checkpoint_write(path))
	{
		nspire_log("demo: the checkpoint could not be written; not recording");
		demo.state = _demo_off;
		return;
	}
	demo.state = _demo_recording;
	demo.count = 0;
	demo.frames = 0;
	nspire_log("demo: recording from game time %ld", nspire_game_time_or_none());
}

static void record(int kind, int looked, const void *data, unsigned long size)
{
	struct demo_reading *reading;

	if (demo.count >= MAXIMUM_READINGS)
	{
		nspire_log("demo: no room for more readings");
		stop_recording();
		return;
	}
	reading = &readings[demo.count++];
	memset(reading, 0, sizeof(*reading));
	reading->game_time = nspire_game_time_or_none();
	reading->kind = (unsigned char)kind;
	reading->looked = (unsigned char)looked;
	memcpy(&reading->u, data, size);
}

/* esc held: what was recorded is kept */
void nspire_demo_quit(void)
{
	stop_recording();
}

/* ---------- the readings */

/* the controller as read (XInputGetState): kept, or replaced by the demo's */
void nspire_demo_gamepad(XINPUT_GAMEPAD *pad)
{
	if (demo.state == _demo_recording)
	{
		record(_reading_gamepad, 0, pad, sizeof(*pad));
		demo.frames++;
	}
	else if (demo.state == _demo_playing)
	{
		const struct demo_reading *reading = next_reading(_reading_gamepad);

		memset(pad, 0, sizeof(*pad));
		if (reading)
		{
			*pad = reading->u.pad;
			demo.frames++;
			if (demo.capture && demo.frames == CAPTURE_FRAME)
			{
				extern void soft_capture_request(void);

				soft_capture_request();
			}
		}
	}
}

/* the touchpad's drag (halo_linux_mouse_look): kept, or the demo's */
int nspire_demo_look(int looked, float *yaw, float *pitch)
{
	if (demo.state == _demo_recording)
	{
		float look[2];

		look[0] = *yaw;
		look[1] = *pitch;
		record(_reading_look, looked, look, sizeof(look));
	}
	else if (demo.state == _demo_playing)
	{
		const struct demo_reading *reading = next_reading(_reading_look);

		*yaw = reading ? reading->u.look[0] : 0.0f;
		*pitch = reading ? reading->u.look[1] : 0.0f;
		looked = reading ? reading->looked : FALSE;
	}
	return looked;
}

/* for the status line */
const char *nspire_demo_status(void)
{
	switch (demo.state)
	{
	case _demo_waiting_for_save:
	case _demo_recording:
		return "REC ";
	case _demo_playing:
		return "DEMO ";
	default:
		return "";
	}
}
