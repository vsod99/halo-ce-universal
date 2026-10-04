"""Ninja rules for the original Xbox build (``ninja xbox``).

The Xbox port (port/xbox/README.md) builds the game with nxdk: clang for
i386-pc-win32 (the Microsoft ABI, as the Windows build's), nxdk's C library
(pdclib), and lld, whose PE file nxdk's cxbe makes into
build/xbox/halo/bin/default.xbe. ``python tools/xbox_dev.py run
build/xbox/halo`` boots it in xemu.

Three kinds of units, as on Windows:
  - the game (source/, port/linux/game), with the Xbox prefix header and the
    Linux build's C runtime wrappers over pdclib;
  - the Xbox-facing platform layer: the Linux build's units that implement
    the Xbox SDK the game calls (port.json "linux_platform_sources") and
    port/xbox/src's own, which see the same headers;
  - the units that talk to nxdk itself (port/xbox/src/nxdk_*.c and
    port/xbox/common), with nxdk's headers (its winapi and kernel) and not
    the Xbox SDK's.
"""

import json
import os
from pathlib import Path
from typing import Any, Dict, List

from .linux_build import (MUSL_MATH_DIR, TOML_DIR, XDK_INCLUDE, compile_launcher, game_defines_and_includes,
                          game_sources, musl_math_sources, xdk_headers)
from .ninja_syntax import Writer
from .windows_build import inline_export_wrapper

PORT_DIR = Path("port/xbox")
LINUX_DIR = Path("port/linux")
PORT_CONFIG = PORT_DIR / "port.json"
LOCAL_CONFIG = PORT_DIR / "xemu.local.toml"
BUILD = Path("build/xbox")
DEFAULT_NXDK = Path("~/source/repos/nxdk")
DEFAULT_LLVM_BIN = Path("/opt/homebrew/opt/llvm/bin")
DEFAULT_LLD_BIN = Path("/opt/homebrew/opt/lld/bin")

# the Xbox's processor (a Pentium III: SSE, no SSE2, so floats are computed
# with SSE and doubles on the x87) and the MSVC semantics the game was
# written against, as in the Windows build (tools/windows_build.py)
XBOX_ABI_FLAGS = [
    "--target=i386-pc-win32",
    "-march=pentium3",
    "-fms-extensions",
    "-fcommon",
    "-fno-strict-aliasing",
    "-fwrapv",
    "-fno-delete-null-pointer-checks",
    "-fno-omit-frame-pointer",
    # the same floating point results on every port (halo_math.h)
    "-ffp-contract=off",
    "-O2",
    "-g",
    "-gdwarf-4",
    "-DNXDK",
    "-D__STDC__=1",
    "-Wno-builtin-macro-redefined",
]

GAME_FLAGS = [
    "-std=gnu89",
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
    "-Wno-microsoft-enum-forward-reference",
    "-Wno-language-extension-token",
]

# the main thread's stack: the AI's functions keep arrays of 80-170 KB on
# it, and one calls another
STACK_SIZE = 0x100000


def _quote(path: Any) -> str:
    text = str(path).replace(os.sep, "/")
    return f'"{text}"' if " " in text else text


def _local_tools() -> Dict[str, str]:
    """[tools] of port/xbox/xemu.local.toml (tools/xbox_dev.py), if present"""
    if not LOCAL_CONFIG.is_file():
        return {}
    from .xbox_dev import parse_flat_toml
    return parse_flat_toml(LOCAL_CONFIG.read_text()).get("tools", {})


def _tool_path(sln: Any, option: str, key: str, default: Path) -> Path:
    value = (getattr(sln, option, None) if option else None) or _local_tools().get(key) or str(default)
    return Path(os.path.expanduser(value)).resolve()


def xbox_configure_inputs() -> List[Path]:
    """Files whose change must re-run configure.py."""
    if not PORT_CONFIG.is_file():
        return []
    inputs = [Path(__file__), PORT_CONFIG,
              *(d for d in (PORT_DIR / "src", PORT_DIR / "include", PORT_DIR / "common") if d.is_dir())]
    if LOCAL_CONFIG.is_file():
        inputs.append(LOCAL_CONFIG)
    return inputs


