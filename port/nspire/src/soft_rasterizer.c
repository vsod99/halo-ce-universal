/*
SOFT_RASTERIZER.C

The Nspire port's rasterizer: draws the device's primitives (d3d8_soft.c)
into the screen's 320x240 RGB565 pixels, with a 16-bit depth buffer.

A draw fetches its vertices from the Xbox vertex formats, runs the vertex
program (soft_vertex.c) once per vertex, clips each triangle in clip space
against the near plane and a guard band, and fills it scanline by scanline
in fixed point. A pixel's colour comes from the NV2A register combiners as
the draw set them (the pixel shader render states, as
port/linux/src/nv2a_psh.c reads them): the textures of the stages that
sample one, the general combiner stages and the final combiner, in 8.8
fixed point; then the alpha test, the depth test and the blend.

Simplifications, for the calculator's speed: texture coordinates and
colours are interpolated affinely (no perspective correction), textures are
point sampled, fog is not drawn, and only the converted maps' swizzled
A4R4G4B4 textures are read (others read as white). Only the screen and the
small render targets (the motion sensor's: drawing_to_texture) are drawn
to: draws into the game's other render targets (the secondary buffer,
water, shadows) are skipped.
*/

#include "soft_rasterizer.h"
#include "nspire.h"
#include "soft_capture.h"

/* the renderer's parts timed one by one (vertex fetch, program, triangle
setup, spans...): many timer reads a triangle, which cost time of their own,
so only when asked for (the replay builds with it) */
#ifndef NSPIRE_FINE_PROFILE
#define NSPIRE_FINE_PROFILE 0
#endif
#if NSPIRE_FINE_PROFILE
#define FINE_PROFILE_BEGIN(section) NSPIRE_PROFILE_BEGIN(section)
#define FINE_PROFILE_END(section) NSPIRE_PROFILE_END(section)
#else
#define FINE_PROFILE_BEGIN(section)
#define FINE_PROFILE_END(section)
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH NSPIRE_SCREEN_WIDTH
#define HEIGHT NSPIRE_SCREEN_HEIGHT
#define ONE 256
#define ONE_SHIFT 8
#define GUARD_BAND 256.0f
#define MINIMUM_W 1.0e-5f
#define MAXIMUM_CLIPPED_VERTICES 16

enum
{
	_register_zero = 0, _register_c0, _register_c1, _register_fog, _register_v0, _register_v1,
	_register_t0 = 8, _register_t1, _register_t2, _register_t3, _register_r0, _register_r1,
	_register_v1r0_sum, _register_ef_product, NUMBER_OF_REGISTERS,
};

enum
{
	_texture_mode_none = 0, _texture_mode_project2d = 1, _texture_mode_project3d = 2, _texture_mode_cube = 3,
	_texture_mode_passthrough = 4, _texture_mode_bump_environment = 6, _texture_mode_bump_environment_luminance = 7,
};

static unsigned short *depth_buffer;
/* the screen's alpha, which RGB565 has no room for: the HUD draws masks
into it (the motion sensor's circle) and blends by them */
static unsigned char *alpha_buffer;

/* Where draws go. The 3D world is drawn at a quarter of the screen's
pixels, 160x120, and doubled onto the screen (resolve) when the engine has
finished it (render_window, before the HUD) or at Present; the HUD and
overlays after it are drawn at the screen's 320x240. Depth is only ever kept
at 160x120 (depth_shift: full-size draws look it up at half their
coordinates). */
#define LOW_WIDTH (WIDTH / 2)
#define LOW_HEIGHT (HEIGHT / 2)

struct target
{
	unsigned short *pixels;
	unsigned char *alpha;
	long width, height;
	int depth_shift;
	/* the game's 640x480 to this */
	float factor;
	/* whether the target keeps alpha: the HUD's does (the motion sensor
	draws its mask into it); the 3D world's reads as one and is never
	written, as on the screen before there was alpha at all (the level's
	passes written to it tinted everything) */
	BOOL keeps_alpha;
};

static struct target low_target, screen_target;
static struct target *target = &low_target;
static unsigned short *low_pixels;
static unsigned char *low_alpha;
/* whether this frame's 3D world is on the screen already */
static BOOL resolved;
static void color_to_fixed(DWORD color, long out[4]);
/* whether a draw has written depth since the depth buffer was cleared */
static BOOL depth_written;
static BOOL transform_grow(unsigned long count);
static void vertex_cache_initialize(void);
static void fetch_tables_initialize(void);
static void fetch_fixed_tables_initialize(void);
static void vertex_cache_reset(void);
static unsigned long primitive_count;

/* where the time goes: totals since the last report, and one frame's draws
logged one by one */
static struct
{
	unsigned long draws, vertices, triangles, clipped, rejected, culled, filled, pixels_tested, pixels_written;
	unsigned long hidden_draws, hidden_triangles;
	/* objects soft_rasterizer_sphere_hidden was asked about, and found hidden */
	unsigned long objects_tested, objects_hidden;
	/* draws that skipped what an earlier pass of theirs did not fill */
	unsigned long repeated_passes;
	/* skinned parts culled whole (part_hidden) */
	unsigned long parts_culled;
	/* of them, those skipped for having shown nothing when last drawn */
	unsigned long parts_skipped;
	unsigned long mismatches;
} counters;
static BOOL log_draws;
/* (the replay sets it to log each draw's combiners) */
BOOL describe_combiners;
static unsigned long draw_number;
/* frames, counted by soft_rasterizer_frame_end (part_hidden) */
static unsigned long part_frame = 16;

/* repeated passes (pass_memo_begin) */
#ifndef NSPIRE_PASS_MEMO
#define NSPIRE_PASS_MEMO 1
#endif
#define PASS_MEMO_ENTRIES 128
#define PASS_MEMO_WORDS 4096

struct pass_memo
{
	/* the position stream's data, and the indices' hash */
	unsigned long positions, indices;
	unsigned long vertex_count, first, type, cull, transform;
	unsigned long *bits;
	/* the clip position of the draw's first vertex, which a later pass's
	program must give too (the same transform) */
	float sample[4];
	BOOL sampled;
};

static struct pass_memo pass_memos[PASS_MEMO_ENTRIES];
static unsigned long pass_memo_count, pass_memo_words_used;
static unsigned long pass_memo_words[PASS_MEMO_WORDS];
/* the draw's: the earlier pass's bits it skips by, the bits it records,
and the order of the triangle at hand (for each, its own count) */
static const unsigned long *pass_skip;
static unsigned long *pass_record;
/* the entry matched (pass_skip once its sample agrees) or recorded */
static struct pass_memo *pass_candidate, *pass_recording;
static unsigned long pass_skip_ordinal, pass_record_ordinal;

/* the order in its draw of the triangle being drawn (pass_record) */
static unsigned long current_ordinal;


/* how fast the calculator does the renderer's kinds of work, logged once:
a check that the processor and its caches run as fast as expected */
static void calibrate(void)
{
	unsigned long long start;
	unsigned long iterations = 4000000, index, accumulator = 1;
	volatile float a = 1.0001f, b = 0.9999f;
	float product = 1.0f;
	unsigned long integer_us, float_ns, fill_us;

	start = nspire_ticks();
	for (index = 0; index < iterations; index++)
	{
		accumulator = accumulator * 3 + index;
		__asm__ volatile ("" : "+r" (accumulator));
	}
	integer_us = (unsigned long)((nspire_ticks() - start) * 1000000ULL / 32768ULL);

	start = nspire_ticks();
	for (index = 0; index < 200000; index++)
		product = product * a * b;
	float_ns = (unsigned long)((nspire_ticks() - start) * 1000000000ULL / 32768ULL / 400000ULL);

	start = nspire_ticks();
	for (index = 0; index < 10; index++)
	{
		unsigned long pixel;

		for (pixel = 0; pixel < WIDTH * HEIGHT; pixel++)
			depth_buffer[pixel] = (unsigned short)(pixel + index);
		__asm__ volatile ("" : : "r" (depth_buffer) : "memory");
	}
	fill_us = (unsigned long)((nspire_ticks() - start) * 1000000ULL / 32768ULL / 10ULL);

	nspire_log("speed: %lu ns per integer loop step (5 instructions, 6 cycles), %lu ns per float multiply, "
		"%lu us to write a screen of 16-bit values (%lu %lu)", (unsigned long)(integer_us * 1000ULL / iterations),
		float_ns, fill_us, accumulator & 1, (unsigned long)(product > 0.0f));
}

void soft_rasterizer_initialize(void)
{
	depth_buffer = malloc(WIDTH * HEIGHT * sizeof(unsigned short));
	if (!depth_buffer)
		nspire_fatal("no memory for the depth buffer");
	alpha_buffer = calloc(WIDTH * HEIGHT, 1);
	low_pixels = calloc(LOW_WIDTH * LOW_HEIGHT, sizeof(unsigned short));
	low_alpha = calloc(LOW_WIDTH * LOW_HEIGHT, 1);
	if (!alpha_buffer || !low_pixels || !low_alpha)
		nspire_fatal("no memory for the alpha buffer");
	low_target.pixels = low_pixels;
	low_target.alpha = low_alpha;
	low_target.width = LOW_WIDTH;
	low_target.height = LOW_HEIGHT;
	low_target.depth_shift = 0;
	low_target.factor = 0.25f;
	screen_target.alpha = alpha_buffer;
	screen_target.width = WIDTH;
	screen_target.height = HEIGHT;
	screen_target.depth_shift = 1;
	screen_target.factor = 0.5f;
	screen_target.keeps_alpha = TRUE;
	calibrate();
	vertex_cache_initialize();
	fetch_tables_initialize();
	fetch_fixed_tables_initialize();
	memset(depth_buffer, 0xFF, WIDTH * HEIGHT * sizeof(unsigned short));
	/* the scratch arrays for a draw's vertices, made while the heap is
	whole (a large draw asking later can find no block big enough) */
	if (!transform_grow(3072))
		nspire_log("no memory for the vertex arrays yet");
#ifdef REPLAY_STALE_DEPTH
	/* (the replay starting from a depth buffer left nearest, as a frame not
	clearing it would see) */
	memset(depth_buffer, 0, WIDTH * HEIGHT * sizeof(unsigned short));
#endif
}

void soft_rasterizer_report(unsigned long frames)
{
	if (!frames)
		return;
	nspire_log("  per frame: %lu draws (%lu hidden), %lu vertices, %lu triangles (%lu clipped, %lu off screen, "
		"%lu culled or thin, %lu filled, %lu hidden), %lu pixels tested, %lu written", counters.draws / frames,
		counters.hidden_draws / frames, counters.vertices / frames,
		counters.triangles / frames, counters.clipped / frames, counters.rejected / frames, counters.culled / frames,
		counters.filled / frames, counters.hidden_triangles / frames, counters.pixels_tested / frames,
		counters.pixels_written / frames);
	nspire_log("  per frame: %lu objects tested against depth, %lu hidden; %lu repeated passes; %lu parts culled whole "
		"(%lu of them skipped as unseen)", counters.objects_tested / frames, counters.objects_hidden / frames,
		counters.repeated_passes / frames, counters.parts_culled / frames, counters.parts_skipped / frames);
	memset(&counters, 0, sizeof(counters));
}

void soft_rasterizer_log_next_frame(void)
{
	log_draws = TRUE;
	draw_number = 0;
	nspire_log("the frame's draws: primitive type, count, program instructions, vertices, triangles, filled, "
		"pixels tested, written, ms; combiner stages, final combiner, textures, blend, alpha test, z test, z write; "
		"pixels the compiled combiners got wrong");
}

/* a note in the draw log (when a frame's draws are logged) of what the
engine draws next, for telling a frame's draws apart */
void soft_rasterizer_mark(const char *text, long value)
{
	if (log_draws)
		nspire_log("  before draw %lu: %s %ld", draw_number, text, value);
}

void soft_rasterizer_frame_end(void)
{
	soft_capture_frame_end();
	resolved = FALSE;
	log_draws = FALSE;
	vertex_cache_reset();
	pass_memo_count = pass_memo_words_used = 0;
	part_frame++;
}

unsigned long soft_rasterizer_take_primitive_count(void)
{
	unsigned long count = primitive_count;

	primitive_count = 0;
	return count;
}

static BOOL drawing_to_screen(void)
{
	return soft_device.render_target == &soft_device.back_buffer;
}

/* ---------- small render targets

The motion sensor draws its sweep and blips into a 64x64 render target and
pastes that on the HUD (source/rasterizer/xbox/rasterizer_xbox_motion_sensor.c);
undrawn, it pasted a white square. Render targets that small, made A4R4G4B4
(port/linux/src/d3d8_resources.c: the only texels the samplers read), are
drawn to here at their size, without depth, and written into their texture,
swizzled, when the game turns to another target. */
#define TEXTURE_TARGET_MOST 64
static unsigned short texture_target_pixels[TEXTURE_TARGET_MOST * TEXTURE_TARGET_MOST];
static unsigned char texture_target_alpha[TEXTURE_TARGET_MOST * TEXTURE_TARGET_MOST];
static struct target texture_target;
/* the surface texture_target holds, drawn since it was last written out */
static const D3DSurface *texture_target_surface;
static BOOL texture_target_dirty;

static BOOL swizzle_tables_get(long width, long height, const unsigned short **x_table,
	const unsigned short **y_table);

static BOOL drawing_to_texture(void)
{
	const D3DSurface *surface = soft_device.render_target;
	struct xgpu_texture_description description;

	if (!surface || surface == &soft_device.back_buffer || !surface->Data)
		return FALSE;
	xgpu_texture_describe(surface->Format, surface->Size, &description);
	return description.format == D3DFMT_A4R4G4B4 && !description.linear && description.depth <= 1 &&
		description.width <= TEXTURE_TARGET_MOST && description.height <= TEXTURE_TARGET_MOST;
}

/* texture_target, made the render target's (whose texels are written over
by what is drawn: the game clears it first) */
static void texture_target_begin(void)
{
	const D3DSurface *surface = soft_device.render_target;
	struct xgpu_texture_description description;

	if (surface == texture_target_surface)
		return;
	xgpu_texture_describe(surface->Format, surface->Size, &description);
	texture_target_surface = surface;
	texture_target.pixels = texture_target_pixels;
	texture_target.alpha = texture_target_alpha;
	texture_target.width = (long)description.width;
	texture_target.height = (long)description.height;
	texture_target.depth_shift = 0;
	texture_target.factor = 1.0f;
	texture_target.keeps_alpha = TRUE;
}

/* what was drawn into the texture, as its swizzled A4R4G4B4 texels
(d3d8_soft.c, as the render target changes) */
void soft_rasterizer_texture_target_flush(void)
{
	const unsigned short *x_table, *y_table;
	unsigned short *texels;
	long x, y;

	if (!texture_target_dirty || !texture_target_surface)
		return;
	texture_target_dirty = FALSE;
	if (!swizzle_tables_get(texture_target.width, texture_target.height, &x_table, &y_table))
		return;
	texels = (unsigned short *)PLATFORM_PHYSICAL_TO_VIRTUAL(texture_target_surface->Data);
	for (y = 0; y < texture_target.height; y++)
	{
		const unsigned short *row = texture_target_pixels + y * texture_target.width;
		const unsigned char *alpha = texture_target_alpha + y * texture_target.width;

		for (x = 0; x < texture_target.width; x++)
		{
			unsigned pixel = row[x];

			texels[x_table[x] | y_table[y]] = (unsigned short)(((unsigned)(alpha[x] >> 4) << 12) |
				((pixel >> 12) << 8) | (((pixel >> 7) & 15) << 4) | ((pixel >> 1) & 15));
		}
	}
}

