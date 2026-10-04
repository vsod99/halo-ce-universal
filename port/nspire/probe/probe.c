/*
PROBE.C

Hardware probe for the TI-Nspire port. Run it once on the calculator: it
measures the memory a program can allocate and checks that the port can map
memory at the Xbox addresses the game is linked to (0x80000000 up), then
writes its findings to halo_probe.txt.tns next to itself and shows a summary.

It changes nothing permanently: the one page table entry it tests is put
back before it exits.
*/

#include <libndls.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEGABYTE 0x100000UL
#define WINDOW_BASE 0x80000000UL

static char report[4096];
static int report_length;

static void say(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	report_length += vsnprintf(report + report_length, sizeof(report) - report_length, format, arguments);
	va_end(arguments);
	if (report_length >= (int)sizeof(report))
		report_length = sizeof(report) - 1;
}

/* ---------- the MMU (ARM926EJ-S, CP15) */

static unsigned long translation_table_base(void)
{
	unsigned long value;

	__asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(value));
	return value & ~0x3FFFUL;
}

static unsigned long domain_access(void)
{
	unsigned long value;

	__asm__ volatile("mrc p15, 0, %0, c3, c0, 0" : "=r"(value));
	return value;
}

static void clean_invalidate_data_cache(void)
{
	/* test, clean and invalidate until the whole data cache is clean */
	__asm__ volatile(
		"1: mrc p15, 0, r15, c7, c14, 3\n"
		"   bne 1b\n"
		"   mov r0, #0\n"
		"   mcr p15, 0, r0, c7, c10, 4\n" /* drain the write buffer */
		::: "r0", "cc", "memory");
}

static void invalidate_tlb(void)
{
	__asm__ volatile("mov r0, #0\n mcr p15, 0, r0, c8, c7, 0" ::: "r0", "memory");
}

/* the physical address of a virtual one, walking the tables the way the MMU
does; 0xFFFFFFFF when it is not mapped */
static unsigned long virtual_to_physical(unsigned long address)
{
	volatile unsigned long *level1 = (volatile unsigned long *)translation_table_base();
	unsigned long entry = level1[address >> 20];

	switch (entry & 3)
	{
	case 2: /* section */
		return (entry & 0xFFF00000UL) | (address & 0x000FFFFFUL);
	case 1: /* coarse table: 256 entries */
	{
		volatile unsigned long *level2 = (volatile unsigned long *)(entry & 0xFFFFFC00UL);
		unsigned long page = level2[(address >> 12) & 0xFF];

		if ((page & 3) == 1) /* large page, 64 KB */
			return (page & 0xFFFF0000UL) | (address & 0xFFFFUL);
		if ((page & 3) == 2) /* small page, 4 KB */
			return (page & 0xFFFFF000UL) | (address & 0xFFFUL);
		return 0xFFFFFFFFUL;
	}
	case 3: /* fine table: 1024 entries */
	{
		volatile unsigned long *level2 = (volatile unsigned long *)(entry & 0xFFFFF000UL);
		unsigned long page = level2[(address >> 10) & 0x3FF];

		if ((page & 3) == 1)
			return (page & 0xFFFF0000UL) | (address & 0xFFFFUL);
		if ((page & 3) == 2)
			return (page & 0xFFFFF000UL) | (address & 0xFFFUL);
		if ((page & 3) == 3) /* tiny page, 1 KB */
			return (page & 0xFFFFFC00UL) | (address & 0x3FFUL);
		return 0xFFFFFFFFUL;
	}
	}
	return 0xFFFFFFFFUL;
}

/* ---------- memory */

static unsigned long largest_block(void)
{
	unsigned long low = 0, high = 128 * MEGABYTE;

	while (high - low > 64 * 1024)
	{
		unsigned long middle = low + (high - low) / 2;
		void *block = malloc(middle);

		if (block)
		{
			free(block);
			low = middle;
		}
		else
		{
			high = middle;
		}
	}
	return low;
}

static unsigned long total_in_megabytes(void)
{
	void *blocks[256];
	unsigned long count = 0, index;

	while (count < 256 && (blocks[count] = malloc(MEGABYTE)) != NULL)
		count++;
	for (index = 0; index < count; index++)
		free(blocks[index]);
	return count;
}

