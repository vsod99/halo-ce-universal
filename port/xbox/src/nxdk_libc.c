/*
NXDK_LIBC.C

What the Xbox's C library, nxdk's pdclib, lacks or gets wrong, replaced
(the link takes an object's definition over the library's, so each of its
objects' functions is defined here whole):

- the printf family: pdclib formats no floating point numbers;
- strtod and atof: pdclib has none (nxdk's assert);
- fmod, which pdclib computes with one partial remainder; scalbn and
  lrint, which assert; frexp, wrong at zero;
- memset, memcpy and memmove, which nxdk builds a byte at a time without
  optimization (its makefile names no -O: a fifth of b30's processor time
  in xemu): here the Pentium III's string instructions, four bytes at a
  time;
- getenv, which has no environment: here the NAME=VALUE lines of
  D:\\environment.txt, which `tools/xbox_dev.py run --env` writes (the
  HALO_* overrides of config.toml, which is on the hard disk).

The replacements are musl's (port/third_party/musl-stdio), which round as
glibc's and the Windows UCRT's do; lrint is the x87's own rounding.

pdclib's standard output and error have no file behind them: what is
written to them goes to the log (COM2 for the development loop,
port/xbox/common/xbox_log.c).
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xbox_log.h"

int halo_musl_vsnprintf(char *buffer, size_t size, const char *format, va_list arguments);
double halo_musl_strtod(const char *text, char **end);
float halo_musl_strtof(const char *text, char **end);

/* ---------- strings */

int vsnprintf(char *buffer, size_t size, const char *format, va_list arguments)
{
	return halo_musl_vsnprintf(buffer, size, format, arguments);
}

int snprintf(char *buffer, size_t size, const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = halo_musl_vsnprintf(buffer, size, format, arguments);
	va_end(arguments);
	return result;
}

int vsprintf(char *buffer, const char *format, va_list arguments)
{
	return halo_musl_vsnprintf(buffer, (size_t)-1 >> 1, format, arguments);
}

int sprintf(char *buffer, const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = vsprintf(buffer, format, arguments);
	va_end(arguments);
	return result;
}

/* ---------- streams */

int vfprintf(FILE *stream, const char *format, va_list arguments)
{
	char small[512];
	char *text = small;
	va_list copy;
	int length;

	va_copy(copy, arguments);
	length = halo_musl_vsnprintf(small, sizeof(small), format, copy);
	va_end(copy);
	if (length < 0)
		return length;
	if ((size_t)length >= sizeof(small))
	{
		text = malloc((size_t)length + 1);
		if (!text)
			return -1;
		halo_musl_vsnprintf(text, (size_t)length + 1, format, arguments);
	}
	if (stream == stdout || stream == stderr)
		xbox_log_write(text);
	else if (fwrite(text, 1, (size_t)length, stream) != (size_t)length)
		length = -1;
	if (text != small)
		free(text);
	return length;
}

int fprintf(FILE *stream, const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = vfprintf(stream, format, arguments);
	va_end(arguments);
	return result;
}

int vprintf(const char *format, va_list arguments)
{
	return vfprintf(stdout, format, arguments);
}

int printf(const char *format, ...)
{
	va_list arguments;
	int result;

	va_start(arguments, format);
	result = vfprintf(stdout, format, arguments);
	va_end(arguments);
	return result;
}

int fputs(const char *text, FILE *stream)
{
	if (stream == stdout || stream == stderr)
	{
		xbox_log_write(text);
		return 0;
	}
	return fwrite(text, 1, strlen(text), stream) == strlen(text) ? 0 : EOF;
}

/* ---------- memory */

void *memset(void *destination, int value, size_t size)
{
	unsigned char *bytes = destination;
	unsigned long word = (unsigned char)value * 0x01010101ul;
	size_t words;

	for (; size && ((unsigned long)bytes & 3); size--)
		*bytes++ = (unsigned char)value;
	words = size >> 2;
	__asm__ volatile("rep stosl" : "+D"(bytes), "+c"(words) : "a"(word) : "memory");
	for (size &= 3; size; size--)
		*bytes++ = (unsigned char)value;
	return destination;
}