static unsigned short rgb565(long r, long g, long b)
{
	return (unsigned short)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* ---------- clearing */

/* the 3D world, doubled onto the screen */
void soft_rasterizer_resolve(void)
{
	unsigned short *screen = nspire_video_pixels();
	long x, y;

	if (resolved || !screen)
		return;
	resolved = TRUE;
	soft_capture_resolve();
	for (y = 0; y < LOW_HEIGHT; y++)
	{
		const unsigned short *source = low_pixels + y * LOW_WIDTH;
		unsigned long *row = (unsigned long *)(screen + 2 * y * WIDTH);
		unsigned long *next = (unsigned long *)(screen + (2 * y + 1) * WIDTH);
		unsigned char *alpha = alpha_buffer + 2 * y * WIDTH;

		for (x = 0; x < LOW_WIDTH; x++)
		{
			unsigned long pair = (unsigned long)source[x] | ((unsigned long)source[x] << 16);

			row[x] = pair;
			next[x] = pair;
		}
		/* (the 3D world's alpha reads as one: low_target.keeps_alpha) */
		memset(alpha, 0xFF, 2 * WIDTH);
	}
}

void soft_clear(long left, long top, long right, long bottom, DWORD flags, D3DCOLOR color, float z)
{
	struct target *clear_target = resolved ? &screen_target : &low_target;
	unsigned short *pixels = resolved ? nspire_video_pixels() : low_pixels;
	/* (the game's coordinates are 640x480) */
	int shift = resolved ? 1 : 2;
	long x0, y0, x1, y1, x, y;

	soft_capture_clear(left, top, right, bottom, flags, color, z, drawing_to_screen());
	if (drawing_to_texture())
	{
		/* (in the target's own texels; no depth) */
		texture_target_begin();
		texture_target_dirty = TRUE;
		if (!(flags & D3DCLEAR_TARGET))
			return;
		x0 = left < 0 ? 0 : left;
		y0 = top < 0 ? 0 : top;
		x1 = right > texture_target.width ? texture_target.width : right;
		y1 = bottom > texture_target.height ? texture_target.height : bottom;
		for (y = y0; y < y1; y++)
		{
			for (x = x0; x < x1; x++)
				texture_target_pixels[y * texture_target.width + x] =
					rgb565((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
			if (x1 > x0)
				memset(texture_target_alpha + y * texture_target.width + x0, (int)(color >> 24), (size_t)(x1 - x0));
		}
		return;
	}
	if (!drawing_to_screen() || !pixels)
		return;
	x0 = left >> shift;
	y0 = top >> shift;
	x1 = (right + (1L << shift) - 1) >> shift;
	y1 = (bottom + (1L << shift) - 1) >> shift;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > clear_target->width) x1 = clear_target->width;
	if (y1 > clear_target->height) y1 = clear_target->height;
	if (flags & D3DCLEAR_TARGET)
	{
		unsigned short value = rgb565((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);

		for (y = y0; y < y1; y++)
		{
			for (x = x0; x < x1; x++)
				pixels[y * clear_target->width + x] = value;
			memset(clear_target->alpha + y * clear_target->width + x0, (int)(color >> 24), (size_t)(x1 - x0));
		}
	}
	if (flags & D3DCLEAR_ZBUFFER)
	{
		/* (depth is kept at 160x120 only) */
		unsigned short value = (unsigned short)(z <= 0.0f ? 0 : z >= 1.0f ? 0xFFFF : (unsigned long)(z * 65535.0f));

		depth_written = FALSE;
		x0 = left >> 2;
		y0 = top >> 2;
		x1 = (right + 3) >> 2;
		y1 = (bottom + 3) >> 2;
		if (x0 < 0) x0 = 0;
		if (y0 < 0) y0 = 0;
		if (x1 > LOW_WIDTH) x1 = LOW_WIDTH;
		if (y1 > LOW_HEIGHT) y1 = LOW_HEIGHT;
		for (y = y0; y < y1; y++)
		{
			for (x = x0; x < x1; x++)
				depth_buffer[y * LOW_WIDTH + x] = value;
		}
	}
}

/* ---------- vertex fetch */

/* a byte over 255, looked up (a division costs a hundred instructions) */
static float byte_unit[256];

static void fetch_tables_initialize(void)
{
	int value;

	for (value = 0; value < 256; value++)
		byte_unit[value] = (float)value / 255.0f;
}

static void fetch_attribute(unsigned char type, const unsigned char *data, float out[4])
{
	out[0] = 0.0f;
	out[1] = 0.0f;
	out[2] = 0.0f;
	out[3] = 1.0f;
	switch (type)
	{
	case D3DVSDT_FLOAT4: memcpy(&out[3], data + 12, 4); /* fall through */
	case D3DVSDT_FLOAT3: memcpy(&out[2], data + 8, 4); /* fall through */
	case D3DVSDT_FLOAT2: memcpy(&out[1], data + 4, 4); /* fall through */
	case D3DVSDT_FLOAT1: memcpy(&out[0], data, 4); break;
	case D3DVSDT_D3DCOLOR:
		out[0] = byte_unit[data[2]];
		out[1] = byte_unit[data[1]];
		out[2] = byte_unit[data[0]];
		out[3] = byte_unit[data[3]];
		break;
	case D3DVSDT_SHORT4: out[3] = (float)(short)(data[6] | data[7] << 8); /* fall through */
	case D3DVSDT_SHORT3: out[2] = (float)(short)(data[4] | data[5] << 8); /* fall through */
	case D3DVSDT_SHORT2: out[1] = (float)(short)(data[2] | data[3] << 8); /* fall through */
	case D3DVSDT_SHORT1: out[0] = (float)(short)(data[0] | data[1] << 8); break;
	case D3DVSDT_NORMSHORT4: out[3] = (short)(data[6] | data[7] << 8) * (1.0f / 32767.0f); /* fall through */
	case D3DVSDT_NORMSHORT3: out[2] = (short)(data[4] | data[5] << 8) * (1.0f / 32767.0f); /* fall through */
	case D3DVSDT_NORMSHORT2: out[1] = (short)(data[2] | data[3] << 8) * (1.0f / 32767.0f); /* fall through */
	case D3DVSDT_NORMSHORT1: out[0] = (short)(data[0] | data[1] << 8) * (1.0f / 32767.0f); break;
	case D3DVSDT_NORMPACKED3:
	{
		unsigned long packed = data[0] | data[1] << 8 | data[2] << 16 | (unsigned long)data[3] << 24;

		out[0] = (float)((long)(packed << 21) >> 21) * (1.0f / 1023.0f);
		out[1] = (float)((long)(packed << 10) >> 21) * (1.0f / 1023.0f);
		out[2] = (float)((long)packed >> 22) * (1.0f / 511.0f);
		break;
	}
	case D3DVSDT_PBYTE4: out[3] = byte_unit[data[3]]; /* fall through */
	case D3DVSDT_PBYTE3: out[2] = byte_unit[data[2]]; /* fall through */
	case D3DVSDT_PBYTE2: out[1] = byte_unit[data[1]]; /* fall through */
	case D3DVSDT_PBYTE1: out[0] = byte_unit[data[0]]; break;
	case D3DVSDT_FLOAT2H:
		memcpy(&out[0], data, 4);
		memcpy(&out[1], data + 4, 4);
		memcpy(&out[3], data + 8, 4);
		break;
	default:
		break;
	}
}

/* a float in 16.16 fixed point, rounded toward zero and saturated, from its
bits (the library's conversions cost far more) */
static __inline__ __attribute__((always_inline)) long fixed16(float value)
{
	union
	{
		float f;
		unsigned long u;
	} bits;
	long exponent, result;
	unsigned long mantissa;

	bits.f = value;
	exponent = (long)((bits.u >> 23) & 0xFF) - 127;
	if (exponent < -17)
		return 0;
	if (exponent >= 15)
		return (bits.u & 0x80000000UL) ? -0x7FFFFFFFL : 0x7FFFFFFFL;
	mantissa = (bits.u & 0x7FFFFFUL) | 0x800000UL;
	result = (long)(exponent >= 7 ? mantissa << (exponent - 7) : mantissa >> (7 - exponent));
	return (bits.u & 0x80000000UL) ? -result : result;
}

/* attributes fetched straight into fixed point for programs that run there */
#ifndef NSPIRE_FIXED_FETCH
#define NSPIRE_FIXED_FETCH 1
#endif
/* clip codes from the programs' fixed-point clip positions */
#ifndef NSPIRE_FIXED_OUTCODES
#define NSPIRE_FIXED_OUTCODES 1
#endif

/* two-phase draws (positions first, the rest for triangles seen): once
cheaper, now dearer than one pass (7-10% of a frame, with flat model
lighting, the vertex code and part culling in) */
#ifndef NSPIRE_TWO_PHASE
#define NSPIRE_TWO_PHASE 0
#endif

/* vertices projected in integers from their 16.16 clip positions */
#ifndef NSPIRE_FIXED_PROJECTION
#define NSPIRE_FIXED_PROJECTION 1
#endif

/* ---------- vertex fetch in fixed point

For a program that runs in 16.16 fixed point (soft_vertex_program_run_fixed),
attributes go straight there from the stream's format, with no floating
point between. A float too large for 16.16 sets *overflow. */

#define FIXED_ONE_UNIT 65536L

static long byte_fixed[256];

static void fetch_fixed_tables_initialize(void)
{
	int value;

	for (value = 0; value < 256; value++)
		byte_fixed[value] = (long)(((long long)value * FIXED_ONE_UNIT) / 255);
}

static __inline__ long float_bits_to_fixed(const unsigned char *data, BOOL *overflow)
{
	float value;

	memcpy(&value, data, sizeof(value));
	{
		long fixed = fixed16(value);

		if (fixed == 0x7FFFFFFFL || fixed == -0x7FFFFFFFL)
			*overflow = TRUE;
		return fixed;
	}
}

static void fetch_attribute_fixed(unsigned char type, const unsigned char *data, long out[4], BOOL *overflow)
{
	out[0] = 0;
	out[1] = 0;
	out[2] = 0;
	out[3] = FIXED_ONE_UNIT;
	switch (type)
	{
	case D3DVSDT_FLOAT4: out[3] = float_bits_to_fixed(data + 12, overflow); /* fall through */
	case D3DVSDT_FLOAT3: out[2] = float_bits_to_fixed(data + 8, overflow); /* fall through */
	case D3DVSDT_FLOAT2: out[1] = float_bits_to_fixed(data + 4, overflow); /* fall through */
	case D3DVSDT_FLOAT1: out[0] = float_bits_to_fixed(data, overflow); break;
	case D3DVSDT_D3DCOLOR:
		out[0] = byte_fixed[data[2]];
		out[1] = byte_fixed[data[1]];
		out[2] = byte_fixed[data[0]];
		out[3] = byte_fixed[data[3]];
		break;
	case D3DVSDT_SHORT4: out[3] = (long)(short)(data[6] | data[7] << 8) << 16; /* fall through */
	case D3DVSDT_SHORT3: out[2] = (long)(short)(data[4] | data[5] << 8) << 16; /* fall through */
	case D3DVSDT_SHORT2: out[1] = (long)(short)(data[2] | data[3] << 8) << 16; /* fall through */
	case D3DVSDT_SHORT1: out[0] = (long)(short)(data[0] | data[1] << 8) << 16; break;
	/* (over 32767: times 65538 / 32768, near enough 65536 / 32767) */
	case D3DVSDT_NORMSHORT4: out[3] = ((long)(short)(data[6] | data[7] << 8) * 65538L) >> 15; /* fall through */
	case D3DVSDT_NORMSHORT3: out[2] = ((long)(short)(data[4] | data[5] << 8) * 65538L) >> 15; /* fall through */
	case D3DVSDT_NORMSHORT2: out[1] = ((long)(short)(data[2] | data[3] << 8) * 65538L) >> 15; /* fall through */
	case D3DVSDT_NORMSHORT1: out[0] = ((long)(short)(data[0] | data[1] << 8) * 65538L) >> 15; break;
	case D3DVSDT_NORMPACKED3:
	{
		unsigned long packed = data[0] | data[1] << 8 | data[2] << 16 | (unsigned long)data[3] << 24;

		/* over 1023 and 511: times 65536 / 1023 and / 511, as 16.16 multipliers */
		out[0] = (long)(((long long)((long)(packed << 21) >> 21) * 4198404LL) >> 16);
		out[1] = (long)(((long long)((long)(packed << 10) >> 21) * 4198404LL) >> 16);
		out[2] = (long)(((long long)((long)packed >> 22) * 8405024LL) >> 16);
		break;
	}
	case D3DVSDT_PBYTE4: out[3] = byte_fixed[data[3]]; /* fall through */
	case D3DVSDT_PBYTE3: out[2] = byte_fixed[data[2]]; /* fall through */
	case D3DVSDT_PBYTE2: out[1] = byte_fixed[data[1]]; /* fall through */
	case D3DVSDT_PBYTE1: out[0] = byte_fixed[data[0]]; break;
	case D3DVSDT_FLOAT2H:
		out[0] = float_bits_to_fixed(data, overflow);
		out[1] = float_bits_to_fixed(data + 4, overflow);
		out[3] = float_bits_to_fixed(data + 8, overflow);
		break;
	default:
		break;
	}
}

/* ---------- transformed vertices */

struct clip_vertex
{
	float clip[4];
	float color[4];
	float texture[4][2];
	/* (lerp_vertex blends the floats above alone) */
	/* u and v in 16.16 instead, when texture_is_fixed (from fixed-point
	vertex code: float only once a triangle is clipped) */
	long texture_fixed[4][2];
	int texture_is_fixed;
	/* the clip position in 16.16 too, when clip_is_fixed (projected then in
	integers) */
	long clip_fixed[4];
	int clip_is_fixed;
};

struct screen_vertex
{
	/* the draw's interpolated attributes, in draw_state.attributes' order,
	in 16.16 (project), for setting up triangles in integers */
	long fixed[13];
	/* x and y in 16.16 (saturated), for the area's sign in integers */
	long fixed_x, fixed_y;
};

/* which clipping planes (bits 0-5, plane_distance) a vertex is outside, and
which sides of the screen itself (bits 6-9) */
#define OUTCODE_CLIP_MASK 0x3F

static struct clip_vertex *transformed;
static unsigned long transformed_capacity;
static float screen_scale[3], screen_offset[3];

/* vertex_outcode's tests for a fixed-point clip position: each axis's terms
(its scale, and for w the offset, far edge and guard band) scaled together so
the largest is near 2^29; a test's sign does not change with the scale */
static long outcode_scale[3], outcode_offset[3], outcode_far[2], outcode_guard[2];

static long outcode_term(float value, float scale)
{
	return fixed16(value * scale);
}

static void outcode_constants(void)
{
	float far_edge[2] = { 640.0f - screen_offset[0], 480.0f - screen_offset[1] };
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		float largest = fabsf(screen_scale[axis]), scale;

		if (fabsf(screen_offset[axis]) > largest)
			largest = fabsf(screen_offset[axis]);
		if (axis < 2)
		{
			if (fabsf(far_edge[axis]) + GUARD_BAND > largest)
				largest = fabsf(far_edge[axis]) + GUARD_BAND;
			if (GUARD_BAND > largest)
				largest = GUARD_BAND;
		}
		/* (fixed16 gives 2^16 times its argument) */
		scale = 8192.0f / largest;
		outcode_scale[axis] = outcode_term(screen_scale[axis], scale);
		outcode_offset[axis] = outcode_term(screen_offset[axis], scale);
		if (axis < 2)
		{
			outcode_far[axis] = outcode_term(far_edge[axis], scale);
			outcode_guard[axis] = outcode_term(GUARD_BAND, scale);
		}
	}
}

/* screen = clip * c[-38] / w + c[-37], the programs' own conversion */
static void screen_constants(void)
{
	const float *scale = soft_device.constants[XGPU_VERTEX_CONSTANT_BIAS - 38];
	const float *offset = soft_device.constants[XGPU_VERTEX_CONSTANT_BIAS - 37];
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		screen_scale[axis] = scale[axis] != 0.0f ? scale[axis] : 1.0f;
		screen_offset[axis] = offset[axis];
	}
	outcode_constants();
}

/* the draw's vertices: transformed only when a primitive uses one (an
indexed draw's index range can be far wider than what it draws) */
static unsigned long *transformed_serials;
static unsigned short *transformed_outcodes;
static struct screen_vertex *projected;
static unsigned long *projected_serials;
static unsigned long draw_serial, projection_serial;

/* where transformed_vertex keeps the draw's vertices: the scratch arrays
above, or a vertex cache entry; a vertex is done when its serial is
vertex_serial */
static struct clip_vertex *vertex_store;
static unsigned long *vertex_serials;
/* and those whose position alone is (vertex_serial too): the first phase of
a two-phase draw (transform_draw_vertices) */
static unsigned long *vertex_position_serials;
static unsigned long *transformed_position_serials;
/* the vertices a two-phase draw's visible triangles use (projection_serial) */
static unsigned long *needed_serials;
/* the draw's cull mode (draw_state's, which comes later in this file) */
static DWORD draw_cull_mode;
/* whether a draw hidden behind the depth buffer can be skipped (draw_hidden:
it tests depth with less or less-equal, at 160x120), and its depth scale */
static BOOL draw_occlusion_test;
static float draw_z_normalize;
/* a two-phase draw's vertices on the 160x120 depth buffer: x and y, and
depth (draw_hidden works them out; triangle_needs tests triangles by them) */
struct occlusion_point
{
	float x, y, z;
};
static struct occlusion_point *occlusion_points;
static BOOL occlusion_ready;
static unsigned short *vertex_outcodes;
static unsigned long vertex_serial;

/* ---------- transformed vertices kept for the frame

Halo draws a mesh in passes, first every object's first pass, then every
object's second, re-sending the same constants in between. An indexed draw
of the same vertices through the same program with the same constant values
(a hash of them) takes the vertices an earlier draw transformed. */

#ifndef VERTEX_CACHE_BYTES
#define VERTEX_CACHE_BYTES (128UL * 1024UL)
#endif
#define VERTEX_CACHE_ENTRIES 192

struct vertex_cache_entry
{
	const struct soft_vertex_shader *program, *shader;
	/* the outputs every cached vertex has (soft_vertex_program_decode) */
	unsigned long wanted;
	unsigned long first, count, serial;
	unsigned long constants_hash[2];
	DWORD data[16];
	UINT stride[16];
	struct clip_vertex *vertices;
	unsigned short *outcodes;
	unsigned long *serials, *position_serials;
};

static unsigned char *vertex_cache_arena;
static unsigned long vertex_cache_used, vertex_cache_serial;
static struct vertex_cache_entry vertex_cache[VERTEX_CACHE_ENTRIES];
static int vertex_cache_count;
static unsigned long hashed_constants_serial = ~0UL, constants_hash[2];

static void vertex_cache_initialize(void)
{
	vertex_cache_arena = malloc(VERTEX_CACHE_BYTES);
}

static void vertex_cache_reset(void)
{
	vertex_cache_used = 0;
	vertex_cache_count = 0;
}

/* the vertex constants and default attributes, hashed two ways */
static void constants_hash_update(void)
{
	const unsigned long *words;
	unsigned long count, index, fnv = 2166136261UL, sum = 0;

	if (hashed_constants_serial == soft_device.constants_serial)
		return;
	hashed_constants_serial = soft_device.constants_serial;
	words = (const unsigned long *)soft_device.constants;
	count = sizeof(soft_device.constants) / sizeof(unsigned long);
	for (index = 0; index < count; index++)
	{
		fnv = (fnv ^ words[index]) * 16777619UL;
		sum = ((sum << 5) | (sum >> 27)) + words[index];
	}
	words = (const unsigned long *)soft_device.attributes;
	count = sizeof(soft_device.attributes) / sizeof(unsigned long);
	for (index = 0; index < count; index++)
	{
		fnv = (fnv ^ words[index]) * 16777619UL;
		sum = ((sum << 5) | (sum >> 27)) + words[index];
	}
	constants_hash[0] = fnv;
	constants_hash[1] = sum;
}

/* the cache entry for an indexed draw's vertices, found or made; NULL when
there is no room */
static struct vertex_cache_entry *vertex_cache_find(const struct soft_vertex_shader *program,
	const struct soft_vertex_shader *shader, unsigned long wanted, unsigned long first, unsigned long count)
{
	struct vertex_cache_entry *entry;
	unsigned long bytes;
	int index, stream;

	if (!vertex_cache_arena)
		return NULL;
	constants_hash_update();
	for (index = vertex_cache_count - 1; index >= 0; index--)
	{
		entry = &vertex_cache[index];
		if (entry->program != program || entry->shader != shader || entry->first != first ||
			entry->count < count || (entry->wanted & wanted) != wanted ||
			entry->constants_hash[0] != constants_hash[0] || entry->constants_hash[1] != constants_hash[1])
		{
			continue;
		}
		for (stream = 0; stream < 16; stream++)
		{
			if (entry->data[stream] != soft_device.streams[stream].data ||
				entry->stride[stream] != soft_device.streams[stream].stride)
			{
				break;
			}
		}
		if (stream < 16)
			continue;
		/* vertices this draw transforms have only its outputs */
		entry->wanted &= wanted;
		return entry;
	}

	bytes = count * (sizeof(struct clip_vertex) + 2 * sizeof(unsigned long) + sizeof(unsigned short));
	bytes = (bytes + 7) & ~7UL;
	if (vertex_cache_count >= VERTEX_CACHE_ENTRIES || vertex_cache_used + bytes > VERTEX_CACHE_BYTES)
		return NULL;
	entry = &vertex_cache[vertex_cache_count++];
	entry->program = program;
	entry->shader = shader;
	entry->wanted = wanted;
	entry->first = first;
	entry->count = count;
	entry->serial = ++vertex_cache_serial;
	entry->constants_hash[0] = constants_hash[0];
	entry->constants_hash[1] = constants_hash[1];
	for (stream = 0; stream < 16; stream++)
	{
		entry->data[stream] = soft_device.streams[stream].data;
		entry->stride[stream] = soft_device.streams[stream].stride;
	}
	entry->vertices = (struct clip_vertex *)(vertex_cache_arena + vertex_cache_used);
	entry->serials = (unsigned long *)(entry->vertices + count);
	entry->position_serials = entry->serials + count;
	entry->outcodes = (unsigned short *)(entry->position_serials + count);
	memset(entry->serials, 0, 2 * count * sizeof(unsigned long));
	vertex_cache_used += bytes;
	return entry;
}

static struct
{
	struct soft_vertex_shader *shader;
	void *program;
	/* the program trimmed to the position, for a two-phase draw; NULL for one
	phase */
	void *position_program;
	/* with flat lighting: the program that lights (the draw's program does
	not), run for one vertex, and the colour it gave, which every vertex
	takes */
	void *lit_program;
	float flat_color[4];
	unsigned long first;
	const float *immediate_vertices;
} draw_source;

static unsigned short vertex_outcode(const float *clip);
static unsigned short vertex_outcode_fixed(const long *clip);

static BOOL transform_grow(unsigned long count);

static BOOL transform_prepare(unsigned long count, struct vertex_cache_entry *entry)
{
	if (count > transformed_capacity)
	{
		/* (grown by half again at least, so a few large draws do not ask the
		heap for ever larger blocks; a failure is logged, as the draw is
		then lost: a level's cliff was) */
		unsigned long wanted = count;

		if (count < transformed_capacity + transformed_capacity / 2)
			count = transformed_capacity + transformed_capacity / 2;
		if (!transform_grow(count) && (count = wanted, !transform_grow(count)))
		{
			static unsigned long logged;

			if (logged++ < 8)
				nspire_log("draw lost: no memory for %lu vertices (%lu kept)", wanted, transformed_capacity);
			return FALSE;
		}
	}
	/* a new serial marks every vertex unprojected (the projection depends on
	the draw's other state), and in the scratch arrays untransformed */
	projection_serial++;
	if (entry)
	{
		vertex_store = entry->vertices;
		vertex_serials = entry->serials;
		vertex_position_serials = entry->position_serials;
		vertex_outcodes = entry->outcodes;
		vertex_serial = entry->serial;
	}
	else
	{
		draw_serial++;
		vertex_store = transformed;
		vertex_serials = transformed_serials;
		vertex_position_serials = transformed_position_serials;
		vertex_outcodes = transformed_outcodes;
		vertex_serial = draw_serial;
	}
	return TRUE;
}

/* the scratch arrays for count vertices (all or none kept grown) */
static BOOL transform_grow(unsigned long count)
{
	{
		struct clip_vertex *grown = realloc(transformed, count * sizeof(*grown));
		unsigned long *serials;

		if (!grown)
			return FALSE;
		transformed = grown;
		serials = realloc(transformed_serials, count * sizeof(*serials));
		if (!serials)
			return FALSE;
		transformed_serials = serials;
		memset(serials + transformed_capacity, 0, (count - transformed_capacity) * sizeof(*serials));
		{
			unsigned short *outcodes = realloc(transformed_outcodes, count * sizeof(*outcodes));
			struct screen_vertex *screen;

			if (!outcodes)
				return FALSE;
			transformed_outcodes = outcodes;
			screen = realloc(projected, count * sizeof(*screen));
			if (!screen)
				return FALSE;
			projected = screen;
			serials = realloc(projected_serials, count * sizeof(*serials));
			if (!serials)
				return FALSE;
			memset(serials + transformed_capacity, 0, (count - transformed_capacity) * sizeof(*serials));
			projected_serials = serials;
			serials = realloc(transformed_position_serials, count * sizeof(*serials));
			if (!serials)
				return FALSE;
			memset(serials + transformed_capacity, 0, (count - transformed_capacity) * sizeof(*serials));
			transformed_position_serials = serials;
			serials = realloc(needed_serials, count * sizeof(*serials));
			if (!serials)
				return FALSE;
			memset(serials + transformed_capacity, 0, (count - transformed_capacity) * sizeof(*serials));
			needed_serials = serials;
			/* (the occlusion points only for two-phase draws, draw_hidden) */
			if (NSPIRE_TWO_PHASE)
			{
				struct occlusion_point *points = realloc(occlusion_points, count * sizeof(*points));

				if (!points)
					return FALSE;
				occlusion_points = points;
			}
		}
		transformed_capacity = count;
	}
	return TRUE;
}

/* ---------- transforming, a batch at a time

A draw's vertices are gathered first (those its primitives use and no
earlier pass has transformed), then run through its vertex program
SOFT_VERTEX_BATCH at a time (soft_vertex.c). */

static float batch_inputs[SOFT_VERTEX_BATCH][XGPU_VERTEX_ATTRIBUTE_COUNT][4];
/* the same in fixed point (vertex_fetch_fixed), with the device's defaults
for registers no stream gives (fixed_defaults) */
static long batch_fixed_inputs[SOFT_VERTEX_BATCH][XGPU_VERTEX_ATTRIBUTE_COUNT][4];
static long fixed_defaults[XGPU_VERTEX_ATTRIBUTE_COUNT][4];
static BOOL fixed_defaults_overflow;
static int batch_overflow[SOFT_VERTEX_BATCH];
static struct soft_vertex_output batch_outputs[SOFT_VERTEX_BATCH];
static unsigned long batch_indices[SOFT_VERTEX_BATCH];

/* a colour component held to 0..1, by its bits (no library compares) */
static __inline__ __attribute__((always_inline)) float unit_clamp(float value)
{
	union
	{
		float f;
		unsigned long u;
	} bits;

	bits.f = value;
	if (bits.u & 0x80000000UL)
		return 0.0f;
	if (bits.u > 0x3F800000UL)
		return 1.0f;
	return value;
}

/* a vertex's attributes from the draw's streams (the registers the streams
do not give keep the defaults batch_defaults put there) */
static void vertex_fetch(unsigned long index, float inputs[][4], unsigned long inputs_read)
{
	if (draw_source.immediate_vertices)
	{
		memcpy(inputs, draw_source.immediate_vertices + index * XGPU_VERTEX_ATTRIBUTE_COUNT * 4,
			XGPU_VERTEX_ATTRIBUTE_COUNT * 4 * sizeof(float));
	}
	else
	{
		const struct soft_vertex_shader *shader = draw_source.shader;
		unsigned long element;

		for (element = 0; element < shader->element_count; element++)
		{
			const struct soft_vertex_element *e = &shader->elements[element];
			DWORD data = soft_device.streams[e->stream].data;

			if (!data || e->reg >= XGPU_VERTEX_ATTRIBUTE_COUNT || !(inputs_read & (1UL << e->reg)))
				continue;
			fetch_attribute(e->type, (const unsigned char *)PLATFORM_PHYSICAL_TO_VIRTUAL(data) +
				(draw_source.first + index) * soft_device.streams[e->stream].stride + e->offset, inputs[e->reg]);
		}
	}
}

/* whether vertex_finish works out the clip codes (not again in a two-phase
draw's second pass) */
static BOOL finish_outcodes = TRUE;

static void vertex_finish(unsigned long index, const struct soft_vertex_output *output)
{
	struct clip_vertex *vertex = &vertex_store[index];
	int k;

	if (output->clip_captured)
	{
		memcpy(vertex->clip, output->clip, sizeof(vertex->clip));
	}
	else
	{
		/* back from screen space to clip space */
		float w = output->position[3];
		int axis;

		for (axis = 0; axis < 3; axis++)
			vertex->clip[axis] = (output->position[axis] - screen_offset[axis]) * w / screen_scale[axis];
		vertex->clip[3] = w;
	}
	vertex->clip_is_fixed = NSPIRE_FIXED_PROJECTION && output->clip_fixed_valid && output->clip_fixed[3] > 0;
	if (vertex->clip_is_fixed)
		memcpy(vertex->clip_fixed, output->clip_fixed, sizeof(vertex->clip_fixed));
	if (draw_source.lit_program)
	{
		vertex->color[0] = draw_source.flat_color[0];
		vertex->color[1] = draw_source.flat_color[1];
		vertex->color[2] = draw_source.flat_color[2];
		vertex->color[3] = draw_source.flat_color[3];
	}
	else
	{
		for (k = 0; k < 4; k++)
			vertex->color[k] = unit_clamp(output->diffuse[k]);
	}
	if (output->texture_fixed_valid)
	{
		memcpy(vertex->texture_fixed, output->texture_fixed, sizeof(vertex->texture_fixed));
		vertex->texture_is_fixed = 1;
	}
	else
	{
		for (k = 0; k < 4; k++)
		{
			vertex->texture[k][0] = output->texture[k][0];
			vertex->texture[k][1] = output->texture[k][1];
		}
		vertex->texture_is_fixed = 0;
	}
	/* (a two-phase draw's position pass found them already) */
	if (finish_outcodes)
	{
		vertex_outcodes[index] = NSPIRE_FIXED_OUTCODES && output->clip_fixed_valid ?
			vertex_outcode_fixed(output->clip_fixed) : vertex_outcode(vertex->clip);
	}
}

/* a vertex's attributes in fixed point from the draw's streams (not for
immediate draws, whose vertices are floats); FALSE if one does not fit */
/* the elements a draw's program reads, each with where its first vertex's
is and the stride, worked out once a draw (projection_serial) and for the
inputs the program reads */
static struct
{
	unsigned long serial, inputs_read;
	int count;
	struct
	{
		const unsigned char *first;
		unsigned long stride;
		unsigned char type, reg;
	} elements[XGPU_VERTEX_ATTRIBUTE_COUNT];
} fetch_plan;

static void fetch_plan_make(unsigned long inputs_read)
{
	const struct soft_vertex_shader *shader = draw_source.shader;
	unsigned long element;

	fetch_plan.serial = projection_serial;
	fetch_plan.inputs_read = inputs_read;
	fetch_plan.count = 0;
	for (element = 0; element < shader->element_count && fetch_plan.count < XGPU_VERTEX_ATTRIBUTE_COUNT; element++)
	{
		const struct soft_vertex_element *e = &shader->elements[element];
		DWORD data = soft_device.streams[e->stream].data;
		unsigned long stride = soft_device.streams[e->stream].stride;

		if (!data || e->reg >= XGPU_VERTEX_ATTRIBUTE_COUNT || !(inputs_read & (1UL << e->reg)))
			continue;
		fetch_plan.elements[fetch_plan.count].first = (const unsigned char *)PLATFORM_PHYSICAL_TO_VIRTUAL(data) +
			draw_source.first * stride + e->offset;
		fetch_plan.elements[fetch_plan.count].stride = stride;
		fetch_plan.elements[fetch_plan.count].type = e->type;
		fetch_plan.elements[fetch_plan.count].reg = e->reg;
		fetch_plan.count++;
	}
}

/* positions fetched over 2^shift (soft_vertex_fixed_scale_positions):
0 but in a draw whose positions do not fit 16.16 */
static int fetch_position_shift;

/* a float position over 2^shift, by its exponent, then in 16.16 */
static __inline__ long float_bits_to_fixed_shifted(const unsigned char *data, int shift, BOOL *overflow)
{
	unsigned long bits;
	float value;

	memcpy(&bits, data, sizeof(bits));
	if (((bits >> 23) & 0xFF) > (unsigned long)shift)
		bits -= (unsigned long)shift << 23;
	else
		bits &= 0x80000000UL;
	memcpy(&value, &bits, sizeof(value));
	{
		long fixed = fixed16(value);

		if (fixed == 0x7FFFFFFFL || fixed == -0x7FFFFFFFL)
			*overflow = TRUE;
		return fixed;
	}
}

static BOOL vertex_fetch_fixed(unsigned long index, long inputs[][4], unsigned long inputs_read)
{
	BOOL overflow = FALSE;
	int element;

	if (fetch_plan.serial != projection_serial || fetch_plan.inputs_read != inputs_read)
		fetch_plan_make(inputs_read);
	for (element = 0; element < fetch_plan.count; element++)
	{
		const unsigned char *data = fetch_plan.elements[element].first + index * fetch_plan.elements[element].stride;
		long *out = inputs[fetch_plan.elements[element].reg];

		if (fetch_position_shift && fetch_plan.elements[element].reg == 0 &&
			fetch_plan.elements[element].type == D3DVSDT_FLOAT3)
		{
			out[0] = float_bits_to_fixed_shifted(data, fetch_position_shift, &overflow);
			out[1] = float_bits_to_fixed_shifted(data + 4, fetch_position_shift, &overflow);
			out[2] = float_bits_to_fixed_shifted(data + 8, fetch_position_shift, &overflow);
			out[3] = FIXED_ONE_UNIT;
			continue;
		}
		fetch_attribute_fixed(fetch_plan.elements[element].type, data, out, &overflow);
	}
	return !overflow;
}

/* A draw whose positions do not fit 16.16 (the sky's): tried in fixed point
over 2^POSITION_SHIFT (soft_vertex_fixed_scale_positions), checked against
floating point on one vertex, and kept for the draw (1) or not (-1). */
#ifndef NSPIRE_POSITION_SCALING
#define NSPIRE_POSITION_SCALING 1
#endif
#define POSITION_SHIFT 4
static int draw_position_scaling;
/* the draw's stages with texture coordinates (state's, set at its start) */
static unsigned long position_scaling_stages;

/* a run's clip position as floats */
static void output_clip(const struct soft_vertex_output *output, float c[4])
{
	int k;

	for (k = 0; k < 4; k++)
		c[k] = output->clip_fixed_valid ? (float)output->clip_fixed[k] * (1.0f / 65536.0f) : output->clip[k];
}

/* whether a scaled fixed-point run agrees with a floating one: the same
point on the screen and depth, and the same texture coordinates */
static BOOL position_scaling_agrees(const struct soft_vertex_output *scaled, const struct soft_vertex_output *reference)
{
	float a[4], b[4];
	int k;

	if (!scaled->clip_fixed_valid || !reference->clip_captured)
		return FALSE;
	output_clip(scaled, a);
	output_clip(reference, b);
	/* (w of the same sign: behind the camera too, the ratios must agree) */
	if (a[3] == 0.0f || b[3] == 0.0f || (a[3] < 0.0f) != (b[3] < 0.0f))
		return FALSE;
	for (k = 0; k < 3; k++)
	{
		float x = a[k] / a[3], y = b[k] / b[3], difference = x - y;

		if (difference < 0.0f) difference = -difference;
		if (difference > 0.001f * (1.0f + (y < 0.0f ? -y : y)))
			return FALSE;
	}
	for (k = 0; k < 4; k++)
	{
		if (!(position_scaling_stages & (1UL << k)))
			continue;
		{
		float u = scaled->texture_fixed_valid ? (float)scaled->texture_fixed[k][0] * (1.0f / 65536.0f) : scaled->texture[k][0];
		float v = scaled->texture_fixed_valid ? (float)scaled->texture_fixed[k][1] * (1.0f / 65536.0f) : scaled->texture[k][1];
		float du = u - reference->texture[k][0], dv = v - reference->texture[k][1];

		if (du < 0.0f) du = -du;
		if (dv < 0.0f) dv = -dv;
		if (du > 0.004f || dv > 0.004f)
			return FALSE;
		}
	}
	return TRUE;
}

static void position_scaling_try(const void *program, unsigned long count, BOOL *fetched, unsigned long inputs_read)
{
	static struct soft_vertex_output reference;
	unsigned long slot, sample = count;

	for (slot = 0; slot < count; slot++)
	{
		if (!fetched[slot] || batch_overflow[slot])
		{
			sample = slot;
			break;
		}
	}
	if (sample == count)
		return;
	/* (the floating-point answer for one that did not fit) */
	memcpy(batch_inputs[0], soft_device.attributes, sizeof(batch_inputs[0]));
	vertex_fetch(batch_indices[sample], batch_inputs[0], inputs_read);
	soft_vertex_program_run_batch(program, 1, (const float (*)[XGPU_VERTEX_ATTRIBUTE_COUNT][4])batch_inputs,
		(const float (*)[4])soft_device.constants, soft_device.constants_serial, &reference);
	/* (the batch again, every vertex scaled: the constants are, for all) */
	soft_vertex_fixed_scale_positions(program, POSITION_SHIFT);
	fetch_position_shift = POSITION_SHIFT;
	for (slot = 0; slot < count; slot++)
		fetched[slot] = vertex_fetch_fixed(batch_indices[slot], batch_fixed_inputs[slot], inputs_read);
	soft_vertex_program_run_fixed(program, count, (const long (*)[XGPU_VERTEX_ATTRIBUTE_COUNT][4])batch_fixed_inputs,
		batch_outputs, batch_overflow);
	if (fetched[sample] && !batch_overflow[sample] && position_scaling_agrees(&batch_outputs[sample], &reference))
	{
		draw_position_scaling = 1;
		return;
	}
	/* (not to be relied on: as before) */
#ifdef DEBUG_POSITION_SCALING
	{
		float a[4], b[4];

		output_clip(&batch_outputs[sample], a);
		output_clip(&reference, b);
		nspire_log("    scaling refused: fetched %d overflow %d valid %d captured %d; clip %ld %ld %ld %ld / %ld %ld %ld %ld (x1000); uv %ld %ld / %ld %ld",
			fetched[sample], batch_overflow[sample], batch_outputs[sample].clip_fixed_valid, reference.clip_captured,
			(long)(a[0] * 1000), (long)(a[1] * 1000), (long)(a[2] * 1000), (long)(a[3] * 1000),
			(long)(b[0] * 1000), (long)(b[1] * 1000), (long)(b[2] * 1000), (long)(b[3] * 1000),
			batch_outputs[sample].texture_fixed[0][0], batch_outputs[sample].texture_fixed[0][1],
			(long)(reference.texture[0][0] * 65536), (long)(reference.texture[0][1] * 65536));
	}
#endif
	draw_position_scaling = -1;
	soft_vertex_fixed_unscale_positions();
	fetch_position_shift = 0;
	for (slot = 0; slot < count; slot++)
		fetched[slot] = vertex_fetch_fixed(batch_indices[slot], batch_fixed_inputs[slot], inputs_read);
	soft_vertex_program_run_fixed(program, count, (const long (*)[XGPU_VERTEX_ATTRIBUTE_COUNT][4])batch_fixed_inputs,
		batch_outputs, batch_overflow);
}

/* the batch's vertices through the program: fetched straight into fixed
point when the program runs there, else as floats */
static void batch_run(const void *program, unsigned long count)
{
	unsigned long slot;
	unsigned long inputs_read = soft_vertex_program_inputs(program);

	if (NSPIRE_FIXED_FETCH && !draw_source.immediate_vertices && !fixed_defaults_overflow &&
		soft_vertex_program_prepare_fixed(program, (const float (*)[4])soft_device.constants,
			soft_device.constants_serial))
	{
		BOOL fetched[SOFT_VERTEX_BATCH];

		FINE_PROFILE_BEGIN(_nspire_profile_vertex_fetch);
		fetch_position_shift = draw_position_scaling > 0 ? POSITION_SHIFT : 0;
		for (slot = 0; slot < count; slot++)
			fetched[slot] = vertex_fetch_fixed(batch_indices[slot], batch_fixed_inputs[slot], inputs_read);
		FINE_PROFILE_END(_nspire_profile_vertex_fetch);
		FINE_PROFILE_BEGIN(_nspire_profile_vertex_program);
		soft_vertex_program_run_fixed(program, count, (const long (*)[XGPU_VERTEX_ATTRIBUTE_COUNT][4])batch_fixed_inputs,
			batch_outputs, batch_overflow);
		if (NSPIRE_POSITION_SCALING && !draw_position_scaling)
			position_scaling_try(program, count, fetched, inputs_read);
		fetch_position_shift = 0;
		/* what did not fit, again from floats: together, as one batch (the
interpreter's work is per instruction more than per vertex) */
		{
			static struct soft_vertex_output again_outputs[SOFT_VERTEX_BATCH];
			unsigned char again[SOFT_VERTEX_BATCH];
			unsigned long again_count = 0, k;

			for (slot = 0; slot < count; slot++)
			{
				if (fetched[slot] && !batch_overflow[slot])
					continue;
				memcpy(batch_inputs[again_count], soft_device.attributes, sizeof(batch_inputs[again_count]));
				vertex_fetch(batch_indices[slot], batch_inputs[again_count], inputs_read);
				again[again_count++] = (unsigned char)slot;
			}
			if (again_count)
			{
				soft_vertex_program_run_batch(program, again_count,
					(const float (*)[XGPU_VERTEX_ATTRIBUTE_COUNT][4])batch_inputs,
					(const float (*)[4])soft_device.constants, soft_device.constants_serial, again_outputs);
				for (k = 0; k < again_count; k++)
					batch_outputs[again[k]] = again_outputs[k];
			}
		}
		FINE_PROFILE_END(_nspire_profile_vertex_program);
	}
	else
	{
		FINE_PROFILE_BEGIN(_nspire_profile_vertex_fetch);
		for (slot = 0; slot < count; slot++)
			vertex_fetch(batch_indices[slot], batch_inputs[slot], inputs_read);
		FINE_PROFILE_END(_nspire_profile_vertex_fetch);
		FINE_PROFILE_BEGIN(_nspire_profile_vertex_program);
		soft_vertex_program_run_batch(program, count, (const float (*)[XGPU_VERTEX_ATTRIBUTE_COUNT][4])batch_inputs,
			(const float (*)[4])soft_device.constants, soft_device.constants_serial, batch_outputs);
		FINE_PROFILE_END(_nspire_profile_vertex_program);
	}
	FINE_PROFILE_BEGIN(_nspire_profile_vertex_finish);
	for (slot = 0; slot < count; slot++)
		vertex_finish(batch_indices[slot], &batch_outputs[slot]);
	FINE_PROFILE_END(_nspire_profile_vertex_finish);
	counters.vertices += count;
}

/* a triangle of a two-phase draw: its vertices are needed if it can be seen
(its positions are known: on the screen side of every plane, and facing
the way the draw does not cull) */
/* ---------- repeated passes

A draw of the same triangles as an earlier one this frame, through the same
transform (the level's lightmap pass after its textures), can show only
triangles the earlier one filled: the rest were off the screen, facing away
or behind the depth buffer, which only grows nearer. Each indexed draw's
filled triangles are kept (a bit each, by their order in the draw), and a
later pass that tests depth without writing it skips the others before
transforming their vertices. */

static void pass_memo_begin(D3DPRIMITIVETYPE type, const WORD *indices, unsigned long vertex_count, unsigned long first,
	const void *position_program, BOOL z_test, BOOL z_write, unsigned long cull)
{
	unsigned long transform = 0, positions = 0, index_hash = 0, words, entry;
	int k;

	pass_skip = NULL;
	pass_record = NULL;
	pass_candidate = pass_recording = NULL;
	pass_skip_ordinal = pass_record_ordinal = 0;
	if (!NSPIRE_PASS_MEMO || !indices || target != &low_target)
		return;
	/* (the positions: what the program trimmed to them reads) */
	transform = soft_vertex_program_key(position_program, (const float (*)[4])soft_device.constants);
	if (!transform)
		return;
	for (k = 0; k < (int)draw_source.shader->element_count; k++)
	{
		if (draw_source.shader->elements[k].reg == 0)
			positions = soft_device.streams[draw_source.shader->elements[k].stream].data +
				draw_source.shader->elements[k].offset;
	}
	if (!positions)
		return;
	for (k = 0; k < (int)vertex_count; k++)
		index_hash = (index_hash << 7 | index_hash >> 25) ^ indices[k];
	for (entry = 0; entry < pass_memo_count; entry++)
	{
		struct pass_memo *memo = &pass_memos[entry];

		if (memo->positions == positions && memo->indices == index_hash && memo->vertex_count == vertex_count && memo->first == first &&
			memo->type == (unsigned long)type && memo->transform == transform)
		{
			if (z_test && !z_write && memo->cull == cull && memo->sampled)
				pass_candidate = memo;
			return;
		}
	}
	words = (vertex_count + 31) >> 5;
	if (pass_memo_count == PASS_MEMO_ENTRIES || pass_memo_words_used + words > PASS_MEMO_WORDS)
		return;
	{
		struct pass_memo *memo = &pass_memos[pass_memo_count++];

		memo->positions = positions;
		memo->indices = index_hash;
		memo->vertex_count = vertex_count;
		memo->first = first;
		memo->type = (unsigned long)type;
		memo->cull = cull;
		memo->transform = transform;
		memo->bits = pass_memo_words + pass_memo_words_used;
		pass_memo_words_used += words;
		memset(memo->bits, 0, words * sizeof(unsigned long));
		memo->sampled = FALSE;
		pass_record = memo->bits;
		pass_recording = memo;
	}
}

/* whether the earlier pass filled the triangle (always when there is none) */
static __inline__ BOOL pass_shows(unsigned long ordinal)
{
	return !pass_skip || (pass_skip[ordinal >> 5] >> (ordinal & 31) & 1);
}

static __inline__ void pass_mark_needed(unsigned long i0, unsigned long i1, unsigned long i2)
{
	if (pass_shows(pass_skip_ordinal++))
		needed_serials[i0] = needed_serials[i1] = needed_serials[i2] = projection_serial;
}

static __inline__ void triangle_needs(unsigned long i0, unsigned long i1, unsigned long i2)
{
	if (!pass_shows(pass_skip_ordinal++))
		return;
	unsigned short o0 = vertex_outcodes[i0], o1 = vertex_outcodes[i1], o2 = vertex_outcodes[i2];

	if (o0 & o1 & o2)
		return;
	if (!((o0 | o1 | o2) & OUTCODE_CLIP_MASK) && draw_cull_mode != D3DCULL_NONE)
	{
		/* the screen area's sign, from the clip positions (w is positive) */
		const float *a = vertex_store[i0].clip, *b = vertex_store[i1].clip, *c = vertex_store[i2].clip;
		float determinant = a[0] * (b[1] * c[3] - c[1] * b[3]) - a[1] * (b[0] * c[3] - c[0] * b[3]) +
			a[3] * (b[0] * c[1] - c[0] * b[1]);
		float area = determinant * screen_scale[0] * screen_scale[1];

		if ((draw_cull_mode == D3DCULL_CCW && area < 0.0f) || (draw_cull_mode == D3DCULL_CW && area > 0.0f))
			return;
	}
	if (occlusion_ready && !((o0 | o1 | o2) & 3))
	{
		/* behind the depth buffer all over its rectangle: hidden */
		const struct occlusion_point *a = &occlusion_points[i0], *b = &occlusion_points[i1], *c = &occlusion_points[i2];
		float min_x = a->x, max_x = a->x, min_y = a->y, max_y = a->y, min_z = a->z;
		long x0, x1, y0, y1, x, y;
		unsigned long nearest;
		BOOL hidden = TRUE;

		if (b->x < min_x) min_x = b->x;
		if (b->x > max_x) max_x = b->x;
		if (c->x < min_x) min_x = c->x;
		if (c->x > max_x) max_x = c->x;
		if (b->y < min_y) min_y = b->y;
		if (b->y > max_y) max_y = b->y;
		if (c->y < min_y) min_y = c->y;
		if (c->y > max_y) max_y = c->y;
		if (b->z < min_z) min_z = b->z;
		if (c->z < min_z) min_z = c->z;
		x0 = (long)min_x - 1;
		x1 = (long)max_x + 2;
		y0 = (long)min_y - 1;
		y1 = (long)max_y + 2;
		if (x0 < 0) x0 = 0;
		if (y0 < 0) y0 = 0;
		if (x1 > LOW_WIDTH) x1 = LOW_WIDTH;
		if (y1 > LOW_HEIGHT) y1 = LOW_HEIGHT;
		nearest = min_z <= 0.0f ? 0 : min_z >= 65535.0f ? 65535 : (unsigned long)min_z;
		for (y = y0; y < y1 && hidden; y++)
		{
			const unsigned short *row = depth_buffer + y * LOW_WIDTH;

			for (x = x0; x < x1; x++)
			{
				if (row[x] >= nearest)
				{
					hidden = FALSE;
					break;
				}
			}
		}
		if (hidden && x0 < x1 && y0 < y1)
		{
			counters.hidden_triangles++;
			return;
		}
	}
	needed_serials[i0] = needed_serials[i1] = needed_serials[i2] = projection_serial;
}

/* whether every depth in the low target's rectangle (x1, y1 past its end) is
nearer than min_z */
static BOOL depth_rectangle_hidden(long x0, long x1, long y0, long y1, float min_z)
{
	unsigned long nearest;
	long x, y;

	if (min_z <= 0.0f)
		return FALSE;
	nearest = min_z >= 65535.0f ? 65535 : (unsigned long)min_z;
	for (y = y0; y < y1; y++)
	{
		const unsigned short *row = depth_buffer + y * LOW_WIDTH;

		for (x = x0; x < x1; x++)
		{
			if (row[x] >= nearest)
				return FALSE;
		}
	}
	return TRUE;
}

/* whether a sphere in the world is behind what the low target's depth
already holds: the corners of its box through the world-to-clip transform
the programs use (vertex constants -96 to -93, rasterizer_set_frustum_z),
for the engine to skip an object's models (source/render/render_objects.c) */
BOOL soft_rasterizer_sphere_hidden(const float center[3], float radius)
{
	const float (*m)[4] = (const float (*)[4])soft_device.constants[XGPU_VERTEX_CONSTANT_BIAS - 96];
	float base[4], step[3][4];
	float min_x = 1.0e9f, max_x = -1.0e9f, min_y = 1.0e9f, max_y = -1.0e9f, min_z;
	float z_scale = soft_device.depth_scale > 0.0f ? soft_device.depth_scale : 65535.0f;
	float z_normalize = 65535.0f / z_scale;
	long x0, x1, y0, y1;
	int k, corner;

	if (resolved || !depth_written || !depth_buffer)
		return FALSE;
	counters.objects_tested++;
	screen_constants();
	for (k = 0; k < 4; k++)
	{
		base[k] = m[k][0] * center[0] + m[k][1] * center[1] + m[k][2] * center[2] + m[k][3];
		step[0][k] = m[k][0] * radius;
		step[1][k] = m[k][1] * radius;
		step[2][k] = m[k][2] * radius;
	}
	for (corner = 0; corner < 8; corner++)
	{
		float c[4], inverse_w, sx, sy;

		for (k = 0; k < 4; k++)
		{
			c[k] = base[k];
			c[k] += corner & 1 ? step[0][k] : -step[0][k];
			c[k] += corner & 2 ? step[1][k] : -step[1][k];
			c[k] += corner & 4 ? step[2][k] : -step[2][k];
		}
		/* (reaching past the near plane: no rectangle bounds it) */
		if (vertex_outcode(c) & 3)
			return FALSE;
		inverse_w = 1.0f / c[3];
		sx = (c[0] * screen_scale[0] * inverse_w + screen_offset[0] + 0.5f) * 0.25f;
		sy = (c[1] * screen_scale[1] * inverse_w + screen_offset[1] + 0.5f) * 0.25f;
		if (sx < min_x) min_x = sx;
		if (sx > max_x) max_x = sx;
		if (sy < min_y) min_y = sy;
		if (sy > max_y) max_y = sy;
	}
	/* the depth: the sphere's point nearest the camera (vertex constant
	-92), not the box's nearest corner, which reaches through a thin hull */
	{
		const float *camera = soft_device.constants[XGPU_VERTEX_CONSTANT_BIAS - 92];
		float toward[3], length_squared = 0.0f, scale, c[4];

		for (k = 0; k < 3; k++)
		{
			toward[k] = center[k] - camera[k];
			length_squared += toward[k] * toward[k];
		}
		if (length_squared <= radius * radius)
			return FALSE;
		scale = radius / sqrtf(length_squared);
		for (k = 0; k < 4; k++)
			c[k] = base[k] - (m[k][0] * toward[0] + m[k][1] * toward[1] + m[k][2] * toward[2]) * scale;
		if (vertex_outcode(c) & 3)
			return FALSE;
		min_z = (c[2] * screen_scale[2] / c[3] + screen_offset[2]) * z_normalize;
	}
	x0 = (long)min_x - 1;
	x1 = (long)max_x + 2;
	y0 = (long)min_y - 1;
	y1 = (long)max_y + 2;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > LOW_WIDTH) x1 = LOW_WIDTH;
	if (y1 > LOW_HEIGHT) y1 = LOW_HEIGHT;
	/* (wholly off the target: the engine's own culling decides) */
	if (x0 >= x1 || y0 >= y1)
		return FALSE;
	if (!depth_rectangle_hidden(x0, x1, y0, y1, min_z))
	{
		if (log_draws)
		{
			/* why not: how much of its rectangle is farther than it */
			long x, y, farther = 0, farthest = 0;

			for (y = y0; y < y1; y++)
			{
				for (x = x0; x < x1; x++)
				{
					long depth = depth_buffer[y * LOW_WIDTH + x];

					if (depth >= (long)min_z)
					{
						farther++;
						if (depth - (long)min_z > farthest)
							farthest = depth - (long)min_z;
					}
				}
			}
			nspire_log("  sphere seen: rectangle %ld-%ld x %ld-%ld, depth %ld; %ld farther depths, by up to %ld",
				x0, x1, y0, y1, (long)min_z, farther, farthest);
		}
		return FALSE;
	}
	counters.objects_hidden++;
	return TRUE;
}

