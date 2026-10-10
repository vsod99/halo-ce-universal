/*
NXDK_NV2A.C

The Xbox's GPU through nxdk's pbkit, for the Direct3D device
(d3d8_nv2a.c), which writes the push buffer's methods itself: the video
mode, the push buffer, the screen's three buffers and their flips at the
vertical blank.

pbkit starts with the constructors, before the game takes its memory
(cache/physical_memory_map.c): its push buffer and frame buffers must be
contiguous, from the low 64 MB that the game state's virtual memory would
otherwise use up (port/xbox/README.md).

pbkit's own context DMAs for the color and depth buffers start at its
buffers. The device draws into the game's surfaces wherever they are, so
it binds two more over all of the low 64 MB (XBOX_GPU_DMA_COLOR and
_ZETA), as pbkit's texture and vertex ones already are. A third
(XBOX_GPU_DMA_REPORT) is the visibility tests' reports, with the fence
after them.
*/

#include <hal/video.h>
#include <pbkit/pbkit.h>
#include <xboxkrnl/xboxkrnl.h>
#include <windows.h>

#include "nxdk_platform.h"

/* (a power of two; a frame of the menus or a map is far less) */
#define PUSH_BUFFER_BYTES (2UL * 1024 * 1024)
/* left free at the end for pbkit's jump back to the head */
#define PUSH_BUFFER_MARGIN 64
/* the GPU is told of new methods once this many bytes are written */
#define KICK_BYTES (16UL * 1024)

static BOOL gpu_ready;
/* the console's video setting (the dashboard's), as the mode was set */
static DWORD video_settings;
static uint32_t *push_buffer_head;
static struct s_CtxDma color_dma, zeta_dma, report_dma;
static volatile unsigned long *reports;

__attribute__((constructor)) static void gpu_start(void)
{
	int status;

	/* (480p, rather than 480i, where the console's setting allows it and
	the cable carries it: nxdk's choice) */
	if (!XVideoSetMode(640, 480, 32, REFRESH_DEFAULT))
	{
		platform_log("GPU: cannot set the 640x480 video mode");
		return;
	}
	video_settings = XVideoGetEncoderSettings();
	pb_size(PUSH_BUFFER_BYTES);
	status = pb_init();
	if (status)
	{
		platform_log("GPU: pbkit cannot start (%d)", status);
		return;
	}
	pb_show_front_screen();
	pb_create_dma_ctx(XBOX_GPU_DMA_COLOR, DMA_CLASS_3D, 0, MAXRAM, &color_dma);
	pb_create_dma_ctx(XBOX_GPU_DMA_ZETA, DMA_CLASS_3D, 0, MAXRAM, &zeta_dma);
	pb_bind_channel(&color_dma);
	pb_bind_channel(&zeta_dma);
	/* uncached: the CPU reads what the GPU writes */
	reports = MmAllocateContiguousMemoryEx(XBOX_GPU_FENCE_OFFSET + 16, 0, 0x03ffffff, 4096,
		PAGE_READWRITE | PAGE_NOCACHE);
	if (reports)
	{
		reports[XBOX_GPU_FENCE_OFFSET / 4] = 0;
		pb_create_dma_ctx(XBOX_GPU_DMA_REPORT, DMA_CLASS_3D, (DWORD)reports, XBOX_GPU_FENCE_OFFSET + 16 - 1,
			&report_dma);
		pb_bind_channel(&report_dma);
	}
	/* the push buffer's head, where pbkit jumps back to */
	pb_reset();
	push_buffer_head = pb_begin();
	gpu_ready = TRUE;
	platform_log("GPU: pbkit started, %lux%lu, %lu KB push buffer; the console's video setting %s%s",
		(unsigned long)pb_back_buffer_width(), (unsigned long)pb_back_buffer_height(), PUSH_BUFFER_BYTES / 1024,
		video_settings & VIDEO_WIDESCREEN ? "16:9" : video_settings & VIDEO_LETTERBOX ? "letterbox" : "4:3",
		video_settings & VIDEO_MODE_480P ? ", 480p allowed" : "");
}

int xbox_video_widescreen(void)
{
	return (video_settings & VIDEO_WIDESCREEN) != 0;
}

int xbox_gpu_ready(void)
{
	return gpu_ready;
}

void xbox_gpu_screen(unsigned long *width, unsigned long *height, unsigned long *pitch)
{
	*width = pb_back_buffer_width();
	*height = pb_back_buffer_height();
	*pitch = pb_back_buffer_pitch();
}

volatile unsigned long *xbox_gpu_reports(void)
{
	return reports;
}

unsigned long xbox_gpu_back_buffer(void)
{
	return (unsigned long)pb_back_buffer() & 0x03ffffff;
}

/* the time the processor spent waiting on the GPU (time stamp counter
cycles), which the frame rate's log line gives as a share */
static unsigned long long waited;

static unsigned long long wait_start(void)
{
	return __builtin_ia32_rdtsc();
}

static void wait_end(unsigned long long start)
{
	waited += __builtin_ia32_rdtsc() - start;
}

unsigned long xbox_gpu_waited_percent(void)
{
	static unsigned long long last_time, last_waited;
	unsigned long long now = __builtin_ia32_rdtsc();
	unsigned long percent = 0;

	if (last_time && now != last_time)
		percent = (unsigned long)((waited - last_waited) * 100 / (now - last_time));
	last_time = now;
	last_waited = waited;
	return percent;
}

