/*
SOFT_VERTEX.C

NV2A vertex programs, run on the calculator's CPU (the Nspire port's
software renderer, soft_rasterizer.c).

The instruction format is the one port/linux/src/nv2a_vsh.c translates to
GLSL: four words, a MAC (vector) and an ILU (scalar) operation on the same
three operands, results written once both have read them, the ILU's to r1
when both units write. Each program is decoded once into soft_vp_*
records; vertices then run through them in batches (soft_vertex_program_
run_batch), each instruction's operands worked out once for the batch.

Only what the draw uses is computed: decoding takes the outputs wanted,
works back through the program to find which components of each result
are read, and drops instructions nothing reads (fog, specular colour, and
the screen-space position when the clip-space one is captured).

Xbox programs end by turning their clip-space position into screen space
(multiply by c[-38], take rcc of w, add c[-37]). The rasterizer clips in
clip space, so the position is also kept as it is when its w is taken the
reciprocal of (as port/linux/src/nv2a_vsh.c does for Android).
*/

#include "soft_rasterizer.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum
{
	_mac_nop, _mac_mov, _mac_mul, _mac_add, _mac_mad, _mac_dp3, _mac_dph, _mac_dp4,
	_mac_dst, _mac_min, _mac_max, _mac_slt, _mac_sge, _mac_arl,
};

enum
{
	_ilu_nop, _ilu_mov, _ilu_rcp, _ilu_rcc, _ilu_rsq, _ilu_exp, _ilu_log, _ilu_lit,
};

enum
{
	_mux_unknown, _mux_temporary, _mux_input, _mux_constant,
};

/* the output registers (o[]) by address */
enum
{
	_output_position = 0,
	_output_diffuse = 3,
	_output_specular = 4,
	_output_fog = 5,
	_output_point_size = 6,
	_output_back_diffuse = 7,
	_output_back_specular = 8,
	_output_texture0 = 9,
	NUMBER_OF_OUTPUT_ADDRESSES = 13,
};

struct soft_vp_operand
{
	unsigned char mux;
	unsigned char index;
	unsigned char negate;
	unsigned char swizzle[4];
	unsigned char identity_swizzle;
	unsigned short constant;
};

struct soft_vp_instruction
{
	unsigned char mac, ilu;
	unsigned char mac_mask, ilu_mask, output_mask;
	unsigned char mac_temporary, ilu_temporary;
	unsigned char output_is_register, output_address, output_from_ilu;
	unsigned char relative;
	/* rcc of r12.w: the clip-space position is r12 now */
	unsigned char captures_clip;
	/* the components computed (bit 0 x ... bit 3 w), and those read from
	each operand */
	unsigned char mac_lanes, ilu_lanes;
	unsigned char operand_lanes[3];
	struct soft_vp_operand operands[3];
};

struct soft_vp_program
{
	/* whether it can run in fixed point (run_batch_fixed): its operations
	all can, and no reciprocal or exponential is used */
	BOOL fixed;
	/* the input registers it reads (bit n for v[n]), and the outputs it writes
	(bit n for o[n]) */
	unsigned long inputs_read, outputs_written;
	/* its fixed-point routine (vjit_routine): 0 not yet written, 1 written,
	2 cannot be */
	int code_state;
	void *code;
	/* the constants it reads by number (bit n of word n / 32), and whether it
	reads any through a0 */
	unsigned long constants_read[(XGPU_VERTEX_CONSTANT_COUNT + 31) / 32];
	BOOL relative;
	/* temporaries read before written (bit n for r[n]) */
	unsigned long temporaries_read_first;
	unsigned long count;
	struct soft_vp_instruction instructions[1];
};

static unsigned long field(const DWORD *instruction, int word, int low_bit, int bit_count)
{
	return (instruction[word] >> low_bit) & ((1UL << bit_count) - 1);
}

static void decode_operand(const DWORD *instruction, int which, struct soft_vp_operand *operand)
{
	switch (which)
	{
	case 0:
		operand->negate = (unsigned char)field(instruction, 1, 8, 1);
		operand->swizzle[0] = (unsigned char)field(instruction, 1, 6, 2);
		operand->swizzle[1] = (unsigned char)field(instruction, 1, 4, 2);
		operand->swizzle[2] = (unsigned char)field(instruction, 1, 2, 2);
		operand->swizzle[3] = (unsigned char)field(instruction, 1, 0, 2);
		operand->index = (unsigned char)field(instruction, 2, 28, 4);
		operand->mux = (unsigned char)field(instruction, 2, 26, 2);
		break;
	case 1:
		operand->negate = (unsigned char)field(instruction, 2, 25, 1);
		operand->swizzle[0] = (unsigned char)field(instruction, 2, 23, 2);
		operand->swizzle[1] = (unsigned char)field(instruction, 2, 21, 2);
		operand->swizzle[2] = (unsigned char)field(instruction, 2, 19, 2);
		operand->swizzle[3] = (unsigned char)field(instruction, 2, 17, 2);
		operand->index = (unsigned char)field(instruction, 2, 13, 4);
		operand->mux = (unsigned char)field(instruction, 2, 11, 2);
		break;
	default:
		operand->negate = (unsigned char)field(instruction, 2, 10, 1);
		operand->swizzle[0] = (unsigned char)field(instruction, 2, 8, 2);
		operand->swizzle[1] = (unsigned char)field(instruction, 2, 6, 2);
		operand->swizzle[2] = (unsigned char)field(instruction, 2, 4, 2);
		operand->swizzle[3] = (unsigned char)field(instruction, 2, 2, 2);
		operand->index = (unsigned char)((field(instruction, 2, 0, 2) << 2) | field(instruction, 3, 30, 2));
		operand->mux = (unsigned char)field(instruction, 3, 28, 2);
		break;
	}
	/* inputs are named by one field for all three operands */
	if (operand->mux == _mux_input)
		operand->index = (unsigned char)field(instruction, 1, 9, 4);
	operand->constant = (unsigned short)field(instruction, 1, 13, 8);
	operand->identity_swizzle = operand->swizzle[0] == 0 && operand->swizzle[1] == 1 &&
		operand->swizzle[2] == 2 && operand->swizzle[3] == 3;
}

/* a write mask (bit 3 x) as lanes (bit 0 x) */
static unsigned char mask_lanes(unsigned char mask)
{
	return (unsigned char)(((mask >> 3) & 1) | ((mask >> 1) & 2) | ((mask << 1) & 4) | ((mask << 3) & 8));
}

/* the components of a source an operand reads for the given lanes */
static unsigned char operand_source_lanes(const struct soft_vp_operand *operand, unsigned char lanes)
{
	unsigned char source = 0;
	int lane;

	for (lane = 0; lane < 4; lane++)
	{
		if (lanes & (1 << lane))
			source |= (unsigned char)(1 << operand->swizzle[lane]);
	}
	return source;
}

/* the lanes each operand reads when a MAC operation computes the given
lanes */
static void mac_operand_lanes(unsigned char mac, unsigned char lanes, unsigned char out[3])
{
	out[0] = out[1] = out[2] = 0;
	if (!lanes)
		return;
	switch (mac)
	{
	case _mac_mov: out[0] = lanes; break;
	case _mac_arl: out[0] = 1; break;
	case _mac_add: out[0] = out[2] = lanes; break;
	case _mac_mad: out[0] = out[1] = out[2] = lanes; break;
	case _mac_dp3: out[0] = out[1] = 7; break;
	case _mac_dph: out[0] = 7; out[1] = 15; break;
	case _mac_dp4: out[0] = out[1] = 15; break;
	case _mac_dst:
		out[0] = lanes & 6;
		out[1] = lanes & 10;
		break;
	case _mac_nop: break;
	default: out[0] = out[1] = lanes; break;
	}
}

static unsigned char ilu_operand_lanes(unsigned char ilu, unsigned char lanes)
{
	if (!lanes || ilu == _ilu_nop)
		return 0;
	if (ilu == _ilu_mov)
		return lanes;
	if (ilu == _ilu_lit)
		return 11;
	return 1;
}

/* works back through the program: which lanes of each result are read,
given the outputs wanted (bit 0 the diffuse colour, bits 1-4 texture
coordinates 0-3), then drops what computes nothing */
static void program_prune(struct soft_vp_program *program, unsigned long wanted)
{
	unsigned char live[13];
	unsigned char output_live[NUMBER_OF_OUTPUT_ADDRESSES];
	BOOL captures = FALSE;
	long index;
	unsigned long kept;
	int stage;

	memset(output_live, 0, sizeof(output_live));
	if (wanted & 1)
		output_live[_output_diffuse] = 15;
	for (stage = 0; stage < 4; stage++)
	{
		if (wanted & (2 << stage))
			output_live[_output_texture0 + stage] = 3;
	}
	for (index = 0; index < (long)program->count; index++)
	{
		if (program->instructions[index].captures_clip)
			captures = TRUE;
	}
	memset(live, 0, sizeof(live));
	/* without a capture, the rasterizer takes the screen position back */
	if (!captures)
		live[12] = 15;

	for (index = (long)program->count - 1; index >= 0; index--)
	{
		struct soft_vp_instruction *instruction = &program->instructions[index];
		unsigned char mac_lanes = 0, ilu_lanes = 0, mac_reads[3], operand;
		unsigned char mac_write = 0, ilu_write = 0, output_write = 0;
		BOOL output_to_r12 = instruction->output_is_register && instruction->output_address == _output_position;

		if (instruction->mac != _mac_nop && instruction->mac != _mac_arl && instruction->mac_temporary <= 12)
			mac_write = mask_lanes(instruction->mac_mask);
		if (instruction->ilu != _ilu_nop && instruction->ilu_temporary <= 12)
			ilu_write = mask_lanes(instruction->ilu_mask);
		if (instruction->output_is_register && instruction->output_address < NUMBER_OF_OUTPUT_ADDRESSES &&
			(instruction->output_from_ilu ? instruction->ilu : instruction->mac) != 0)
		{
			output_write = mask_lanes(instruction->output_mask);
		}

		/* the lanes of each unit's result something reads */
		if (instruction->mac == _mac_arl)
			mac_lanes = 1;
		else if (mac_write)
			mac_lanes |= mac_write & live[instruction->mac_temporary];
		if (ilu_write)
			ilu_lanes |= ilu_write & live[instruction->ilu_temporary];
		if (output_write)
		{
			unsigned char read = output_write & (output_to_r12 ? live[12] : output_live[instruction->output_address]);

			if (instruction->output_from_ilu)
				ilu_lanes |= read;
			else
				mac_lanes |= read;
		}
		/* scalar results: one computation serves every lane */
		if (instruction->ilu != _ilu_mov && instruction->ilu != _ilu_exp && instruction->ilu != _ilu_log &&
			instruction->ilu != _ilu_lit && ilu_lanes)
		{
			ilu_lanes = 15;
		}

		/* then its writes kill, and its reads make live */
		if (mac_write)
			live[instruction->mac_temporary] &= (unsigned char)~mac_write;
		if (ilu_write)
			live[instruction->ilu_temporary] &= (unsigned char)~ilu_write;
		if (output_write && output_to_r12)
			live[12] &= (unsigned char)~output_write;

		instruction->mac_lanes = mac_lanes;
		instruction->ilu_lanes = ilu_lanes;
		mac_operand_lanes(instruction->mac, mac_lanes, mac_reads);
		instruction->operand_lanes[0] = mac_reads[0];
		instruction->operand_lanes[1] = mac_reads[1];
		instruction->operand_lanes[2] = (unsigned char)(mac_reads[2] | ilu_operand_lanes(instruction->ilu, ilu_lanes));
		if (instruction->captures_clip)
		{
			/* the capture reads all of r12 (and is kept: below) */
			instruction->operand_lanes[2] |= 1;
		}
		for (operand = 0; operand < 3; operand++)
		{
			const struct soft_vp_operand *o = &instruction->operands[operand];

			if (o->mux == _mux_temporary && o->index <= 12)
				live[o->index] |= operand_source_lanes(o, instruction->operand_lanes[operand]);
		}
		if (instruction->captures_clip)
			live[12] = 15;
	}
	/* the temporaries read before they are written (live at the start): the
	only ones a run clears for each vertex */
	program->temporaries_read_first = 0;
	for (stage = 0; stage <= 12; stage++)
	{
		if (live[stage])
			program->temporaries_read_first |= 1UL << stage;
	}

	for (index = 0, kept = 0; index < (long)program->count; index++)
	{
		const struct soft_vp_instruction *instruction = &program->instructions[index];

		if (instruction->mac_lanes || instruction->ilu_lanes || instruction->captures_clip)
			program->instructions[kept++] = *instruction;
	}
	program->count = kept;
}

