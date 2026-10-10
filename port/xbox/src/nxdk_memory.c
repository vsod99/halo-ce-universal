/*
NXDK_MEMORY.C

Xbox contiguous memory (XPhysicalAlloc) and page protection, from the
Xbox's own kernel: the other ports emulate the window at 0x80000000 that
it maps physical memory at (port/linux/src/xbox_memory.c), which is real
here.

The kernel gives contiguous memory only from the low 64 MB, even on a
128 MB console, and ordinary virtual memory takes the low half's free pages
before the upper half's (port/xbox/README.md, the probe). So the caches are
contiguous and allocated first (cache/physical_memory_map.c), and the game
state, which is larger on the native builds than on the Xbox
(halo_port_capacity.h), is virtual memory at a fixed address instead of at
physical 0x1A00000 (xbox_game_state_allocate).
*/

#include <xboxkrnl/xboxkrnl.h>
#include <windows.h>

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

void *platform_contiguous_alloc(unsigned long size, unsigned long alignment,
	unsigned long physical_address, DWORD protect)
{
	void *result;

	size = (size + PAGE_BYTES - 1) & ~(PAGE_BYTES - 1);
	if (alignment < PAGE_BYTES)
		alignment = PAGE_BYTES;
	if (physical_address == ANY_PHYSICAL_ADDRESS)
		result = MmAllocateContiguousMemoryEx(size, 0, 0xFFFFFFFF, alignment, protect);
	else
		result = MmAllocateContiguousMemoryEx(size, physical_address, physical_address + size - 1, alignment, protect);
	/* (a placed block is had exactly where asked or not at all) */
	return result;
}

void platform_contiguous_free(void *address)
{
	if (platform_is_contiguous(address))
		MmFreeContiguousMemory(address);
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

/* ---------- the game state */

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
		MmFreeContiguousMemory(address);
	}
	else if (address)
	{
		/* the game state (xbox_game_state_allocate) */
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
