/*
NSPIRE_HEAP.C

The Nspire port's allocations, tracked so that exiting gives them back.

malloc on the calculator is the OS's own (Ndless libsyscalls), and what a
program leaves allocated stays allocated until the calculator is reset. The
game does not free everything it allocates, and the port holds megabytes
(the pager's compressed blocks), so the link wraps malloc, calloc, realloc
and free (--wrap, tools/nspire_build.py): each block carries a header on a
list, and at exit every block still on it is freed.

The C library's own allocations (stdio buffers) are made inside
libsyscalls, which the wrapping does not reach; newlib reclaims those.
*/

#include "nspire.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define HEAP_MAGIC 0x68656170UL /* 'heap' */

void *__real_malloc(size_t size);
void *__real_realloc(void *memory, size_t size);
void __real_free(void *memory);

struct heap_block
{
	struct heap_block *next;
	struct heap_block *previous;
	unsigned long size;
	unsigned long magic;
};

static struct heap_block list = { &list, &list, 0, HEAP_MAGIC };
static unsigned long block_count, byte_count;

static void link_block(struct heap_block *block, unsigned long size)
{
	block->size = size;
	block->magic = HEAP_MAGIC;
	block->next = list.next;
	block->previous = &list;
	list.next->previous = block;
	list.next = block;
	block_count++;
	byte_count += size;
}

static void unlink_block(struct heap_block *block)
{
	block->previous->next = block->next;
	block->next->previous = block->previous;
	block->magic = 0;
	block_count--;
	byte_count -= block->size;
}

void *__wrap_malloc(size_t size)
{
	struct heap_block *block;

	if (!size)
		return NULL;
	/* the calculator's memory is short: where the big blocks go (the game's
	own are logged by file and line in debug_memory.c) */
	if (size >= 0x40000)
		nspire_log("heap: %lu KB for %p", (unsigned long)size / 1024, __builtin_return_address(0));
	block = __real_malloc(sizeof(*block) + size);
	if (!block)
		return NULL;
	link_block(block, size);
	return block + 1;
}

void *__wrap_calloc(size_t count, size_t size)
{
	unsigned long long total = (unsigned long long)count * size;
	void *memory;

	if (total > 0xFFFFFFF0ULL)
		return NULL;
	memory = __wrap_malloc((size_t)total);
	if (memory)
		memset(memory, 0, (size_t)total);
	return memory;
}

void __wrap_free(void *memory)
{
	struct heap_block *block;

	if (!memory)
		return;
	block = (struct heap_block *)memory - 1;
	if (block->magic != HEAP_MAGIC)
	{
		nspire_log("free of %p, which malloc did not return", memory);
		return;
	}
	unlink_block(block);
	__real_free(block);
}

void *__wrap_realloc(void *memory, size_t size)
{
	struct heap_block *block, *moved;

	if (!memory)
		return __wrap_malloc(size);
	if (!size)
	{
		__wrap_free(memory);
		return NULL;
	}
	block = (struct heap_block *)memory - 1;
	if (block->magic != HEAP_MAGIC)
		return NULL;
	unlink_block(block);
	moved = __real_realloc(block, sizeof(*block) + size);
	if (!moved)
	{
		link_block(block, block->size);
		return NULL;
	}
	link_block(moved, size);
	return moved + 1;
}

/* every block still allocated; runs last at exit (registered first) */
static void heap_release_all(void)
{
	while (list.next != &list)
	{
		struct heap_block *block = list.next;

		unlink_block(block);
		__real_free(block);
	}
}

void nspire_heap_initialize(void)
{
	atexit(heap_release_all);
}

void nspire_heap_statistics(unsigned long *blocks, unsigned long *bytes)
{
	*blocks = block_count;
	*bytes = byte_count;
}
