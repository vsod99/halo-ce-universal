/*
REPLAY.C

Draws a frame captured on the calculator (port/nspire/src/soft_capture.c)
again through the same renderer (soft_rasterizer.c, soft_vertex.c,
soft_textures.c, built as the game builds them), bare on QEMU's ARM926
board. QEMU counts instructions as time (-icount), so the per-draw and
per-section times it logs are in thousands of instructions; the frame is
written to replay_frame.ppm.

The renderer reads Xbox physical addresses through
PLATFORM_PHYSICAL_TO_VIRTUAL; build.sh points that at replay_address,
which finds them in the capture's memory records.
*/

#include "nspire.h"
#include "soft_rasterizer.h"
#include "soft_capture.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

int semihost(int operation, void *arguments);

#define SEMIHOST_OPEN 0x01
#define SEMIHOST_CLOSE 0x02
#define SEMIHOST_WRITE0 0x04
#define SEMIHOST_WRITE 0x05
#define SEMIHOST_READ 0x06
#define SEMIHOST_FLEN 0x0C
#define SEMIHOST_EXIT 0x18

#define MAXIMUM_RANGES 32768
#define MAXIMUM_PROGRAMS 512

struct soft_device soft_device;
DWORD D3D__RenderState[D3DRS_MAX];
DWORD D3D__TextureState[D3DTSS_MAXSTAGES][D3DTSS_MAX];

static struct
{
	unsigned long address, size;
	const unsigned char *data;
} ranges[MAXIMUM_RANGES];
static unsigned long range_count;

static struct
{
	unsigned long id;
	struct soft_vertex_shader *object;
} programs[MAXIMUM_PROGRAMS];
static unsigned long program_count;

static struct soft_vertex_shader *current_program;
static DWORD texture_headers[4][5];
static unsigned short screen[NSPIRE_SCREEN_WIDTH * NSPIRE_SCREEN_HEIGHT];

/* ---------- the console */

static void put_text(const char *text)
{
	semihost(SEMIHOST_WRITE0, (void *)text);
}

static void format_number(char **out, char *end, unsigned long value, int base, int width, char pad)
{
	char digits[16];
	int count = 0;

	do
	{
		digits[count++] = "0123456789abcdef"[value % base];
		value /= base;
	}
	while (value);
	while (width-- > count && *out < end)
		*(*out)++ = pad;
	while (count && *out < end)
		*(*out)++ = digits[--count];
}

/* the formats the renderer's messages use: %s %d %u %lu %ld %lx %c, widths */
static void format(char *buffer, unsigned long size, const char *text, va_list arguments)
{
	char *out = buffer, *end = buffer + size - 1;

	while (*text && out < end)
	{
		int width = 0, is_long = 0;
		char pad = ' ';

		if (*text != '%')
		{
			*out++ = *text++;
			continue;
		}
		text++;
		if (*text == '0')
			pad = '0';
		while (*text >= '0' && *text <= '9')
			width = width * 10 + (*text++ - '0');
		while (*text == 'l')
		{
			is_long = 1;
			text++;
		}
		switch (*text++)
		{
		case 's':
		{
			const char *string = va_arg(arguments, const char *);

			while (string && *string && out < end)
				*out++ = *string++;
			break;
		}
		case 'd':
		{
			long value = is_long ? va_arg(arguments, long) : va_arg(arguments, int);

			if (value < 0 && out < end)
			{
				*out++ = '-';
				value = -value;
			}
			format_number(&out, end, (unsigned long)value, 10, width, pad);
			break;
		}
		case 'u':
			format_number(&out, end, is_long ? va_arg(arguments, unsigned long) : va_arg(arguments, unsigned), 10, width, pad);
			break;
		case 'x':
			format_number(&out, end, is_long ? va_arg(arguments, unsigned long) : va_arg(arguments, unsigned), 16, width, pad);
			break;
		case 'c':
			*out++ = (char)va_arg(arguments, int);
			break;
		case '%':
			*out++ = '%';
			break;
		default:
			break;
		}
	}
	*out = 0;
}

