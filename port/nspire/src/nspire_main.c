/*
NSPIRE_MAIN.C

The Nspire port's entry point, log and messages.

The game's own main() (source/shell/shell_xbox.c) takes no arguments; the
link wraps it (--wrap=main, tools/nspire_build.py) so that Ndless's start-up
code calls this one first. It remembers the folder the program was started
from, which holds the converted map and everything the game writes
(port/nspire/README.md), sends the log (stderr: platform_log, and the
game's own errors) to halo_log.txt.tns there, starts the clock and the
memory window, and puts the timer and the page tables back at exit.
*/

#include "platform.h"
#include "nspire.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define LOG_NAME "halo_log.txt.tns"

int __real_main(void);

/* the game's stack (nspire_switch.S): Halo's AI functions keep 80-170 KB
arrays on the stack, and one calls another */
#define GAME_STACK_SIZE (512UL * 1024UL)
#define GAME_STACK_FILL 0xA5A5A5A5UL
int nspire_call_on_stack(int (*function)(void), void *stack_top);
static unsigned long *game_stack;
/* the start of the program (Ndless's linker script puts it at 0) */
extern char _start[];

/* libndls (its header's BOOL is not the Xbox SDK's) */
unsigned _show_msgbox(const char *title, const char *message, unsigned button_count, ...);
unsigned hwtype(void);
/* (on the OS's screen: the game's video mode let go first) */
#define show_msgbox(title, message) (nspire_video_release(), _show_msgbox(title, message, 0))

static char program_directory[256] = "/documents/ndless";
static char log_path[300];

const char *nspire_program_directory(void)
{
	return program_directory;
}

/* the level to play: named by the program's own name, halo_<level>.tns
(halo_a10.tns plays a10; halo_demo.tns and q_demo.tns play the demo:
nspire_demo.c); b30 for any other name */
static char level_name[32] = "b30";

const char *nspire_level_name(void)
{
	return level_name;
}

static void level_from_program_name(const char *path)
{
	const char *name = strrchr(path, '/');
	unsigned long length = 0;

	name = name ? name + 1 : path;
	/* (q_demo.tns too: the only name under q, which the computer selects in
	the file browser by typing q to launch it: tools/nspire_bench.py) */
	if (!strncasecmp(name, "q_demo", 6))
		name = "halo_demo";
	if (strncasecmp(name, "halo_", 5))
		return;
	name += 5;
	while (name[length] && name[length] != '.' && length < sizeof(level_name) - 1)
		length++;
	if (!length)
		return;
	memcpy(level_name, name, length);
	level_name[length] = 0;
	/* halo_demo.tns plays the demo recorded, in b30 (nspire_demo.c) */
	if (!strcasecmp(level_name, "demo"))
	{
		extern void nspire_demo_request_play(void);

		strcpy(level_name, "b30");
		nspire_demo_request_play();
	}
}

/* ---------- log */

/* The calculator's OS keeps a file's writes until it is closed: a reset
would lose the log's end, which is the part that matters. Closing the log
and opening it again for every line, though, cost tens of milliseconds a
frame (a report is sixty lines), so the lines gather in stderr's buffer and
the log is closed and opened again (its writes kept) at most every two
seconds, and at once for a fatal error and at exit: a reset loses two
seconds of it at most (platform_log's lines, which write to stderr
directly, go with these). */
#define LOG_KEEP_TICKS (2UL * 32768UL)

static char log_buffer[16 * 1024];
static unsigned long long log_kept_at;

static void log_reopen(void)
{
	if (log_path[0] && freopen(log_path, "a", stderr))
		setvbuf(stderr, log_buffer, _IOFBF, sizeof(log_buffer));
}

/* the log's lines so far into the file, for good */
void nspire_log_keep(void)
{
	fflush(stderr);
	log_reopen();
	log_kept_at = nspire_ticks();
}

void nspire_log(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);
	if (nspire_ticks() - log_kept_at >= LOG_KEEP_TICKS)
		nspire_log_keep();
}

void nspire_fatal(const char *format, ...)
{
	char message[512];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	nspire_log("fatal: %s", message);
	nspire_log_keep();
	show_msgbox("Halo", message);
	exit(EXIT_FAILURE);
}

void platform_show_message(char const *title, char const *message)
{
	nspire_log("%s: %s", title, message);
	nspire_log_keep();
	show_msgbox(title, message);
	/* (the game goes on: its screen back) */
	nspire_video_initialize();
}

/* the data comes converted from a computer (tools/nspire_map.py); there is
no disc image to offer on the calculator */
BOOL platform_offer_game_data(const char *destination)
{
	(void)destination;
	return FALSE;
}

/* ---------- crashes (nspire_abort.S) */

