/*
NSPIRE_PAGING.C

Demand paging of the tag cache for the Nspire port.

Every campaign map needs about 22 MB of tag data and structure BSP at the
tag cache's fixed addresses (0x803A6000 up), more than the calculator has
(port/nspire/README.md). The converted map (tools/nspire_map.py) stores
that data as independently deflated 16 KB blocks, aligned to the addresses
they load at. The loader (cache_files_nspire.c) tells the pager where in the
file each block is instead of reading the data into place. The tag cache's
addresses are reserved but not mapped; the first access to a block takes a
data abort. Reading the file there is out of the question (the OS's file
system cannot be called from an exception), so the exception handler
(nspire_exceptions.c) records the faulting code's state and returns into a
trampoline (nspire_abort.S) in the program's own mode, which calls
nspire_paging_service: that reads the block, inflates it into a slot of a
fixed pool and maps the slot's pages, and the trampoline then runs the
faulting instruction again with every register as it was. A block already
in the pool (its pages unmapped one by one) is mapped again from the
handler itself.

When the pool is full the oldest block goes: dropped if it is unchanged, or
kept for good (pinned) if the game wrote to it, since there is nowhere to
write it back. An access to a reserved address with no block behind it (the
game touches no such address, but a stray write should not crash) gets a
zeroed, pinned slot.

zlib (source/memory/zlib, 1.1.3) allocates while it inflates, so it gets a
static arena here, reset after each block.
*/

#include "platform.h"
#include "nspire.h"
#include "nspire_mmu.h"

#include "zlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_SIZE 0x4000UL
#define BLOCK_SHIFT 14
#define PAGES_PER_BLOCK (BLOCK_SIZE / NSPIRE_PAGE_SIZE)
#define SLOTS_PER_CHUNK 64UL
#define MAXIMUM_CHUNKS 64
#define NO_SLOT 0xFFFF

/* each exception vector is "ldr pc, [pc, #0x18]", which loads the handler
from the slot 0x20 bytes on */
#define VECTOR_INSTRUCTION 0xE59FF018UL
#define VECTOR_SLOT_DISTANCE 0x20UL
#define UNDEFINED_VECTOR_OFFSET 0x04UL
#define PREFETCH_ABORT_VECTOR_OFFSET 0x0CUL
#define DATA_ABORT_VECTOR_OFFSET 0x10UL
#define FIQ_VECTOR_OFFSET 0x1CUL

/* where the program spends its time: the pc the program was at and its lr,
SAMPLE_HZ times a second, the last NSPIRE_SAMPLES of them (a power of two)
kept and written at exit to halo_samples.tns for tools/nspire_samples.py
(0: none kept). The first
timer of the SP804 at 0x900D0000 (Ndless's msleep's; its interrupt is the
VIC's 19) interrupts as a FIQ: Ndless runs a program with interrupts
masked, so the FIQ alone is let through, and the OS's IRQs stay out. */
/* (off unless asked for, 16384 say: its FIQ is the one thing interrupting
the game, and a unit's vectors once came out corrupt with it running, so it
is under suspicion until shown otherwise) */
#ifndef NSPIRE_SAMPLES
#define NSPIRE_SAMPLES 0UL
#endif
#define SAMPLE_HZ 256UL
#define SAMPLE_TIMER ((volatile unsigned long *)0x900D0000)
#define SAMPLE_TIMER_INTERRUPT (1UL << 19)
#define VIC ((volatile unsigned long *)0xDC000000)
#define VIC_SELECT (0x0C / 4)
#define VIC_ENABLE (0x10 / 4)
#define VIC_ENABLE_CLEAR (0x14 / 4)

/* nspire_abort.S's FIQ handler reads it: samples taken, the ring's mask,
the ring (pc + 4, lr pairs), and a word for its r0 */
struct
{
	unsigned long count, mask;
	unsigned long *buffer;
	unsigned long saved_r0;
} nspire_sampling;

/* fault status: translation faults, of a section or a page */
#define FAULT_STATUS_MASK 0xFUL
#define FAULT_SECTION_TRANSLATION 0x5UL
#define FAULT_PAGE_TRANSLATION 0x7UL

