/*
NXDK_MEMORY.C

Xbox contiguous memory (XPhysicalAlloc) and page protection, from the
Xbox's own kernel: the other ports emulate the window at 0x80000000 that
it maps physical memory at (port/linux/src/xbox_memory.c), which is real
here.

The kernel gives contiguous memory only from the low 64 MB, even on a
128 MB console, and ordinary virtual memory takes the low half's free pages
before the upper half's (port/xbox/README.md, the probe). So the caches the
GPU reads are contiguous and allocated first (cache/physical_memory_map.c),
then a pool of contiguous memory for later, and the game state, which is
larger on the native builds than on the Xbox (halo_port_capacity.h), is
virtual memory at a fixed address instead of at physical 0x1A00000
(xbox_game_state_allocate), as the sound cache is virtual memory.
*/

#include <xboxkrnl/xboxkrnl.h>
#include <windows.h>
#include <string.h>

#include "nxdk_platform.h"

#define PAGE_BYTES 0x1000UL
#define CONTIGUOUS_BASE 0x80000000UL
#define CONTIGUOUS_SIZE 0x08000000UL
#define ANY_PHYSICAL_ADDRESS 0xffffffffUL

/* (port/linux/src/platform.h's names for these, which this unit, built
with nxdk's headers instead of the Xbox SDK's, does not include) */
void *platform_contiguous_alloc(unsigned long size, unsigned long alignment,
	unsigned long physical_address, DWORD protect);
void platform_contiguous_free(void *address);
BOOL platform_is_contiguous(const void *address);

BOOL platform_is_contiguous(const void *address)
{
	return (unsigned long)address >= CONTIGUOUS_BASE &&
		(unsigned long)address < CONTIGUOUS_BASE + CONTIGUOUS_SIZE;
}

/* Contiguous memory set aside before the game state is allocated
(xbox_contiguous_reserve): a pool, its pages handed out when the kernel has
no contiguous memory left. The game state and the heap are virtual memory,
which the kernel gives the low 64 MB's free pages first, and only the low
64 MB is had as contiguous (the program and the caches fill most of it):
with nothing set aside, 3.4 MB was left once the game state was allocated,
then less as the heap grew, too little for the menus' larger pictures. Set
aside, the pool's pages are the upper half's instead, and its pages stay
its own: given back to the kernel, the heap would take them. */
#define POOL_BLOCK_BYTES (64UL * 1024)
#define POOL_MAXIMUM_BYTES (12UL * 1024 * 1024)
/* (the pool's blocks lie in the low 64 MB: its span, in pages) */
#define POOL_MAXIMUM_SPAN (64UL * 1024 * 1024 / PAGE_BYTES)

static struct
{
	unsigned long base;
	unsigned long span;
	/* each page: whether free, and at an allocation's first page, its
	length in pages */
	unsigned char free[POOL_MAXIMUM_SPAN];
	unsigned short length[POOL_MAXIMUM_SPAN];
	unsigned long pages, used_pages;
	RTL_CRITICAL_SECTION lock;
} pool;

void xbox_contiguous_reserve(void)
{
	void *blocks[POOL_MAXIMUM_BYTES / POOL_BLOCK_BYTES];
	unsigned long count = 0, index, low = 0xffffffffUL, high = 0;

	RtlInitializeCriticalSection(&pool.lock);
	while (count < POOL_MAXIMUM_BYTES / POOL_BLOCK_BYTES)
	{
		void *block = MmAllocateContiguousMemoryEx(POOL_BLOCK_BYTES, 0, 0xFFFFFFFF, 0, PAGE_READWRITE);

		if (!block)
			break;
		blocks[count++] = block;
		if ((unsigned long)block < low)
			low = (unsigned long)block;
		if ((unsigned long)block + POOL_BLOCK_BYTES > high)
			high = (unsigned long)block + POOL_BLOCK_BYTES;
	}
	if (!count)
		return;
	pool.base = low;
	pool.span = (high - low) / PAGE_BYTES;
	for (index = 0; index < count; index++)
		memset(pool.free + ((unsigned long)blocks[index] - low) / PAGE_BYTES, 1, POOL_BLOCK_BYTES / PAGE_BYTES);
	pool.pages = count * (POOL_BLOCK_BYTES / PAGE_BYTES);
}

