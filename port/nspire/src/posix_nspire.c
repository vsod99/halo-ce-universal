/*
POSIX_NSPIRE.C

The helpers of port/linux/src/posix.h over newlib and Ndless's system calls
(libsyscalls: open, read, stat, opendir, ...), and the few C library and
compiler runtime functions the calculator's toolchain lacks.

Like the Linux port's posix_*.c, this file sees the C library's own
headers, not the MSVC ones the game and the rest of the platform layer see
(tools/nspire_build.py compiles it without the prefix header).
*/

#include "posix.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ---------- files */

static void information_from_stat(const struct stat *status, struct posix_file_information *information)
{
	memset(information, 0, sizeof(*information));
	if (S_ISDIR(status->st_mode))
		information->flags |= _posix_file_is_directory;
	information->size_low = (posix_ulong)status->st_size;
	information->modification_seconds = (posix_ulong)status->st_mtim.tv_sec;
	information->access_seconds = (posix_ulong)status->st_atim.tv_sec;
	information->creation_seconds = (posix_ulong)status->st_ctim.tv_sec;
}

int posix_stat(const char *path, struct posix_file_information *information)
{
	struct stat status;

	if (stat(path, &status) != 0)
		return -1;
	information_from_stat(&status, information);
	return 0;
}

int posix_fstat(int descriptor, struct posix_file_information *information)
{
	struct stat status;

	if (fstat(descriptor, &status) != 0)
		return -1;
	information_from_stat(&status, information);
	return 0;
}

/* the calculator's file system keeps no times the game needs */
int posix_set_file_times(const char *path,
	posix_ulong access_seconds, posix_ulong access_nanoseconds,
	posix_ulong modification_seconds, posix_ulong modification_nanoseconds)
{
	(void)path;
	(void)access_seconds;
	(void)access_nanoseconds;
	(void)modification_seconds;
	(void)modification_nanoseconds;
	return 0;
}

int posix_seek(int descriptor, posix_long offset_low, posix_long offset_high, int whence,
	posix_ulong *position_low, posix_ulong *position_high)
{
	off_t position;

	/* files on the calculator are far below 2 GB */
	if (offset_high != 0 && offset_high != -1)
	{
		errno = EINVAL;
		return -1;
	}
	position = lseek(descriptor, (off_t)offset_low, whence);
	if (position == (off_t)-1)
		return -1;
	if (position_low)
		*position_low = (posix_ulong)position;
	if (position_high)
		*position_high = 0;
	return 0;
}

/* Ndless has no ftruncate. Growing a file writes zeros at its end; the game
only ever grows (saved game files are made at their full size). */
int posix_truncate(int descriptor, posix_ulong size_low, posix_ulong size_high)
{
	static const char zeros[512];
	off_t end;

	if (size_high)
	{
		errno = EINVAL;
		return -1;
	}
	end = lseek(descriptor, 0, SEEK_END);
	if (end == (off_t)-1)
		return -1;
	while ((posix_ulong)end < size_low)
	{
		posix_ulong count = size_low - (posix_ulong)end < sizeof(zeros) ? size_low - (posix_ulong)end : sizeof(zeros);

		if (write(descriptor, zeros, count) != (ssize_t)count)
			return -1;
		end += count;
	}
	if ((posix_ulong)end > size_low)
	{
		errno = ENOSYS;
		return -1;
	}
	return 0;
}

/* the documents folder of a CX II holds about 90 MB */
int posix_disk_space(const char *path,
	posix_ulong *free_low, posix_ulong *free_high,
	posix_ulong *total_low, posix_ulong *total_high)
{
	(void)path;
	if (free_low)
		*free_low = 16UL * 1024 * 1024;
	if (free_high)
		*free_high = 0;
	if (total_low)
		*total_low = 90UL * 1024 * 1024;
	if (total_high)
		*total_high = 0;
	return 0;
}

int posix_set_read_only(const char *path, int read_only)
{
	(void)path;
	(void)read_only;
	return 0;
}

int posix_make_directory(const char *path)
{
	return mkdir(path, 0755);
}

void *posix_directory_open(const char *path)
{
	return opendir(path);
}

int posix_directory_next(void *directory, char *name, posix_ulong name_size)
{
	struct dirent *entry;

	while ((entry = readdir((DIR *)directory)) != NULL)
	{
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		strncpy(name, entry->d_name, name_size - 1);
		name[name_size - 1] = 0;
		return 1;
	}
	return 0;
}

void posix_directory_close(void *directory)
{
	closedir((DIR *)directory);
}

