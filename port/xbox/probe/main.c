/*
MAIN.C

The Xbox port's memory probe (port/xbox/README.md, Phase 0): can an nxdk
program have the physical memory the game asks for at fixed addresses?

The maps are linked to the tag cache at physical 0x3A6000 (0x1600000 bytes,
source/cache/physical_memory_map.c), and the native builds put their larger
game state at physical 0x1A00000 (0x1000000 bytes,
port/linux/include/halo_port_capacity.h). The texture (0x1600000, write
combined) and sound (0x400000) caches go anywhere. The probe asks for them
as the game would, first thing in main, reports where nxdk's start-up left
its own image, stack and heap, and what remains for everything else (code,
heap, Direct3D, the frame buffers), then sets a video mode with the caches
still held, so the screen shows whether that still fits.

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

struct region
{
	const char *name;
	unsigned long physical_address; /* where it must be; 0: anywhere */
	unsigned long size;
	unsigned long protect;
	void *address;
};

static struct region regions[] =
{
	{"tag cache", 0x003A6000, 0x1600000, PAGE_READWRITE, NULL},
	{"game state (native builds)", 0x01A00000, 0x1000000, PAGE_READWRITE, NULL},
	{"texture cache", 0, 0x1600000, PAGE_READWRITE | PAGE_WRITECOMBINE, NULL},
	{"sound cache", 0, 0x400000, PAGE_READWRITE, NULL},
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

static int allocate_regions(void)
{
	int all = 1;
	unsigned int index;

	for (index = 0; index < REGION_COUNT; index++)
	{
		struct region *region = &regions[index];

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
			if (region->physical_address)
			{
				xbox_log("FAIL %-28s %08lX bytes at physical %08lX", region->name, region->size,
					region->physical_address);
				scan_range(region->physical_address, region->size);
			}
			else
			{
				xbox_log("FAIL %-28s %08lX bytes anywhere", region->name, region->size);
			}
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