struct paged_block
{
	/* where the deflated bytes are in the map file; size 0 for a block with
	no data */
	unsigned long file_offset;
	unsigned long size;
	unsigned short slot;
};

/* the largest a deflated block can be (deflate stores a block it cannot
shrink with a few bytes of header) */
#define MAXIMUM_DEFLATED_SIZE (BLOCK_SIZE + 0x400UL)

struct slot
{
	unsigned char *memory;
	/* the block it holds, or NO_SLOT */
	unsigned long block;
	unsigned long checksum;
	unsigned char pinned;
};

/* nspire_abort.S */
extern void nspire_abort_entry(void);
extern void nspire_fiq_entry(void);
/* where the program was loaded (the samples' addresses are taken from it) */
extern char _start[];
/* the timer and the VIC as they were (sampling_stop puts them back) */
static unsigned long sample_timer_saved[3], vic_select_saved, vic_enable_saved;
extern void nspire_prefetch_entry(void);
extern void nspire_undefined_entry(void);
extern unsigned long nspire_abort_chain;
extern unsigned long nspire_prefetch_chain;
extern unsigned long nspire_undefined_chain;
extern unsigned long nspire_os_abort_sp;
extern unsigned long nspire_os_undefined_sp;
unsigned long nspire_banked_get(unsigned long mode, int lr);

static unsigned long reserved_base, reserved_end;
/* the reservation's first 16 KB block address, and its blocks */
static unsigned long first_block_address;
static unsigned long block_count;
static struct paged_block *blocks;

static struct slot *slots;
static unsigned long slot_count, slots_used, clock_hand;
static void *pool_chunks[MAXIMUM_CHUNKS];
static unsigned long pool_chunk_count;

static unsigned long vector_base;
static int hooked;

static struct nspire_paging_statistics statistics;

/* the map file the blocks are read from (posix_nspire.c descriptors) */
static int source_file = -1;
/* (and the dummy byte inflate_block adds) */
static unsigned char read_buffer[MAXIMUM_DEFLATED_SIZE + 1];

/* ---------- zlib's arena */

#define ZLIB_ARENA_SIZE 0x14000UL

static unsigned char zlib_arena[ZLIB_ARENA_SIZE] __attribute__((aligned(8)));
static unsigned long zlib_arena_used, zlib_arena_mark;
static z_stream inflater;
static int inflater_ready;

static voidpf zlib_allocate(voidpf opaque, uInt items, uInt size)
{
	unsigned long bytes = ((unsigned long)items * size + 7) & ~7UL;
	void *result;

	(void)opaque;
	if (zlib_arena_used + bytes > ZLIB_ARENA_SIZE)
		return Z_NULL;
	result = zlib_arena + zlib_arena_used;
	zlib_arena_used += bytes;
	return result;
}

/* everything allocated after inflateInit goes when a block is done */
static void zlib_free(voidpf opaque, voidpf address)
{
	(void)opaque;
	(void)address;
}

static int inflater_initialize(void)
{
	if (inflater_ready)
		return 1;
	memset(&inflater, 0, sizeof(inflater));
	inflater.zalloc = zlib_allocate;
	inflater.zfree = zlib_free;
	/* raw deflate (no zlib header), with a 32 KB window */
	if (inflateInit2(&inflater, -MAX_WBITS) != Z_OK)
		return 0;
	zlib_arena_mark = zlib_arena_used;
	inflater_ready = 1;
	return 1;
}

static int inflate_block(const unsigned char *data, unsigned long size, unsigned char *output)
{
	int result;

	inflateReset(&inflater);
	zlib_arena_used = zlib_arena_mark;
	/* zlib 1.1.3 needs a byte past the end of a raw deflate stream to see
	that it has ended (gzio.c supplies the same "dummy" byte): without it,
	30 of b30's 1610 blocks come out whole but report Z_BUF_ERROR. The caller
	leaves room for it. */
	((unsigned char *)data)[size] = 0;
	inflater.next_in = (Bytef *)data;
	inflater.avail_in = size + 1;
	inflater.next_out = output;
	inflater.avail_out = BLOCK_SIZE;
	result = inflate(&inflater, Z_FINISH);
	zlib_arena_used = zlib_arena_mark;
	return result == Z_STREAM_END && inflater.avail_out == 0;
}

