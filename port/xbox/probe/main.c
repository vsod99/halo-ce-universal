/*
MAIN.C

The Xbox port's memory probe (port/xbox/README.md, Phase 0): can an nxdk
program have the physical memory the game asks for at fixed addresses?

The maps are linked to the tag cache at physical 0x3A6000 (0x1600000 bytes,
source/cache/physical_memory_map.c). The texture (0x1600000, write
combined) and sound (0x400000) caches go anywhere contiguous, and the native
builds' 16 MB game state (port/linux/include/halo_port_capacity.h, at
physical 0x1A00000 there) here goes to virtual memory at a fixed address,
since the kernel gives contiguous memory only from the low 64 MB (see
regions[] and port/xbox/README.md). The probe asks for them first thing in
main, reports where nxdk's start-up left its own image, stack and heap,
which of a 128 MB console's upper half can be had contiguous, and what
remains, then sets a video mode with the regions still held.

Run it with python tools/xbox_dev.py run port/xbox/probe.
*/

#include <hal/debug.h>
#include <hal/video.h>
#include <stdlib.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "../common/xbox_log.h"

#define PAGE_BYTES 0x1000UL
#define SCAN_STEP 0x10000UL
#define UPPER_HALF_START 0x04000000UL
#define UPPER_HALF_END 0x07FFFFFFUL

struct region
{
	const char *name;
	unsigned long physical_address; /* contiguous, where it must be; 0: anywhere */
	unsigned long virtual_address; /* not 0: ordinary virtual memory there instead */
	unsigned long size;
	unsigned long protect;
	void *address;
};

/* The kernel gives contiguous memory only from the low 64 MB, even on a
128 MB console (whose upper half backs ordinary virtual memory), so the
layout that fits: the tag cache at its physical address, the texture and
sound caches contiguous anywhere (the GPU and APU read them), and the game
state, which nothing but the CPU reads, as virtual memory at a fixed
address, taking its pages from wherever they are free. */
static struct region regions[] =
{
	{"tag cache", 0x003A6000, 0, 0x1600000, PAGE_READWRITE, NULL},
	{"texture cache", 0, 0, 0x1600000, PAGE_READWRITE | PAGE_WRITECOMBINE, NULL},
	{"sound cache", 0, 0, 0x400000, PAGE_READWRITE, NULL},
	{"game state (virtual)", 0, 0x40000000, 0x1000000, PAGE_READWRITE, NULL},
};

#define REGION_COUNT (sizeof(regions) / sizeof(regions[0]))

static void log_statistics(const char *when)
{
	MM_STATISTICS statistics;

	statistics.Length = sizeof(statistics);
	if (!NT_SUCCESS(MmQueryStatistics(&statistics)))
	{
		xbox_log("%s: MmQueryStatistics failed", when);
		return;
	}
	/* in KB: nxdk's printf formats no floating point */
	xbox_log("%s: %lu KB in all, %lu KB available; image %lu KB, pool %lu KB, stacks %lu KB",
		when,
		statistics.TotalPhysicalPages * (PAGE_BYTES / 1024),
		statistics.AvailablePages * (PAGE_BYTES / 1024),
		statistics.ImagePagesCommitted * (PAGE_BYTES / 1024),
		statistics.PoolPagesCommitted * (PAGE_BYTES / 1024),
		statistics.StackPagesCommitted * (PAGE_BYTES / 1024));
}

static void log_physical(const char *name, void *address)
{
	xbox_log("%-28s virtual %08lX physical %08lX", name,
		(unsigned long)address, (unsigned long)MmGetPhysicalAddress(address));
}

/* what holds a range that could not be had: tries a page at every
SCAN_STEP, and logs the runs of taken steps */
static void scan_range(unsigned long start, unsigned long size)
{
	unsigned long address;
	unsigned long run_start = 0;
	int in_run = 0;

	for (address = start; address < start + size; address += SCAN_STEP)
	{
		void *page = MmAllocateContiguousMemoryEx(PAGE_BYTES, address, address + PAGE_BYTES - 1, 0, PAGE_READWRITE);
		int taken = page == NULL;

		if (page)
			MmFreeContiguousMemory(page);
		if (taken && !in_run)
		{
			run_start = address;
			in_run = 1;
		}
		else if (!taken && in_run)
		{
			xbox_log("    taken: %08lX-%08lX", run_start, address - 1);
			in_run = 0;
		}
	}
	if (in_run)
		xbox_log("    taken: %08lX-%08lX", run_start, start + size - 1);
}