/* what program_fixed and the input conversion need to know */
static void program_classify(struct soft_vp_program *program)
{
	unsigned long index;
	int operand;

	program->fixed = TRUE;
	program->inputs_read = 0;
	program->outputs_written = 0;
	program->code_state = 0;
	program->code = NULL;
	program->relative = FALSE;
	memset(program->constants_read, 0, sizeof(program->constants_read));
	for (index = 0; index < program->count; index++)
	{
		const struct soft_vp_instruction *instruction = &program->instructions[index];

		if (instruction->mac_lanes && (instruction->mac == _mac_dst || instruction->mac > _mac_arl))
			program->fixed = FALSE;
		if (instruction->ilu_lanes && instruction->ilu != _ilu_nop && instruction->ilu != _ilu_mov)
			program->fixed = FALSE;
		for (operand = 0; operand < 3; operand++)
		{
			if (instruction->operand_lanes[operand] && instruction->operands[operand].mux == _mux_input)
				program->inputs_read |= 1UL << instruction->operands[operand].index;
			if (instruction->operand_lanes[operand] && instruction->operands[operand].mux == _mux_constant)
			{
				unsigned short constant = instruction->operands[operand].constant;

				if (instruction->relative)
					program->relative = TRUE;
				else if (constant < XGPU_VERTEX_CONSTANT_COUNT)
					program->constants_read[constant / 32] |= 1UL << (constant % 32);
			}
		}
		if (instruction->output_mask && instruction->output_is_register &&
			instruction->output_address < NUMBER_OF_OUTPUT_ADDRESSES)
		{
			program->outputs_written |= 1UL << instruction->output_address;
		}
	}
}

unsigned long soft_vertex_program_inputs(const void *decoded)
{
	return decoded ? ((const struct soft_vp_program *)decoded)->inputs_read : ~0UL;
}

unsigned long soft_vertex_program_length(const void *decoded)
{
	return decoded ? ((const struct soft_vp_program *)decoded)->count : 0;
}

/* a hash of the constants a program reads (0 when it reads them through
a0) */
unsigned long soft_vertex_program_key(const void *decoded, const float constants[][4])
{
	const struct soft_vp_program *program = decoded;
	const unsigned long *words;
	unsigned long hash = 2166136261UL, constant;

	if (!program || program->relative)
		return 0;
	for (constant = 0; constant < XGPU_VERTEX_CONSTANT_COUNT; constant++)
	{
		if (!(program->constants_read[constant / 32] >> (constant % 32) & 1))
			continue;
		words = (const unsigned long *)constants[constant];
		hash = (hash ^ constant) * 16777619UL;
		hash = (hash ^ words[0]) * 16777619UL;
		hash = (hash ^ words[1]) * 16777619UL;
		hash = (hash ^ words[2]) * 16777619UL;
		hash = (hash ^ words[3]) * 16777619UL;
	}
	return hash ? hash : 1;
}

static void program_classify(struct soft_vp_program *program);

void *soft_vertex_program_decode(const DWORD *instructions, unsigned long count, unsigned long wanted)
{
	struct soft_vp_program *program = malloc(sizeof(*program) +
		(count ? count - 1 : 0) * sizeof(struct soft_vp_instruction));
	unsigned long index;

	if (!program)
		return NULL;
	program->count = 0;
	for (index = 0; index < count; index++)
	{
		const DWORD *instruction = instructions + index * 4;
		struct soft_vp_instruction *decoded = &program->instructions[program->count++];
		int operand;

		memset(decoded, 0, sizeof(*decoded));
		decoded->mac = (unsigned char)field(instruction, 1, 21, 4);
		decoded->ilu = (unsigned char)field(instruction, 1, 25, 3);
		decoded->mac_mask = (unsigned char)field(instruction, 3, 24, 4);
		decoded->mac_temporary = (unsigned char)field(instruction, 3, 20, 4);
		decoded->ilu_mask = (unsigned char)field(instruction, 3, 16, 4);
		decoded->ilu_temporary = decoded->mac != _mac_nop ? 1 : decoded->mac_temporary;
		decoded->output_mask = (unsigned char)field(instruction, 3, 12, 4);
		decoded->output_is_register = (unsigned char)field(instruction, 3, 11, 1);
		decoded->output_address = (unsigned char)field(instruction, 3, 3, 8);
		decoded->output_from_ilu = (unsigned char)field(instruction, 3, 2, 1);
		decoded->relative = (unsigned char)field(instruction, 3, 1, 1);
		for (operand = 0; operand < 3; operand++)
			decode_operand(instruction, operand, &decoded->operands[operand]);
		decoded->captures_clip = decoded->ilu == _ilu_rcc && decoded->operands[2].mux == _mux_temporary &&
			decoded->operands[2].index == 12;
		if (field(instruction, 3, 0, 1))
			break;
	}
	program_prune(program, wanted);
	program_classify(program);
	return program;
}

/* ---------- running

Vertices run in batches: each instruction's operands are worked out once
for the batch (a constant's swizzle and sign too), then the instruction runs
for every vertex of it, so the decoding costs a batch what it cost one
vertex. */

#define BATCH SOFT_VERTEX_BATCH

struct batch
{
	/* r0-r11, and r12, which is oPos (output 0 writes it too) */
	float r[13][BATCH][4];
	float o[NUMBER_OF_OUTPUT_ADDRESSES][BATCH][4];
	long a0[BATCH];
};

/* fixed-point programs as machine code (vjit_routine) */
#ifndef NSPIRE_VERTEX_CODE
#define NSPIRE_VERTEX_CODE 1
#endif

#define FMUL(a, b) ((a) * (b))
#define FADD(a, b) ((a) + (b))

typedef union
{
	float f;
	unsigned long u;
} float_bits_t;

