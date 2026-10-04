/*
NSPIRE_MMU.H

The ARM926EJ-S's MMU and caches, for the Nspire port's memory window
(nspire_memory.c) and pager (nspire_paging.c).

The caches are virtually indexed: memory reached through two addresses (a
heap page and the window page mapped onto it) has a line for each, so data
written through one address is cleaned to memory, and the other address's
lines invalidated, before it is read through the other. Hardware page table
walks read memory, not the cache, so table entries are cleaned too.
*/

#ifndef __HALO_NSPIRE_MMU_H
#define __HALO_NSPIRE_MMU_H

#define NSPIRE_CACHE_LINE 32UL
#define NSPIRE_PAGE_SIZE 0x1000UL

/* level 1: a coarse page table in domain 0 (the OS's heap sections are in
domain 0, which the DACR makes a manager domain) */
#define NSPIRE_LEVEL1_COARSE(table) (((unsigned long)(table) & 0xFFFFFC00UL) | 0x10UL | 0x01UL)
/* level 2: a small page, read/write in every subpage, cacheable and
bufferable as the heap is */
#define NSPIRE_LEVEL2_SMALL_PAGE(physical) (((unsigned long)(physical) & 0xFFFFF000UL) | 0xFF0UL | 0x0CUL | 0x02UL)

static inline volatile unsigned long *nspire_level1_table(void)
{
	unsigned long value;

	__asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(value));
	return (volatile unsigned long *)(value & ~0x3FFFUL);
}

static inline void nspire_drain_write_buffer(void)
{
	__asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}

/* the whole data cache, written back and emptied */
static inline void nspire_clean_invalidate_data_cache(void)
{
	__asm__ volatile(
		"1: mrc p15, 0, APSR_nzcv, c7, c14, 3\n"
		"   bne 1b\n"
		::: "cc", "memory");
	nspire_drain_write_buffer();
}

/* the lines of [address, address + size), written back */
static inline void nspire_clean_data_range(unsigned long address, unsigned long size)
{
	unsigned long line;

	for (line = address & ~(NSPIRE_CACHE_LINE - 1); line < address + size; line += NSPIRE_CACHE_LINE)
		__asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(line) : "memory");
	nspire_drain_write_buffer();
}

/* the lines of [address, address + size), written back and emptied */
static inline void nspire_clean_invalidate_data_range(unsigned long address, unsigned long size)
{
	unsigned long line;

	for (line = address & ~(NSPIRE_CACHE_LINE - 1); line < address + size; line += NSPIRE_CACHE_LINE)
		__asm__ volatile("mcr p15, 0, %0, c7, c14, 1" :: "r"(line) : "memory");
	nspire_drain_write_buffer();
}

static inline void nspire_invalidate_tlb(void)
{
	__asm__ volatile("mcr p15, 0, %0, c8, c7, 0" :: "r"(0) : "memory");
}

static inline void nspire_invalidate_tlb_entry(unsigned long address)
{
	__asm__ volatile("mcr p15, 0, %0, c8, c7, 1" :: "r"(address & ~(NSPIRE_PAGE_SIZE - 1)) : "memory");
}

/* The heap is identity mapped with sections (port/nspire/probe showed it),
so a heap address is its physical address; the tables are walked anyway, in
case an OS version maps it otherwise. 0xFFFFFFFF when not mapped. */
static inline unsigned long nspire_virtual_to_physical(unsigned long address)
{
	volatile unsigned long *level1 = nspire_level1_table();
	unsigned long entry = level1[address >> 20];

	if ((entry & 3) == 2)
		return (entry & 0xFFF00000UL) | (address & 0x000FFFFFUL);
	if ((entry & 3) == 1)
	{
		volatile unsigned long *level2 = (volatile unsigned long *)(entry & 0xFFFFFC00UL);
		unsigned long page = level2[(address >> 12) & 0xFF];

		if ((page & 3) == 1)
			return (page & 0xFFFF0000UL) | (address & 0xFFFFUL);
		if ((page & 3) == 2)
			return (page & 0xFFFFF000UL) | (address & 0xFFFUL);
	}
	return 0xFFFFFFFFUL;
}

#endif
