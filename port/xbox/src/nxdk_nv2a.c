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
_ZETA), as pbkit's texture and vertex ones already are.
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
static struct s_CtxDma color_dma, zeta_dma;

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

unsigned long xbox_gpu_back_buffer(void)
{
	return (unsigned long)pb_back_buffer() & 0x03ffffff;
}

unsigned long *xbox_gpu_begin(unsigned long dwords)
{
	uint32_t *p = pb_begin();

	/* past the end: back to the head, once the GPU has read up to here */
	if (p + dwords + PUSH_BUFFER_MARGIN >= push_buffer_head + PUSH_BUFFER_BYTES / 4)
	{
		pb_reset();
		p = pb_begin();
	}
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