static const float zero_vector[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

static void swizzle_into(const struct soft_vp_operand *operand, const float *source, unsigned lanes, float out[4])
{
	if (lanes & 1) out[0] = source[operand->swizzle[0]];
	if (lanes & 2) out[1] = source[operand->swizzle[1]];
	if (lanes & 4) out[2] = source[operand->swizzle[2]];
	if (lanes & 8) out[3] = source[operand->swizzle[3]];
	if (operand->negate)
	{
		/* (a float's sign is its top bit: no library call) */
		unsigned long *bits = (unsigned long *)out;

		if (lanes & 1) bits[0] ^= 0x80000000UL;
		if (lanes & 2) bits[1] ^= 0x80000000UL;
		if (lanes & 4) bits[2] ^= 0x80000000UL;
		if (lanes & 8) bits[3] ^= 0x80000000UL;
	}
}

static __inline__ __attribute__((always_inline)) void write_masked(float *destination, const float *value,
	unsigned long mask)
{
	/* bit 3 is x */
	if (mask & 8) destination[0] = value[0];
	if (mask & 4) destination[1] = value[1];
	if (mask & 2) destination[2] = value[2];
	if (mask & 1) destination[3] = value[3];
}

static float reciprocal_clamped(float x)
{
	float r = 1.0f / x;

	if (r > 0.0f)
		return r < 5.42101e-20f ? 5.42101e-20f : r > 1.884467e+19f ? 1.884467e+19f : r;
	return r > -5.42101e-20f ? -5.42101e-20f : r < -1.884467e+19f ? -1.884467e+19f : r;
}

static void ilu_compute(unsigned char ilu, const float *c, float ilu_result[4])
{
	switch (ilu)
	{
	case _ilu_mov:
		ilu_result[0] = c[0]; ilu_result[1] = c[1]; ilu_result[2] = c[2]; ilu_result[3] = c[3];
		break;
	case _ilu_rcp:
		ilu_result[0] = ilu_result[1] = ilu_result[2] = ilu_result[3] = 1.0f / c[0];
		break;
	case _ilu_rcc:
		ilu_result[0] = ilu_result[1] = ilu_result[2] = ilu_result[3] = reciprocal_clamped(c[0]);
		break;
	case _ilu_rsq:
		ilu_result[0] = ilu_result[1] = ilu_result[2] = ilu_result[3] = 1.0f / sqrtf(fabsf(c[0]));
		break;
	case _ilu_exp:
	{
		float whole = floorf(c[0]);

		ilu_result[0] = exp2f(whole); ilu_result[1] = c[0] - whole; ilu_result[2] = exp2f(c[0]); ilu_result[3] = 1.0f;
		break;
	}
	case _ilu_log:
	{
		float x = fabsf(c[0]);

		if (x == 0.0f)
		{
			ilu_result[0] = -1.0e30f; ilu_result[1] = 1.0f; ilu_result[2] = -1.0e30f; ilu_result[3] = 1.0f;
		}
		else
		{
			float e = floorf(log2f(x));

			ilu_result[0] = e; ilu_result[1] = x / exp2f(e); ilu_result[2] = log2f(x); ilu_result[3] = 1.0f;
		}
		break;
	}
	case _ilu_lit:
	{
		float power = c[3] < -127.9961f ? -127.9961f : c[3] > 127.9961f ? 127.9961f : c[3];

		ilu_result[0] = 1.0f;
		ilu_result[1] = c[0] > 0.0f ? c[0] : 0.0f;
		ilu_result[2] = c[0] > 0.0f ? powf(c[1] > 0.0f ? c[1] : 0.0f, power) : 0.0f;
		ilu_result[3] = 1.0f;
		break;
	}
	default:
		break;
	}
}

/* an operand, ready for the batch: where vertex 0's four floats are, the
step to the next vertex's (0 when all share them), and which of them each
lane reads */
struct lean_operand
{
	const float *base;
	unsigned long stride;
	int x, y, z, w;
	/* a constant indexed by a0, which differs between vertices */
	BOOL relative;
	unsigned short constant;
};

static struct lean_operand lean_prepare(struct batch *machine, const struct soft_vp_operand *operand, int relative,
	const float (*inputs)[XGPU_VERTEX_ATTRIBUTE_COUNT][4], const float (*constants)[4], float shared[4])
{
	struct lean_operand result, *out = &result;

	out->relative = FALSE;
	out->x = operand->swizzle[0];
	out->y = operand->swizzle[1];
	out->z = operand->swizzle[2];
	out->w = operand->swizzle[3];
	switch (operand->mux)
	{
	case _mux_temporary:
		out->base = operand->index <= 12 ? machine->r[operand->index][0] : zero_vector;
		out->stride = operand->index <= 12 ? 4 : 0;
		break;
	case _mux_input:
		out->base = inputs[0][operand->index];
		out->stride = XGPU_VERTEX_ATTRIBUTE_COUNT * 4;
		break;
	case _mux_constant:
		if (relative)
		{
			out->relative = TRUE;
			out->constant = operand->constant;
			out->base = constants[0];
			out->stride = 0;
			break;
		}
		/* the same for every vertex: swizzled and negated once */
		swizzle_into(operand, constants[operand->constant], 15, shared);
		out->base = shared;
		out->stride = 0;
		out->x = 0;
		out->y = 1;
		out->z = 2;
		out->w = 3;
		break;
	default:
		out->base = zero_vector;
		out->stride = 0;
		break;
	}
	return result;
}

/* the operand's floats for a vertex, in place; a negated one (not a
constant: those are negated once) is copied out negated */
static __inline__ __attribute__((always_inline)) const float *lean_get(const struct batch *machine,
	struct lean_operand operand_value, BOOL negate, unsigned long vertex, float temporary[4])
{
	const float *values;
	const struct lean_operand *operand = &operand_value;

	if (operand->relative)
	{
		long index = (long)operand->constant + machine->a0[vertex];

		if (index < 0)
			index = 0;
		if (index >= XGPU_VERTEX_CONSTANT_COUNT)
			index = XGPU_VERTEX_CONSTANT_COUNT - 1;
		values = operand->base + index * 4;
	}
	else
	{
		values = operand->base + vertex * operand->stride;
	}
	if (negate)
	{
		float_bits_t *t = (float_bits_t *)temporary;
		const float_bits_t *v = (const float_bits_t *)values;

		t[0].u = v[0].u ^ 0x80000000UL;
		t[1].u = v[1].u ^ 0x80000000UL;
		t[2].u = v[2].u ^ 0x80000000UL;
		t[3].u = v[3].u ^ 0x80000000UL;
		return temporary;
	}
	return values;
}

static void run_batch(const struct soft_vp_program *program, unsigned long count,
	const float (*inputs)[XGPU_VERTEX_ATTRIBUTE_COUNT][4], const float (*constants)[4],
	struct soft_vertex_output *outputs)
{
	static struct batch machine;
	unsigned long index, vertex;

	memset(machine.r, 0, sizeof(machine.r[0]) * 13);
	for (vertex = 0; vertex < count; vertex++)
	{
		static const float defaults[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		static const float ones[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

		memcpy(machine.o[_output_diffuse][vertex], defaults, sizeof(defaults));
		memcpy(machine.o[_output_texture0][vertex], defaults, sizeof(defaults));
		memcpy(machine.o[_output_texture0 + 1][vertex], defaults, sizeof(defaults));
		memcpy(machine.o[_output_texture0 + 2][vertex], defaults, sizeof(defaults));
		memcpy(machine.o[_output_texture0 + 3][vertex], defaults, sizeof(defaults));
		memcpy(machine.o[_output_fog][vertex], ones, sizeof(ones));
		machine.a0[vertex] = 0;
		outputs[vertex].clip_captured = FALSE;
		outputs[vertex].clip_fixed_valid = FALSE;
		outputs[vertex].texture_fixed_valid = FALSE;
	}

	for (index = 0; index < program->count; index++)
	{
		const struct soft_vp_instruction *instruction = &program->instructions[index];
		const struct soft_vp_operand *operands = instruction->operands;
		struct lean_operand A = { zero_vector, 0, 0, 1, 2, 3, FALSE, 0 }, B = A, C = A;
		float shared[3][4];
		unsigned lanes = instruction->mac_lanes, ilu_lanes = instruction->ilu_lanes;
		unsigned char mac = lanes ? instruction->mac : _mac_nop;
		unsigned char ilu = ilu_lanes ? instruction->ilu : _ilu_nop;
		/* constants are negated as they are prepared */
		BOOL negate_a = operands[0].negate && operands[0].mux != _mux_constant;
		BOOL negate_b = operands[1].negate && operands[1].mux != _mux_constant;
		BOOL negate_c = operands[2].negate && operands[2].mux != _mux_constant;
		float *mac_destination = NULL, *ilu_destination = NULL, *output_destination = NULL;
		unsigned long mac_mask = instruction->mac_mask, ilu_mask = instruction->ilu_mask;
		unsigned long output_mask = instruction->output_mask;
		BOOL output_from_ilu = instruction->output_from_ilu;
		BOOL captures = instruction->captures_clip;
		const BOOL use_a = instruction->operand_lanes[0] != 0, use_b = instruction->operand_lanes[1] != 0;
		const BOOL use_c = instruction->operand_lanes[2] != 0;

		if (instruction->operand_lanes[0])
			A = lean_prepare(&machine, &operands[0], instruction->relative, inputs, constants, shared[0]);
		if (instruction->operand_lanes[1])
			B = lean_prepare(&machine, &operands[1], instruction->relative, inputs, constants, shared[1]);
		if (instruction->operand_lanes[2])
			C = lean_prepare(&machine, &operands[2], instruction->relative, inputs, constants, shared[2]);
		if (mac != _mac_nop && mac != _mac_arl && mac_mask && instruction->mac_temporary <= 12)
			mac_destination = machine.r[instruction->mac_temporary][0];
		if (ilu != _ilu_nop && ilu_mask && instruction->ilu_temporary <= 12)
			ilu_destination = machine.r[instruction->ilu_temporary][0];
		if (output_mask && instruction->output_is_register && instruction->output_address < NUMBER_OF_OUTPUT_ADDRESSES &&
			(output_from_ilu ? ilu : mac) != 0)
		{
			output_destination = instruction->output_address == _output_position ?
				machine.r[12][0] : machine.o[instruction->output_address][0];
		}

/* one vertex's part of the instruction: the MAC operation (BODY) reading a,
b and c through the swizzles, then the ILU's, then the writes */
#define VERTEX_LOOP(BODY) \
		for (vertex = 0; vertex < count; vertex++) \
		{ \
			float ta[4], tb[4], tc[4], mac_result[4], ilu_result[4]; \
			const float *a = ta, *b = tb, *c = tc; \
			if (use_a) a = lean_get(&machine, A, negate_a, vertex, ta); \
			if (use_b) b = lean_get(&machine, B, negate_b, vertex, tb); \
			if (use_c) c = lean_get(&machine, C, negate_c, vertex, tc); \
			BODY \
			if (ilu != _ilu_nop) \
			{ \
				float cv[4]; \
				cv[0] = c[C.x]; cv[1] = c[C.y]; cv[2] = c[C.z]; cv[3] = c[C.w]; \
				ilu_compute(ilu, cv, ilu_result); \
			} \
			if (captures) \
			{ \
				memcpy(outputs[vertex].clip, machine.r[12][vertex], sizeof(outputs[vertex].clip)); \
				outputs[vertex].clip_captured = TRUE; \
			} \
			if (mac == _mac_arl) \
				machine.a0[vertex] = (long)floorf(mac_result[0] + 0.001f); \
			if (mac_destination) \
				write_masked(mac_destination + vertex * 4, mac_result, mac_mask); \
			if (ilu_destination) \
				write_masked(ilu_destination + vertex * 4, ilu_result, ilu_mask); \
			if (output_destination) \
				write_masked(output_destination + vertex * 4, output_from_ilu ? ilu_result : mac_result, output_mask); \
		}

#define AX a[A.x]
#define AY a[A.y]
#define AZ a[A.z]
#define AW a[A.w]
#define BX b[B.x]
#define BY b[B.y]
#define BZ b[B.z]
#define BW b[B.w]
#define CX c[C.x]
#define CY c[C.y]
#define CZ c[C.z]
#define CW c[C.w]

		switch (mac)
		{
		case _mac_mov:
		case _mac_arl:
			VERTEX_LOOP(mac_result[0] = AX; mac_result[1] = AY; mac_result[2] = AZ; mac_result[3] = AW;)
			break;
		case _mac_mul:
			VERTEX_LOOP(
				if (lanes & 1) mac_result[0] = FMUL(AX, BX);
				if (lanes & 2) mac_result[1] = FMUL(AY, BY);
				if (lanes & 4) mac_result[2] = FMUL(AZ, BZ);
				if (lanes & 8) mac_result[3] = FMUL(AW, BW);)
			break;
		case _mac_add:
			VERTEX_LOOP(
				if (lanes & 1) mac_result[0] = FADD(AX, CX);
				if (lanes & 2) mac_result[1] = FADD(AY, CY);
				if (lanes & 4) mac_result[2] = FADD(AZ, CZ);
				if (lanes & 8) mac_result[3] = FADD(AW, CW);)
			break;
		case _mac_mad:
			VERTEX_LOOP(
				if (lanes & 1) mac_result[0] = FADD(FMUL(AX, BX), CX);
				if (lanes & 2) mac_result[1] = FADD(FMUL(AY, BY), CY);
				if (lanes & 4) mac_result[2] = FADD(FMUL(AZ, BZ), CZ);
				if (lanes & 8) mac_result[3] = FADD(FMUL(AW, BW), CW);)
			break;
		case _mac_dp3:
			VERTEX_LOOP(mac_result[0] = mac_result[1] = mac_result[2] = mac_result[3] =
				FADD(FADD(FMUL(AX, BX), FMUL(AY, BY)), FMUL(AZ, BZ));)
			break;
		case _mac_dph:
			VERTEX_LOOP(mac_result[0] = mac_result[1] = mac_result[2] = mac_result[3] =
				FADD(FADD(FADD(FMUL(AX, BX), FMUL(AY, BY)), FMUL(AZ, BZ)), BW);)
			break;
		case _mac_dp4:
			VERTEX_LOOP(mac_result[0] = mac_result[1] = mac_result[2] = mac_result[3] =
				FADD(FADD(FADD(FMUL(AX, BX), FMUL(AY, BY)), FMUL(AZ, BZ)), FMUL(AW, BW));)
			break;
		case _mac_dst:
			VERTEX_LOOP(mac_result[0] = 1.0f; mac_result[1] = FMUL(AY, BY); mac_result[2] = AZ; mac_result[3] = BW;)
			break;
		case _mac_min:
			VERTEX_LOOP(
				mac_result[0] = AX < BX ? AX : BX; mac_result[1] = AY < BY ? AY : BY;
				mac_result[2] = AZ < BZ ? AZ : BZ; mac_result[3] = AW < BW ? AW : BW;)
			break;
		case _mac_max:
			VERTEX_LOOP(
				mac_result[0] = AX > BX ? AX : BX; mac_result[1] = AY > BY ? AY : BY;
				mac_result[2] = AZ > BZ ? AZ : BZ; mac_result[3] = AW > BW ? AW : BW;)
			break;
		case _mac_slt:
			VERTEX_LOOP(
				mac_result[0] = AX < BX; mac_result[1] = AY < BY; mac_result[2] = AZ < BZ; mac_result[3] = AW < BW;)
			break;
		case _mac_sge:
			VERTEX_LOOP(
				mac_result[0] = AX >= BX; mac_result[1] = AY >= BY; mac_result[2] = AZ >= BZ; mac_result[3] = AW >= BW;)
			break;
		default:
			VERTEX_LOOP(;)
			break;
		}
#undef VERTEX_LOOP
	}

	for (vertex = 0; vertex < count; vertex++)
	{
		struct soft_vertex_output *output = &outputs[vertex];

		memcpy(output->position, machine.r[12][vertex], sizeof(output->position));
		memcpy(output->diffuse, machine.o[_output_diffuse][vertex], sizeof(output->diffuse));
		memcpy(output->texture[0], machine.o[_output_texture0][vertex], sizeof(output->texture[0]));
		memcpy(output->texture[1], machine.o[_output_texture0 + 1][vertex], sizeof(output->texture[1]));
		memcpy(output->texture[2], machine.o[_output_texture0 + 2][vertex], sizeof(output->texture[2]));
		memcpy(output->texture[3], machine.o[_output_texture0 + 3][vertex], sizeof(output->texture[3]));
		output->fog = machine.o[_output_fog][vertex][0];
	}
}

/* ---------- running in fixed point

Most of what a vertex program does is multiply-adds on small numbers:
positions of a few metres, matrices, texture coordinates. In 16.16 fixed
point a multiply is one 64-bit integer multiply, where the calculator's
software floating point spends some thirty instructions. A program of
those operations only (program_classify) runs here; any value outside
16.16's +-32768 marks its vertex, which then runs again in floating point. */

typedef long fixed_t;

#define FIXED_ONE 65536L

struct fixed_batch
{
	fixed_t r[13][BATCH][4];
	fixed_t o[NUMBER_OF_OUTPUT_ADDRESSES][BATCH][4];
	fixed_t v[BATCH][XGPU_VERTEX_ATTRIBUTE_COUNT][4];
	long a0[BATCH];
	BOOL overflow[BATCH];
};

/* the constants in fixed point, made again when they change (the caller's
serial says so) */
static fixed_t fixed_constants[XGPU_VERTEX_CONSTANT_COUNT][4];
static BOOL fixed_constants_overflow[XGPU_VERTEX_CONSTANT_COUNT];
/* the same as bits (bit n of word n / 32), for the routines, which do not
check each constant as they read it */
static unsigned long fixed_constants_overflow_bits[(XGPU_VERTEX_CONSTANT_COUNT + 31) / 32];
static unsigned long fixed_constants_serial;
static BOOL fixed_constants_valid;

/* positions too large for 16.16 (the sky's, some 50000 units out): the
fixed-point run takes them over 2^shift, and the translations their
transforms add (the .w of the world-to-clip rows, c-96 to c-93, and of
every constant reached through a0, the nodes' matrices) over the same, so
the clip position comes out over 2^shift as a whole, which projects the
same. The rasterizer checks a vertex against floating point before relying
on it (other uses of those .w would not scale), and puts them back after. */
static fixed_t fixed_scaled_saved[XGPU_VERTEX_CONSTANT_COUNT];
static int fixed_scaled_shift;

static void fixed_constants_scale_one(unsigned long index, int shift)
{
	fixed_scaled_saved[index] = fixed_constants[index][3];
	fixed_constants[index][3] >>= shift;
}

/* the first constant the program reaches through a0 (the nodes' matrices
from there on), or past the end when it reaches none */
static unsigned long fixed_scaled_first_relative;

void soft_vertex_fixed_scale_positions(const void *decoded, int shift)
{
	const struct soft_vp_program *program = decoded;
	unsigned long index, first = XGPU_VERTEX_CONSTANT_COUNT;

	if (fixed_scaled_shift || shift <= 0 || !program)
		return;
	for (index = 0; index < program->count; index++)
	{
		const struct soft_vp_instruction *instruction = &program->instructions[index];
		int operand;

		if (!instruction->relative)
			continue;
		for (operand = 0; operand < 3; operand++)
		{
			if (instruction->operands[operand].mux == _mux_constant && instruction->operands[operand].constant < first)
				first = instruction->operands[operand].constant;
		}
	}
	fixed_scaled_first_relative = first;
	for (index = XGPU_VERTEX_CONSTANT_BIAS - 96; index <= XGPU_VERTEX_CONSTANT_BIAS - 93; index++)
		fixed_constants_scale_one(index, shift);
	for (index = first; index < XGPU_VERTEX_CONSTANT_COUNT; index++)
	{
		if (index < XGPU_VERTEX_CONSTANT_BIAS - 96 || index > XGPU_VERTEX_CONSTANT_BIAS - 93)
			fixed_constants_scale_one(index, shift);
	}
	fixed_scaled_shift = shift;
}

void soft_vertex_fixed_unscale_positions(void)
{
	unsigned long index;

	if (!fixed_scaled_shift)
		return;
	for (index = XGPU_VERTEX_CONSTANT_BIAS - 96; index <= XGPU_VERTEX_CONSTANT_BIAS - 93; index++)
		fixed_constants[index][3] = fixed_scaled_saved[index];
	for (index = fixed_scaled_first_relative; index < XGPU_VERTEX_CONSTANT_COUNT; index++)
		fixed_constants[index][3] = fixed_scaled_saved[index];
	fixed_scaled_shift = 0;
}

/* a float in 16.16, rounded toward zero, from its bits; *overflow set when
it does not fit */
static __inline__ __attribute__((always_inline)) fixed_t float_to_fixed(float value, BOOL *overflow)
{
	float_bits_t bits;
	long exponent, result;
	unsigned long mantissa;

	bits.f = value;
	exponent = (long)((bits.u >> 23) & 0xFF) - 127;
	if (exponent < -17)
		return 0;
	if (exponent >= 15)
	{
		*overflow = TRUE;
		return 0;
	}
	mantissa = (bits.u & 0x7FFFFFUL) | 0x800000UL;
	result = (long)(exponent >= 7 ? mantissa << (exponent - 7) : mantissa >> (7 - exponent));
	return (bits.u & 0x80000000UL) ? -result : result;
}

/* a 16.16 value as a float, from its bits (exact: 31 bits fit a float's 24
only when they are few, so the low ones are cut) */
static __inline__ __attribute__((always_inline)) float fixed_to_float(fixed_t value)
{
	float_bits_t bits;
	unsigned long magnitude, sign = 0;
	int top;

	if (!value)
		return 0.0f;
	if (value < 0)
	{
		sign = 0x80000000UL;
		magnitude = (unsigned long)-value;
	}
	else
	{
		magnitude = (unsigned long)value;
	}
	top = 31 - __builtin_clz(magnitude);
	if (top > 23)
		magnitude >>= top - 23;
	else
		magnitude <<= 23 - top;
	bits.u = sign | ((unsigned long)(top - 16 + 127) << 23) | (magnitude & 0x7FFFFFUL);
	return bits.f;
}

static void fixed_constants_prepare(const float (*constants)[4], unsigned long serial)
{
	unsigned long index;
	int k;

	unsigned long word;
	/* (all of them the first time, or for other constants than the
	device's or a serial of 0; else those written since: soft_device's
	constants_dirty) */
	BOOL all = !fixed_constants_valid || !serial || constants != (const float (*)[4])soft_device.constants;

	if (fixed_constants_valid && serial && serial == fixed_constants_serial)
		return;
	/* (new constants: what scaling there was, undone first) */
	soft_vertex_fixed_unscale_positions();
	for (word = 0; word < (XGPU_VERTEX_CONSTANT_COUNT + 31) / 32; word++)
	{
		unsigned long dirty = all ? ~0UL : soft_device.constants_dirty[word];

		while (dirty)
		{
			int bit = __builtin_ctzl(dirty);
			BOOL overflow = FALSE;

			dirty &= dirty - 1;
			index = word * 32 + bit;
			if (index >= XGPU_VERTEX_CONSTANT_COUNT)
				break;
			for (k = 0; k < 4; k++)
				fixed_constants[index][k] = float_to_fixed(constants[index][k], &overflow);
			fixed_constants_overflow[index] = overflow;
			if (overflow)
				fixed_constants_overflow_bits[word] |= 1UL << bit;
			else
				fixed_constants_overflow_bits[word] &= ~(1UL << bit);
		}
		if (constants == (const float (*)[4])soft_device.constants)
			soft_device.constants_dirty[word] = 0;
	}
	fixed_constants_serial = serial;
	fixed_constants_valid = TRUE;
}

/* a product, or a sum of products, back to 16.16; overflow marks the vertex */
#define FIXED_CHECK(wide, vertex) \
	((wide) > 0x7FFFFFFFLL || (wide) < -0x7FFFFFFFLL ? (machine.overflow[vertex] = TRUE, 0) : (fixed_t)(wide))
#define FIXED_PRODUCT(a, b) ((long long)(a) * (b))

struct fixed_operand
{
	const fixed_t *base;
	unsigned long stride;
	int x, y, z, w;
	BOOL negate, relative;
	unsigned short constant;
};

static struct fixed_operand fixed_prepare(struct fixed_batch *machine, const struct soft_vp_operand *operand,
	int relative, fixed_t shared[4], BOOL *constant_overflow)
{
	struct fixed_operand result;
	static const fixed_t zero[4] = { 0, 0, 0, 0 };

	result.x = operand->swizzle[0];
	result.y = operand->swizzle[1];
	result.z = operand->swizzle[2];
	result.w = operand->swizzle[3];
	result.negate = operand->negate;
	result.relative = FALSE;
	result.constant = operand->constant;
	switch (operand->mux)
	{
	case _mux_temporary:
		result.base = operand->index <= 12 ? machine->r[operand->index][0] : zero;
		result.stride = operand->index <= 12 ? 4 : 0;
		break;
	case _mux_input:
		result.base = machine->v[0][operand->index];
		result.stride = XGPU_VERTEX_ATTRIBUTE_COUNT * 4;
		break;
	case _mux_constant:
		if (relative)
		{
			result.relative = TRUE;
			result.base = fixed_constants[0];
			result.stride = 0;
			break;
		}
		if (fixed_constants_overflow[operand->constant])
			*constant_overflow = TRUE;
		/* the same for every vertex: swizzled and negated once */
		shared[0] = fixed_constants[operand->constant][result.x];
		shared[1] = fixed_constants[operand->constant][result.y];
		shared[2] = fixed_constants[operand->constant][result.z];
		shared[3] = fixed_constants[operand->constant][result.w];
		if (result.negate)
		{
			shared[0] = -shared[0];
			shared[1] = -shared[1];
			shared[2] = -shared[2];
			shared[3] = -shared[3];
		}
		result.base = shared;
		result.stride = 0;
		result.x = 0;
		result.y = 1;
		result.z = 2;
		result.w = 3;
		result.negate = FALSE;
		break;
	default:
		result.base = zero;
		result.stride = 0;
		break;
	}
	return result;
}

static __inline__ __attribute__((always_inline)) const fixed_t *fixed_get(struct fixed_batch *machine,
	struct fixed_operand operand, unsigned long vertex, fixed_t temporary[4])
{
	const fixed_t *values;

	if (operand.relative)
	{
		long index = (long)operand.constant + machine->a0[vertex];

		if (index < 0)
			index = 0;
		if (index >= XGPU_VERTEX_CONSTANT_COUNT)
			index = XGPU_VERTEX_CONSTANT_COUNT - 1;
		if (fixed_constants_overflow[index])
			machine->overflow[vertex] = TRUE;
		values = fixed_constants[index];
	}
	else
	{
		values = operand.base + vertex * operand.stride;
	}
	if (operand.negate)
	{
		temporary[0] = -values[0];
		temporary[1] = -values[1];
		temporary[2] = -values[2];
		temporary[3] = -values[3];
		return temporary;
	}
	return values;
}

static __inline__ __attribute__((always_inline)) void fixed_write(fixed_t *destination, const fixed_t *value,
	unsigned long mask)
{
	if (mask & 8) destination[0] = value[0];
	if (mask & 4) destination[1] = value[1];
	if (mask & 2) destination[2] = value[2];
	if (mask & 1) destination[3] = value[3];
}

/* the batch in fixed point; FALSE for any vertex (overflow[]) that must
run again in floating point */
static void run_batch_fixed(const struct soft_vp_program *program, unsigned long count,
	const float (*inputs)[XGPU_VERTEX_ATTRIBUTE_COUNT][4], struct soft_vertex_output *outputs, BOOL overflow[])
{
	static struct fixed_batch machine;
	unsigned long index, vertex, input;

	memset(machine.r, 0, sizeof(machine.r));
	for (vertex = 0; vertex < count; vertex++)
	{
		static const fixed_t defaults[4] = { 0, 0, 0, FIXED_ONE };

		memcpy(machine.o[_output_diffuse][vertex], defaults, sizeof(defaults));
		memcpy(machine.o[_output_texture0][vertex], defaults, sizeof(defaults));
		memcpy(machine.o[_output_texture0 + 1][vertex], defaults, sizeof(defaults));
		memcpy(machine.o[_output_texture0 + 2][vertex], defaults, sizeof(defaults));
		memcpy(machine.o[_output_texture0 + 3][vertex], defaults, sizeof(defaults));
		machine.a0[vertex] = 0;
		machine.overflow[vertex] = FALSE;
		outputs[vertex].clip_captured = FALSE;
		outputs[vertex].clip_fixed_valid = FALSE;
		outputs[vertex].texture_fixed_valid = FALSE;
		/* only the inputs the program reads */
		for (input = 0; input < XGPU_VERTEX_ATTRIBUTE_COUNT; input++)
		{
			if (program->inputs_read & (1UL << input))
			{
				machine.v[vertex][input][0] = float_to_fixed(inputs[vertex][input][0], &machine.overflow[vertex]);
				machine.v[vertex][input][1] = float_to_fixed(inputs[vertex][input][1], &machine.overflow[vertex]);
				machine.v[vertex][input][2] = float_to_fixed(inputs[vertex][input][2], &machine.overflow[vertex]);
				machine.v[vertex][input][3] = float_to_fixed(inputs[vertex][input][3], &machine.overflow[vertex]);
			}
		}
	}

	for (index = 0; index < program->count; index++)
	{
		const struct soft_vp_instruction *instruction = &program->instructions[index];
		const struct soft_vp_operand *operands = instruction->operands;
		struct fixed_operand A, B, C;
		fixed_t shared[3][4];
		unsigned lanes = instruction->mac_lanes;
		unsigned char mac = lanes ? instruction->mac : _mac_nop;
		BOOL ilu_move = instruction->ilu_lanes && instruction->ilu == _ilu_mov;
		fixed_t *mac_destination = NULL, *ilu_destination = NULL, *output_destination = NULL;
		unsigned long mac_mask = instruction->mac_mask, ilu_mask = instruction->ilu_mask;
		unsigned long output_mask = instruction->output_mask;
		BOOL output_from_ilu = instruction->output_from_ilu;
		BOOL captures = instruction->captures_clip;
		BOOL constant_overflow = FALSE, simple;
		const BOOL use_a = instruction->operand_lanes[0] != 0, use_b = instruction->operand_lanes[1] != 0;
		const BOOL use_c = instruction->operand_lanes[2] != 0;

		A.base = B.base = C.base = NULL;
		A.stride = B.stride = C.stride = 0;
		A.relative = B.relative = C.relative = FALSE;
		A.negate = B.negate = C.negate = FALSE;
		if (use_a) A = fixed_prepare(&machine, &operands[0], instruction->relative, shared[0], &constant_overflow);
		if (use_b) B = fixed_prepare(&machine, &operands[1], instruction->relative, shared[1], &constant_overflow);
		if (use_c) C = fixed_prepare(&machine, &operands[2], instruction->relative, shared[2], &constant_overflow);
		if (constant_overflow)
		{
			for (vertex = 0; vertex < count; vertex++)
				machine.overflow[vertex] = TRUE;
		}
		if (mac != _mac_nop && mac != _mac_arl && mac_mask && instruction->mac_temporary <= 12)
			mac_destination = machine.r[instruction->mac_temporary][0];
		if (ilu_move && ilu_mask && instruction->ilu_temporary <= 12)
			ilu_destination = machine.r[instruction->ilu_temporary][0];
		if (output_mask && instruction->output_is_register && instruction->output_address < NUMBER_OF_OUTPUT_ADDRESSES &&
			(output_from_ilu ? ilu_move : mac != _mac_nop))
		{
			output_destination = instruction->output_address == _output_position ?
				machine.r[12][0] : machine.o[instruction->output_address][0];
		}

#define FIXED_LOOP(BODY) \
		for (vertex = 0; vertex < count; vertex++) \
		{ \
			fixed_t ta[4], tb[4], tc[4], mac_result[4], ilu_result[4]; \
			const fixed_t *a = ta, *b = tb, *c = tc; \
			if (use_a) a = fixed_get(&machine, A, vertex, ta); \
			if (use_b) b = fixed_get(&machine, B, vertex, tb); \
			if (use_c) c = fixed_get(&machine, C, vertex, tc); \
			BODY \
			if (ilu_move) \
			{ \
				ilu_result[0] = c[C.x]; ilu_result[1] = c[C.y]; ilu_result[2] = c[C.z]; ilu_result[3] = c[C.w]; \
			} \
			if (captures) \
			{ \
				outputs[vertex].clip[0] = fixed_to_float(machine.r[12][vertex][0]); \
				outputs[vertex].clip[1] = fixed_to_float(machine.r[12][vertex][1]); \
				outputs[vertex].clip[2] = fixed_to_float(machine.r[12][vertex][2]); \
				outputs[vertex].clip[3] = fixed_to_float(machine.r[12][vertex][3]); \
				outputs[vertex].clip_captured = TRUE; \
				memcpy(outputs[vertex].clip_fixed, machine.r[12][vertex], sizeof(outputs[vertex].clip_fixed)); \
				outputs[vertex].clip_fixed_valid = TRUE; \
			} \
			if (mac == _mac_arl) \
				machine.a0[vertex] = (mac_result[0] + 66) >> 16; \
			if (mac_destination) \
				fixed_write(mac_destination + vertex * 4, mac_result, mac_mask); \
			if (ilu_destination) \
				fixed_write(ilu_destination + vertex * 4, ilu_result, ilu_mask); \
			if (output_destination) \
				fixed_write(output_destination + vertex * 4, output_from_ilu ? ilu_result : mac_result, output_mask); \
		}

/* the same without what most instructions do not need: no relative or
negated operands, no ILU result, no capture, no address register */
#define FAST_LOOP(BODY) \
		{ \
			const fixed_t *pa = A.base, *pb = B.base, *pc = C.base; \
			const unsigned long sa = A.stride, sb = B.stride, sc = C.stride; \
			for (vertex = 0; vertex < count; vertex++, pa += sa, pb += sb, pc += sc) \
			{ \
				fixed_t mac_result[4]; \
				const fixed_t *a = pa, *b = pb, *c = pc; \
				BODY \
				if (mac_destination) \
					fixed_write(mac_destination + vertex * 4, mac_result, mac_mask); \
				if (output_destination) \
					fixed_write(output_destination + vertex * 4, mac_result, output_mask); \
			} \
		}
#define LOOP(BODY) \
		if (simple) \
			FAST_LOOP(BODY) \
		else \
			FIXED_LOOP(BODY)

#define FAX a[A.x]
#define FAY a[A.y]
#define FAZ a[A.z]
#define FAW a[A.w]
#define FBX b[B.x]
#define FBY b[B.y]
#define FBZ b[B.z]
#define FBW b[B.w]
#define FCX c[C.x]
#define FCY c[C.y]
#define FCZ c[C.z]
#define FCW c[C.w]
#define MUL(x, y) FIXED_CHECK(FIXED_PRODUCT(x, y) >> 16, vertex)
#define ADD(x, y) FIXED_CHECK((long long)(x) + (y), vertex)
#define MAD(x, y, z) FIXED_CHECK((FIXED_PRODUCT(x, y) >> 16) + (z), vertex)

		simple = !A.relative && !B.relative && !C.relative && !A.negate && !B.negate && !C.negate && !ilu_move &&
			!captures && mac != _mac_arl && !output_from_ilu;
		switch (mac)
		{
		case _mac_mov:
		case _mac_arl:
			LOOP(mac_result[0] = FAX; mac_result[1] = FAY; mac_result[2] = FAZ; mac_result[3] = FAW;)
			break;
		case _mac_mul:
			LOOP(
				if (lanes & 1) mac_result[0] = MUL(FAX, FBX);
				if (lanes & 2) mac_result[1] = MUL(FAY, FBY);
				if (lanes & 4) mac_result[2] = MUL(FAZ, FBZ);
				if (lanes & 8) mac_result[3] = MUL(FAW, FBW);)
			break;
		case _mac_add:
			LOOP(
				if (lanes & 1) mac_result[0] = ADD(FAX, FCX);
				if (lanes & 2) mac_result[1] = ADD(FAY, FCY);
				if (lanes & 4) mac_result[2] = ADD(FAZ, FCZ);
				if (lanes & 8) mac_result[3] = ADD(FAW, FCW);)
			break;
		case _mac_mad:
			LOOP(
				if (lanes & 1) mac_result[0] = MAD(FAX, FBX, FCX);
				if (lanes & 2) mac_result[1] = MAD(FAY, FBY, FCY);
				if (lanes & 4) mac_result[2] = MAD(FAZ, FBZ, FCZ);
				if (lanes & 8) mac_result[3] = MAD(FAW, FBW, FCW);)
			break;
		case _mac_dp3:
			LOOP(mac_result[0] = mac_result[1] = mac_result[2] = mac_result[3] = FIXED_CHECK(
				(FIXED_PRODUCT(FAX, FBX) + FIXED_PRODUCT(FAY, FBY) + FIXED_PRODUCT(FAZ, FBZ)) >> 16, vertex);)
			break;
		case _mac_dph:
			LOOP(mac_result[0] = mac_result[1] = mac_result[2] = mac_result[3] = FIXED_CHECK(
				((FIXED_PRODUCT(FAX, FBX) + FIXED_PRODUCT(FAY, FBY) + FIXED_PRODUCT(FAZ, FBZ)) >> 16) + FBW, vertex);)
			break;
		case _mac_dp4:
			LOOP(mac_result[0] = mac_result[1] = mac_result[2] = mac_result[3] = FIXED_CHECK(
				(FIXED_PRODUCT(FAX, FBX) + FIXED_PRODUCT(FAY, FBY) + FIXED_PRODUCT(FAZ, FBZ) + FIXED_PRODUCT(FAW, FBW)) >> 16,
				vertex);)
			break;
		case _mac_min:
			LOOP(
				mac_result[0] = FAX < FBX ? FAX : FBX; mac_result[1] = FAY < FBY ? FAY : FBY;
				mac_result[2] = FAZ < FBZ ? FAZ : FBZ; mac_result[3] = FAW < FBW ? FAW : FBW;)
			break;
		case _mac_max:
			LOOP(
				mac_result[0] = FAX > FBX ? FAX : FBX; mac_result[1] = FAY > FBY ? FAY : FBY;
				mac_result[2] = FAZ > FBZ ? FAZ : FBZ; mac_result[3] = FAW > FBW ? FAW : FBW;)
			break;
		case _mac_slt:
			LOOP(
				mac_result[0] = FAX < FBX ? FIXED_ONE : 0; mac_result[1] = FAY < FBY ? FIXED_ONE : 0;
				mac_result[2] = FAZ < FBZ ? FIXED_ONE : 0; mac_result[3] = FAW < FBW ? FIXED_ONE : 0;)
			break;
		case _mac_sge:
			LOOP(
				mac_result[0] = FAX >= FBX ? FIXED_ONE : 0; mac_result[1] = FAY >= FBY ? FIXED_ONE : 0;
				mac_result[2] = FAZ >= FBZ ? FIXED_ONE : 0; mac_result[3] = FAW >= FBW ? FIXED_ONE : 0;)
			break;
		default:
			LOOP(;)
			break;
		}
#undef FIXED_LOOP
#undef FAST_LOOP
#undef LOOP
#undef MUL
#undef ADD
#undef MAD
	}

	for (vertex = 0; vertex < count; vertex++)
	{
		struct soft_vertex_output *output = &outputs[vertex];
		int k, t;

		overflow[vertex] = machine.overflow[vertex];
		if (overflow[vertex])
			continue;
		/* (the position only when no clip position was captured; outputs the
		program does not write keep their defaults) */
		if (!output->clip_captured)
		{
			for (k = 0; k < 4; k++)
				output->position[k] = fixed_to_float(machine.r[12][vertex][k]);
		}
		if (program->outputs_written & (1UL << _output_diffuse))
		{
			for (k = 0; k < 4; k++)
				output->diffuse[k] = fixed_to_float(machine.o[_output_diffuse][vertex][k]);
		}
		else
		{
			output->diffuse[0] = output->diffuse[1] = output->diffuse[2] = 0.0f;
			output->diffuse[3] = 1.0f;
		}
		/* (u and v kept as they are: the rasterizer scales them in integers) */
		for (t = 0; t < 4; t++)
		{
			if (program->outputs_written & (1UL << (_output_texture0 + t)))
			{
				output->texture_fixed[t][0] = machine.o[_output_texture0 + t][vertex][0];
				output->texture_fixed[t][1] = machine.o[_output_texture0 + t][vertex][1];
			}
			else
				output->texture_fixed[t][0] = output->texture_fixed[t][1] = 0;
		}
		output->texture_fixed_valid = TRUE;
		output->fog = 1.0f;
	}
}

/* ---------- fixed-point programs as machine code

A program run_batch_fixed can run is also written once as ARM code, for
one vertex at a time: an operand's swizzle is only which word it loads, a
dot product a chain of 64-bit multiply-accumulates, and a few instructions
after each check that the result fits 16.16 (the routine returns non-zero
if one did not; that vertex runs again in floating point). The numbers are
run_batch_fixed's. */

/* a vertex's registers as the routine sees them (offsets below) */
struct vertex_block
{
	fixed_t r[13][4];
	fixed_t o[NUMBER_OF_OUTPUT_ADDRESSES][4];
	fixed_t v[XGPU_VERTEX_ATTRIBUTE_COUNT][4];
	long a0;
	fixed_t clip[4];
	long captured;
	/* (the stage's results before they are written) */
	fixed_t scratch[8];
};

#define BLOCK_TEMPORARY(reg, lane) (offsetof(struct vertex_block, r) + ((reg) * 4 + (lane)) * 4)
#define BLOCK_OUTPUT(reg, lane) (offsetof(struct vertex_block, o) + ((reg) * 4 + (lane)) * 4)
#define BLOCK_INPUT(reg, lane) (offsetof(struct vertex_block, v) + ((reg) * 4 + (lane)) * 4)
#define BLOCK_A0 offsetof(struct vertex_block, a0)
#define BLOCK_CLIP(lane) (offsetof(struct vertex_block, clip) + (lane) * 4)
#define BLOCK_CAPTURED offsetof(struct vertex_block, captured)
#define BLOCK_SCRATCH(slot) (offsetof(struct vertex_block, scratch) + (slot) * 4)

typedef long (*vertex_routine)(struct vertex_block *block, const fixed_t (*constants)[4]);

#define VJIT_MAXIMUM_WORDS 4096

static struct
{
	unsigned long *code;
	unsigned long count;
	BOOL overflow;
} vjit;

/* registers of the routine: r0 the block, r1 the constants, r11 the
overflow flag; r2-r10 and r12 for values */
#define VJIT_OVERFLOW 11

static void vemit(unsigned long word)
{
	if (vjit.count < VJIT_MAXIMUM_WORDS)
		vjit.code[vjit.count++] = word;
	else
		vjit.overflow = TRUE;
}

static unsigned long vimmediate(unsigned long value)
{
	int rotate;

	if (value < 256)
		return value;
	for (rotate = 1; rotate < 16; rotate++)
	{
		unsigned long rotated = (value << (2 * rotate)) | (value >> (32 - 2 * rotate));

		if (rotated < 256)
			return ((unsigned long)rotate << 8) | rotated;
	}
	vjit.overflow = TRUE;
	return 0;
}

#define COND(c) ((unsigned long)(c) << 28)
enum { _vc_eq = 0, _vc_ne = 1, _vc_vs = 6, _vc_hi = 8, _vc_ge = 10, _vc_lt = 11, _vc_gt = 12, _vc_al = 14 };

static void v_load(int rd, int base, unsigned long offset) { vemit(0xE5900000UL | (base << 16) | (rd << 12) | offset); }
static void v_store(int rd, int base, unsigned long offset) { vemit(0xE5800000UL | (base << 16) | (rd << 12) | offset); }
static void v_mov_imm(int rd, unsigned long value, int c) { vemit(COND(c) | 0x03A00000UL | (rd << 12) | vimmediate(value)); }
static void v_mov(int rd, int rm, int c) { vemit(COND(c) | 0x01A00000UL | (rd << 12) | rm); }
static void v_negate(int rd) { vemit(0xE2600000UL | (rd << 16) | (rd << 12)); }
static void v_cmp(int rn, int rm) { vemit(0xE1500000UL | (rn << 16) | rm); }
static void v_cmp_imm(int rn, unsigned long value) { vemit(0xE3500000UL | (rn << 16) | vimmediate(value)); }
static void v_add_imm(int rd, int rn, unsigned long value) { vemit(0xE2800000UL | (rn << 16) | (rd << 12) | vimmediate(value)); }
static void v_add_shifted(int rd, int rn, int rm, int lsl) { vemit(0xE0800000UL | (rn << 16) | (rd << 12) | (lsl << 7) | rm); }
static void v_smull(int lo, int hi, int rm, int rs) { vemit(0xE0C00090UL | (hi << 16) | (lo << 12) | (rs << 8) | rm); }
static void v_smlal(int lo, int hi, int rm, int rs) { vemit(0xE0E00090UL | (hi << 16) | (lo << 12) | (rs << 8) | rm); }

/* rd = (hi:lo) >> 16, marking overflow unless hi is within +-32768 */
static void v_narrow(int rd, int lo, int hi)
{
	/* t = (hi >> 15) + 1 must be 0 or 1 */
	vemit(0xE1A00000UL | (12 << 12) | (15 << 7) | (2 << 5) | hi);
	v_add_imm(12, 12, 1);
	v_cmp_imm(12, 1);
	vemit(COND(_vc_hi) | 0x03800000UL | (VJIT_OVERFLOW << 16) | (VJIT_OVERFLOW << 12) | 1);
	vemit(0xE1A00000UL | (rd << 12) | (16 << 7) | (1 << 5) | lo);
	vemit(0xE1800000UL | (rd << 16) | (rd << 12) | (16 << 7) | hi);
}

/* rd = rn + rm, marking overflow */
static void v_add_checked(int rd, int rn, int rm)
{
	vemit(0xE0900000UL | (rn << 16) | (rd << 12) | rm);
	vemit(COND(_vc_vs) | 0x03800000UL | (VJIT_OVERFLOW << 16) | (VJIT_OVERFLOW << 12) | 1);
}

/* an operand's lane (the component its swizzle names) into rd */
static void v_operand(int rd, const struct soft_vp_operand *operand, int relative, int lane)
{
	int component = operand->swizzle[lane];

	switch (operand->mux)
	{
	case _mux_temporary:
		if (operand->index <= 12)
			v_load(rd, 0, BLOCK_TEMPORARY(operand->index, component));
		else
			v_mov_imm(rd, 0, _vc_al);
		break;
	case _mux_input:
		v_load(rd, 0, BLOCK_INPUT(operand->index, component));
		break;
	case _mux_constant:
		if (relative)
		{
			/* c[a0 + n], the index held to the table */
			v_load(12, 0, BLOCK_A0);
			v_add_imm(12, 12, operand->constant);
			v_cmp_imm(12, 0);
			v_mov_imm(12, 0, _vc_lt);
			v_cmp_imm(12, XGPU_VERTEX_CONSTANT_COUNT - 1);
			v_mov_imm(12, XGPU_VERTEX_CONSTANT_COUNT - 1, _vc_gt);
			v_add_shifted(12, 1, 12, 4);
			v_load(rd, 12, component * 4);
		}
		else
		{
			v_load(rd, 1, operand->constant * 16 + component * 4);
		}
		break;
	default:
		v_mov_imm(rd, 0, _vc_al);
		break;
	}
	if (operand->negate)
		v_negate(rd);
}

/* a lane of a dot product (lanes of the operands, n of them, plus b.w for dph)
into rd */
static void v_dot(int rd, const struct soft_vp_instruction *instruction, int n, BOOL homogeneous)
{
	int k;

	for (k = 0; k < n; k++)
	{
		v_operand(2, &instruction->operands[0], instruction->relative, k);
		v_operand(3, &instruction->operands[1], instruction->relative, k);
		if (k == 0)
			v_smull(4, 5, 2, 3);
		else
			v_smlal(4, 5, 2, 3);
	}
	v_narrow(rd, 4, 5);
	if (homogeneous)
	{
		v_operand(2, &instruction->operands[1], instruction->relative, 3);
		v_add_checked(rd, rd, 2);
	}
}

/* the program as a routine; FALSE if it did not fit */
static BOOL vjit_compile(const struct soft_vp_program *program)
{
	unsigned long index;

	/* push {r4-r11, lr}; mov r11, #0 */
	vemit(0xE92D4FF0UL);
	v_mov_imm(VJIT_OVERFLOW, 0, _vc_al);
	for (index = 0; index < program->count; index++)
	{
		const struct soft_vp_instruction *instruction = &program->instructions[index];
		unsigned lanes = instruction->mac_lanes;
		unsigned char mac = lanes ? instruction->mac : _mac_nop;
		BOOL ilu_move = instruction->ilu_lanes && instruction->ilu == _ilu_mov;
		BOOL mac_written = mac != _mac_nop && mac != _mac_arl && instruction->mac_mask && instruction->mac_temporary <= 12;
		BOOL ilu_written = ilu_move && instruction->ilu_mask && instruction->ilu_temporary <= 12;
		BOOL output_written = instruction->output_mask && instruction->output_is_register &&
			instruction->output_address < NUMBER_OF_OUTPUT_ADDRESSES &&
			(instruction->output_from_ilu ? ilu_move : mac != _mac_nop);
		int lane;

		/* the MAC's lanes into scratch 0-3, the ILU's into 4-7 */
		switch (mac)
		{
		case _mac_mov:
		case _mac_arl:
			for (lane = 0; lane < 4; lane++)
			{
				if (!(lanes & (1 << lane)) && mac != _mac_arl)
					continue;
				v_operand(6, &instruction->operands[0], instruction->relative, lane);
				v_store(6, 0, BLOCK_SCRATCH(lane));
				if (mac == _mac_arl)
					break;
			}
			break;
		case _mac_mul:
		case _mac_add:
		case _mac_mad:
			for (lane = 0; lane < 4; lane++)
			{
				if (!(lanes & (1 << lane)))
					continue;
				if (mac == _mac_add)
				{
					v_operand(2, &instruction->operands[0], instruction->relative, lane);
					v_operand(3, &instruction->operands[2], instruction->relative, lane);
					v_add_checked(6, 2, 3);
				}
				else
				{
					v_operand(2, &instruction->operands[0], instruction->relative, lane);
					v_operand(3, &instruction->operands[1], instruction->relative, lane);
					v_smull(4, 5, 2, 3);
					v_narrow(6, 4, 5);
					if (mac == _mac_mad)
					{
						v_operand(2, &instruction->operands[2], instruction->relative, lane);
						v_add_checked(6, 6, 2);
					}
				}
				v_store(6, 0, BLOCK_SCRATCH(lane));
			}
			break;
		case _mac_dp3:
		case _mac_dph:
		case _mac_dp4:
			v_dot(6, instruction, mac == _mac_dp4 ? 4 : 3, mac == _mac_dph);
			for (lane = 0; lane < 4; lane++)
				v_store(6, 0, BLOCK_SCRATCH(lane));
			break;
		case _mac_min:
		case _mac_max:
		case _mac_slt:
		case _mac_sge:
			for (lane = 0; lane < 4; lane++)
			{
				v_operand(2, &instruction->operands[0], instruction->relative, lane);
				v_operand(3, &instruction->operands[1], instruction->relative, lane);
				v_cmp(2, 3);
				switch (mac)
				{
				case _mac_min:
					v_mov(6, 3, _vc_al);
					v_mov(6, 2, _vc_lt);
					break;
				case _mac_max:
					v_mov(6, 3, _vc_al);
					v_mov(6, 2, _vc_gt);
					break;
				case _mac_slt:
					v_mov_imm(6, 0, _vc_al);
					v_mov_imm(6, FIXED_ONE, _vc_lt);
					break;
				default:
					v_mov_imm(6, 0, _vc_al);
					v_mov_imm(6, FIXED_ONE, _vc_ge);
					break;
				}
				v_store(6, 0, BLOCK_SCRATCH(lane));
			}
			break;
		case _mac_nop:
			break;
		default:
			return FALSE;
		}
		if (ilu_move)
		{
			for (lane = 0; lane < 4; lane++)
			{
				v_operand(6, &instruction->operands[2], instruction->relative, lane);
				v_store(6, 0, BLOCK_SCRATCH(4 + lane));
			}
		}
		else if (instruction->ilu_lanes && instruction->ilu != _ilu_nop)
		{
			return FALSE;
		}

		/* the capture: r12 as it is before this instruction writes */
		if (instruction->captures_clip)
		{
			for (lane = 0; lane < 4; lane++)
			{
				v_load(6, 0, BLOCK_TEMPORARY(12, lane));
				v_store(6, 0, BLOCK_CLIP(lane));
			}
			v_mov_imm(6, 1, _vc_al);
			v_store(6, 0, BLOCK_CAPTURED);
		}

		/* then the writes, in fixed_write's order */
		if (mac == _mac_arl)
		{
			/* a0 = (x + 66) >> 16 */
			v_load(6, 0, BLOCK_SCRATCH(0));
			v_add_imm(6, 6, 66);
			vemit(0xE1A00000UL | (6 << 12) | (16 << 7) | (2 << 5) | 6);
			v_store(6, 0, BLOCK_A0);
		}
		{
			/* fixed_write: bit 3 of the mask is lane 0 */
			static const unsigned char mask_bits[4] = { 8, 4, 2, 1 };

			for (lane = 0; lane < 4; lane++)
			{
				if (mac_written && (instruction->mac_mask & mask_bits[lane]))
				{
					v_load(6, 0, BLOCK_SCRATCH(lane));
					v_store(6, 0, BLOCK_TEMPORARY(instruction->mac_temporary, lane));
				}
			}
			for (lane = 0; lane < 4; lane++)
			{
				if (ilu_written && (instruction->ilu_mask & mask_bits[lane]))
				{
					v_load(6, 0, BLOCK_SCRATCH(4 + lane));
					v_store(6, 0, BLOCK_TEMPORARY(instruction->ilu_temporary, lane));
				}
			}
			for (lane = 0; lane < 4; lane++)
			{
				if (output_written && (instruction->output_mask & mask_bits[lane]))
				{
					v_load(6, 0, BLOCK_SCRATCH((instruction->output_from_ilu ? 4 : 0) + lane));
					if (instruction->output_address == _output_position)
						v_store(6, 0, BLOCK_TEMPORARY(12, lane));
					else
						v_store(6, 0, BLOCK_OUTPUT(instruction->output_address, lane));
				}
			}
		}
	}
	/* mov r0, r11; pop {r4-r11, pc} */
	v_mov(0, VJIT_OVERFLOW, _vc_al);
	vemit(0xE8BD8FF0UL);
	return !vjit.overflow;
}

/* code written as data, to memory and out of the instruction cache */
static void vjit_sync(const void *code, unsigned long size)
{
	unsigned long line, end = (unsigned long)code + size;

	for (line = (unsigned long)code & ~31UL; line < end; line += 32)
		__asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(line) : "memory");
	__asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
	__asm__ volatile("mcr p15, 0, %0, c7, c5, 0" :: "r"(0) : "memory");
}

/* the program's routine, written the first time; NULL if it cannot be */
static vertex_routine vjit_routine(struct soft_vp_program *program)
{
	static unsigned long buffer[VJIT_MAXIMUM_WORDS];
	unsigned long *code;

	if (program->code_state == 1)
		return (vertex_routine)program->code;
	if (program->code_state == 2)
		return NULL;
	vjit.code = buffer;
	vjit.count = 0;
	vjit.overflow = FALSE;
	program->code_state = 2;
	if (!vjit_compile(program))
		return NULL;
	code = malloc(vjit.count * sizeof(unsigned long));
	if (!code)
		return NULL;
	memcpy(code, buffer, vjit.count * sizeof(unsigned long));
	vjit_sync(code, vjit.count * sizeof(unsigned long));
	program->code = code;
	program->code_state = 1;
	return (vertex_routine)code;
}

/* the batch through the program's routine, a vertex at a time; overflow[]
as run_batch_fixed gives it */
static void run_batch_code(const struct soft_vp_program *program, vertex_routine routine, unsigned long count,
	const float (*inputs)[XGPU_VERTEX_ATTRIBUTE_COUNT][4], const long (*fixed_inputs)[XGPU_VERTEX_ATTRIBUTE_COUNT][4],
	struct soft_vertex_output *outputs, BOOL overflow[])
{
	static struct vertex_block block;
	unsigned long vertex, input;
	int k, t;

	/* the outputs' defaults once for the batch: a program writes the same
	lanes of each vertex, so those it leaves keep them; and the temporaries
	all cleared, then for each vertex those it reads before writing */
	{
		static const fixed_t defaults[4] = { 0, 0, 0, FIXED_ONE };

		memset(block.r, 0, sizeof(block.r));
		memcpy(block.o[_output_diffuse], defaults, sizeof(defaults));
		memcpy(block.o[_output_texture0], defaults, sizeof(defaults));
		memcpy(block.o[_output_texture0 + 1], defaults, sizeof(defaults));
		memcpy(block.o[_output_texture0 + 2], defaults, sizeof(defaults));
		memcpy(block.o[_output_texture0 + 3], defaults, sizeof(defaults));
	}
	for (vertex = 0; vertex < count; vertex++)
	{
		struct soft_vertex_output *output = &outputs[vertex];
		BOOL bad = FALSE;
		unsigned long cleared = program->temporaries_read_first;
		unsigned long read;

		while (cleared)
		{
			int reg = __builtin_ctzl(cleared);

			memset(block.r[reg], 0, sizeof(block.r[reg]));
			cleared &= cleared - 1;
		}
		block.a0 = 0;
		block.captured = 0;
		/* (the inputs read, by their bits) */
		for (read = program->inputs_read; read; read &= read - 1)
		{
			input = (unsigned long)__builtin_ctzl(read);
			if (fixed_inputs)
			{
				/* (already in fixed point: fetched so) */
				const long *from = fixed_inputs[vertex][input];

				block.v[input][0] = from[0];
				block.v[input][1] = from[1];
				block.v[input][2] = from[2];
				block.v[input][3] = from[3];
				continue;
			}
			block.v[input][0] = float_to_fixed(inputs[vertex][input][0], &bad);
			block.v[input][1] = float_to_fixed(inputs[vertex][input][1], &bad);
			block.v[input][2] = float_to_fixed(inputs[vertex][input][2], &bad);
			block.v[input][3] = float_to_fixed(inputs[vertex][input][3], &bad);
		}
		if (!bad && routine(&block, (const fixed_t (*)[4])fixed_constants))
			bad = TRUE;
#ifdef EXPERIMENT_ROUTINE_TWICE
		if (!bad)
			routine(&block, (const fixed_t (*)[4])fixed_constants);
#endif
		overflow[vertex] = bad;
		if (bad)
			continue;
		output->clip_captured = block.captured != 0;
		output->clip_fixed_valid = output->clip_captured;
		if (output->clip_captured)
		{
			for (k = 0; k < 4; k++)
				output->clip[k] = fixed_to_float(block.clip[k]);
			memcpy(output->clip_fixed, block.clip, sizeof(output->clip_fixed));
		}
		else
		{
			for (k = 0; k < 4; k++)
				output->position[k] = fixed_to_float(block.r[12][k]);
		}
		if (program->outputs_written & (1UL << _output_diffuse))
		{
			for (k = 0; k < 4; k++)
				output->diffuse[k] = fixed_to_float(block.o[_output_diffuse][k]);
		}
		else
		{
			output->diffuse[0] = output->diffuse[1] = output->diffuse[2] = 0.0f;
			output->diffuse[3] = 1.0f;
		}
		/* (u and v kept as they are: the rasterizer scales them in integers) */
		for (t = 0; t < 4; t++)
		{
			if (program->outputs_written & (1UL << (_output_texture0 + t)))
			{
				output->texture_fixed[t][0] = block.o[_output_texture0 + t][0];
				output->texture_fixed[t][1] = block.o[_output_texture0 + t][1];
			}
			else
				output->texture_fixed[t][0] = output->texture_fixed[t][1] = 0;
		}
		output->texture_fixed_valid = TRUE;
		output->fog = 1.0f;
	}
}

/* whether the constants a program reads all fit 16.16: those it names, and
for one that indexes by a0 (the skinning matrices) all past the reserved
ones (the viewport's, c[-38] and c[-37], hold its large depth scale) */
static BOOL program_constants_fit(const struct soft_vp_program *program)
{
	unsigned long word;

	for (word = 0; word < (XGPU_VERTEX_CONSTANT_COUNT + 31) / 32; word++)
	{
		unsigned long relative_mask = 0;

		if (program->relative)
		{
			unsigned long first = word * 32, bit;

			for (bit = 0; bit < 32; bit++)
			{
				if (first + bit >= XGPU_VERTEX_CONSTANT_BIAS && first + bit < XGPU_VERTEX_CONSTANT_COUNT)
					relative_mask |= 1UL << bit;
			}
		}
		if (fixed_constants_overflow_bits[word] & (program->constants_read[word] | relative_mask))
			return FALSE;
	}
	return TRUE;
}

/* how vertices ran: in fixed point, in floating point, and again in floating
point after not fitting */
unsigned long soft_vertex_fixed_count, soft_vertex_float_count, soft_vertex_overflow_count;

void soft_vertex_program_run_batch(const void *decoded, unsigned long count,
	const float inputs[][XGPU_VERTEX_ATTRIBUTE_COUNT][4], const float constants[][4], unsigned long constants_serial,
	struct soft_vertex_output *outputs)
{
	const struct soft_vp_program *program = decoded;
	unsigned long done;

	if (program->fixed)
		fixed_constants_prepare(constants, constants_serial);
	for (done = 0; done < count; done += BATCH)
	{
		unsigned long batch = count - done < BATCH ? count - done : BATCH;

		if (program->fixed)
		{
			BOOL overflow[BATCH];
			unsigned long vertex;

			vertex_routine routine = NSPIRE_VERTEX_CODE && program_constants_fit(program) ?
				vjit_routine((struct soft_vp_program *)program) : NULL;

			if (routine)
				run_batch_code(program, routine, batch, inputs + done, NULL, outputs + done, overflow);
			else
				run_batch_fixed(program, batch, inputs + done, outputs + done, overflow);
			soft_vertex_fixed_count += batch;
			/* what did not fit, again in floating point */
			for (vertex = 0; vertex < batch; vertex++)
			{
				if (overflow[vertex])
				{
					run_batch(program, 1, inputs + done + vertex, constants, outputs + done + vertex);
					soft_vertex_overflow_count++;
				}
			}
		}
		else
		{
			run_batch(program, batch, inputs + done, constants, outputs + done);
			soft_vertex_float_count += batch;
		}
	}
}

/* for vertices fetched straight into fixed point: whether the program can
run so (fixed point, its constants fit, its routine written), having made
the constants ready */
int soft_vertex_program_prepare_fixed(const void *decoded, const float constants[][4], unsigned long constants_serial)
{
	struct soft_vp_program *program = (struct soft_vp_program *)decoded;

	if (!NSPIRE_VERTEX_CODE || !program || !program->fixed)
		return 0;
	fixed_constants_prepare(constants, constants_serial);
	return program_constants_fit(program) && vjit_routine(program) != NULL;
}

/* vertices in fixed point (16.16) through the program prepared above;
overflow[] marks those to run again in floating point */
void soft_vertex_program_run_fixed(const void *decoded, unsigned long count,
	const long inputs[][XGPU_VERTEX_ATTRIBUTE_COUNT][4], struct soft_vertex_output *outputs, int overflow[])
{
	struct soft_vp_program *program = (struct soft_vp_program *)decoded;
	vertex_routine routine = vjit_routine(program);
	unsigned long done, vertex;

	for (done = 0; done < count; done += BATCH)
	{
		unsigned long batch = count - done < BATCH ? count - done : BATCH;
		BOOL flags[BATCH];

		run_batch_code(program, routine, batch, NULL, (const long (*)[XGPU_VERTEX_ATTRIBUTE_COUNT][4])(inputs + done),
			outputs + done, flags);
		for (vertex = 0; vertex < batch; vertex++)
			overflow[done + vertex] = flags[vertex];
		soft_vertex_fixed_count += batch;
	}
}

void soft_vertex_program_run(const void *decoded, const float inputs[][4], const float constants[][4],
	struct soft_vertex_output *output)
{
	soft_vertex_program_run_batch(decoded, 1, (const float (*)[XGPU_VERTEX_ATTRIBUTE_COUNT][4])inputs, constants, 0,
		output);
}

#ifdef DEBUG_PROGRAMS
int snprintf(char *, unsigned long, const char *, ...);
void nspire_log(const char *format, ...);

/* a program's instructions to the log, for looking at */
void soft_vertex_program_dump(const void *decoded)
{
	static const char *const macs[] = { "nop", "mov", "mul", "add", "mad", "dp3", "dph", "dp4", "dst", "min", "max",
		"slt", "sge", "arl" };
	static const char *const ilus[] = { "nop", "mov", "rcp", "rcc", "rsq", "exp", "log", "lit" };
	static const char muxes[] = { '?', 'r', 'v', 'c' };
	const struct soft_vp_program *program = decoded;
	unsigned long index;

	for (index = 0; index < program->count; index++)
	{
		const struct soft_vp_instruction *in = &program->instructions[index];
		char operands[3][16];
		int k;

		for (k = 0; k < 3; k++)
		{
			const struct soft_vp_operand *o = &in->operands[k];

			snprintf(operands[k], sizeof(operands[k]), "%s%c%d", o->negate ? "-" : "", muxes[o->mux & 3],
				o->mux == _mux_constant ? (int)o->constant - 96 : o->index);
		}
		nspire_log("    %2lu: %s r%d.%x <- %s %s %s | %s r%d.%x | out%s %d.%x%s", index, macs[in->mac], in->mac_temporary,
			in->mac_mask, operands[0], operands[1], operands[2], ilus[in->ilu], in->ilu_temporary, in->ilu_mask,
			in->output_is_register ? "" : "?", in->output_address, in->output_mask, in->output_from_ilu ? " (ilu)" : "");
	}
}
#endif
