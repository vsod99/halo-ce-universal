/*
UNISTD.H

The POSIX calls the platform layer shared with Linux makes, for the Xbox
build: nxdk's C library (pdclib) has no file descriptors, so the descriptors
and the rest are port/xbox/src/nxdk_posix.c's. Threads, clocks and sched_yield
are the Windows build's (port/windows/include/posix, win32_threads.c).
*/

#ifndef __HALO_XBOX_UNISTD_H
#define __HALO_XBOX_UNISTD_H

#include <stddef.h>
#include <stdlib.h>
#include <sys/types.h>

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef int ssize_t;
#endif

#define _SC_PAGESIZE 1
#define _SC_PHYS_PAGES 2
#define _SC_AVPHYS_PAGES 3
#define _SC_NPROCESSORS_ONLN 4

#define F_OK 0
#define W_OK 2
#define R_OK 4

/* 64-bit, as on Windows (port/windows/include/posix/unistd.h) */
typedef long long halo_xbox_off_t;
#define off_t halo_xbox_off_t

int close(int descriptor);
ssize_t read(int descriptor, void *buffer, size_t count);
ssize_t write(int descriptor, const void *buffer, size_t count);
/* 32-bit offsets, as MSVC's _lseek (posix_seek moves by 64 bits) */
long lseek(int descriptor, long offset, int whence);
ssize_t pread(int descriptor, void *buffer, size_t count, off_t offset);
ssize_t pwrite(int descriptor, const void *buffer, size_t count, off_t offset);
int unlink(const char *path);
int rmdir(const char *path);
int chdir(const char *path);
char *getcwd(char *buffer, size_t size);
int access(const char *path, int mode);
int isatty(int descriptor);
long sysconf(int name);
/* waits for good (nothing signals the Xbox's threads) */
int pause(void);
/* only for /proc/self/exe: the XBE's path, with forward slashes
(port/linux/src/xbox_files.c finds the data folder from it) */
ssize_t readlink(const char *path, char *buffer, size_t size);

#endif /* __HALO_XBOX_UNISTD_H */
