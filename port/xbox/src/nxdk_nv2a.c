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

static BOOL gpu_ready;
static uint32_t *push_buffer_head;
static struct s_CtxDma color_dma, zeta_dma, report_dma;
static volatile unsigned long *reports;

__attribute__((constructor)) static void gpu_start(void)
{
	int status;

	if (!XVideoSetMode(640, 480, 32, REFRESH_DEFAULT))
	{
		platform_log("GPU: cannot set the 640x480 video mode");
		return;
	}
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
	platform_log("GPU: pbkit started, %lux%lu, %lu KB push buffer", (unsigned long)pb_back_buffer_width(),
		(unsigned long)pb_back_buffer_height(), PUSH_BUFFER_BYTES / 1024);
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

/* where the GPU reads the push buffer next (physical); pbkit's channel 0 */
static uint32_t gpu_get(void)
{
	return *(volatile uint32_t *)(VIDEO_BASE + 0x00800044) & 0x03ffffff;
}

/* until the GPU has read what lies from p to p+dwords (and the margin
after it, for pbkit's own methods at the flips). Behind p it reads this
lap, ahead of p what is left of the last one */
static void gpu_wait_room(const uint32_t *p, unsigned long dwords)
{
	uint32_t start = (uint32_t)p & 0x03ffffff;
	uint32_t end = start + (dwords + PUSH_BUFFER_MARGIN) * 4;
	uint32_t get;

	while ((get = gpu_get()) > start && get < end)
		;
}

unsigned long *xbox_gpu_begin(unsigned long dwords)
{
	uint32_t *p = pb_begin();

	/* past the end: a jump back to the head, as pb_reset writes, without
	its wait for the GPU to read up to it. It reads this lap here (at or
	behind p), so the head is free once it is past the room asked for */
	if (p + dwords + PUSH_BUFFER_MARGIN >= push_buffer_head + PUSH_BUFFER_BYTES / 4)
	{
		uint32_t head = (uint32_t)push_buffer_head & 0x03ffffff;
		uint32_t room_end = head + (dwords + PUSH_BUFFER_MARGIN) * 4;
		uint32_t get;

		while ((get = gpu_get()) >= head && get < room_end)
			;
		*p = head | 1;
		pb_end(push_buffer_head);
		p = pb_begin();
	}
	gpu_wait_room(p, dwords);
	return (unsigned long *)p;
}

void xbox_gpu_end(unsigned long *end)
{
	pb_end((uint32_t *)end);
}

int xbox_gpu_busy(void)
{
	return pb_busy();
}

void xbox_gpu_wait_idle(void)
{
	while (pb_busy())
		;
}

unsigned long xbox_gpu_fence(void)
{
	return reports ? reports[XBOX_GPU_FENCE_OFFSET / 4] : 0;
}

void xbox_gpu_wait_fence(unsigned long fence)
{
	while ((long)(xbox_gpu_fence() - fence) < 0)
		NtYieldExecution();
}

void xbox_gpu_present(void)
{
	/* the back buffer is shown at the next vertical blank; pbkit then
	draws into the next of its three */
	while (pb_finished())
		NtYieldExecution();
}

unsigned long xbox_gpu_wait_vertical_blank(void)
{
	return pb_wait_for_vbl();
}

unsigned long xbox_gpu_vertical_blank_count(void)
{
	return pb_get_vbl_counter();
}
