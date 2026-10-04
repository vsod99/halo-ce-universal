/*
NSPIRE_MEMORY.C

Xbox contiguous memory (XPhysicalAlloc) for the Nspire port.

On the Xbox, physical memory at address P is visible at virtual address
0x80000000 + P, and the game puts its game state and tag cache at fixed
addresses there (source/cache/physical_memory_map.c); the cache files are
linked to them. The calculator's OS leaves the megabytes at 0x80000000 unmapped
(port/nspire/probe), so the port maps heap memory there with the ARM926's
MMU: each allocation takes pages of the window, mallocs the memory behind
them in chunks of at most a megabyte (the heap is too fragmented for the
large blocks), and points 4 KB small-page entries of coarse page tables at
the chunks' pages. Everything else about the window follows the Linux
port's xbox_memory.c: placed requests get exactly the address asked for,
the rest go top-down.

The ARM926's caches are virtually indexed, so the data cache is cleaned
before a mapping changes: a heap page is only ever used through the window
while it is mapped.
*/

#include "platform.h"
#include "nspire.h"
#include "nspire_mmu.h"

#include <stdlib.h>
#include <string.h>

#define PAGE_SIZE_BYTES 0x1000UL
#define MEGABYTE 0x100000UL
#define WINDOW_PAGE_COUNT (PLATFORM_CONTIGUOUS_SIZE / PAGE_SIZE_BYTES)
#define WINDOW_MEGABYTE_COUNT (PLATFORM_CONTIGUOUS_SIZE / MEGABYTE)
#define PAGES_PER_MEGABYTE (MEGABYTE / PAGE_SIZE_BYTES)
/* the largest piece of heap one chunk takes */
#define CHUNK_PAGE_COUNT 256UL
#define MAXIMUM_BLOCKS 512

struct window_block
{
	unsigned long first_page;
	unsigned long page_count;
	DWORD protect;
	unsigned long chunk_count;
	/* the heap blocks behind the pages, as malloc returned them */
	void **chunks;
};

static struct window_block blocks[MAXIMUM_BLOCKS];
/* per page of the window: the index of its block plus one, 0 when free, or
PAGED_PAGE for the pager's pages (nspire_paging.c) */
#define PAGED_PAGE 0xFFFF
static unsigned short page_block[WINDOW_PAGE_COUNT];
/* per megabyte of the window: its coarse page table (1 KB aligned) */
static unsigned long *coarse_tables[WINDOW_MEGABYTE_COUNT];
static void *coarse_table_memory[WINDOW_MEGABYTE_COUNT];
static unsigned long saved_level1[WINDOW_MEGABYTE_COUNT];
static unsigned long mapped_page_count;
static int initialized;

/* ---------- the MMU */

int nspire_memory_initialize(void)
{
	volatile unsigned long *level1 = nspire_level1_table();
	unsigned long index;

	if (initialized)
		return 1;
	for (index = 0; index < WINDOW_MEGABYTE_COUNT; index++)
	{
		saved_level1[index] = level1[(PLATFORM_CONTIGUOUS_BASE >> 20) + index];
		if (saved_level1[index] & 3)
		{
			nspire_log("the megabyte at 0x%08lx is already mapped (%08lx)",
				PLATFORM_CONTIGUOUS_BASE + index * MEGABYTE, saved_level1[index]);
			return 0;
		}
	}
	initialized = 1;
	return 1;
}

void nspire_memory_dispose(void)
{
	volatile unsigned long *level1 = nspire_level1_table();
	unsigned long index;

	if (!initialized)
		return;
	nspire_clean_invalidate_data_cache();
	for (index = 0; index < WINDOW_MEGABYTE_COUNT; index++)
		level1[(PLATFORM_CONTIGUOUS_BASE >> 20) + index] = saved_level1[index];
	nspire_clean_invalidate_data_cache();
	nspire_invalidate_tlb();
	initialized = 0;
}

