/*
NXDK_POSIX.C

The POSIX calls the platform layer shared with Linux makes, and the file
half of port/linux/src/posix.h, with nxdk's Windows API. nxdk's C library
(pdclib) has no file descriptors: a descriptor here is an index into a table
of Windows file handles. Threads, clocks and sched_yield are the Windows
build's (port/windows/src/win32_threads.c).

Paths are the platform layer's host paths, with forward slashes and a drive
letter ("D:/maps/ui.map": port/linux/src/xbox_files.c translates the game's
to them); nxdk takes them with backslashes. D: is the XBE's folder (nxdk's
automount) and E: the hard disk's first partition (nxdk_main.c). The Xbox's
file systems (FATX and ISO 9660) ignore case.
*/

#include <xboxkrnl/xboxkrnl.h>
#include <windows.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "posix.h"
#include "nxdk_platform.h"

#define MAXIMUM_DESCRIPTORS 256
/* seconds from 1601 (FILETIME) to 1970 (Unix time) */
#define FILETIME_UNIX_EPOCH 11644473600ULL

/* ---------- paths */

/* host path to nxdk's: backslashes, and a bare drive ("D:") as its root */
static int native_path(const char *path, char *native, size_t size)
{
	size_t length = strlen(path);
	size_t index;

	if (length + 2 > size)
	{
		errno = ENAMETOOLONG;
		return 0;
	}
	for (index = 0; index <= length; index++)
		native[index] = path[index] == '/' ? '\\' : path[index];
	if (length == 2 && native[1] == ':')
		strcpy(native + 2, "\\");
	return 1;
}

static int errno_from_windows_error(DWORD error)
{
	switch (error)
	{
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
	case ERROR_INVALID_DRIVE:
	case ERROR_INVALID_NAME:
		return ENOENT;
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
	case ERROR_WRITE_PROTECT:
		return EACCES;
	case ERROR_ALREADY_EXISTS:
	case ERROR_FILE_EXISTS:
		return EEXIST;
	case ERROR_DISK_FULL:
		return ENOSPC;
	case ERROR_DIR_NOT_EMPTY:
		return ENOTEMPTY;
	case ERROR_NOT_ENOUGH_MEMORY:
	case ERROR_OUTOFMEMORY:
		return ENOMEM;
	case ERROR_INVALID_HANDLE:
		return EBADF;
	default:
		return EIO;
	}
}

static int fail(void)
{
	errno = errno_from_windows_error(GetLastError());
	return -1;
}

static void unix_time(const FILETIME *time, posix_ulong *seconds, posix_ulong *nanoseconds)
{
	unsigned long long intervals = ((unsigned long long)time->dwHighDateTime << 32) | time->dwLowDateTime;

	if (intervals < FILETIME_UNIX_EPOCH * 10000000ULL)
	{
		*seconds = 0;
		*nanoseconds = 0;
		return;
	}
	intervals -= FILETIME_UNIX_EPOCH * 10000000ULL;
	*seconds = (posix_ulong)(intervals / 10000000ULL);
	*nanoseconds = (posix_ulong)(intervals % 10000000ULL) * 100;
}

static FILETIME file_time(posix_ulong seconds, posix_ulong nanoseconds)
{
	unsigned long long intervals = (seconds + FILETIME_UNIX_EPOCH) * 10000000ULL + nanoseconds / 100;
	FILETIME time;

	time.dwLowDateTime = (DWORD)intervals;
	time.dwHighDateTime = (DWORD)(intervals >> 32);
	return time;
}

/* ---------- descriptors */

static HANDLE descriptors[MAXIMUM_DESCRIPTORS];
static CRITICAL_SECTION descriptors_lock;

__attribute__((constructor))
static void descriptors_initialize(void)
{
	InitializeCriticalSection(&descriptors_lock);
}

static HANDLE descriptor_handle(int descriptor)
{
	HANDLE handle = descriptor >= 0 && descriptor < MAXIMUM_DESCRIPTORS ? descriptors[descriptor] : NULL;

	if (!handle)
		errno = EBADF;
	return handle;
}