int snprintf(char *buffer, size_t size, const char *text, ...)
{
	va_list arguments;

	if (!size)
		return 0;
	va_start(arguments, text);
	format(buffer, size, text, arguments);
	va_end(arguments);
	return (int)strlen(buffer);
}

void nspire_log(const char *text, ...)
{
	char buffer[512];
	va_list arguments;
	unsigned long length;

	va_start(arguments, text);
	format(buffer, sizeof(buffer) - 1, text, arguments);
	va_end(arguments);
	length = strlen(buffer);
	buffer[length] = '\n';
	buffer[length + 1] = 0;
	put_text(buffer);
}

void nspire_fatal(const char *text, ...)
{
	char buffer[512];
	va_list arguments;

	va_start(arguments, text);
	format(buffer, sizeof(buffer), text, arguments);
	va_end(arguments);
	put_text("fatal: ");
	put_text(buffer);
	put_text("\n");
	semihost(SEMIHOST_EXIT, (void *)0x20026);
	for (;;)
		;
}

/* ---------- what the renderer asks of the rest of the port */

#define TIMER ((volatile unsigned long *)0x101E2000)

/* in the calculator's 32768ths of a second, as if an instruction took 1/32.768 us:
the renderer's milliseconds come out as thousands of instructions */
unsigned long long nspire_ticks(void)
{
	static unsigned long last, high;
	unsigned long now = 0xFFFFFFFFUL - TIMER[1];

	if (now < last)
		high++;
	last = now;
	/* (32768/1000 is 32.768: times 33, near enough, and no division, so
	that reading the clock costs the renderer little) */
	return (((unsigned long long)high << 32) | now) * 33ULL;
}

unsigned short *nspire_video_pixels(void)
{
	return screen;
}

const char *nspire_program_directory(void)
{
	return ".";
}

struct soft_vertex_shader *soft_current_program(void)
{
	return current_program;
}

void soft_surface_dimensions(const D3DSurface *surface, unsigned long *width, unsigned long *height, BOOL *depth)
{
	(void)surface;
	*width = 640;
	*height = 480;
	*depth = FALSE;
}

BOOL soft_capture_active(void) { return FALSE; }
void soft_capture_request(void) {}
void nspire_capture_poll(void) {}
void soft_capture_frame_end(void) {}
void soft_capture_clear(long left, long top, long right, long bottom, DWORD flags, D3DCOLOR color, float z,
	BOOL to_screen) { (void)left; (void)top; (void)right; (void)bottom; (void)flags; (void)color; (void)z; (void)to_screen; }
void soft_capture_resolve(void) {}
void soft_capture_draw(D3DPRIMITIVETYPE type, unsigned long vertex_count, const WORD *indices, unsigned long start_vertex,
	const float *immediate_vertices, BOOL to_screen) { (void)type; (void)vertex_count; (void)indices; (void)start_vertex;
	(void)immediate_vertices; (void)to_screen; }

/* the capture's memory, sorted by address once it is all read (the
renderer's lookups should cost what an OR costs on the calculator: the
last range found is tried first, then a binary search) */
static unsigned long last_range;
static unsigned long replay_draw;

static void ranges_sort(void)
{
	unsigned long i, j;

	/* (insertion sort: the capture writes them mostly in order) */
	for (i = 1; i < range_count; i++)
	{
		unsigned long address = ranges[i].address, size = ranges[i].size;
		const unsigned char *data = ranges[i].data;

		for (j = i; j > 0 && ranges[j - 1].address > address; j--)
			ranges[j] = ranges[j - 1];
		ranges[j].address = address;
		ranges[j].size = size;
		ranges[j].data = data;
	}
}

