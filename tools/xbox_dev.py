"""The Xbox port's development loop under xemu (port/xbox/README.md).

    python tools/xbox_dev.py doctor          what is installed and configured
    python tools/xbox_dev.py build <dir>     make an nxdk project (its XBE and ISO)
    python tools/xbox_dev.py run <dir>       build it, boot it in xemu, stream its log
                                             until it prints the done marker, then
                                             stop xemu
    python tools/xbox_dev.py link MAP        two xemus, linked: the game hosting MAP
                                             as a system link game and the game
                                             joining it (debug.network_test)
    python tools/xbox_dev.py link MAP --internet
                                             the same over the internet: each on
                                             its own NAT, joining by the invite

The machine's paths live in port/xbox/xemu.local.toml (not committed; the
template is port/xbox/xemu.example.toml): nxdk, xemu, and the console files
xemu boots with (MCPX boot ROM, flash BIOS, EEPROM, hard disk image), which
must be dumped from your own console and are never committed.

`run --press 20:a,start` presses controller buttons in xemu's window, the
seconds after the program's first log line: xemu plays its keyboard as the
controller in port 1, and the keys are sent with System Events (macOS: the
terminal needs the Accessibility permission).

`run --shot 25` saves xemu's window 25 seconds after the first log line as
shot-25.png in the run's folder (the terminal needs the Screen Recording
permission).

A program reports through COM2 (port/xbox/common/xbox_log.c). xemu emulates
the debug kits' SuperIO serial port (-device lpc47m157), and the runner reads
it from a socket. The program ends its run with XBOX_LOG_DONE_MARKER; a
program that hangs is stopped at the timeout.
"""

import argparse
import base64
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOCAL_CONFIG = ROOT / "port/xbox/xemu.local.toml"
DONE_MARKER = "== XBOX DONE =="


def parse_flat_toml(text: str) -> dict:
    """the subset xemu.local.toml uses: [section] and key = "string",
    comments after #; macOS's own Python (3.9) has no tomllib"""
    config: dict = {}
    section = config
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("["):
            section = config.setdefault(line.strip("[]").strip(), {})
            continue
        if "=" not in line or line.startswith("#"):
            continue
        key, value = line.split("=", 1)
        value = value.strip()
        if value.startswith('"'):
            value = value[1:value.index('"', 1)]
        else:
            value = value.split("#", 1)[0].strip()
        section[key.strip()] = value
    return config


def load_config() -> dict:
    if not LOCAL_CONFIG.is_file():
        sys.exit(f"{LOCAL_CONFIG.relative_to(ROOT)} is missing: copy port/xbox/xemu.example.toml there and fill it in")
    config = parse_flat_toml(LOCAL_CONFIG.read_text())
    for section in ("tools", "console"):
        config.setdefault(section, {})
    return config


def expand(path: str) -> Path:
    return Path(os.path.expanduser(path))


def nxdk_environment(config: dict) -> dict:
    nxdk = expand(config["tools"]["nxdk"])
    llvm = expand(config["tools"].get("llvm_bin", "/opt/homebrew/opt/llvm/bin"))
    lld = expand(config["tools"].get("lld_bin", "/opt/homebrew/opt/lld/bin"))
    env = dict(os.environ)
    env["NXDK_DIR"] = str(nxdk)
    env["PATH"] = os.pathsep.join([str(nxdk / "bin"), str(llvm), str(lld), env.get("PATH", "")])
    return env


# the XBE header's initialization flags (nxdk tools/cxbe/Xbe.h): cxbe always
# sets "limit development kit run time memory to 64 MB", with which the
# kernel gives a 128 MB console's program only 64 MB
XBE_INIT_FLAGS_OFFSET = 0x124
XBE_INIT_FLAG_LIMIT_64MB = 0x4


def set_memory_limit(xbe: Path, limit_64mb: bool) -> None:
    data = bytearray(xbe.read_bytes())
    if data[:4] != b"XBEH":
        sys.exit(f"{xbe} is not an XBE")
    flags = int.from_bytes(data[XBE_INIT_FLAGS_OFFSET:XBE_INIT_FLAGS_OFFSET + 4], "little")
    flags = flags | XBE_INIT_FLAG_LIMIT_64MB if limit_64mb else flags & ~XBE_INIT_FLAG_LIMIT_64MB
    data[XBE_INIT_FLAGS_OFFSET:XBE_INIT_FLAGS_OFFSET + 4] = flags.to_bytes(4, "little")
    xbe.write_bytes(bytes(data))


def add_maps(config: dict, maps_dir: Path) -> None:
    """the maps [game] names (ui alone by default) from its maps folder,
    beside the game's XBE (D:\\maps), linked rather than copied"""
    game = config.get("game", {})
    if not game.get("maps_folder"):
        sys.exit("[game] maps_folder is not set in port/xbox/xemu.local.toml (the Xbox maps the game reads)")
    source = expand(game["maps_folder"])
    names = [name.strip() for name in game.get("maps", "ui").split(",") if name.strip()]
    if maps_dir.is_dir():
        shutil.rmtree(maps_dir)
    maps_dir.mkdir(parents=True)
    for name in names:
        map_file = source / f"{name}.map"
        if not map_file.is_file():
            sys.exit(f"{map_file} is missing")
        try:
            os.link(map_file, maps_dir / map_file.name)
        except OSError:
            shutil.copyfile(map_file, maps_dir / map_file.name)


def write_environment(bin_dir: Path, environment: list) -> None:
    """the --env NAME=VALUE settings as D:\\environment.txt, which the
    game's getenv reads (port/xbox/src/nxdk_libc.c): the HALO_* overrides
    of config.toml, which is on the Xbox's hard disk; none, no file"""
    path = bin_dir / "environment.txt"
    path.unlink(missing_ok=True)
    for setting in environment:
        if "=" not in setting:
            sys.exit(f"--env {setting}: not NAME=VALUE")
    if environment:
        path.write_text("".join(f"{setting}\n" for setting in environment))