/* Maps one section at 0x80000000 onto a 1 MB-aligned piece of a heap block
(which must be physically contiguous there), writes through the new address,
and reads the value back through the heap's own address. */
static void test_window(void)
{
	volatile unsigned long *level1 = (volatile unsigned long *)translation_table_base();
	unsigned long index = WINDOW_BASE >> 20;
	unsigned long saved = level1[index];
	unsigned char *block = malloc(2 * MEGABYTE);
	unsigned long aligned, physical, heap_entry, section;
	unsigned long base_page_physical;
	unsigned long page;
	int contiguous = 1;

	if (!block)
	{
		say("window test: no 2 MB block\n");
		return;
	}
	aligned = ((unsigned long)block + MEGABYTE - 1) & ~(MEGABYTE - 1);
	physical = virtual_to_physical(aligned);
	base_page_physical = physical;
	for (page = 0; page < MEGABYTE; page += 0x1000)
	{
		if (virtual_to_physical(aligned + page) != base_page_physical + page)
			contiguous = 0;
	}
	say("heap block %08lx -> physical %08lx (%s)\n", aligned, physical,
		contiguous ? "contiguous" : "scattered");
	heap_entry = level1[aligned >> 20];
	say("heap L1 entry %08lx, window L1 entry %08lx\n", heap_entry, saved);

	if (saved != 0)
	{
		say("window test: 0x80000000 is already mapped, skipped\n");
		free(block);
		return;
	}
	if (!contiguous || (physical & (MEGABYTE - 1)) != 0)
	{
		say("window test: needs page tables (heap is not section-aligned)\n");
		free(block);
		return;
	}

	/* a section with the heap's domain, access and cache bits: AP=11 (read
	and write), domain from the heap entry, C and B from the heap entry when it
	is a section, else cacheable and bufferable */
	section = (physical & 0xFFF00000UL) | (3UL << 10) | 0x12;
	if ((heap_entry & 3) == 2)
		section |= heap_entry & (0xFUL << 5 | 0xCUL);
	else
		section |= (heap_entry & (0xFUL << 5)) | 0xCUL;

	clean_invalidate_data_cache();
	level1[index] = section;
	clean_invalidate_data_cache();
	invalidate_tlb();

	*(volatile unsigned long *)WINDOW_BASE = 0x48414C4FUL; /* HALO */
	*(volatile unsigned long *)(WINDOW_BASE + 0xFFFFC) = 0x0000CE01UL;
	clean_invalidate_data_cache();
	say("window test: wrote at 0x80000000, heap reads %08lx %08lx (%s)\n",
		*(volatile unsigned long *)aligned, *(volatile unsigned long *)(aligned + 0xFFFFC),
		*(volatile unsigned long *)aligned == 0x48414C4FUL
			&& *(volatile unsigned long *)(aligned + 0xFFFFC) == 0x0000CE01UL ? "PASS" : "FAIL");

	level1[index] = saved;
	clean_invalidate_data_cache();
	invalidate_tlb();
	free(block);
}

/* The port's clock (port/nspire/src/nspire_time.c) is the second timer of
the SP804 at 0x900D0000, free running at 32768 Hz; the first one is the
OS's. Counts it across a 500 ms msleep, which uses the first. */
static void test_timer(void)
{
	volatile unsigned long *timer = (volatile unsigned long *)0x900D0020;
	unsigned long saved_load = timer[0], saved_control = timer[2];
	unsigned long before, after;

	say("timer 2 before: load %08lx value %08lx control %08lx\n", timer[0], timer[1], timer[2]);
	timer[2] = 0;
	timer[0] = 0xFFFFFFFFUL;
	timer[2] = 0x82; /* enabled, free running, no interrupt, 32-bit */
	before = timer[1];
	msleep(500);
	after = timer[1];
	say("timer 2: %lu ticks in 500 ms (expect about 16384)\n", before - after);
	timer[2] = 0;
	timer[0] = saved_load;
	timer[2] = saved_control;
}

int main(int argc, char **argv)
{
	char path[256];
	char *slash;
	FILE *file;
	unsigned long entry;
	unsigned long used = 0, index;
	volatile unsigned long *level1;

	say("Halo Nspire probe\n");
	say("hwtype %u, touchpad %d\n", hwtype(), is_touchpad);
	say("largest block %lu KB\n", largest_block() / 1024);
	say("total in 1 MB blocks %lu MB\n", total_in_megabytes());
	say("TTB %08lx, DACR %08lx\n", translation_table_base(), domain_access());
	say("program at %08lx -> physical %08lx\n", (unsigned long)&main,
		virtual_to_physical((unsigned long)&main));

	/* which megabytes of the address space are mapped (runs of them) */
	level1 = (volatile unsigned long *)translation_table_base();
	say("mapped (MB index: first L1 entry):\n");
	for (index = 0; index < 4096; index++)
	{
		entry = level1[index];
		if ((entry & 3) && (index == 0 || (level1[index - 1] & 3) == 0))
			say(" %03lx: %08lx", index, entry);
		if ((entry & 3) && (index == 4095 || (level1[index + 1] & 3) == 0))
			say(" .. %03lx\n", index);
		if (entry & 3)
			used++;
	}
	say("%lu MB mapped\n", used);

	test_window();
	test_timer();

	strncpy(path, argc > 0 && argv[0] ? argv[0] : "/documents/ndless/probe.tns", sizeof(path) - 32);
	path[sizeof(path) - 32] = 0;
	slash = strrchr(path, '/');
	strcpy(slash ? slash + 1 : path, "halo_probe.txt.tns");
	file = fopen(path, "w");
	if (file)
	{
		fputs(report, file);
		fclose(file);
	}

	show_msgbox("Halo probe", report);
	return 0;
}
