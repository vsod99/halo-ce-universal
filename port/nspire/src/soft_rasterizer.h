/*
SOFT_RASTERIZER.H

The Nspire port's software Direct3D device (d3d8_soft.c) and rasterizer
(soft_rasterizer.c).
*/

#ifndef __HALO_NSPIRE_SOFT_RASTERIZER_H
#define __HALO_NSPIRE_SOFT_RASTERIZER_H

#include "xgpu.h"

#define SOFT_VERTEX_SHADER_SIGNATURE 0x76736864UL /* 'vshd' */
#define SOFT_VERTEX_PROGRAM_SLOTS 136
#define SOFT_VISIBILITY_SLOTS 4096

struct soft_vertex_element
{
	unsigned char reg;
	unsigned char stream;
	unsigned char type;
	unsigned char bytes;
	unsigned short offset;
};

struct soft_vertex_shader
{
	unsigned long signature;
	unsigned long id;
	DWORD *instructions;
	unsigned long instruction_count;
	struct soft_vertex_element elements[XGPU_VERTEX_ATTRIBUTE_COUNT];
	unsigned long element_count;
	/* the rasterizer's decoded programs (soft_rasterizer.c), made on first
	use, one for each set of outputs a draw reads (soft_vertex_program_decode) */
	void *decoded[32];
};

struct soft_device
{
	D3DPRESENT_PARAMETERS presentation;
	D3DSurface back_buffer;
	D3DSurface depth_buffer;
	D3DSurface *render_target;
	D3DSurface *depth_stencil;
	D3DVIEWPORT8 viewport;
	D3DMATRIX transforms[D3DTS_MAX];
	D3DBaseTexture *textures[D3DTSS_MAXSTAGES];
	D3DPalette *palettes[D3DTSS_MAXSTAGES];
	D3DSHADERCONSTANTMODE shader_constant_mode;

	struct soft_vertex_shader *vertex_shader;
	struct soft_vertex_shader *program_slots[SOFT_VERTEX_PROGRAM_SLOTS];
	unsigned long program_address;
	float constants[XGPU_VERTEX_CONSTANT_COUNT][4];
	unsigned long constants_serial;
	/* the constants written since soft_vertex.c last took them into fixed
	point (bit n of word n / 32) */
	unsigned long constants_dirty[(XGPU_VERTEX_CONSTANT_COUNT + 31) / 32];
	float viewport_scale[4];
	float viewport_offset[4];
	float depth_scale;

	struct
	{
		DWORD data;
		UINT stride;
	} streams[16];
	UINT base_vertex_index;

	float attributes[XGPU_VERTEX_ATTRIBUTE_COUNT][4];
	BOOL immediate_active;
	D3DPRIMITIVETYPE immediate_type;
	float *immediate_vertices;
	unsigned long immediate_count;
	unsigned long immediate_capacity;

	BOOL visibility_test_active;
	unsigned long visibility_count;
	unsigned long visibility_results[SOFT_VISIBILITY_SLOTS];

	unsigned long frame;
	unsigned long next_vertex_shader_id;
	BOOL created;
};

extern struct soft_device soft_device;

/* a vertex program's results (soft_vertex.c) */
struct soft_vertex_output
{
	/* oPos, in screen space */
	float position[4];
	/* the clip-space position, when the program's screen conversion was
	seen (rcc of r12.w) */
	float clip[4];
	BOOL clip_captured;
	/* the same in 16.16 fixed point, when a fixed-point run captured it */
	long clip_fixed[4];
	BOOL clip_fixed_valid;
	float diffuse[4];
	float texture[4][4];
	float fog;
	/* u and v of each texture in 16.16 instead, from a fixed-point run
	(texture then unset) */
	long texture_fixed[4][2];
	BOOL texture_fixed_valid;
};

/* soft_vertex.c */
/* wanted: bit 0 the diffuse colour, bits 1-4 texture coordinates 0-3 */
void *soft_vertex_program_decode(const DWORD *instructions, unsigned long count, unsigned long wanted);
unsigned long soft_vertex_program_length(const void *decoded);
/* a hash of the constants a program reads (0: it reads them through a0) */
unsigned long soft_vertex_program_key(const void *decoded, const float constants[][4]);
/* the input registers a program reads (bit n for v[n]) */
unsigned long soft_vertex_program_inputs(const void *decoded);
/* vertices fetched straight into 16.16 fixed point: whether a program can run
them (and its constants made ready), then the run; overflow[] marks those
that must run again in floating point */
int soft_vertex_program_prepare_fixed(const void *decoded, const float constants[][4], unsigned long constants_serial);
/* positions over 2^shift in the fixed-point runs that follow, with the
translations that go with them (the rasterizer's fetch shifts the
positions); and back */
void soft_vertex_fixed_scale_positions(const void *decoded, int shift);
void soft_vertex_fixed_unscale_positions(void);
void soft_vertex_program_run_fixed(const void *decoded, unsigned long count,
	const long inputs[][XGPU_VERTEX_ATTRIBUTE_COUNT][4], struct soft_vertex_output *outputs, int overflow[]);
void soft_vertex_program_run(const void *program, const float inputs[][4], const float constants[][4],
	struct soft_vertex_output *output);
/* vertices run together: each instruction is worked out once for up to
SOFT_VERTEX_BATCH of them */
#define SOFT_VERTEX_BATCH 16
/* constants_serial: changes when the constants do (0: they may have) */
void soft_vertex_program_run_batch(const void *program, unsigned long count,
	const float inputs[][XGPU_VERTEX_ATTRIBUTE_COUNT][4], const float constants[][4], unsigned long constants_serial,
	struct soft_vertex_output *outputs);

/* d3d8_soft.c */
struct soft_vertex_shader *soft_current_program(void);
void soft_surface_dimensions(const D3DSurface *surface, unsigned long *width, unsigned long *height, BOOL *depth);

/* soft_rasterizer.c */
void soft_rasterizer_initialize(void);
/* a draw: indices (NULL for consecutive vertices from start_vertex), or
immediate-mode vertices (every register, four floats each) */
void soft_draw(D3DPRIMITIVETYPE type, unsigned long vertex_count, const WORD *indices, unsigned long start_vertex,
	const float *immediate_vertices);
/* a clear of the current targets, in the game's 640x480 coordinates */
void soft_clear(long left, long top, long right, long bottom, DWORD flags, D3DCOLOR color, float z);
unsigned long soft_rasterizer_take_primitive_count(void);
/* counts per frame over the given frames, logged; and the next frame's
draws logged one by one, until soft_rasterizer_frame_end */
void soft_rasterizer_report(unsigned long frames);
void soft_rasterizer_log_next_frame(void);
void soft_rasterizer_frame_end(void);
/* the frame's 3D world (drawn at 160x120) onto the screen, if it is not yet */
void soft_rasterizer_resolve(void);

#endif