/* whether all of a two-phase draw is behind what the depth buffer holds:
its corners' screen rectangle, and the nearest of their depths (a triangle
is no nearer than its nearest corner), against every depth in the
rectangle; never for a draw that reaches past the near plane */
static BOOL draw_hidden(const WORD *indices, unsigned long vertex_count, unsigned long minimum)
{
	float min_x = 1.0e9f, max_x = -1.0e9f, min_y = 1.0e9f, max_y = -1.0e9f, min_z = 1.0e9f;
	unsigned long position;
	long x0, x1, y0, y1;
	unsigned short all_outside = 0xFFFF;

	BOOL near = FALSE;

	occlusion_ready = FALSE;
	if (!draw_occlusion_test || !vertex_count)
		return FALSE;
	if (!occlusion_points)
		return FALSE;
	for (position = 0; position < vertex_count; position++)
	{
		unsigned long index = indices ? (unsigned long)indices[position] - minimum : position;
		const float *c = vertex_store[index].clip;
		float inverse_w, sx, sy, sz;

		all_outside &= vertex_outcodes[index];
		/* (past the near plane or behind the eye: no rectangle bounds it) */
		if (vertex_outcodes[index] & 3)
		{
			near = TRUE;
			continue;
		}
		inverse_w = 1.0f / c[3];
		sx = (c[0] * screen_scale[0] * inverse_w + screen_offset[0] + 0.5f) * 0.25f;
		sy = (c[1] * screen_scale[1] * inverse_w + screen_offset[1] + 0.5f) * 0.25f;
		sz = (c[2] * screen_scale[2] * inverse_w + screen_offset[2]) * draw_z_normalize;
		occlusion_points[index].x = sx;
		occlusion_points[index].y = sy;
		occlusion_points[index].z = sz;
		if (sx < min_x) min_x = sx;
		if (sx > max_x) max_x = sx;
		if (sy < min_y) min_y = sy;
		if (sy > max_y) max_y = sy;
		if (sz < min_z) min_z = sz;
	}
	if (all_outside)
		return TRUE;
	occlusion_ready = TRUE;
	if (near)
		return FALSE;
	x0 = (long)min_x - 1;
	x1 = (long)max_x + 2;
	y0 = (long)min_y - 1;
	y1 = (long)max_y + 2;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > LOW_WIDTH) x1 = LOW_WIDTH;
	if (y1 > LOW_HEIGHT) y1 = LOW_HEIGHT;
	return depth_rectangle_hidden(x0, x1, y0, y1, min_z);
}

/* the vertices among the draw's (vertex_count of them, through indices when
there are some) that select() takes, run through the program not already */
#define TRANSFORM_PASS(program, wanted_test, done_serials) \
	do \
	{ \
		unsigned long position, filled = 0; \
		for (position = 0; position < vertex_count; position++) \
		{ \
			unsigned long index = indices ? (unsigned long)indices[position] - minimum : position; \
			if (vertex_serials[index] == vertex_serial || done_serials[index] == vertex_serial || !(wanted_test)) \
				continue; \
			done_serials[index] = vertex_serial; \
			batch_indices[filled++] = index; \
			if (filled == SOFT_VERTEX_BATCH) \
			{ \
				batch_run(program, filled); \
				filled = 0; \
			} \
		} \
		if (filled) \
			batch_run(program, filled); \
	} \
	while (0)

/* each triangle of the draw, in order, through EACH(i0, i1, i2) (with
VERTEX(i) defined) */
#define EACH_TRIANGLE() \
	do \
	{ \
		switch (type) \
		{ \
		case D3DPT_TRIANGLELIST: \
			for (index = 0; index + 2 < vertex_count; index += 3) \
				EACH(VERTEX(index), VERTEX(index + 1), VERTEX(index + 2)); \
			break; \
		case D3DPT_TRIANGLESTRIP: \
			for (index = 0; index + 2 < vertex_count; index++) \
			{ \
				if (index & 1) \
					EACH(VERTEX(index + 1), VERTEX(index), VERTEX(index + 2)); \
				else \
					EACH(VERTEX(index), VERTEX(index + 1), VERTEX(index + 2)); \
			} \
			break; \
		case D3DPT_TRIANGLEFAN: \
		case D3DPT_POLYGON: \
			for (index = 1; index + 1 < vertex_count; index++) \
				EACH(VERTEX(0), VERTEX(index), VERTEX(index + 1)); \
			break; \
		case D3DPT_QUADLIST: \
			for (index = 0; index + 3 < vertex_count; index += 4) \
			{ \
				EACH(VERTEX(index), VERTEX(index + 1), VERTEX(index + 2)); \
				EACH(VERTEX(index), VERTEX(index + 2), VERTEX(index + 3)); \
			} \
			break; \
		case D3DPT_QUADSTRIP: \
			for (index = 0; index + 3 < vertex_count; index += 2) \
			{ \
				EACH(VERTEX(index), VERTEX(index + 1), VERTEX(index + 3)); \
				EACH(VERTEX(index), VERTEX(index + 3), VERTEX(index + 2)); \
			} \
			break; \
		default: \
			break; \
		} \
	} \
	while (0)

/* every vertex the draw's primitives use, transformed if it is not yet. A
two-phase draw (draw_source.position_program) first finds every position
with the program trimmed to it, then runs the whole program only for the
vertices of triangles that can be seen. */
static void transform_draw_vertices(D3DPRIMITIVETYPE type, const WORD *indices, unsigned long vertex_count,
	unsigned long minimum)
{
	unsigned long slot, index;

	NSPIRE_PROFILE_BEGIN(_nspire_profile_vertices);
	/* the attributes no stream gives: the device's defaults */
	if (!draw_source.immediate_vertices)
	{
		int reg, k;

		for (slot = 0; slot < SOFT_VERTEX_BATCH; slot++)
			memcpy(batch_inputs[slot], soft_device.attributes, sizeof(batch_inputs[slot]));
		fixed_defaults_overflow = FALSE;
		for (reg = 0; reg < XGPU_VERTEX_ATTRIBUTE_COUNT; reg++)
		{
			for (k = 0; k < 4; k++)
			{
				fixed_defaults[reg][k] = fixed16(soft_device.attributes[reg][k]);
				if (fixed_defaults[reg][k] == 0x7FFFFFFFL || fixed_defaults[reg][k] == -0x7FFFFFFFL)
					fixed_defaults_overflow = TRUE;
			}
		}
		for (slot = 0; slot < SOFT_VERTEX_BATCH; slot++)
			memcpy(batch_fixed_inputs[slot], fixed_defaults, sizeof(fixed_defaults));
	}
	if (draw_source.lit_program && vertex_count)
	{
		/* the draw's colour: its first vertex, lit */
		unsigned long first = indices ? (unsigned long)indices[0] - minimum : 0;
		int k;

		vertex_fetch(first, batch_inputs[0], ~0UL);
		soft_vertex_program_run_batch(draw_source.lit_program, 1,
			(const float (*)[XGPU_VERTEX_ATTRIBUTE_COUNT][4])batch_inputs, (const float (*)[4])soft_device.constants,
			soft_device.constants_serial, batch_outputs);
		for (k = 0; k < 4; k++)
			draw_source.flat_color[k] = unit_clamp(batch_outputs[0].diffuse[k]);
		if (!draw_source.immediate_vertices)
			memcpy(batch_inputs[0], soft_device.attributes, sizeof(batch_inputs[0]));
	}
	finish_outcodes = TRUE;
	if (pass_candidate && vertex_count)
	{
		/* the same transform as the earlier pass: its first vertex lands
		where it did */
		const void *program = draw_source.position_program ? draw_source.position_program : draw_source.program;

		batch_indices[0] = indices ? (unsigned long)indices[0] - minimum : 0;
		batch_run(program, 1);
		if (!memcmp(vertex_store[batch_indices[0]].clip, pass_candidate->sample, sizeof(pass_candidate->sample)))
		{
			pass_skip = pass_candidate->bits;
			counters.repeated_passes++;
		}
	}
	if (!draw_source.position_program && pass_skip)
	{
#define VERTEX(i) (indices ? (unsigned long)indices[i] - minimum : (i))
#define EACH pass_mark_needed
		EACH_TRIANGLE();
#undef EACH
#undef VERTEX
		TRANSFORM_PASS(draw_source.program, needed_serials[index] == projection_serial, vertex_serials);
		NSPIRE_PROFILE_END(_nspire_profile_vertices);
		return;
	}
	if (!draw_source.position_program)
	{
		TRANSFORM_PASS(draw_source.program, TRUE, vertex_serials);
		NSPIRE_PROFILE_END(_nspire_profile_vertices);
		return;
	}

	if (pass_skip)
	{
		/* positions only for the triangles the earlier pass filled (which
		decided too what the depth hides); a new serial then for those
		triangle_needs keeps */
#define VERTEX(i) (indices ? (unsigned long)indices[i] - minimum : (i))
#define EACH pass_mark_needed
		EACH_TRIANGLE();
#undef EACH
#undef VERTEX
		TRANSFORM_PASS(draw_source.position_program, needed_serials[index] == projection_serial, vertex_position_serials);
		projection_serial++;
		occlusion_ready = FALSE;
		pass_skip_ordinal = 0;
	}
	else
		TRANSFORM_PASS(draw_source.position_program, TRUE, vertex_position_serials);
	if (!pass_skip && draw_hidden(indices, vertex_count, minimum))
	{
		/* (its triangles, with no vertex wholly transformed, are skipped) */
		counters.hidden_draws++;
		NSPIRE_PROFILE_END(_nspire_profile_vertices);
		return;
	}

#define VERTEX(i) (indices ? (unsigned long)indices[i] - minimum : (i))
#define EACH triangle_needs
	EACH_TRIANGLE();
#undef EACH
#undef VERTEX

	/* (positions only for vertices the first pass did not find, which
	cannot happen here: every needed vertex had its position found) */
	finish_outcodes = FALSE;
	TRANSFORM_PASS(draw_source.program, needed_serials[index] == projection_serial, vertex_serials);
	finish_outcodes = TRUE;
	NSPIRE_PROFILE_END(_nspire_profile_vertices);
}

/* a vertex of the draw, transformed by transform_draw_vertices */
static __inline__ const struct clip_vertex *transformed_vertex(unsigned long index)
{
	return &vertex_store[index];
}

/* ---------- clipping, in clip space

Each plane is a linear function of the clip position that is non-negative
inside: the near plane (screen z >= 0), w above zero, and the guard band
around the screen, which keeps the fixed-point rasterizer's numbers small. */

static float plane_distance(int plane, const float *c)
{
	switch (plane)
	{
	case 0: return c[2] * screen_scale[2] + screen_offset[2] * c[3];
	case 1: return c[3] - MINIMUM_W;
	case 2: return c[0] * screen_scale[0] + (screen_offset[0] + GUARD_BAND) * c[3];
	case 3: return -c[0] * screen_scale[0] + (640.0f + GUARD_BAND - screen_offset[0]) * c[3];
	case 4: return c[1] * screen_scale[1] + (screen_offset[1] + GUARD_BAND) * c[3];
	default: return -c[1] * screen_scale[1] + (480.0f + GUARD_BAND - screen_offset[1]) * c[3];
	}
}

/* 1 when a float is below zero (or negative zero), from its sign bit */
static __inline__ __attribute__((always_inline)) unsigned negative(float value)
{
	union
	{
		float f;
		unsigned long u;
	} bits;

	bits.f = value;
	return (unsigned)(bits.u >> 31);
}

/* plane_distance's tests and the screen's edges, sharing their terms */
static unsigned short vertex_outcode(const float *c)
{
	float x = c[0] * screen_scale[0], y = c[1] * screen_scale[1], w = c[3];
	float left = x + screen_offset[0] * w, right = (640.0f - screen_offset[0]) * w - x;
	float top = y + screen_offset[1] * w, bottom = (480.0f - screen_offset[1]) * w - y;
	float guard = GUARD_BAND * w;

	return (unsigned short)(negative(c[2] * screen_scale[2] + screen_offset[2] * w) |
		(negative(w - MINIMUM_W) << 1) |
		(negative(left + guard) << 2) | (negative(right + guard) << 3) |
		(negative(top + guard) << 4) | (negative(bottom + guard) << 5) |
		(negative(left) << 6) | (negative(right) << 7) | (negative(top) << 8) | (negative(bottom) << 9));
}

/* vertex_outcode's tests on a 16.16 clip position, in 64-bit integers */
static unsigned short vertex_outcode_fixed(const long *c)
{
	long w = c[3];
	long long x = (long long)c[0] * outcode_scale[0], y = (long long)c[1] * outcode_scale[1];
	long long left = x + (long long)outcode_offset[0] * w, right = (long long)outcode_far[0] * w - x;
	long long top = y + (long long)outcode_offset[1] * w, bottom = (long long)outcode_far[1] * w - y;
	long long guard_x = (long long)outcode_guard[0] * w, guard_y = (long long)outcode_guard[1] * w;
	long long z = (long long)c[2] * outcode_scale[2] + (long long)outcode_offset[2] * w;

	/* (w below MINIMUM_W is w below one 16.16 step) */
	return (unsigned short)((z < 0) | ((w < 1) << 1) |
		((left + guard_x < 0) << 2) | ((right + guard_x < 0) << 3) |
		((top + guard_y < 0) << 4) | ((bottom + guard_y < 0) << 5) |
		((left < 0) << 6) | ((right < 0) << 7) | ((top < 0) << 8) | ((bottom < 0) << 9));
}

static void lerp_vertex(const struct clip_vertex *a, const struct clip_vertex *b, float t, struct clip_vertex *out)
{
	float *o = (float *)out;
	const float *pa = (const float *)a, *pb = (const float *)b;
	unsigned long index;

	for (index = 0; index < offsetof(struct clip_vertex, texture_fixed) / sizeof(float); index++)
		o[index] = pa[index] + (pb[index] - pa[index]) * t;
	out->texture_is_fixed = 0;
	out->clip_is_fixed = 0;
}

/* a vertex's u and v as floats, for clipping */
static void clip_vertex_float_textures(struct clip_vertex *vertex)
{
	int k;

	if (!vertex->texture_is_fixed)
		return;
	for (k = 0; k < 4; k++)
	{
		vertex->texture[k][0] = (float)vertex->texture_fixed[k][0] * (1.0f / 65536.0f);
		vertex->texture[k][1] = (float)vertex->texture_fixed[k][1] * (1.0f / 65536.0f);
	}
	vertex->texture_is_fixed = 0;
}

/* clips the polygon in place; returns its new vertex count */
static int clip_polygon(struct clip_vertex *polygon, int count)
{
	struct clip_vertex scratch[MAXIMUM_CLIPPED_VERTICES];
	int plane;

	for (plane = 0; plane < 6 && count >= 3; plane++)
	{
		float distances[MAXIMUM_CLIPPED_VERTICES];
		int index, out = 0, all_inside = 1;

		for (index = 0; index < count; index++)
		{
			distances[index] = plane_distance(plane, polygon[index].clip);
			if (distances[index] < 0.0f)
				all_inside = 0;
		}
		if (all_inside)
			continue;
		for (index = 0; index < count && out < MAXIMUM_CLIPPED_VERTICES - 1; index++)
		{
			int next = (index + 1) % count;

			if (distances[index] >= 0.0f)
				scratch[out++] = polygon[index];
			if ((distances[index] >= 0.0f) != (distances[next] >= 0.0f))
			{
				float t = distances[index] / (distances[index] - distances[next]);

				lerp_vertex(&polygon[index], &polygon[next], t, &scratch[out++]);
			}
		}
		memcpy(polygon, scratch, out * sizeof(*polygon));
		count = out;
	}
	return count;
}

/* ---------- textures */

#define MAXIMUM_LEVELS 9

struct sampler
{
	BOOL active;
	/* A4R4G4B4 texels, swizzled, of the level the triangle being drawn
	uses (sampler_select_level); NULL reads white */
	const unsigned short *texels;
	unsigned long width_mask, height_mask;
	long width, height;
	const unsigned short *swizzle_x, *swizzle_y;
	/* coordinates are in texels of the first level: shifted right this much
	for the one in use */
	int shift;
	/* the level sampler_select_level chose last (-1: none yet this draw) */
	int selected_level;
	/* the first level's size, and where each level starts */
	long base_width, base_height;
	int levels;
	const unsigned short *level_texels[MAXIMUM_LEVELS];
	BOOL clamp_u, clamp_v;
	/* outside the texture, the border colour (D3DTADDRESS_BORDER) */
	BOOL border_u, border_v;
	long border[4];
	unsigned long mode;
};

/* the swizzle of a texture of each size: for coordinate x (or y), the
offset of its bits among the texel index's (port/linux/src/xbox_textures.c);
made when first needed, by log2 of the width and height */
static unsigned short *swizzle_tables[9][9][2];

static struct sampler samplers[4];

static unsigned long spread(unsigned long mask, unsigned long value)
{
	unsigned long result = 0, bit = 1;

	while (value && bit)
	{
		if (mask & bit)
		{
			if (value & 1)
				result |= bit;
			value >>= 1;
		}
		bit <<= 1;
	}
	return result;
}

/* the swizzle tables of a texture size, made when first asked for */
static BOOL swizzle_tables_get(long width, long height, const unsigned short **x_table,
	const unsigned short **y_table)
{
	int width_log = 0, height_log = 0;
	unsigned short **tables;

	while ((1L << width_log) < width && width_log < 8) width_log++;
	while ((1L << height_log) < height && height_log < 8) height_log++;
	tables = swizzle_tables[width_log][height_log];
	if (!tables[0])
	{
		unsigned long mask_x = 0, mask_y = 0, bit = 1, mask_bit = 1;
		long index;
		unsigned short *x = malloc(width * sizeof(unsigned short)), *y = malloc(height * sizeof(unsigned short));

		if (!x || !y)
		{
			free(x);
			free(y);
			return FALSE;
		}
		do
		{
			int more = 0;

			if (bit < (unsigned long)width) { mask_x |= mask_bit; mask_bit <<= 1; more = 1; }
			if (bit < (unsigned long)height) { mask_y |= mask_bit; mask_bit <<= 1; more = 1; }
			bit <<= 1;
			if (!more)
				break;
		}
		while (1);
		for (index = 0; index < width; index++)
			x[index] = (unsigned short)spread(mask_x, (unsigned long)index);
		for (index = 0; index < height; index++)
			y[index] = (unsigned short)spread(mask_y, (unsigned long)index);
		tables[0] = x;
		tables[1] = y;
	}
	*x_table = tables[0];
	*y_table = tables[1];
	return TRUE;
}

/* the level a triangle samples: the one whose texels are about a pixel
apart, from how far its coordinates move per pixel (16.16, in the first
level's texels) */
static void sampler_select_level(struct sampler *sampler, unsigned long texels_per_pixel)
{
	int level = 0;

	while (level + 1 < sampler->levels && texels_per_pixel >= 0x18000)
	{
		texels_per_pixel >>= 1;
		level++;
	}
	/* (the level the last triangle chose, most often) */
	if (level == sampler->selected_level)
		return;
	sampler->selected_level = level;
	sampler->shift = level;
	sampler->width = sampler->base_width >> level;
	sampler->height = sampler->base_height >> level;
	if (sampler->width < 1) sampler->width = 1;
	if (sampler->height < 1) sampler->height = 1;
	sampler->width_mask = (unsigned long)sampler->width - 1;
	sampler->height_mask = (unsigned long)sampler->height - 1;
	sampler->texels = sampler->level_texels[level];
	if (!swizzle_tables_get(sampler->width, sampler->height, &sampler->swizzle_x, &sampler->swizzle_y))
		sampler->texels = NULL;
}

static void sampler_prepare(int stage, BOOL used)
{
	struct sampler *sampler = &samplers[stage];
	const DWORD *resource = (const DWORD *)soft_device.textures[stage];
	struct xgpu_texture_description description;
	unsigned long mask_x = 0, mask_y = 0, bit = 1, mask_bit = 1;
	long x;

	sampler->active = used;
	sampler->texels = NULL;
	sampler->selected_level = -1;
	sampler->mode = (D3D__RenderState[D3DRS_PSTEXTUREMODES] >> (5 * stage)) & 0x1F;
	if (!used || !resource || !resource[1])
		return;
	xgpu_texture_describe(resource[3], resource[4], &description);
	if (description.format != D3DFMT_A4R4G4B4 || description.linear || description.width > 256 ||
		description.height > 256 || description.depth > 1)
		return;
	sampler->texels = (const unsigned short *)PLATFORM_PHYSICAL_TO_VIRTUAL(resource[1]);
	sampler->base_width = (long)description.width;
	sampler->base_height = (long)description.height;
	sampler->levels = description.levels < 1 ? 1 : description.levels > MAXIMUM_LEVELS ? MAXIMUM_LEVELS :
		(int)description.levels;
	{
		int level;
		unsigned long offset = 0;

		for (level = 0; level < sampler->levels; level++)
		{
			unsigned long width = description.width >> level, height = description.height >> level;

			sampler->level_texels[level] = sampler->texels + offset;
			offset += (width ? width : 1) * (height ? height : 1);
		}
	}
	sampler->clamp_u = D3D__TextureState[stage][D3DTSS_ADDRESSU] >= D3DTADDRESS_CLAMP;
	sampler->clamp_v = D3D__TextureState[stage][D3DTSS_ADDRESSV] >= D3DTADDRESS_CLAMP;
	sampler->border_u = D3D__TextureState[stage][D3DTSS_ADDRESSU] == D3DTADDRESS_BORDER;
	sampler->border_v = D3D__TextureState[stage][D3DTSS_ADDRESSV] == D3DTADDRESS_BORDER;
	if (sampler->border_u || sampler->border_v)
		color_to_fixed(D3D__TextureState[stage][D3DTSS_BORDERCOLOR], sampler->border);
	sampler_select_level(sampler, 0);
}

/* texel at 16.16 texel coordinates, as r, g, b, a in 0..256 */
static void sample(const struct sampler *sampler, long u, long v, long out[4])
{
	unsigned long texel;
	long x = (u >> 16) >> sampler->shift, y = (v >> 16) >> sampler->shift;

	if (!sampler->texels)
	{
		out[0] = out[1] = out[2] = out[3] = ONE;
		return;
	}
	if ((sampler->border_u && (x < 0 || x >= sampler->width)) || (sampler->border_v && (y < 0 || y >= sampler->height)))
	{
		out[0] = sampler->border[0];
		out[1] = sampler->border[1];
		out[2] = sampler->border[2];
		out[3] = sampler->border[3];
		return;
	}
	if (sampler->clamp_u)
		x = x < 0 ? 0 : x >= sampler->width ? sampler->width - 1 : x;
	else
		x &= (long)sampler->width_mask;
	if (sampler->clamp_v)
		y = y < 0 ? 0 : y >= sampler->height ? sampler->height - 1 : y;
	else
		y &= (long)sampler->height_mask;
	texel = sampler->texels[sampler->swizzle_x[x] | sampler->swizzle_y[y]];
	/* a nibble n is n * 17 of 255, near enough n * 17 + n / 8 of 256 */
	out[3] = ((texel >> 12) & 15) * 17 + (((texel >> 12) & 15) >> 3);
	out[0] = ((texel >> 8) & 15) * 17 + (((texel >> 8) & 15) >> 3);
	out[1] = ((texel >> 4) & 15) * 17 + (((texel >> 4) & 15) >> 3);
	out[2] = (texel & 15) * 17 + ((texel & 15) >> 3);
}

/* ---------- the register combiners, in 8.8 fixed point */

struct combiner_input
{
	unsigned char reg, alpha, mapping;
};

struct combiner_portion
{
	struct combiner_input inputs[4];
	unsigned char ab, cd, sum;
	unsigned char ab_dot, cd_dot, mux;
	unsigned char mapping;
	unsigned char ab_dot_to_alpha, cd_dot_to_alpha;
};

struct combiner_stage
{
	struct combiner_portion rgb, alpha;
	long c0[4], c1[4];
};

struct combiner_program
{
	int stage_count;
	struct combiner_stage stages[8];
	BOOL mux_msb;
	BOOL has_final;
	struct combiner_input final_inputs[7];
	unsigned char final_settings;
	long final_c0[4], final_c1[4];
	/* texture stages and registers the combiners read, and registers they
	write */
	BOOL uses_texture[4];
	BOOL uses_register[NUMBER_OF_REGISTERS];
	unsigned long written_registers;
};

static struct combiner_program combiners;

static void color_to_fixed(DWORD color, long out[4])
{
	out[0] = (long)((color >> 16) & 0xFF) + (long)(((color >> 16) & 0xFF) >> 7);
	out[1] = (long)((color >> 8) & 0xFF) + (long)(((color >> 8) & 0xFF) >> 7);
	out[2] = (long)(color & 0xFF) + (long)((color & 0xFF) >> 7);
	out[3] = (long)((color >> 24) & 0xFF) + (long)(((color >> 24) & 0xFF) >> 7);
}

static void decode_input(unsigned long byte, struct combiner_input *input)
{
	input->reg = (unsigned char)(byte & 0x0F);
	input->alpha = (byte & 0x10) != 0;
	input->mapping = (unsigned char)(byte & 0xE0);
	combiners.uses_register[input->reg] = TRUE;
	if (input->reg >= _register_t0 && input->reg <= _register_t3)
		combiners.uses_texture[input->reg - _register_t0] = TRUE;
}

static void decode_portion(DWORD inputs, DWORD outputs, struct combiner_portion *portion)
{
	unsigned long flags = outputs >> 12;

	decode_input(inputs >> 24, &portion->inputs[0]);
	decode_input(inputs >> 16, &portion->inputs[1]);
	decode_input(inputs >> 8, &portion->inputs[2]);
	decode_input(inputs, &portion->inputs[3]);
	portion->cd = (unsigned char)(outputs & 0x0F);
	portion->ab = (unsigned char)((outputs >> 4) & 0x0F);
	portion->sum = (unsigned char)((outputs >> 8) & 0x0F);
	portion->cd_dot = (flags & 0x01) != 0;
	portion->ab_dot = (flags & 0x02) != 0;
	portion->mux = (flags & 0x04) != 0;
	portion->mapping = (unsigned char)(flags & 0x38);
	portion->cd_dot_to_alpha = (flags & 0x40) != 0;
	portion->ab_dot_to_alpha = (flags & 0x80) != 0;
	combiners.written_registers |= (1UL << portion->ab) | (1UL << portion->cd) | (1UL << portion->sum);
}

static void combiners_compile(void);

/* the render states the combiners are made of: when a draw's are the last
draw's (a run of text, one draw a character), what was made stands */
#define COMBINER_STATE_WORDS (5 + 6 * 8)
static DWORD combiner_state_last[COMBINER_STATE_WORDS];
static BOOL combiner_state_valid;

static BOOL combiner_state_unchanged(void)
{
	DWORD now[COMBINER_STATE_WORDS];
	int stage, index = 0;

	now[index++] = D3D__RenderState[D3DRS_PSCOMBINERCOUNT];
	now[index++] = D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSABCD];
	now[index++] = D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSEFG];
	now[index++] = D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT0];
	now[index++] = D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT1];
	for (stage = 0; stage < 8; stage++)
	{
		now[index++] = D3D__RenderState[D3DRS_PSRGBINPUTS0 + stage];
		now[index++] = D3D__RenderState[D3DRS_PSRGBOUTPUTS0 + stage];
		now[index++] = D3D__RenderState[D3DRS_PSALPHAINPUTS0 + stage];
		now[index++] = D3D__RenderState[D3DRS_PSALPHAOUTPUTS0 + stage];
		now[index++] = D3D__RenderState[D3DRS_PSCONSTANT0_0 + stage];
		now[index++] = D3D__RenderState[D3DRS_PSCONSTANT1_0 + stage];
	}
	if (combiner_state_valid && !memcmp(now, combiner_state_last, sizeof(now)))
		return TRUE;
	memcpy(combiner_state_last, now, sizeof(now));
	combiner_state_valid = TRUE;
	return FALSE;
}

static BOOL combiners_prepare(void)
{
	DWORD count_state = D3D__RenderState[D3DRS_PSCOMBINERCOUNT];
	BOOL unique_c0 = (count_state & 0x1000) != 0, unique_c1 = (count_state & 0x10000) != 0;
	DWORD final_abcd = D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSABCD];
	DWORD final_efg = D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSEFG];
	int stage;

	if (combiner_state_unchanged())
		return TRUE;
	memset(&combiners, 0, sizeof(combiners));
	combiners.stage_count = (int)(count_state & 0xFF);
	if (combiners.stage_count > 8)
		combiners.stage_count = 8;
	combiners.mux_msb = (count_state & 0x100) != 0;
	for (stage = 0; stage < combiners.stage_count; stage++)
	{
		struct combiner_stage *s = &combiners.stages[stage];

		decode_portion(D3D__RenderState[D3DRS_PSRGBINPUTS0 + stage], D3D__RenderState[D3DRS_PSRGBOUTPUTS0 + stage], &s->rgb);
		decode_portion(D3D__RenderState[D3DRS_PSALPHAINPUTS0 + stage], D3D__RenderState[D3DRS_PSALPHAOUTPUTS0 + stage],
			&s->alpha);
		color_to_fixed(D3D__RenderState[D3DRS_PSCONSTANT0_0 + (unique_c0 ? stage : 0)], s->c0);
		color_to_fixed(D3D__RenderState[D3DRS_PSCONSTANT1_0 + (unique_c1 ? stage : 0)], s->c1);
	}
	combiners.has_final = final_abcd != 0 || final_efg != 0;
	if (combiners.has_final)
	{
		decode_input(final_abcd >> 24, &combiners.final_inputs[0]);
		decode_input(final_abcd >> 16, &combiners.final_inputs[1]);
		decode_input(final_abcd >> 8, &combiners.final_inputs[2]);
		decode_input(final_abcd, &combiners.final_inputs[3]);
		decode_input(final_efg >> 24, &combiners.final_inputs[4]);
		decode_input(final_efg >> 16, &combiners.final_inputs[5]);
		decode_input(final_efg >> 8, &combiners.final_inputs[6]);
		combiners.final_settings = (unsigned char)(final_efg & 0xFF);
		color_to_fixed(D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT0], combiners.final_c0);
		color_to_fixed(D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT1], combiners.final_c1);
	}
	/* r0 starts with t0's alpha */
	combiners.uses_texture[0] = TRUE;
	/* (the zero register, whatever a portion's unused outputs name, stays
	zero; r0 is set for each pixel anyway) */
	combiners.written_registers &= ~((1UL << _register_zero) | (1UL << _register_r0));
	/* with no final combiner and no stages, the result is r0 */
	combiners_compile();
	return FALSE;
}

static long clamp_unit(long value)
{
	return value < 0 ? 0 : value > ONE ? ONE : value;
}

static long map_input(long value, unsigned char mapping)
{
	long positive = value < 0 ? 0 : value;

	switch (mapping)
	{
	case 0x00: return positive;
	case 0x20: return ONE - clamp_unit(value);
	case 0x40: return 2 * positive - ONE;
	case 0x60: return ONE - 2 * positive;
	case 0x80: return positive - ONE / 2;
	case 0xA0: return ONE / 2 - positive;
	case 0xC0: return value;
	default: return -value;
	}
}