def build(config: dict, project: Path, environment: list = ()) -> Path:
    """make the project's bin/default.xbe (an nxdk project with its
    Makefile, or the game, build/xbox/halo, with `ninja xbox`), set its
    memory limit to the configured console's, and pack bin/ into an ISO;
    returns the ISO"""
    jobs = str(os.cpu_count() or 4)
    env = nxdk_environment(config)
    if (project / "Makefile").is_file():
        command = ["make", "-C", str(project), "-j", jobs]
    elif project == ROOT / "build/xbox/halo":
        command = ["ninja", "-C", str(ROOT), "xbox"]
    else:
        sys.exit(f"{project} is neither an nxdk project (no Makefile) nor build/xbox/halo")
    result = subprocess.run(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    lines = [line for line in result.stdout.splitlines()
             if not line.startswith(("[ CC", "[ CXX", "[ AS")) and " XBOX CC " not in line]
    if result.returncode != 0:
        print("\n".join(lines[-40:]))
        sys.exit(f"build of {project} failed")
    if project == ROOT / "build/xbox/halo":
        add_maps(config, project / "bin/maps")
    return pack(config, project, environment, project.name)


def pack(config: dict, project: Path, environment: list, name: str) -> Path:
    """the project's bin/ with the environment as build/xbox/NAME.iso"""
    env = nxdk_environment(config)
    bin_dir = project / "bin"
    write_environment(bin_dir, list(environment))
    set_memory_limit(bin_dir / "default.xbe", config["console"].get("memory", "128") != "128")
    iso = ROOT / "build/xbox" / f"{name}.iso"
    iso.parent.mkdir(parents=True, exist_ok=True)
    iso.unlink(missing_ok=True)
    extract_xiso = Path(env["NXDK_DIR"]) / "tools/extract-xiso/build/extract-xiso"
    result = subprocess.run([str(extract_xiso), "-c", str(bin_dir), str(iso)],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode != 0 or not iso.is_file():
        print(result.stdout)
        sys.exit(f"packing {bin_dir} failed")
    return iso


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def write_xemu_config(config: dict, iso: Path, directory: Path, files: dict = None, net: list = None) -> Path:
    """xemu's configuration for a run; files replaces console files (a
    link's own disk and EEPROM), net the network's lines (NAT by default)"""
    console = config["console"]
    files = {**{key: str(expand(console[key])) for key in ("bootrom_path", "flashrom_path", "eeprom_path",
                                                             "hdd_path") if console.get(key)},
             **(files or {})}
    files["dvd_path"] = str(iso)
    lines = [
        "[general]",
        "show_welcome = false",
        "skip_boot_anim = true",
        "updates.check = false",
        "",
        "[sys]",
        f"mem_limit = '{console.get('memory', '128')}'",
        "",
        "[sys.files]",
        *(f"{key} = {json.dumps(value)}" for key, value in files.items()),
        "",
        # a controller in port 1: xemu's keyboard (its arrows, the letter
        # keys for the buttons), so every run has one plugged in
        "[input.bindings]",
        "port1 = 'keyboard'",
        "",
        "[net]",
        "enable = true",
        *(net or ["backend = 'nat'"]),
    ]
    path = directory / "xemu.toml"
    path.write_text("\n".join(lines) + "\n")
    return path


# xemu's keyboard controller (its default map) as macOS key codes; the
# sticks are E S D F and I J K L
PRESS_KEYS = {
    "a": 0, "b": 11, "x": 7, "y": 16, "start": 36, "back": 51,
    "up": 126, "down": 125, "left": 123, "right": 124,
    "lt": 13, "rt": 31,
    "lup": 14, "lleft": 1, "ldown": 2, "lright": 3,
    "rup": 34, "rleft": 38, "rdown": 40, "rright": 37,
}


def parse_presses(texts: list) -> list:
    """--press SECONDS:BUTTON,BUTTON... as (seconds, [key codes])"""
    presses = []
    for text in texts:
        seconds, _, names = text.partition(":")
        try:
            codes = [PRESS_KEYS[name.strip().lower()] for name in names.split(",") if name.strip()]
            presses.append((float(seconds), codes))
        except (KeyError, ValueError):
            sys.exit(f"--press {text}: SECONDS:BUTTON,... with buttons {', '.join(PRESS_KEYS)}")
    return sorted(presses)


def press_buttons(presses: list, start: float) -> None:
    """brings xemu forward and holds each button for 0.12 s (longer repeats in the menus), in one AppleScript
    each time (separate ones lose keys)"""
    for seconds, codes in presses:
        time.sleep(max(0.0, start + seconds - time.time()))
        script = ['tell application "System Events"',
                  'set frontmost of (first process whose name contains "xemu") to true',
                  "delay 0.3"]
        for code in codes:
            script += [f"key down {code}", "delay 0.12", f"key up {code}", "delay 0.4"]
        script.append("end tell")
        result = subprocess.run(["osascript", "-"], input="\n".join(script), text=True,
                                capture_output=True)
        if result.returncode != 0:
            print(f"(--press: {result.stderr.strip()}; the terminal needs the Accessibility permission)")
            return


def take_shots(shots: list, start: float, out: Path) -> None:
    """saves xemu's window as out/shot-SECONDS.png at each time after the program's first log line: its
    bounds from System Events, the pixels with screencapture (the terminal needs the Screen Recording
    permission; xemu's QMP has no screendump)"""
    for seconds in shots:
        time.sleep(max(0.0, start + seconds - time.time()))
        script = ['tell application "System Events"',
                  'set xemu to first process whose name contains "xemu"',
                  "set frontmost of xemu to true",
                  "delay 0.3",
                  "set {x, y} to position of front window of xemu",
                  "set {w, h} to size of front window of xemu",
                  'return (x as text) & "," & (y as text) & "," & (w as text) & "," & (h as text)',
                  "end tell"]
        result = subprocess.run(["osascript", "-"], input="\n".join(script), text=True, capture_output=True)
        if result.returncode != 0:
            print(f"(--shot: {result.stderr.strip()})")
            return
        path = out / f"shot-{seconds:g}.png"
        captured = subprocess.run(["screencapture", "-x", "-o", "-R", result.stdout.strip(), str(path)],
                                  capture_output=True, text=True)
        if captured.returncode != 0:
            print(f"(--shot: {captured.stderr.strip()}; the terminal needs the Screen Recording permission)")
            return
        print(f"(shot {path.relative_to(ROOT)})", flush=True)


class Qmp:
    def __init__(self, port: int, deadline: float):
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.2)
        self.file = self.sock.makefile("rw")
        self.file.readline()  # greeting
        self.command("qmp_capabilities")

    def command(self, name: str, **arguments) -> dict:
        self.file.write(json.dumps({"execute": name, "arguments": arguments}) + "\n")
        self.file.flush()
        while True:
            reply = json.loads(self.file.readline())
            if "return" in reply or "error" in reply:
                return reply


class Xemu:
    """one xemu machine of a run: its window, its program's log on COM2
    (a socket xemu connects to), the kernel debugger's COM1 in out/com1.bin,
    and QMP to stop it"""
    def __init__(self, config: dict, iso: Path, out: Path, headless: bool = False, gdb: bool = False,
                 wav: bool = False, files: dict = None, net: list = None, gdb_port: int = 0):
        xemu = expand(config["tools"].get("xemu", "/Applications/xemu.app/Contents/MacOS/xemu"))
        out.mkdir(parents=True, exist_ok=True)
        self.work = Path(tempfile.mkdtemp(prefix="xemu-"))
        xemu_config = write_xemu_config(config, iso, self.work, files, net)
        serial_port, self.qmp_port = free_port(), free_port()

        # the serial port's socket: xemu connects to it as a client
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", serial_port))
        self.listener.listen(1)

        # COM1 is the kernel debugger's when the kernel sees the SuperIO chip
        # (Cerbios's hybrid kernel): its packets go to a file; the program's
        # log is on COM2 (port/xbox/common/xbox_log.c)
        command = [str(xemu), "-config_path", str(xemu_config),
                   "-device", "lpc47m157",
                   "-serial", f"file:{out / 'com1.bin'}",
                   "-serial", f"tcp:127.0.0.1:{serial_port}",
                   "-qmp", f"tcp:127.0.0.1:{self.qmp_port},server,nowait"]
        if headless:
            command += ["-display", "none"]
        if wav:
            # the sound card's output to a file instead of the Mac's speakers
            # (xemu names no audio backend of its own to capture from)
            command += ["-audio", f"driver=wav,path={out / 'sound.wav'}"]
        if gdb_port:
            # (the profile's sampler: Sampler)
            command += ["-gdb", f"tcp:127.0.0.1:{gdb_port}"]
        if gdb:
            command += ["-s", "-S"]
            print("gdb: target remote localhost:1234 (the CPU waits for 'continue')")
        print(" ".join(command))
        self.xemu_log = (out / "xemu.txt").open("w")
        self.process = subprocess.Popen(command, stdout=self.xemu_log, stderr=subprocess.STDOUT)
        self.qmp = None

    def connect(self, deadline: float):
        """the program's serial connection, once xemu makes it (None if xemu
        exits first or the deadline passes)"""
        connection = None
        self.listener.settimeout(0.5)
        while connection is None and time.time() < deadline and self.process.poll() is None:
            try:
                connection, _ = self.listener.accept()
            except socket.timeout:
                pass
        if connection is not None:
            try:
                self.qmp = Qmp(self.qmp_port, deadline)
            except OSError:
                print("(no QMP: xemu cannot be stopped cleanly)")
        return connection

    def stop(self) -> None:
        if self.qmp is not None and self.process.poll() is None:
            try:
                self.qmp.command("quit")
            except OSError:
                pass
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
        shutil.rmtree(self.work, ignore_errors=True)


def listener() -> Path:
    """tools/xbox_listen.swift, built when it changes"""
    source = ROOT / "tools/xbox_listen.swift"
    program = ROOT / "build/xbox/tools/xbox_listen"
    if not program.exists() or program.stat().st_mtime < source.stat().st_mtime:
        program.parent.mkdir(parents=True, exist_ok=True)
        if subprocess.run(["swiftc", "-O", str(source), "-o", str(program)]).returncode != 0:
            sys.exit("building tools/xbox_listen.swift failed (swiftc, from Xcode's command line tools)")
    return program


class Sampler:
    """a sampling profile of the Xbox's processor: xemu's gdb stub stops the
    machine about every interval, the instruction pointer is read, and it
    runs on; the samples, by function (halo.exe's DWARF, llvm-symbolizer)
    and by source folder, go to profile.txt in the run's folder. The
    processor is xemu's, whose instructions do not cost what a Pentium III's
    do (the x87's least of all): shares to find where time goes, not
    timings"""
    def __init__(self, config: dict, port: int, start: float, seconds: float, out: Path, interval: float = 0.01):
        self.config = config
        self.port = port
        self.start = start
        self.seconds = seconds
        self.out = out
        self.interval = interval
        self.samples = []
        self.thread = None

    def begin(self) -> None:
        self.thread = threading.Thread(target=self.sample, daemon=True)
        self.thread.start()

    def send(self, data: str) -> None:
        self.connection.sendall(f"${data}#{sum(data.encode()) % 256:02x}".encode())

    def receive(self) -> str:
        """the stub's next packet (acknowledged), skipping its acknowledgements"""
        while True:
            while b"#" not in self.pending or len(self.pending) < self.pending.index(b"#") + 3:
                data = self.connection.recv(4096)
                if not data:
                    raise ConnectionError("xemu's gdb stub closed")
                self.pending += data
            start = self.pending.find(b"$")
            end = self.pending.index(b"#")
            if start < 0 or start > end:
                self.pending = self.pending[end + 3:]
                continue
            packet = self.pending[start + 1:end].decode("latin-1")
            self.pending = self.pending[end + 3:]
            self.connection.sendall(b"+")
            return packet

    def read(self, address: int, size: int) -> bytes:
        self.send(f"m{address:x},{size:x}")
        reply = self.receive()
        return b"" if reply.startswith("E") else bytes.fromhex(reply)

    def stack(self) -> list:
        """the stopped processor's instruction pointer, then the return
        addresses above it: the first code address on the stack (the caller
        of a function without a frame, such as pdclib's memset), then the
        chain of frame pointers (the game is built with them)"""
        registers = self.read_registers()
        esp, ebp, eip = registers[4], registers[5], registers[8]
        addresses = [eip]
        if eip >= 0x80000000:
            return addresses
        words = self.read(esp, 32)
        for offset in range(0, len(words) - 3, 4):
            word = int.from_bytes(words[offset:offset + 4], "little")
            if self.code[0] <= word < self.code[1]:
                addresses.append(word)
                break
        for _ in range(16):
            if ebp < 0x1000:
                break
            frame = self.read(ebp, 8)
            if len(frame) < 8:
                break
            ebp, address = int.from_bytes(frame[:4], "little"), int.from_bytes(frame[4:], "little")
            if not self.code[0] <= address < self.code[1]:
                break
            if address != addresses[-1]:
                addresses.append(address)
        return addresses

    def read_registers(self) -> list:
        self.send("g")
        reply = bytes.fromhex(self.receive())
        return [int.from_bytes(reply[index:index + 4], "little") for index in range(0, 40, 4)]

    def sample(self) -> None:
        # (halo.exe's code: its .text section in the link map, loaded at 0x10000)
        self.code = (0x11000, 0x11000)
        for line in (ROOT / "build/xbox/halo.exe.map").read_text(errors="replace").splitlines()[:40]:
            match = re.match(r"\s*0001:00000000 ([0-9a-f]{8})H \.text", line)
            if match:
                self.code = (0x11000, 0x11000 + int(match.group(1), 16))
                break
        time.sleep(self.start)
        try:
            self.connection = socket.create_connection(("127.0.0.1", self.port), timeout=10)
            self.pending = b""
            # (the stub stops the machine when it is attached to)
            self.send("?")
            self.receive()
            self.send("c")
            print(f"(profile: sampling for {self.seconds:.0f} s)", flush=True)
            end = time.time() + self.seconds
            while time.time() < end:
                time.sleep(self.interval)
                self.connection.sendall(b"\x03")
                self.receive()
                self.samples.append(self.stack())
                self.send("c")
            # (detached without waiting for its answer: the machine runs on)
            self.send("D")
            self.connection.close()
        except (OSError, ConnectionError, ValueError) as error:
            print(f"(profile: {error})", flush=True)
        self.report()

    def report(self) -> None:
        if not self.samples:
            return
        # (the raw stacks, for another look: tools/xbox_dev.py profile RUN)
        (self.out / "samples.json").write_text(json.dumps(self.samples))
        # (return addresses are symbolized at the call: the byte before them)
        stacks = [[stack[0]] + [address - 1 for address in stack[1:]] for stack in self.samples]
        symbolizer = expand(self.config["tools"].get("llvm_bin", "/opt/homebrew/opt/llvm/bin")) / "llvm-symbolizer"
        addresses = sorted({address for stack in stacks for address in stack if address < 0x80000000})
        names = {}
        if addresses:
            result = subprocess.run([str(symbolizer), "--output-style=JSON", "--no-inlines", "--relative-address",
                                     f"--obj={ROOT / 'build/xbox/halo.exe'}"],
                                    input="\n".join(hex(address - 0x10000) for address in addresses),
                                    stdout=subprocess.PIPE, text=True)
            entries = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
            for address, entry in zip(addresses, entries):
                symbol = (entry.get("Symbol") or [{}])[0]
                name = symbol.get("FunctionName") or "?"
                where = symbol.get("FileName") or ""
                names[address] = (name, where)
        # (code without DWARF, nxdk's libraries: the link map's symbols, the
        # nearest below)
        import bisect
        symbols = []
        for line in (ROOT / "build/xbox/halo.exe.map").read_text(errors="replace").splitlines():
            parts = line.split()
            if len(parts) >= 4 and re.fullmatch(r"[0-9a-f]{4}:[0-9a-f]{8}", parts[0]) and \
                    re.fullmatch(r"[0-9a-f]{16}", parts[2]):
                symbols.append((int(parts[2], 16), parts[1].lstrip("_"), parts[-1]))
        symbols.sort()
        starts = [symbol[0] for symbol in symbols]
        for address, (name, where) in list(names.items()):
            index = bisect.bisect_right(starts, address) - 1
            if name == "?" and index >= 0:
                names[address] = (symbols[index][1], symbols[index][2].split(":")[0])

        def describe(address: int) -> tuple:
            if address >= 0x80000000:
                return "(kernel)", "(kernel)"
            name, where = names.get(address, ("?", ""))
            where = os.path.relpath(where, ROOT) if where.startswith("/") else where
            folder = (os.path.dirname(where) or where) if where else "(unknown)"
            return (name if name not in ("?", "") else f"(unknown in {folder})"), folder

        functions, folders, inclusive, callers = {}, {}, {}, {}
        for stack in stacks:
            frames = [describe(address)[0] for address in stack]
            name, folder = describe(stack[0])
            functions[name] = functions.get(name, 0) + 1
            folders[folder] = folders.get(folder, 0) + 1
            for frame in set(frames):
                inclusive[frame] = inclusive.get(frame, 0) + 1
            for index, frame in enumerate(frames):
                if frame in frames[:index]:
                    continue
                caller = next((other for other in frames[index + 1:] if other != frame), "(none found)")
                callers.setdefault(frame, {})
                callers[frame][caller] = callers[frame].get(caller, 0) + 1
        total = len(stacks)

        def table(counts: dict, limit: int, indent: str = "") -> list:
            return [f"{indent}{count * 100 / total:6.1f}%  {name}" for name, count in
                    sorted(counts.items(), key=lambda item: -item[1])[:limit]]
        lines = [f"{total} samples, about every {self.interval * 1000:.0f} ms", "", "by source folder:"]
        lines += table(folders, 30)
        lines += ["", "by function (where the processor was):"]
        lines += table(functions, 60)
        lines += ["", "by function and what it called (anywhere on the stack):"]
        lines += table(inclusive, 60)
        lines += ["", "the callers of the busiest functions (anywhere on the stack):"]
        for name, _ in sorted(inclusive.items(), key=lambda item: -item[1])[:25]:
            lines += [f"  {name}:"] + table(callers[name], 8, "    ")
        (self.out / "profile.txt").write_text("\n".join(lines) + "\n")
        print(f"(profile: {self.out.relative_to(ROOT) / 'profile.txt'})", flush=True)


def run(config: dict, project: Path, timeout: float, headless: bool, gdb: bool, presses: list,
        shots: list, environment: list, wav: bool = False, listen: bool = False,
        router_forward: bool = False, profile: str = "") -> int:
    router = RouterForward() if router_forward else None
    forwarded = ForwardedPort(router.port, router.external_address) if router else None
    if forwarded:
        # (its own UPnP would ask for xemu's address, which the router refuses)
        environment = environment + forwarded.environment() + ["HALO_NET_ALLOW_UPNP=false"]
    iso = build(config, project, environment)
    out = ROOT / "build/xbox/runs" / time.strftime("%Y%m%d-%H%M%S")
    sampler = None
    if profile:
        start, _, seconds = profile.partition(":")
        sampler = Sampler(config, free_port(), float(start), float(seconds or 30), out)
    machine = Xemu(config, iso, out, headless, gdb, wav or listen, net=forwarded.net() if forwarded else None,
                   gdb_port=sampler.port if sampler else 0)
    # (xemu plays the AC'97 controller's sound into a WAV file alone: its SDL
    # output crashes for it; the player follows the file)
    player = subprocess.Popen([str(listener()), str(out / "sound.wav")]) if listen else None

    # xemu's QMP has no screendump (it draws with its own renderer): frames
    # come from the program itself, in its log
    deadline = time.time() + timeout
    status = 2
    try:
        connection = machine.connect(deadline)
        if connection is not None:
            if sampler:
                sampler.begin()
            if presses:
                threading.Thread(target=press_buttons, args=(presses, time.time()), daemon=True).start()
            if shots:
                threading.Thread(target=take_shots, args=(sorted(shots), time.time(), out), daemon=True).start()
            status = stream_log(connection, out / "log.txt", machine.process, deadline)
        if status != 0:
            reason = "xemu exited" if machine.process.poll() is not None else f"the {timeout:.0f} s timeout"
            print(f"(no '{DONE_MARKER}': {reason}; xemu's own log is {out.relative_to(ROOT) / 'xemu.txt'})")
    finally:
        machine.stop()
        if player:
            player.terminate()
        if forwarded:
            forwarded.close()
        if router:
            router.close()
    if wav or listen:
        finish_wav(out / "sound.wav")
    print(f"run saved in {out.relative_to(ROOT)}")
    return status


def eeprom_checksum(data: bytes) -> int:
    """the EEPROM's sections' checksum: the 32-bit words summed with their
    carries, folded and inverted"""
    high = low = 0
    for offset in range(0, len(data), 4):
        total = (high << 32 | low) + int.from_bytes(data[offset:offset + 4], "little")
        high, low = total >> 32 & 0xFFFFFFFF, total & 0xFFFFFFFF
    return ~(high + low) & 0xFFFFFFFF


def eeprom_with_ethernet_address(source: Path, destination: Path, last_byte: int) -> str:
    """a copy of the EEPROM whose factory section (0x30: its checksum over
    0x34-0x5F; the Ethernet address at 0x40) gives the machine another
    Ethernet address, its last byte changed; returns the address"""
    data = bytearray(source.read_bytes())
    if eeprom_checksum(data[0x34:0x60]) != int.from_bytes(data[0x30:0x34], "little"):
        sys.exit(f"{source}: the factory section's checksum is wrong (an encrypted or damaged EEPROM?)")
    data[0x45] = last_byte
    data[0x30:0x34] = eeprom_checksum(data[0x34:0x60]).to_bytes(4, "little")
    destination.write_bytes(bytes(data))
    return ":".join(f"{byte:02x}" for byte in data[0x40:0x46])


def free_udp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Cable:
    """the link's cable: each xemu's network card sends its frames to a
    socket of this, which passes them to the other's, and writes them all
    to a pcap file (Wireshark, tcpdump -r) with a summary of each machine's
    traffic at the end"""
    def __init__(self, path: Path):
        self.sockets = []
        for _ in range(2):
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.bind(("127.0.0.1", 0))
            s.settimeout(0.5)
            self.sockets.append(s)
        self.ports = [s.getsockname()[1] for s in self.sockets]
        self.cards = [None, None]  # each card's own port, as its first frame shows
        self.pcap = path.open("wb")
        # (the pcap header: microsecond times, frames up to 64 KB, Ethernet)
        self.pcap.write((0xA1B2C3D4).to_bytes(4, "little") + (2).to_bytes(2, "little") + (4).to_bytes(2, "little") +
                        bytes(8) + (65535).to_bytes(4, "little") + (1).to_bytes(4, "little"))
        self.lock = threading.Lock()
        self.counts = [{}, {}]
        self.running = True
        self.threads = [threading.Thread(target=self.carry, args=(index,), daemon=True) for index in range(2)]
        for thread in self.threads:
            thread.start()

    def carry(self, index: int) -> None:
        """machine index's frames to the other machine"""
        while self.running:
            try:
                frame, source = self.sockets[index].recvfrom(65536)
            except socket.timeout:
                continue
            except OSError:
                return
            self.cards[index] = source
            other = self.cards[1 - index]
            if other is not None:
                self.sockets[1 - index].sendto(frame, other)
            now = time.time()
            kind = self.kind(frame)
            with self.lock:
                self.counts[index][kind] = self.counts[index].get(kind, 0) + 1
                self.pcap.write(int(now).to_bytes(4, "little") + int(now % 1 * 1e6).to_bytes(4, "little") +
                                len(frame).to_bytes(4, "little") * 2 + frame)

    @staticmethod
    def kind(frame: bytes) -> str:
        """a frame's protocol and ports, for the summary"""
        if len(frame) < 14:
            return "short"
        ethertype = int.from_bytes(frame[12:14], "big")
        if ethertype == 0x0806:
            return "ARP"
        if ethertype == 0x86DD:
            return "IPv6"
        if ethertype != 0x0800 or len(frame) < 34:
            return f"ethertype {ethertype:04x}"
        header = (frame[14] & 15) * 4
        protocol = frame[23]
        destination = ".".join(str(byte) for byte in frame[30:34])
        if protocol in (6, 17) and len(frame) >= 14 + header + 4:
            ports = frame[14 + header:14 + header + 4]
            name = "TCP" if protocol == 6 else "UDP"
            return (f"{name} {int.from_bytes(ports[:2], 'big')} -> {destination}:"
                    f"{int.from_bytes(ports[2:], 'big')}")
        return f"IP protocol {protocol} -> {destination}"

    def close(self, names: tuple) -> None:
        self.running = False
        for thread in self.threads:
            thread.join()
        for s in self.sockets:
            s.close()
        self.pcap.close()
        for name, counts in zip(names, self.counts):
            summary = ", ".join(f"{kind} x{count}" for kind, count in
                                sorted(counts.items(), key=lambda item: -item[1])[:12])
            print(f"cable: {name} sent {sum(counts.values())} frames: {summary or 'none'}")


class ForwardedPort:
    """the host's router in an internet link that forwards its tunnel port
    (network.tunnel_port), as a player would set up for a NAT that stops
    connections: xemu's NAT forwards a port of the Mac to the host's tunnel
    port, and this STUN server, the host's only one, tells it that its
    internet address is that port of 10.0.2.2, which the joining machine's
    xemu NAT takes to the Mac (127.0.0.1)"""
    TUNNEL_PORT = 2310

    def __init__(self, mac_port: int = 0, address: str = "10.0.2.2"):
        """mac_port and address: the port and address told, which the
        router forwards (RouterForward), instead of 10.0.2.2 and a port of
        the Mac's own"""
        self.mac_port = mac_port or free_udp_port()
        self.address = address
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind(("127.0.0.1", 0))
        self.socket.settimeout(0.5)
        self.running = True
        self.answered = 0
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def environment(self) -> list:
        return [f"HALO_NET_TUNNEL_PORT={self.TUNNEL_PORT}", f"HALO_NET_STUN=10.0.2.2:{self.socket.getsockname()[1]}"]

    def net(self) -> list:
        return ["backend = 'nat'", "", "[net.nat]",
                f"forward_ports = [{{ host = {self.mac_port}, guest = {self.TUNNEL_PORT}, protocol = 'udp' }}]"]

    def serve(self) -> None:
        """answers STUN binding requests (RFC 5389) with 10.0.2.2:mac_port
        as the XOR-MAPPED-ADDRESS"""
        cookie = 0x2112A442
        while self.running:
            try:
                request, source = self.socket.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                return
            if len(request) < 20 or request[:2] != b"\x00\x01":
                continue
            address = socket.inet_aton(self.address)
            value = (b"\x00\x01" + (self.mac_port ^ cookie >> 16).to_bytes(2, "big") +
                     (int.from_bytes(address, "big") ^ cookie).to_bytes(4, "big"))
            attribute = (0x0020).to_bytes(2, "big") + len(value).to_bytes(2, "big") + value
            self.socket.sendto(b"\x01\x01" + len(attribute).to_bytes(2, "big") + request[4:20] + attribute, source)
            self.answered += 1

    def close(self) -> None:
        self.running = False
        self.thread.join()
        self.socket.close()
        print(f"router: forwarded the Mac's UDP port {self.mac_port} to the Xbox's {self.TUNNEL_PORT}; "
              f"answered {self.answered} STUN requests")


class RouterForward:
    """a UDP port of the Mac's network's router forwarded to the Mac, asked
    for with UPnP (as the Xbox would ask for its own: xemu's NAT address is
    not on the router's network, so the router refuses it that), for an
    hour, and given up at the end; the router's internet address"""
    SERVICES = ("urn:schemas-upnp-org:service:WANIPConnection:1", "urn:schemas-upnp-org:service:WANIPConnection:2",
                "urn:schemas-upnp-org:service:WANPPPConnection:1")

    def __init__(self):
        import urllib.parse
        import urllib.request
        import xml.etree.ElementTree as ElementTree
        self.request = urllib.request
        search = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        search.settimeout(2)
        search.sendto(b"M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n"
                      b"ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n", ("239.255.255.250", 1900))
        locations = []
        try:
            while True:
                reply, _ = search.recvfrom(4096)
                found = re.search(rb"(?im)^location:\s*(\S+)", reply)
                if found and found.group(1).decode() not in locations:
                    locations.append(found.group(1).decode())
        except socket.timeout:
            pass
        search.close()
        self.control = None
        for location in locations:
            description = ElementTree.fromstring(urllib.request.urlopen(location, timeout=5).read())
            for service in description.iter():
                if not service.tag.endswith("}service"):
                    continue
                fields = {child.tag.split("}")[-1]: (child.text or "").strip() for child in service}
                if fields.get("serviceType") in self.SERVICES:
                    self.control = urllib.parse.urljoin(location, fields["controlURL"])
                    self.service = fields["serviceType"]
                    break
            if self.control:
                break
        if not self.control:
            sys.exit(f"router: no UPnP Internet Gateway Device answered ({len(locations)} devices)")
        # this Mac's address on the router's network
        router_host = urllib.parse.urlparse(self.control).hostname
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.connect((router_host, 1900))
        self.lan_address = probe.getsockname()[0]
        probe.close()
        self.external_address = self.soap("GetExternalIPAddress", {})["NewExternalIPAddress"]
        self.port = 0
        for _ in range(8):
            port = free_udp_port() if not self.port else 49152 + int.from_bytes(os.urandom(2), "big") % 16384
            try:
                self.soap("AddPortMapping", {
                    "NewRemoteHost": "", "NewExternalPort": port, "NewProtocol": "UDP", "NewInternalPort": port,
                    "NewInternalClient": self.lan_address, "NewEnabled": 1,
                    "NewPortMappingDescription": "Halo Xbox test (xbox_dev.py)", "NewLeaseDuration": 3600})
                self.port = port
                break
            except RuntimeError as error:
                print(f"router: port {port}: {error}")
                self.port = -1
        if self.port <= 0:
            sys.exit("router: it refused to forward a port")
        print(f"router: {self.external_address}:{self.port} forwarded to this Mac ({self.lan_address}) for an hour")

    def soap(self, action: str, arguments: dict) -> dict:
        body = "".join(f"<{name}>{value}</{name}>" for name, value in arguments.items())
        envelope = ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
                    's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>'
                    f'<u:{action} xmlns:u="{self.service}">{body}</u:{action}></s:Body></s:Envelope>')
        request = self.request.Request(self.control, data=envelope.encode(), headers={
            "Content-Type": 'text/xml; charset="utf-8"', "SOAPAction": f'"{self.service}#{action}"'})
        try:
            reply = self.request.urlopen(request, timeout=5).read().decode("utf-8", "replace")
        except Exception as error:
            text = error.read().decode("utf-8", "replace") if hasattr(error, "read") else str(error)
            code = re.search(r"<errorCode>(\d+)</errorCode>", text)
            raise RuntimeError(f"{action} failed ({code.group(1) if code else text[:200]})")
        return dict(re.findall(r"<(New\w+)>([^<]*)</New\w+>", reply))

    def close(self) -> None:
        try:
            self.soap("DeletePortMapping", {"NewRemoteHost": "", "NewExternalPort": self.port, "NewProtocol": "UDP"})
            print(f"router: forwarding of port {self.port} removed")
        except RuntimeError as error:
            print(f"router: removing the forwarding of port {self.port}: {error}")


