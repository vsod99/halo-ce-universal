/*
STDIO.H

The <stdio.h> the musl files in ../src see, instead of the C library's:
musl's FILE (stdio_impl.h) and only what they use. Written for this port,
not musl's.
*/

#ifndef __HALO_MUSL_STDIO_STDIO_H
#define __HALO_MUSL_STDIO_STDIO_H

#include <stdarg.h>
#include <stddef.h>

typedef struct _IO_FILE FILE;
typedef long long off_t;

#define EOF (-1)

#define ferror(f) (!!((f)->flags & F_ERR))

int vfprintf(FILE *restrict f, const char *restrict fmt, va_list ap);
int vsnprintf(char *restrict s, size_t n, const char *restrict fmt, va_list ap);
size_t fwrite(const void *restrict src, size_t size, size_t nmemb, FILE *restrict f);

#endif /* __HALO_MUSL_STDIO_STDIO_H */
