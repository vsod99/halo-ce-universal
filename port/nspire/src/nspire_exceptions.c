/*
NSPIRE_EXCEPTIONS.C

What the Nspire port does with a data abort, prefetch abort or undefined
instruction (the handlers are in nspire_abort.S):

- a translation fault in the tag cache pages the block in (nspire_paging.c):
  from the pool at once, or, when it must be read from the map file, by
  returning into nspire_abort.S's trampoline, which reads it outside the
  exception (nspire_paging_service) and runs the instruction again;
- an alignment fault is done byte by byte. The game was written for x86,
  which reads and writes words at any address, and does so all over
  (debug_malloc puts a word after every block, tag data has unaligned
  fields); the calculator's OS has the ARM926 fault on those. Like Linux on
  ARMv5, the handler decodes the load or store (LDR, STR, the halfword and
  doubleword forms, LDM and STM) and carries it out a byte at a time;
- anything else in the program's code is a crash: its registers go to
  nspire_crash, and nspire_crash_report (nspire_main.c) writes it to the log
  and exits cleanly; in the OS's code it goes on to the OS's handler.

This runs in an exception mode with interrupts off: no OS calls.
*/

#include "nspire.h"
#include "nspire_mmu.h"

enum
{
	_exception_not_ours = 0,
	_exception_retry,
	_exception_skip,
	_exception_crash,
	/* nspire_pagein_address must be read in: the trampoline does it */
	_exception_page_in,
};

/* memory_ready's answers */
enum
{
	_memory_bad = 0,
	_memory_ready,
	_memory_needs_read,
};

enum
{
	_kind_data_abort = 1,
	_kind_prefetch_abort,
	_kind_undefined_instruction,
};

#define MODE_MASK 0x1FUL
#define MODE_SUPERVISOR 0x13UL
#define THUMB_BIT 0x20UL
#define CARRY_BIT (1UL << 29)

/* nspire_abort.S */
unsigned long nspire_banked_get(unsigned long mode, int lr);
void nspire_banked_set(unsigned long mode, int lr, unsigned long value);
unsigned long nspire_spsr(void);
extern unsigned long nspire_abort_chain, nspire_prefetch_chain, nspire_undefined_chain;

/* r0-r12, sp, lr, pc, cpsr, fault address, fault status, kind */
unsigned long nspire_crash[20];

/* the page the trampoline reads in, and the faulting code's pc and cpsr
it goes back to (nspire_abort.S) */
unsigned long nspire_pagein_address;
unsigned long nspire_pagein_pc;
unsigned long nspire_pagein_psr;

/* the program, which alone may be interrupted to read the file */
extern char _start[];
extern char _end[];

static struct nspire_alignment_statistics alignment;

/* ---------- registers of the interrupted code */

struct context
{
	unsigned long *frame;
	unsigned long spsr;
	unsigned long pc;
};

static unsigned long register_get(const struct context *context, unsigned long index)
{
	if (index < 13)
		return context->frame[index];
	if (index == 13)
		return nspire_banked_get(context->spsr, 0);
	if (index == 14)
		return nspire_banked_get(context->spsr, 1);
	return context->pc + 8;
}

static int register_set(const struct context *context, unsigned long index, unsigned long value)
{
	if (index < 13)
		context->frame[index] = value;
	else if (index == 13)
		nspire_banked_set(context->spsr, 0, value);
	else if (index == 14)
		nspire_banked_set(context->spsr, 1, value);
	else
		return 0;
	return 1;
}

/* ---------- memory, a byte at a time */

/* whether every byte of [address, address + size) can be touched without
a fault: mapped, or tag cache the pager maps now; or a page that must be
read first (nspire_pagein_address) */
static int memory_ready(unsigned long address, unsigned long size)
{
	unsigned long page;

	for (page = address & ~(NSPIRE_PAGE_SIZE - 1); page < address + size; page += NSPIRE_PAGE_SIZE)
	{
		if (nspire_paging_is_paged(page))
		{
			int result = nspire_paging_make_resident(page);

			if (result == NSPIRE_PAGING_NEEDS_READ)
			{
				nspire_pagein_address = page;
				return _memory_needs_read;
			}
			if (result != NSPIRE_PAGING_DONE)
				return _memory_bad;
		}
		else if (nspire_virtual_to_physical(page) == 0xFFFFFFFFUL)
		{
			return _memory_bad;
		}
	}
	return _memory_ready;
}

static unsigned long read_bytes(unsigned long address, unsigned long size)
{
	const volatile unsigned char *bytes = (const volatile unsigned char *)address;
	unsigned long value = 0, index;

	for (index = 0; index < size; index++)
		value |= (unsigned long)bytes[index] << (index * 8);
	return value;
}

static void write_bytes(unsigned long address, unsigned long size, unsigned long value)
{
	volatile unsigned char *bytes = (volatile unsigned char *)address;
	unsigned long index;

	for (index = 0; index < size; index++)
		bytes[index] = (unsigned char)(value >> (index * 8));
}