/* where the GPU reads the push buffer next (physical); pbkit's channel 0 */
static uint32_t gpu_get(void)
{
	return *(volatile uint32_t *)(VIDEO_BASE + 0x00800044) & 0x03ffffff;
}

/* the end of what the device wrote, how far it may write without a look at
the GPU (xbox_gpu_begin's test, inline in the device), and where the GPU is
told of it next: as the SDK's Direct3D, the GPU hears of new methods now
and then (each telling is a cache flush and register accesses), not at each
block */
struct xbox_gpu_push xbox_gpu_push;

static uint32_t *kicked_end;

/* the GPU told of all written, before a wait on it or pbkit's own methods */
void xbox_gpu_kick(void)
{
	uint32_t *write_end = (uint32_t *)xbox_gpu_push.write_end;

	if (write_end != kicked_end)
	{
		pb_end(write_end);
		kicked_end = write_end;
	}
	xbox_gpu_push.kick_end = (unsigned long *)(kicked_end + KICK_BYTES / 4);
}

/* a new place to write from (pbkit's, after a flip): nothing known free */
static void push_restart(uint32_t *p)
{
	xbox_gpu_push.write_end = (unsigned long *)p;
	xbox_gpu_push.room_end = NULL;
	kicked_end = p;
	xbox_gpu_push.kick_end = (unsigned long *)(p + KICK_BYTES / 4);
}

/* until the GPU has read what lies from p to p+dwords (and the margin
after it, for pbkit's own methods at the flips). Behind p it reads this
lap, ahead of p what is left of the last one. The room it leaves is free
until the device writes past it: the GPU's read position only moves on (a
register read is costly, in xemu above all) */
static void gpu_wait_room(uint32_t *p, unsigned long dwords)
{
	uint32_t *tail = push_buffer_head + PUSH_BUFFER_BYTES / 4 - PUSH_BUFFER_MARGIN - 1;
	uint32_t start = (uint32_t)p & 0x03ffffff;
	uint32_t end = start + (dwords + PUSH_BUFFER_MARGIN) * 4;
	uint32_t get;
	unsigned long long started;

	started = wait_start();
	while ((get = gpu_get()) > start && get < end)
		;
	wait_end(started);
	/* behind p, it reads this lap: free to the buffer's end */
	xbox_gpu_push.room_end = (unsigned long *)(get > start ? p + (get - start) / 4 - PUSH_BUFFER_MARGIN : tail);
	if (xbox_gpu_push.room_end > (unsigned long *)tail)
		xbox_gpu_push.room_end = (unsigned long *)tail;
}

unsigned long *xbox_gpu_begin_room(unsigned long dwords)
{
	uint32_t *p;

	if (!xbox_gpu_push.write_end)
		push_restart(pb_begin());
	p = (uint32_t *)xbox_gpu_push.write_end;
	/* past the end: a jump back to the head, as pb_reset writes, without
	its wait for the GPU to read up to it. It reads this lap here (at or
	behind p), so the head is free once it is past the room asked for */
	if (p + dwords + PUSH_BUFFER_MARGIN >= push_buffer_head + PUSH_BUFFER_BYTES / 4)
	{
		uint32_t head = (uint32_t)push_buffer_head & 0x03ffffff;
		uint32_t room_end = head + (dwords + PUSH_BUFFER_MARGIN) * 4;
		uint32_t get;
		unsigned long long started;

		xbox_gpu_kick();
		started = wait_start();
		while ((get = gpu_get()) >= head && get < room_end)
			;
		wait_end(started);
		*p = head | 1;
		pb_end(push_buffer_head);
		p = push_buffer_head;
		push_restart(p);
	}
	gpu_wait_room(p, dwords);
	return (unsigned long *)p;
}

int xbox_gpu_busy(void)
{
	xbox_gpu_kick();
	return pb_busy();
}

void xbox_gpu_wait_idle(void)
{
	unsigned long long started;

	xbox_gpu_kick();
	started = wait_start();
	while (pb_busy())
		;
	wait_end(started);
}

unsigned long xbox_gpu_fence(void)
{
	return reports ? reports[XBOX_GPU_FENCE_OFFSET / 4] : 0;
}

void xbox_gpu_wait_fence(unsigned long fence)
{
	unsigned long long started;

	if ((long)(xbox_gpu_fence() - fence) >= 0)
		return;
	xbox_gpu_kick();
	started = wait_start();
	while ((long)(xbox_gpu_fence() - fence) < 0)
		NtYieldExecution();
	wait_end(started);
}

void xbox_gpu_present(void)
{
	/* the back buffer is shown at the next vertical blank; pbkit then
	draws into the next of its three, its methods after the device's */
	unsigned long long started;

	xbox_gpu_kick();
	started = wait_start();
	while (pb_finished())
		NtYieldExecution();
	wait_end(started);
	push_restart(pb_begin());
}

unsigned long xbox_gpu_wait_vertical_blank(void)
{
	return pb_wait_for_vbl();
}

unsigned long xbox_gpu_vertical_blank_count(void)
{
	return pb_get_vbl_counter();
}
