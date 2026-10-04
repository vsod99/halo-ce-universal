/*
SOFT_CAPTURE.C

One frame of the software renderer written to halo_frame.tns (var asks for
it; xinput_nspire.c): every clear and draw with the state it drew with, the
vertex and index data it read and the textures it sampled. A replay of the
file (tools/nspire_replay) draws the frame again through the same
renderer on a computer, to measure and check the renderer away from the
calculator.

The file is a series of records, each a tag, a length and that many bytes,
in the calculator's byte order (soft_capture.h has the tags).
*/

#include "nspire.h"
#include "soft_rasterizer.h"
#include "soft_capture.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CAPTURE_FILE "halo_frame.tns"
#define MAXIMUM_RANGES 2048
#define MAXIMUM_IDS 512

typedef char verify_render_state_count[D3DRS_MAX == 144 ? 1 : -1];
typedef char verify_texture_state_count[D3DTSS_MAX == 32 && D3DTSS_MAXSTAGES == 4 ? 1 : -1];

static struct
{
	/* the file, written a buffer at a time with write() (as the game's
	other large files are: stdio hung) */
	int file;
	BOOL open;
	unsigned long buffered;
	unsigned char buffer[32768];
	unsigned long records, logged;
	BOOL requested;
	unsigned long long started;
	unsigned long bytes;
	unsigned long ranges;
	unsigned long range_address[MAXIMUM_RANGES];
	unsigned long range_size[MAXIMUM_RANGES];
	unsigned long ids;
	const void *id[MAXIMUM_IDS];
	unsigned long constants_serial;
	BOOL constants_written;
} capture;

/* bytes to the file through a buffer: what the game draws from may be in
the memory window, which only a fault in the game's own code pages in (the
OS's file writing would hang on one) */
static void flush_buffer(void)
{
	if (capture.buffered)
	{
		if (write(capture.file, capture.buffer, capture.buffered) != (long)capture.buffered)
			nspire_log("capture: a write failed");
		capture.buffered = 0;
	}
}

static void write_through(const void *data, unsigned long size)
{
	const unsigned char *source = data;

	while (size)
	{
		unsigned long chunk = sizeof(capture.buffer) - capture.buffered;

		if (chunk > size)
			chunk = size;
		memcpy(capture.buffer + capture.buffered, source, chunk);
		capture.buffered += chunk;
		source += chunk;
		size -= chunk;
		if (capture.buffered == sizeof(capture.buffer))
			flush_buffer();
	}
}

static void record(unsigned long tag, const void *first, unsigned long first_size, const void *second,
	unsigned long second_size)
{
	unsigned long header[2];

	header[0] = tag;
	header[1] = first_size + second_size;
	write_through(header, sizeof(header));
	if (first_size)
		write_through(first, first_size);
	if (second_size)
		write_through(second, second_size);
	if ((first_size + second_size) & 3)
	{
		static const unsigned char padding[3] = { 0, 0, 0 };

		write_through(padding, 4 - ((first_size + second_size) & 3));
	}
	capture.bytes += sizeof(header) + ((first_size + second_size + 3) & ~3UL);
	/* (progress, in case it stops) */
	if (++capture.records - capture.logged >= 64)
	{
		capture.logged = capture.records;
		nspire_log("capture: %lu records, %lu KB", capture.records, capture.bytes / 1024);
	}
}

/* the memory written so far, as sorted ranges that do not touch */
static BOOL range_insert(unsigned long start, unsigned long end)
{
	unsigned long index = 0, merge;

	while (index < capture.ranges && capture.range_address[index] + capture.range_size[index] < start)
		index++;
	/* overlapping or touching ranges from index on merge into one */
	merge = index;
	while (merge < capture.ranges && capture.range_address[merge] <= end)
	{
		unsigned long merge_end = capture.range_address[merge] + capture.range_size[merge];

		if (capture.range_address[merge] < start) start = capture.range_address[merge];
		if (merge_end > end) end = merge_end;
		merge++;
	}
	if (merge == index)
	{
		if (capture.ranges >= MAXIMUM_RANGES)
			return FALSE;
		memmove(&capture.range_address[index + 1], &capture.range_address[index],
			(capture.ranges - index) * sizeof(unsigned long));
		memmove(&capture.range_size[index + 1], &capture.range_size[index], (capture.ranges - index) * sizeof(unsigned long));
		capture.ranges++;
	}
	else if (merge > index + 1)
	{
		memmove(&capture.range_address[index + 1], &capture.range_address[merge],
			(capture.ranges - merge) * sizeof(unsigned long));
		memmove(&capture.range_size[index + 1], &capture.range_size[merge], (capture.ranges - merge) * sizeof(unsigned long));
		capture.ranges -= merge - index - 1;
	}
	capture.range_address[index] = start;
	capture.range_size[index] = end - start;
	return TRUE;
}

