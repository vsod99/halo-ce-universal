/*
SUPPORT.C

What the musl files in src/ call that is not among them (README.md):
strnlen and wctomb, which a C library may lack, and musl's stdio hooks,
which a string never reaches. Written for this port, not musl's.
*/

#include "include/musl_stdio.h"
#include "include/stdio_impl.h"

size_t halo_musl_strnlen(const char *string, size_t maximum)
{
	size_t length = 0;

	while (length < maximum && string[length])
		length++;
	return length;
}

/* UTF-8, as musl's (the wide characters here are 16 bits) */
int halo_musl_wctomb(char *s, wchar_t wc)
{
	unsigned int c = (unsigned short)wc;

	if (!s)
		return 0;
	if (c < 0x80)
	{
		s[0] = (char)c;
		return 1;
	}
	if (c < 0x800)
	{
		s[0] = (char)(0xc0 | (c >> 6));
		s[1] = (char)(0x80 | (c & 0x3f));
		return 2;
	}
	s[0] = (char)(0xe0 | (c >> 12));
	s[1] = (char)(0x80 | ((c >> 6) & 0x3f));
	s[2] = (char)(0x80 | (c & 0x3f));
	return 3;
}

/* (a string pseudo-FILE ends at its NUL: strtod never reads past it) */
int halo_musl_uflow(FILE *f)
{
	(void)f;
	return EOF;
}

void halo_musl_stdio_exit_needed(void)
{
}
