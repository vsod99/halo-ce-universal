/*
NXDK_AUDIO.C

The Xbox's sound output for the DirectSound mixer (port/linux/src/
dsound_sdl.c): 16-bit stereo at 48 kHz through the MCPX's AC'97 controller,
the analog and digital outputs together. The APU is left alone for now.

The controller plays a ring of 32 buffer descriptors. A thread above the
game's priority keeps a few of them (CHUNKS_AHEAD, about 64 ms; xemu let
43 ms run dry now and then) filled ahead of the one being played: it reads
the controller's current descriptor every few milliseconds and mixes the
chunks it has played into their place.
nxdk's driver (hal/audio.h) resets the controller and fills the descriptors;
its own completion callback is not used, since it runs as a DPC, where the
mixer could neither wait for its lock nor use the FPU.

The ring is contiguous memory set aside with the constructors, before the
game takes the low 64 MB (port/xbox/README.md); the controller starts only
when DirectSound is first created. Every 30 s the log has the share of the
processor the mixing took and the times the controller ran dry.
*/

#include <hal/audio.h>
#include <xboxkrnl/xboxkrnl.h>
#include <windows.h>
#include <string.h>

#include "nxdk_platform.h"

#define CHUNK_FRAMES 512
#define CHUNK_BYTES (CHUNK_FRAMES * 2 * sizeof(short))
#define CHUNKS 32
#define CHUNKS_AHEAD 6
/* the controller's analog output: the current descriptor (CIV) */
#define AC97_PCM_OUT_CURRENT ((volatile unsigned char *)0xfec00114)
/* its status, whose bit 0 says it has played every chunk it was given and
stopped (read only: the driver's interrupt clears the others) */
#define AC97_PCM_OUT_STATUS ((volatile unsigned short *)0xfec00116)
#define AC97_STATUS_HALTED 1
#define REPORT_SECONDS 30

static short *ring;
static xbox_audio_mix_function mix_function;
static BOOL audio_started;

__attribute__((constructor)) static void audio_reserve(void)
{
	ring = MmAllocateContiguousMemoryEx(CHUNKS * CHUNK_BYTES, 0, 0x03ffffff, 4096,
		PAGE_READWRITE | PAGE_WRITECOMBINE);
	if (ring)
		memset(ring, 0, CHUNKS * CHUNK_BYTES);
	else
		platform_log("sound: no contiguous memory for the AC'97 buffers");
}

static void chunk_fill(short *chunk)
{
	float mixed[CHUNK_FRAMES * 2];
	unsigned long sample;

	mix_function(mixed, CHUNK_FRAMES);
	for (sample = 0; sample < CHUNK_FRAMES * 2; sample++)
	{
		float value = mixed[sample] * 32767.0f;

		if (value > 32767.0f)
			value = 32767.0f;
		else if (value < -32768.0f)
			value = -32768.0f;
		chunk[sample] = (short)value;
	}
}

static DWORD WINAPI audio_thread(LPVOID parameter)
{
	/* the descriptor the next chunk goes in: nxdk's driver fills them in
	turn from 0 */
	unsigned long next = 0;
	ULONGLONG frequency = KeQueryPerformanceFrequency();
	ULONGLONG report_start = KeQueryPerformanceCounter();
	ULONGLONG mixing = 0, mixing_peak = 0;
	/* the times the controller was found stopped, out of chunks */
	unsigned long ran_dry = 0;

	(void)parameter;
	XAudioInit(16, 2, NULL, NULL);
	for (;;)
	{
		/* the descriptors given to the controller and not yet played out,
		counting the one it is playing */
		unsigned long current = *AC97_PCM_OUT_CURRENT & (CHUNKS - 1);
		unsigned long queued = (next - current) & (CHUNKS - 1);
		LARGE_INTEGER interval;
		ULONGLONG now;

		if (audio_started && (*AC97_PCM_OUT_STATUS & AC97_STATUS_HALTED))
			ran_dry++;
		while (queued < CHUNKS_AHEAD)
		{
			short *chunk = ring + next * CHUNK_FRAMES * 2;
			ULONGLONG start = KeQueryPerformanceCounter(), took;

			chunk_fill(chunk);
			took = KeQueryPerformanceCounter() - start;
			mixing += took;
			if (took > mixing_peak)
				mixing_peak = took;
			XAudioProvideSamples((unsigned char *)chunk, CHUNK_BYTES, 0);
			next = (next + 1) & (CHUNKS - 1);
			queued++;
		}
		if (!audio_started)
		{
			audio_started = TRUE;
			XAudioPlay();
		}
		now = KeQueryPerformanceCounter();
		if (now - report_start >= REPORT_SECONDS * frequency)
		{
			/* (a chunk is 10.7 ms of sound) */
			platform_log("sound: mixing took %lu.%lu%% of the processor, at most %lu us a chunk; "
				"ran dry %lu times",
				(unsigned long)(mixing * 1000 / (now - report_start) / 10),
				(unsigned long)(mixing * 1000 / (now - report_start) % 10),
				(unsigned long)(mixing_peak * 1000000 / frequency), ran_dry);
			report_start = now;
			mixing = mixing_peak = 0;
			ran_dry = 0;
		}
		/* a chunk lasts 10.7 ms */
		interval.QuadPart = -4 * 10000LL;
		KeDelayExecutionThread(KernelMode, FALSE, &interval);
	}
	return 0;
}

int xbox_audio_start(xbox_audio_mix_function mix)
{
	HANDLE thread;

	if (!ring)
		return 0;
	mix_function = mix;
	thread = CreateThread(NULL, 64 * 1024, audio_thread, NULL, 0, NULL);
	if (!thread)
	{
		platform_log("sound: cannot start the mixer's thread");
		return 0;
	}
	SetThreadPriority(thread, THREAD_PRIORITY_HIGHEST);
	CloseHandle(thread);
	platform_log("sound: AC'97 output, %d Hz stereo, %d ms ahead", 48000, CHUNKS_AHEAD * CHUNK_FRAMES * 1000 / 48000);
	return 1;
}