/* memory the frame reads, at its address in the Xbox's physical space:
only the parts not written already */
static void capture_memory(unsigned long physical, unsigned long size)
{
	unsigned long end = physical + size, at = physical, index;

	if (!physical || !size)
		return;
	for (index = 0; index < capture.ranges && at < end; index++)
	{
		unsigned long range_start = capture.range_address[index];
		unsigned long range_end = range_start + capture.range_size[index];

		if (range_end <= at)
			continue;
		if (range_start >= end)
			break;
		if (range_start > at)
		{
			unsigned long piece = at;

			record(SOFT_CAPTURE_MEMORY, &piece, sizeof(piece), PLATFORM_PHYSICAL_TO_VIRTUAL(at), range_start - at);
		}
		at = range_end;
	}
	if (at < end)
	{
		unsigned long piece = at;

		record(SOFT_CAPTURE_MEMORY, &piece, sizeof(piece), PLATFORM_PHYSICAL_TO_VIRTUAL(at), end - at);
	}
	if (!range_insert(physical, end))
		nspire_log("capture: too many memory ranges");
}

/* whether a program or shader was written already (and now it is) */
static BOOL capture_seen(const void *id)
{
	unsigned long index;

	for (index = 0; index < capture.ids; index++)
	{
		if (capture.id[index] == id)
			return TRUE;
	}
	if (capture.ids < MAXIMUM_IDS)
		capture.id[capture.ids++] = id;
	return FALSE;
}

BOOL soft_capture_active(void)
{
	return capture.open;
}

void soft_capture_request(void)
{
	capture.requested = TRUE;
}

/* at the end of a frame: the capture finished, or one begun */
void soft_capture_frame_end(void)
{
	if (capture.open)
	{
		record(SOFT_CAPTURE_END, NULL, 0, NULL, 0);
		flush_buffer();
		close(capture.file);
		capture.open = FALSE;
		nspire_log("captured a frame: %lu KB in %lu ms", capture.bytes / 1024,
			(unsigned long)((nspire_ticks() - capture.started) * 1000ULL / 32768ULL));
	}
	if (capture.requested)
	{
		char path[256];
		unsigned long magic = SOFT_CAPTURE_MAGIC;

		capture.requested = FALSE;
		snprintf(path, sizeof(path), "%s/%s", nspire_program_directory(), CAPTURE_FILE);
		capture.file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (capture.file < 0)
		{
			nspire_log("cannot write %s", path);
			return;
		}
		capture.open = TRUE;
		capture.buffered = 0;
		capture.records = capture.logged = 0;
		capture.started = nspire_ticks();
		capture.bytes = 0;
		capture.ranges = 0;
		capture.ids = 0;
		capture.constants_written = FALSE;
		write_through(&magic, sizeof(magic));
		nspire_log("capturing the next frame to %s", path);
	}
}

void soft_capture_screen(const unsigned short *pixels, unsigned long width, unsigned long height)
{
	unsigned long header[2];

	if (!capture.open || !pixels)
		return;
	header[0] = width;
	header[1] = height;
	record(SOFT_CAPTURE_SCREEN, header, sizeof(header), pixels, width * height * sizeof(unsigned short));
}

void soft_capture_clear(long left, long top, long right, long bottom, DWORD flags, D3DCOLOR color, float z,
	BOOL to_screen)
{
	unsigned long words[8];

	if (!capture.open)
		return;
	words[0] = (unsigned long)left;
	words[1] = (unsigned long)top;
	words[2] = (unsigned long)right;
	words[3] = (unsigned long)bottom;
	words[4] = flags;
	words[5] = color;
	memcpy(&words[6], &z, sizeof(z));
	words[7] = to_screen ? 1 : 0;
	record(SOFT_CAPTURE_CLEAR, words, sizeof(words), NULL, 0);
}

void soft_capture_resolve(void)
{
	if (capture.open)
		record(SOFT_CAPTURE_RESOLVE, NULL, 0, NULL, 0);
}