def link(config: dict, timeout: float, host_environment: list, join_environment: list, environment: list,
         headless: bool, internet: bool = False, forward: bool = False, upnp: bool = False) -> int:
    """two Xboxes and a cable: the game hosting a system link game
    (debug.network_test host:MAP) and the game joining it, in two xemus
    whose network cards send each other their frames over UDP on this Mac,
    through the runner, which saves them as cable.pcap (no DHCP server:
    their link-local addresses). Each has its own clone of
    the hard disk (xemu locks the image) and its own Ethernet address (an
    EEPROM copy); the logs, prefixed, until both print the done marker or
    the timeout. The runs are in build/xbox/runs/TIME/{host,join}.

    With internet, no cable: each machine is on its own xemu NAT (the Mac's
    internet), and the joining one starts once the host logs its invite,
    with that as its command line (HALO_COMMAND_LINE): internet play's
    signalling, STUN and tunnel, as two players' homes. xemu's NAT gives
    each destination its own port, which two machines cannot get through
    (p2p.c), so forward has the host's router forward its port
    (ForwardedPort). UPnP is off unless upnp: the machines' searches would
    reach the Mac's network through xemu's NAT, and ask its router to
    forward ports to xemu's address."""
    project = ROOT / "build/xbox/halo"
    names = ("host", "join")
    router = ForwardedPort() if internet and forward else None
    if internet and not upnp:
        environment = environment + ["HALO_NET_ALLOW_UPNP=false"]
    environments = (environment + host_environment + (router.environment() if router else []),
                    environment + join_environment)
    isos = [build(config, project, environments[0])]
    if not internet:
        isos.append(pack(config, project, environments[1], "halo-join"))
    out = ROOT / "build/xbox/runs" / time.strftime("%Y%m%d-%H%M%S")
    out.mkdir(parents=True)
    console = config["console"]
    eeprom = expand(console.get("eeprom_path", ""))
    if not eeprom.is_file():
        sys.exit("a link needs [console] eeprom_path (each machine its own Ethernet address)")
    cable = None if internet else Cable(out / "cable.pcap")
    machines = []
    deadline = time.time() + timeout
    statuses = [2, 2]
    invite = []
    invited = threading.Event()

    def watch_host(line: str) -> None:
        found = re.search(r"halo://join/[0-9a-f]+", line)
        if found and not invited.is_set():
            invite.append(found.group(0))
            invited.set()

    def start(index: int) -> None:
        name = names[index]
        files = {}
        # (APFS clones: instant, the image itself untouched)
        disk = out / f"{name}-hdd.qcow2"
        if subprocess.run(["cp", "-c", str(expand(console["hdd_path"])), str(disk)]).returncode != 0:
            sys.exit(f"cloning {console['hdd_path']} failed")
        files["hdd_path"] = str(disk)
        files["eeprom_path"] = str(out / f"{name}-eeprom.bin")
        address = eeprom_with_ethernet_address(eeprom, Path(files["eeprom_path"]), 0x10 + index)
        if internet:
            net = router.net() if router and index == 0 else None
        else:
            net = ["backend = 'udp'", "", "[net.udp]", f"bind_addr = '127.0.0.1:{free_udp_port()}'",
                   f"remote_addr = '127.0.0.1:{cable.ports[index]}'"]
        print(f"{name}: Ethernet {address}")
        machines.append(Xemu(config, isos[index], out / name, headless, files=files, net=net))

    def follow(index: int) -> None:
        connection = machines[index].connect(deadline)
        if connection is not None:
            statuses[index] = stream_log(connection, out / names[index] / "log.txt", machines[index].process,
                                         deadline, f"[{names[index]}] ", watch_host if index == 0 else None)
    try:
        threads = []
        for index in range(2):
            if index == 1 and internet:
                if not invited.wait(max(0.0, deadline - time.time())) or machines[0].process.poll() is not None:
                    print("(host: no invite logged; the joining machine is not started)")
                    break
                print(f"join: opening {invite[0]}")
                isos.append(pack(config, project, environments[1] + [f"HALO_COMMAND_LINE={invite[0]}"],
                                 "halo-join"))
            start(index)
            threads.append(threading.Thread(target=follow, args=(index,), daemon=True))
            threads[-1].start()
        for thread in threads:
            thread.join()
        for name, status, machine in zip(names, statuses, machines):
            if status != 0:
                reason = "xemu exited" if machine.process.poll() is not None else f"the {timeout:.0f} s timeout"
                print(f"({name}: no '{DONE_MARKER}': {reason})")
    finally:
        for machine in machines:
            machine.stop()
        if cable:
            cable.close(names)
        if router:
            router.close()
        for name in names:
            (out / f"{name}-hdd.qcow2").unlink(missing_ok=True)
    print(f"link saved in {out.relative_to(ROOT)}")
    return max(statuses)


