/*
WIN32_POSIX.C

The POSIX calls the platform layer shared with Linux makes, implemented with
Windows (the headers are in port/windows/include/posix): sysconf, positioned
file transfers, and the memory mapping the Xbox memory window uses (threads,
mutexes, condition variables and clocks are in win32_threads.c). Also the
process start-up the Windows build needs.
*/

#include <windows.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <timeapi.h>

#include "sys/mman.h"
#include "time.h"
#include "unistd.h"

/* ---------- start-up */

/* laptops with a second, faster GPU (NVIDIA Optimus, AMD switchable
graphics) run a program on the integrated one unless it exports these */
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

/* win32_crash.c's: whether this process is a crash reporter, which leaves
halo.log to the game it reports */
int crash_reporter_process(void);

__attribute__((constructor))
static void windows_startup(void)
{
	/* files are binary unless opened otherwise, as on the Xbox (the C
	runtime's default text mode would translate line ends) */
	_set_fmode(_O_BINARY);
	/* Sleep() waits in 1 ms steps rather than the default 15.6 ms: the
	game's frame pacing sleeps for short intervals */
	timeBeginPeriod(1);
	/* a release build has no console (/SUBSYSTEM:WINDOWS), so the platform
	layer's log (platform_log's stderr: invite links, the data root, crash
	reports) goes to halo.log next to halo.exe, unless stderr already goes
	somewhere (a pipe or a file the game was started with) */
	if (_fileno(stderr) < 0 && !crash_reporter_process())
	{
		char path[MAX_PATH];
		DWORD length = GetModuleFileNameA(NULL, path, sizeof(path));
		char *slash;

		if (length && length < sizeof(path) && (slash = strrchr(path, '\\')) &&
			(size_t)(slash + 1 - path) + sizeof("halo.log") <= sizeof(path))
		{
			strcpy(slash + 1, "halo.log");
			if (freopen(path, "w", stderr))
				setvbuf(stderr, NULL, _IONBF, 0);
		}
	}
}

static int errno_from_windows_error(DWORD error)
{
	switch (error)
	{
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
	case ERROR_INVALID_DRIVE:
		return ENOENT;
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION:
		return EACCES;
	case ERROR_ALREADY_EXISTS:
	case ERROR_FILE_EXISTS:
		return EEXIST;
	case ERROR_NOT_ENOUGH_MEMORY:
	case ERROR_OUTOFMEMORY:
	case ERROR_COMMITMENT_LIMIT:
		return ENOMEM;
	case ERROR_INVALID_ADDRESS:
	case ERROR_INVALID_PARAMETER:
		return EINVAL;
	default:
		return EIO;
	}
}

long sysconf(int name)
{
	SYSTEM_INFO system;
	MEMORYSTATUSEX memory;

	GetSystemInfo(&system);
	switch (name)
	{
	case _SC_PAGESIZE:
		return (long)system.dwPageSize;
	case _SC_NPROCESSORS_ONLN:
		return (long)system.dwNumberOfProcessors;
	case _SC_PHYS_PAGES:
	case _SC_AVPHYS_PAGES:
		memory.dwLength = sizeof(memory);
		if (!GlobalMemoryStatusEx(&memory))
			return -1;
		return (long)((name == _SC_PHYS_PAGES ? memory.ullTotalPhys : memory.ullAvailPhys) / system.dwPageSize);
	default:
		errno = EINVAL;
		return -1;
	}
}

/* ---------- files */

static ssize_t positioned_transfer(int descriptor, void *buffer, size_t count, off_t offset, BOOL write)
{
	HANDLE handle = (HANDLE)_get_osfhandle(descriptor);
	OVERLAPPED overlapped;
	DWORD transferred = 0;
	BOOL result;

	if (handle == INVALID_HANDLE_VALUE)
	{
		errno = EBADF;
		return -1;
	}
	memset(&overlapped, 0, sizeof(overlapped));
	overlapped.Offset = (DWORD)offset;
	overlapped.OffsetHigh = (DWORD)((unsigned long long)offset >> 32);
	result = write ?
		WriteFile(handle, buffer, (DWORD)count, &transferred, &overlapped) :
		ReadFile(handle, buffer, (DWORD)count, &transferred, &overlapped);
	if (!result)
	{
		if (!write && GetLastError() == ERROR_HANDLE_EOF)
			return 0;
		errno = errno_from_windows_error(GetLastError());
		return -1;
	}
	return (ssize_t)transferred;
}

