# musl-stdio

`printf`'s formatting and `strtod`, from musl libc, MIT licensed (see
`COPYRIGHT`), for a C library without them: nxdk's pdclib, the original
Xbox port's (`port/xbox`), formats no floating point numbers (`%f`, `%e`,
`%g` print nothing) and has no `strtod` or `atof`.

Upstream: https://musl.libc.org, release 1.2.5 (as `musl-math`). The files
in `src/` are musl's, copied unchanged: `stdio/vfprintf.c`,
`stdio/vsnprintf.c`, `stdio/__towrite.c`, `stdio/fwrite.c` (for
`__fwritex`), `internal/floatscan.c`, `internal/shgetc.c` and `.h`,
`internal/floatscan.h`, `stdlib/strtod.c`.

The game's scripts give their real numbers as text (`hs_compile.c` reads
them with `atof`), and every machine in a game must read the same values:
musl's `strtod` rounds correctly, as glibc's and the Windows UCRT's do.

`include/` holds this port's stand-ins for musl's internal headers and for
the C library headers the files include: musl's `FILE` for strings only (no
locks, no files), and `long double` as `double` (the Microsoft ABI's).
`musl_stdio.h`, force-included ahead of each file, gives every name the
files define or call a `halo_musl_` prefix, so they sit beside the
platform's C library; `support.c` has the few functions they call that are
not among them. The platform layer decides what to call them for
(`port/xbox/src/nxdk_libc.c`).