def finish_wav(path: Path) -> None:
    """the RIFF and data sizes of a WAV file xemu wrote without them (it
    writes them only when it exits cleanly)"""
    if not path.exists() or path.stat().st_size < 44:
        return
    with path.open("r+b") as file:
        header = file.read(44)
        if header[:4] != b"RIFF" or header[36:40] != b"data":
            return
        size = path.stat().st_size
        file.seek(4)
        file.write((size - 8).to_bytes(4, "little"))
        file.seek(40)
        file.write((size - 44).to_bytes(4, "little"))


def write_png(path: Path, width: int, height: int, rows: list) -> None:
    """an 8-bit RGB PNG of rows of width * 3 bytes (no Pillow needed)"""
    def chunk(kind: bytes, data: bytes) -> bytes:
        return (len(data).to_bytes(4, "big") + kind + data +
                (zlib.crc32(kind + data) & 0xffffffff).to_bytes(4, "big"))
    header = width.to_bytes(4, "big") + height.to_bytes(4, "big") + bytes([8, 2, 0, 0, 0])
    raw = b"".join(b"\0" + row for row in rows)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 6)) +
                     chunk(b"IEND", b""))


class LogScreenshots:
    """the frames the game writes to its log (debug.screenshot_every:
    port/xbox/src/d3d8_nv2a.c) as frame-FRAME.png in the run's folder"""
    def __init__(self, out: Path):
        self.out = out
        self.header = None
        self.rows = []

    def take(self, line: str) -> bool:
        """whether the line was part of a frame (and so not the log's)"""
        if line.startswith("screenshot ") and line != "screenshot end":
            parts = line.split()
            # a target besides the screen carries its address: frame-FRAME-ADDRESS.png
            if len(parts) in (4, 5) and all(part.isdigit() for part in parts[1:4]):
                name = parts[1] + ("-" + parts[4] if len(parts) == 5 else "")
                self.header = (name, int(parts[2]), int(parts[3]))
                self.rows = []
                return True
        if self.header is None:
            return False
        if line.startswith("~"):
            self.rows.append(base64.b64decode(line[1:]))
            return True
        if line == "screenshot end":
            frame, width, height = self.header
            self.header = None
            if len(self.rows) == height and all(len(row) == width * 3 for row in self.rows):
                path = self.out / f"frame-{frame}.png"
                write_png(path, width, height, self.rows)
                print(f"(frame {path.relative_to(ROOT)})", flush=True)
            else:
                print(f"(frame {frame}: {len(self.rows)} of {height} rows; not saved)", flush=True)
            return True
        return False