void *replay_address(unsigned long physical)
{
	static unsigned char missing[4096];
	static unsigned long reported;
	unsigned long low = 0, high = range_count;

	physical &= 0x7FFFFFFFUL;
	if (last_range < range_count && physical >= ranges[last_range].address &&
		physical < ranges[last_range].address + ranges[last_range].size)
	{
		return (void *)(ranges[last_range].data + (physical - ranges[last_range].address));
	}
	while (low < high)
	{
		unsigned long middle = (low + high) / 2;

		if (ranges[middle].address + ranges[middle].size <= physical)
			low = middle + 1;
		else if (ranges[middle].address > physical)
			high = middle;
		else
		{
			last_range = middle;
			return (void *)(ranges[middle].data + (physical - ranges[middle].address));
		}
	}
	/* a stream's base before what the draw reads (its first vertex further
	on, captured from there): an address that the draw's offsets carry into
	the next range captured */
	if (low < range_count && ranges[low].address - physical < 4UL * 1024 * 1024)
		return (void *)(ranges[low].data - (ranges[low].address - physical));
	if (reported++ < 8)
		nspire_log("replay: %08lx was not captured (draw %lu)", physical, replay_draw);
	return missing;
}

/* ---------- files */

static int file_open(const char *name, int mode)
{
	unsigned long arguments[3];

	arguments[0] = (unsigned long)name;
	arguments[1] = (unsigned long)mode;
	arguments[2] = strlen(name);
	return semihost(SEMIHOST_OPEN, arguments);
}

static unsigned char *file_read(const char *name, unsigned long *size)
{
	unsigned long arguments[3];
	int handle = file_open(name, 1);
	unsigned char *data;

	if (handle < 0)
		return NULL;
	arguments[0] = (unsigned long)handle;
	*size = (unsigned long)semihost(SEMIHOST_FLEN, arguments);
	data = malloc(*size);
	arguments[1] = (unsigned long)data;
	arguments[2] = *size;
	semihost(SEMIHOST_READ, arguments);
	semihost(SEMIHOST_CLOSE, arguments);
	return data;
}

/* the screen as a PPM, to look at */
static const unsigned short *device_screen;

static void write_screen(const char *name, const unsigned short *screen)
{
	static unsigned char rgb[NSPIRE_SCREEN_WIDTH * NSPIRE_SCREEN_HEIGHT * 3];
	static const char header[] = "P6\n320 240\n255\n";
	unsigned long arguments[3], pixel;
	int handle = file_open(name, 5);

	if (handle < 0)
		return;
	for (pixel = 0; pixel < NSPIRE_SCREEN_WIDTH * NSPIRE_SCREEN_HEIGHT; pixel++)
	{
		unsigned short value = screen[pixel];

		rgb[pixel * 3] = (unsigned char)(((value >> 11) & 31) * 255 / 31);
		rgb[pixel * 3 + 1] = (unsigned char)(((value >> 5) & 63) * 255 / 63);
		rgb[pixel * 3 + 2] = (unsigned char)((value & 31) * 255 / 31);
	}
	arguments[0] = (unsigned long)handle;
	arguments[1] = (unsigned long)header;
	arguments[2] = sizeof(header) - 1;
	semihost(SEMIHOST_WRITE, arguments);
	arguments[1] = (unsigned long)rgb;
	arguments[2] = sizeof(rgb);
	semihost(SEMIHOST_WRITE, arguments);
	semihost(SEMIHOST_CLOSE, arguments);
}

/* ---------- the records */

static struct soft_vertex_shader *program_object(unsigned long id)
{
	unsigned long index;
	struct soft_vertex_shader *object;

	for (index = 0; index < program_count; index++)
	{
		if (programs[index].id == id)
			return programs[index].object;
	}
	object = calloc(1, sizeof(*object));
	object->signature = SOFT_VERTEX_SHADER_SIGNATURE;
	object->id = id;
	if (program_count < MAXIMUM_PROGRAMS)
	{
		programs[program_count].id = id;
		programs[program_count].object = object;
		program_count++;
	}
	return object;
}