/* The calculator's file names can differ from the game's in case, and a
file sent from a computer ends in .tns: "b30.map.tns" answers for "b30.map". */
int posix_find_entry_case_insensitive(const char *directory, const char *name,
	char *result, posix_ulong result_size)
{
	DIR *handle = opendir(directory);
	struct dirent *entry;
	int found = 0;
	size_t length = strlen(name);

	if (!handle)
		return 0;
	while (!found && (entry = readdir(handle)) != NULL)
	{
		if (!strcasecmp(entry->d_name, name) ||
			(!strncasecmp(entry->d_name, name, length) && !strcasecmp(entry->d_name + length, ".tns")))
		{
			strncpy(result, entry->d_name, result_size - 1);
			result[result_size - 1] = 0;
			found = 1;
		}
	}
	closedir(handle);
	return found;
}

/* ---------- missing C library functions */

ssize_t pread(int descriptor, void *buffer, size_t count, off_t offset)
{
	if (lseek(descriptor, offset, SEEK_SET) == (off_t)-1)
		return -1;
	return read(descriptor, buffer, count);
}

ssize_t pwrite(int descriptor, const void *buffer, size_t count, off_t offset)
{
	if (lseek(descriptor, offset, SEEK_SET) == (off_t)-1)
		return -1;
	return write(descriptor, buffer, count);
}

ssize_t readlink(const char *path, char *buffer, size_t size)
{
	(void)path;
	(void)buffer;
	(void)size;
	errno = ENOSYS;
	return -1;
}

/* The OS's malloc (libsyscalls) does not say how big a block is: the
game's _msize callers get the size they asked for from the header below.
Nothing on the Nspire needs it: _msize reports 0. */
size_t malloc_usable_size(void *pointer)
{
	(void)pointer;
	return 0;
}

/* ---------- atomics

ARMv5 has no exclusive loads and stores, so clang calls these for the
__sync and __atomic builtins. The port's threads are cooperative and no
interrupt handler touches the game's memory, so plain operations are
atomic enough. */

unsigned int __atomic_fetch_add_4(volatile void *pointer, unsigned int value, int order)
{
	volatile unsigned int *target = pointer;
	unsigned int previous = *target;

	(void)order;
	*target = previous + value;
	return previous;
}

unsigned int __atomic_fetch_sub_4(volatile void *pointer, unsigned int value, int order)
{
	volatile unsigned int *target = pointer;
	unsigned int previous = *target;

	(void)order;
	*target = previous - value;
	return previous;
}

unsigned int __atomic_exchange_4(volatile void *pointer, unsigned int value, int order)
{
	volatile unsigned int *target = pointer;
	unsigned int previous = *target;

	(void)order;
	*target = value;
	return previous;
}

_Bool __atomic_compare_exchange_4(volatile void *pointer, void *expected, unsigned int desired,
	int success_order, int failure_order)
{
	volatile unsigned int *target = pointer;
	unsigned int *expected_value = expected;

	(void)success_order;
	(void)failure_order;
	if (*target == *expected_value)
	{
		*target = desired;
		return 1;
	}
	*expected_value = *target;
	return 0;
}

/* ---------- the game's 32-bit time_t (port/nspire/include/time.h) */

long halo_nspire_time(long *result)
{
	long value = (long)time(NULL);

	if (result)
		*result = value;
	return value;
}

struct tm *halo_nspire_localtime(const long *value)
{
	time_t wide = *value;

	return localtime(&wide);
}

struct tm *halo_nspire_gmtime(const long *value)
{
	time_t wide = *value;

	return gmtime(&wide);
}

long halo_nspire_mktime(struct tm *value)
{
	return (long)mktime(value);
}

char *halo_nspire_ctime(const long *value)
{
	time_t wide = *value;

	return ctime(&wide);
}

/* ---------- files left open at exit

The calculator's OS keeps a file a program leaves open open after it
exits, and has few to give (Ndless allows a program 17): runs that end in an
error would otherwise use them up for every later run until a reset. At
exit every descriptor is closed, after the streams over them are flushed. */

#include <stdio.h>

#define NDLESS_MAXIMUM_OPEN_FILES 20

void nspire_close_all_files(void)
{
	int descriptor;

	fflush(NULL);
	for (descriptor = 3; descriptor < NDLESS_MAXIMUM_OPEN_FILES; descriptor++)
		close(descriptor);
}

/* ---------- the pager's reads of the map file (nspire_paging.c) */

int nspire_file_open_read(const char *path)
{
	return open(path, O_RDONLY);
}

void nspire_file_close(int descriptor)
{
	if (descriptor >= 0)
		close(descriptor);
}

long nspire_file_read_at(int descriptor, unsigned long offset, void *buffer, unsigned long size)
{
	unsigned long total = 0;

	if (lseek(descriptor, (off_t)offset, SEEK_SET) == (off_t)-1)
		return -1;
	while (total < size)
	{
		ssize_t count = read(descriptor, (char *)buffer + total, size - total);

		if (count <= 0)
			return -1;
		total += (unsigned long)count;
	}
	return (long)total;
}