/* ---------- alignment faults */

static unsigned long shifted_register(const struct context *context, unsigned long instruction)
{
	unsigned long value = register_get(context, instruction & 0xF);
	unsigned long amount = (instruction >> 7) & 0x1F;

	switch ((instruction >> 5) & 3)
	{
	case 0: return value << amount;
	case 1: return amount ? value >> amount : 0;
	case 2: return amount ? (unsigned long)((long)value >> amount) : ((long)value < 0 ? 0xFFFFFFFFUL : 0);
	default:
		if (!amount)
			return (value >> 1) | ((context->spsr & CARRY_BIT) ? 0x80000000UL : 0);
		return (value >> amount) | (value << (32 - amount));
	}
}

/* The fix_* functions return 1 when the access is done, 0 when it cannot
be, and 2 when a page must be read first (nspire_pagein_address). */

/* LDR and STR of a word (a byte cannot be unaligned) */
static int fix_word_transfer(const struct context *context, unsigned long instruction)
{
	int pre = (instruction >> 24) & 1, up = (instruction >> 23) & 1, byte = (instruction >> 22) & 1;
	int writeback = (instruction >> 21) & 1, load = (instruction >> 20) & 1;
	unsigned long rn = (instruction >> 16) & 0xF, rd = (instruction >> 12) & 0xF;
	unsigned long offset = (instruction & (1UL << 25)) ? shifted_register(context, instruction) : instruction & 0xFFF;
	unsigned long base = register_get(context, rn);
	unsigned long moved = up ? base + offset : base - offset;
	unsigned long address = pre ? moved : base;
	unsigned long value = 0;

	int ready;

	if (byte || (load && rd == 15))
		return 0;
	ready = memory_ready(address, 4);
	if (ready != _memory_ready)
		return ready == _memory_needs_read ? 2 : 0;
	if (load)
		value = read_bytes(address, 4);
	else
		write_bytes(address, 4, rd == 15 ? context->pc + 12 : register_get(context, rd));
	if (!pre || writeback)
		register_set(context, rn, moved);
	if (load)
		register_set(context, rd, value);
	return 1;
}

/* LDRH, STRH, LDRSH, LDRSB, LDRD, STRD */
static int fix_extra_transfer(const struct context *context, unsigned long instruction)
{
	int pre = (instruction >> 24) & 1, up = (instruction >> 23) & 1, immediate = (instruction >> 22) & 1;
	int writeback = (instruction >> 21) & 1, load = (instruction >> 20) & 1;
	unsigned long kind = (instruction >> 5) & 3;
	unsigned long rn = (instruction >> 16) & 0xF, rd = (instruction >> 12) & 0xF;
	unsigned long offset = immediate ? (((instruction >> 4) & 0xF0) | (instruction & 0xF)) :
		register_get(context, instruction & 0xF);
	unsigned long base = register_get(context, rn);
	unsigned long moved = up ? base + offset : base - offset;
	unsigned long address = pre ? moved : base;
	unsigned long first = 0, second = 0;
	int dual = !load && (kind == 2 || kind == 3);

	int ready;

	if (rd == 15)
		return 0;
	ready = memory_ready(address, dual ? 8 : 2);
	if (ready != _memory_ready)
		return ready == _memory_needs_read ? 2 : 0;
	if (dual)
	{
		if (rd & 1)
			return 0;
		if (kind == 2)
		{
			/* LDRD */
			first = read_bytes(address, 4);
			second = read_bytes(address + 4, 4);
		}
		else
		{
			/* STRD */
			write_bytes(address, 4, register_get(context, rd));
			write_bytes(address + 4, 4, register_get(context, rd + 1));
		}
	}
	else if (load)
	{
		first = read_bytes(address, kind == 2 ? 1 : 2);
		if (kind == 2)
			first = (unsigned long)(long)(signed char)first;
		else if (kind == 3)
			first = (unsigned long)(long)(short)first;
	}
	else if (kind == 1)
	{
		write_bytes(address, 2, register_get(context, rd));
	}
	else
	{
		return 0;
	}
	if (!pre || writeback)
		register_set(context, rn, moved);
	if (dual && kind == 2)
	{
		register_set(context, rd, first);
		register_set(context, rd + 1, second);
	}
	else if (load)
	{
		register_set(context, rd, first);
	}
	return 1;
}