static long map_output(long value, unsigned char mapping)
{
	switch (mapping)
	{
	case 0x08: value -= ONE / 2; break;
	case 0x10: value *= 2; break;
	case 0x18: value = (value - ONE / 2) * 2; break;
	case 0x20: value *= 4; break;
	case 0x30: value /= 2; break;
	default: break;
	}
	return value < -ONE ? -ONE : value > ONE ? ONE : value;
}

/* the portion's input as three (rgb) or one (alpha) channels */
static void read_input(long registers[NUMBER_OF_REGISTERS][4], const struct combiner_input *input, BOOL alpha_portion,
	long out[3])
{
	const long *source = registers[input->reg];
	int channel;

	if (alpha_portion)
	{
		out[0] = map_input(source[input->alpha ? 3 : 2], input->mapping);
		return;
	}
	for (channel = 0; channel < 3; channel++)
		out[channel] = map_input(source[input->alpha ? 3 : channel], input->mapping);
}

static void run_portion(long registers[NUMBER_OF_REGISTERS][4], const struct combiner_portion *portion,
	BOOL alpha_portion, long ab[3], long cd[3], long sum[3])
{
	long a[3], b[3], c[3], d[3];
	int channels = alpha_portion ? 1 : 3, channel;

	read_input(registers, &portion->inputs[0], alpha_portion, a);
	read_input(registers, &portion->inputs[1], alpha_portion, b);
	read_input(registers, &portion->inputs[2], alpha_portion, c);
	read_input(registers, &portion->inputs[3], alpha_portion, d);
	if (!alpha_portion && portion->ab_dot)
		ab[0] = ab[1] = ab[2] = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) >> 8;
	else
		for (channel = 0; channel < channels; channel++)
			ab[channel] = (a[channel] * b[channel]) >> 8;
	if (!alpha_portion && portion->cd_dot)
		cd[0] = cd[1] = cd[2] = (c[0] * d[0] + c[1] * d[1] + c[2] * d[2]) >> 8;
	else
		for (channel = 0; channel < channels; channel++)
			cd[channel] = (c[channel] * d[channel]) >> 8;
	for (channel = 0; channel < channels; channel++)
	{
		if (portion->mux)
		{
			BOOL pick_cd = combiners.mux_msb ? registers[_register_r0][3] >= ONE / 2 : (registers[_register_r0][3] & 1);

			sum[channel] = pick_cd ? cd[channel] : ab[channel];
		}
		else
		{
			sum[channel] = ab[channel] + cd[channel];
		}
		ab[channel] = map_output(ab[channel], portion->mapping);
		cd[channel] = map_output(cd[channel], portion->mapping);
		sum[channel] = map_output(sum[channel], portion->mapping);
	}
}

static void write_portion(long registers[NUMBER_OF_REGISTERS][4], const struct combiner_portion *portion,
	BOOL alpha_portion, const long ab[3], const long cd[3], const long sum[3])
{
	unsigned char destinations[3] = { portion->ab, portion->cd, portion->sum };
	const long *values[3] = { ab, cd, sum };
	int which;

	for (which = 0; which < 3; which++)
	{
		unsigned char reg = destinations[which];

		if (reg < _register_v0 || reg >= _register_v1r0_sum || (reg > _register_v1 && reg < _register_t0))
			continue;
		if (alpha_portion)
		{
			registers[reg][3] = values[which][0];
		}
		else
		{
			registers[reg][0] = values[which][0];
			registers[reg][1] = values[which][1];
			registers[reg][2] = values[which][2];
			if ((which == 0 && portion->ab_dot_to_alpha) || (which == 1 && portion->cd_dot_to_alpha))
				registers[reg][3] = values[which][2];
		}
	}
}

static long final_input(long registers[NUMBER_OF_REGISTERS][4], const struct combiner_input *input, int channel)
{
	long value = registers[input->reg][input->alpha ? 3 : channel];

	value = clamp_unit(value);
	return (input->mapping & 0x20) ? ONE - value : value;
}

/* the combiners for one pixel: r, g, b, a in 0..256 */
static void combine(long registers[NUMBER_OF_REGISTERS][4], long out[4])
{
	int stage, channel;

	for (stage = 0; stage < combiners.stage_count; stage++)
	{
		const struct combiner_stage *s = &combiners.stages[stage];
		long rgb_ab[3], rgb_cd[3], rgb_sum[3], alpha_ab[3], alpha_cd[3], alpha_sum[3];

		memcpy(registers[_register_c0], s->c0, sizeof(s->c0));
		memcpy(registers[_register_c1], s->c1, sizeof(s->c1));
		run_portion(registers, &s->rgb, FALSE, rgb_ab, rgb_cd, rgb_sum);
		run_portion(registers, &s->alpha, TRUE, alpha_ab, alpha_cd, alpha_sum);
		write_portion(registers, &s->rgb, FALSE, rgb_ab, rgb_cd, rgb_sum);
		write_portion(registers, &s->alpha, TRUE, alpha_ab, alpha_cd, alpha_sum);
	}
	if (!combiners.has_final)
	{
		for (channel = 0; channel < 4; channel++)
			out[channel] = clamp_unit(registers[_register_r0][channel]);
		return;
	}
	memcpy(registers[_register_c0], combiners.final_c0, sizeof(combiners.final_c0));
	memcpy(registers[_register_c1], combiners.final_c1, sizeof(combiners.final_c1));
	for (channel = 0; channel < 3; channel++)
	{
		long v1 = clamp_unit(registers[_register_v1][channel]), r0 = clamp_unit(registers[_register_r0][channel]);
		long sum = ((combiners.final_settings & 0x40) ? ONE - v1 : v1) + ((combiners.final_settings & 0x20) ? ONE - r0 : r0);

		registers[_register_v1r0_sum][channel] = (combiners.final_settings & 0x80) ? clamp_unit(sum) : sum;
		registers[_register_ef_product][channel] = (final_input(registers, &combiners.final_inputs[4], channel) *
			final_input(registers, &combiners.final_inputs[5], channel)) >> 8;
	}
	registers[_register_v1r0_sum][3] = 0;
	registers[_register_ef_product][3] = 0;
	for (channel = 0; channel < 3; channel++)
	{
		long a = final_input(registers, &combiners.final_inputs[0], channel);
		long b = final_input(registers, &combiners.final_inputs[1], channel);
		long c = final_input(registers, &combiners.final_inputs[2], channel);
		long d = final_input(registers, &combiners.final_inputs[3], channel);

		out[channel] = clamp_unit(((a * b + (ONE - a) * c) >> 8) + d);
	}
	{
		/* G reads blue or alpha */
		const struct combiner_input *g = &combiners.final_inputs[6];
		long value = clamp_unit(registers[g->reg][g->alpha ? 3 : 2]);

		out[3] = (g->mapping & 0x20) ? ONE - value : value;
	}
}

/* ---------- the combiners compiled for a draw

What the pixel loop runs: only the portions and outputs whose results reach
the pixel, with inputs from the constant registers (zero, c0, c1, fog)
mapped once. The arithmetic is combine()'s, which stays as the reference. */

struct compiled_input
{
	unsigned char constant, reg, mapping;
	unsigned char component[3];
	long value[3];
};

struct compiled_portion
{
	unsigned char live, channels;
	unsigned char need_ab, need_cd;
	unsigned char ab_dot, cd_dot, mux, mapping;
	/* destinations; 0 for an output nothing reads */
	unsigned char ab, cd, sum;
	unsigned char ab_to_alpha, cd_to_alpha;
	/* how each product is made (product_kind): many have a constant zero or
	one for an input */
	unsigned char ab_kind, cd_kind;
	struct compiled_input inputs[4];
};

enum
{
	_product_full, _product_zero, _product_first, _product_second,
};

static struct
{
	int stage_count;
	struct compiled_portion portions[8][2];
	BOOL has_final, need_v1r0, need_ef;
	/* the final combiner only passing a register on: A, B and C constant zero,
	D read as it is (then out = clamp(D), alpha from G) */
	BOOL final_passes;
	struct compiled_input final_inputs[7];
	unsigned char final_settings;
	/* registers written, so reset for each pixel */
	unsigned long reset_registers;
} compiled;

static BOOL register_writable(unsigned char reg)
{
	return reg == _register_v0 || reg == _register_v1 || (reg >= _register_t0 && reg <= _register_r1);
}

/* a constant register's component: stage is -1 for the final combiner */
static long constant_register(unsigned char reg, int stage, int component)
{
	switch (reg)
	{
	case _register_c0: return stage < 0 ? combiners.final_c0[component] : combiners.stages[stage].c0[component];
	case _register_c1: return stage < 0 ? combiners.final_c1[component] : combiners.stages[stage].c1[component];
	case _register_fog: return component == 3 ? ONE : 0;
	default: return 0;
	}
}

static void compile_input(const struct combiner_input *input, BOOL alpha_portion, int stage,
	struct compiled_input *out)
{
	int channel;

	out->reg = input->reg;
	out->mapping = input->mapping;
	out->constant = input->reg <= _register_fog;
	for (channel = 0; channel < 3; channel++)
	{
		out->component[channel] = (unsigned char)(input->alpha ? 3 : alpha_portion ? 2 : channel);
		if (out->constant)
		{
			long value = constant_register(input->reg, stage, out->component[channel]);

			if (stage < 0)
			{
				value = clamp_unit(value);
				out->value[channel] = (input->mapping & 0x20) ? ONE - value : value;
			}
			else
			{
				out->value[channel] = map_input(value, input->mapping);
			}
		}
	}
}

/* the components of its register an input reads, as a mask (bit n is
component n) */
static unsigned input_components(const struct combiner_input *input, BOOL alpha_portion)
{
	return input->alpha ? 8 : alpha_portion ? 4 : 7;
}

/* whether a constant input is the same value in the channels a portion uses */
static BOOL constant_is(const struct compiled_input *input, int channels, long value)
{
	int channel;

	if (!input->constant)
		return FALSE;
	for (channel = 0; channel < channels; channel++)
	{
		if (input->value[channel] != value)
			return FALSE;
	}
	return TRUE;
}

/* a product of two inputs: zero when either is a constant zero, the other
when one is a constant one (a * 256 >> 8 is a, exactly), else a multiply */
static unsigned char product_kind(const struct compiled_input *a, const struct compiled_input *b, int channels)
{
	if (constant_is(a, channels, 0) || constant_is(b, channels, 0))
		return _product_zero;
	if (constant_is(b, channels, ONE))
		return _product_first;
	if (constant_is(a, channels, ONE))
		return _product_second;
	return _product_full;
}

static void combiners_compile(void)
{
	unsigned char live[NUMBER_OF_REGISTERS];
	int stage, which, texture;

	memset(&compiled, 0, sizeof(compiled));
	memset(live, 0, sizeof(live));
	compiled.stage_count = combiners.stage_count;
	compiled.has_final = combiners.has_final;
	if (combiners.has_final)
	{
		const struct combiner_input *inputs = combiners.final_inputs;

		compiled.final_settings = combiners.final_settings;
		for (which = 0; which < 7; which++)
		{
			if (which < 4 || which == 6)
			{
				if (inputs[which].reg == _register_ef_product)
					compiled.need_ef = TRUE;
			}
		}
		for (which = 0; which < 7; which++)
		{
			if ((which < 4 || which == 6 || compiled.need_ef) && inputs[which].reg == _register_v1r0_sum)
				compiled.need_v1r0 = TRUE;
		}
		for (which = 0; which < 7; which++)
		{
			if (which >= 4 && which < 6 && !compiled.need_ef)
				continue;
			compile_input(&inputs[which], which == 6, -1, &compiled.final_inputs[which]);
			if (which == 6 && !inputs[which].alpha)
				compiled.final_inputs[which].component[0] = 2;
			if (register_writable(inputs[which].reg) || inputs[which].reg >= _register_t0)
				live[inputs[which].reg] |= (unsigned char)input_components(&inputs[which], which == 6);
		}
		if (compiled.need_v1r0)
		{
			live[_register_v1] |= 7;
			live[_register_r0] |= 7;
		}
	}
	else
	{
		live[_register_r0] = 15;
	}

	for (stage = combiners.stage_count - 1; stage >= 0; stage--)
	{
		const struct combiner_stage *source = &combiners.stages[stage];
		unsigned char kill[NUMBER_OF_REGISTERS], read[NUMBER_OF_REGISTERS];
		int portion_index;

		memset(kill, 0, sizeof(kill));
		memset(read, 0, sizeof(read));
		for (portion_index = 0; portion_index < 2; portion_index++)
		{
			const struct combiner_portion *portion = portion_index ? &source->alpha : &source->rgb;
			struct compiled_portion *out = &compiled.portions[stage][portion_index];
			BOOL alpha_portion = portion_index == 1;
			unsigned char ab_components = (unsigned char)(alpha_portion ? 8 : portion->ab_dot_to_alpha ? 15 : 7);
			unsigned char cd_components = (unsigned char)(alpha_portion ? 8 : portion->cd_dot_to_alpha ? 15 : 7);
			unsigned char sum_components = (unsigned char)(alpha_portion ? 8 : 7);
			int input;

			out->channels = (unsigned char)(alpha_portion ? 1 : 3);
			out->ab_dot = (unsigned char)(!alpha_portion && portion->ab_dot);
			out->cd_dot = (unsigned char)(!alpha_portion && portion->cd_dot);
			out->mux = portion->mux;
			out->mapping = portion->mapping;
			out->ab_to_alpha = (unsigned char)(!alpha_portion && portion->ab_dot_to_alpha);
			out->cd_to_alpha = (unsigned char)(!alpha_portion && portion->cd_dot_to_alpha);
			if (register_writable(portion->ab) && (live[portion->ab] & ab_components))
				out->ab = portion->ab;
			if (register_writable(portion->cd) && (live[portion->cd] & cd_components))
				out->cd = portion->cd;
			if (register_writable(portion->sum) && (live[portion->sum] & sum_components))
				out->sum = portion->sum;
			out->need_ab = (unsigned char)(out->ab || out->sum);
			out->need_cd = (unsigned char)(out->cd || out->sum);
			out->live = (unsigned char)(out->need_ab || out->need_cd);
			if (!out->live)
				continue;
			if (out->ab) kill[out->ab] |= ab_components;
			if (out->cd) kill[out->cd] |= cd_components;
			if (out->sum) kill[out->sum] |= sum_components;
			for (input = 0; input < 4; input++)
			{
				if (input < 2 ? !out->need_ab : !out->need_cd)
					continue;
				compile_input(&portion->inputs[input], alpha_portion, stage, &out->inputs[input]);
				if (!out->inputs[input].constant)
					read[portion->inputs[input].reg] |= (unsigned char)input_components(&portion->inputs[input], alpha_portion);
			}
			if (out->mux && out->sum)
				read[_register_r0] |= 8;
			out->ab_kind = out->ab_dot ? _product_full : product_kind(&out->inputs[0], &out->inputs[1], out->channels);
			out->cd_kind = out->cd_dot ? _product_full : product_kind(&out->inputs[2], &out->inputs[3], out->channels);
			if (out->ab) compiled.reset_registers |= 1UL << out->ab;
			if (out->cd) compiled.reset_registers |= 1UL << out->cd;
			if (out->sum) compiled.reset_registers |= 1UL << out->sum;
		}
		for (which = 0; which < NUMBER_OF_REGISTERS; which++)
			live[which] = (unsigned char)((live[which] & ~kill[which]) | read[which]);
	}
	/* r0 is set for each pixel (its alpha from t0's) */
	compiled.reset_registers &= ~(1UL << _register_r0);
	compiled.final_passes = compiled.has_final && !compiled.need_ef && !compiled.need_v1r0 &&
		constant_is(&compiled.final_inputs[0], 3, 0) && constant_is(&compiled.final_inputs[1], 3, 0) &&
		constant_is(&compiled.final_inputs[2], 3, 0) && !compiled.final_inputs[3].constant &&
		!(compiled.final_inputs[3].mapping & 0x20);

	/* only the textures and colour something reads */
	for (texture = 0; texture < 4; texture++)
		combiners.uses_texture[texture] = live[_register_t0 + texture] != 0;
	if (live[_register_r0] & 8)
		combiners.uses_texture[0] = TRUE;
	combiners.uses_register[_register_v0] = live[_register_v0] != 0;
}

static __inline__ __attribute__((always_inline)) void compiled_read(long registers[NUMBER_OF_REGISTERS][4],
	const struct compiled_input *input, int channels, long out[3])
{
	const long *source;
	int channel;

	if (input->constant)
	{
		out[0] = input->value[0];
		out[1] = input->value[1];
		out[2] = input->value[2];
		return;
	}
	source = registers[input->reg];
	switch (input->mapping)
	{
	case 0x00:
		for (channel = 0; channel < channels; channel++)
		{
			long value = source[input->component[channel]];

			out[channel] = value < 0 ? 0 : value;
		}
		break;
	case 0x20:
		for (channel = 0; channel < channels; channel++)
			out[channel] = ONE - clamp_unit(source[input->component[channel]]);
		break;
	default:
		for (channel = 0; channel < channels; channel++)
			out[channel] = map_input(source[input->component[channel]], input->mapping);
		break;
	}
}

static __inline__ __attribute__((always_inline)) long compiled_map_output(long value, unsigned char mapping)
{
	if (mapping)
		return map_output(value, mapping);
	return value < -ONE ? -ONE : value > ONE ? ONE : value;
}

static void compiled_portion_run(long registers[NUMBER_OF_REGISTERS][4], const struct compiled_portion *portion,
	BOOL pick_cd, long results[3][3])
{
	long a[3], b[3], c[3], d[3], ab[3] = { 0, 0, 0 }, cd[3] = { 0, 0, 0 };
	int channels = portion->channels, channel;

	if (portion->need_ab)
	{
		switch (portion->ab_kind)
		{
		case _product_zero:
			break;
		case _product_first:
			compiled_read(registers, &portion->inputs[0], channels, ab);
			break;
		case _product_second:
			compiled_read(registers, &portion->inputs[1], channels, ab);
			break;
		default:
			compiled_read(registers, &portion->inputs[0], channels, a);
			compiled_read(registers, &portion->inputs[1], channels, b);
			if (portion->ab_dot)
				ab[0] = ab[1] = ab[2] = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) >> 8;
			else
				for (channel = 0; channel < channels; channel++)
					ab[channel] = (a[channel] * b[channel]) >> 8;
			break;
		}
	}
	if (portion->need_cd)
	{
		switch (portion->cd_kind)
		{
		case _product_zero:
			break;
		case _product_first:
			compiled_read(registers, &portion->inputs[2], channels, cd);
			break;
		case _product_second:
			compiled_read(registers, &portion->inputs[3], channels, cd);
			break;
		default:
			compiled_read(registers, &portion->inputs[2], channels, c);
			compiled_read(registers, &portion->inputs[3], channels, d);
			if (portion->cd_dot)
				cd[0] = cd[1] = cd[2] = (c[0] * d[0] + c[1] * d[1] + c[2] * d[2]) >> 8;
			else
				for (channel = 0; channel < channels; channel++)
					cd[channel] = (c[channel] * d[channel]) >> 8;
			break;
		}
	}
	for (channel = 0; channel < channels; channel++)
	{
		if (portion->sum)
		{
			long sum = portion->mux ? (pick_cd ? cd[channel] : ab[channel]) : ab[channel] + cd[channel];

			results[2][channel] = compiled_map_output(sum, portion->mapping);
		}
		if (portion->ab)
			results[0][channel] = compiled_map_output(ab[channel], portion->mapping);
		if (portion->cd)
			results[1][channel] = compiled_map_output(cd[channel], portion->mapping);
	}
}

static void compiled_portion_write(long registers[NUMBER_OF_REGISTERS][4], const struct compiled_portion *portion,
	const long results[3][3])
{
	const unsigned char destinations[3] = { portion->ab, portion->cd, portion->sum };
	int which;

	for (which = 0; which < 3; which++)
	{
		long *destination;

		if (!destinations[which])
			continue;
		destination = registers[destinations[which]];
		if (portion->channels == 1)
		{
			destination[3] = results[which][0];
		}
		else
		{
			destination[0] = results[which][0];
			destination[1] = results[which][1];
			destination[2] = results[which][2];
			if ((which == 0 && portion->ab_to_alpha) || (which == 1 && portion->cd_to_alpha))
				destination[3] = results[which][2];
		}
	}
}

static __inline__ __attribute__((always_inline)) long compiled_final_read(long registers[NUMBER_OF_REGISTERS][4],
	const struct compiled_input *input, int channel)
{
	long value;

	if (input->constant)
		return input->value[channel];
	value = clamp_unit(registers[input->reg][input->component[channel]]);
	return (input->mapping & 0x20) ? ONE - value : value;
}

/* the compiled combiners for one pixel: r, g, b, a in 0..256 */
static void combine_compiled(long registers[NUMBER_OF_REGISTERS][4], long out[4])
{
	int stage, channel;

	for (stage = 0; stage < compiled.stage_count; stage++)
	{
		const struct compiled_portion *rgb = &compiled.portions[stage][0], *alpha = &compiled.portions[stage][1];
		long rgb_results[3][3], alpha_results[3][3];
		BOOL pick_cd = FALSE;

		if (!rgb->live && !alpha->live)
			continue;
		if (rgb->mux || alpha->mux)
			pick_cd = combiners.mux_msb ? registers[_register_r0][3] >= ONE / 2 : (registers[_register_r0][3] & 1);
		if (rgb->live)
			compiled_portion_run(registers, rgb, pick_cd, rgb_results);
		if (alpha->live)
			compiled_portion_run(registers, alpha, pick_cd, alpha_results);
		if (rgb->live)
			compiled_portion_write(registers, rgb, (const long (*)[3])rgb_results);
		if (alpha->live)
			compiled_portion_write(registers, alpha, (const long (*)[3])alpha_results);
	}
	if (!compiled.has_final)
	{
		for (channel = 0; channel < 4; channel++)
			out[channel] = clamp_unit(registers[_register_r0][channel]);
		return;
	}
	if (compiled.final_passes)
	{
		const struct compiled_input *d = &compiled.final_inputs[3];

		out[0] = clamp_unit(registers[d->reg][d->component[0]]);
		out[1] = clamp_unit(registers[d->reg][d->component[1]]);
		out[2] = clamp_unit(registers[d->reg][d->component[2]]);
		out[3] = compiled_final_read(registers, &compiled.final_inputs[6], 0);
		return;
	}
	if (compiled.need_v1r0)
	{
		for (channel = 0; channel < 3; channel++)
		{
			long v1 = clamp_unit(registers[_register_v1][channel]), r0 = clamp_unit(registers[_register_r0][channel]);
			long sum = ((compiled.final_settings & 0x40) ? ONE - v1 : v1) + ((compiled.final_settings & 0x20) ? ONE - r0 : r0);

			registers[_register_v1r0_sum][channel] = (compiled.final_settings & 0x80) ? clamp_unit(sum) : sum;
		}
		registers[_register_v1r0_sum][3] = 0;
	}
	if (compiled.need_ef)
	{
		for (channel = 0; channel < 3; channel++)
		{
			registers[_register_ef_product][channel] = (compiled_final_read(registers, &compiled.final_inputs[4], channel) *
				compiled_final_read(registers, &compiled.final_inputs[5], channel)) >> 8;
		}
		registers[_register_ef_product][3] = 0;
	}
	for (channel = 0; channel < 3; channel++)
	{
		long a = compiled_final_read(registers, &compiled.final_inputs[0], channel);
		long b = compiled_final_read(registers, &compiled.final_inputs[1], channel);
		long c = compiled_final_read(registers, &compiled.final_inputs[2], channel);
		long d = compiled_final_read(registers, &compiled.final_inputs[3], channel);

		out[channel] = clamp_unit(((a * b + (ONE - a) * c) >> 8) + d);
	}
	out[3] = compiled_final_read(registers, &compiled.final_inputs[6], 0);
}

/* whether the compiled combiners are of the simple kind combine_simple
runs: inputs read as they are or inverted, products and their sum, no dot
products or muxes or output mappings, and a final combiner that only
passes a register on (or none) */
static BOOL combiners_simple(void)
{
	int stage, portion, input;

	if (compiled.has_final && !compiled.final_passes)
		return FALSE;
	for (stage = 0; stage < compiled.stage_count; stage++)
	{
		for (portion = 0; portion < 2; portion++)
		{
			const struct compiled_portion *p = &compiled.portions[stage][portion];

			if (!p->live)
				continue;
			if (p->ab_dot || p->cd_dot || p->mux || p->mapping || p->ab_to_alpha || p->cd_to_alpha)
				return FALSE;
			for (input = 0; input < 4; input++)
			{
				if ((input < 2 ? p->need_ab : p->need_cd) && !p->inputs[input].constant &&
					p->inputs[input].mapping != 0x00 && p->inputs[input].mapping != 0x20)
				{
					return FALSE;
				}
			}
		}
	}
	return TRUE;
}

/* code written as data: out of the data cache to memory, and the
instruction cache emptied (the ARM926's caches are separate) */
static void jit_sync_caches(const void *code, unsigned long size)
{
	unsigned long line, end = (unsigned long)code + size;

	for (line = (unsigned long)code & ~31UL; line < end; line += 32)
		__asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(line) : "memory");
	__asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
	__asm__ volatile("mcr p15, 0, %0, c7, c5, 0" :: "r"(0) : "memory");
}

/* ---------- the combiners as machine code

What combine_compiled works out for a pixel, written once per combiner
setup as ARM code and run for every pixel: an input is a load from the
register file and two to four conditional instructions for its mapping, a
product a multiply and a shift. The routine takes the register file in r0
and the colour out in r1, as combine_compiled does, and gives the same
numbers. Routines are kept for the setups seen (JIT_CACHE_ENTRIES). */

typedef void (*shader_routine)(long registers[NUMBER_OF_REGISTERS][4], long out[4]);

#define JIT_CACHE_ENTRIES 48
#define JIT_KEY_WORDS 48
#define JIT_MAXIMUM_WORDS 2048

static struct
{
	unsigned long key[JIT_KEY_WORDS];
	unsigned long used;
	unsigned long *code;
} jit_cache[JIT_CACHE_ENTRIES];
static unsigned long jit_clock;

static struct
{
	unsigned long *code;
	unsigned long count;
	BOOL overflow;
} jit;

/* the scratch the routine keeps a stage's results in, on its stack */
#define JIT_SCRATCH_BYTES 64
#define JIT_REGISTER_OFFSET(reg, component) ((reg) * 16 + (component) * 4)

enum
{
	_jit_eq = 0x0, _jit_ne = 0x1, _jit_ge = 0xA, _jit_lt = 0xB, _jit_gt = 0xC, _jit_le = 0xD, _jit_al = 0xE,
};

static void emit(unsigned long word)
{
	if (jit.count < JIT_MAXIMUM_WORDS)
		jit.code[jit.count++] = word;
	else
		jit.overflow = TRUE;
}

/* an ARM data-processing immediate (8 bits rotated), or ~0 if there is none */
static unsigned long immediate(unsigned long value)
{
	int rotate;

	for (rotate = 0; rotate < 16; rotate++)
	{
		unsigned long rotated = (value << (2 * rotate)) | (value >> ((32 - 2 * rotate) & 31));

		if (rotate == 0)
			rotated = value;
		if (rotated < 256)
			return ((unsigned long)rotate << 8) | rotated;
	}
	return ~0UL;
}

static void emit_mov_constant(int rd, long value, int condition)
{
	unsigned long encoded = immediate((unsigned long)value);

	if (encoded != ~0UL)
	{
		emit(((unsigned long)condition << 28) | 0x03A00000UL | ((unsigned long)rd << 12) | encoded);
		return;
	}
	encoded = immediate(~(unsigned long)value);
	if (encoded != ~0UL)
	{
		emit(((unsigned long)condition << 28) | 0x03E00000UL | ((unsigned long)rd << 12) | encoded);
		return;
	}
	/* (no combiner value needs more: they are within +-1024) */
	jit.overflow = TRUE;
}

static void emit_load(int rd, int base, unsigned long offset)
{
	emit(0xE5900000UL | ((unsigned long)base << 16) | ((unsigned long)rd << 12) | offset);
}

static void emit_store(int rd, int base, unsigned long offset)
{
	emit(0xE5800000UL | ((unsigned long)base << 16) | ((unsigned long)rd << 12) | offset);
}

/* rd = rn op #value (op: 0x4 add, 0x2 sub, 0x3 rsb) */
static void emit_arithmetic_constant(unsigned long op, int rd, int rn, long value)
{
	unsigned long encoded = immediate((unsigned long)value);

	if (encoded == ~0UL)
	{
		jit.overflow = TRUE;
		return;
	}
	emit(0xE2000000UL | (op << 21) | ((unsigned long)rn << 16) | ((unsigned long)rd << 12) | encoded);
}

static void emit_compare_constant(int rn, long value)
{
	if (value < 0)
		emit(0xE3700000UL | ((unsigned long)rn << 16) | immediate((unsigned long)-value));
	else
		emit(0xE3500000UL | ((unsigned long)rn << 16) | immediate((unsigned long)value));
}

/* rd = 0 below zero */
static void emit_positive(int rd)
{
	emit_compare_constant(rd, 0);
	emit_mov_constant(rd, 0, _jit_lt);
}

/* rd held to 0..ONE */
static void emit_clamp_unit(int rd)
{
	emit_positive(rd);
	emit_compare_constant(rd, ONE);
	emit_mov_constant(rd, ONE, _jit_gt);
}

/* rd held to -ONE..ONE */
static void emit_clamp_signed(int rd)
{
	emit_compare_constant(rd, ONE);
	emit_mov_constant(rd, ONE, _jit_gt);
	emit_compare_constant(rd, -ONE);
	emit_mov_constant(rd, -ONE, _jit_lt);
}

static void emit_shift(int rd, int rm, int type, int amount)
{
	/* type: 0 lsl, 1 lsr, 2 asr */
	emit(0xE1A00000UL | ((unsigned long)rd << 12) | ((unsigned long)amount << 7) | ((unsigned long)type << 5) | (unsigned long)rm);
}

static void emit_register_op(unsigned long op, int rd, int rn, int rm)
{
	/* op: 0x4 add, 0x2 sub */
	emit(0xE0000000UL | (op << 21) | ((unsigned long)rn << 16) | ((unsigned long)rd << 12) | (unsigned long)rm);
}

static void emit_multiply(int rd, int rm, int rs)
{
	emit(0xE0000090UL | ((unsigned long)rd << 16) | ((unsigned long)rs << 8) | (unsigned long)rm);
}

static void emit_multiply_add(int rd, int rm, int rs, int rn)
{
	emit(0xE0200090UL | ((unsigned long)rd << 16) | ((unsigned long)rn << 12) | ((unsigned long)rs << 8) | (unsigned long)rm);
}

static void emit_move(int rd, int rm, int condition)
{
	emit(((unsigned long)condition << 28) | 0x01A00000UL | ((unsigned long)rd << 12) | (unsigned long)rm);
}

/* a combiner input's value for one channel, as map_input gives it */
static void emit_input(int rd, const struct compiled_input *input, int channel)
{
	if (input->constant)
	{
		emit_mov_constant(rd, input->value[channel], _jit_al);
		return;
	}
	emit_load(rd, 0, JIT_REGISTER_OFFSET(input->reg, input->component[channel]));
	switch (input->mapping)
	{
	case 0x00:
		emit_positive(rd);
		break;
	case 0x20:
		emit_clamp_unit(rd);
		emit_arithmetic_constant(0x3, rd, rd, ONE);
		break;
	case 0x40:
		emit_positive(rd);
		emit_shift(rd, rd, 0, 1);
		emit_arithmetic_constant(0x2, rd, rd, ONE);
		break;
	case 0x60:
		emit_positive(rd);
		emit_shift(rd, rd, 0, 1);
		emit_arithmetic_constant(0x3, rd, rd, ONE);
		break;
	case 0x80:
		emit_positive(rd);
		emit_arithmetic_constant(0x2, rd, rd, ONE / 2);
		break;
	case 0xA0:
		emit_positive(rd);
		emit_arithmetic_constant(0x3, rd, rd, ONE / 2);
		break;
	case 0xC0:
		break;
	default:
		emit_arithmetic_constant(0x3, rd, rd, 0);
		break;
	}
}

/* map_output: the portion's output mapping, then -ONE..ONE */
static void emit_map_output(int rd, unsigned char mapping)
{
	switch (mapping)
	{
	case 0x08:
		emit_arithmetic_constant(0x2, rd, rd, ONE / 2);
		break;
	case 0x10:
		emit_shift(rd, rd, 0, 1);
		break;
	case 0x18:
		emit_arithmetic_constant(0x2, rd, rd, ONE / 2);
		emit_shift(rd, rd, 0, 1);
		break;
	case 0x20:
		emit_shift(rd, rd, 0, 2);
		break;
	case 0x30:
		/* C's division by two, toward zero */
		emit(0xE0800FA0UL | ((unsigned long)rd << 16) | ((unsigned long)rd << 12) | (unsigned long)rd);
		emit_shift(rd, rd, 2, 1);
		break;
	default:
		break;
	}
	emit_clamp_signed(rd);
}

/* a product into rd (scratch r2, r3): its kind (product_kind) or a dot product */
static void emit_product(int rd, const struct compiled_input *a, const struct compiled_input *b, unsigned char kind,
	BOOL dot, int channel)
{
	if (dot)
	{
		int k;

		for (k = 0; k < 3; k++)
		{
			emit_input(2, a, k);
			emit_input(3, b, k);
			if (k == 0)
				emit_multiply(rd, 2, 3);
			else
				emit_multiply_add(rd, 2, 3, rd);
		}
		emit_shift(rd, rd, 2, 8);
		return;
	}
	switch (kind)
	{
	case _product_zero:
		emit_mov_constant(rd, 0, _jit_al);
		break;
	case _product_first:
		emit_input(rd, a, channel);
		break;
	case _product_second:
		emit_input(rd, b, channel);
		break;
	default:
		emit_input(2, a, channel);
		emit_input(3, b, channel);
		emit_multiply(rd, 2, 3);
		emit_shift(rd, rd, 2, 8);
		break;
	}
}