/* ---------- slots */

static unsigned long checksum(const unsigned char *memory)
{
	const unsigned long *words = (const unsigned long *)memory;
	unsigned long a = 1, b = 0, index;

	for (index = 0; index < BLOCK_SIZE / sizeof(unsigned long); index++)
	{
		a += words[index];
		b += a;
	}
	return a ^ (b << 7) ^ (b >> 25);
}

static int page_is_paged(unsigned long address)
{
	return address >= reserved_base && address < reserved_end;
}

int nspire_paging_is_paged(unsigned long address)
{
	return page_is_paged(address);
}

int nspire_paging_make_resident(unsigned long address)
{
	if (*nspire_memory_page_entry(address & ~(NSPIRE_PAGE_SIZE - 1)))
		return NSPIRE_PAGING_DONE;
	return nspire_paging_fault(address, FAULT_PAGE_TRANSLATION, 0);
}

static void map_slot(unsigned long block_index, struct slot *slot)
{
	unsigned long address = first_block_address + (block_index << BLOCK_SHIFT);
	unsigned long page;

	/* the data was written through the heap's addresses */
	nspire_clean_invalidate_data_range((unsigned long)slot->memory, BLOCK_SIZE);
	for (page = 0; page < PAGES_PER_BLOCK; page++)
	{
		unsigned long page_address = address + page * NSPIRE_PAGE_SIZE;

		if (page_is_paged(page_address))
		{
			volatile unsigned long *entry = nspire_memory_page_entry(page_address);

			*entry = NSPIRE_LEVEL2_SMALL_PAGE(
				nspire_virtual_to_physical((unsigned long)slot->memory + page * NSPIRE_PAGE_SIZE));
			nspire_clean_data_range((unsigned long)entry, sizeof(*entry));
			nspire_invalidate_tlb_entry(page_address);
		}
	}
}

static void unmap_slot(struct slot *slot)
{
	unsigned long address = first_block_address + (slot->block << BLOCK_SHIFT);
	unsigned long page;

	for (page = 0; page < PAGES_PER_BLOCK; page++)
	{
		unsigned long page_address = address + page * NSPIRE_PAGE_SIZE;

		if (page_is_paged(page_address))
		{
			volatile unsigned long *entry = nspire_memory_page_entry(page_address);

			/* the game's writes, through the window's addresses */
			nspire_clean_invalidate_data_range(page_address, NSPIRE_PAGE_SIZE);
			*entry = 0;
			nspire_clean_data_range((unsigned long)entry, sizeof(*entry));
			nspire_invalidate_tlb_entry(page_address);
		}
	}
	blocks[slot->block].slot = NO_SLOT;
	slot->block = NO_SLOT;
}

/* a free slot, taking the oldest unchanged block's; NULL when every slot
holds a pinned block */
static struct slot *slot_take(unsigned long *slot_index)
{
	unsigned long tries;

	if (slots_used < slot_count)
	{
		*slot_index = slots_used;
		return &slots[slots_used++];
	}
	for (tries = 0; tries < slot_count * 2; tries++)
	{
		struct slot *slot = &slots[clock_hand];
		unsigned long index = clock_hand;

		clock_hand = (clock_hand + 1) % slot_count;
		if (slot->pinned)
			continue;
		if (slot->block == NO_SLOT)
		{
			*slot_index = index;
			return slot;
		}
		/* written to: it stays */
		nspire_clean_invalidate_data_range(first_block_address + (slot->block << BLOCK_SHIFT), BLOCK_SIZE);
		if (checksum(slot->memory) != slot->checksum)
		{
			slot->pinned = 1;
			statistics.pinned_blocks++;
			continue;
		}
		unmap_slot(slot);
		statistics.evictions++;
		*slot_index = index;
		return slot;
	}
	return NULL;
}

/* ---------- faults

nspire_paging_fault runs in the data abort handler: it maps a block the
pool holds, or makes a zeroed one for an address with no data, and
otherwise says the block must be read (nspire_paging_service, from the
trampoline). */

static struct paged_block *block_at(unsigned long address, unsigned long *block_index)
{
	*block_index = (address - first_block_address) >> BLOCK_SHIFT;
	return &blocks[*block_index];
}

