/*
SOFT_CAPTURE.H

A frame of the software renderer to a file and back (soft_capture.c, and
the replay in tools/nspire_replay).
*/

#ifndef __HALO_NSPIRE_SOFT_CAPTURE_H
#define __HALO_NSPIRE_SOFT_CAPTURE_H

/* records padded to whole words ('HFR2'); 'HFR1' files were not */
#define SOFT_CAPTURE_MAGIC 0x32524648UL
#define SOFT_CAPTURE_MAGIC_UNPADDED 0x31524648UL

enum
{
	SOFT_CAPTURE_MEMORY = 1,     /* address, bytes */
	SOFT_CAPTURE_PROGRAM,        /* id, instruction count, instructions */
	SOFT_CAPTURE_SHADER,         /* id, element count, struct soft_vertex_element[] */
	SOFT_CAPTURE_CONSTANTS,      /* soft_device.constants, then .attributes */
	SOFT_CAPTURE_STATE,          /* struct soft_capture_state */
	SOFT_CAPTURE_DRAW,           /* type, count, start vertex, indexed, immediate; then indices or vertices */
	SOFT_CAPTURE_CLEAR,          /* left, top, right, bottom, flags, colour, z, to the screen */
	SOFT_CAPTURE_RESOLVE,
	SOFT_CAPTURE_END,
	SOFT_CAPTURE_SCREEN,         /* width, height, the screen's 16-bit pixels as the device showed them */
};

struct soft_capture_state
{
	unsigned long render_state[144];
	unsigned long texture_state[4][32];
	unsigned long streams[16][2];
	unsigned long base_vertex_index;
	float depth_scale;
	/* bit 0: to the screen; bit 1: a visibility test is counting */
	unsigned long flags;
	unsigned long program, shader;
	/* each stage's texture header (Common, Data, Lock, Format, Size), or zeros */
	unsigned long textures[4][5];
};

BOOL soft_capture_active(void);
void soft_capture_request(void);
/* var looked at (xinput_nspire.c) */
void nspire_capture_poll(void);
void soft_capture_frame_end(void);
/* the screen as presented (d3d8_soft.c), to compare the replay's with */
void soft_capture_screen(const unsigned short *pixels, unsigned long width, unsigned long height);
void soft_capture_clear(long left, long top, long right, long bottom, DWORD flags, D3DCOLOR color, float z,
	BOOL to_screen);
void soft_capture_resolve(void);
void soft_capture_draw(D3DPRIMITIVETYPE type, unsigned long vertex_count, const WORD *indices, unsigned long start_vertex,
	const float *immediate_vertices, BOOL to_screen);

#endif
