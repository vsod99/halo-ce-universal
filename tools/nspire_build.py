"""Ninja rules for the TI-Nspire CX II build (``ninja nspire``).

The Nspire port (port/nspire/README.md) runs one campaign level on the
calculator's ARM926EJ-S under Ndless. clang compiles the game for ARMv5TE
with the soft-float EABI and the MSVC semantics the other ports use; the
Ndless toolchain (GCC's binutils, newlib and libgcc) links it with the
platform layer in port/nspire/src, and genzehn and make-prg turn the ELF
file into build/nspire/halo.tns.
"""

import os
from pathlib import Path
from typing import Any, List

from .linux_build import XDK_INCLUDE, compile_launcher, game_defines_and_includes, xdk_headers
from .ninja_syntax import Writer

PORT_DIR = Path("port/nspire")
LINUX_DIR = Path("port/linux")
PORT_CONFIG = PORT_DIR / "port.json"
BUILD = Path("build/nspire")
MUSL_MATH_DIR = Path("port/third_party/musl-math")
DEFAULT_NDLESS_SDK = Path("../Ndless/ndless-sdk")

# The calculator's ABI: ARMv5TE code in ARM state, soft float. The rest
# reproduces the MSVC/Xbox environment the source was written for, as on the
# other ports (tools/linux_build.py), plus two ARM defaults MSVC differs
# from: char is signed, and every enum is an int.
NSPIRE_ABI_FLAGS = [
    "--target=armv5te-none-eabi",
    "-mcpu=arm926ej-s",
    "-marm",
    "-mfloat-abi=soft",
    "-fsigned-char",
    "-fno-short-enums",
    "-fshort-wchar",
    "-ffunction-sections",
    "-fdata-sections",
    "-ffp-contract=off",
    "-fno-omit-frame-pointer",
    # __thread variables through __emutls_get_address, which the port's
    # cooperative threads implement (port/nspire/src/nspire_threads.c)
    "-femulated-tls",
    # MSVC's wint_t is an unsigned short, as port/linux/include/wchar.h has
    # it; clang's own headers would otherwise make it an int first
    "-U__WINT_TYPE__",
    "'-D__WINT_TYPE__=unsigned short'",
    "-DHALO_NSPIRE=1",
    "-D_TINSPIRE=1",
    "-O2",
    "-g",
]

# the renderer's units and the maths library, where the frames' time goes,
# are built for speed: -O3, and no frame pointer (the game's own code keeps
# it: interface/hud_draw.c reads its caller's caller's return address)
SPEED_SOURCES = {"soft_rasterizer.c", "soft_vertex.c", "soft_textures.c"}
# the game's own code where its ticks go (objects, units, their physics and
# AI, models' animation, the maths) at -O3 too
GAME_SPEED_DIRECTORIES = {("source", name) for name in
                          ("math", "physics", "models", "units", "ai", "objects", "structures")}
SPEED_FLAGS = ["-O3", "-fomit-frame-pointer"]

GAME_CODE_FLAGS = [
    "-fms-extensions",
    "-fcommon",
    "-fno-strict-aliasing",
    "-fwrapv",
    "-fno-delete-null-pointer-checks",
    *(f"-fno-builtin-{name}" for name in (
        "wcslen", "wcsnlen", "wcschr", "wcsrchr", "wcscmp", "wcsncmp", "wcscpy",
        "wcsncpy", "wcscat", "wcsncat", "wmemchr", "wmemcmp", "wmemcpy",
        "wmemmove", "wmemset",
    )),
]

GAME_FLAGS = [
    "-std=gnu89",
    "-D__STRICT_ANSI__",
    "-w",
    "-Wno-error=incompatible-pointer-types",
    "-Wno-error=incompatible-function-pointer-types",
    "-Wno-error=int-conversion",
    "-Wno-error=implicit-function-declaration",
    "-Wno-error=implicit-int",
    "-Wno-error=return-type",
]

PLATFORM_FLAGS = [
    "-std=gnu11",
    # newlib declares clock_gettime, CLOCK_MONOTONIC and the pthread
    # functions only for systems that have them; port/nspire/src supplies
    # them (nspire_time.c, nspire_threads.c)
    "-D_POSIX_TIMERS=1",
    "-D_POSIX_MONOTONIC_CLOCK=1",
    "-D_POSIX_THREADS=1",
    "-D_POSIX_TIMEOUTS=1",
    "-D_UNIX98_THREAD_MUTEX_ATTRIBUTES=1",
    "-DHALO_LINUX_PLATFORM_LAYER",
    "-Wall",
    "-Wno-unused-function",
    "-Wno-unknown-pragmas",
    "-Wno-microsoft-anon-tag",
    "-Wno-pragma-pack",
    "-Wno-ignored-attributes",
    "-Wno-duplicate-decl-specifier",
    "-Wno-missing-braces",
    "-Wno-unused-variable",
    "-Wno-ignored-pragmas",
]