int nspire_paging_fault(unsigned long address, unsigned long status, unsigned long pc)
{
	unsigned long block_index, slot_index;
	struct paged_block *block;
	struct slot *slot;

	(void)pc;
	if ((status & FAULT_STATUS_MASK) != FAULT_PAGE_TRANSLATION &&
		(status & FAULT_STATUS_MASK) != FAULT_SECTION_TRANSLATION)
		return NSPIRE_PAGING_NOT_OURS;
	if (!page_is_paged(address) || !slots)
		return NSPIRE_PAGING_NOT_OURS;
	block = block_at(address, &block_index);
	statistics.faults++;
	if (block->slot != NO_SLOT)
	{
		/* in the pool, but this page was not mapped: map it again */
		map_slot(block_index, &slots[block->slot]);
		return NSPIRE_PAGING_DONE;
	}
	if (block->size)
		return NSPIRE_PAGING_NEEDS_READ;
	/* no data here: a zeroed block the game may write to */
	slot = slot_take(&slot_index);
	if (!slot)
	{
		statistics.out_of_slots++;
		return NSPIRE_PAGING_NOT_OURS;
	}
	memset(slot->memory, 0, BLOCK_SIZE);
	slot->pinned = 1;
	statistics.pinned_blocks++;
	statistics.anonymous_blocks++;
	slot->block = block_index;
	slot->checksum = checksum(slot->memory);
	block->slot = (unsigned short)slot_index;
	map_slot(block_index, slot);
	return NSPIRE_PAGING_DONE;
}

/* Reads the block at address into the pool and maps it: the trampoline
calls it in the program's own mode, with interrupts as the faulting code
had them. Nonzero on success. */
int nspire_paging_service(unsigned long address)
{
	unsigned long block_index, slot_index;
	struct paged_block *block;
	struct slot *slot;

	if (!page_is_paged(address) || !slots)
		return 0;
	block = block_at(address, &block_index);
	if (block->slot != NO_SLOT)
	{
		map_slot(block_index, &slots[block->slot]);
		return 1;
	}
	if (!block->size || block->size > MAXIMUM_DEFLATED_SIZE || source_file < 0)
		return 0;
	slot = slot_take(&slot_index);
	if (!slot)
	{
		statistics.out_of_slots++;
		return 0;
	}
	if (nspire_file_read_at(source_file, block->file_offset, read_buffer, block->size) != (long)block->size ||
		!inflate_block(read_buffer, block->size, slot->memory))
	{
		statistics.inflate_failures++;
		/* the slot stays free */
		slot->block = NO_SLOT;
		return 0;
	}
	statistics.reads++;
	slot->pinned = 0;
	slot->block = block_index;
	slot->checksum = checksum(slot->memory);
	block->slot = (unsigned short)slot_index;
	map_slot(block_index, slot);
	return 1;
}

/* ---------- set-up */

void *nspire_paging_reserve(unsigned long address, unsigned long size)
{
	unsigned long index;

	if (!nspire_memory_reserve_paged(address, size))
	{
		nspire_log("paging: cannot reserve %lu KB at 0x%08lx", size / 1024, address);
		return NULL;
	}
	reserved_base = address;
	reserved_end = address + size;
	first_block_address = address & ~(BLOCK_SIZE - 1);
	block_count = (reserved_end - first_block_address + BLOCK_SIZE - 1) >> BLOCK_SHIFT;
	blocks = calloc(block_count, sizeof(*blocks));
	if (!blocks || !inflater_initialize())
		nspire_fatal("paging: out of memory for %lu blocks", block_count);
	for (index = 0; index < block_count; index++)
		blocks[index].slot = NO_SLOT;
	if (!hooked)
		nspire_fatal("This OS version's exception vectors are not supported (see halo_log.txt).");
	nspire_log("paging: reserved %lu KB at 0x%08lx (%lu blocks)", size / 1024, address, block_count);
	return (void *)address;
}

/* the vectors as the OS left them: the instruction and its slot */
struct vector_original
{
	unsigned long offset;
	unsigned long instruction;
	unsigned long slot;
};

static struct vector_original vector_originals[4];
static int sampling;