/* r0-r12, sp, lr, pc, cpsr, fault address, fault status, kind */
extern unsigned long nspire_crash[20];

void nspire_crash_report(void)
{
	static const char *const kinds[] = { "?", "data abort", "prefetch abort", "undefined instruction" };
	unsigned long *c = nspire_crash;
	unsigned long base = (unsigned long)&_start;
	unsigned long kind = c[19] < 4 ? c[19] : 0;
	char message[256];

	/* the exception left interrupts off; files need them */
	__asm__ volatile("mrs r0, cpsr\n bic r0, r0, #0xC0\n msr cpsr_c, r0" ::: "r0");
	nspire_log("CRASH: %s at pc %08lx (halo.elf %08lx), address %08lx, status %03lx", kinds[kind], c[15],
		c[15] - base, c[17], c[18] & 0xFFF);
	nspire_log("  r0 %08lx r1 %08lx r2 %08lx r3 %08lx r4 %08lx r5 %08lx r6 %08lx", c[0], c[1], c[2], c[3], c[4], c[5], c[6]);
	nspire_log("  r7 %08lx r8 %08lx r9 %08lx r10 %08lx r11 %08lx r12 %08lx", c[7], c[8], c[9], c[10], c[11], c[12]);
	nspire_log("  sp %08lx lr %08lx (halo.elf %08lx) cpsr %08lx", c[13], c[14], c[14] - base, c[16]);
	snprintf(message, sizeof(message), "Halo crashed (%s at %08lx, address %08lx). The details are in halo_log.txt.tns.",
		kinds[kind], c[15] - base, c[17]);
	nspire_log_keep();
	show_msgbox("Halo", message);
	exit(EXIT_FAILURE);
}

/* ---------- the overclock

NoverII (a CX II overclocking program) keeps the clock it set up in
/documents/ndless/noverII.cfg.tns and sets it again only when it runs. It
writes the clock registers only where the values it reads differ from its
own, and never the one at 0x90140810 (a precedence slip in its setConfig):
after another program has left the clock slow with the registers still as
NoverII left them, running it again changes nothing until a reset. So Halo
sets NoverII's saved clock itself as it starts: every register, whatever
they read, with interrupts off, as NoverII does. The registers go to the
log before and after, beside the speed the renderer then measures. */

/* the CX II's clock: the PMU's registers, and the SDRAM controller's (an
FTDDR3030), logged to set a run at NoverII's overclock against one where the
clock has fallen back. Only read: setting NoverII's saved clock from here froze
the calculator once the OS had changed the clock itself (it does for USB). */
#define PMU ((volatile unsigned long *)0x90140000)
#define SDRAM_CONTROL ((volatile unsigned long *)0x90120000)

int nspire_is_cx2(void);

static void nspire_clock_log(void)
{
	int index;

	nspire_log("clock registers: %08lx %08lx %08lx (12 MHz x %lu / %lu)",
		PMU[0x30 / 4], PMU[0x20 / 4], PMU[0x810 / 4], (PMU[0x30 / 4] >> 24) & 0x3F, (PMU[0x30 / 4] >> 16) & 0x1F);
	for (index = 0; index < 64; index += 8)
		nspire_log("sdram %02x: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx", index * 4,
			SDRAM_CONTROL[index], SDRAM_CONTROL[index + 1], SDRAM_CONTROL[index + 2], SDRAM_CONTROL[index + 3],
			SDRAM_CONTROL[index + 4], SDRAM_CONTROL[index + 5], SDRAM_CONTROL[index + 6], SDRAM_CONTROL[index + 7]);
	nspire_log_keep();
}

/* whether ui.map.tns is next to the program: the game then starts at its
main menu (source/main/main.c; the game's own fopen goes through the data
root, so the port looks) */
int nspire_main_menu_present(void)
{
	char path[300];
	FILE *file;
	extern int nspire_demo_playing(void);

	/* (a demo goes straight to its level) */
	if (nspire_demo_playing())
		return 0;
	snprintf(path, sizeof(path), "%s/ui.map.tns", program_directory);
	file = fopen(path, "rb");
	nspire_log("main menu: %s %s", path, file ? "found: the game starts there" : "not found: the level at once");
	if (!file)
		return 0;
	fclose(file);
	return 1;
}

/* ---------- start and exit */