static void *pool_alloc(unsigned long pages, unsigned long alignment, DWORD protect)
{
	unsigned long first, run = 0;
	void *result = NULL;

	RtlEnterCriticalSection(&pool.lock);
	/* the first free run long enough, from an aligned page */
	for (first = 0; first + pages <= pool.span; first++)
	{
		if (!pool.free[first] || ((pool.base + first * PAGE_BYTES) & (alignment - 1)))
			continue;
		for (run = 0; run < pages && pool.free[first + run]; run++)
			;
		if (run == pages)
			break;
		first += run;
	}
	if (first + pages <= pool.span && run == pages)
	{
		memset(pool.free + first, 0, pages);
		pool.length[first] = (unsigned short)pages;
		pool.used_pages += pages;
		result = (void *)(pool.base + first * PAGE_BYTES);
	}
	RtlLeaveCriticalSection(&pool.lock);
	if (result && protect != PAGE_READWRITE)
		MmSetAddressProtect(result, pages * PAGE_BYTES, protect);
	return result;
}

/* whether the pool's (and so given back to it) */
static BOOL pool_free(void *address)
{
	unsigned long first = ((unsigned long)address - pool.base) / PAGE_BYTES;

	if (!pool.span || (unsigned long)address < pool.base || first >= pool.span)
		return FALSE;
	RtlEnterCriticalSection(&pool.lock);
	if (pool.length[first])
	{
		MmSetAddressProtect(address, pool.length[first] * PAGE_BYTES, PAGE_READWRITE);
		memset(pool.free + first, 1, pool.length[first]);
		pool.used_pages -= pool.length[first];
		pool.length[first] = 0;
	}
	RtlLeaveCriticalSection(&pool.lock);
	return TRUE;
}

void *platform_contiguous_alloc(unsigned long size, unsigned long alignment,
	unsigned long physical_address, DWORD protect)
{
	void *result;

	size = (size + PAGE_BYTES - 1) & ~(PAGE_BYTES - 1);
	if (alignment < PAGE_BYTES)
		alignment = PAGE_BYTES;
	if (physical_address != ANY_PHYSICAL_ADDRESS)
	{
		/* (a placed block is had exactly where asked or not at all) */
		return MmAllocateContiguousMemoryEx(size, physical_address, physical_address + size - 1, alignment,
			protect);
	}
	result = MmAllocateContiguousMemoryEx(size, 0, 0xFFFFFFFF, alignment, protect);
	if (!result && pool.pages)
	{
		static BOOL logged;

		result = pool_alloc(size / PAGE_BYTES, alignment, protect);
		if (!logged || !result)
		{
			platform_log("memory: contiguous memory from the pool set aside (%lu KB %s, %lu of its %lu KB in use)",
				size / 1024, result ? "had" : "not had", pool.used_pages * (PAGE_BYTES / 1024),
				pool.pages * (PAGE_BYTES / 1024));
			logged = TRUE;
		}
	}
	return result;
}

void platform_contiguous_free(void *address)
{
	if (platform_is_contiguous(address) && !pool_free(address))
		MmFreeContiguousMemory(address);
}

/* what is free in the pool, in KB */
unsigned long xbox_contiguous_pool_free_kb(void)
{
	return (pool.pages - pool.used_pages) * (PAGE_BYTES / 1024);
}

/* the largest contiguous block to be had, in KB (for reports of its running
out: the game's heap takes the low half's free pages too) */
unsigned long xbox_contiguous_largest_kb(void)
{
	unsigned long low = 0, high = 64 * 1024 * 1024 / PAGE_BYTES;

	/* (pages: had at low, not at high) */
	while (high - low > 1)
	{
		unsigned long middle = (low + high) / 2;
		void *block = MmAllocateContiguousMemoryEx(middle * PAGE_BYTES, 0, 0xFFFFFFFF, 0, PAGE_READWRITE);

		if (block)
		{
			MmFreeContiguousMemory(block);
			low = middle;
		}
		else
			high = middle;
	}
	return low * PAGE_BYTES / 1024;
}