void *memcpy(void *destination, const void *source, size_t size)
{
	void *to = destination;
	const void *from = source;
	size_t words = size >> 2;
	size_t bytes = size & 3;

	__asm__ volatile("rep movsl\n\tmovl %3, %%ecx\n\trep movsb"
		: "+D"(to), "+S"(from), "+c"(words) : "r"(bytes) : "memory");
	return destination;
}

void *memmove(void *destination, const void *source, size_t size)
{
	unsigned char *to = destination;
	const unsigned char *from = source;

	if (to <= from || to >= from + size)
		return memcpy(destination, source, size);
	/* (overlapping, the destination above: from the end down) */
	to += size - 1;
	from += size - 1;
	__asm__ volatile("std\n\trep movsb\n\tcld" : "+D"(to), "+S"(from), "+c"(size) : : "memory");
	return destination;
}

/* ---------- numbers */

double strtod(const char *text, char **end)
{
	return halo_musl_strtod(text, end);
}

float strtof(const char *text, char **end)
{
	return halo_musl_strtof(text, end);
}

double atof(const char *text)
{
	return halo_musl_strtod(text, NULL);
}

/* ---------- the environment */

char *getenv(const char *name)
{
	static char text[4096];
	static char *entries[64];
	static int count = -1;
	size_t length = strlen(name);
	int index;

	if (count < 0)
	{
		FILE *file = fopen("D:\\environment.txt", "r");
		size_t size = 0;
		char *line, *end;

		count = 0;
		if (file)
		{
			size = fread(text, 1, sizeof(text) - 1, file);
			fclose(file);
		}
		text[size] = 0;
		for (line = text; *line && count < (int)(sizeof(entries) / sizeof(entries[0])); line = end)
		{
			end = line + strcspn(line, "\r\n");
			if (*end)
				*end++ = 0;
			if (*line)
			{
				entries[count++] = line;
				xbox_log("environment: %s", line);
			}
		}
	}
	for (index = 0; index < count; index++)
	{
		if (!strncmp(entries[index], name, length) && entries[index][length] == '=')
			return entries[index] + length + 1;
	}
	return NULL;
}

/* ---------- maths (long double is double: the Microsoft ABI's) */

double halo_musl_fmod(double x, double y);
float halo_musl_fmodf(float x, float y);
double halo_musl_scalbn(double x, int n);
float halo_musl_scalbnf(float x, int n);
double halo_musl_frexp(double x, int *e);
float halo_musl_frexpf(float x, int *e);

double fmod(double x, double y) { return halo_musl_fmod(x, y); }
float fmodf(float x, float y) { return halo_musl_fmodf(x, y); }
long double fmodl(long double x, long double y) { return halo_musl_fmod(x, y); }
double scalbn(double x, int n) { return halo_musl_scalbn(x, n); }
float scalbnf(float x, int n) { return halo_musl_scalbnf(x, n); }
long double scalbnl(long double x, int n) { return halo_musl_scalbn(x, n); }
double frexp(double x, int *e) { return halo_musl_frexp(x, e); }
float frexpf(float x, int *e) { return halo_musl_frexpf(x, e); }
long double frexpl(long double x, int *e) { return halo_musl_frexp(x, e); }

/* to an integer in the current rounding mode, as the x87 stores one (the
game's fast_ftol: port/linux/src/halo_linker_common.c) */
long lrint(double x)
{
	long result;

	__asm__ ("fistpl %0" : "=m"(result) : "t"(x) : "st");
	return result;
}

long lrintf(float x)
{
	return lrint(x);
}

long lrintl(long double x)
{
	return lrint(x);
}