def generate_xbox_build(n: Writer, sln: Any) -> None:
    if not PORT_CONFIG.is_file():
        return
    nxdk = _tool_path(sln, "nxdk", "nxdk", DEFAULT_NXDK)
    if not (nxdk / "lib" / "pdclib" / "include").is_dir():
        # (no nxdk on this machine: no Xbox build)
        return
    llvm_bin = _tool_path(sln, None, "llvm_bin", DEFAULT_LLVM_BIN)
    lld_bin = _tool_path(sln, None, "lld_bin", DEFAULT_LLD_BIN)
    config = json.loads(PORT_CONFIG.read_text(encoding="utf-8"))
    linux_config = json.loads((LINUX_DIR / "port.json").read_text(encoding="utf-8"))

    tags_header = BUILD / "halo_msvc_tags.h"
    obj_dir = BUILD / "obj"
    exe = BUILD / "halo.exe"
    xbe = BUILD / "halo" / "bin" / "default.xbe"
    prefix_header = PORT_DIR / "include" / "halo_xbox_prefix.h"
    nxdk_lib = nxdk / "lib"

    n.comment("Original Xbox build (ninja xbox)")
    n.variable("xbox_cc", _quote(llvm_bin / "clang"))
    n.variable("xbox_lld", _quote(lld_bin / "lld"))
    n.variable("xbox_cxbe", _quote(nxdk / "tools" / "cxbe" / "cxbe"))
    # MSVC gives struct tags first named in a prototype file scope; clang
    # does not (as the Windows build)
    n.rule(
        name="xbox_msvc_tags",
        command="$python tools/linux_msvc_semantics.py --output $out --tags source",
        description="XBOX MSVC TAGS $out",
        restat=True,
    )
    game_headers = sorted(p for p in Path("source").rglob("*") if p.suffix in (".c", ".h"))
    n.build(outputs=tags_header, rule="xbox_msvc_tags",
            implicit=[Path("tools/linux_msvc_semantics.py"), *game_headers])
    n.rule(
        name="xbox_cc",
        command=f"{compile_launcher(sln)}$xbox_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description="XBOX CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    # nxdk-link's arguments (nxdk/bin/nxdk-link), with the game's stack
    n.rule(
        name="xbox_link",
        command=("$xbox_lld -flavor link -subsystem:windows -fixed -base:0x00010000 "
                 f"-stack:{STACK_SIZE:#x} -merge:.edata=.edataxb -debug -out:$out @$out.rsp"),
        description="XBOX LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline $libs",
    )
    n.rule(
        name="xbox_xbe",
        command="$xbox_cxbe -OUT:$out -TITLE:Halo $in > /dev/null",
        description="XBOX XBE $out",
    )

    abi = " ".join(XBOX_ABI_FLAGS + (["-DHALO_RELEASE"] if getattr(sln, "port_release", False) else []))
    # pdclib, and nxdk's additions to it; the Linux build's C runtime
    # wrappers and port/xbox/include's stand-ins for the headers pdclib
    # lacks come first
    libc_includes = " ".join([
        f"-I{LINUX_DIR / 'include'}", f"-I{PORT_DIR / 'include'}",
        f"-isystem {_quote(nxdk_lib / 'pdclib' / 'include')}",
        f"-I{_quote(nxdk_lib / 'pdclib' / 'platform' / 'xbox' / 'include')}",
        f"-I{_quote(nxdk_lib / 'xboxrt' / 'libc_extensions')}",
    ])
    implicit = [*xdk_headers(), prefix_header, tags_header]
    objects: List[Path] = []

    def add_object(source: Path, cflags: str) -> None:
        obj = obj_dir / source.with_suffix(".o")
        objects.append(obj)
        n.build(outputs=obj, rule="xbox_cc", inputs=inline_export_wrapper(source, BUILD),
                implicit=[*implicit, source], variables={"cflags": cflags})

    game_cflags = " ".join([
        abi, " ".join(GAME_FLAGS),
        f"-include {prefix_header}", f"-include {tags_header}",
        game_defines_and_includes(linux_config), libc_includes, f"-I{XDK_INCLUDE}",
    ])
    game_objects_start = len(objects)
    for source in game_sources(linux_config):
        add_object(source, game_cflags)
    for source in sorted(Path(linux_config["game_sources"]).glob("*.c")):
        add_object(source, game_cflags)
    game_objects = objects[game_objects_start:]

    platform_cflags = " ".join([
        abi, " ".join(PLATFORM_FLAGS),
        f"-include {prefix_header}",
        f"-I{PORT_DIR / 'src'}", f"-I{LINUX_DIR / 'src'}", "-Iport/include",
        f"-I{TOML_DIR}", "-Isource -Isource/cseries",
        libc_includes, f"-I{XDK_INCLUDE}",
    ])
    # nxdk-cc's own flags (nxdk/bin/nxdk-cc)
    nxdk_cflags = " ".join([
        abi, "-std=gnu11", "-Wall", "-ffreestanding", "-fno-builtin",
        f"-I{PORT_DIR / 'src'}", f"-I{PORT_DIR / 'common'}",
        f"-I{_quote(nxdk_lib)}", f"-I{_quote(nxdk_lib / 'xboxrt' / 'libc_extensions')}",
        f"-isystem {_quote(nxdk_lib / 'pdclib' / 'include')}",
        f"-I{_quote(nxdk_lib / 'pdclib' / 'platform' / 'xbox' / 'include')}",
        f"-I{_quote(nxdk_lib / 'winapi')}", f"-I{_quote(nxdk_lib / 'xboxrt' / 'vcruntime')}",
        "-U__STDC_NO_THREADS__",
    ])
    for source in config.get("linux_platform_sources", []):
        add_object(Path(source), platform_cflags)
    for source in sorted((PORT_DIR / "src").glob("*.c")):
        add_object(source, nxdk_cflags if source.name.startswith("nxdk_") else platform_cflags)
    for source in sorted((PORT_DIR / "common").glob("*.c")):
        add_object(source, nxdk_cflags)
    # the settings file's parser (port_config.c)
    add_object(TOML_DIR / "tomlc17.c", " ".join([abi, "-std=gnu11", "-w", libc_includes]))
    # the game's sin, pow and the rest, the same on every port
    # (port/include/halo_math.h)
    musl_cflags = " ".join([abi, "-std=gnu11", "-w", libc_includes, f"-I{MUSL_MATH_DIR}/include",
                            f"-include {MUSL_MATH_DIR}/include/libm.h"])
    for source in musl_math_sources():
        add_object(source, musl_cflags)

    libs = " ".join(_quote(nxdk_lib / name) for name in config.get("libraries", []))
    n.build(outputs=exe, rule="xbox_link", inputs=objects, variables={"libs": libs})
    n.build(outputs=xbe, rule="xbox_xbe", inputs=exe)
    # the game's units alone, which compile before the platform layer links
    n.build(outputs="xbox-game", rule="phony", inputs=game_objects)
    n.build(outputs="xbox", rule="phony", inputs=xbe)
    n.newline()