/* a final combiner input (final_input): held to 0..ONE, maybe inverted */
static void emit_final_input(int rd, const struct compiled_input *input, int channel)
{
	if (input->constant)
	{
		emit_mov_constant(rd, input->value[channel], _jit_al);
		return;
	}
	emit_load(rd, 0, JIT_REGISTER_OFFSET(input->reg, input->component[channel]));
	emit_clamp_unit(rd);
	if (input->mapping & 0x20)
		emit_arithmetic_constant(0x3, rd, rd, ONE);
}

/* scratch slot of a stage's result: portion (0 rgb, 1 alpha), which (ab, cd,
sum), channel */
#define SCRATCH(portion, which, channel) ((((portion) * 3 + (which)) * 3 + (channel)) * 4)

static void jit_compile_routine(void)
{
	int stage, channel;

	/* push {r4-r11, lr}; sub sp, sp, #scratch */
	emit(0xE92D4FF0UL);
	emit(0xE24DD000UL | immediate(JIT_SCRATCH_BYTES * 2));

	for (stage = 0; stage < compiled.stage_count; stage++)
	{
		const struct compiled_portion *rgb = &compiled.portions[stage][0], *alpha = &compiled.portions[stage][1];
		int portion_index;

		if (!rgb->live && !alpha->live)
			continue;
		if ((rgb->live && rgb->mux && rgb->sum) || (alpha->live && alpha->mux && alpha->sum))
		{
			/* r10: whether a mux takes cd, from r0's alpha before the stage */
			emit_load(10, 0, JIT_REGISTER_OFFSET(_register_r0, 3));
			if (combiners.mux_msb)
			{
				emit_compare_constant(10, ONE / 2);
				emit_mov_constant(10, 0, _jit_al);
				emit_mov_constant(10, 1, _jit_ge);
			}
			else
			{
				emit(0xE2000001UL | (10UL << 16) | (10UL << 12));
			}
		}
		for (portion_index = 0; portion_index < 2; portion_index++)
		{
			const struct compiled_portion *portion = portion_index ? alpha : rgb;

			if (!portion->live)
				continue;
			for (channel = 0; channel < portion->channels; channel++)
			{
				/* r6 ab, r7 cd, r8 sum */
				if (portion->need_ab)
					emit_product(6, &portion->inputs[0], &portion->inputs[1], portion->ab_kind, portion->ab_dot, channel);
				else
					emit_mov_constant(6, 0, _jit_al);
				if (portion->need_cd)
					emit_product(7, &portion->inputs[2], &portion->inputs[3], portion->cd_kind, portion->cd_dot, channel);
				else
					emit_mov_constant(7, 0, _jit_al);
				if (portion->sum)
				{
					if (portion->mux)
					{
						emit_move(8, 6, _jit_al);
						emit_compare_constant(10, 0);
						emit_move(8, 7, _jit_ne);
					}
					else
					{
						emit_register_op(0x4, 8, 6, 7);
					}
					emit_map_output(8, portion->mapping);
					emit_store(8, 13, SCRATCH(portion_index, 2, channel));
				}
				if (portion->ab)
				{
					emit_map_output(6, portion->mapping);
					emit_store(6, 13, SCRATCH(portion_index, 0, channel));
				}
				if (portion->cd)
				{
					emit_map_output(7, portion->mapping);
					emit_store(7, 13, SCRATCH(portion_index, 1, channel));
				}
			}
		}
		/* the stage's results, written once both portions have read */
		for (portion_index = 0; portion_index < 2; portion_index++)
		{
			const struct compiled_portion *portion = portion_index ? alpha : rgb;
			const unsigned char destinations[3] = { portion->ab, portion->cd, portion->sum };
			int which;

			if (!portion->live)
				continue;
			for (which = 0; which < 3; which++)
			{
				if (!destinations[which])
					continue;
				if (portion->channels == 1)
				{
					emit_load(2, 13, SCRATCH(portion_index, which, 0));
					emit_store(2, 0, JIT_REGISTER_OFFSET(destinations[which], 3));
				}
				else
				{
					for (channel = 0; channel < 3; channel++)
					{
						emit_load(2, 13, SCRATCH(portion_index, which, channel));
						emit_store(2, 0, JIT_REGISTER_OFFSET(destinations[which], channel));
					}
					if ((which == 0 && portion->ab_to_alpha) || (which == 1 && portion->cd_to_alpha))
						emit_store(2, 0, JIT_REGISTER_OFFSET(destinations[which], 3));
				}
			}
		}
	}

	if (!compiled.has_final)
	{
		for (channel = 0; channel < 4; channel++)
		{
			emit_load(2, 0, JIT_REGISTER_OFFSET(_register_r0, channel));
			emit_clamp_unit(2);
			emit_store(2, 1, channel * 4);
		}
	}
	else if (compiled.final_passes)
	{
		const struct compiled_input *d = &compiled.final_inputs[3];

		for (channel = 0; channel < 3; channel++)
		{
			emit_load(2, 0, JIT_REGISTER_OFFSET(d->reg, d->component[channel]));
			emit_clamp_unit(2);
			emit_store(2, 1, channel * 4);
		}
		emit_final_input(2, &compiled.final_inputs[6], 0);
		emit_store(2, 1, 12);
	}
	else
	{
		if (compiled.need_v1r0)
		{
			for (channel = 0; channel < 3; channel++)
			{
				emit_load(2, 0, JIT_REGISTER_OFFSET(_register_v1, channel));
				emit_clamp_unit(2);
				if (compiled.final_settings & 0x40)
					emit_arithmetic_constant(0x3, 2, 2, ONE);
				emit_load(3, 0, JIT_REGISTER_OFFSET(_register_r0, channel));
				emit_clamp_unit(3);
				if (compiled.final_settings & 0x20)
					emit_arithmetic_constant(0x3, 3, 3, ONE);
				emit_register_op(0x4, 2, 2, 3);
				if (compiled.final_settings & 0x80)
					emit_clamp_unit(2);
				emit_store(2, 0, JIT_REGISTER_OFFSET(_register_v1r0_sum, channel));
			}
			emit_mov_constant(2, 0, _jit_al);
			emit_store(2, 0, JIT_REGISTER_OFFSET(_register_v1r0_sum, 3));
		}
		if (compiled.need_ef)
		{
			for (channel = 0; channel < 3; channel++)
			{
				emit_final_input(2, &compiled.final_inputs[4], channel);
				emit_final_input(3, &compiled.final_inputs[5], channel);
				emit_multiply(4, 2, 3);
				emit_shift(4, 4, 2, 8);
				emit_store(4, 0, JIT_REGISTER_OFFSET(_register_ef_product, channel));
			}
			emit_mov_constant(2, 0, _jit_al);
			emit_store(2, 0, JIT_REGISTER_OFFSET(_register_ef_product, 3));
		}
		for (channel = 0; channel < 3; channel++)
		{
			/* clamp(((a * b + (ONE - a) * c) >> 8) + d) */
			emit_final_input(4, &compiled.final_inputs[0], channel);
			emit_final_input(5, &compiled.final_inputs[1], channel);
			emit_final_input(6, &compiled.final_inputs[2], channel);
			emit_final_input(7, &compiled.final_inputs[3], channel);
			emit_multiply(8, 4, 5);
			emit_arithmetic_constant(0x3, 9, 4, ONE);
			emit_multiply_add(8, 9, 6, 8);
			emit_shift(8, 8, 2, 8);
			emit_register_op(0x4, 8, 8, 7);
			emit_clamp_unit(8);
			emit_store(8, 1, channel * 4);
		}
		emit_final_input(2, &compiled.final_inputs[6], 0);
		emit_store(2, 1, 12);
	}

	/* add sp, sp, #scratch; pop {r4-r11, pc} */
	emit(0xE28DD000UL | immediate(JIT_SCRATCH_BYTES * 2));
	emit(0xE8BD8FF0UL);
}

/* the render state the combiners are made from: the key of a routine */
static void jit_key(unsigned long key[JIT_KEY_WORDS])
{
	int index, words = 0;

	memset(key, 0, JIT_KEY_WORDS * sizeof(unsigned long));
	key[words++] = D3D__RenderState[D3DRS_PSCOMBINERCOUNT];
	key[words++] = D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSABCD];
	key[words++] = D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSEFG];
	key[words++] = D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT0];
	key[words++] = D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT1];
	key[words++] = D3D__RenderState[D3DRS_PSTEXTUREMODES];
	for (index = 0; index < combiners.stage_count && words + 6 <= JIT_KEY_WORDS; index++)
	{
		key[words++] = D3D__RenderState[D3DRS_PSRGBINPUTS0 + index];
		key[words++] = D3D__RenderState[D3DRS_PSRGBOUTPUTS0 + index];
		key[words++] = D3D__RenderState[D3DRS_PSALPHAINPUTS0 + index];
		key[words++] = D3D__RenderState[D3DRS_PSALPHAOUTPUTS0 + index];
		key[words++] = D3D__RenderState[D3DRS_PSCONSTANT0_0 + index];
		key[words++] = D3D__RenderState[D3DRS_PSCONSTANT1_0 + index];
	}
}

/* the routine for the compiled combiners, made if this setup is new; NULL
if it did not fit (combine_compiled runs instead) */
static shader_routine jit_routine(void)
{
	unsigned long key[JIT_KEY_WORDS];
	unsigned long oldest = 0, index;

	jit_key(key);
	jit_clock++;
	for (index = 0; index < JIT_CACHE_ENTRIES; index++)
	{
		if (jit_cache[index].code && !memcmp(jit_cache[index].key, key, sizeof(key)))
		{
			jit_cache[index].used = jit_clock;
			return (shader_routine)jit_cache[index].code;
		}
		if (jit_cache[index].used < jit_cache[oldest].used)
			oldest = index;
	}
	if (!jit_cache[oldest].code)
	{
		jit_cache[oldest].code = malloc(JIT_MAXIMUM_WORDS * sizeof(unsigned long));
		if (!jit_cache[oldest].code)
			return NULL;
	}
	jit.code = jit_cache[oldest].code;
	jit.count = 0;
	jit.overflow = FALSE;
	jit_compile_routine();
	if (jit.overflow)
	{
		memset(jit_cache[oldest].key, 0xFF, sizeof(jit_cache[oldest].key));
		jit_cache[oldest].used = 0;
		return NULL;
	}
	memcpy(jit_cache[oldest].key, key, sizeof(key));
	jit_cache[oldest].used = jit_clock;
	/* to memory, and out of the instruction cache: it is code now */
	jit_sync_caches(jit.code, jit.count * sizeof(unsigned long));
	return (shader_routine)jit.code;
}

/* the compiled combiners, for working out which are common (the replay
logs them) */
void soft_rasterizer_describe_combiners(void)
{
	int stage, portion, input;
	static const char *const names[] = { "0", "c0", "c1", "fog", "v0", "v1", "?6", "?7", "t0", "t1", "t2", "t3", "r0", "r1",
		"v1r0", "ef" };

	for (stage = 0; stage < compiled.stage_count; stage++)
	{
		for (portion = 0; portion < 2; portion++)
		{
			const struct compiled_portion *p = &compiled.portions[stage][portion];
			char text[200];
			int length = 0;

			if (!p->live)
				continue;
			for (input = 0; input < 4; input++)
			{
				const struct compiled_input *in = &p->inputs[input];

				length += snprintf(text + length, sizeof(text) - length, " %s%s%s/%x",
					in->constant ? "#" : "", names[in->reg & 15], in->component[0] == 3 ? ".a" : "", in->mapping);
			}
			nspire_log("    stage %d %s:%s -> ab %s cd %s sum %s%s%s map %x", stage, portion ? "alpha" : "rgb", text,
				names[p->ab], names[p->cd], names[p->sum], p->ab_dot ? " abdot" : "", p->mux ? " mux" : "", p->mapping);
		}
	}
	if (compiled.has_final)
	{
		char text[200];
		int length = 0;

		for (input = 0; input < 7; input++)
		{
			const struct compiled_input *in = &compiled.final_inputs[input];

			length += snprintf(text + length, sizeof(text) - length, " %c=%s%s%s/%x", "ABCDEFG"[input], in->constant ? "#" : "",
				names[in->reg & 15], in->component[0] == 3 ? ".a" : "", in->mapping);
		}
		nspire_log("    final:%s settings %x", text, compiled.final_settings);
	}
}

/* ---------- blending and tests */

static BOOL compare(DWORD function, unsigned long value, unsigned long reference)
{
	switch (function)
	{
	case D3DCMP_NEVER: return FALSE;
	case D3DCMP_LESS: return value < reference;
	case D3DCMP_EQUAL: return value == reference;
	case D3DCMP_LESSEQUAL: return value <= reference;
	case D3DCMP_GREATER: return value > reference;
	case D3DCMP_NOTEQUAL: return value != reference;
	case D3DCMP_GREATEREQUAL: return value >= reference;
	default: return TRUE;
	}
}

/* a blend factor for the alpha channel: colour factors take their alpha */
static long blend_factor_alpha(DWORD factor, long source_alpha, long destination_alpha)
{
	switch (factor)
	{
	case D3DBLEND_ZERO: return 0;
	case D3DBLEND_SRCCOLOR:
	case D3DBLEND_SRCALPHA: return source_alpha;
	case D3DBLEND_INVSRCCOLOR:
	case D3DBLEND_INVSRCALPHA: return ONE - source_alpha;
	case D3DBLEND_DESTCOLOR:
	case D3DBLEND_DESTALPHA: return destination_alpha;
	case D3DBLEND_INVDESTCOLOR:
	case D3DBLEND_INVDESTALPHA: return ONE - destination_alpha;
	default: return ONE;
	}
}

static long blend_factor(DWORD factor, const long source[4], const long destination[4], int channel)
{
	switch (factor)
	{
	case D3DBLEND_ZERO: return 0;
	case D3DBLEND_ONE: return ONE;
	case D3DBLEND_SRCCOLOR: return source[channel];
	case D3DBLEND_INVSRCCOLOR: return ONE - source[channel];
	case D3DBLEND_SRCALPHA: return source[3];
	case D3DBLEND_INVSRCALPHA: return ONE - source[3];
	/* (the screen's alpha is alpha_buffer's) */
	case D3DBLEND_DESTALPHA: return destination[3];
	case D3DBLEND_INVDESTALPHA: return ONE - destination[3];
	case D3DBLEND_DESTCOLOR: return destination[channel];
	case D3DBLEND_INVDESTCOLOR: return ONE - destination[channel];
	case D3DBLEND_SRCALPHASAT: return source[3] < ONE - destination[3] ? source[3] : ONE - destination[3];
	default: return ONE;
	}
}

/* ---------- the rasterizer */

struct draw_state
{
	unsigned short *pixels;
	BOOL z_test, z_write, color_write, blend, alpha_test;
	DWORD z_function, alpha_function, source_blend, destination_blend;
	unsigned long alpha_reference;
	DWORD cull_mode;
	float z_normalize;
	/* the attributes interpolated (z always first), and whether the
	combiners read the diffuse colour */
	int attributes[13];
	int attribute_count;
	BOOL uses_diffuse;
	/* the diffuse colour the same all over the draw (flat lighting): set once,
	not interpolated; in 0..256 */
	BOOL constant_diffuse;
	long diffuse[4];
	/* registers the combiners write, reset for each pixel */
	unsigned long written_registers;
	/* the combiners as machine code (jit_routine), or NULL */
	void (*shader)(long registers[NUMBER_OF_REGISTERS][4], long out[4]);
	/* combiners that read no texture or vertex colour give every pixel the
	same colour, worked out once */
	BOOL constant_color;
	/* (shader made at least once: state_prepare may keep it) */
	BOOL shader_valid;
	long color[4];
	/* the texture stages sampled, in order, and how */
	int stages[4], stage_kinds[4], stage_count;
	/* the stages whose coordinates are interpolated (bit n for stage n) */
	unsigned long coordinate_stages;
	/* the blending as one of the common cases, or generic */
	int blend_kind;
	/* the screen's alpha (alpha_buffer) written, or read by the blending */
	BOOL alpha_write, reads_destination_alpha;
};

enum
{
	_stage_passthrough, _stage_sample, _stage_grey,
};

enum
{
	_blend_generic, _blend_copy, _blend_alpha, _blend_add, _blend_alpha_add, _blend_multiply, _blend_multiply2x,
	_blend_premultiplied,
};

static struct draw_state state;

/* models lit once a draw rather than at every vertex: per-vertex lighting
is often half a model's vertex program, and at 160x120 the shading across a
part barely shows (draw_source.lit_program) */
#ifndef NSPIRE_FLAT_MODEL_LIGHTING
#define NSPIRE_FLAT_MODEL_LIGHTING 1
#endif

/* whether the logged frame checks the compiled combiners against combine()
(it found no difference on b30; the replay can build with it) */
#ifndef CHECK_COMPILED_COMBINERS
#define CHECK_COMPILED_COMBINERS 0
#endif

/* the combiners as machine code (jit_routine) rather than combine_compiled */
#ifndef NSPIRE_COMBINER_CODE
#define NSPIRE_COMBINER_CODE 1
#endif


/* skinned model parts culled whole by their nodes' boxes (part_hidden) */
#ifndef NSPIRE_PART_CULLING
#define NSPIRE_PART_CULLING 1
#endif

/* opaque triangles too shaded by 2x2 blocks in the 3D view (each block of
its own triangle; depth still by pixel) */
#ifndef NSPIRE_OPAQUE_BLOCKS
#define NSPIRE_OPAQUE_BLOCKS 1
#endif

/* twice the area from which a blended layer's triangle is shaded by 2x2
blocks (other triangles: BLOCK_SHADING_AREA / 4) */
#ifndef BLOCKED_LAYER_AREA
#define BLOCKED_LAYER_AREA 32.0f
#endif

/* log2 of the sky's shading blocks (drawn before any depth, with none) */
#ifndef SKY_BLOCK_SHIFT
#define SKY_BLOCK_SHIFT 1
#endif

/* opaque draws through their own pixel loop */
#ifndef NSPIRE_FAST_OPAQUE
#define NSPIRE_FAST_OPAQUE 1
#endif

/* blended layers shaded by blocks through their own pixel loop */
#ifndef NSPIRE_FAST_LAYERS
#define NSPIRE_FAST_LAYERS 1
#endif

/* twice a triangle's area in 32.32: 0.01 square pixels, under which it is
too thin to draw, and 2^19, over which it can only come of corners at
infinity; and the bound on the 8.24 terms of the integer setup */
#ifndef FIXED_THIN_AREA
#define FIXED_THIN_AREA 42949673LL
#endif
/* the deepest a pixel's depth goes (times 256) */
#ifndef DEPTH_CLAMP_MAX
#define DEPTH_CLAMP_MAX (65535L << 8)
#endif
#define FIXED_HUGE_AREA (1LL << 51)
#define FIXED_TERM_LIMIT (127LL << 24)

/* triangles set up in integers from the corners' 16.16 attributes */
#ifndef NSPIRE_FIXED_SETUP
#define NSPIRE_FIXED_SETUP 1
#endif

/* twice the area, in pixels, below which a triangle is one colour (the
average of its corners): it covers a pixel or two */
#define SMALL_TRIANGLE_AREA 4.0f

/* twice the area, in pixels, from which a triangle shades by 2x2 blocks */
#define BLOCK_SHADING_AREA 512.0f

/* the colours shaded for the current pair of rows, a block of two pixels
each, and which triangle and row pair (block_stamp) shaded them */
static long block_colors[WIDTH / 2][4];
/* a fast layer's (fill_triangle) blend for each block: the result is
(terms[0..2] + destination * terms[3..5]) >> 8 for red, green and blue,
or, when it does not depend on the destination (an opaque block), the
pixel itself in terms[6] (-1 otherwise) */
static long block_terms[WIDTH / 2][7];
/* a multiplying layer's (the level's lightmap pass) blocks over the whole
low target, shared by all the draw's triangles: soft light, shaded once a
block whatever the triangles' sizes */
#ifndef NSPIRE_SHARED_LAYER_BLOCKS
#define NSPIRE_SHARED_LAYER_BLOCKS 1
#endif
static long shared_terms[LOW_HEIGHT / 2][LOW_WIDTH / 2][7];
static unsigned long shared_stamps[LOW_HEIGHT / 2][LOW_WIDTH / 2];
static unsigned long shared_stamp;
/* an opaque draw's (the level's, models') colours by 2x2 block over the
whole low target, shared by its triangles where the depths agree (a
surface behind does not lend its colour): with the depth shaded at */
#ifndef NSPIRE_SHARED_OPAQUE
#define NSPIRE_SHARED_OPAQUE 1
#endif
#ifndef SHARED_DEPTH_TOLERANCE
#define SHARED_DEPTH_TOLERANCE 64UL
#endif
static long shared_opaque_colors[LOW_HEIGHT / 2][LOW_WIDTH / 2][4];
static unsigned long shared_opaque_depths[LOW_HEIGHT / 2][LOW_WIDTH / 2];
static unsigned long shared_opaque_stamps[LOW_HEIGHT / 2][LOW_WIDTH / 2];
/* the first-person weapon and hands, every triangle of them shaded by its
own 2x2 blocks, however small (they are close and many-staged: most of a
frame's shading); depth and coverage stay per pixel. (Sharing blocks across
triangles took a covered triangle's colour.) */
#ifndef NSPIRE_FIRST_PERSON_BLOCKS
#define NSPIRE_FIRST_PERSON_BLOCKS 1
#endif
static BOOL first_person_drawing;

/* The first-person weapon and hands drawn every NSPIRE_FIRST_PERSON_EVERY
frames (they are a third of the renderer's work): a frame that draws them
keeps the pixels they changed (colour and depth, the 3D view's, found by
setting the view before them against after), and the next pastes those
instead of drawing them, so their animation moves at half the rate. The
depth pasted too keeps the level's later passes (its lightmap, testing for
equal depth) off them, as when drawn. */
#ifndef NSPIRE_FIRST_PERSON_EVERY
#define NSPIRE_FIRST_PERSON_EVERY 3
#endif
#define FIRST_PERSON_PIXELS (LOW_WIDTH * LOW_HEIGHT)
/* before the drawing: the view; after: the kept pixels' colour and depth */
static unsigned short *first_person_colors, *first_person_depths;
static unsigned long first_person_mask[(FIRST_PERSON_PIXELS + 31) / 32];
/* the frame (part_frame) the pixels were kept in, and the frames since the
last drawing */
static unsigned long first_person_kept_frame;
static BOOL first_person_kept, first_person_recording;

static void first_person_paste(void)
{
	unsigned long word;

	for (word = 0; word < (FIRST_PERSON_PIXELS + 31) / 32; word++)
	{
		unsigned long bits = first_person_mask[word];

		while (bits)
		{
			unsigned long pixel = word * 32 + (unsigned long)__builtin_ctzl(bits);

			bits &= bits - 1;
			low_pixels[pixel] = first_person_colors[pixel];
			depth_buffer[pixel] = first_person_depths[pixel];
		}
	}
	depth_written = TRUE;
}

static void first_person_keep(void)
{
	unsigned long pixel;

	memset(first_person_mask, 0, sizeof(first_person_mask));
	for (pixel = 0; pixel < FIRST_PERSON_PIXELS; pixel++)
	{
		if (low_pixels[pixel] != first_person_colors[pixel] || depth_buffer[pixel] != first_person_depths[pixel])
		{
			first_person_mask[pixel >> 5] |= 1UL << (pixel & 31);
			first_person_colors[pixel] = low_pixels[pixel];
			first_person_depths[pixel] = depth_buffer[pixel];
		}
	}
	first_person_kept = TRUE;
	first_person_kept_frame = part_frame;
}

/* the engine drawing the first-person weapon and hands, or done
(source/render/render_objects.c); at the start, whether to draw them (else
this frame has the last drawing's pixels pasted) */
/* The HUD (interface_draw_screen: the meters, the motion sensor, the
messages), drawn every NSPIRE_HUD_EVERY frames on the 320x240 screen and its
pixels pasted between, as the first-person weapon's are: what it changed
found by comparing the screen before and after it. Under its translucent
parts the world is then a frame old. */
#ifndef NSPIRE_HUD_EVERY
#define NSPIRE_HUD_EVERY 3
#endif
#define HUD_PIXELS (WIDTH * HEIGHT)
static unsigned short *hud_before, *hud_colors;
static unsigned long hud_mask[(HUD_PIXELS + 31) / 32];
static unsigned long hud_kept_frame;
static BOOL hud_kept, hud_recording;

int soft_rasterizer_hud(int drawing)
{
	unsigned short *screen = nspire_video_pixels();
	unsigned long pixel, word;

	if (NSPIRE_HUD_EVERY < 2 || !screen)
		return 1;
	if (!hud_before)
	{
		hud_before = malloc(HUD_PIXELS * sizeof(unsigned short));
		hud_colors = malloc(HUD_PIXELS * sizeof(unsigned short));
		if (!hud_before || !hud_colors)
		{
			free(hud_before);
			free(hud_colors);
			hud_before = hud_colors = NULL;
			return 1;
		}
	}
	if (drawing)
	{
		if (hud_kept && part_frame - hud_kept_frame < NSPIRE_HUD_EVERY)
		{
			/* (the last drawing's pixels, pasted) */
			for (word = 0; word < (HUD_PIXELS + 31) / 32; word++)
			{
				unsigned long bits = hud_mask[word];

				while (bits)
				{
					int bit = __builtin_ctzl(bits);

					bits &= bits - 1;
					pixel = word * 32 + bit;
					screen[pixel] = hud_colors[pixel];
				}
			}
			return 0;
		}
		memcpy(hud_before, screen, HUD_PIXELS * sizeof(unsigned short));
		hud_recording = TRUE;
		return 1;
	}
	if (!hud_recording)
		return 1;
	hud_recording = FALSE;
	memset(hud_mask, 0, sizeof(hud_mask));
	for (pixel = 0; pixel < HUD_PIXELS; pixel++)
	{
		if (screen[pixel] != hud_before[pixel])
		{
			hud_mask[pixel >> 5] |= 1UL << (pixel & 31);
			hud_colors[pixel] = screen[pixel];
		}
	}
	hud_kept = TRUE;
	hud_kept_frame = part_frame;
	return 1;
}

int soft_rasterizer_first_person(int drawing)
{
	int draw = TRUE;

	first_person_drawing = drawing != 0;
#if !defined(NSPIRE_REPLAY) || defined(REPLAY_FIRST_PERSON_CACHE)
	if (NSPIRE_FIRST_PERSON_EVERY > 1 && target == &low_target)
	{
		if (!first_person_colors)
		{
			first_person_colors = malloc(FIRST_PERSON_PIXELS * sizeof(unsigned short));
			first_person_depths = malloc(FIRST_PERSON_PIXELS * sizeof(unsigned short));
		}
		if (drawing && first_person_colors && first_person_depths)
		{
			if (first_person_kept && part_frame - first_person_kept_frame < NSPIRE_FIRST_PERSON_EVERY &&
				!soft_capture_active())
			{
				first_person_paste();
				first_person_drawing = FALSE;
				return FALSE;
			}
			memcpy(first_person_colors, low_pixels, FIRST_PERSON_PIXELS * sizeof(unsigned short));
			memcpy(first_person_depths, depth_buffer, FIRST_PERSON_PIXELS * sizeof(unsigned short));
			first_person_recording = TRUE;
		}
		else if (!drawing && first_person_recording)
		{
			first_person_keep();
			first_person_recording = FALSE;
		}
	}
#endif
	return draw;
}
static unsigned long block_stamps[WIDTH / 2];
static unsigned long block_stamp, triangle_serial;
static BOOL block_shading;
/* log2 of the block size: 1 (2x2), or 2 (4x4) for the background (the sky,
drawn before anything writes depth and mostly covered afterwards) */
static int block_shift;

/* project's terms for the draw: the screen conversion with the target's
scale and the half-pixel shift folded in, and each texture stage's size */
static struct
{
	float x_scale, x_offset, y_scale, y_offset, z_scale, z_offset;
	float texture_scale[4][2];
	long texture_scale_fixed[4][2];
	/* the scales and offsets in 16.16, for projecting in integers */
	long long x_scale_fixed, x_offset_fixed, y_scale_fixed, y_offset_fixed, z_scale_fixed, z_offset_fixed;
} projection;

static void projection_prepare(void)
{
	float z_factor = state.z_normalize * (1.0f / 256.0f);
	int index;

	projection.x_scale = screen_scale[0] * target->factor;
	projection.x_offset = (screen_offset[0] + 0.5f) * target->factor;
	projection.y_scale = screen_scale[1] * target->factor;
	projection.y_offset = (screen_offset[1] + 0.5f) * target->factor;
	projection.z_scale = screen_scale[2] * z_factor;
	projection.z_offset = screen_offset[2] * z_factor;
	projection.x_scale_fixed = fixed16(projection.x_scale);
	projection.x_offset_fixed = fixed16(projection.x_offset);
	projection.y_scale_fixed = fixed16(projection.y_scale);
	projection.y_offset_fixed = fixed16(projection.y_offset);
	projection.z_scale_fixed = fixed16(projection.z_scale);
	projection.z_offset_fixed = fixed16(projection.z_offset);
	for (index = 0; index < state.stage_count; index++)
	{
		int s = state.stages[index];
		/* in texels for a texture sampled, unscaled otherwise */
		BOOL texels = samplers[s].texels != NULL;

		projection.texture_scale[index][0] = texels ? (float)samplers[s].base_width : 1.0f;
		projection.texture_scale[index][1] = texels ? (float)samplers[s].base_height : 1.0f;
		projection.texture_scale_fixed[index][0] = texels ? (long)samplers[s].base_width : 1;
		projection.texture_scale_fixed[index][1] = texels ? (long)samplers[s].base_height : 1;
	}
}

static void project_float(const struct clip_vertex *in, struct screen_vertex *out);

/* a 64-bit value saturated to 32 bits */
static __inline__ __attribute__((always_inline)) long saturate32(long long value)
{
	return value > 0x7FFFFFFFLL ? 0x7FFFFFFFL : value < -0x7FFFFFFFLL ? -0x7FFFFFFFL : (long)value;
}

/* project's position from a 16.16 clip position (w above zero), in
integers: 1 / w from one 32-bit division and a Newton step (about 30 bits),
then each axis a 64-bit product */
static void project_fixed_position(const long c[4], struct screen_vertex *out, long *z)
{
	unsigned long w = (unsigned long)c[3], normalized, top;
	int n = __builtin_clzl(w);
	unsigned long long reciprocal, product;
	long long error;
	int shift;

	/* 2^62 / normalized, the normalized w in [2^31, 2^32) */
	normalized = w << n;
	top = normalized >> 16;
	reciprocal = (unsigned long long)(0xFFFFFFFFUL / top) << 14;
	product = (unsigned long long)normalized * reciprocal;
	error = (long long)((1ULL << 62) - product);
	reciprocal = (unsigned long long)((long long)reciprocal + (((long long)reciprocal * (error >> 31)) >> 31));
	/* each axis over w, in 16.16: c * reciprocal * 2^(n - 62 + 16) */
	shift = 46 - n;
	{
		long long x = ((long long)c[0] * (long long)reciprocal) >> shift;
		long long y = ((long long)c[1] * (long long)reciprocal) >> shift;
		/* (depth over w with 24 fraction bits: the far depths crowd together) */
		long long depth = ((long long)c[2] * (long long)reciprocal) >> (shift - 8);

		out->fixed_x = saturate32(((x * projection.x_scale_fixed) >> 16) + projection.x_offset_fixed);
		out->fixed_y = saturate32(((y * projection.y_scale_fixed) >> 16) + projection.y_offset_fixed);
		depth = ((depth * projection.z_scale_fixed) >> 24) + projection.z_offset_fixed;
		/* (z over 256, held to 0 .. 65535 / 256) */
		*z = depth < 0 ? 0 : depth > DEPTH_CLAMP_MAX ? DEPTH_CLAMP_MAX : (long)depth;
	}
}

#ifndef NSPIRE_COLOR_BITS
#define NSPIRE_COLOR_BITS 1
#endif
/* fixed16(c * ONE) for a colour channel c in [0, 1] (unit_clamp's), from
its bits: the soft-float multiply and conversion cost a hundred cycles */
static __inline__ __attribute__((always_inline)) long unit_to_fixed_one(float c)
{
	union { float f; unsigned long u; } bits;
	long exponent, mantissa, shift;

	bits.f = c;
	exponent = (long)((bits.u >> 23) & 0xFF);
	if (!exponent || (bits.u >> 31))
		return 0;
	mantissa = (long)((bits.u & 0x7FFFFFUL) | 0x800000UL);
	/* (c * ONE * 65536 = mantissa * 2^(exponent - 127 - 23) * ONE * 2^16) */
	shift = exponent - 127 - 23 + 16 + ONE_SHIFT;
	if (shift >= 0)
		return mantissa << shift;
	return shift <= -31 ? 0 : mantissa >> -shift;
}

static void project(const struct clip_vertex *in, struct screen_vertex *out)
{
	if (in->clip_is_fixed)
	{
		/* (in integers: the position, then the colour and the textures as
		below) */
		long *fixed = out->fixed;
		int index;

		project_fixed_position(in->clip_fixed, out, fixed++);
		if (state.uses_diffuse)
		{
			int channel;

			for (channel = 0; channel < 4; channel++)
				*fixed++ = NSPIRE_COLOR_BITS ? unit_to_fixed_one(in->color[channel]) : fixed16(in->color[channel] * (float)ONE);
		}
#ifdef CHECK_PROJECTION
		{
			struct screen_vertex reference;
			static int logged;

			project_float(in, &reference);
			if ((labs(reference.fixed_x - out->fixed_x) > 256 || labs(reference.fixed_y - out->fixed_y) > 256 ||
				labs(reference.fixed[0] - out->fixed[0]) > 2) && logged++ < 10)
			{
				nspire_log("    projection: fixed %ld %ld %ld float %ld %ld %ld; clip %ld %ld %ld %ld",
					out->fixed_x, out->fixed_y, out->fixed[0], reference.fixed_x, reference.fixed_y, reference.fixed[0],
					in->clip_fixed[0], in->clip_fixed[1], in->clip_fixed[2], in->clip_fixed[3]);
			}
		}
#endif
		for (index = 0; index < state.stage_count; index++)
		{
			int s = state.stages[index];

			if (state.stage_kinds[index] == _stage_grey)
				continue;
			if (in->texture_is_fixed)
			{
				long long u = (long long)in->texture_fixed[s][0] * projection.texture_scale_fixed[index][0];
				long long v = (long long)in->texture_fixed[s][1] * projection.texture_scale_fixed[index][1];

				*fixed++ = saturate32(u);
				*fixed++ = saturate32(v);
			}
			else
			{
				*fixed++ = fixed16(in->texture[s][0] * projection.texture_scale[index][0]);
				*fixed++ = fixed16(in->texture[s][1] * projection.texture_scale[index][1]);
			}
		}
		return;
	}
	project_float(in, out);
}

