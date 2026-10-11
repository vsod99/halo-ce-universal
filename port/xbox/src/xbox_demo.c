/*
XBOX_DEMO.C

A demo: the same stretch of play, run the same way every time, to time and
profile builds against each other in a scene that moves (the player running
into a fight, the view turning) rather than one standing still.

Recording: Back held while both sticks are clicked, during play, saves the
game where it stands (the game's core save, E:\halo\z\core\demo.bin) and from
the next frame keeps every reading of the first controller. The same
buttons again (or the room running out) end it, and the readings go to
E:\halo\demo.bin with the level's name. The buttons are kept from the game
while held, so that Back alone does what it did.

Playing (debug.demo "play", `tools/xbox_dev.py run --env HALO_DEMO=play`):
the level is started a few seconds into the main menu, as the campaign's
menus start one, the core save is loaded once it has loaded, the game gets
the readings kept instead of the controller's, and the program logs the
frames and their time and quits when they run out.

While a demo records or plays, every frame runs a fixed time (main.c), so
that the game ticks alike however long the frames take to draw: recorded in
xemu at 10 frames a second, the game goes at a third of its speed. The
game's random seed is not in its save: the recording keeps it, and the
demo plays from it, so that the AI and every other chance goes as it did.
Each reading keeps the game time it was taken at, and the recording the
seed as it ended: a reading played at another time, or another seed at the
end, means the game went another way, and the log says so.
*/

#include "platform.h"
#include "nxdk_platform.h"
#include "port_config.h"
#include "xbox_demo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEMO_INPUT_FILE "E:\\halo\\demo.bin"
/* the game's core save, in its core folder (game_state_xbox.c) */
#define DEMO_CORE_NAME "demo.bin"
/* a recording's own, until it ends: then they become the demo's, so that
one not ended (the console turned off) leaves the last demo whole */
#define RECORDING_INPUT_FILE "E:\\halo\\demo_recording.bin"
#define RECORDING_CORE_NAME "demo_recording.bin"
#define DEMO_MAGIC 0x344D4458UL /* XDM4 */
/* (at a reading a frame, over 30 minutes at xemu's 10 frames a second) */
#define MAXIMUM_READINGS 20000
/* the main menu's frames before playing starts the level (its ui.map loaded) */
#define PLAY_START_FRAMES 90
#define RECORD_BUTTONS (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB)

/* the game's (source/game/game.h, source/main/main.h,
port/linux/game/menu_functions.c) */
long game_time_get(void);
unsigned char game_in_progress(void);
/* (the main menu is a game too: ui.map's) */
unsigned char main_menu_is_active(void);
void main_save_core_name(char const *core_name);
void main_load_core_name_at_startup(char const *core_name);
void main_load_core_name(char const *core_name);
void pc_menu_start_map(char const *map_name);
char *main_get_map_name(void);
short main_get_difficulty(void);
void main_set_difficulty(short difficulty);
/* the game's random numbers (source/math/random_math.c): not part of the
game state, so not in its save */
unsigned long get_random_seed(void);
unsigned long *get_global_random_seed_address(void);
unsigned long *get_global_local_random_seed_address(void);
/* back to the dashboard (nxdk_main.c) */
void platform_request_quit(void);

struct demo_header
{
	unsigned long magic;
	unsigned long count;
	long difficulty;
	char level[256];
	/* the game time the recording started at, and the game's random seed
	then and as it ended: every random number the game takes moves it, so
	a demo played the same way ends with the same */
	long start_game_time;
	unsigned long start_seed;
	unsigned long end_seed;
	/* the seed of the effects' and sounds' random numbers, at the start */
	unsigned long start_local_seed;
};

struct demo_reading
{
	long game_time;
	/* the game's random seed as the reading was taken */
	unsigned long seed;
	XINPUT_GAMEPAD pad;
};

enum
{
	_demo_off,
	_demo_waiting_for_save,
	_demo_waiting_for_load,
	_demo_recording,
	_demo_playing,
	_demo_played
};