static volatile unsigned long *vector_word(unsigned long offset)
{
	return (volatile unsigned long *)(vector_base + offset);
}

static void vector_write(unsigned long offset, unsigned long value)
{
	*vector_word(offset) = value;
	nspire_clean_data_range((unsigned long)vector_word(offset), sizeof(unsigned long));
}

/* Points the vector at handler, making it "ldr pc, [pc, #0x18]" if it is
not one (OS 5.3 leaves the prefetch abort vector 0). Returns the OS's handler
to go on to, or 0 when there is none. */
static unsigned long vector_hook(struct vector_original *original, unsigned long offset, unsigned long handler)
{
	unsigned long chain;

	original->offset = offset;
	original->instruction = *vector_word(offset);
	original->slot = *vector_word(offset + VECTOR_SLOT_DISTANCE);
	chain = original->instruction == VECTOR_INSTRUCTION ? original->slot : 0;
	vector_write(offset + VECTOR_SLOT_DISTANCE, handler);
	if (original->instruction != VECTOR_INSTRUCTION)
	{
		nspire_log("exceptions: the vector at 0x%02lx was %08lx; made it ldr pc, [pc, #0x18]",
			offset, original->instruction);
		vector_write(offset, VECTOR_INSTRUCTION);
		/* the vector is an instruction: the instruction cache must see it */
		__asm__ volatile("mcr p15, 0, %0, c7, c5, 0" :: "r"(0) : "memory");
	}
	return chain;
}

static void vector_unhook(const struct vector_original *original);

/* the sampling timer started, its interrupt a FIQ, the FIQ let through
(when the OS sends nothing to the FIQ itself) */
static void sampling_start(void)
{
	unsigned long psr;

	if (!NSPIRE_SAMPLES || sampling)
		return;
	if (VIC[VIC_SELECT])
	{
		nspire_log("sampling: the OS uses the FIQ (%08lx); none kept", VIC[VIC_SELECT]);
		return;
	}
	nspire_sampling.buffer = malloc(NSPIRE_SAMPLES * 2 * sizeof(unsigned long));
	if (!nspire_sampling.buffer)
		return;
	nspire_sampling.count = 0;
	nspire_sampling.mask = NSPIRE_SAMPLES - 1;
	nspire_clean_data_range((unsigned long)&nspire_sampling, sizeof(nspire_sampling));
	vector_hook(&vector_originals[3], FIQ_VECTOR_OFFSET, (unsigned long)nspire_fiq_entry);
	sample_timer_saved[0] = SAMPLE_TIMER[0];
	sample_timer_saved[1] = SAMPLE_TIMER[2];
	sample_timer_saved[2] = SAMPLE_TIMER[6];
	vic_select_saved = VIC[VIC_SELECT];
	vic_enable_saved = VIC[VIC_ENABLE];
	/* periodic, interrupting, 32-bit, at 32768 Hz */
	SAMPLE_TIMER[2] = 0;
	SAMPLE_TIMER[3] = 1;
	SAMPLE_TIMER[0] = 32768UL / SAMPLE_HZ;
	SAMPLE_TIMER[6] = 32768UL / SAMPLE_HZ;
	SAMPLE_TIMER[2] = 0xE2;
	VIC[VIC_SELECT] = vic_select_saved | SAMPLE_TIMER_INTERRUPT;
	VIC[VIC_ENABLE] = SAMPLE_TIMER_INTERRUPT;
	__asm__ volatile("mrs %0, cpsr\n bic %0, %0, #0x40\n msr cpsr_c, %0" : "=r"(psr) :: "memory");
	sampling = 1;
	nspire_log("sampling: %lu times a second (the VIC had %08lx enabled)", SAMPLE_HZ, vic_enable_saved);
}