static unsigned long *coarse_table(unsigned long megabyte)
{
	if (!coarse_tables[megabyte])
	{
		/* 1 KB, 1 KB aligned */
		unsigned char *memory = malloc(0x800);
		unsigned long *table;

		if (!memory)
			return NULL;
		table = (unsigned long *)(((unsigned long)memory + 0x3FF) & ~0x3FFUL);
		memset(table, 0, 0x400);
		coarse_table_memory[megabyte] = memory;
		coarse_tables[megabyte] = table;
		nspire_clean_invalidate_data_cache();
		nspire_level1_table()[(PLATFORM_CONTIGUOUS_BASE >> 20) + megabyte] =
			NSPIRE_LEVEL1_COARSE(nspire_virtual_to_physical((unsigned long)table));
	}
	return coarse_tables[megabyte];
}

static int map_page(unsigned long page, unsigned long physical)
{
	unsigned long *table = coarse_table(page / PAGES_PER_MEGABYTE);

	if (!table)
		return 0;
	table[page % PAGES_PER_MEGABYTE] = NSPIRE_LEVEL2_SMALL_PAGE(physical);
	return 1;
}

static void unmap_page(unsigned long page)
{
	unsigned long *table = coarse_tables[page / PAGES_PER_MEGABYTE];

	if (table)
		table[page % PAGES_PER_MEGABYTE] = 0;
}

/* ---------- the window */

BOOL platform_is_contiguous(const void *address)
{
	unsigned long value = (unsigned long)address;

	return value >= PLATFORM_CONTIGUOUS_BASE && value - PLATFORM_CONTIGUOUS_BASE < PLATFORM_CONTIGUOUS_SIZE;
}

static BOOL pages_free(unsigned long first, unsigned long count)
{
	unsigned long page;

	if (first + count > WINDOW_PAGE_COUNT)
		return FALSE;
	for (page = first; page < first + count; page++)
	{
		if (page_block[page])
			return FALSE;
	}
	return TRUE;
}

static void block_release(struct window_block *block)
{
	unsigned long page, chunk;

	nspire_clean_invalidate_data_cache();
	for (page = block->first_page; page < block->first_page + block->page_count; page++)
	{
		unmap_page(page);
		page_block[page] = 0;
	}
	nspire_clean_invalidate_data_cache();
	nspire_invalidate_tlb();
	for (chunk = 0; chunk < block->chunk_count; chunk++)
		free(block->chunks[chunk]);
	free(block->chunks);
	mapped_page_count -= block->page_count;
	memset(block, 0, sizeof(*block));
}