static struct
{
	int state;
	int read_setting;
	BOOL buttons_held;
	struct demo_header header;
	long next;
	BOOL out_of_step;
	BOOL seed_differs;
	long frames;
	long menu_frames;
	BOOL level_started;
	/* the frames run a fixed time since then, and the system's milliseconds
	then (xbox_demo_milliseconds) */
	unsigned long fixed_frames;
	unsigned long fixed_start_ms;
	unsigned long started_ms;
	/* the time stamp counter and the GPU waits' cycles at the start */
	unsigned long long started_cycles, started_waited;
	long first_game_time;
} demo;

static struct demo_reading readings[MAXIMUM_READINGS];

/* the game time of a level being played, else -1 */
static long game_time_or_none(void)
{
	return game_in_progress() && !main_menu_is_active() ? game_time_get() : -1;
}

/* a core save's file: z:\core\<name>, below the save root
(game_state_xbox.c, xbox_files.c) */
static const char *core_file(const char *name)
{
	static char path[2][300];
	static int which;
	char *separator;

	which ^= 1;
	snprintf(path[which], sizeof(path[which]), "%s\\z\\core\\%s", platform_save_root(), name);
	/* (the console's paths take backslashes) */
	for (separator = path[which]; *separator; separator++)
	{
		if (*separator == '/')
			*separator = '\\';
	}
	return path[which];
}

/* the core save's size in bytes, -1 when there is none */
static long core_size(void)
{
	FILE *file = fopen(core_file(DEMO_CORE_NAME), "rb");
	long size = -1;

	if (file && !fseek(file, 0, SEEK_END))
		size = ftell(file);
	if (file)
		fclose(file);
	return size;
}

static void demo_setting(void)
{
	if (demo.read_setting)
		return;
	demo.read_setting = TRUE;
	if (!strcmp(config_string("debug.demo"), "play"))
		demo.state = _demo_playing;
}

/* ---------- playing */

static void load_readings(void)
{
	FILE *file = fopen(DEMO_INPUT_FILE, "rb");
	BOOL valid = file && fread(&demo.header, sizeof(demo.header), 1, file) == 1 && demo.header.magic == DEMO_MAGIC &&
		demo.header.count > 0 && demo.header.count <= MAXIMUM_READINGS &&
		fread(readings, sizeof(readings[0]), demo.header.count, file) == demo.header.count;

	if (file)
		fclose(file);
	demo.header.level[sizeof(demo.header.level) - 1] = 0;
	if (!valid)
	{
		platform_log("demo: nothing to play in %s (record one: Back held, both sticks clicked); quitting",
			DEMO_INPUT_FILE);
		platform_log("== XBOX DONE ==");
		platform_request_quit();
		demo.state = _demo_played;
		return;
	}
	demo.first_game_time = readings[0].game_time;
	platform_log("demo: %lu readings of %s from game time %ld; its save %s, %ld bytes", demo.header.count,
		demo.header.level, demo.first_game_time, core_file(DEMO_CORE_NAME), core_size());
}

static void finish_playing(void)
{
	unsigned long milliseconds = GetTickCount() - demo.started_ms;
	unsigned long long cycles = __builtin_ia32_rdtsc() - demo.started_cycles;
	unsigned long long waited = xbox_gpu_waited_cycles() - demo.started_waited;
	/* (the processor's own time: the share of the cycles not spent waiting
	on the GPU, which xemu's GPU makes much of the frame's time) */
	unsigned long own = cycles ? (unsigned long)((cycles - waited) * milliseconds / cycles) : milliseconds;
	unsigned long frames = demo.frames ? (unsigned long)demo.frames : 1UL;

	demo.state = _demo_played;
	platform_log("demo: done: %ld frames, %ld ticks in %lu ms, %lu.%lu ms a frame, %lu.%lu of the processor's own "
		"(%lu%% waiting on the GPU)%s", demo.frames, game_time_or_none() - demo.first_game_time, milliseconds,
		milliseconds / frames, milliseconds * 10UL / frames % 10UL, own / frames, own * 10UL / frames % 10UL,
		cycles ? (unsigned long)(waited * 100 / cycles) : 0UL,
		demo.out_of_step ? " (the game went another way: not comparable)" : "");
	if (get_random_seed() != demo.header.end_seed)
		platform_log("demo: the game's random seed ended at %08lx, not the recording's %08lx: the game went another way "
			"(not comparable)", get_random_seed(), demo.header.end_seed);
	else
		platform_log("demo: the game went as recorded (its random seed ended at the recording's, %08lx)",
			demo.header.end_seed);
	platform_log("== XBOX DONE ==");
	platform_request_quit();
}