static void sampling_stop(void)
{
	unsigned long psr;

	if (!sampling)
		return;
	__asm__ volatile("mrs %0, cpsr\n orr %0, %0, #0x40\n msr cpsr_c, %0" : "=r"(psr) :: "memory");
	SAMPLE_TIMER[2] = 0;
	SAMPLE_TIMER[3] = 1;
	if (!(vic_enable_saved & SAMPLE_TIMER_INTERRUPT))
		VIC[VIC_ENABLE_CLEAR] = SAMPLE_TIMER_INTERRUPT;
	VIC[VIC_SELECT] = vic_select_saved;
	SAMPLE_TIMER[0] = sample_timer_saved[0];
	SAMPLE_TIMER[6] = sample_timer_saved[2];
	SAMPLE_TIMER[2] = sample_timer_saved[1];
	vector_unhook(&vector_originals[3]);
	sampling = 0;
}

static void vector_unhook(const struct vector_original *original)
{
	vector_write(original->offset + VECTOR_SLOT_DISTANCE, original->slot);
	if (*vector_word(original->offset) != original->instruction)
	{
		vector_write(original->offset, original->instruction);
		__asm__ volatile("mcr p15, 0, %0, c7, c5, 0" :: "r"(0) : "memory");
	}
}

/* The data abort, prefetch abort and undefined instruction handlers
(nspire_abort.S): the pager's, and the crash report for the rest. */
int nspire_paging_hook(void)
{
	unsigned long control;

	if (hooked)
		return 1;
	__asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(control));
	vector_base = (control & (1UL << 13)) ? 0xFFFF0000UL : 0;
	/* the pager needs the data abort vector as the OS has always had it */
	if (*vector_word(DATA_ABORT_VECTOR_OFFSET) != VECTOR_INSTRUCTION)
	{
		nspire_log("exceptions: the data abort vector is %08lx, not ldr pc, [pc, #0x18]",
			*vector_word(DATA_ABORT_VECTOR_OFFSET));
		return 0;
	}
	/* the handlers run on their own stack and put the OS's back */
	nspire_os_abort_sp = nspire_banked_get(0x17, 0);
	nspire_os_undefined_sp = nspire_banked_get(0x1B, 0);
	nspire_abort_chain = vector_hook(&vector_originals[0], DATA_ABORT_VECTOR_OFFSET, (unsigned long)nspire_abort_entry);
	nspire_prefetch_chain = vector_hook(&vector_originals[1], PREFETCH_ABORT_VECTOR_OFFSET,
		(unsigned long)nspire_prefetch_entry);
	nspire_undefined_chain = vector_hook(&vector_originals[2], UNDEFINED_VECTOR_OFFSET,
		(unsigned long)nspire_undefined_entry);
	nspire_clean_data_range((unsigned long)&nspire_abort_chain, sizeof(unsigned long));
	nspire_clean_data_range((unsigned long)&nspire_prefetch_chain, sizeof(unsigned long));
	nspire_clean_data_range((unsigned long)&nspire_undefined_chain, sizeof(unsigned long));
	hooked = 1;
	sampling_start();
	nspire_log("exceptions: handlers installed (the OS's: data %08lx, prefetch %08lx, undefined %08lx)",
		nspire_abort_chain, nspire_prefetch_chain, nspire_undefined_chain);
	return 1;
}

void nspire_paging_unhook(void)
{
	if (!hooked)
		return;
	sampling_stop();
	vector_unhook(&vector_originals[2]);
	vector_unhook(&vector_originals[1]);
	vector_unhook(&vector_originals[0]);
	hooked = 0;
}

/* The pool: as many slots as fit in bytes. It cannot grow once blocks are
paged in, since the handler cannot allocate. */
unsigned long nspire_paging_allocate_pool(unsigned long bytes)
{
	unsigned long wanted = bytes / BLOCK_SIZE;

	if (slots)
		return slot_count * BLOCK_SIZE;
	slots = calloc(wanted ? wanted : 1, sizeof(*slots));
	if (!slots)
		return 0;
	while (slot_count < wanted && pool_chunk_count < MAXIMUM_CHUNKS)
	{
		unsigned long count = wanted - slot_count < SLOTS_PER_CHUNK ? wanted - slot_count : SLOTS_PER_CHUNK;
		unsigned char *chunk = malloc(count * BLOCK_SIZE + NSPIRE_PAGE_SIZE - 1);
		unsigned long aligned, index;

		if (!chunk)
			break;
		pool_chunks[pool_chunk_count++] = chunk;
		aligned = ((unsigned long)chunk + NSPIRE_PAGE_SIZE - 1) & ~(NSPIRE_PAGE_SIZE - 1);
		for (index = 0; index < count; index++)
		{
			slots[slot_count].memory = (unsigned char *)(aligned + index * BLOCK_SIZE);
			slots[slot_count].block = NO_SLOT;
			slot_count++;
		}
	}
	nspire_log("paging: pool of %lu slots (%lu KB)", slot_count, slot_count * BLOCK_SIZE / 1024);
	return slot_count * BLOCK_SIZE;
}