/* what a 128 MB console's upper half gives: which of it is taken, and the
largest blocks it gives, with and without write combining */
static void probe_upper_half(void)
{
	static const unsigned long sizes[] = {0x100000, 0x400000, 0x800000, 0x1000000, 0x1600000};
	unsigned int index;
	int combine;

	xbox_log("upper 64 MB, taken 64 KB steps:");
	scan_range(UPPER_HALF_START, UPPER_HALF_END - UPPER_HALF_START + 1);
	for (combine = 0; combine < 2; combine++)
	{
		for (index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++)
		{
			unsigned long protect = PAGE_READWRITE | (combine ? PAGE_WRITECOMBINE : 0);
			void *block = MmAllocateContiguousMemoryEx(sizes[index], UPPER_HALF_START, UPPER_HALF_END, 0, protect);

			xbox_log("  %5lu KB%s: %s %08lX", sizes[index] / 1024, combine ? " write combined" : "",
				block ? "ok at" : "FAIL", block ? (unsigned long)MmGetPhysicalAddress(block) : 0UL);
			if (block)
				MmFreeContiguousMemory(block);
		}
	}
}

/* touches every page of a virtual block and logs where its pages are */
static void log_virtual_pages(const char *name, void *base, unsigned long size)
{
	unsigned long offset, above = 0, lowest = 0xFFFFFFFF, highest = 0;

	for (offset = 0; offset < size; offset += PAGE_BYTES)
	{
		unsigned long physical;

		((volatile unsigned char *)base)[offset] = 1;
		physical = (unsigned long)MmGetPhysicalAddress((unsigned char *)base + offset);
		if (physical >= UPPER_HALF_START)
			above++;
		if (physical < lowest)
			lowest = physical;
		if (physical > highest)
			highest = physical;
	}
	xbox_log("     %-28s %lu of %lu pages above 64 MB, physical %08lX-%08lX",
		name, above, size / PAGE_BYTES, lowest, highest);
}

/* ordinary virtual memory, at a fixed address when one is given */
static void *allocate_virtual(unsigned long address, unsigned long size)
{
	PVOID base = (PVOID)address;
	SIZE_T region_size = size;
	NTSTATUS status = NtAllocateVirtualMemory(&base, 0, &region_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

	if (!NT_SUCCESS(status))
	{
		xbox_log("     NtAllocateVirtualMemory: %08lX", (unsigned long)status);
		return NULL;
	}
	return base;
}

static int allocate_regions(void)
{
	int all = 1;
	unsigned int index;

	for (index = 0; index < REGION_COUNT; index++)
	{
		struct region *region = &regions[index];

		if (region->virtual_address)
		{
			region->address = allocate_virtual(region->virtual_address, region->size);
			if (region->address)
			{
				xbox_log("ok   %-28s %08lX bytes at virtual %08lX", region->name, region->size,
					(unsigned long)region->address);
				log_virtual_pages(region->name, region->address, region->size);
			}
			else
			{
				all = 0;
				xbox_log("FAIL %-28s %08lX bytes at virtual %08lX", region->name, region->size,
					region->virtual_address);
			}
			continue;
		}
		if (region->physical_address)
		{
			region->address = MmAllocateContiguousMemoryEx(region->size, region->physical_address,
				region->physical_address + region->size - 1, 0, region->protect);
		}
		else
		{
			region->address = MmAllocateContiguousMemoryEx(region->size, 0, 0xFFFFFFFF, 0, region->protect);
		}
		if (region->address)
		{
			xbox_log("ok   %-28s %08lX bytes at physical %08lX", region->name, region->size,
				(unsigned long)MmGetPhysicalAddress(region->address));
		}
		else
		{
			all = 0;
			xbox_log("FAIL %-28s %08lX bytes at physical %08lX", region->name, region->size,
				region->physical_address);
			if (region->physical_address)
				scan_range(region->physical_address, region->size);
		}
	}
	return all;
}

int main(void)
{
	int stack_variable = 0;
	void *heap_block;
	int serial;
	int all;

	serial = xbox_log_initialize(0);
	xbox_log("Xbox memory probe (serial %s)", serial ? "on" : "off");
	log_statistics("at start");
	heap_block = malloc(64);
	log_physical("code (main)", (void *)main);
	log_physical("data (regions)", (void *)regions);
	log_physical("stack", &stack_variable);
	log_physical("heap (malloc)", heap_block);

	all = allocate_regions();
	log_statistics("with the caches held");
	probe_upper_half();

	if (XVideoSetMode(640, 480, 32, REFRESH_DEFAULT))
	{
		xbox_log_to_screen(1);
		xbox_log("Xbox memory probe: the game's regions %s", all ? "all fit" : "do NOT all fit (see the log)");
		log_statistics("with video");
	}
	else
	{
		xbox_log("XVideoSetMode failed with the caches held");
	}

	xbox_log_done();
	for (;;)
		Sleep(1000);
	return 0;
}