void soft_capture_draw(D3DPRIMITIVETYPE type, unsigned long vertex_count, const WORD *indices, unsigned long start_vertex,
	const float *immediate_vertices, BOOL to_screen)
{
	struct soft_vertex_shader *program = soft_current_program();
	struct soft_vertex_shader *shader = soft_device.vertex_shader;
	struct soft_capture_state state;
	unsigned long first, count, index, stage;
	unsigned long draw[5];

	if (!capture.open || !program || !shader)
		return;

	/* the program and the vertex layout, once each */
	if (!capture_seen(program))
	{
		unsigned long header[2];

		header[0] = (unsigned long)program;
		header[1] = program->instruction_count;
		record(SOFT_CAPTURE_PROGRAM, header, sizeof(header), program->instructions,
			program->instruction_count * 4 * sizeof(DWORD));
	}
	if (!capture_seen((const char *)shader + 1))
	{
		unsigned long header[2];

		header[0] = (unsigned long)shader;
		header[1] = shader->element_count;
		record(SOFT_CAPTURE_SHADER, header, sizeof(header), shader->elements,
			shader->element_count * sizeof(struct soft_vertex_element));
	}
	if (!capture.constants_written || capture.constants_serial != soft_device.constants_serial)
	{
		capture.constants_written = TRUE;
		capture.constants_serial = soft_device.constants_serial;
		record(SOFT_CAPTURE_CONSTANTS, soft_device.constants, sizeof(soft_device.constants), soft_device.attributes,
			sizeof(soft_device.attributes));
	}

	/* the vertex data the draw reads */
	if (indices)
	{
		unsigned long minimum = indices[0], maximum = indices[0];

		for (index = 1; index < vertex_count; index++)
		{
			if (indices[index] < minimum) minimum = indices[index];
			if (indices[index] > maximum) maximum = indices[index];
		}
		first = soft_device.base_vertex_index + minimum;
		count = maximum - minimum + 1;
	}
	else
	{
		first = start_vertex;
		count = vertex_count;
	}
	if (!immediate_vertices)
	{
		for (index = 0; index < shader->element_count; index++)
		{
			const struct soft_vertex_element *e = &shader->elements[index];
			DWORD data = soft_device.streams[e->stream].data;
			UINT stride = soft_device.streams[e->stream].stride;

			if (data)
				capture_memory(data + first * stride, count * stride);
		}
	}

	/* the textures */
	memset(&state, 0, sizeof(state));
	for (stage = 0; stage < 4; stage++)
	{
		const DWORD *resource = (const DWORD *)soft_device.textures[stage];

		if (resource && resource[1])
		{
			struct xgpu_texture_description description;
			unsigned long size;

			memcpy(state.textures[stage], resource, sizeof(state.textures[stage]));
			xgpu_texture_describe(resource[3], resource[4], &description);
			size = xgpu_texture_face_size(&description) * (description.cube_map ? 6 : 1);
			capture_memory(resource[1], size);
		}
	}

	memcpy(state.render_state, D3D__RenderState, sizeof(state.render_state));
	memcpy(state.texture_state, D3D__TextureState, sizeof(state.texture_state));
	for (index = 0; index < 16; index++)
	{
		state.streams[index][0] = soft_device.streams[index].data;
		state.streams[index][1] = soft_device.streams[index].stride;
	}
	state.base_vertex_index = soft_device.base_vertex_index;
	state.depth_scale = soft_device.depth_scale;
	state.flags = (to_screen ? 1 : 0) | (soft_device.visibility_test_active ? 2 : 0);
	state.program = (unsigned long)program;
	state.shader = (unsigned long)shader;
	record(SOFT_CAPTURE_STATE, &state, sizeof(state), NULL, 0);

	draw[0] = type;
	draw[1] = vertex_count;
	draw[2] = start_vertex;
	draw[3] = indices ? 1 : 0;
	draw[4] = immediate_vertices ? 1 : 0;
	if (immediate_vertices)
		record(SOFT_CAPTURE_DRAW, draw, sizeof(draw), immediate_vertices,
			vertex_count * XGPU_VERTEX_ATTRIBUTE_COUNT * 4 * sizeof(float));
	else if (indices)
	{
		/* (padded to a whole word, without reading past the indices) */
		static const WORD padding = 0;
		unsigned long header[2];

		header[0] = SOFT_CAPTURE_DRAW;
		header[1] = sizeof(draw) + ((vertex_count + 1) & ~1UL) * sizeof(WORD);
		write_through(header, sizeof(header));
		write_through(draw, sizeof(draw));
		write_through(indices, vertex_count * sizeof(WORD));
		if (vertex_count & 1)
			write_through(&padding, sizeof(padding));
		capture.bytes += sizeof(header) + header[1];
	}
	else
		record(SOFT_CAPTURE_DRAW, draw, sizeof(draw), NULL, 0);
}