/* the memory left, to the log */
void xbox_memory_report(const char *when)
{
	MM_STATISTICS statistics;

	statistics.Length = sizeof(statistics);
	if (NT_SUCCESS(MmQueryStatistics(&statistics)))
		platform_log("memory: %s: %lu KB free, the largest contiguous block %lu KB, %lu KB free in the pool set aside", when,
			(unsigned long)statistics.AvailablePages * (PAGE_BYTES / 1024), xbox_contiguous_largest_kb(),
			(pool.pages - pool.used_pages) * (PAGE_BYTES / 1024));
}

/* ---------- the game state, and other virtual memory */

/* memory only the processor reads, anywhere (the sound cache: the sounds
are mixed in software); XPhysicalFree frees it */
void *xbox_virtual_allocate(unsigned long size)
{
	PVOID base = NULL;
	SIZE_T region_size = size;
	NTSTATUS status = NtAllocateVirtualMemory(&base, 0, &region_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

	if (!NT_SUCCESS(status))
	{
		platform_log("cannot have %lu KB of virtual memory (%08lx)", size / 1024, (unsigned long)status);
		return NULL;
	}
	return base;
}

void *xbox_game_state_allocate(unsigned long address, unsigned long size)
{
	PVOID base = (PVOID)address;
	SIZE_T region_size = size;
	NTSTATUS status = NtAllocateVirtualMemory(&base, 0, &region_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

	if (!NT_SUCCESS(status))
	{
		platform_log("the game state: cannot have %lu KB of virtual memory at %08lx (%08lx)",
			size / 1024, address, (unsigned long)status);
		return NULL;
	}
	return base;
}

/* ---------- Halo Custom Edition's tag cache

A Custom Edition map's tags are linked to run at 0x40440000, 23 MB of them
at most (port/linux/game/cache_file_formats.h), where the Xbox maps' run at
0x803A6000, 22 MB. Only one map is loaded at a time, and there are not 23 MB
to spare, so the window is the Xbox tag cache's own pages mapped a second
time, and 1 MB more for its last megabyte. Every program runs in the
processor's most privileged mode here, so the window is made by writing the
page tables (the kernel's, mapped at 0xC0000000 as Windows NT's are), in an
address range reserved from the kernel, which never commits, frees or looks
into it: the range covers the page tables' 4 MB steps the window touches, so
no other allocation shares their page tables. */
#define PAGE_TABLES_BASE 0xC0000000UL
#define PAGE_DIRECTORY_BASE 0xC0300000UL
#define PAGE_TABLE_SPAN 0x400000UL
#define PAGE_PRESENT 0x001UL
#define PAGE_WRITABLE 0x002UL
#define PAGE_ACCESSED 0x020UL
#define PAGE_DIRTY 0x040UL
#define PAGE_LARGE 0x080UL
#define PAGE_FRAME 0xFFFFF000UL
/* (port/linux/game/cache_file_formats.h, port/linux/game/custom_edition_cache.h) */
#define CUSTOM_EDITION_TAG_CACHE_ADDRESS 0x40440000UL
#define CUSTOM_EDITION_TAG_CACHE_BYTES 0x01700000UL
#define CUSTOM_EDITION_MAP_DIRECTORY "D:\\custom_maps"

/* (port/linux/src/port_config.c) */
int config_boolean(const char *name);

static void *custom_edition_tag_cache = NULL;
/* the bytes of it the GPU reads at the Xbox tag cache's address on (all of
it, once made) */
static unsigned long custom_edition_tag_cache_gpu_bytes = 0;

static volatile unsigned long *page_directory_entry(unsigned long address)
{
	return (volatile unsigned long *)(PAGE_DIRECTORY_BASE + (address / PAGE_TABLE_SPAN) * 4);
}

static volatile unsigned long *page_table_entry(unsigned long address)
{
	return (volatile unsigned long *)(PAGE_TABLES_BASE + (address / PAGE_BYTES) * 4);
}

/* whether the page tables are where they are looked for: a page of the
program's own, which the kernel mapped, is found through them at its
physical address */
static BOOL page_tables_found(void)
{
	static unsigned long probe = 1;
	unsigned long address = (unsigned long)&probe;
	unsigned long directory = *page_directory_entry(address);
	unsigned long physical = (unsigned long)MmGetPhysicalAddress(&probe);

	if (!(directory & PAGE_PRESENT))
		return FALSE;
	if (directory & PAGE_LARGE)
		return (directory & ~(PAGE_TABLE_SPAN - 1)) == (physical & ~(PAGE_TABLE_SPAN - 1));
	return (*page_table_entry(address) & PAGE_FRAME) == (physical & PAGE_FRAME);
}

void xbox_custom_edition_tag_cache_map(void *tag_cache, unsigned long tag_cache_bytes)
{
	unsigned long first = CUSTOM_EDITION_TAG_CACHE_ADDRESS & ~(PAGE_TABLE_SPAN - 1);
	unsigned long end = (CUSTOM_EDITION_TAG_CACHE_ADDRESS + CUSTOM_EDITION_TAG_CACHE_BYTES + PAGE_TABLE_SPAN - 1) &
		~(PAGE_TABLE_SPAN - 1);
	unsigned long extra_bytes = CUSTOM_EDITION_TAG_CACHE_BYTES - tag_cache_bytes;
	unsigned long page_tables = (end - first) / PAGE_TABLE_SPAN;
	unsigned char *tables, *extra;
	unsigned long address, index;
	PVOID base = (PVOID)first;
	SIZE_T region_size = end - first;
	DWORD attributes;

	if (!config_boolean("game.custom_edition"))
		return;
	/* (the 1 MB more is had only when there are Custom Edition maps) */
	attributes = GetFileAttributesA(CUSTOM_EDITION_MAP_DIRECTORY);
	if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
		return;
	if (tag_cache_bytes > CUSTOM_EDITION_TAG_CACHE_BYTES || !page_tables_found())
	{
		platform_log("custom edition: the page tables are not where they were looked for: Custom Edition maps cannot run");
		return;
	}
	if (!NT_SUCCESS(NtAllocateVirtualMemory(&base, 0, &region_size, MEM_RESERVE, PAGE_READWRITE)) ||
		(unsigned long)base != first)
	{
		platform_log("custom edition: cannot reserve %08lx-%08lx: Custom Edition maps cannot run", first, end);
		return;
	}
	/* (the last megabyte where the tag cache's pages end, so that the whole
	window is one run of physical memory, which the GPU reads at the tag
	cache's address on: structure BSPs load to the top, and their vertices
	are compressed where they lie, custom_edition_geometry.c) */
	{
		unsigned long tag_cache_end = (unsigned long)MmGetPhysicalAddress((unsigned char *)tag_cache + tag_cache_bytes - 1) + 1;

		extra = extra_bytes ?
			MmAllocateContiguousMemoryEx(extra_bytes, tag_cache_end, tag_cache_end + extra_bytes - 1, PAGE_BYTES, PAGE_READWRITE) :
			NULL;
		custom_edition_tag_cache_gpu_bytes = extra ? CUSTOM_EDITION_TAG_CACHE_BYTES : tag_cache_bytes;
	}
	tables = MmAllocateContiguousMemoryEx(page_tables * PAGE_BYTES, 0, 0xFFFFFFFF, PAGE_BYTES, PAGE_READWRITE);
	if (!tables || (extra_bytes && !extra))
	{
		platform_log("custom edition: no memory for the tag cache's last %lu KB after its pages: Custom Edition maps cannot run",
			extra_bytes / 1024);
		if (tables)
			MmFreeContiguousMemory(tables);
		if (extra)
			MmFreeContiguousMemory(extra);
		custom_edition_tag_cache_gpu_bytes = 0;
		return;
	}
	memset(tables, 0, page_tables * PAGE_BYTES);
	if (extra)
		memset(extra, 0, extra_bytes);
	for (index = 0; index < page_tables; index++)
	{
		*page_directory_entry(first + index * PAGE_TABLE_SPAN) =
			((unsigned long)MmGetPhysicalAddress(tables + index * PAGE_BYTES) & PAGE_FRAME) |
			PAGE_PRESENT | PAGE_WRITABLE | PAGE_ACCESSED;
	}
	__asm__ __volatile__("movl %%cr3, %%eax\n\tmovl %%eax, %%cr3" ::: "eax", "memory");
	for (address = CUSTOM_EDITION_TAG_CACHE_ADDRESS;
		address < CUSTOM_EDITION_TAG_CACHE_ADDRESS + CUSTOM_EDITION_TAG_CACHE_BYTES;
		address += PAGE_BYTES)
	{
		unsigned long offset = address - CUSTOM_EDITION_TAG_CACHE_ADDRESS;
		void *page = offset < tag_cache_bytes ? (unsigned char *)tag_cache + offset : extra + (offset - tag_cache_bytes);

		*page_table_entry(address) = ((unsigned long)MmGetPhysicalAddress(page) & PAGE_FRAME) |
			PAGE_PRESENT | PAGE_WRITABLE | PAGE_ACCESSED | PAGE_DIRTY;
	}
	__asm__ __volatile__("movl %%cr3, %%eax\n\tmovl %%eax, %%cr3" ::: "eax", "memory");
	/* (what is written to one is read from the other) */
	{
		volatile unsigned long *window = (volatile unsigned long *)CUSTOM_EDITION_TAG_CACHE_ADDRESS;
		volatile unsigned long *own = (volatile unsigned long *)tag_cache;
		unsigned long kept = *own;
		BOOL same;

		*window = 0x43454D50UL;
		same = *own == 0x43454D50UL;
		*own = kept;
		if (!same)
		{
			platform_log("custom edition: the tag cache window does not show the tag cache: Custom Edition maps cannot run");
			return;
		}
	}
	custom_edition_tag_cache = (void *)CUSTOM_EDITION_TAG_CACHE_ADDRESS;
	platform_log("custom edition: the tag cache at %08lx is the Xbox tag cache's pages and the %lu KB after them",
		CUSTOM_EDITION_TAG_CACHE_ADDRESS, extra_bytes / 1024);
}

unsigned long xbox_custom_edition_tag_cache_gpu_bytes(void)
{
	return custom_edition_tag_cache_gpu_bytes;
}

void *halo_custom_edition_tag_cache(void)
{
	return custom_edition_tag_cache;
}

/* ---------- XAPI */

LPVOID WINAPI XPhysicalAlloc(SIZE_T size, ULONG_PTR physical_address, ULONG_PTR alignment, DWORD protect)
{
	/* as port/linux/src/xbox_memory.c: a highest-acceptable address inside
	physical memory places the block there (the tag cache) */
	void *result = platform_contiguous_alloc(size, alignment,
		physical_address < CONTIGUOUS_SIZE ? physical_address : ANY_PHYSICAL_ADDRESS, protect);

	if (!result)
	{
		platform_log("XPhysicalAlloc: cannot allocate %lu KB (physical address 0x%08lx)",
			(unsigned long)size / 1024, (unsigned long)physical_address);
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
	}
	return result;
}

VOID WINAPI XPhysicalFree(LPVOID address)
{
	if (platform_is_contiguous(address))
	{
		platform_contiguous_free(address);
	}
	else if (address)
	{
		/* the game state and the sound cache (xbox_game_state_allocate, xbox_virtual_allocate) */
		PVOID base = address;
		SIZE_T region_size = 0;

		NtFreeVirtualMemory(&base, &region_size, MEM_RELEASE);
	}
}

/* (VirtualProtect under the name the game and the platform layer call it
by: halo_xbox_prefix.h) */
BOOL WINAPI halo_xbox_VirtualProtect(LPVOID address, SIZE_T size, DWORD new_protect, PDWORD old_protect)
{
	if (old_protect)
		*old_protect = MmQueryAddressProtect(address);
	if (platform_is_contiguous(address))
	{
		MmSetAddressProtect(address, size, new_protect);
	}
	else
	{
		PVOID base = address;
		SIZE_T region_size = size;
		ULONG previous;

		if (!NT_SUCCESS(NtProtectVirtualMemory(&base, &region_size, new_protect, &previous)))
		{
			SetLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
	}
	return TRUE;
}

VOID WINAPI XPhysicalProtect(LPVOID address, SIZE_T size, DWORD new_protect)
{
	halo_xbox_VirtualProtect(address, size, new_protect, NULL);
}

DWORD WINAPI XQueryMemoryProtect(LPVOID address)
{
	return MmQueryAddressProtect(address);
}