static void project_float(const struct clip_vertex *in, struct screen_vertex *out)
{
	float inverse_w = 1.0f / in->clip[3];
	/* (the attributes but the textures' in floating point first) */
	float attributes[5], *attribute = attributes;
	float z;
	int channel, index;

	/* the game's 640x480, whose pixel centres are on integers, to the
	screen's 320x240 with centres on halves */
	out->fixed_x = fixed16(in->clip[0] * inverse_w * projection.x_scale + projection.x_offset);
	out->fixed_y = fixed16(in->clip[1] * inverse_w * projection.y_scale + projection.y_offset);
	/* attributes: z (over 256, so its 16.16 fits), r g b a (0..256), then u
	v of each texture stage sampled (in texels) */
	z = in->clip[2] * inverse_w * projection.z_scale + projection.z_offset;
	if (z < 0.0f) z = 0.0f;
	if (z > (float)DEPTH_CLAMP_MAX / 65536.0f) z = (float)DEPTH_CLAMP_MAX / 65536.0f;
	*attribute++ = z;
	if (state.uses_diffuse)
	{
		for (channel = 0; channel < 4; channel++)
			*attribute++ = in->color[channel] * (float)ONE;
	}
	{
		long *fixed = out->fixed;
		int count = (int)(attribute - attributes);

		for (index = 0; index < count; index++)
			fixed[index] = fixed16(attributes[index]);
		fixed += count;
		for (index = 0; index < state.stage_count; index++)
		{
			int s = state.stages[index];

			if (state.stage_kinds[index] == _stage_grey)
				continue;
			if (in->texture_is_fixed)
			{
				/* (in integers: u and v in 16.16 times the texture's size) */
				long long u = (long long)in->texture_fixed[s][0] * projection.texture_scale_fixed[index][0];
				long long v = (long long)in->texture_fixed[s][1] * projection.texture_scale_fixed[index][1];

				*fixed++ = u > 0x7FFFFFFFLL ? 0x7FFFFFFFL : u < -0x7FFFFFFFLL ? -0x7FFFFFFFL : (long)u;
				*fixed++ = v > 0x7FFFFFFFLL ? 0x7FFFFFFFL : v < -0x7FFFFFFFLL ? -0x7FFFFFFFL : (long)v;
			}
			else
			{
				*fixed++ = fixed16(in->texture[s][0] * projection.texture_scale[index][0]);
				*fixed++ = fixed16(in->texture[s][1] * projection.texture_scale[index][1]);
			}
		}
	}
}


/* an edge's x at the first scanline centre from y_start and its step per
scanline, in 16.16 */
static void edge_setup(const struct screen_vertex *top, const struct screen_vertex *bottom, long y_start,
	long *x, long *step)
{
	/* from the corners in 16.16: one division in floating point, the rest
	in integers */
	long dy = bottom->fixed_y - top->fixed_y;
	long slope = dy > 0 ? fixed16((float)(bottom->fixed_x - top->fixed_x) / (float)dy) : 0;
	long offset = (y_start << 16) + 0x8000 - top->fixed_y;

	*x = top->fixed_x + (long)(((long long)slope * offset) >> 16);
	*step = slope;
}

/* (measuring: the combiners run twice, or the whole shading) */
#if defined(EXPERIMENT_COMBINE_TWICE)
#define SHADE_AGAIN() (local_shader ? local_shader(registers, color) : combine_compiled(registers, color))
#else
#define SHADE_AGAIN() ((void)0)
#endif

#ifdef EXPERIMENT_NO_SAMPLE
#define SAMPLE(sampler, u, v, texel) ((texel)[0] = (texel)[1] = (texel)[2] = (texel)[3] = 200)
#else
#define SAMPLE(sampler, u, v, texel) sample(sampler, u, v, texel)
#endif

#ifdef COUNT_SHADES
unsigned long loop_pixels[3];
unsigned long shade_count;
#define SHADE_COUNT() (shade_count++)
#else
#define SHADE_COUNT() ((void)0)
#endif

/* fill_triangle's shading of one pixel (its registers, values, attributes
and draw settings in locals) into color */
#ifdef EXPERIMENT_NO_SHADE
#define SHADE_PIXEL(color) ((color)[0] = (color)[1] = (color)[2] = 128, (color)[3] = 256)
#else
#define SHADE_PIXEL(color) \
	do \
	{ \
		int attribute = 1, stage; \
		SHADE_COUNT(); \
		unsigned long written = local_written_registers; \
		while (written) \
		{ \
			int reg = __builtin_ctzl(written); \
			registers[reg][0] = registers[reg][1] = registers[reg][2] = registers[reg][3] = 0; \
			written &= written - 1; \
		} \
		if (local_constant_diffuse) \
		{ \
			/* (the combiners may have written over it) */ \
			registers[_register_v0][0] = local_diffuse[0]; \
			registers[_register_v0][1] = local_diffuse[1]; \
			registers[_register_v0][2] = local_diffuse[2]; \
			registers[_register_v0][3] = local_diffuse[3]; \
		} \
		if (local_uses_diffuse) \
		{ \
			for (k = 0; k < 4; k++) \
			{ \
				long channel = values[attribute + k] >> 16; \
				registers[_register_v0][k] = channel < 0 ? 0 : channel > ONE ? ONE : channel; \
			} \
			attribute += 4; \
		} \
		for (stage = 0; stage < local_stage_count; stage++) \
		{ \
			int s = local_stages[stage]; \
			long *texel = registers[_register_t0 + s]; \
			switch (local_stage_kinds[stage]) \
			{ \
			case _stage_sample: \
				SAMPLE(&samplers[s], values[attribute], values[attribute + 1], texel); \
				break; \
			case _stage_passthrough: \
				texel[0] = clamp_unit(values[attribute] >> 8); \
				texel[1] = clamp_unit(values[attribute + 1] >> 8); \
				texel[2] = 0; \
				texel[3] = ONE; \
				break; \
			default: \
				texel[0] = texel[1] = texel[2] = ONE / 2; \
				texel[3] = ONE; \
				attribute -= 2; \
				break; \
			} \
			attribute += 2; \
		} \
		registers[_register_r0][0] = registers[_register_r0][1] = registers[_register_r0][2] = 0; \
		registers[_register_r0][3] = registers[_register_t0][3]; \
		if (local_shader) \
			local_shader(registers, color); \
		else \
			combine_compiled(registers, color); \
		SHADE_AGAIN(); \
	} \
	while (0)
#endif

#ifdef DEBUG_TRIANGLES
static int debug_culled_logged;
#ifndef DEBUG_DRAW
#define DEBUG_DRAW 11
#endif
#endif

/* the row's values but depth (fill_triangle's fast loops), once */
#define ROW_VALUES_ENSURE() \
	do \
	{ \
		if (!row_values_ready) \
		{ \
			for (k = 1; k < count; k++) \
			{ \
				long long value = origin[k] + (long long)step_x[k] * (2 * x_begin + 1) / 2 + \
					(long long)step_y[k] * (2 * y + 1) / 2; \
				row_values[k] = value > 0x7FFFFFFFLL ? 0x7FFFFFFF : value < -0x7FFFFFFFLL ? -0x7FFFFFFF : (long)value; \
			} \
			row_values_ready = TRUE; \
		} \
	} \
	while (0)

static void fill_triangle(const struct screen_vertex *v0, const struct screen_vertex *v1, const struct screen_vertex *v2)
{
	const struct screen_vertex *sorted[3] = { v0, v1, v2 }, *swap;
	long long origin[13];
	long step_x[13], step_y[13];
	long registers[NUMBER_OF_REGISTERS][4];
	float area, inverse_area;
	long long fixed_area;
	long long fa_x, fa_y, fb_x, fb_y;
	long level_speed[13];
	BOOL flat_triangle;
	long x0_fixed, y0_fixed;
	long y, y_start, y_middle, y_end;
	long long_x, long_step, short_x, short_step;
	BOOL long_on_left;
	int k, count = state.attribute_count;

	/* twice the area, in integers from the corners in 16.16 (32.32 here):
	the screen's y goes down, so a positive area is clockwise. Triangles
	turned away or thin go no further, nor any larger than all the guard
	band holds (corners at infinity, which clipping can leave) */
	fixed_area = (long long)(v1->fixed_x - v0->fixed_x) * (v2->fixed_y - v0->fixed_y) -
		(long long)(v2->fixed_x - v0->fixed_x) * (v1->fixed_y - v0->fixed_y);
	if ((fixed_area > -FIXED_THIN_AREA && fixed_area < FIXED_THIN_AREA) ||
		fixed_area >= FIXED_HUGE_AREA || fixed_area <= -FIXED_HUGE_AREA ||
		(state.cull_mode == D3DCULL_CCW && fixed_area < 0) || (state.cull_mode == D3DCULL_CW && fixed_area > 0))
	{
#ifdef DEBUG_TRIANGLES
		if (log_draws && draw_number == DEBUG_DRAW && debug_culled_logged < 8)
		{
			debug_culled_logged++;
			nspire_log("    culled: area %ld/1000, cull mode %lu, corners %ld,%ld %ld,%ld %ld,%ld", (long)(area * 1000),
				(unsigned long)state.cull_mode, v0->fixed_x >> 16, v0->fixed_y >> 16, v1->fixed_x >> 16, v1->fixed_y >> 16,
				v2->fixed_x >> 16, v2->fixed_y >> 16);
		}
#endif
		counters.culled++;
		return;
	}
	/* (in floating point too, in square pixels, from 20 fraction bits) */
	area = (float)(long)(fixed_area >> 20) * (1.0f / 4096.0f);
	primitive_count++;
	counters.filled++;
	if (pass_record)
		pass_record[current_ordinal >> 5] |= 1UL << (current_ordinal & 31);
	FINE_PROFILE_BEGIN(_nspire_profile_triangle_setup);
	/* a large triangle shades once per 2x2 block of pixels (depth and
	coverage stay per pixel); small ones, like text, every pixel */
	if (target == &low_target)
	{
		/* at 160x120, by blocks: the background (the sky), and layers blended
		over the world without writing depth (effects, detail passes),
		which are soft anyway */
		BOOL blended_layer = state.blend_kind != _blend_copy && !state.z_write;

		block_shading = ((!depth_written && !state.z_test && !state.z_write) || blended_layer ||
			NSPIRE_OPAQUE_BLOCKS) && (area >= BLOCK_SHADING_AREA / 4 || area <= -BLOCK_SHADING_AREA / 4);
		if (NSPIRE_FIRST_PERSON_BLOCKS && first_person_drawing)
			block_shading = TRUE;
		/* (blended layers, soft as they are, by blocks from a smaller size) */
		if (blended_layer && (area >= BLOCKED_LAYER_AREA || area <= -BLOCKED_LAYER_AREA))
			block_shading = TRUE;
		if (NSPIRE_SHARED_LAYER_BLOCKS && blended_layer &&
			(state.blend_kind == _blend_multiply || state.blend_kind == _blend_multiply2x))
		{
			block_shading = TRUE;
		}
		/* (2x2: 4x4 made the sand blowing over the beach visibly blocky, for 3%
		of the frame; the sky, behind everything, by 4x4 when SKY_BLOCK_SHIFT
		says so) */
		block_shift = !depth_written && !state.z_test && !state.z_write ? SKY_BLOCK_SHIFT : 1;
	}
	else
	{
		block_shading = area >= BLOCK_SHADING_AREA || area <= -BLOCK_SHADING_AREA;
		block_shift = 1;
	}
	triangle_serial++;
	FINE_PROFILE_BEGIN(_nspire_profile_setup_planes);
	inverse_area = 1.0f / area;

	/* each attribute as a plane: its value at the screen's origin and its
	steps across and down, in 16.16; the four terms (the corners' differences
	over the area) in 8.24, from one division */
	{
		/* (the area's inverse times 2^24, in 16.16: at most 100 times 2^24) */
		long long inverse = fixed16(inverse_area * 256.0f);

		fa_x = ((long long)(v2->fixed_y - v0->fixed_y) * inverse) >> 16;
		fa_y = ((long long)(v1->fixed_y - v0->fixed_y) * inverse) >> 16;
		fb_x = ((long long)(v1->fixed_x - v0->fixed_x) * inverse) >> 16;
		fb_y = ((long long)(v2->fixed_x - v0->fixed_x) * inverse) >> 16;
	}
	x0_fixed = v0->fixed_x;
	y0_fixed = v0->fixed_y;
	if (area < SMALL_TRIANGLE_AREA && area > -SMALL_TRIANGLE_AREA)
	{
		/* a pixel or two (distant models are made of them): the average of
		its corners all over, and the textures' levels from how far their
		coordinates spread across it */
		for (k = 0; k < count; k++)
		{
			long a0 = v0->fixed[k], a1 = v1->fixed[k], a2 = v2->fixed[k];
			long spread1 = a1 - a0, spread2 = a2 - a0;

			if (spread1 < 0) spread1 = -spread1;
			if (spread2 < 0) spread2 = -spread2;
			step_x[k] = 0;
			step_y[k] = 0;
			level_speed[k] = spread1 > spread2 ? spread1 : spread2;
			origin[k] = (((long long)a0 + a1 + a2) * 21846) >> 16;
		}
		flat_triangle = TRUE;
	}
	else if (NSPIRE_FIXED_SETUP && fa_x < FIXED_TERM_LIMIT && fa_x > -FIXED_TERM_LIMIT &&
		fa_y < FIXED_TERM_LIMIT && fa_y > -FIXED_TERM_LIMIT && fb_x < FIXED_TERM_LIMIT && fb_x > -FIXED_TERM_LIMIT &&
		fb_y < FIXED_TERM_LIMIT && fb_y > -FIXED_TERM_LIMIT)
	{
		/* the planes in integers: the four terms in 8.24, each attribute's
		differences in 16.16 */

		for (k = 0; k < count; k++)
		{
			long a0 = v0->fixed[k];
			long d1 = v1->fixed[k] - a0, d2 = v2->fixed[k] - a0;

			step_x[k] = (long)(((long long)d1 * fa_x - (long long)d2 * fa_y) >> 24);
			step_y[k] = (long)(((long long)d2 * fb_x - (long long)d1 * fb_y) >> 24);
			origin[k] = (long long)a0 - (((long long)step_x[k] * x0_fixed) >> 16) -
				(((long long)step_y[k] * y0_fixed) >> 16);
		}
		flat_triangle = FALSE;
	}
	else
	{
		/* (terms too large for 8.24: in floating point) */
		/* (from the corners in 16.16: project_fixed_position gives no floats) */
		float scale = inverse_area * (1.0f / 65536.0f);
		/* (the differences in 64 bits: saturated corners would wrap) */
		float a_x = (float)((long long)v2->fixed_y - v0->fixed_y) * scale;
		float a_y = (float)((long long)v1->fixed_y - v0->fixed_y) * scale;
		float b_x = (float)((long long)v1->fixed_x - v0->fixed_x) * scale;
		float b_y = (float)((long long)v2->fixed_x - v0->fixed_x) * scale;

		for (k = 0; k < count; k++)
		{
			/* (from the 16.16 values: project gives no others) */
			float a0 = (float)v0->fixed[k] * (1.0f / 65536.0f);
			float d1 = (float)(v1->fixed[k] - v0->fixed[k]) * (1.0f / 65536.0f);
			float d2 = (float)(v2->fixed[k] - v0->fixed[k]) * (1.0f / 65536.0f);

			step_x[k] = fixed16(d1 * a_x - d2 * a_y);
			step_y[k] = fixed16(d2 * b_x - d1 * b_y);
			origin[k] = (long long)fixed16(a0) - (((long long)step_x[k] * x0_fixed) >> 16) -
				(((long long)step_y[k] * y0_fixed) >> 16);
		}
		flat_triangle = FALSE;
	}
	FINE_PROFILE_END(_nspire_profile_setup_planes);
	FINE_PROFILE_BEGIN(_nspire_profile_setup_levels);
	/* each texture's level, from how fast its coordinates move */
	{
		int index, attribute = state.uses_diffuse ? 5 : 1;

		for (index = 0; index < state.stage_count; index++)
		{
			struct sampler *sampler = &samplers[state.stages[index]];

			if (state.stage_kinds[index] == _stage_grey)
				continue;
			if (state.stage_kinds[index] == _stage_sample && sampler->texels && sampler->levels > 1)
			{
				unsigned long fastest = 0, speed;
				int k2;

				for (k2 = attribute; k2 < attribute + 2; k2++)
				{
					if (flat_triangle)
					{
						speed = (unsigned long)level_speed[k2];
						if (speed > fastest) fastest = speed;
						continue;
					}
					speed = (unsigned long)(step_x[k2] < 0 ? -step_x[k2] : step_x[k2]);
					if (speed > fastest) fastest = speed;
					speed = (unsigned long)(step_y[k2] < 0 ? -step_y[k2] : step_y[k2]);
					if (speed > fastest) fastest = speed;
				}
				sampler_select_level(sampler, fastest);
			}
			attribute += 2;
		}
	}

	FINE_PROFILE_END(_nspire_profile_setup_levels);
	FINE_PROFILE_BEGIN(_nspire_profile_setup_edges);
	if (sorted[1]->fixed_y < sorted[0]->fixed_y) { swap = sorted[0]; sorted[0] = sorted[1]; sorted[1] = swap; }
	if (sorted[2]->fixed_y < sorted[1]->fixed_y) { swap = sorted[1]; sorted[1] = sorted[2]; sorted[2] = swap; }
	if (sorted[1]->fixed_y < sorted[0]->fixed_y) { swap = sorted[0]; sorted[0] = sorted[1]; sorted[1] = swap; }

	/* the first scanline centre at or below each vertex (in integers) */
	y_start = (sorted[0]->fixed_y - 0x8000 + 0xFFFF) >> 16;
	y_middle = (sorted[1]->fixed_y - 0x8000 + 0xFFFF) >> 16;
	y_end = (sorted[2]->fixed_y - 0x8000 + 0xFFFF) >> 16;
	if (y_start >= y_end)
	{
		FINE_PROFILE_END(_nspire_profile_setup_edges);
		FINE_PROFILE_END(_nspire_profile_triangle_setup);
		return;
	}
	/* (the rows on the target only: a triangle clipping leaves huge, from
	vertices far behind the eye, walked tens of thousands above it) */
	if (y_start < 0)
		y_start = 0;
	if (y_end > target->height)
		y_end = target->height;
	if (y_start >= y_end)
	{
		FINE_PROFILE_END(_nspire_profile_setup_edges);
		FINE_PROFILE_END(_nspire_profile_triangle_setup);
		return;
	}
	edge_setup(sorted[0], sorted[2], y_start, &long_x, &long_step);
	/* which side the long edge is on: where it passes the middle vertex */
	long_on_left = (long long)(sorted[2]->fixed_x - sorted[0]->fixed_x) * (sorted[1]->fixed_y - sorted[0]->fixed_y) <
		(long long)(sorted[1]->fixed_x - sorted[0]->fixed_x) * (sorted[2]->fixed_y - sorted[0]->fixed_y);
	if (y_middle > y_start)
		edge_setup(sorted[0], sorted[1], y_start, &short_x, &short_step);
	else
		edge_setup(sorted[1], sorted[2], y_start, &short_x, &short_step);

	FINE_PROFILE_END(_nspire_profile_setup_edges);
	memset(registers, 0, sizeof(registers));
	registers[_register_fog][3] = ONE;
	if (state.constant_diffuse)
	{
		registers[_register_v0][0] = state.diffuse[0];
		registers[_register_v0][1] = state.diffuse[1];
		registers[_register_v0][2] = state.diffuse[2];
		registers[_register_v0][3] = state.diffuse[3];
	}
	else
	{
		registers[_register_v0][0] = registers[_register_v0][1] = registers[_register_v0][2] =
			registers[_register_v0][3] = ONE;
	}
	FINE_PROFILE_END(_nspire_profile_triangle_setup);
	FINE_PROFILE_BEGIN(_nspire_profile_spans);
	/* the draw's settings in locals for the pixel loop: through the global
	the compiler must read them again after every store (the build allows
	any pointer to alias any other) */
	const BOOL local_z_test = state.z_test, local_z_write = state.z_write, local_alpha_test = state.alpha_test;
	const BOOL local_alpha_write = state.alpha_write, local_blend = state.blend, local_color_write = state.color_write;
	const BOOL local_constant_color = state.constant_color, local_constant_diffuse = state.constant_diffuse;
	const BOOL local_reads_destination_alpha = state.reads_destination_alpha, local_uses_diffuse = state.uses_diffuse;
	const BOOL local_visibility = soft_device.visibility_test_active, local_block_shading = block_shading;
	const DWORD local_z_function = state.z_function, local_alpha_function = state.alpha_function;
	const DWORD local_source_blend = state.source_blend, local_destination_blend = state.destination_blend;
	const unsigned long local_alpha_reference = state.alpha_reference, local_written_registers = state.written_registers;
	const int local_blend_kind = state.blend_kind, local_stage_count = state.stage_count, local_block_shift = block_shift;
	long local_color[4], local_diffuse[4];
	void (*const local_shader)(long registers[NUMBER_OF_REGISTERS][4], long out[4]) = state.shader;
	int local_stages[4], local_stage_kinds[4];

	memcpy(local_color, state.color, sizeof(local_color));
	memcpy(local_diffuse, state.diffuse, sizeof(local_diffuse));
	memcpy(local_stages, state.stages, sizeof(local_stages));
	memcpy(local_stage_kinds, state.stage_kinds, sizeof(local_stage_kinds));
	const BOOL fast_layer = NSPIRE_FAST_LAYERS && local_block_shading && !local_constant_color && !local_alpha_test &&
		!local_z_write && local_color_write &&
		!(local_alpha_write && target->keeps_alpha) &&
		(!local_z_test || local_z_function == D3DCMP_LESS || local_z_function == D3DCMP_LESSEQUAL ||
			local_z_function == D3DCMP_EQUAL) &&
		local_blend_kind >= _blend_alpha && local_blend_kind <= _blend_premultiplied;
	/* (no colour, no alpha and no alpha test: only depth matters) */
	const BOOL depth_only = !local_color_write && !(local_alpha_write && target->keeps_alpha) && !local_alpha_test;
	/* (as the general loop does for these, but with its other cases left out) */
	const BOOL fast_opaque = NSPIRE_FAST_OPAQUE && !fast_layer && local_blend_kind == _blend_copy &&
		!local_visibility && local_color_write && !(local_alpha_write && target->keeps_alpha) &&
		(!local_z_test || local_z_function == D3DCMP_LESS || local_z_function == D3DCMP_LESSEQUAL);
	/* (not alpha tested: leaves cut out by blocks lose their shapes) */
	const BOOL shared_opaque = fast_opaque && NSPIRE_SHARED_OPAQUE && target == &low_target &&
		!local_constant_color && !local_alpha_test;
	const BOOL shared_layer = fast_layer && NSPIRE_SHARED_LAYER_BLOCKS && target == &low_target &&
		local_block_shift == 1 && (local_blend_kind == _blend_multiply || local_blend_kind == _blend_multiply2x);

	for (y = y_start; y < y_end; y++)
	{
		long left, right, x, x_begin, x_finish;
		long values[13], row_values[13];
		BOOL row_values_ready;
		unsigned short *pixel_row, *depth_row;
		unsigned char *alpha_row;
		int depth_shift = target->depth_shift;

		if (y == y_middle && y_middle > y_start)
			edge_setup(sorted[1], sorted[2], y, &short_x, &short_step);
		left = long_on_left ? long_x : short_x;
		right = long_on_left ? short_x : long_x;
		long_x += long_step;
		short_x += short_step;
		if (y < 0 || y >= target->height)
			continue;
		/* pixel x covers centres x + 0.5 from left to right */
		x_begin = (left - 0x8000 + 0xFFFF) >> 16;
		x_finish = (right - 0x8000 + 0xFFFF) >> 16;
		if (x_begin < 0) x_begin = 0;
		if (x_finish > target->width) x_finish = target->width;
		if (x_begin >= x_finish)
			continue;
		counters.pixels_tested += (unsigned long)(x_finish - x_begin);
#ifdef COUNT_SHADES
		loop_pixels[fast_opaque ? 0 : fast_layer ? 1 : 2] += (unsigned long)(x_finish - x_begin);
#endif
		/* (the fast loops step depth alone: the rest of the row's values
		are found when it first shades, ROW_VALUES_ENSURE) */
		row_values_ready = !(fast_layer || fast_opaque || depth_only);
		for (k = 0; k < (row_values_ready ? count : 1); k++)
		{
			long long value = origin[k] + (long long)step_x[k] * (2 * x_begin + 1) / 2 +
				(long long)step_y[k] * (2 * y + 1) / 2;

			values[k] = value > 0x7FFFFFFFLL ? 0x7FFFFFFF : value < -0x7FFFFFFFLL ? -0x7FFFFFFF : (long)value;
		}
		row_values[0] = values[0];
		pixel_row = target->pixels + y * target->width;
		depth_row = depth_buffer + (y >> target->depth_shift) * LOW_WIDTH;
		alpha_row = target->alpha + y * target->width;
		block_stamp = (triangle_serial << 8) | (unsigned long)(y >> block_shift);

		if (depth_only)
		{
			/* nothing drawn but depth, or pixels counted (a visibility test,
			lens flares'): no shading at all */
			long z_value = values[0], z_step = step_x[0];
			unsigned long depth_written_here = 0;

			for (x = x_begin; x < x_finish; x++, z_value += z_step)
			{
				unsigned long z = (unsigned long)(z_value < 0 ? 0 : z_value > DEPTH_CLAMP_MAX ? DEPTH_CLAMP_MAX :
					z_value) >> 8;

				if (local_z_test && !compare(local_z_function, z, depth_row[x >> depth_shift]))
					continue;
				if (local_visibility)
					soft_device.visibility_count++;
				if (local_z_write)
					depth_row[x >> depth_shift] = (unsigned short)z;
				depth_written_here++;
			}
			counters.pixels_written += depth_written_here;
			continue;
		}
		if (fast_opaque)
		{
			/* an opaque draw (the level, models): depth alone stepped per
			pixel, the rest found when a pixel (or its 2x2 block) is shaded */
			long z_value = values[0], z_step = step_x[0];
			unsigned long opaque_written = 0;
			long (*shared_row_colors)[4] = shared_opaque_colors[(y >> 1) % (LOW_HEIGHT / 2)];
			unsigned long *shared_row_depths = shared_opaque_depths[(y >> 1) % (LOW_HEIGHT / 2)];
			unsigned long *shared_row_stamps = shared_opaque_stamps[(y >> 1) % (LOW_HEIGHT / 2)];

			for (x = x_begin; x < x_finish; x++, z_value += z_step)
			{
				unsigned long z = (unsigned long)(z_value < 0 ? 0 : z_value > DEPTH_CLAMP_MAX ? DEPTH_CLAMP_MAX :
					z_value) >> 8;
				long color[4];

				if (local_z_test)
				{
					unsigned long depth = depth_row[x >> depth_shift];

					if (local_z_function == D3DCMP_LESS ? z >= depth : z > depth)
						continue;
				}
				if (local_constant_color)
				{
					color[0] = local_color[0];
					color[1] = local_color[1];
					color[2] = local_color[2];
					color[3] = local_color[3];
				}
				else if (shared_opaque)
				{
					unsigned long block = (unsigned long)x >> 1;
					unsigned long shaded_depth = shared_row_depths[block];

					if (shared_row_stamps[block] == shared_stamp &&
						(z > shaded_depth ? z - shaded_depth : shaded_depth - z) <= SHARED_DEPTH_TOLERANCE)
					{
						color[0] = shared_row_colors[block][0];
						color[1] = shared_row_colors[block][1];
						color[2] = shared_row_colors[block][2];
						color[3] = shared_row_colors[block][3];
					}
					else
					{
						long offset = x - x_begin;

						ROW_VALUES_ENSURE();
						for (k = 1; k < count; k++)
							values[k] = row_values[k] + step_x[k] * offset;
						SHADE_PIXEL(color);
						shared_row_colors[block][0] = color[0];
						shared_row_colors[block][1] = color[1];
						shared_row_colors[block][2] = color[2];
						shared_row_colors[block][3] = color[3];
						shared_row_depths[block] = z;
						shared_row_stamps[block] = shared_stamp;
					}
				}
				else
				{
					unsigned long block = (unsigned long)x >> local_block_shift;

					if (local_block_shading && block_stamps[block] == block_stamp)
					{
						color[0] = block_colors[block][0];
						color[1] = block_colors[block][1];
						color[2] = block_colors[block][2];
						color[3] = block_colors[block][3];
					}
					else
					{
						long offset = x - x_begin;

						ROW_VALUES_ENSURE();
						for (k = 1; k < count; k++)
							values[k] = row_values[k] + step_x[k] * offset;
						SHADE_PIXEL(color);
						if (local_block_shading)
						{
							block_colors[block][0] = color[0];
							block_colors[block][1] = color[1];
							block_colors[block][2] = color[2];
							block_colors[block][3] = color[3];
							block_stamps[block] = block_stamp;
						}
					}
				}
				if (local_alpha_test && !compare(local_alpha_function,
					(unsigned long)(color[3] >= ONE ? 255 : color[3]), local_alpha_reference))
				{
					continue;
				}
				pixel_row[x] = rgb565(color[0] > 255 ? 255 : color[0], color[1] > 255 ? 255 : color[1],
					color[2] > 255 ? 255 : color[2]);
				if (local_z_write)
					depth_row[x >> depth_shift] = (unsigned short)z;
				opaque_written++;
			}
			counters.pixels_written += opaque_written;
			continue;
		}
		if (fast_layer)
		{
			/* a blended layer shaded by blocks (particles, smoke): depth alone
			stepped per pixel, the rest found when a block is shaded, and
			each block's blend kept as terms */
			long z_value = values[0], z_step = step_x[0];
			unsigned long layer_written = 0;
			unsigned long *shared_row_stamps = shared_stamps[(y >> 1) % (LOW_HEIGHT / 2)];
			long (*shared_row_terms)[7] = shared_terms[(y >> 1) % (LOW_HEIGHT / 2)];
			unsigned long current_block = ~0UL;
			const long *current_terms = NULL;

			for (x = x_begin; x < x_finish; x++, z_value += z_step)
			{
				unsigned long z = (unsigned long)(z_value < 0 ? 0 : z_value > DEPTH_CLAMP_MAX ? DEPTH_CLAMP_MAX :
					z_value) >> 8;
				unsigned long block;
				const long *terms;
				unsigned long old;
				long red, green, blue;

				if (local_z_test)
				{
					unsigned long depth = depth_row[x >> depth_shift];

					if (local_z_function == D3DCMP_LESS ? z >= depth :
						local_z_function == D3DCMP_EQUAL ? z != depth : z > depth)
					{
						continue;
					}
				}
				if (local_visibility)
					soft_device.visibility_count++;
				block = (unsigned long)x >> local_block_shift;
				/* (the block of the pixel before, most often: looked up again
				only when the block changes) */
				if (block == current_block)
					terms = current_terms;
				else
				{
				if (shared_layer ? shared_row_stamps[block] != shared_stamp : block_stamps[block] != block_stamp)
				{
					long color[4], offset = x - x_begin;
					long *new_terms = shared_layer ? shared_row_terms[block] : block_terms[block];

					ROW_VALUES_ENSURE();
					for (k = 1; k < count; k++)
						values[k] = row_values[k] + step_x[k] * offset;
					SHADE_PIXEL(color);
					switch (local_blend_kind)
					{
					case _blend_alpha:
						for (k = 0; k < 3; k++)
						{
							new_terms[k] = color[k] * color[3];
							new_terms[3 + k] = ONE - color[3];
						}
						break;
					case _blend_premultiplied:
						for (k = 0; k < 3; k++)
						{
							new_terms[k] = color[k] * ONE;
							new_terms[3 + k] = ONE - color[3];
						}
						break;
					case _blend_add:
						for (k = 0; k < 3; k++)
						{
							new_terms[k] = color[k] * ONE;
							new_terms[3 + k] = ONE;
						}
						break;
					case _blend_alpha_add:
						for (k = 0; k < 3; k++)
						{
							new_terms[k] = ((color[k] * color[3]) >> 8) * ONE;
							new_terms[3 + k] = ONE;
						}
						break;
					case _blend_multiply:
						for (k = 0; k < 3; k++)
						{
							new_terms[k] = 0;
							new_terms[3 + k] = color[k];
						}
						break;
					default:
						/* (multiply2x) */
						for (k = 0; k < 3; k++)
						{
							new_terms[k] = 0;
							new_terms[3 + k] = color[k] * 2;
						}
						break;
					}
					new_terms[6] = -1;
					if (!new_terms[3] && !new_terms[4] && !new_terms[5])
					{
						long red = new_terms[0] >> 8, green = new_terms[1] >> 8, blue = new_terms[2] >> 8;

						red = red > 255 ? 255 : red < 0 ? 0 : red;
						green = green > 255 ? 255 : green < 0 ? 0 : green;
						blue = blue > 255 ? 255 : blue < 0 ? 0 : blue;
						new_terms[6] = rgb565(red, green, blue);
					}
					if (shared_layer)
						shared_row_stamps[block] = shared_stamp;
					else
						block_stamps[block] = block_stamp;
				}
				terms = shared_layer ? shared_row_terms[block] : block_terms[block];
				current_block = block;
				current_terms = terms;
				}
				if (terms[6] >= 0)
				{
					pixel_row[x] = (unsigned short)terms[6];
					layer_written++;
					continue;
				}
				old = pixel_row[x];
				red = ((old >> 11) & 31) * 8 + ((old >> 11) & 31) / 4;
				green = ((old >> 5) & 63) * 4 + ((old >> 5) & 63) / 16;
				blue = (old & 31) * 8 + (old & 31) / 4;
				red = (terms[0] + red * terms[3]) >> 8;
				green = (terms[1] + green * terms[4]) >> 8;
				blue = (terms[2] + blue * terms[5]) >> 8;
				red = red > 255 ? 255 : red < 0 ? 0 : red;
				green = green > 255 ? 255 : green < 0 ? 0 : green;
				blue = blue > 255 ? 255 : blue < 0 ? 0 : blue;
				pixel_row[x] = rgb565(red, green, blue);
				layer_written++;
			}
			counters.pixels_written += layer_written;
			continue;
		}

		for (x = x_begin; x < x_finish; x++)
		{
			long color[4];
			unsigned long z = (unsigned long)(values[0] < 0 ? 0 : values[0] > DEPTH_CLAMP_MAX ? DEPTH_CLAMP_MAX :
				values[0]) >> 8;

			if (!local_z_test || compare(local_z_function, z, depth_row[x >> depth_shift]))
			{
				int attribute = 1, stage;
				unsigned long written = local_written_registers;
				unsigned long block = (unsigned long)x >> local_block_shift;

				if (local_constant_color)
				{
					color[0] = local_color[0];
					color[1] = local_color[1];
					color[2] = local_color[2];
					color[3] = local_color[3];
					goto shaded;
				}
				if (local_block_shading && block_stamps[block] == block_stamp)
				{
					/* shaded already, for another pixel of this 2x2 block */
					color[0] = block_colors[block][0];
					color[1] = block_colors[block][1];
					color[2] = block_colors[block][2];
					color[3] = block_colors[block][3];
					goto shaded;
				}

				/* registers the last pixel's combiners wrote */
				while (written)
				{
					int reg = __builtin_ctzl(written);

					registers[reg][0] = registers[reg][1] = registers[reg][2] = registers[reg][3] = 0;
					written &= written - 1;
				}
				if (local_constant_diffuse)
				{
					/* (the combiners may have written over it) */
					registers[_register_v0][0] = local_diffuse[0];
					registers[_register_v0][1] = local_diffuse[1];
					registers[_register_v0][2] = local_diffuse[2];
					registers[_register_v0][3] = local_diffuse[3];
				}
				if (local_uses_diffuse)
				{
					for (k = 0; k < 4; k++)
					{
						long channel = values[attribute + k] >> 16;

						registers[_register_v0][k] = channel < 0 ? 0 : channel > ONE ? ONE : channel;
					}
					attribute += 4;
				}
				for (stage = 0; stage < local_stage_count; stage++)
				{
					int s = local_stages[stage];
					long *texel = registers[_register_t0 + s];

					switch (local_stage_kinds[stage])
					{
					case _stage_sample:
#ifdef EXPERIMENT_NO_SAMPLE
						texel[0] = texel[1] = texel[2] = texel[3] = 200;
#else
						sample(&samplers[s], values[attribute], values[attribute + 1], texel);
#endif
						break;
					case _stage_passthrough:
						texel[0] = clamp_unit(values[attribute] >> 8);
						texel[1] = clamp_unit(values[attribute + 1] >> 8);
						texel[2] = 0;
						texel[3] = ONE;
						break;
					default:
						/* cube maps and dot product stages: a neutral grey */
						texel[0] = texel[1] = texel[2] = ONE / 2;
						texel[3] = ONE;
						attribute -= 2;
						break;
					}
					attribute += 2;
				}
				registers[_register_r0][0] = registers[_register_r0][1] = registers[_register_r0][2] = 0;
				registers[_register_r0][3] = registers[_register_t0][3];
				if (CHECK_COMPILED_COMBINERS && log_draws)
				{
					/* the compiled combiners checked against the reference */
					long reference_registers[NUMBER_OF_REGISTERS][4], reference[4];

					memcpy(reference_registers, registers, sizeof(reference_registers));
					combine(reference_registers, reference);
					if (local_shader)
						local_shader(registers, color);
					else
						combine_compiled(registers, color);
					if (reference[0] != color[0] || reference[1] != color[1] || reference[2] != color[2] ||
						reference[3] != color[3])
					{
						counters.mismatches++;
					}
				}
				else
				{
#ifdef EXPERIMENT_NO_COMBINE
					color[0] = registers[_register_t0][0]; color[1] = registers[_register_t0][1];
					color[2] = registers[_register_t0][2]; color[3] = registers[_register_t0][3];
#else
					if (local_shader)
						local_shader(registers, color);
					else
						combine_compiled(registers, color);
#endif
				}
				if (local_block_shading)
				{
					block_colors[block][0] = color[0];
					block_colors[block][1] = color[1];
					block_colors[block][2] = color[2];
					block_colors[block][3] = color[3];
					block_stamps[block] = block_stamp;
				}
shaded:

				if (!local_alpha_test || compare(local_alpha_function,
					(unsigned long)(color[3] >= ONE ? 255 : color[3]), local_alpha_reference))
				{
					if (local_visibility)
						soft_device.visibility_count++;
					long source_alpha = color[3] > ONE ? ONE : color[3] < 0 ? 0 : color[3];
					long destination_alpha = 0;

					if (!target->keeps_alpha)
					{
						destination_alpha = ONE;
					}
					else if (local_reads_destination_alpha || local_alpha_write)
					{
						destination_alpha = alpha_row[x];
						destination_alpha += destination_alpha >> 7;
					}
					/* (blended by its alpha, a pixel of alpha 0 leaves colour and
					alpha as they were: most of a character of text) */
					if ((local_blend_kind == _blend_alpha || local_blend_kind == _blend_alpha_add) && color[3] <= 0)
						goto colour_written;
					if (local_color_write)
					{
						if (local_blend_kind != _blend_copy)
						{
							unsigned short old = pixel_row[x];
							long destination[4];

							destination[3] = destination_alpha;
							destination[0] = ((old >> 11) & 31) * 8 + ((old >> 11) & 31) / 4;
							destination[1] = ((old >> 5) & 63) * 4 + ((old >> 5) & 63) / 16;
							destination[2] = (old & 31) * 8 + (old & 31) / 4;
							switch (local_blend_kind)
							{
							case _blend_alpha:
							{
								long alpha = color[3];

								color[0] = (color[0] * alpha + destination[0] * (ONE - alpha)) >> 8;
								color[1] = (color[1] * alpha + destination[1] * (ONE - alpha)) >> 8;
								color[2] = (color[2] * alpha + destination[2] * (ONE - alpha)) >> 8;
								break;
							}
							case _blend_premultiplied:
							{
								long remaining = ONE - color[3];

								color[0] += (destination[0] * remaining) >> 8;
								color[1] += (destination[1] * remaining) >> 8;
								color[2] += (destination[2] * remaining) >> 8;
								break;
							}
							case _blend_add:
								color[0] += destination[0];
								color[1] += destination[1];
								color[2] += destination[2];
								break;
							case _blend_alpha_add:
								color[0] = ((color[0] * color[3]) >> 8) + destination[0];
								color[1] = ((color[1] * color[3]) >> 8) + destination[1];
								color[2] = ((color[2] * color[3]) >> 8) + destination[2];
								break;
							case _blend_multiply:
								color[0] = (color[0] * destination[0]) >> 8;
								color[1] = (color[1] * destination[1]) >> 8;
								color[2] = (color[2] * destination[2]) >> 8;
								break;
							case _blend_multiply2x:
								color[0] = (color[0] * destination[0] * 2) >> 8;
								color[1] = (color[1] * destination[1] * 2) >> 8;
								color[2] = (color[2] * destination[2] * 2) >> 8;
								break;
							default:
								for (k = 0; k < 3; k++)
								{
									color[k] = (color[k] * blend_factor(local_source_blend, color, destination, k) +
										destination[k] * blend_factor(local_destination_blend, color, destination, k)) >> 8;
								}
								break;
							}
							for (k = 0; k < 3; k++)
								color[k] = color[k] > 255 ? 255 : color[k] < 0 ? 0 : color[k];
						}
						pixel_row[x] = rgb565(color[0] > 255 ? 255 : color[0], color[1] > 255 ? 255 : color[1],
							color[2] > 255 ? 255 : color[2]);
					}
					if (local_alpha_write && target->keeps_alpha)
					{
						long alpha = source_alpha;

						if (local_blend)
						{
							alpha = (source_alpha * blend_factor_alpha(local_source_blend, source_alpha, destination_alpha) +
								destination_alpha * blend_factor_alpha(local_destination_blend, source_alpha,
									destination_alpha)) >> 8;
						}
						alpha_row[x] = (unsigned char)(alpha > 255 ? 255 : alpha < 0 ? 0 : alpha);
					}
colour_written:
					if (local_z_write)
						depth_row[x >> depth_shift] = (unsigned short)z;
					counters.pixels_written++;
				}
			}
			for (k = 0; k < count; k++)
				values[k] += step_x[k];
		}
	}
	FINE_PROFILE_END(_nspire_profile_spans);
}