static void nspire_dispose(void)
{
	struct nspire_paging_statistics paging;
	unsigned long blocks, bytes;

	if (game_stack)
	{
		/* how deep it went: the fill left untouched at the bottom */
		unsigned long untouched = 0;

		while (untouched < GAME_STACK_SIZE / sizeof(unsigned long) && game_stack[untouched] == GAME_STACK_FILL)
			untouched++;
		nspire_log("game stack: %lu KB used of %lu", (GAME_STACK_SIZE - untouched * sizeof(unsigned long)) / 1024,
			GAME_STACK_SIZE / 1024);
	}

	nspire_paging_get_statistics(&paging);
	nspire_heap_statistics(&blocks, &bytes);
	nspire_log("exiting: %lu KB of the memory window in use; %lu KB left allocated in %lu blocks; "
		"paging: %lu faults, %lu reads, %lu evictions, %lu pinned blocks of %lu",
		nspire_memory_window_size() / 1024, bytes / 1024, blocks, paging.faults, paging.reads, paging.evictions,
		paging.pinned_blocks, paging.slots);
	/* the abort handler goes before the tables it maps into */
	nspire_paging_dispose();
	nspire_memory_dispose();
	/* last: the log is one of them */
	nspire_close_all_files();
	nspire_time_dispose();
}

/* While Halo runs, the OS's USB task does not, and the computer gives up on
the calculator; it stays given up after Halo quits, until the cable is
plugged in again. So at the very end the USB controller (a ChipIdea, its
USBCMD at 0xB0000140) leaves the bus for a moment, its run bit off, and
comes back: to the computer, the cable plugged in again, and it can launch
the next run and fetch the log by itself (tools/nspire_bench.py). */
#define USB_COMMAND ((volatile unsigned long *)0xB0000140)

static void nspire_usb_reattach(void)
{
	volatile unsigned long wait;

	/* (counted, not timed: the timer is the OS's again by now) */
	*USB_COMMAND &= ~1UL;
	for (wait = 0; wait < 120000000UL; wait++)
		;
	*USB_COMMAND |= 1UL;
}

int __wrap_main(int argc, char **argv)
{
	char *slash;

	if (argc > 0 && argv[0])
	{
		level_from_program_name(argv[0]);
		strncpy(program_directory, argv[0], sizeof(program_directory) - 1);
		program_directory[sizeof(program_directory) - 1] = 0;
		slash = strrchr(program_directory, '/');
		if (slash && slash != program_directory)
			*slash = 0;
	}
	/* (first, so that it runs last at exit) */
	atexit(nspire_usb_reattach);
	snprintf(log_path, sizeof(log_path), "%s/%s", program_directory, LOG_NAME);
	if (!freopen(log_path, "w", stderr))
		show_msgbox("Halo", "cannot write the log next to the program");
	setvbuf(stderr, log_buffer, _IOFBF, sizeof(log_buffer));
	/* (and what is left of it kept at the end) */
	atexit(nspire_log_keep);

	/* first, so that freeing what is left runs after everything else at exit */
	nspire_heap_initialize();
	nspire_log("Halo for the TI-Nspire, running from %s (hwtype %u), playing %s", program_directory, hwtype(), level_name);
	if (hwtype() < 1)
		nspire_fatal("This port needs a TI-Nspire CX or CX II.");
	/* (the CX II's clock registers: NoverII is for it alone) */
	if (hwtype() == 1 && nspire_is_cx2())
		nspire_clock_log();
	nspire_time_initialize();
	if (!nspire_memory_initialize())
		nspire_fatal("The memory at 0x80000000 is already in use.");
	atexit(nspire_dispose);
	/* the load address: a crash's pc less this is its address in halo.elf */
	nspire_log("loaded at %08lx", (unsigned long)&_start);
	if (!nspire_paging_hook())
		nspire_fatal("This OS version's exception vectors are not supported (see halo_log.txt.tns).");
	nspire_log("heap free at start: %lu KB", nspire_memory_heap_free() / 1024);

	game_stack = malloc(GAME_STACK_SIZE);
	if (!game_stack)
		nspire_fatal("no memory for the game's stack");
	{
		unsigned long index;

		for (index = 0; index < GAME_STACK_SIZE / sizeof(unsigned long); index++)
			game_stack[index] = GAME_STACK_FILL;
	}
	return nspire_call_on_stack(__real_main, (unsigned char *)game_stack + GAME_STACK_SIZE);
}

/* ---------- the Linux port's hooks the game calls */

/* the software renderer draws a frame per game tick */
int halo_interpolation_enabled(void)
{
	return 0;
}

/* no mouse on the calculator (its touchpad is the right stick) */
int halo_ui_pointer_update(int menus_active, struct halo_ui_pointer *pointer)
{
	(void)menus_active;
	(void)pointer;
	return 0;
}

/* automated network tests (port/linux/game/network_test.c) */
void test_input_hold_action(int hold)
{
	(void)hold;
}

/* an MSVC intrinsic (source/scenario/scenario.c): a compiler barrier */
void _ReadWriteBarrier(void)
{
	__asm__ volatile("" ::: "memory");
}
