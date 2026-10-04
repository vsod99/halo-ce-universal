/*
NSPIRE_TIME.C

The clocks of the Nspire port. The monotonic clock is the second timer of
the SP804 at 0x900D0000, free running down from 0xFFFFFFFF at 32768 Hz; the
OS uses the first one, and msleep() in libndls reprograms it, so the second
is the one nothing else touches (port/nspire/probe measured it). Wall-clock
seconds come from the real-time clock at 0x90090000, as libsyscalls'
gettimeofday reads it.

The timer's registers are put back when the program exits.
*/

#include "nspire.h"

#include <errno.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define TIMER_FREQUENCY 32768ULL

#define TIMER_LOAD (*(volatile unsigned long *)0x900D0020)
#define TIMER_VALUE (*(volatile unsigned long *)0x900D0024)
#define TIMER_CONTROL (*(volatile unsigned long *)0x900D0028)
#define REAL_TIME_CLOCK (*(volatile unsigned long *)0x90090000)

/* enabled, free running, no interrupt, no prescaler, 32-bit */
#define TIMER_FREE_RUNNING 0x82

static unsigned long saved_load, saved_control;
static int initialized;
static unsigned long last_value;
static unsigned long long elapsed_ticks;
/* the real-time clock and the tick count at start-up */
static unsigned long start_seconds;

void nspire_time_initialize(void)
{
	if (initialized)
		return;
	saved_load = TIMER_LOAD;
	saved_control = TIMER_CONTROL;
	TIMER_CONTROL = 0;
	TIMER_LOAD = 0xFFFFFFFFUL;
	{
		/* the value register reads the old count until the next timer
		clock (about 30 microseconds) */
		unsigned long stale = TIMER_VALUE;

		TIMER_CONTROL = TIMER_FREE_RUNNING;
		while (TIMER_VALUE == stale)
			;
	}
	last_value = TIMER_VALUE;
	elapsed_ticks = 0;
	start_seconds = REAL_TIME_CLOCK;
	initialized = 1;
}

void nspire_time_dispose(void)
{
	if (!initialized)
		return;
	TIMER_CONTROL = 0;
	TIMER_LOAD = saved_load;
	TIMER_CONTROL = saved_control;
	initialized = 0;
}

unsigned long long nspire_ticks(void)
{
	unsigned long value;

	if (!initialized)
		nspire_time_initialize();
	value = TIMER_VALUE;
	/* counting down, and wrapping every 36 hours, which the unsigned
	difference handles */
	elapsed_ticks += (unsigned long)(last_value - value);
	last_value = value;
	return elapsed_ticks;
}

int clock_gettime(clockid_t clock, struct timespec *time)
{
	unsigned long long ticks = nspire_ticks();
	unsigned long long seconds = ticks / TIMER_FREQUENCY;
	unsigned long remainder = (unsigned long)(ticks % TIMER_FREQUENCY);

	time->tv_nsec = (long)((remainder * 1000000000ULL) / TIMER_FREQUENCY);
	time->tv_sec = (time_t)seconds;
	if (clock == CLOCK_REALTIME)
		time->tv_sec += start_seconds;
	return 0;
}

int nanosleep(const struct timespec *duration, struct timespec *remaining)
{
	unsigned long long end = nspire_ticks() +
		(unsigned long long)duration->tv_sec * TIMER_FREQUENCY +
		((unsigned long long)duration->tv_nsec * TIMER_FREQUENCY) / 1000000000ULL;

	while (nspire_ticks() < end)
		nspire_yield();
	if (remaining)
		remaining->tv_sec = remaining->tv_nsec = 0;
	return 0;
}

int pause(void)
{
	for (;;)
		nspire_yield();
}