void *platform_contiguous_alloc(unsigned long size, unsigned long alignment,
	unsigned long physical_address, DWORD protect)
{
	unsigned long count = (size + PAGE_SIZE_BYTES - 1) / PAGE_SIZE_BYTES;
	unsigned long alignment_pages = alignment > PAGE_SIZE_BYTES ? alignment / PAGE_SIZE_BYTES : 1;
	unsigned long first = WINDOW_PAGE_COUNT;
	unsigned long block_index, page, mapped;
	struct window_block *block;
	void *address;

	if (!initialized && !nspire_memory_initialize())
		return NULL;
	if (!count)
		count = 1;
	protect &= ~(PAGE_WRITECOMBINE | PAGE_NOCACHE);
	if (!protect)
		protect = PAGE_READWRITE;

	for (block_index = 0; block_index < MAXIMUM_BLOCKS && blocks[block_index].page_count; block_index++)
		;
	if (block_index == MAXIMUM_BLOCKS)
		return NULL;

	if (physical_address != PLATFORM_ANY_PHYSICAL_ADDRESS)
	{
		unsigned long wanted = (physical_address & ~PLATFORM_CONTIGUOUS_BASE) / PAGE_SIZE_BYTES;

		if (pages_free(wanted, count))
			first = wanted;
	}
	else if (count <= WINDOW_PAGE_COUNT)
	{
		/* top-down first fit, like the Xbox contiguous allocator */
		unsigned long candidate = WINDOW_PAGE_COUNT - count;

		for (;;)
		{
			candidate -= candidate % alignment_pages;
			if (pages_free(candidate, count))
			{
				first = candidate;
				break;
			}
			if (candidate == 0)
				break;
			candidate--;
		}
	}
	if (first == WINDOW_PAGE_COUNT)
		return NULL;

	block = &blocks[block_index];
	block->first_page = first;
	block->page_count = count;
	block->protect = protect;
	block->chunks = calloc((count + CHUNK_PAGE_COUNT - 1) / CHUNK_PAGE_COUNT, sizeof(void *));
	if (!block->chunks)
	{
		memset(block, 0, sizeof(*block));
		return NULL;
	}
	for (page = first; page < first + count; page++)
		page_block[page] = (unsigned short)(block_index + 1);
	mapped_page_count += count;

	/* back the pages with heap chunks */
	nspire_clean_invalidate_data_cache();
	for (mapped = 0; mapped < count;)
	{
		unsigned long pages = count - mapped < CHUNK_PAGE_COUNT ? count - mapped : CHUNK_PAGE_COUNT;
		unsigned char *chunk = malloc(pages * PAGE_SIZE_BYTES + PAGE_SIZE_BYTES - 1);
		unsigned long aligned, index;

		if (!chunk)
		{
			nspire_log("XPhysicalAlloc: out of heap after %lu of %lu pages", mapped, count);
			block_release(block);
			return NULL;
		}
		block->chunks[block->chunk_count++] = chunk;
		aligned = ((unsigned long)chunk + PAGE_SIZE_BYTES - 1) & ~(PAGE_SIZE_BYTES - 1);
		for (index = 0; index < pages; index++)
		{
			if (!map_page(first + mapped + index, nspire_virtual_to_physical(aligned + index * PAGE_SIZE_BYTES)))
			{
				block_release(block);
				return NULL;
			}
		}
		mapped += pages;
	}
	nspire_clean_invalidate_data_cache();
	nspire_invalidate_tlb();

	/* fresh pages are zeroed, as the Xbox kernel's are */
	address = (void *)(PLATFORM_CONTIGUOUS_BASE + first * PAGE_SIZE_BYTES);
	memset(address, 0, count * PAGE_SIZE_BYTES);
	return address;
}

void platform_contiguous_free(void *address)
{
	unsigned long page;
	unsigned short block_index;

	if (!platform_is_contiguous(address))
		return;
	page = ((unsigned long)address - PLATFORM_CONTIGUOUS_BASE) / PAGE_SIZE_BYTES;
	block_index = page_block[page];
	if (block_index && block_index != PAGED_PAGE && blocks[block_index - 1].first_page == page)
		block_release(&blocks[block_index - 1]);
}

/* ---------- the pager's pages (nspire_paging.c)

They belong to no block: their table entries are made present and absent
by the pager as it pages them in and out. Their coarse tables are made now,
because the pager works in the data abort handler, which cannot allocate. */

int nspire_memory_reserve_paged(unsigned long address, unsigned long size)
{
	unsigned long first = (address - PLATFORM_CONTIGUOUS_BASE) / PAGE_SIZE_BYTES;
	unsigned long count = (size + PAGE_SIZE_BYTES - 1) / PAGE_SIZE_BYTES;
	unsigned long page;

	if (!initialized && !nspire_memory_initialize())
		return 0;
	if (!platform_is_contiguous((void *)address) || !pages_free(first, count))
		return 0;
	for (page = first; page < first + count; page += PAGES_PER_MEGABYTE - page % PAGES_PER_MEGABYTE)
	{
		if (!coarse_table(page / PAGES_PER_MEGABYTE))
			return 0;
	}
	if (!coarse_table((first + count - 1) / PAGES_PER_MEGABYTE))
		return 0;
	for (page = first; page < first + count; page++)
		page_block[page] = PAGED_PAGE;
	return 1;
}

/* the level 2 entry of a paged page */
volatile unsigned long *nspire_memory_page_entry(unsigned long address)
{
	unsigned long page = (address - PLATFORM_CONTIGUOUS_BASE) / PAGE_SIZE_BYTES;

	return &coarse_tables[page / PAGES_PER_MEGABYTE][page % PAGES_PER_MEGABYTE];
}