int open(const char *path, int flags, ...)
{
	char native[MAX_PATH];
	DWORD access, disposition;
	HANDLE handle;
	int descriptor;

	if (!native_path(path, native, sizeof(native)))
		return -1;
	switch (flags & O_ACCMODE)
	{
	case O_WRONLY: access = GENERIC_WRITE; break;
	case O_RDWR: access = GENERIC_READ | GENERIC_WRITE; break;
	default: access = GENERIC_READ; break;
	}
	if ((flags & O_CREAT) && (flags & O_EXCL))
		disposition = CREATE_NEW;
	else if ((flags & O_CREAT) && (flags & O_TRUNC))
		disposition = CREATE_ALWAYS;
	else if (flags & O_CREAT)
		disposition = OPEN_ALWAYS;
	else if (flags & O_TRUNC)
		disposition = TRUNCATE_EXISTING;
	else
		disposition = OPEN_EXISTING;
	handle = CreateFileA(native, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, disposition,
		FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE)
		return fail();
	if (flags & O_APPEND)
		SetFilePointer(handle, 0, NULL, FILE_END);

	EnterCriticalSection(&descriptors_lock);
	/* (0 to 2 are the standard streams', which pdclib keeps itself) */
	for (descriptor = 3; descriptor < MAXIMUM_DESCRIPTORS && descriptors[descriptor]; descriptor++)
		;
	if (descriptor < MAXIMUM_DESCRIPTORS)
		descriptors[descriptor] = handle;
	LeaveCriticalSection(&descriptors_lock);
	if (descriptor == MAXIMUM_DESCRIPTORS)
	{
		CloseHandle(handle);
		errno = EMFILE;
		return -1;
	}
	return descriptor;
}

int close(int descriptor)
{
	HANDLE handle = descriptor_handle(descriptor);

	if (!handle)
		return -1;
	EnterCriticalSection(&descriptors_lock);
	descriptors[descriptor] = NULL;
	LeaveCriticalSection(&descriptors_lock);
	return CloseHandle(handle) ? 0 : fail();
}

ssize_t read(int descriptor, void *buffer, size_t count)
{
	HANDLE handle = descriptor_handle(descriptor);
	DWORD transferred;

	if (!handle)
		return -1;
	if (!ReadFile(handle, buffer, (DWORD)count, &transferred, NULL))
		return GetLastError() == ERROR_HANDLE_EOF ? 0 : fail();
	return (ssize_t)transferred;
}

ssize_t write(int descriptor, const void *buffer, size_t count)
{
	HANDLE handle = descriptor_handle(descriptor);
	DWORD transferred;

	if (!handle)
		return -1;
	if (!WriteFile(handle, buffer, (DWORD)count, &transferred, NULL))
		return fail();
	return (ssize_t)transferred;
}

long lseek(int descriptor, long offset, int whence)
{
	posix_ulong low, high;

	if (posix_seek(descriptor, offset, offset < 0 ? -1 : 0, whence, &low, &high) != 0)
		return -1;
	return (long)low;
}

/* (as the Windows build's: the file's position moves too) */
static ssize_t positioned_transfer(int descriptor, void *buffer, size_t count, off_t offset, BOOL write)
{
	HANDLE handle = descriptor_handle(descriptor);
	IO_STATUS_BLOCK status;
	LARGE_INTEGER position;
	NTSTATUS result;

	if (!handle)
		return -1;
	position.QuadPart = offset;
	result = write ?
		NtWriteFile(handle, NULL, NULL, NULL, &status, buffer, (ULONG)count, &position) :
		NtReadFile(handle, NULL, NULL, NULL, &status, buffer, (ULONG)count, &position);
	if (result == STATUS_END_OF_FILE)
		return 0;
	if (!NT_SUCCESS(result))
	{
		errno = EIO;
		return -1;
	}
	return (ssize_t)status.Information;
}

ssize_t pread(int descriptor, void *buffer, size_t count, off_t offset)
{
	return positioned_transfer(descriptor, buffer, count, offset, FALSE);
}

ssize_t pwrite(int descriptor, const void *buffer, size_t count, off_t offset)
{
	return positioned_transfer(descriptor, (void *)buffer, count, offset, TRUE);
}

int isatty(int descriptor)
{
	(void)descriptor;
	return 0;
}

/* ---------- streams: pdclib's have no descriptors */

FILE *fdopen(int descriptor, const char *mode)
{
	(void)descriptor;
	(void)mode;
	errno = EBADF;
	return NULL;
}

int fileno(FILE *stream)
{
	if (stream == stdin)
		return STDIN_FILENO;
	if (stream == stdout)
		return STDOUT_FILENO;
	if (stream == stderr)
		return STDERR_FILENO;
	errno = EBADF;
	return -1;
}

/* ---------- names */

int unlink(const char *path)
{
	char native[MAX_PATH];

	if (!native_path(path, native, sizeof(native)))
		return -1;
	return DeleteFileA(native) ? 0 : fail();
}