def stream_log(connection: socket.socket, path: Path, process: subprocess.Popen, deadline: float,
               prefix: str = "", watch=None) -> int:
    """prints and saves the program's serial lines, its frames as PNGs, and
    passes each line to watch; 0 at the done marker"""
    connection.settimeout(0.5)
    pending = b""
    screenshots = LogScreenshots(path.parent)
    with path.open("w") as log:
        while time.time() < deadline and process.poll() is None:
            try:
                data = connection.recv(4096)
            except socket.timeout:
                continue
            if not data:
                break
            pending += data
            *lines, pending = pending.split(b"\n")
            for raw in lines:
                line = raw.decode("latin-1").rstrip("\r")
                if screenshots.take(line):
                    continue
                print(prefix + line, flush=True)
                log.write(line + "\n")
                log.flush()
                if watch:
                    watch(line)
                if line.strip() == DONE_MARKER:
                    return 0
    return 2


def doctor(config: dict) -> int:
    ok = True
    tools = config["tools"]
    checks = [
        ("nxdk", expand(tools.get("nxdk", "")) / "Makefile"),
        ("clang", expand(tools.get("llvm_bin", "/opt/homebrew/opt/llvm/bin")) / "clang"),
        ("lld-link", expand(tools.get("lld_bin", "/opt/homebrew/opt/lld/bin")) / "lld-link"),
        ("xemu", expand(tools.get("xemu", "/Applications/xemu.app/Contents/MacOS/xemu"))),
    ]
    for key in ("bootrom_path", "flashrom_path", "eeprom_path", "hdd_path"):
        value = config["console"].get(key)
        checks.append((f"console.{key}", expand(value) if value else None))
    for name, path in checks:
        present = path is not None and path.exists()
        ok &= present or name == "console.eeprom_path"  # xemu makes an EEPROM if none
        print(f"{'ok ' if present else 'MISSING'} {name}: {path or '(not set)'}")
    print(f"memory: {config['console'].get('memory', '128')} MB")
    bootrom = config["console"].get("bootrom_path")
    if bootrom and expand(bootrom).is_file():
        ok &= check_bootrom(expand(bootrom))
    return 0 if ok else 1