/* what malloc can still give, in 64 KB pieces (the OS's allocator says no
more than that) */
unsigned long nspire_memory_heap_free(void)
{
	enum { PIECE = 0x10000, MAXIMUM_PIECES = 1024 };
	static void *pieces[MAXIMUM_PIECES];
	unsigned long count = 0, index;

	while (count < MAXIMUM_PIECES && (pieces[count] = malloc(PIECE)) != NULL)
		count++;
	for (index = 0; index < count; index++)
		free(pieces[index]);
	return count * PIECE;
}

unsigned long nspire_memory_window_size(void)
{
	return mapped_page_count * PAGE_SIZE_BYTES;
}

unsigned long nspire_memory_window_free(void)
{
	return PLATFORM_CONTIGUOUS_SIZE - nspire_memory_window_size();
}

/* ---------- XAPI */

LPVOID WINAPI XPhysicalAlloc(SIZE_T size, ULONG_PTR physical_address, ULONG_PTR alignment, DWORD protect)
{
	void *result = platform_contiguous_alloc(size, alignment,
		physical_address < PLATFORM_CONTIGUOUS_SIZE ? physical_address : PLATFORM_ANY_PHYSICAL_ADDRESS,
		protect);

	if (!result)
	{
		nspire_log("XPhysicalAlloc: cannot allocate %lu bytes (physical address 0x%08lx)",
			(unsigned long)size, (unsigned long)physical_address);
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
	}
	return result;
}

VOID WINAPI XPhysicalFree(LPVOID address)
{
	platform_contiguous_free(address);
}

/* The MMU could enforce these, but nothing in the game relies on a fault;
the protection is only remembered for XQueryMemoryProtect. */
BOOL WINAPI VirtualProtect(LPVOID address, SIZE_T size, DWORD new_protect, PDWORD old_protect)
{
	unsigned long page = ((unsigned long)address - PLATFORM_CONTIGUOUS_BASE) / PAGE_SIZE_BYTES;
	unsigned short block_index = platform_is_contiguous(address) ? page_block[page] : 0;

	(void)size;
	if (block_index == PAGED_PAGE)
		block_index = 0;
	if (old_protect)
		*old_protect = block_index ? blocks[block_index - 1].protect : PAGE_READWRITE;
	if (block_index)
		blocks[block_index - 1].protect = new_protect & ~(PAGE_WRITECOMBINE | PAGE_NOCACHE);
	return TRUE;
}

VOID WINAPI XPhysicalProtect(LPVOID address, SIZE_T size, DWORD new_protect)
{
	VirtualProtect(address, size, new_protect, NULL);
}

DWORD WINAPI XQueryMemoryProtect(LPVOID address)
{
	unsigned long page = ((unsigned long)address - PLATFORM_CONTIGUOUS_BASE) / PAGE_SIZE_BYTES;
	unsigned short block_index = platform_is_contiguous(address) ? page_block[page] : 0;

	if (block_index == PAGED_PAGE)
		return PAGE_READWRITE;
	return block_index ? blocks[block_index - 1].protect : platform_is_contiguous(address) ? PAGE_NOACCESS : PAGE_READWRITE;
}

/* ---------- guest memory write tracking (memory_watch.c on Linux)

The software renderer decodes textures at draw time, so nothing is
watched. */

void memory_watch_initialize(void)
{
}

void memory_watch_protect(unsigned long address, unsigned long size)
{
	(void)address;
	(void)size;
}

unsigned long memory_watch_generation(unsigned long address, unsigned long size)
{
	(void)address;
	(void)size;
	return 0;
}

unsigned long memory_watch_serial(void)
{
	return 0;
}

void memory_watch_prepare_write(void *address, unsigned long size)
{
	(void)address;
	(void)size;
}

void memory_watch_forget(void *address, unsigned long size)
{
	(void)address;
	(void)size;
}