int rmdir(const char *path)
{
	char native[MAX_PATH];

	if (!native_path(path, native, sizeof(native)))
		return -1;
	return RemoveDirectoryA(native) ? 0 : fail();
}

int access(const char *path, int mode)
{
	char native[MAX_PATH];
	DWORD attributes;

	if (!native_path(path, native, sizeof(native)))
		return -1;
	attributes = GetFileAttributesA(native);
	if (attributes == INVALID_FILE_ATTRIBUTES)
		return fail();
	if ((mode & W_OK) && (attributes & FILE_ATTRIBUTE_READONLY))
	{
		errno = EACCES;
		return -1;
	}
	return 0;
}

/* no working directory: the XBE's folder stands for it */
int chdir(const char *path)
{
	(void)path;
	errno = ENOSYS;
	return -1;
}

char *getcwd(char *buffer, size_t size)
{
	if (!buffer || size < sizeof("D:"))
	{
		errno = ERANGE;
		return NULL;
	}
	strcpy(buffer, "D:");
	return buffer;
}

ssize_t readlink(const char *path, char *buffer, size_t size)
{
	static const char executable[] = "D:/default.xbe";

	if (strcmp(path, "/proc/self/exe") != 0)
	{
		errno = EINVAL;
		return -1;
	}
	if (size < sizeof(executable) - 1)
	{
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(buffer, executable, sizeof(executable) - 1);
	return (ssize_t)(sizeof(executable) - 1);
}

/* ---------- the system */

long sysconf(int name)
{
	MM_STATISTICS statistics;

	switch (name)
	{
	case _SC_PAGESIZE:
		return 0x1000;
	case _SC_NPROCESSORS_ONLN:
		return 1;
	case _SC_PHYS_PAGES:
	case _SC_AVPHYS_PAGES:
		statistics.Length = sizeof(statistics);
		if (!NT_SUCCESS(MmQueryStatistics(&statistics)))
			return -1;
		return (long)(name == _SC_PHYS_PAGES ? statistics.TotalPhysicalPages : statistics.AvailablePages);
	default:
		errno = EINVAL;
		return -1;
	}
}

int pause(void)
{
	Sleep(INFINITE);
	return -1;
}

/* (pdclib declares localtime_s and gmtime_s, but nxdk's build of it has
only localtime and gmtime, whose result each copies under a lock) */
static CRITICAL_SECTION time_lock;

__attribute__((constructor))
static void time_lock_initialize(void)
{
	InitializeCriticalSection(&time_lock);
}

static struct tm *time_copy(struct tm *(*convert)(const time_t *), const time_t *timer, struct tm *result)
{
	struct tm *converted;

	EnterCriticalSection(&time_lock);
	converted = convert(timer);
	if (converted)
		*result = *converted;
	LeaveCriticalSection(&time_lock);
	return converted ? result : NULL;
}

struct tm *localtime_r(const time_t *timer, struct tm *result)
{
	return time_copy(localtime, timer, result);
}

struct tm *gmtime_r(const time_t *timer, struct tm *result)
{
	return time_copy(gmtime, timer, result);
}

/* ---------- random bytes

The Xbox has no entropy source the kernel gathers: a SHA-1 state (the
kernel's) seeded with the EEPROM, which differs between consoles (serial
number, keys), and fed the performance counter at every draw, as nxdk's
rand_s. Good for the menus' XML hash salt (expat); internet play's keys
(posix.h) want more (the plan's fourth phase). */
static unsigned char random_state[116];
static CRITICAL_SECTION random_lock;
/* 0, then 1 while the first caller sets the state up, then 2 */
static volatile LONG random_ready;

/* on first use, not in a constructor: nxdk's start-up draws its stack
protection cookie (rand_s) before any constructor runs */
static void random_initialize(void)
{
	unsigned char eeprom[256];
	ULONG read = 0, type;

	if (random_ready == 2)
		return;
	if (InterlockedCompareExchange(&random_ready, 1, 0) != 0)
	{
		while (random_ready != 2)
			SwitchToThread();
		return;
	}
	InitializeCriticalSection(&random_lock);
	XcSHAInit(random_state);
	ExQueryNonVolatileSetting(0xFFFF, &type, eeprom, sizeof(eeprom), &read);
	XcSHAUpdate(random_state, eeprom, read);
	InterlockedExchange(&random_ready, 2);
}

void posix_random_bytes(void *buffer, posix_ulong size)
{
	unsigned char *bytes = buffer;

	random_initialize();
	EnterCriticalSection(&random_lock);
	while (size)
	{
		LARGE_INTEGER counter;
		unsigned char digest[20];
		posix_ulong count = size < sizeof(digest) ? size : sizeof(digest);

		QueryPerformanceCounter(&counter);
		XcSHAUpdate(random_state, (PUCHAR)&counter, sizeof(counter));
		XcSHAFinal(random_state, digest);
		memcpy(bytes, digest, count);
		/* (the output fed back, as nxdk's rand_s) */
		XcSHAUpdate(random_state, digest, sizeof(digest));
		bytes += count;
		size -= count;
	}
	LeaveCriticalSection(&random_lock);
}

/* Microsoft's, for expat's hash salt (as on Windows); nxdk's own sits
beside its strtod, which would then be linked over nxdk_libc.c's */
int rand_s(unsigned int *value)
{
	if (!value)
		return EINVAL;
	posix_random_bytes(value, sizeof(*value));
	return 0;
}

/* pdclib's allocator is dlmalloc, which knows a block's size */
size_t dlmalloc_usable_size(void *pointer);

size_t malloc_usable_size(void *pointer)
{
	return dlmalloc_usable_size(pointer);
}

/* ---------- posix.h: files */

static void information_from_attributes(const WIN32_FILE_ATTRIBUTE_DATA *data, struct posix_file_information *information)
{
	memset(information, 0, sizeof(*information));
	if (data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		information->flags |= _posix_file_is_directory;
	if (data->dwFileAttributes & FILE_ATTRIBUTE_READONLY)
		information->flags |= _posix_file_is_read_only;
	information->size_low = data->nFileSizeLow;
	information->size_high = data->nFileSizeHigh;
	unix_time(&data->ftLastWriteTime, &information->modification_seconds, &information->modification_nanoseconds);
	unix_time(&data->ftLastAccessTime, &information->access_seconds, &information->access_nanoseconds);
	unix_time(&data->ftCreationTime, &information->creation_seconds, &information->creation_nanoseconds);
}

int posix_stat(const char *path, struct posix_file_information *information)
{
	char native[MAX_PATH];
	WIN32_FILE_ATTRIBUTE_DATA data;

	if (!native_path(path, native, sizeof(native)))
		return -1;
	if (!GetFileAttributesExA(native, GetFileExInfoStandard, &data))
		return fail();
	information_from_attributes(&data, information);
	return 0;
}

int posix_fstat(int descriptor, struct posix_file_information *information)
{
	HANDLE handle = descriptor_handle(descriptor);
	BY_HANDLE_FILE_INFORMATION data;
	WIN32_FILE_ATTRIBUTE_DATA attributes;

	if (!handle)
		return -1;
	if (!GetFileInformationByHandle(handle, &data))
		return fail();
	attributes.dwFileAttributes = data.dwFileAttributes;
	attributes.ftCreationTime = data.ftCreationTime;
	attributes.ftLastAccessTime = data.ftLastAccessTime;
	attributes.ftLastWriteTime = data.ftLastWriteTime;
	attributes.nFileSizeHigh = data.nFileSizeHigh;
	attributes.nFileSizeLow = data.nFileSizeLow;
	information_from_attributes(&attributes, information);
	return 0;
}

int posix_set_file_times(const char *path,
	posix_ulong access_seconds, posix_ulong access_nanoseconds,
	posix_ulong modification_seconds, posix_ulong modification_nanoseconds)
{
	char native[MAX_PATH];
	FILETIME access_time = file_time(access_seconds, access_nanoseconds);
	FILETIME modification_time = file_time(modification_seconds, modification_nanoseconds);
	HANDLE handle;
	BOOL result;

	if (!native_path(path, native, sizeof(native)))
		return -1;
	handle = CreateFileA(native, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE)
		return fail();
	result = SetFileTime(handle, NULL, access_seconds ? &access_time : NULL,
		modification_seconds ? &modification_time : NULL);
	CloseHandle(handle);
	return result ? 0 : fail();
}

int posix_seek(int descriptor, posix_long offset_low, posix_long offset_high, int whence,
	posix_ulong *position_low, posix_ulong *position_high)
{
	HANDLE handle = descriptor_handle(descriptor);
	LARGE_INTEGER distance, position;
	DWORD method = whence == SEEK_CUR ? FILE_CURRENT : whence == SEEK_END ? FILE_END : FILE_BEGIN;

	if (!handle)
		return -1;
	distance.LowPart = (DWORD)offset_low;
	distance.HighPart = offset_high;
	if (!SetFilePointerEx(handle, distance, &position, method))
		return fail();
	if (position_low)
		*position_low = position.LowPart;
	if (position_high)
		*position_high = (posix_ulong)position.HighPart;
	return 0;
}

int posix_truncate(int descriptor, posix_ulong size_low, posix_ulong size_high)
{
	HANDLE handle = descriptor_handle(descriptor);
	LARGE_INTEGER size, previous, zero;

	if (!handle)
		return -1;
	zero.QuadPart = 0;
	if (!SetFilePointerEx(handle, zero, &previous, FILE_CURRENT))
		return fail();
	size.LowPart = size_low;
	size.HighPart = (LONG)size_high;
	if (!SetFilePointerEx(handle, size, NULL, FILE_BEGIN) || !SetEndOfFile(handle))
		return fail();
	SetFilePointerEx(handle, previous, NULL, FILE_BEGIN);
	return 0;
}

int posix_disk_space(const char *path,
	posix_ulong *free_low, posix_ulong *free_high,
	posix_ulong *total_low, posix_ulong *total_high)
{
	char root[4] = "D:\\";
	ULARGE_INTEGER available, total, free_bytes;

	if (path[0] && path[1] == ':')
		root[0] = path[0];
	if (!GetDiskFreeSpaceExA(root, &available, &total, &free_bytes))
		return fail();
	*free_low = available.LowPart;
	*free_high = available.HighPart;
	*total_low = total.LowPart;
	*total_high = total.HighPart;
	return 0;
}

int posix_set_read_only(const char *path, int read_only)
{
	char native[MAX_PATH];
	DWORD attributes;

	if (!native_path(path, native, sizeof(native)))
		return -1;
	attributes = GetFileAttributesA(native);
	if (attributes == INVALID_FILE_ATTRIBUTES)
		return fail();
	attributes = read_only ? attributes | FILE_ATTRIBUTE_READONLY : attributes & ~FILE_ATTRIBUTE_READONLY;
	return SetFileAttributesA(native, attributes) ? 0 : fail();
}

int posix_make_directory(const char *path)
{
	char native[MAX_PATH];

	if (!native_path(path, native, sizeof(native)))
		return -1;
	return CreateDirectoryA(native, NULL) ? 0 : fail();
}

/* ---------- posix.h: directories */

struct directory
{
	HANDLE find;
	WIN32_FIND_DATAA data;
	/* the entry FindFirstFile gave, not yet returned */
	BOOL pending;
};

void *posix_directory_open(const char *path)
{
	char pattern[MAX_PATH];
	size_t length;
	struct directory *directory;

	if (!native_path(path, pattern, sizeof(pattern) - 2))
		return NULL;
	length = strlen(pattern);
	if (length && pattern[length - 1] != '\\')
		strcat(pattern, "\\");
	strcat(pattern, "*");
	directory = calloc(1, sizeof(*directory));
	if (!directory)
		return NULL;
	directory->find = FindFirstFileA(pattern, &directory->data);
	if (directory->find == INVALID_HANDLE_VALUE)
	{
		/* an empty folder has no entries at all */
		if (GetLastError() == ERROR_FILE_NOT_FOUND)
			return directory;
		free(directory);
		fail();
		return NULL;
	}
	directory->pending = TRUE;
	return directory;
}

int posix_directory_next(void *handle, char *name, posix_ulong name_size)
{
	struct directory *directory = handle;

	if (directory->find == INVALID_HANDLE_VALUE || !directory->find)
		return 0;
	for (;;)
	{
		if (!directory->pending && !FindNextFileA(directory->find, &directory->data))
			return 0;
		directory->pending = FALSE;
		if (!strcmp(directory->data.cFileName, ".") || !strcmp(directory->data.cFileName, ".."))
			continue;
		strncpy(name, directory->data.cFileName, name_size - 1);
		name[name_size - 1] = '\0';
		return 1;
	}
}

void posix_directory_close(void *handle)
{
	struct directory *directory = handle;

	if (directory->find && directory->find != INVALID_HANDLE_VALUE)
		FindClose(directory->find);
	free(directory);
}

/* the file systems ignore case: the name is its own on-disk spelling */
int posix_find_entry_case_insensitive(const char *directory, const char *name,
	char *result, posix_ulong result_size)
{
	char path[MAX_PATH];
	char native[MAX_PATH];

	if ((size_t)snprintf(path, sizeof(path), "%s/%s", directory, name) >= sizeof(path) ||
		!native_path(path, native, sizeof(native)) ||
		GetFileAttributesA(native) == INVALID_FILE_ATTRIBUTES)
	{
		return 0;
	}
	strncpy(result, name, result_size - 1);
	result[result_size - 1] = '\0';
	return 1;
}