/* the next reading, once the game reaches its time; NULL before */
static const struct demo_reading *next_reading(void)
{
	const struct demo_reading *reading;
	long now = game_time_or_none();

	if (demo.next >= (long)demo.header.count)
	{
		finish_playing();
		return NULL;
	}
	reading = &readings[demo.next];
	if (now < 0 || now < reading->game_time)
		return NULL;
	if (demo.next == 0)
	{
		demo.started_ms = GetTickCount();
		demo.started_cycles = __builtin_ia32_rdtsc();
		demo.started_waited = xbox_gpu_waited_cycles();
		platform_log("demo: playing from game time %ld", now);
	}
	if (reading->seed != get_random_seed() && !demo.seed_differs)
	{
		platform_log("demo: the game's random seed differs from reading %ld on (game time %ld): %08lx recorded, "
			"%08lx now", demo.next, now, reading->seed, get_random_seed());
		demo.seed_differs = TRUE;
	}
	if (now != reading->game_time && !demo.out_of_step)
	{
		platform_log("demo: out of step at reading %ld: recorded at %ld, played at %ld", demo.next,
			reading->game_time, now);
		demo.out_of_step = TRUE;
	}
	demo.next++;
	return reading;
}

/* ---------- recording */

static void stop_recording(void)
{
	FILE *file;
	BOOL written;

	if (demo.state != _demo_recording)
	{
		demo.state = _demo_off;
		return;
	}
	demo.state = _demo_off;
	demo.header.magic = DEMO_MAGIC;
	demo.header.end_seed = get_random_seed();
	file = fopen(RECORDING_INPUT_FILE, "wb");
	written = file && fwrite(&demo.header, sizeof(demo.header), 1, file) == 1 &&
		fwrite(readings, sizeof(readings[0]), demo.header.count, file) == demo.header.count;
	if (file && fclose(file) != 0)
		written = FALSE;
	/* (the recording's save and readings become the demo's together) */
	if (written)
	{
		remove(core_file(DEMO_CORE_NAME));
		remove(DEMO_INPUT_FILE);
		written = !rename(core_file(RECORDING_CORE_NAME), core_file(DEMO_CORE_NAME)) &&
			!rename(RECORDING_INPUT_FILE, DEMO_INPUT_FILE);
	}
	platform_log("demo: %lu readings (%ld frames) of %s %s %s; its save %s, %ld bytes", demo.header.count,
		demo.frames, demo.header.level, written ? "written to" : "could not be written to", DEMO_INPUT_FILE,
		core_file(DEMO_CORE_NAME), core_size());
}

static void toggle_recording(void)
{
	if (demo.state == _demo_off)
	{
		char const *level = main_get_map_name();

		if (game_time_or_none() < 0 || !level || !*level)
			return;
		/* (the game saves before the next frame's controller readings;
		recording starts there: xbox_demo_core_saved) */
		memset(&demo.header, 0, sizeof(demo.header));
		snprintf(demo.header.level, sizeof(demo.header.level), "%s", level);
		demo.header.difficulty = main_get_difficulty();
		platform_log("demo: recording asked for in %s; saving the game", level);
		demo.state = _demo_waiting_for_save;
		main_save_core_name(RECORDING_CORE_NAME);
	}
	else if (demo.state == _demo_waiting_for_save || demo.state == _demo_waiting_for_load ||
		demo.state == _demo_recording)
	{
		stop_recording();
	}
}