static void apply_state(const struct soft_capture_state *state)
{
	unsigned long stage, stream;

	memcpy(D3D__RenderState, state->render_state, sizeof(D3D__RenderState));
	memcpy(D3D__TextureState, state->texture_state, sizeof(D3D__TextureState));
	for (stream = 0; stream < 16; stream++)
	{
		soft_device.streams[stream].data = state->streams[stream][0];
		soft_device.streams[stream].stride = state->streams[stream][1];
	}
	soft_device.base_vertex_index = state->base_vertex_index;
	soft_device.depth_scale = state->depth_scale;
	soft_device.render_target = (state->flags & 1) ? &soft_device.back_buffer : &soft_device.depth_buffer;
	soft_device.visibility_test_active = (state->flags & 2) != 0;
	current_program = program_object(state->program);
	soft_device.vertex_shader = program_object(state->shader);
	for (stage = 0; stage < 4; stage++)
	{
		memcpy(texture_headers[stage], state->textures[stage], sizeof(texture_headers[stage]));
		soft_device.textures[stage] = state->textures[stage][1] ? (D3DBaseTexture *)texture_headers[stage] : NULL;
	}
}

int main(void)
{
	unsigned long size, offset = 4, draws = 0, magic = 0;
	unsigned char *file;
	unsigned long long start;

	TIMER[2] = 0;
	TIMER[0] = 0xFFFFFFFFUL;
	TIMER[2] = 0x82;

	file = file_read("halo_frame.tns", &size);
	if (file && size >= 4)
		memcpy(&magic, file, sizeof(magic));
	if (!file || size < 4 || (magic != SOFT_CAPTURE_MAGIC && magic != SOFT_CAPTURE_MAGIC_UNPADDED))
		nspire_fatal("replay: no capture in halo_frame.tns");
	soft_rasterizer_initialize();
	soft_rasterizer_log_next_frame();
	{
		extern BOOL describe_combiners;

		describe_combiners = TRUE;
	}
	start = nspire_ticks();

	/* first the memory, all of it (a frame may draw from memory captured
after the draw: the capture writes each draw's memory as it goes) */
	{
		unsigned long at = 4;

		while (at + 8 <= size)
		{
			unsigned long tag, length;

			memcpy(&tag, file + at, sizeof(tag));
			memcpy(&length, file + at + 4, sizeof(length));
			if (tag == SOFT_CAPTURE_MEMORY && range_count < MAXIMUM_RANGES)
			{
				memcpy(&ranges[range_count].address, file + at + 8, sizeof(unsigned long));
				ranges[range_count].size = length - 4;
				ranges[range_count].data = file + at + 12;
				range_count++;
			}
			at += 8 + (magic == SOFT_CAPTURE_MAGIC ? (length + 3) & ~3UL : length);
		}
		ranges_sort();
	}
#ifdef REPLAY_TWICE
	/* the frame twice, measured the second time: what is kept between
	frames (part bounds, compiled code) is then made already */
	{
		int pass;

		for (pass = 0; pass < 2; pass++)
		{
			if (pass == 1)
			{
				extern void soft_rasterizer_frame_end(void);

				soft_rasterizer_frame_end();
				nspire_log("first pass: %lu thousand instructions; the second:",
					(unsigned long)((nspire_ticks() - start) * 1000ULL / 32768ULL));
				soft_rasterizer_report(1);
				nspire_profile_report(1);
				offset = 4;
				draws = 0;
				start = nspire_ticks();
			}
#endif
	while (offset + 8 <= size)
	{
		unsigned long tag, length;
		const unsigned char *payload;
		const unsigned long *words;

		/* (the first captures did not pad their records to words) */
		memcpy(&tag, file + offset, sizeof(tag));
		memcpy(&length, file + offset + 4, sizeof(length));
		payload = file + offset + 8;
		offset += 8 + (magic == SOFT_CAPTURE_MAGIC ? (length + 3) & ~3UL : length);
		if (((unsigned long)payload & 3) && tag != SOFT_CAPTURE_MEMORY && length)
		{
			unsigned char *aligned = malloc(length);

			memcpy(aligned, payload, length);
			payload = aligned;
		}
		words = (const unsigned long *)payload;
		switch (tag)
		{
		case SOFT_CAPTURE_MEMORY:
			/* (read before the replay) */
			break;
		case SOFT_CAPTURE_PROGRAM:
		{
			struct soft_vertex_shader *object = program_object(words[0]);

			object->instruction_count = words[1];
			object->instructions = (DWORD *)(words + 2);
			break;
		}
		case SOFT_CAPTURE_SHADER:
		{
			struct soft_vertex_shader *object = program_object(words[0]);

			object->element_count = words[1];
			memcpy(object->elements, words + 2, words[1] * sizeof(struct soft_vertex_element));
			break;
		}
		case SOFT_CAPTURE_CONSTANTS:
		{
			/* (marked written: the constants that changed, as the device
			marks those it writes) */
			unsigned long constant;

			for (constant = 0; constant < XGPU_VERTEX_CONSTANT_COUNT; constant++)
			{
				if (memcmp(soft_device.constants[constant], payload + constant * sizeof(soft_device.constants[0]),
					sizeof(soft_device.constants[0])))
				{
					soft_device.constants_dirty[constant / 32] |= 1UL << (constant % 32);
				}
			}
			memcpy(soft_device.constants, payload, sizeof(soft_device.constants));
			memcpy(soft_device.attributes, payload + sizeof(soft_device.constants), sizeof(soft_device.attributes));
			soft_device.constants_serial++;
			break;
		}
		case SOFT_CAPTURE_STATE:
			apply_state((const struct soft_capture_state *)payload);
			break;
		case SOFT_CAPTURE_DRAW:
			replay_draw = draws;
			draws++;
#ifdef REPLAY_FIRST
			/* the profile of the draws REPLAY_FIRST to REPLAY_LAST alone,
			between two reports */
			if (replay_draw == REPLAY_FIRST || replay_draw == REPLAY_LAST + 1)
			{
				nspire_log(replay_draw == REPLAY_FIRST ? "profile before draw %lu:" : "profile of the range, to draw %lu:",
					replay_draw);
				nspire_profile_report(1);
			}
#endif
#ifdef REPLAY_ONLY_MODELS
			/* (the models alone: eight combiner stages) */
			if ((D3D__RenderState[D3DRS_PSCOMBINERCOUNT] & 0xFF) != 8)
				break;
#endif
#ifdef REPLAY_DESCRIBE_QUADS
			/* (every draw of one quad, the HUD's: its textures) */
			if (words[1] == 4)
#else
			if (draws == 109 || draws == 119 || draws == 124 || draws == 134)
#endif
			{
				unsigned long stage;

				for (stage = 0; stage < 4; stage++)
				{
					if (soft_device.textures[stage])
					{
						const DWORD *resource = (const DWORD *)soft_device.textures[stage];
						struct xgpu_texture_description description;

						xgpu_texture_describe(resource[3], resource[4], &description);
						nspire_log("draw %lu stage %lu: format %lx %lux%lu levels %lu linear %d cube %d data %08lx", draws - 1,
							stage, description.format, description.width, description.height, description.levels,
							description.linear, description.cube_map, resource[1]);
					}
				}
				nspire_log("  texture modes %08lx", D3D__RenderState[D3DRS_PSTEXTUREMODES]);
			}
#ifdef REPLAY_FIRST_PERSON_FIRST
			/* (the first-person weapon's draws, which the engine marks on the device) */
			{
				extern int soft_rasterizer_first_person(int drawing);
				static int was_first_person, first_person_skipped;
				int first_person = replay_draw >= REPLAY_FIRST_PERSON_FIRST && replay_draw <= REPLAY_FIRST_PERSON_LAST;

				/* (on the changes alone, as the engine calls it; with
				REPLAY_FIRST_PERSON_CACHE, the draws it says to skip are) */
				if (first_person != was_first_person)
				{
					int draw = soft_rasterizer_first_person(first_person);

					first_person_skipped = first_person && !draw;
					was_first_person = first_person;
				}
#ifdef REPLAY_FIRST_PERSON_CACHE
				if (first_person && first_person_skipped)
					break;
#endif
			}
#endif
#ifdef REPLAY_FIND_COUNT
			/* (a draw's number here, by its vertex count) */
			if (words[1] == REPLAY_FIND_COUNT)
				nspire_log("replay draw %lu: type %lu, %lu vertices", replay_draw, words[0], words[1]);
#endif
#ifdef REPLAY_OBJECT_PER_DRAW
			/* (each draw an object of its own, as the engine says on the
			calculator: what part_hidden keeps per object) */
			{
				extern void soft_rasterizer_set_object(long object_index);

				soft_rasterizer_set_object((long)replay_draw);
			}
#endif
#ifdef REPLAY_SKIP
			/* (the frame without draw REPLAY_SKIP: what it adds) */
			if (replay_draw == REPLAY_SKIP)
				break;
#endif
#ifdef REPLAY_STOP
			/* (the image as it stands after draw REPLAY_STOP) */
			if (replay_draw > REPLAY_STOP)
				break;
#endif
			if (words[4])
				soft_draw((D3DPRIMITIVETYPE)words[0], words[1], NULL, words[2], (const float *)(words + 5));
			else
				soft_draw((D3DPRIMITIVETYPE)words[0], words[1], words[3] ? (const WORD *)(words + 5) : NULL, words[2], NULL);
			break;
		case SOFT_CAPTURE_CLEAR:
		{
			float z;

			memcpy(&z, &words[6], sizeof(z));
			soft_device.render_target = words[7] ? &soft_device.back_buffer : &soft_device.depth_buffer;
			soft_clear((long)words[0], (long)words[1], (long)words[2], (long)words[3], words[4], words[5], z);
			break;
		}
		case SOFT_CAPTURE_RESOLVE:
			soft_rasterizer_resolve();
			break;
		case SOFT_CAPTURE_SCREEN:
			/* what the device showed: kept as device_frame.ppm */
			if (words[0] == NSPIRE_SCREEN_WIDTH && words[1] == NSPIRE_SCREEN_HEIGHT)
				device_screen = (const unsigned short *)(words + 2);
			break;
		case SOFT_CAPTURE_END:
			soft_rasterizer_resolve();
			offset = size;
			break;
		default:
			nspire_fatal("replay: unknown record %lu", tag);
		}
	}
#ifdef REPLAY_TWICE
		}
	}
#endif
	nspire_log("replayed %lu draws in %lu thousand instructions", draws,
		(unsigned long)((nspire_ticks() - start) * 1000ULL / 32768ULL));
	{
		extern unsigned long soft_vertex_fixed_count, soft_vertex_float_count, soft_vertex_overflow_count;

		nspire_log("vertices: %lu fixed point (%lu of them again in floating point), %lu floating point",
			soft_vertex_fixed_count, soft_vertex_overflow_count, soft_vertex_float_count);
	}
#ifdef COUNT_SHADES
	{
		extern unsigned long shade_count, loop_pixels[3];

		nspire_log("pixels shaded: %lu; pixels through the opaque loop %lu, the layer loop %lu, the general one %lu",
			shade_count, loop_pixels[0], loop_pixels[1], loop_pixels[2]);
	}
#endif
	soft_rasterizer_report(1);
	nspire_profile_report(1);
	write_screen("replay_frame.ppm", screen);
	if (device_screen)
		write_screen("device_frame.ppm", device_screen);
	return 0;
}

/* (the game's deferred error code, which nspire_profile.c watches: none here) */
short *nspire_widget_error_watch(void)
{
	static short none = -1;

	return &none;
}