static void draw_polygon(struct clip_vertex *polygon, int count)
{
	struct screen_vertex projected[MAXIMUM_CLIPPED_VERTICES];
	int index;

	count = clip_polygon(polygon, count);
	if (count < 3)
		return;
	for (index = 0; index < count; index++)
		project(&polygon[index], &projected[index]);
	for (index = 1; index + 1 < count; index++)
		fill_triangle(&projected[0], &projected[index], &projected[index + 1]);
}

/* a vertex inside every clipping plane, projected once for the draw */
static const struct screen_vertex *projected_vertex(unsigned long index, const struct clip_vertex *vertex)
{
	if (projected_serials[index] != projection_serial)
	{
		projected_serials[index] = projection_serial;
		FINE_PROFILE_BEGIN(_nspire_profile_project);
		project(vertex, &projected[index]);
		FINE_PROFILE_END(_nspire_profile_project);
	}
	return &projected[index];
}

/* A screen-aligned rectangle with one texture (text, the interface's
pieces: a character a draw, about 40 thousand instructions through the
triangles' setup for some 50 pixels): filled row by row, its texture
coordinates stepped, each pixel shaded by the draw's own combiners and
blended as fill_triangle would. FALSE: not such a rectangle, drawn as
triangles. */
#ifndef NSPIRE_RECTANGLES
#define NSPIRE_RECTANGLES 1
#endif

/* whether a compiled combiner is the texture times the diffuse colour,
channel by channel ((t * d) >> 8), tried on a spread of values: the
rectangles then shade without it. Kept for the last routine asked about. */
static BOOL combiner_is_modulate(shader_routine shader, int stage)
{
	static shader_routine known_shader;
	static int known_stage = -1;
	static BOOL known_result;
	static const long samples[][2] = { { 0, 0 }, { 256, 256 }, { 256, 0 }, { 0, 256 }, { 128, 200 }, { 17, 255 },
		{ 255, 17 }, { 100, 3 }, { 3, 100 }, { 77, 133 }, { 240, 16 }, { 1, 1 } };
	long registers[NUMBER_OF_REGISTERS][4], color[4];
	unsigned long sample_index;
	int k;

	if (!shader)
		return FALSE;
	if (shader == known_shader && stage == known_stage)
		return known_result;
	known_shader = shader;
	known_stage = stage;
	known_result = FALSE;
	for (sample_index = 0; sample_index < sizeof(samples) / sizeof(samples[0]); sample_index++)
	{
		long t = samples[sample_index][0], d = samples[sample_index][1];

		memset(registers, 0, sizeof(registers));
		registers[_register_fog][3] = ONE;
		for (k = 0; k < 4; k++)
		{
			/* (each channel its own pair, so a mix of channels shows) */
			registers[_register_t0 + stage][k] = (t + 37 * k) % 257;
			registers[_register_v0][k] = (d + 53 * k) % 257;
		}
		registers[_register_r0][3] = registers[_register_t0][3];
		shader(registers, color);
		for (k = 0; k < 4; k++)
		{
			if (color[k] != ((((t + 37 * k) % 257) * ((d + 53 * k) % 257)) >> 8))
				return FALSE;
		}
	}
	known_result = TRUE;
	return TRUE;
}

static BOOL fill_rectangle(unsigned long i0, unsigned long i1, unsigned long i2, unsigned long i3)
{
	unsigned long indices[4] = { i0, i1, i2, i3 };
	struct screen_vertex corners[4];
	long x_low = 0, x_high = 0, y_low = 0, y_high = 0, u_low = 0, u_high = 0, v_low = 0, v_high = 0;
	long registers[NUMBER_OF_REGISTERS][4], diffuse[4];
	int k, corner, uv = state.uses_diffuse ? 5 : 1;
	long x_begin, x_end, y_begin, y_end, x, y, u_step, v_step, u_start, v;
	long long width, height;
	struct sampler *sampler = &samplers[state.stages[0]];
	shader_routine shader = state.shader;
	const unsigned long written_registers = state.written_registers;

	if (!NSPIRE_RECTANGLES || state.z_test || state.z_write || state.alpha_test || !state.color_write ||
		state.reads_destination_alpha || soft_device.visibility_test_active || state.constant_color ||
		(state.blend_kind != _blend_copy && state.blend_kind != _blend_alpha) || state.stage_count != 1 ||
		state.stage_kinds[0] != _stage_sample || !sampler->texels || state.attribute_count != uv + 2)
	{
		return FALSE;
	}
	for (corner = 0; corner < 4; corner++)
	{
		const struct clip_vertex *c = transformed_vertex(indices[corner]);

		if (vertex_serials[indices[corner]] != vertex_serial || vertex_outcodes[indices[corner]])
			return FALSE;
		project(c, &corners[corner]);
	}
	/* (two x and two y among the corners, each corner at one of each; the
	colour one; u along x and v along y alone) */
	x_low = x_high = corners[0].fixed_x;
	y_low = y_high = corners[0].fixed_y;
	for (corner = 1; corner < 4; corner++)
	{
		if (corners[corner].fixed_x < x_low) x_low = corners[corner].fixed_x;
		if (corners[corner].fixed_x > x_high) x_high = corners[corner].fixed_x;
		if (corners[corner].fixed_y < y_low) y_low = corners[corner].fixed_y;
		if (corners[corner].fixed_y > y_high) y_high = corners[corner].fixed_y;
	}
	if (x_high - x_low < 0x10000 / 4 || y_high - y_low < 0x10000 / 4)
		return FALSE;
	{
		int seen = 0;

		for (corner = 0; corner < 4; corner++)
		{
			const struct screen_vertex *c = &corners[corner];
			BOOL left = c->fixed_x == x_low, top = c->fixed_y == y_low;

			if ((!left && c->fixed_x != x_high) || (!top && c->fixed_y != y_high))
				return FALSE;
			seen |= 1 << ((left ? 0 : 1) | (top ? 0 : 2));
			if (state.uses_diffuse)
			{
				for (k = 1; k <= 4; k++)
				{
					if (c->fixed[k] != corners[0].fixed[k])
						return FALSE;
				}
			}
		}
		if (seen != 15)
			return FALSE;
	}
	{
		/* u at the left and right, v at the top and bottom, from the corners */
		BOOL u_set[2] = { FALSE, FALSE }, v_set[2] = { FALSE, FALSE };
		long u_side[2] = { 0, 0 }, v_side[2] = { 0, 0 };

		for (corner = 0; corner < 4; corner++)
		{
			const struct screen_vertex *c = &corners[corner];
			int side_x = c->fixed_x == x_low ? 0 : 1, side_y = c->fixed_y == y_low ? 0 : 1;

			if (u_set[side_x] && u_side[side_x] != c->fixed[uv])
				return FALSE;
			if (v_set[side_y] && v_side[side_y] != c->fixed[uv + 1])
				return FALSE;
			u_side[side_x] = c->fixed[uv];
			v_side[side_y] = c->fixed[uv + 1];
			u_set[side_x] = v_set[side_y] = TRUE;
		}
		u_low = u_side[0];
		u_high = u_side[1];
		v_low = v_side[0];
		v_high = v_side[1];
	}
	/* the pixels whose centres are inside, as fill_triangle's spans take them */
	x_begin = (x_low - 0x8000 + 0xFFFF) >> 16;
	x_end = (x_high - 0x8000 + 0xFFFF) >> 16;
	y_begin = (y_low - 0x8000 + 0xFFFF) >> 16;
	y_end = (y_high - 0x8000 + 0xFFFF) >> 16;
	if (x_begin < 0) x_begin = 0;
	if (y_begin < 0) y_begin = 0;
	if (x_end > target->width) x_end = target->width;
	if (y_end > target->height) y_end = target->height;
	counters.triangles += 2;
	counters.filled += 2;
	primitive_count += 2;
	if (x_begin >= x_end || y_begin >= y_end)
		return TRUE;
	width = (long long)(x_high - x_low);
	height = (long long)(y_high - y_low);
	u_step = (long)(((long long)(u_high - u_low) << 16) / width);
	v_step = (long)(((long long)(v_high - v_low) << 16) / height);
	u_start = u_low + (long)(((long long)(((long long)x_begin << 16) + 0x8000 - x_low) * (u_high - u_low)) / width);
	sampler_select_level(sampler, (unsigned long)(u_step < 0 ? -u_step : u_step) > (unsigned long)(v_step < 0 ? -v_step : v_step) ?
		(unsigned long)(u_step < 0 ? -u_step : u_step) : (unsigned long)(v_step < 0 ? -v_step : v_step));
	memset(registers, 0, sizeof(registers));
	registers[_register_fog][3] = ONE;
	for (k = 0; k < 4; k++)
	{
		long channel = state.uses_diffuse ? corners[0].fixed[1 + k] >> 16 : state.constant_diffuse ? state.diffuse[k] : ONE;

		diffuse[k] = state.uses_diffuse ? (channel < 0 ? 0 : channel > ONE ? ONE : channel) : channel;
	}
	FINE_PROFILE_BEGIN(_nspire_profile_spans);
	/* (texture times colour, blended by alpha, alpha not kept: text) */
	if (state.blend_kind == _blend_alpha && !(state.alpha_write && target->keeps_alpha) &&
		combiner_is_modulate(shader, state.stages[0]))
	{
		for (y = y_begin; y < y_end; y++)
		{
			unsigned short *pixel_row = target->pixels + y * target->width;
			long u = u_start, texel[4];

			v = v_low + (long)(((long long)(((long long)y << 16) + 0x8000 - y_low) * (v_high - v_low)) / height);
			for (x = x_begin; x < x_end; x++, u += u_step)
			{
				long alpha, old, red, green, blue;

				counters.pixels_tested++;
				sample(sampler, u, v, texel);
				alpha = (texel[3] * diffuse[3]) >> 8;
				if (alpha <= 0)
					continue;
				red = (texel[0] * diffuse[0]) >> 8;
				green = (texel[1] * diffuse[1]) >> 8;
				blue = (texel[2] * diffuse[2]) >> 8;
				old = pixel_row[x];
				red = (red * alpha + (((old >> 11) & 31) * 8 + ((old >> 11) & 31) / 4) * (ONE - alpha)) >> 8;
				green = (green * alpha + (((old >> 5) & 63) * 4 + ((old >> 5) & 63) / 16) * (ONE - alpha)) >> 8;
				blue = (blue * alpha + ((old & 31) * 8 + (old & 31) / 4) * (ONE - alpha)) >> 8;
				pixel_row[x] = rgb565(red > 255 ? 255 : red < 0 ? 0 : red, green > 255 ? 255 : green < 0 ? 0 : green,
					blue > 255 ? 255 : blue < 0 ? 0 : blue);
				counters.pixels_written++;
			}
		}
		FINE_PROFILE_END(_nspire_profile_spans);
		return TRUE;
	}
	for (y = y_begin; y < y_end; y++)
	{
		unsigned short *pixel_row = target->pixels + y * target->width;
		unsigned char *alpha_row = target->alpha + y * target->width;
		long u = u_start;

		v = v_low + (long)(((long long)(((long long)y << 16) + 0x8000 - y_low) * (v_high - v_low)) / height);
		for (x = x_begin; x < x_end; x++, u += u_step)
		{
			long color[4], *texel = registers[_register_t0 + state.stages[0]];
			unsigned long written = written_registers;

			counters.pixels_tested++;
			while (written)
			{
				int reg = __builtin_ctzl(written);

				registers[reg][0] = registers[reg][1] = registers[reg][2] = registers[reg][3] = 0;
				written &= written - 1;
			}
			registers[_register_v0][0] = diffuse[0];
			registers[_register_v0][1] = diffuse[1];
			registers[_register_v0][2] = diffuse[2];
			registers[_register_v0][3] = diffuse[3];
			sample(sampler, u, v, texel);
			registers[_register_r0][0] = registers[_register_r0][1] = registers[_register_r0][2] = 0;
			registers[_register_r0][3] = registers[_register_t0][3];
			if (shader)
				shader(registers, color);
			else
				combine_compiled(registers, color);
			if (state.blend_kind == _blend_alpha)
			{
				long alpha = color[3], old = pixel_row[x];

				if (alpha <= 0)
					continue;
				color[0] = (color[0] * alpha + (((old >> 11) & 31) * 8 + ((old >> 11) & 31) / 4) * (ONE - alpha)) >> 8;
				color[1] = (color[1] * alpha + (((old >> 5) & 63) * 4 + ((old >> 5) & 63) / 16) * (ONE - alpha)) >> 8;
				color[2] = (color[2] * alpha + ((old & 31) * 8 + (old & 31) / 4) * (ONE - alpha)) >> 8;
				for (k = 0; k < 3; k++)
					color[k] = color[k] > 255 ? 255 : color[k] < 0 ? 0 : color[k];
			}
			pixel_row[x] = rgb565(color[0] > 255 ? 255 : color[0] < 0 ? 0 : color[0],
				color[1] > 255 ? 255 : color[1] < 0 ? 0 : color[1], color[2] > 255 ? 255 : color[2] < 0 ? 0 : color[2]);
			if (state.alpha_write && target->keeps_alpha)
			{
				long source_alpha = color[3] > ONE ? ONE : color[3] < 0 ? 0 : color[3], alpha = source_alpha;

				if (state.blend)
				{
					long destination_alpha = alpha_row[x];

					destination_alpha += destination_alpha >> 7;
					alpha = (source_alpha * blend_factor_alpha(state.source_blend, source_alpha, destination_alpha) +
						destination_alpha * blend_factor_alpha(state.destination_blend, source_alpha, destination_alpha)) >> 8;
				}
				alpha_row[x] = (unsigned char)(alpha > 255 ? 255 : alpha < 0 ? 0 : alpha);
			}
			counters.pixels_written++;
		}
	}
	FINE_PROFILE_END(_nspire_profile_spans);
	return TRUE;
}

static void draw_triangle(unsigned long i0, unsigned long i1, unsigned long i2)
{
	struct clip_vertex polygon[MAXIMUM_CLIPPED_VERTICES];
	unsigned short outcode0, outcode1, outcode2;

	const struct clip_vertex *c0 = transformed_vertex(i0), *c1 = transformed_vertex(i1), *c2 = transformed_vertex(i2);

	current_ordinal = pass_record_ordinal++;
#ifdef DEBUG_TRIANGLES
	if (log_draws && draw_number == DEBUG_DRAW && current_ordinal < 6)
		nspire_log("    triangle %lu: clip %ld %ld %ld %ld / %ld %ld %ld %ld / %ld %ld %ld %ld (x1000), outcodes %x %x %x",
			current_ordinal, (long)(c0->clip[0] * 1000), (long)(c0->clip[1] * 1000), (long)(c0->clip[2] * 1000),
			(long)(c0->clip[3] * 1000), (long)(c1->clip[0] * 1000), (long)(c1->clip[1] * 1000), (long)(c1->clip[2] * 1000),
			(long)(c1->clip[3] * 1000), (long)(c2->clip[0] * 1000), (long)(c2->clip[1] * 1000), (long)(c2->clip[2] * 1000),
			(long)(c2->clip[3] * 1000), vertex_outcodes[i0], vertex_outcodes[i1], vertex_outcodes[i2]);
#endif
	if (!pass_shows(current_ordinal))
	{
		counters.triangles++;
		counters.rejected++;
		return;
	}
	/* (a two-phase draw leaves out the vertices of triangles it saw cannot
	be seen) */
	if (vertex_serials[i0] != vertex_serial || vertex_serials[i1] != vertex_serial ||
		vertex_serials[i2] != vertex_serial)
	{
		counters.triangles++;
		counters.rejected++;
		return;
	}
	outcode0 = vertex_outcodes[i0];
	outcode1 = vertex_outcodes[i1];
	outcode2 = vertex_outcodes[i2];
	counters.triangles++;
	/* wholly beyond one edge of the screen or clipping plane: nothing */
	if (outcode0 & outcode1 & outcode2)
	{
		counters.rejected++;
		return;
	}
	if (!((outcode0 | outcode1 | outcode2) & OUTCODE_CLIP_MASK))
	{
		fill_triangle(projected_vertex(i0, c0), projected_vertex(i1, c1), projected_vertex(i2, c2));
	}
	else
	{
		counters.clipped++;
		polygon[0] = *c0;
		polygon[1] = *c1;
		polygon[2] = *c2;
		clip_vertex_float_textures(&polygon[0]);
		clip_vertex_float_textures(&polygon[1]);
		clip_vertex_float_textures(&polygon[2]);
		polygon[0].clip_is_fixed = polygon[1].clip_is_fixed = polygon[2].clip_is_fixed = 0;
		draw_polygon(polygon, 3);
	}
}

/* what the samplers are made of: the textures, their descriptions and
addressing, and the texture modes; unchanged since the last draw (with its
combiners), the samplers and the combiners' code stand */
#define SAMPLER_STATE_WORDS (1 + 4 * 8)
static DWORD sampler_state_last[SAMPLER_STATE_WORDS];
static BOOL sampler_state_valid;

static BOOL sampler_state_unchanged(void)
{
	DWORD now[SAMPLER_STATE_WORDS];
	int stage, index = 0, k;

	now[index++] = D3D__RenderState[D3DRS_PSTEXTUREMODES];
	for (stage = 0; stage < 4; stage++)
	{
		const DWORD *resource = (const DWORD *)soft_device.textures[stage];

		now[index++] = (DWORD)(unsigned long)resource;
		for (k = 0; k < 5; k++)
			now[index++] = resource ? resource[k] : 0;
		now[index++] = D3D__TextureState[stage][D3DTSS_ADDRESSU] | (D3D__TextureState[stage][D3DTSS_ADDRESSV] << 8);
		now[index++] = D3D__TextureState[stage][D3DTSS_BORDERCOLOR];
	}
	if (sampler_state_valid && !memcmp(now, sampler_state_last, sizeof(now)))
		return TRUE;
	memcpy(sampler_state_last, now, sizeof(now));
	sampler_state_valid = TRUE;
	return FALSE;
}

static BOOL state_prepare(void)
{
	float z_scale = soft_device.depth_scale > 0.0f ? soft_device.depth_scale : 65535.0f;
	int stage;
	BOOL reuse_samplers;

	state.pixels = nspire_video_pixels();
	if (!state.pixels)
		return FALSE;
	screen_target.pixels = state.pixels;
	state.z_test = D3D__RenderState[D3DRS_ZENABLE] != 0;
	state.z_write = D3D__RenderState[D3DRS_ZWRITEENABLE] != 0;
	state.z_function = D3D__RenderState[D3DRS_ZFUNC];
	state.color_write = (D3D__RenderState[D3DRS_COLORWRITEENABLE] & 0x00FFFFFF) != 0;
	state.alpha_write = (D3D__RenderState[D3DRS_COLORWRITEENABLE] & D3DCOLORWRITEENABLE_ALPHA) != 0;
	state.blend = D3D__RenderState[D3DRS_ALPHABLENDENABLE] != 0;
	state.source_blend = D3D__RenderState[D3DRS_SRCBLEND];
	state.destination_blend = D3D__RenderState[D3DRS_DESTBLEND];
	state.alpha_test = D3D__RenderState[D3DRS_ALPHATESTENABLE] != 0;
	state.alpha_function = D3D__RenderState[D3DRS_ALPHAFUNC];
	state.alpha_reference = D3D__RenderState[D3DRS_ALPHAREF] & 0xFF;
	state.cull_mode = D3D__RenderState[D3DRS_CULLMODE];
#ifdef EXPERIMENT_NO_CULL
	state.cull_mode = D3DCULL_NONE;
#endif
	draw_cull_mode = state.cull_mode;
	/* the 3D world at 160x120 until render_window (source/render/render.c)
	has drawn it and asks for it on the screen; a small render target at its
	size, without depth */
	if (drawing_to_texture())
	{
		texture_target_begin();
		texture_target_dirty = TRUE;
		target = &texture_target;
		state.z_test = FALSE;
		state.z_write = FALSE;
	}
	else if (!resolved)
	{
		target = &low_target;
	}
	else
	{
		soft_rasterizer_resolve();
		target = &screen_target;
	}
	if (state.z_write)
		depth_written = TRUE;
	state.z_normalize = 65535.0f / z_scale;
	draw_z_normalize = state.z_normalize;
	draw_occlusion_test = target == &low_target && state.z_test &&
		(state.z_function == D3DCMP_LESS || state.z_function == D3DCMP_LESSEQUAL);
	{
		/* (both, so neither's snapshot goes stale) */
		BOOL combiners_unchanged = combiners_prepare();
		BOOL samplers_unchanged = sampler_state_unchanged();

		reuse_samplers = combiners_unchanged && samplers_unchanged && state.shader_valid;
	}
	state.attribute_count = 0;
	state.stage_count = 0;
	state.constant_diffuse = FALSE;
	state.coordinate_stages = 0;
	state.attributes[state.attribute_count++] = 0;
	state.uses_diffuse = combiners.uses_register[_register_v0];
	if (state.uses_diffuse)
	{
		int channel;

		for (channel = 1; channel <= 4; channel++)
			state.attributes[state.attribute_count++] = channel;
	}
	for (stage = 0; stage < 4; stage++)
	{
		unsigned long mode = (D3D__RenderState[D3DRS_PSTEXTUREMODES] >> (5 * stage)) & 0x1F;

		if (!reuse_samplers)
			sampler_prepare(stage, combiners.uses_texture[stage] && mode != _texture_mode_none);
		else
			samplers[stage].selected_level = -1;
		if (samplers[stage].active)
		{
			unsigned long sampler_mode = samplers[stage].mode;
			int kind = sampler_mode == _texture_mode_passthrough ? _stage_passthrough :
				sampler_mode == _texture_mode_project2d || sampler_mode == _texture_mode_project3d ||
				sampler_mode == _texture_mode_bump_environment ||
				sampler_mode == _texture_mode_bump_environment_luminance ? _stage_sample : _stage_grey;

			/* (a grey stage, a cube map's, needs no coordinates: the vertex
			program is spared computing them) */
			if (kind != _stage_grey)
			{
				state.attributes[state.attribute_count++] = 5 + stage * 2;
				state.attributes[state.attribute_count++] = 6 + stage * 2;
				state.coordinate_stages |= 1UL << stage;
			}
			state.stages[state.stage_count] = stage;
			state.stage_kinds[state.stage_count++] = kind;
		}
	}
	{
		DWORD source = state.source_blend, destination = state.destination_blend;

		if (!state.blend || (source == D3DBLEND_ONE && destination == D3DBLEND_ZERO))
			state.blend_kind = _blend_copy;
		else if (source == D3DBLEND_SRCALPHA && destination == D3DBLEND_INVSRCALPHA)
			state.blend_kind = _blend_alpha;
		else if (source == D3DBLEND_ONE && destination == D3DBLEND_ONE)
			state.blend_kind = _blend_add;
		else if (source == D3DBLEND_SRCALPHA && destination == D3DBLEND_ONE)
			state.blend_kind = _blend_alpha_add;
		else if (source == D3DBLEND_ONE && destination == D3DBLEND_INVSRCALPHA)
			state.blend_kind = _blend_premultiplied;
		else if ((source == D3DBLEND_DESTCOLOR && destination == D3DBLEND_ZERO) ||
			(source == D3DBLEND_ZERO && destination == D3DBLEND_SRCCOLOR))
			state.blend_kind = _blend_multiply;
		else if (source == D3DBLEND_DESTCOLOR && destination == D3DBLEND_SRCCOLOR)
			state.blend_kind = _blend_multiply2x;
		else
			state.blend_kind = _blend_generic;
		state.reads_destination_alpha = state.blend &&
			(source == D3DBLEND_DESTALPHA || source == D3DBLEND_INVDESTALPHA || source == D3DBLEND_SRCALPHASAT ||
			destination == D3DBLEND_DESTALPHA || destination == D3DBLEND_INVDESTALPHA ||
			destination == D3DBLEND_SRCALPHASAT);
	}
	state.written_registers = compiled.reset_registers;
	if (reuse_samplers)
		return TRUE;
	state.shader = NSPIRE_COMBINER_CODE ? jit_routine() : NULL;
	state.shader_valid = TRUE;
	state.constant_color = !combiners.uses_texture[0] && !combiners.uses_texture[1] && !combiners.uses_texture[2] &&
		!combiners.uses_texture[3] && !combiners.uses_register[_register_v0];
	if (state.constant_color)
	{
		long registers[NUMBER_OF_REGISTERS][4];

		memset(registers, 0, sizeof(registers));
		registers[_register_fog][3] = ONE;
		registers[_register_v0][0] = registers[_register_v0][1] = registers[_register_v0][2] =
			registers[_register_v0][3] = ONE;
		combine_compiled(registers, state.color);
	}
	return TRUE;
}