/* The map file the blocks are read from, by path. Another map (the main
menu's ui.map, then the level's) forgets every block of the last one first:
the same addresses hold the new map's tag data. */
static char source_path[300];

int nspire_paging_set_source(const char *path)
{
	if (source_file >= 0 && strcmp(source_path, path))
	{
		nspire_paging_drop(first_block_address, block_count << BLOCK_SHIFT);
		nspire_file_close(source_file);
		source_file = -1;
		nspire_log("paging: %s replaces %s", path, source_path);
	}
	if (source_file < 0)
	{
		source_file = nspire_file_open_read(path);
		strncpy(source_path, path, sizeof(source_path) - 1);
	}
	return source_file >= 0;
}

/* Where the block at address is in the map file (size 0: no data). */
void nspire_paging_set_block(unsigned long address, unsigned long file_offset, unsigned long size)
{
	unsigned long index = (address - first_block_address) >> BLOCK_SHIFT;

	if (address < first_block_address || index >= block_count)
		return;
	nspire_paging_drop(address, BLOCK_SIZE);
	blocks[index].file_offset = file_offset;
	blocks[index].size = size;
}

/* Forgets the blocks overlapping [address, address + size) and pages them
out (a structure BSP being unloaded). */
void nspire_paging_drop(unsigned long address, unsigned long size)
{
	unsigned long first, last, index;

	if (!blocks || !size)
		return;
	if (address < first_block_address)
		address = first_block_address;
	first = (address - first_block_address) >> BLOCK_SHIFT;
	last = (address + size - 1 - first_block_address) >> BLOCK_SHIFT;
	for (index = first; index <= last && index < block_count; index++)
	{
		struct paged_block *block = &blocks[index];

		if (block->slot != NO_SLOT)
		{
			struct slot *slot = &slots[block->slot];

			unmap_slot(slot);
			slot->pinned = 0;
		}
		block->file_offset = 0;
		block->size = 0;
	}
}

void nspire_paging_get_statistics(struct nspire_paging_statistics *result)
{
	*result = statistics;
	result->slots = slot_count;
	result->slots_used = slots_used;
}

void nspire_paging_dispose(void)
{
	unsigned long index;

	nspire_paging_unhook();
	if (nspire_sampling.buffer)
	{
		/* "HSM2", where the program was loaded, the count, then each pc + 4
		and lr, oldest first */
		char path[300];
		FILE *file;
		unsigned long kept = nspire_sampling.count < NSPIRE_SAMPLES ? nspire_sampling.count : NSPIRE_SAMPLES;
		unsigned long oldest = nspire_sampling.count < NSPIRE_SAMPLES ? 0 : nspire_sampling.count & (NSPIRE_SAMPLES - 1);

		snprintf(path, sizeof(path), "%s/halo_samples.tns", nspire_program_directory());
		file = fopen(path, "wb");
		if (file)
		{
			unsigned long header[3];

			header[0] = 0x324D5348UL;
			header[1] = (unsigned long)_start;
			header[2] = kept;
			fwrite(header, sizeof(header), 1, file);
			fwrite(nspire_sampling.buffer + oldest * 2, sizeof(unsigned long) * 2, kept - oldest, file);
			fwrite(nspire_sampling.buffer, sizeof(unsigned long) * 2, oldest, file);
			fclose(file);
		}
		nspire_log("sampling: %lu samples taken, the last %lu written to %s", nspire_sampling.count, kept, path);
		free(nspire_sampling.buffer);
		nspire_sampling.buffer = NULL;
	}
	/* (the descriptor is closed with the others at exit) */
	source_file = -1;
	for (index = 0; index < pool_chunk_count; index++)
		free(pool_chunks[index]);
	pool_chunk_count = 0;
}
