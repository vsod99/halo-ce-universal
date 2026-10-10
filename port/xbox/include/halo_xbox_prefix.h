/*
HALO_XBOX_PREFIX.H

Force-included ahead of every game unit and every Xbox-facing platform unit
in the original Xbox build (clang -include; tools/xbox_build.py). The units
that talk to nxdk itself (port/xbox/src/nxdk_*.c) never see it.

nxdk's target (i386-pc-win32) is clang's Microsoft target, as the Windows
build's is, so this follows halo_windows_prefix.h. The C library is nxdk's
pdclib instead of Microsoft's, so the game gets the Linux build's C runtime
wrappers (port/linux/include), which add the MSVC names to a C library
without them.
*/

#ifndef __HALO_XBOX_PREFIX_H
#define __HALO_XBOX_PREFIX_H

#if !defined(_M_IX86)
#error the Xbox port targets 32-bit x86
#endif

#define HALO_XBOX 1

/* ---------- XDK architecture selection */

#define _X86_ 1
#define _STDCALL_SUPPORTED 1
#define _USE_MATH_DEFINES

/* ---------- the C library's C99 spellings in gnu89 units */

#define restrict __restrict

/* ---------- MSVC inline semantics

As on Windows: clang's Microsoft target gives C `__inline` functions MSVC's
COMDAT linkage, and the build supplies the copies MSVC would have left
(port/linux/game/msvc_comdat.c, tools/windows_build.py's wrappers). */

/* ---------- MSVC intrinsics (as halo_windows_prefix.h) */

#define _InterlockedCompareExchange halo_linux_InterlockedCompareExchange
#define _InterlockedDecrement halo_linux_InterlockedDecrement
#define _InterlockedExchange halo_linux_InterlockedExchange
#define _InterlockedExchangeAdd halo_linux_InterlockedExchangeAdd
#define _InterlockedIncrement halo_linux_InterlockedIncrement

/* ---------- the game's x87 conversions

The game's FISTP conversions are `(long)__builtin_rint(x)` in its sources
(cseries.h's fast_ftol, bitmaps_inlines.h's colors, decals.c). clang calls
pdclib's rint (an FRNDINT) and truncates with two control word changes; here
they are one FISTP in the current rounding mode, as the Xbox game's were:
the same integer, and the conversion back to double folds away. Every use is
cast to an integer at once. (b30's particles' sprites, mostly their colors:
6.6% of the processor time in xemu before, 2.1% after.) */

static __inline__ long halo_xbox_fistp(double value)
{
	long result;

	__asm__ ("fistpl %0" : "=m"(result) : "t"(value) : "st");
	return result;
}

#define __builtin_rint(value) ((double)halo_xbox_fistp(value))

/* ---------- structured exception handling (as halo_windows_prefix.h) */

#define __try if (1)
#define __except(filter) else if (0)
#define __finally
#define __leave

/* ---------- multiplayer session limits of the native builds */

#include "../../linux/include/halo_port_limits.h"

#define FD_SETSIZE HALO_PORT_FD_SETSIZE

/* ---------- Xbox functions named like Windows functions

nxdk's winapi defines CreateFileA, Sleep and the rest for the Xbox-facing
platform code's own use (port/xbox/src); the game and the Linux build's
implementations of them see other names, as on Windows. */

#include "../../windows/include/halo_windows_api_names.h"

/* and the kernel's critical sections, which nxdk's kernel library exports
under the same names */
#define RtlEnterCriticalSection halo_xbox_RtlEnterCriticalSection
#define RtlInitializeCriticalSection halo_xbox_RtlInitializeCriticalSection
#define RtlLeaveCriticalSection halo_xbox_RtlLeaveCriticalSection
#define RtlTryEnterCriticalSection halo_xbox_RtlTryEnterCriticalSection

/* ---------- Winsock and source fixups shared with the Linux build */

#ifndef HALO_LINUX_PLATFORM_LAYER
#include "../../linux/include/halo_linux_winsock_names.h"
#include "../../linux/include/halo_linux_source_fixups.h"
#endif

#include <stddef.h>

#endif /* __HALO_XBOX_PREFIX_H */