static void soft_draw_body(D3DPRIMITIVETYPE type, unsigned long vertex_count, const WORD *indices,
	unsigned long start_vertex, const float *immediate_vertices);

void soft_draw(D3DPRIMITIVETYPE type, unsigned long vertex_count, const WORD *indices, unsigned long start_vertex,
	const float *immediate_vertices)
{
	/* (var is looked at here too: a frame takes seconds, and the controls
	are read once a frame) */
	nspire_capture_poll();
	soft_capture_draw(type, vertex_count, indices, start_vertex, immediate_vertices, drawing_to_screen());
	NSPIRE_PROFILE_BEGIN(_nspire_profile_draws);
	soft_draw_body(type, vertex_count, indices, start_vertex, immediate_vertices);
	NSPIRE_PROFILE_END(_nspire_profile_draws);
}

/* soft_draw's work (timed as a whole there) */
/* ---------- skinned parts culled whole

A model part's vertices each lie between their two nodes' transforms of
them, so inside the hull of each node's transform of the box around its
vertices. The part's own position program run on those boxes' corners
(proxies: v0 a corner, v5 that node twice) bounds every vertex it can make:
all corners beyond one clipping plane, or behind the depth buffer, and
none of the part can be seen. The boxes are found once for each part's
vertices (by their address) and kept. */

/* the same for draws whose positions come of v0 alone (the sky's) */
#ifndef NSPIRE_RIGID_CULLING
#define NSPIRE_RIGID_CULLING 0
#endif

#ifndef PART_BOUNDS_ENTRIES
#define PART_BOUNDS_ENTRIES 128
#endif
#ifndef PART_BOUNDS_NODES
#define PART_BOUNDS_NODES 32
#endif
/* tested only when the proxies are at most this fraction of the vertices
(their program costs as much as that many of them) */
#ifndef PART_PROXY_SHARE
#define PART_PROXY_SHARE 1
#endif

struct part_bounds
{
	DWORD data;
	unsigned long first, count;
	/* 0: none found yet; -1: not usable (too many nodes, another layout) */
	int node_count;
	float node[PART_BOUNDS_NODES];
	float minimum[PART_BOUNDS_NODES][3], maximum[PART_BOUNDS_NODES][3];
	/* the last frame the part filled a triangle in (part_frame): it is then
	not tested again for a while, being likely seen still */
	unsigned long seen_frame;
};

/* the part being drawn */
static struct part_bounds *part_drawn;

/* What a part of one object's model did, kept per object (the engine says
which it draws, soft_rasterizer_set_object): the bounds are the mesh's, which
every marine shares. A part that wrote no pixel when last drawn (behind the
level, or a hill's crest, which its box crosses) and that its box cannot
show hidden is skipped for as many frames as it has been drawn in a row
showing nothing, up to PART_SKIP_MOST: every other frame after one such, every
fourth after three. Far scenery on a ridge comes out that many frames late
at most. */
#ifndef PART_SKIP_MOST
#define PART_SKIP_MOST 3
#endif
#ifndef NSPIRE_PART_SKIPPING
#define NSPIRE_PART_SKIPPING 1
#endif
#define PART_INSTANCES 512
struct part_instance
{
	long object_index;
	struct part_bounds *bounds;
	unsigned long seen_frame, drawn_frame, skip_frame;
	/* the frames in a row before drawn_frame it was drawn in and showed
	nothing */
	unsigned long empty_streak;
};
static struct part_instance part_instances[PART_INSTANCES];
static struct part_instance *part_instance_drawn;
static long draw_object_index = -1;

void soft_rasterizer_set_object(long object_index)
{
	draw_object_index = object_index;
}

static struct part_instance *part_instance_find(struct part_bounds *bounds)
{
	unsigned long hash, probe;

	/* (an object index's top half is its salt, often over 0x8000: only -1,
	NONE, is no object) */
	if (draw_object_index == -1 || !bounds)
		return NULL;
	hash = ((unsigned long)draw_object_index * 2654435761UL) ^ ((unsigned long)bounds >> 4);
	for (probe = 0; probe < 8; probe++)
	{
		struct part_instance *instance = &part_instances[(hash + probe) % PART_INSTANCES];

		if (instance->object_index == draw_object_index && instance->bounds == bounds)
			return instance;
		/* (a slot free, or not drawn for a while: taken) */
		if (!instance->bounds || instance->drawn_frame + PART_SKIP_MOST + 2 < part_frame)
		{
			instance->object_index = draw_object_index;
			instance->bounds = bounds;
			instance->seen_frame = instance->drawn_frame = instance->skip_frame = 0;
			instance->empty_streak = 0;
			return instance;
		}
	}
	return NULL;
}

static struct part_bounds part_bounds[PART_BOUNDS_ENTRIES];
static unsigned long part_bounds_next;

static const struct soft_vertex_element *element_of(const struct soft_vertex_shader *shader, unsigned reg)
{
	unsigned long element;

	for (element = 0; element < shader->element_count; element++)
	{
		if (shader->elements[element].reg == reg)
			return &shader->elements[element];
	}
	return NULL;
}

static struct part_bounds *part_bounds_find(const struct soft_vertex_shader *shader, unsigned long first,
	unsigned long count, BOOL skinned)
{
	const struct soft_vertex_element *position = element_of(shader, 0), *nodes = element_of(shader, 5);
	struct part_bounds *bounds;
	DWORD data;
	unsigned long entry, index;

	if (!position || position->bytes != 12 || !soft_device.streams[position->stream].data ||
		(skinned && (!nodes || !element_of(shader, 6) || position->stream != nodes->stream)))
	{
		return NULL;
	}
	data = soft_device.streams[position->stream].data;
	for (entry = 0; entry < PART_BOUNDS_ENTRIES; entry++)
	{
		bounds = &part_bounds[entry];
		if (bounds->data == data && bounds->first == first && bounds->count == count)
			return bounds->node_count > 0 ? bounds : NULL;
	}
	bounds = &part_bounds[part_bounds_next++ % PART_BOUNDS_ENTRIES];
	bounds->data = data;
	bounds->first = first;
	bounds->count = count;
	bounds->node_count = 0;
	bounds->seen_frame = 0;
	for (index = 0; index < count && bounds->node_count >= 0; index++)
	{
		float inputs[XGPU_VERTEX_ATTRIBUTE_COUNT][4];
		int side;

		if (!skinned)
		{
			/* (positions alone: one box) */
			int k;

			vertex_fetch(index, inputs, 1UL << 0);
			if (!bounds->node_count)
			{
				bounds->node_count = 1;
				bounds->node[0] = 0.0f;
				for (k = 0; k < 3; k++)
					bounds->minimum[0][k] = bounds->maximum[0][k] = inputs[0][k];
			}
			for (k = 0; k < 3; k++)
			{
				if (inputs[0][k] < bounds->minimum[0][k]) bounds->minimum[0][k] = inputs[0][k];
				if (inputs[0][k] > bounds->maximum[0][k]) bounds->maximum[0][k] = inputs[0][k];
			}
			continue;
		}
		vertex_fetch(index, inputs, (1UL << 0) | (1UL << 5) | (1UL << 6));
		for (side = 0; side < 2 && bounds->node_count >= 0; side++)
		{
			int node, k;

			/* (a node with none of the vertex's weight: often an unused index) */
			if (side == 0 ? inputs[6][0] <= 0.0f : inputs[6][0] >= 1.0f)
				continue;

			for (node = 0; node < bounds->node_count; node++)
			{
				if (bounds->node[node] == inputs[5][side])
					break;
			}
			if (node == bounds->node_count)
			{
				if (node == PART_BOUNDS_NODES)
				{
					bounds->node_count = -1;
					break;
				}
				bounds->node[node] = inputs[5][side];
				bounds->node_count++;
				for (k = 0; k < 3; k++)
					bounds->minimum[node][k] = bounds->maximum[node][k] = inputs[0][k];
			}
			for (k = 0; k < 3; k++)
			{
				if (inputs[0][k] < bounds->minimum[node][k]) bounds->minimum[node][k] = inputs[0][k];
				if (inputs[0][k] > bounds->maximum[node][k]) bounds->maximum[node][k] = inputs[0][k];
			}
		}
	}
	return bounds->node_count > 0 ? bounds : NULL;
}

/* whether a skinned part (a two-phase draw) cannot be seen at all */
static BOOL part_hidden_test(const void *position_program, struct soft_vertex_shader *shader, unsigned long first,
	unsigned long count, BOOL skinned)
{
	static float inputs[PART_BOUNDS_NODES * 8][XGPU_VERTEX_ATTRIBUTE_COUNT][4];
	static struct soft_vertex_output outputs[PART_BOUNDS_NODES * 8];
	struct part_bounds *bounds;
	float min_x = 1.0e9f, max_x = -1.0e9f, min_y = 1.0e9f, max_y = -1.0e9f, min_z = 1.0e9f;
	unsigned short all_outside = 0xFFFF;
	unsigned long proxy, proxies;
	BOOL near = FALSE;
	long x0, x1, y0, y1;
	long fixed_min_x = 0x7FFFFFFFL, fixed_max_x = -0x7FFFFFFFL, fixed_min_y = 0x7FFFFFFFL,
		fixed_max_y = -0x7FFFFFFFL, fixed_min_z = 0x7FFFFFFFL;
	int node, corner;

	draw_source.first = first;
	draw_source.shader = shader;
	draw_source.immediate_vertices = NULL;
	FINE_PROFILE_BEGIN(_nspire_profile_part_find);
	bounds = part_bounds_find(shader, first, count, skinned);
	FINE_PROFILE_END(_nspire_profile_part_find);
	part_drawn = bounds;
#ifdef DEBUG_PART_REASONS
	if (count == 666 || count == 405)
		nspire_log("    part %lu: bounds %lx nodes %ld seen %lu frame %lu", count, (unsigned long)bounds,
			bounds ? (long)bounds->node_count : -2L, bounds ? bounds->seen_frame : 0UL, part_frame);
#endif
	if (!bounds || (unsigned long)bounds->node_count * 8 * PART_PROXY_SHARE > count)
		return FALSE;
	part_instance_drawn = NSPIRE_PART_SKIPPING ? part_instance_find(bounds) : NULL;
	if (part_instance_drawn)
	{
		/* (skipped this frame already: its later passes too) */
		if (part_instance_drawn->skip_frame == part_frame)
			return TRUE;
		if (part_instance_drawn->seen_frame + 1 >= part_frame &&
			((part_frame + (unsigned long)(part_instance_drawn - part_instances)) & 7))
		{
			return FALSE;
		}
	}
	/* (seen in the last frame: drawn untested, but for every eighth frame) */
	else if (bounds->seen_frame + 1 >= part_frame && ((part_frame + (unsigned long)(bounds - part_bounds)) & 7))
		return FALSE;
	FINE_PROFILE_BEGIN(_nspire_profile_part_run);
	proxies = 0;
	for (node = 0; node < bounds->node_count; node++)
	{
		for (corner = 0; corner < 8; corner++, proxies++)
		{
			memcpy(inputs[proxies], soft_device.attributes, sizeof(inputs[proxies]));
			inputs[proxies][0][0] = corner & 1 ? bounds->maximum[node][0] : bounds->minimum[node][0];
			inputs[proxies][0][1] = corner & 2 ? bounds->maximum[node][1] : bounds->minimum[node][1];
			inputs[proxies][0][2] = corner & 4 ? bounds->maximum[node][2] : bounds->minimum[node][2];
			inputs[proxies][0][3] = 1.0f;
			inputs[proxies][5][0] = inputs[proxies][5][1] = bounds->node[node];
			inputs[proxies][5][2] = 0.0f;
			inputs[proxies][5][3] = 1.0f;
			/* (the second weight is v6.w - v6.x) */
			inputs[proxies][6][0] = 1.0f;
			inputs[proxies][6][1] = inputs[proxies][6][2] = 0.0f;
			inputs[proxies][6][3] = 1.0f;
		}
	}
	BOOL done_fixed = FALSE;

	if (soft_vertex_program_prepare_fixed(position_program, (const float (*)[4])soft_device.constants,
		soft_device.constants_serial))
	{
		/* (in fixed point through the program's code, as vertices are) */
		static long fixed_inputs[PART_BOUNDS_NODES * 8][XGPU_VERTEX_ATTRIBUTE_COUNT][4];
		static int overflow[PART_BOUNDS_NODES * 8];
		unsigned long reads = soft_vertex_program_inputs(position_program), reg, k;

		/* (the registers the program reads; but for v0, v5 and v6, the
		draw's defaults, alike for every corner: converted once) */
		for (reg = 0; reg < XGPU_VERTEX_ATTRIBUTE_COUNT; reg++)
		{
			if (!(reads & (1UL << reg)))
				continue;
			for (k = 0; k < 4; k++)
				fixed_inputs[0][reg][k] = fixed16(inputs[0][reg][k]);
		}
		for (proxy = 0; proxy < proxies; proxy++)
		{
			if (proxy)
				memcpy(fixed_inputs[proxy], fixed_inputs[0], sizeof(fixed_inputs[0]));
			for (k = 0; k < 3; k++)
				fixed_inputs[proxy][0][k] = fixed16(inputs[proxy][0][k]);
			fixed_inputs[proxy][5][0] = fixed_inputs[proxy][5][1] = fixed16(inputs[proxy][5][0]);
		}
		soft_vertex_program_run_fixed(position_program, proxies,
			(const long (*)[XGPU_VERTEX_ATTRIBUTE_COUNT][4])fixed_inputs, outputs, overflow);
		done_fixed = TRUE;
		for (proxy = 0; proxy < proxies; proxy++)
		{
			/* (a corner too large for 16.16: all of them in floating point) */
			if (overflow[proxy])
				done_fixed = FALSE;
		}
	}
	if (!done_fixed)
	{
		for (proxy = 0; proxy < proxies; proxy += SOFT_VERTEX_BATCH)
		{
			unsigned long batch = proxies - proxy < SOFT_VERTEX_BATCH ? proxies - proxy : SOFT_VERTEX_BATCH;

			soft_vertex_program_run_batch(position_program, batch,
				(const float (*)[XGPU_VERTEX_ATTRIBUTE_COUNT][4])(inputs + proxy),
				(const float (*)[4])soft_device.constants, soft_device.constants_serial, outputs + proxy);
		}
	}
	FINE_PROFILE_END(_nspire_profile_part_run);
	FINE_PROFILE_BEGIN(_nspire_profile_part_project);
	for (proxy = 0; proxy < proxies; proxy++)
	{
		float c[4], inverse_w, sx, sy, sz;
		unsigned short outcode;
		int axis;

		/* (in integers, as vertices are, when the run left a fixed clip
		position: the projection's scales are the low target's) */
		if (NSPIRE_FIXED_PROJECTION && done_fixed && outputs[proxy].clip_fixed_valid &&
			outputs[proxy].clip_fixed[3] > 0 && target == &low_target)
		{
			struct screen_vertex corner_screen;
			long z;

			outcode = vertex_outcode_fixed(outputs[proxy].clip_fixed);
			all_outside &= outcode;
			if (outcode & 3)
			{
				near = TRUE;
				continue;
			}
			project_fixed_position(outputs[proxy].clip_fixed, &corner_screen, &z);
			if (corner_screen.fixed_x < fixed_min_x) fixed_min_x = corner_screen.fixed_x;
			if (corner_screen.fixed_x > fixed_max_x) fixed_max_x = corner_screen.fixed_x;
			if (corner_screen.fixed_y < fixed_min_y) fixed_min_y = corner_screen.fixed_y;
			if (corner_screen.fixed_y > fixed_max_y) fixed_max_y = corner_screen.fixed_y;
			if (z < fixed_min_z) fixed_min_z = z;
			continue;
		}
		if (outputs[proxy].clip_captured)
			memcpy(c, outputs[proxy].clip, sizeof(c));
		else
		{
			float w = outputs[proxy].position[3];

			for (axis = 0; axis < 3; axis++)
				c[axis] = (outputs[proxy].position[axis] - screen_offset[axis]) * w / screen_scale[axis];
			c[3] = w;
		}
		outcode = vertex_outcode(c);
		all_outside &= outcode;
		if (outcode & 3)
		{
			near = TRUE;
			continue;
		}
		inverse_w = 1.0f / c[3];
		sx = (c[0] * screen_scale[0] * inverse_w + screen_offset[0] + 0.5f) * 0.25f;
		sy = (c[1] * screen_scale[1] * inverse_w + screen_offset[1] + 0.5f) * 0.25f;
		sz = (c[2] * screen_scale[2] * inverse_w + screen_offset[2]) * draw_z_normalize;
		if (sx < min_x) min_x = sx;
		if (sx > max_x) max_x = sx;
		if (sy < min_y) min_y = sy;
		if (sy > max_y) max_y = sy;
		if (sz < min_z) min_z = sz;
	}
	FINE_PROFILE_END(_nspire_profile_part_project);
	/* (the corners projected in integers: z there is depth times 256) */
	if (fixed_min_x <= fixed_max_x)
	{
		if ((float)(fixed_min_x >> 16) < min_x) min_x = (float)(fixed_min_x >> 16);
		if ((float)(fixed_max_x >> 16) + 1.0f > max_x) max_x = (float)(fixed_max_x >> 16) + 1.0f;
		if ((float)(fixed_min_y >> 16) < min_y) min_y = (float)(fixed_min_y >> 16);
		if ((float)(fixed_max_y >> 16) + 1.0f > max_y) max_y = (float)(fixed_max_y >> 16) + 1.0f;
		if ((float)fixed_min_z * (1.0f / 256.0f) < min_z) min_z = (float)fixed_min_z * (1.0f / 256.0f);
	}
	/* (all beyond one plane or one edge of the screen) */
	if (all_outside)
		return TRUE;
	if (near || !draw_occlusion_test)
		return FALSE;
	x0 = (long)min_x - 1;
	x1 = (long)max_x + 2;
	y0 = (long)min_y - 1;
	y1 = (long)max_y + 2;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > LOW_WIDTH) x1 = LOW_WIDTH;
	if (y1 > LOW_HEIGHT) y1 = LOW_HEIGHT;
	if (x0 >= x1 || y0 >= y1)
		return TRUE;
#ifdef DEBUG_PART_REASONS
	if (count == 666 || count == 405)
	{
		long x, y, farther = 0;

		for (y = y0; y < y1; y++)
			for (x = x0; x < x1; x++)
				if (depth_buffer[y * LOW_WIDTH + x] >= (unsigned long)min_z)
					farther++;
		nspire_log("    part %lu box %ld..%ld x %ld..%ld, nearest %ld, %ld of %ld pixels farther", count, x0, x1, y0, y1,
			(long)min_z, farther, (x1 - x0) * (y1 - y0));
	}
#endif
	return depth_rectangle_hidden(x0, x1, y0, y1, min_z);
}

/* (part_hidden_test, then the skip of a part that showed nothing when last
drawn: NSPIRE_PART_SKIPPING) */
static BOOL part_hidden(const void *position_program, struct soft_vertex_shader *shader, unsigned long first,
	unsigned long count, BOOL skinned)
{
	struct part_instance *instance;

	part_instance_drawn = NULL;
	if (part_hidden_test(position_program, shader, first, count, skinned))
		return TRUE;
	instance = part_instance_drawn;
	if (instance && instance->drawn_frame && instance->seen_frame < instance->drawn_frame)
	{
		unsigned long empties = instance->empty_streak + 1;

		if (empties > PART_SKIP_MOST)
			empties = PART_SKIP_MOST;
		if (part_frame - instance->drawn_frame <= empties)
		{
			instance->skip_frame = part_frame;
			counters.parts_skipped++;
			return TRUE;
		}
	}
	return FALSE;
}

static void soft_draw_body(D3DPRIMITIVETYPE type, unsigned long vertex_count, const WORD *indices, unsigned long start_vertex,
	const float *immediate_vertices)
{
	struct soft_vertex_shader *program = soft_current_program();
	struct soft_vertex_shader *shader = soft_device.vertex_shader;
	unsigned long first, count, index, minimum = 0, maximum = 0;

	if (!(drawing_to_screen() || drawing_to_texture()) || !program || !shader || !program->instruction_count)
		return;
	unsigned long wanted, stage;
	void *decoded;
	struct
	{
		unsigned long start, vertices, triangles, filled, tested, written, mismatches;
#ifdef DEBUG_TRIANGLES
		unsigned long rejected, clipped, culled, hidden;
#endif
	} before;

	FINE_PROFILE_BEGIN(_nspire_profile_draw_setup);
	/* (the last draw's scaled positions, if it had them, undone) */
	soft_vertex_fixed_unscale_positions();
	draw_position_scaling = 0;
	FINE_PROFILE_BEGIN(_nspire_profile_setup_state);
	if (!state_prepare())
	{
		FINE_PROFILE_END(_nspire_profile_setup_state);
		FINE_PROFILE_END(_nspire_profile_draw_setup);
		return;
	}
	FINE_PROFILE_END(_nspire_profile_setup_state);
	position_scaling_stages = state.coordinate_stages;
	/* the program computes only what the rasterizer reads */
	wanted = state.uses_diffuse ? 1 : 0;
	wanted |= state.coordinate_stages << 1;
	(void)stage;
	if (!program->decoded[wanted])
		program->decoded[wanted] = soft_vertex_program_decode(program->instructions, program->instruction_count, wanted);
	decoded = program->decoded[wanted];
	if (!decoded)
	{
		FINE_PROFILE_END(_nspire_profile_draw_setup);
		return;
	}
	screen_constants();
	projection_prepare();
	counters.draws++;
	/* (a multiplying layer's shared blocks are the draw's own) */
	shared_stamp++;
	before.start = (unsigned long)nspire_ticks();
	before.vertices = counters.vertices;
	before.triangles = counters.triangles;
	before.filled = counters.filled;
	before.tested = counters.pixels_tested;
	before.written = counters.pixels_written;
	before.mismatches = counters.mismatches;
#ifdef DEBUG_TRIANGLES
	before.rejected = counters.rejected;
	before.clipped = counters.clipped;
	before.culled = counters.culled;
	before.hidden = counters.hidden_triangles;
#endif

	/* the vertices the draw uses, transformed once each */
	if (indices)
	{
		minimum = maximum = indices[0];
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
	FINE_PROFILE_BEGIN(_nspire_profile_setup_cache);
	{
		struct vertex_cache_entry *cached = indices ? vertex_cache_find(program, shader, wanted, first, count) : NULL;
		FINE_PROFILE_END(_nspire_profile_setup_cache);
		if (!transform_prepare(count, cached))
		{
			FINE_PROFILE_END(_nspire_profile_draw_setup);
			return;
		}
	}
	if (0)
	{
		FINE_PROFILE_END(_nspire_profile_draw_setup);
		return;
	}
	draw_source.shader = shader;
	draw_source.program = decoded;
	draw_source.lit_program = NULL;
#if NSPIRE_FLAT_MODEL_LIGHTING
	/* lighting that costs much of the program (models'): worked out for one
	vertex, the draw's colour */
	if (wanted & 1)
	{
		if (!program->decoded[wanted & ~1UL])
			program->decoded[wanted & ~1UL] = soft_vertex_program_decode(program->instructions, program->instruction_count,
				wanted & ~1UL);
		if (program->decoded[wanted & ~1UL] &&
			soft_vertex_program_length(decoded) >= soft_vertex_program_length(program->decoded[wanted & ~1UL]) + 8)
		{
			draw_source.lit_program = decoded;
			draw_source.program = program->decoded[wanted & ~1UL];
		}
	}
#endif
	/* two phases when finding positions alone costs well under the whole
	program (models: skinning, then lighting and texture coordinates) */
	draw_source.position_program = NULL;
	if (type != D3DPT_POINTLIST && type != D3DPT_LINELIST && type != D3DPT_LINESTRIP && type != D3DPT_LINELOOP)
	{
		if (!program->decoded[0])
			program->decoded[0] = soft_vertex_program_decode(program->instructions, program->instruction_count, 0);
		if (NSPIRE_TWO_PHASE && program->decoded[0] &&
			soft_vertex_program_length(program->decoded[0]) * 10 <= soft_vertex_program_length(draw_source.program) * 6)
		{
			draw_source.position_program = program->decoded[0];
		}
	}
	part_drawn = NULL;
	part_instance_drawn = NULL;
	FINE_PROFILE_BEGIN(_nspire_profile_setup_part);
	/* (skinned parts, whose programs read node indices, v5; and draws whose
	positions come of v0 alone, by one box) */
	if (NSPIRE_PART_CULLING && program->decoded[0] && indices && !immediate_vertices &&
		((soft_vertex_program_inputs(program->decoded[0]) & (1UL << 5)) ||
			(NSPIRE_RIGID_CULLING && soft_vertex_program_inputs(program->decoded[0]) == 1UL)) &&
		part_hidden(program->decoded[0], shader, first, count,
			(soft_vertex_program_inputs(program->decoded[0]) & (1UL << 5)) != 0))
	{
		counters.hidden_draws++;
		counters.parts_culled++;
		FINE_PROFILE_END(_nspire_profile_setup_part);
		FINE_PROFILE_END(_nspire_profile_draw_setup);
		return;
	}
	FINE_PROFILE_END(_nspire_profile_setup_part);
	draw_source.first = first;
	draw_source.immediate_vertices = immediate_vertices;

	if (type == D3DPT_TRIANGLELIST || type == D3DPT_TRIANGLESTRIP || type == D3DPT_TRIANGLEFAN ||
		type == D3DPT_POLYGON || type == D3DPT_QUADLIST || type == D3DPT_QUADSTRIP)
	{
#ifdef DEBUG_TRIANGLES
		if (log_draws && draw_number == DEBUG_DRAW)
		{
			unsigned long e, v;

			for (e = 0; e < shader->element_count; e++)
				nspire_log("    element v%u stream %u type %02x offset %u: data %08lx stride %lu", shader->elements[e].reg,
					shader->elements[e].stream, shader->elements[e].type, shader->elements[e].offset,
					(unsigned long)soft_device.streams[shader->elements[e].stream].data,
					soft_device.streams[shader->elements[e].stream].stride);
			nspire_log("    first %lu count %lu, indices %u %u %u %u", first, count, indices ? indices[0] : 0,
				indices ? indices[1] : 0, indices ? indices[2] : 0, indices ? indices[3] : 0);
			for (v = 0; v < 4 && v < count; v++)
			{
				float in[XGPU_VERTEX_ATTRIBUTE_COUNT][4];

				draw_source.shader = shader;
				draw_source.first = first;
				draw_source.immediate_vertices = NULL;
				vertex_fetch(v, in, 1);
				nspire_log("    vertex %lu: %ld %ld %ld (x1000)", v, (long)(in[0][0] * 1000), (long)(in[0][1] * 1000),
					(long)(in[0][2] * 1000));
			}
		}
#endif
#ifdef DEBUG_PROGRAMS
		if (log_draws && draw_number == DEBUG_PROGRAMS)
		{
			extern void soft_vertex_program_dump(const void *decoded);

			nspire_log("  the draw's program (%lu):", soft_vertex_program_length(draw_source.program));
			soft_vertex_program_dump(draw_source.program);
		}
#endif
		FINE_PROFILE_BEGIN(_nspire_profile_setup_memo);
		pass_memo_begin(type, indices, vertex_count, first, program->decoded[0], state.z_test, state.z_write, state.cull_mode);
		FINE_PROFILE_END(_nspire_profile_setup_memo);
		FINE_PROFILE_END(_nspire_profile_draw_setup);
		transform_draw_vertices(type, indices, vertex_count, minimum);
		if (pass_recording && vertex_count)
		{
			unsigned long sample = (unsigned long)indices[0] - minimum;

			if (vertex_serials[sample] == vertex_serial || vertex_position_serials[sample] == vertex_serial)
			{
				memcpy(pass_recording->sample, vertex_store[sample].clip, sizeof(pass_recording->sample));
				pass_recording->sampled = TRUE;
			}
		}
	}
	if (draw_source.lit_program && state.uses_diffuse)
	{
		/* one colour all over (flat lighting): the diffuse colour is a
		register set once, not four attributes interpolated */
		int k;

		state.uses_diffuse = FALSE;
		state.constant_diffuse = TRUE;
		for (k = 0; k < 4; k++)
			state.diffuse[k] = (long)(draw_source.flat_color[k] * (float)ONE);
		for (k = 1; k + 4 < state.attribute_count; k++)
			state.attributes[k] = state.attributes[k + 4];
		state.attribute_count -= 4;
	}

	NSPIRE_PROFILE_BEGIN(_nspire_profile_pixels);
#define VERTEX(i) (indices ? (unsigned long)indices[i] - minimum : (i))
	switch (type)
	{
	case D3DPT_TRIANGLELIST:
		for (index = 0; index + 2 < vertex_count; index += 3)
			draw_triangle(VERTEX(index), VERTEX(index + 1), VERTEX(index + 2));
		break;
	case D3DPT_TRIANGLESTRIP:
		for (index = 0; index + 2 < vertex_count; index++)
		{
			if (index & 1)
				draw_triangle(VERTEX(index + 1), VERTEX(index), VERTEX(index + 2));
			else
				draw_triangle(VERTEX(index), VERTEX(index + 1), VERTEX(index + 2));
		}
		break;
	case D3DPT_TRIANGLEFAN:
	case D3DPT_POLYGON:
		if (vertex_count == 4 && fill_rectangle(VERTEX(0), VERTEX(1), VERTEX(2), VERTEX(3)))
			break;
		for (index = 1; index + 1 < vertex_count; index++)
			draw_triangle(VERTEX(0), VERTEX(index), VERTEX(index + 1));
		break;
	case D3DPT_QUADLIST:
		for (index = 0; index + 3 < vertex_count; index += 4)
		{
			if (fill_rectangle(VERTEX(index), VERTEX(index + 1), VERTEX(index + 2), VERTEX(index + 3)))
				continue;
			draw_triangle(VERTEX(index), VERTEX(index + 1), VERTEX(index + 2));
			draw_triangle(VERTEX(index), VERTEX(index + 2), VERTEX(index + 3));
		}
		break;
	case D3DPT_QUADSTRIP:
		for (index = 0; index + 3 < vertex_count; index += 2)
		{
			draw_triangle(VERTEX(index), VERTEX(index + 1), VERTEX(index + 3));
			draw_triangle(VERTEX(index), VERTEX(index + 3), VERTEX(index + 2));
		}
		break;
	default:
		/* points and lines: debugging only */
		break;
	}
#undef VERTEX
	NSPIRE_PROFILE_END(_nspire_profile_pixels);
	/* (seen: a pixel written, not merely a triangle set up, which a part
behind the level or another model does with every pixel failing depth) */
	if (part_drawn && counters.pixels_written != before.written)
		part_drawn->seen_frame = part_frame;
	if (part_instance_drawn)
	{
		/* (the first pass of a frame: the last frame drawn closes the streak) */
		if (part_instance_drawn->drawn_frame != part_frame)
		{
			if (part_instance_drawn->drawn_frame && part_instance_drawn->seen_frame < part_instance_drawn->drawn_frame)
				part_instance_drawn->empty_streak++;
			else
				part_instance_drawn->empty_streak = 0;
		}
		part_instance_drawn->drawn_frame = part_frame;
		if (counters.pixels_written != before.written)
			part_instance_drawn->seen_frame = part_frame;
	}
#ifdef COUNT_SHADES
	{
		static unsigned long last_general, last_shades;

		if (log_draws)
		{
			extern unsigned long soft_vertex_overflow_count;
			static unsigned long last_overflows;

			nspire_log("  shades: %lu, vertices again in floating point %lu", shade_count - last_shades,
				soft_vertex_overflow_count - last_overflows);
			last_overflows = soft_vertex_overflow_count;
		}
		last_shades = shade_count;

		if (log_draws && loop_pixels[2] != last_general)
			nspire_log("  general loop: %lu pixels (blend kind %d, alpha test %d, z %lu/%d, block %d)",
				loop_pixels[2] - last_general, state.blend_kind, state.alpha_test, (unsigned long)state.z_function,
				state.z_write, block_shading);
		last_general = loop_pixels[2];
	}
#endif
#ifdef DEBUG_TRIANGLES
	if (log_draws)
		nspire_log("  triangles: %lu off screen, %lu clipped, %lu culled or thin, %lu hidden", counters.rejected - before.rejected,
			counters.clipped - before.clipped, counters.culled - before.culled, counters.hidden_triangles - before.hidden);
#endif
#ifdef DEBUG_FIXED_COUNTS
	if (log_draws)
	{
		extern unsigned long soft_vertex_fixed_count, soft_vertex_float_count, soft_vertex_overflow_count;
		static unsigned long last[3];

		nspire_log("  fixed %lu float %lu overflow %lu", soft_vertex_fixed_count - last[0], soft_vertex_float_count - last[1],
			soft_vertex_overflow_count - last[2]);
		last[0] = soft_vertex_fixed_count;
		last[1] = soft_vertex_float_count;
		last[2] = soft_vertex_overflow_count;
	}
#endif
	if (log_draws && describe_combiners)
		soft_rasterizer_describe_combiners();
	if (log_draws)
	{
		nspire_log("  draw %lu: %d %lu, %lu, %lu %lu %lu, %lu %lu, %lu ms; %d %d %lx %d %d %d %d; %lu wrong; blend %lu %lu kind %d; simple %d",
			draw_number++,
			(int)type, vertex_count, soft_vertex_program_length(decoded), counters.vertices - before.vertices,
			counters.triangles - before.triangles, counters.filled - before.filled,
			counters.pixels_tested - before.tested, counters.pixels_written - before.written,
			((unsigned long)nspire_ticks() - before.start) * 1000UL / 32768UL, combiners.stage_count,
			combiners.has_final, wanted >> 1, state.blend, state.alpha_test, state.z_test, state.z_write,
			counters.mismatches - before.mismatches, state.source_blend, state.destination_blend, state.blend_kind,
			combiners_simple());
	}
}