/* ---------- the game's hooks */

/* Loading a save is not going on from where it was made: the game takes
its after-load steps, which build its caches and connections again. So the
recording loads its own save at once, and it and the demo played both go on
from a save just loaded, with the random seeds the save leaves out. The main
loop saves and loads before it reads the controllers: the first reading is
the one after the load in both. */
void xbox_demo_core_saved(void)
{
	if (demo.state != _demo_waiting_for_save)
		return;
	demo.state = _demo_waiting_for_load;
	main_load_core_name(RECORDING_CORE_NAME);
}

void xbox_demo_core_loaded(void)
{
	if (demo.state == _demo_waiting_for_load)
	{
		demo.state = _demo_recording;
		demo.header.count = 0;
		demo.frames = 0;
		demo.header.start_game_time = game_time_or_none();
		demo.header.start_seed = get_random_seed();
		demo.header.start_local_seed = *get_global_local_random_seed_address();
		platform_log("demo: recording from game time %ld, random seed %08lx", demo.header.start_game_time,
			demo.header.start_seed);
		return;
	}
	if (demo.state != _demo_playing || !demo.level_started)
		return;
	*get_global_random_seed_address() = demo.header.start_seed;
	*get_global_local_random_seed_address() = demo.header.start_local_seed;
	platform_log("demo: the save loaded at game time %ld (recorded at %ld), random seed %08lx", game_time_or_none(),
		demo.header.start_game_time, demo.header.start_seed);
}

int xbox_demo_fixed_frames(void)
{
	return demo.state == _demo_recording || demo.state == _demo_playing;
}

long xbox_demo_milliseconds(void)
{
	/* (a frame is a tick and a half: 50 ms) */
	if (xbox_demo_fixed_frames())
		return (long)(demo.fixed_start_ms + demo.fixed_frames * 50UL);
	return (long)GetTickCount();
}

void xbox_demo_frame(void)
{
	demo_setting();
	if (xbox_demo_fixed_frames())
	{
		if (!demo.fixed_frames && !demo.fixed_start_ms)
			demo.fixed_start_ms = GetTickCount();
		demo.fixed_frames++;
	}
	if (demo.state == _demo_playing && !demo.level_started && main_menu_is_active() &&
		++demo.menu_frames >= PLAY_START_FRAMES)
	{
		load_readings();
		if (demo.state != _demo_playing)
			return;
		demo.level_started = TRUE;
		platform_log("demo: starting %s", demo.header.level);
		main_load_core_name_at_startup(DEMO_CORE_NAME);
		pc_menu_start_map(demo.header.level);
		main_set_difficulty((short)demo.header.difficulty);
	}
}

void xbox_demo_gamepad(XINPUT_GAMEPAD *pad)
{
	BOOL held;

	demo_setting();
	if (demo.state == _demo_playing || demo.state == _demo_played)
	{
		const struct demo_reading *reading = demo.state == _demo_playing && demo.level_started ? next_reading() : NULL;

		memset(pad, 0, sizeof(*pad));
		if (reading)
		{
			*pad = reading->pad;
			demo.frames++;
		}
		return;
	}
	/* Back with both sticks clicked starts or ends a recording, and the
	game sees none of the three while they are held */
	held = (pad->wButtons & XINPUT_GAMEPAD_BACK) &&
		(pad->wButtons & (XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB)) ==
		(XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB);
	if (held && !demo.buttons_held)
		toggle_recording();
	demo.buttons_held = held;
	if (held)
		pad->wButtons &= ~RECORD_BUTTONS;
	if (demo.state == _demo_recording)
	{
		struct demo_reading *reading;

		if (demo.header.count >= MAXIMUM_READINGS)
		{
			platform_log("demo: no room for more readings");
			stop_recording();
			return;
		}
		reading = &readings[demo.header.count++];
		reading->game_time = game_time_or_none();
		reading->seed = get_random_seed();
		reading->pad = *pad;
		demo.frames++;
	}
}