/* LDM and STM (not the user-register or pc-loading forms) */
static int fix_multiple_transfer(const struct context *context, unsigned long instruction)
{
	int pre = (instruction >> 24) & 1, up = (instruction >> 23) & 1, user = (instruction >> 22) & 1;
	int writeback = (instruction >> 21) & 1, load = (instruction >> 20) & 1;
	unsigned long rn = (instruction >> 16) & 0xF, list = instruction & 0xFFFF;
	unsigned long count = 0, index, address, base;
	unsigned long values[16];
	int ready;

	if (user || (load && (list & 0x8000)) || !list)
		return 0;
	for (index = 0; index < 16; index++)
		count += (list >> index) & 1;
	base = register_get(context, rn);
	address = up ? base + (pre ? 4 : 0) : base - 4 * count + (pre ? 0 : 4);
	ready = memory_ready(address, 4 * count);
	if (ready != _memory_ready)
		return ready == _memory_needs_read ? 2 : 0;
	for (index = 0; index < 16; index++)
	{
		if (!((list >> index) & 1))
			continue;
		if (load)
			values[index] = read_bytes(address, 4);
		else
			write_bytes(address, 4, index == 15 ? context->pc + 12 : register_get(context, index));
		address += 4;
	}
	if (writeback && !(load && ((list >> rn) & 1)))
		register_set(context, rn, up ? base + 4 * count : base - 4 * count);
	if (load)
	{
		for (index = 0; index < 15; index++)
		{
			if ((list >> index) & 1)
				register_set(context, index, values[index]);
		}
	}
	return 1;
}

static int fix_alignment(const struct context *context)
{
	unsigned long instruction;
	int fixed = 0;

	if ((context->spsr & THUMB_BIT) || memory_ready(context->pc, 4) != _memory_ready)
		return 0;
	instruction = *(const unsigned long *)context->pc;
	if ((instruction & 0x0C000000UL) == 0x04000000UL)
		fixed = fix_word_transfer(context, instruction);
	else if ((instruction & 0x0E000090UL) == 0x00000090UL && (instruction & 0x60UL))
		fixed = fix_extra_transfer(context, instruction);
	else if ((instruction & 0x0E000000UL) == 0x08000000UL)
		fixed = fix_multiple_transfer(context, instruction);
	if (fixed == 1)
	{
		unsigned long index;

		alignment.fixes++;
		for (index = 0; index < alignment.site_count; index++)
		{
			if (alignment.sites[index] == context->pc)
				break;
		}
		if (index == alignment.site_count && alignment.site_count < NSPIRE_ALIGNMENT_SITES)
			alignment.sites[alignment.site_count++] = context->pc;
	}
	return fixed;
}

void nspire_alignment_get_statistics(struct nspire_alignment_statistics *statistics)
{
	*statistics = alignment;
}

/* ---------- the handlers' decision */

static void record_crash(const struct context *context, unsigned long kind, unsigned long address,
	unsigned long status)
{
	unsigned long index;

	for (index = 0; index < 13; index++)
		nspire_crash[index] = context->frame[index];
	nspire_crash[13] = nspire_banked_get(context->spsr, 0);
	nspire_crash[14] = nspire_banked_get(context->spsr, 1);
	nspire_crash[15] = context->pc;
	nspire_crash[16] = context->spsr;
	nspire_crash[17] = kind == _kind_data_abort ? address : 0;
	nspire_crash[18] = kind == _kind_data_abort ? status : 0;
	nspire_crash[19] = kind;
}

int nspire_exception(unsigned long kind, unsigned long *frame, unsigned long address, unsigned long status)
{
	struct context context;
	unsigned long chain;

	context.frame = frame;
	context.spsr = nspire_spsr();
	/* the exception's lr: the data abort's is the instruction + 8, the others'
	the instruction + 4 */
	context.pc = frame[13] - (kind == _kind_data_abort ? 8 : 4);

	if (kind == _kind_data_abort)
	{
		unsigned long fault = status & 0xF;
		int needs_read = 0;

		if (fault == 0x5 || fault == 0x7)
		{
			int result = nspire_paging_fault(address, status, context.pc);

			if (result == NSPIRE_PAGING_DONE)
				return _exception_retry;
			if (result == NSPIRE_PAGING_NEEDS_READ)
			{
				nspire_pagein_address = address;
				needs_read = 1;
			}
		}
		else if (fault == 0x1 || fault == 0x3)
		{
			int result = fix_alignment(&context);

			if (result == 1)
				return _exception_skip;
			needs_read = result == 2;
		}
		/* the file can be read only when the program itself was interrupted,
		in its own mode: not the OS, whose file system may be what faulted */
		if (needs_read && (context.spsr & MODE_MASK) == MODE_SUPERVISOR && !(context.spsr & THUMB_BIT) &&
			context.pc >= (unsigned long)_start && context.pc < (unsigned long)_end)
		{
			nspire_pagein_pc = context.pc;
			nspire_pagein_psr = context.spsr;
			return _exception_page_in;
		}
	}

	chain = kind == _kind_data_abort ? nspire_abort_chain :
		kind == _kind_prefetch_abort ? nspire_prefetch_chain : nspire_undefined_chain;
	if ((context.spsr & MODE_MASK) == MODE_SUPERVISOR || !chain)
	{
		record_crash(&context, kind, address, status);
		return _exception_crash;
	}
	return _exception_not_ours;
}

/* ---------- the trampoline's part (nspire_abort.S), outside the exception */

void nspire_pagein(unsigned long address)
{
	if (!nspire_paging_service(address))
		nspire_fatal("The pager could not read the block at %08lx from the map.", address);
}
