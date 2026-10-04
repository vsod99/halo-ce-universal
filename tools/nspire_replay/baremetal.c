/*
BAREMETAL.C

What the C library would give the replay on the calculator: memory
allocation (a simple heap) and the memory functions. Compiled without
builtins, so the loops below stay loops.
*/

#include <stddef.h>

#define HEAP_SIZE (96UL << 20)

static unsigned char *heap_next;
extern unsigned char _end[];

struct block
{
	size_t size;
	size_t reserved;
};

void *memcpy(void *destination, const void *source, size_t size)
{
	if (!(((size_t)destination | (size_t)source | size) & 3))
	{
		unsigned long *d = destination;
		const unsigned long *s = source;

		size >>= 2;
		while (size--)
			*d++ = *s++;
	}
	else
	{
		unsigned char *d = destination;
		const unsigned char *s = source;

		while (size--)
			*d++ = *s++;
	}
	return destination;
}

void *memmove(void *destination, const void *source, size_t size)
{
	unsigned char *d = destination;
	const unsigned char *s = source;

	if (d < s)
		while (size--)
			*d++ = *s++;
	else
		while (size--)
			d[size] = s[size];
	return destination;
}

void *memset(void *destination, int value, size_t size)
{
	if (!(((size_t)destination | size) & 3))
	{
		unsigned long word = (unsigned char)value * 0x01010101UL, *d = destination;

		size >>= 2;
		while (size--)
			*d++ = word;
	}
	else
	{
		unsigned char *d = destination;

		while (size--)
			*d++ = (unsigned char)value;
	}
	return destination;
}

int memcmp(const void *a, const void *b, size_t size)
{
	const unsigned char *x = a, *y = b;

	for (; size; size--, x++, y++)
		if (*x != *y)
			return *x - *y;
	return 0;
}

size_t strlen(const char *text)
{
	size_t length = 0;

	while (text[length])
		length++;
	return length;
}

void *malloc(size_t size)
{
	struct block *block;

	if (!heap_next)
		heap_next = (unsigned char *)(((size_t)_end + 15) & ~(size_t)15);
	block = (struct block *)heap_next;
	block->size = size;
	heap_next += sizeof(struct block) + ((size + 15) & ~(size_t)15);
	return block + 1;
}

void *calloc(size_t count, size_t size)
{
	void *pointer = malloc(count * size);

	memset(pointer, 0, count * size);
	return pointer;
}

void free(void *pointer)
{
	(void)pointer;
}

void *realloc(void *pointer, size_t size)
{
	void *grown;

	if (!pointer)
		return malloc(size);
	if (((struct block *)pointer - 1)->size >= size)
		return pointer;
	grown = malloc(size);
	memcpy(grown, pointer, ((struct block *)pointer - 1)->size);
	return grown;
}

int *__errno(void)
{
	static int error;

	return &error;
}

void abort(void)
{
	for (;;)
		;
}

void __aeabi_memcpy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy4(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy8(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memmove(void *d, const void *s, size_t n) { memmove(d, s, n); }
void __aeabi_memmove4(void *d, const void *s, size_t n) { memmove(d, s, n); }
void __aeabi_memclr(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr4(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr8(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memset(void *d, size_t n, int c) { memset(d, c, n); }
void __aeabi_memset4(void *d, size_t n, int c) { memset(d, c, n); }
void __aeabi_memset8(void *d, size_t n, int c) { memset(d, c, n); }