# the MCPX 1.0 boot ROM xemu expects, and the common bad dump of it
# (xemu.app/docs/required-files)
MCPX_GOOD_MD5 = "d49c52a4102f6df7bcf8d0617ac475ed"
MCPX_BAD_MD5 = "196a5f59a13382c185636e691d6c323d"


def check_bootrom(path: Path) -> bool:
    import hashlib
    data = path.read_bytes()
    digest = hashlib.md5(data).hexdigest()
    if digest == MCPX_GOOD_MD5:
        print("ok  MCPX boot ROM: the 1.0 ROM xemu expects")
        return True
    if digest == MCPX_BAD_MD5:
        print("BAD MCPX boot ROM: the known slightly corrupted dump; dump it again")
    elif len(data) != 512:
        print(f"BAD MCPX boot ROM: {len(data)} bytes, not 512")
    else:
        bounds = "right" if data[:2] == b"\x33\xc0" and data[-2:] == b"\x02\xee" else "wrong"
        print(f"??? MCPX boot ROM: unknown checksum {digest} (first/last bytes {bounds}; "
              "a 1.1 ROM or a bad dump)")
    return False


def run_environment(args: argparse.Namespace) -> list:
    """--env, with --input and --frames as the settings they stand for"""
    environment = list(args.env)
    if args.input:
        environment.append("HALO_TEST_INPUT=press:" + ";".join(args.input))
    if args.frames:
        environment.append(f"HALO_SCREENSHOT_EVERY={args.frames}")
    return environment


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("doctor")
    build_parser = sub.add_parser("build")
    build_parser.add_argument("project", type=Path)
    run_parser = sub.add_parser("run")
    run_parser.add_argument("project", type=Path)
    run_parser.add_argument("--timeout", type=float, default=60)
    run_parser.add_argument("--headless", action="store_true", help="no xemu window (-display none)")
    run_parser.add_argument("--gdb", action="store_true", help="wait for gdb on localhost:1234")
    run_parser.add_argument("--press", action="append", default=[], metavar="SECONDS:BUTTON,...",
                            help=f"press controller buttons in xemu ({', '.join(PRESS_KEYS)}); repeatable")
    run_parser.add_argument("--shot", action="append", default=[], type=float, metavar="SECONDS",
                            help="save xemu's window as shot-SECONDS.png in the run's folder; repeatable")
    run_parser.add_argument("--input", action="append", default=[], metavar="SECONDS:BUTTON,...",
                            help="press buttons from inside the game (debug.test_input \"press:\"), timed from "
                                 "its first controller read; needs no access to the Mac's screen; rup, rdown, "
                                 "rleft and rright turn the view; BUTTON*SECONDS holds it that long; repeatable")
    run_parser.add_argument("--frames", type=int, default=0, metavar="N",
                            help="the game writes every Nth frame to the log, saved as frame-N.png "
                                 "(debug.screenshot_every), and the targets besides the screen it drew into "
                                 "as frame-N-ADDRESS.png")
    run_parser.add_argument("--wav", action="store_true",
                            help="record the sound as sound.wav in the run's folder (44.1 kHz stereo)")
    run_parser.add_argument("--listen", action="store_true",
                            help="play the sound on the Mac as it comes (xemu cannot play the AC'97 "
                                 "controller's itself): recorded as with --wav, and followed by "
                                 "tools/xbox_listen.swift")
    run_parser.add_argument("--profile", default="", metavar="START:SECONDS",
                            help="sample the Xbox's processor through xemu's gdb stub for SECONDS, START seconds "
                                 "after the program's first log line: profile.txt in the run's folder")
    run_parser.add_argument("--router-forward", action="store_true",
                            help="ask the Mac's network's router (UPnP) to forward a UDP port to the Mac, which "
                                 "xemu forwards to the Xbox's network.tunnel_port, and tell the Xbox (as its only "
                                 "STUN server) that the router's address and that port are its own: internet "
                                 "play reaches it as a console whose router forwards it a port. Removed at the end")
    run_parser.add_argument("--env", action="append", default=[], metavar="NAME=VALUE",
                            help="an environment variable for the run, such as HALO_GPU_TRACE=1500 "
                                 "(D:\\environment.txt); repeatable")
    link_parser = sub.add_parser("link", help="two Xboxes in a system link game (build/xbox/halo)")
    link_parser.add_argument("map", help="the map the host plays, MAP[:VARIANT,...] (debug.network_test host:)")
    link_parser.add_argument("--timeout", type=float, default=180)
    link_parser.add_argument("--headless", action="store_true", help="no xemu windows (-display none)")
    link_parser.add_argument("--internet", action="store_true",
                             help="no cable: each machine on its own xemu NAT, the joining one opening the "
                                  "host's invite (internet play)")
    link_parser.add_argument("--forward", action="store_true",
                             help="with --internet: the host's router forwards its tunnel port (which xemu's "
                                  "NAT otherwise needs to connect)")
    link_parser.add_argument("--upnp", action="store_true",
                             help="with --internet: let the machines ask the Mac's network's router to forward "
                                  "ports (UPnP), which is off otherwise")
    link_parser.add_argument("--env", action="append", default=[], metavar="NAME=VALUE",
                             help="an environment variable for both machines; repeatable")
    link_parser.add_argument("--host-env", action="append", default=[], metavar="NAME=VALUE",
                             help="an environment variable for the host alone; repeatable")
    link_parser.add_argument("--join-env", action="append", default=[], metavar="NAME=VALUE",
                             help="an environment variable for the joining machine alone; repeatable")
    profile_parser = sub.add_parser("profile", help="write a run's profile.txt again from its samples.json "
                                                    "(halo.exe as it is now)")
    profile_parser.add_argument("run", type=Path, help="the run's folder (build/xbox/runs/...)")
    args = parser.parse_args()
    config = load_config()
    if args.command == "profile":
        sampler = Sampler(config, 0, 0, 0, args.run.resolve())
        sampler.samples = json.loads((args.run / "samples.json").read_text())
        sampler.report()
        return
    if args.command == "doctor":
        sys.exit(doctor(config))
    if args.command == "build":
        print(build(config, args.project.resolve()))
        return
    if args.command == "link":
        sys.exit(link(config, args.timeout, [f"HALO_NETWORK_TEST=host:{args.map}"] + args.host_env,
                      ["HALO_NETWORK_TEST=join"] + args.join_env, args.env, args.headless, args.internet,
                      args.forward, args.upnp))
    sys.exit(run(config, args.project.resolve(), args.timeout, args.headless, args.gdb, parse_presses(args.press),
            args.shot, run_environment(args), args.wav, args.listen, args.router_forward,
            args.profile))


if __name__ == "__main__":
    main()