ssize_t pread(int descriptor, void *buffer, size_t count, off_t offset)
{
	return positioned_transfer(descriptor, buffer, count, offset, FALSE);
}

ssize_t pwrite(int descriptor, const void *buffer, size_t count, off_t offset)
{
	return positioned_transfer(descriptor, (void *)buffer, count, offset, TRUE);
}

ssize_t readlink(const char *path, char *buffer, size_t size)
{
	DWORD length;
	DWORD index;

	if (strcmp(path, "/proc/self/exe") != 0)
	{
		errno = EINVAL;
		return -1;
	}
	length = GetModuleFileNameA(NULL, buffer, (DWORD)size);
	if (!length || length >= size)
	{
		errno = ENAMETOOLONG;
		return -1;
	}
	for (index = 0; index < length; index++)
	{
		if (buffer[index] == '\\')
			buffer[index] = '/';
	}
	return (ssize_t)length;
}

int pause(void)
{
	Sleep(INFINITE);
	return -1;
}

/* ---------- memory mapping */

static DWORD windows_protection(int protection)
{
	switch (protection & (PROT_READ | PROT_WRITE | PROT_EXEC))
	{
	case PROT_NONE: return PAGE_NOACCESS;
	case PROT_READ: return PAGE_READONLY;
	case PROT_EXEC: return PAGE_EXECUTE;
	case PROT_READ | PROT_EXEC: return PAGE_EXECUTE_READ;
	case PROT_READ | PROT_WRITE | PROT_EXEC:
	case PROT_WRITE | PROT_EXEC: return PAGE_EXECUTE_READWRITE;
	default: return PAGE_READWRITE;
	}
}

static BOOL address_reserved(void *address)
{
	MEMORY_BASIC_INFORMATION information;

	return address && VirtualQuery(address, &information, sizeof(information)) &&
		information.State != MEM_FREE;
}

void *mmap(void *address, size_t length, int protection, int flags, int descriptor, long offset)
{
	void *result;

	(void)descriptor;
	(void)offset;
	if (!(flags & MAP_ANONYMOUS))
	{
		errno = ENODEV;
		return MAP_FAILED;
	}
	if (protection == PROT_NONE && (flags & MAP_NORESERVE))
	{
		if ((flags & MAP_FIXED) && address_reserved(address))
		{
			/* give pages inside a reservation back */
			if (!VirtualFree(address, length, MEM_DECOMMIT))
			{
				errno = errno_from_windows_error(GetLastError());
				return MAP_FAILED;
			}
			return address;
		}
		result = VirtualAlloc(address, length, MEM_RESERVE, PAGE_NOACCESS);
	}
	else if ((flags & MAP_FIXED) && address_reserved(address))
	{
		/* fresh zeroed pages inside a reservation: committing pages that
		are already committed would keep their contents */
		VirtualFree(address, length, MEM_DECOMMIT);
		result = VirtualAlloc(address, length, MEM_COMMIT, windows_protection(protection));
	}
	else
	{
		result = VirtualAlloc(address, length, MEM_RESERVE | MEM_COMMIT, windows_protection(protection));
	}
	if (!result)
	{
		errno = errno_from_windows_error(GetLastError());
		return MAP_FAILED;
	}
	return result;
}

int munmap(void *address, size_t length)
{
	MEMORY_BASIC_INFORMATION information;

	if (!VirtualQuery(address, &information, sizeof(information)))
	{
		errno = EINVAL;
		return -1;
	}
	if (information.AllocationBase == address &&
		!VirtualFree(address, 0, MEM_RELEASE))
	{
		errno = errno_from_windows_error(GetLastError());
		return -1;
	}
	if (information.AllocationBase != address &&
		!VirtualFree(address, length, MEM_DECOMMIT))
	{
		errno = errno_from_windows_error(GetLastError());
		return -1;
	}
	return 0;
}

int mprotect(void *address, size_t length, int protection)
{
	DWORD previous;

	if (!VirtualProtect(address, length, windows_protection(protection), &previous))
	{
		errno = errno_from_windows_error(GetLastError());
		return -1;
	}
	return 0;
}