def _load_config() -> Any:
    import json
    with open(PORT_CONFIG, encoding="utf-8") as f:
        return json.load(f)


def _quote(path: Any) -> str:
    text = str(path).replace(os.sep, "/")
    return f'"{text}"' if " " in text else text


def nspire_configure_inputs() -> List[Path]:
    return [Path("tools/nspire_build.py"), PORT_CONFIG] if PORT_CONFIG.is_file() else []


def ndless_sdk(sln: Any) -> Path:
    sdk = getattr(sln, "ndless_sdk", None) or os.environ.get("NDLESS_SDK") or DEFAULT_NDLESS_SDK
    return Path(sdk).resolve()


def game_sources(config: Any) -> List[Path]:
    """the game's C sources (port.json "game"): every one under its root but
    those excluded, which the Nspire replaces or does without"""
    game = config["game"]
    excluded = set(game.get("exclude", []))
    excluded_dirs = tuple(directory.rstrip("/") + "/" for directory in game.get("exclude_dirs", []))
    return sorted(
        source for source in Path(game["root"]).rglob("*.c")
        if source.as_posix() not in excluded and not source.as_posix().startswith(excluded_dirs)
    )


def generate_nspire_build(n: Writer, sln: Any) -> None:
    if not PORT_CONFIG.is_file():
        return
    config = _load_config()
    sdk = ndless_sdk(sln)
    toolchain = sdk / "toolchain" / "install"
    newlib_include = toolchain / "arm-none-eabi" / "include"
    obj_dir = BUILD / "obj"
    elf = BUILD / "halo.elf"
    tns = BUILD / "halo.tns"
    cc = getattr(sln, "nspire_cc", None) or "clang"
    prefix_header = LINUX_DIR / "include" / "halo_linux_prefix.h"
    semantics_header = BUILD / "halo_msvc_semantics.h"
    platform_semantics_header = BUILD / "platform_msvc_semantics.h"

    n.comment("TI-Nspire CX II build (ninja nspire)")
    n.variable("nspire_cc", cc)
    n.variable("nspire_bin", _quote(sdk / "bin"))
    n.variable("nspire_toolchain_bin", _quote(toolchain / "bin"))

    n.rule(
        name="nspire_msvc_semantics",
        command="$python tools/linux_msvc_semantics.py --output $out $scan",
        description="NSPIRE MSVC SEMANTICS $out",
        restat=True,
    )
    game_headers = sorted(p for p in Path("source").rglob("*") if p.suffix in (".c", ".h"))
    n.build(
        outputs=semantics_header,
        rule="nspire_msvc_semantics",
        implicit=[Path("tools/linux_msvc_semantics.py"), *xdk_headers(), *game_headers],
        variables={"scan": f"--all-inlines --tags source --inlines source --inlines {XDK_INCLUDE}"},
    )
    n.build(
        outputs=platform_semantics_header,
        rule="nspire_msvc_semantics",
        implicit=[Path("tools/linux_msvc_semantics.py"), *xdk_headers()],
        variables={"scan": f"--inlines {XDK_INCLUDE}"},
    )

    n.rule(
        name="nspire_cc",
        command=f"{compile_launcher(sln)}$nspire_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description="NSPIRE CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    # The link is arm-none-eabi-ld with the arguments Ndless's nspire-ld
    # would give it (its crt files, linker script and libraries), called
    # directly: through the GCC driver, a command line this long reaches
    # Ndless's ld wrapper as a response file it cannot filter.
    gcc_lib = sorted((toolchain / "lib" / "gcc" / "arm-none-eabi").glob("*"))
    libgcc = (gcc_lib[-1] if gcc_lib else toolchain / "lib" / "gcc" / "arm-none-eabi" / "14.2.0") / "libgcc.a"
    system = sdk / "system"
    n.variable("nspire_libs", " ".join([
        f"-L{_quote(sdk / 'lib')}", f"-L{_quote(toolchain / 'arm-none-eabi' / 'lib')}",
        "--start-group", "-lSDL", "-lndls", "-lsyscalls", "-lm", "-lc", "--end-group",
        _quote(libgcc), _quote(system / "crtn.o"),
    ]))
    n.rule(
        name="nspire_link",
        command=("make -C " + _quote(system) + " -s all && "
                 "$nspire_toolchain_bin/arm-none-eabi-ld --pic-veneer --emit-relocs -static "
                 f"-T {_quote(system / 'ldscript')} {_quote(system / 'crt0.o')} {_quote(system / 'crti.o')} "
                 "--gc-sections --no-enum-size-warning --no-wchar-size-warning --no-warn-rwx-segments "
                 "--wrap=main --wrap=exit --wrap=malloc --wrap=calloc --wrap=realloc --wrap=free -Map $out.map -o $out @$out.rsp $nspire_libs"),
        description="NSPIRE LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.rule(
        name="nspire_tns",
        command=("export PATH=$nspire_bin:$nspire_toolchain_bin:$$PATH && "
                 "genzehn --input $in --output $out.zehn --name halo --compress --uses-lcd-blit true --240x320-support true "
                 "&& make-prg $out.zehn $out && rm $out.zehn"),
        description="NSPIRE TNS $out",
    )

    abi = " ".join(NSPIRE_ABI_FLAGS + (["-DHALO_RELEASE"] if getattr(sln, "port_release", False) else []))
    libc_includes = f"-isystem {_quote(newlib_include)}"
    implicit = [*xdk_headers(), prefix_header, semantics_header, platform_semantics_header]
    objects: List[Path] = []

    def add_object(source: Path, cflags: str) -> None:
        obj = obj_dir / Path(str(source).lstrip("/")).with_suffix(".o")
        objects.append(obj)
        n.build(outputs=obj, rule="nspire_cc", inputs=source, implicit=implicit, variables={"cflags": cflags})

    # the game; the Nspire's own include directory comes first, so its
    # headers (port capacities, say) stand in for the Linux port's
    game_cflags = " ".join([
        abi, " ".join(GAME_CODE_FLAGS), " ".join(GAME_FLAGS),
        f"-include {prefix_header}", f"-include {semantics_header}",
        f"-I{PORT_DIR}/include", f"-I{LINUX_DIR}/include", game_defines_and_includes(config),
        libc_includes, f"-idirafter {XDK_INCLUDE}",
    ])
    for source in game_sources(config):
        # (no frame pointer but in the interface's units: hud_draw.c's
        # get_return_eip reads its caller's return address through it; the
        # stack walk of assertions just stops sooner)
        frame = "" if Path(source).parts[:2] == ("source", "interface") else " -fomit-frame-pointer"
        if Path(source).parts[:2] in GAME_SPEED_DIRECTORIES:
            add_object(source, f"{game_cflags} -O3{frame}")
        else:
            add_object(source, f"{game_cflags}{frame}")
    for source in config.get("port_game_sources", []):
        add_object(Path(source), game_cflags)

    # the platform layer: the Nspire's own units, and the portable ones of
    # the Linux port
    platform_cflags = " ".join([
        abi, " ".join(GAME_CODE_FLAGS), " ".join(PLATFORM_FLAGS),
        f"-include {prefix_header}", f"-include {platform_semantics_header}",
        f"-I{PORT_DIR}/src", f"-I{PORT_DIR}/include", f"-I{LINUX_DIR}/src", f"-I{LINUX_DIR}/include",
        "-Iport/include", "-Isource -Isource/cseries -Isource/memory/zlib",
        libc_includes, f"-I{_quote(sdk / 'include')}", f"-idirafter {XDK_INCLUDE}",
    ])
    # posix_*.c see the C library's own headers, as on Linux (posix.h)
    posix_cflags = " ".join([
        abi, "-std=gnu11", "-Wall", "-D_POSIX_TIMERS=1", "-D_POSIX_MONOTONIC_CLOCK=1",
        f"-I{LINUX_DIR}/src", libc_includes, f"-I{_quote(sdk / 'include')}",
    ])
    for source in sorted((PORT_DIR / "src").glob("*.c")):
        if source.name.startswith("posix_"):
            add_object(source, posix_cflags)
        elif source.name in SPEED_SOURCES:
            add_object(source, f"{platform_cflags} {' '.join(SPEED_FLAGS)}")
        else:
            add_object(source, platform_cflags)
    for source in config.get("linux_platform_sources", []):
        add_object(Path(source), platform_cflags)
    # assembly (the thread switch)
    for source in sorted((PORT_DIR / "src").glob("*.S")):
        add_object(source, abi)

    # the game's sin, pow and the rest, the same on every port
    musl_math_cflags = " ".join([abi, "-std=gnu11", "-w", libc_includes, f"-I{MUSL_MATH_DIR}/include",
                                 f"-include {MUSL_MATH_DIR}/include/libm.h", *SPEED_FLAGS])
    for source in sorted((MUSL_MATH_DIR / "src").glob("*.c")):
        add_object(source, musl_math_cflags)

    n.build(outputs=elf, rule="nspire_link", inputs=objects)
    n.build(outputs=tns, rule="nspire_tns", inputs=elf)
    n.build(outputs="nspire", rule="phony", inputs=tns)
    n.newline()
