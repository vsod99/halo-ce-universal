"""The Xbox port's development loop under xemu (port/xbox/README.md).

    python tools/xbox_dev.py doctor          what is installed and configured
    python tools/xbox_dev.py build <dir>     make an nxdk project (its XBE and ISO)
    python tools/xbox_dev.py run <dir>       build it, boot it in xemu, stream its log
                                             until it prints the done marker, then
                                             stop xemu

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
    bin_dir = project / "bin"
    if project == ROOT / "build/xbox/halo":
        add_maps(config, bin_dir / "maps")
    write_environment(bin_dir, list(environment))
    set_memory_limit(bin_dir / "default.xbe", config["console"].get("memory", "128") != "128")
    iso = ROOT / "build/xbox" / f"{project.name}.iso"
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


def write_xemu_config(config: dict, iso: Path, directory: Path) -> Path:
    console = config["console"]
    files = {key: str(expand(console[key])) for key in ("bootrom_path", "flashrom_path", "eeprom_path", "hdd_path")
             if console.get(key)}
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
        "backend = 'nat'",
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


def run(config: dict, project: Path, timeout: float, headless: bool, gdb: bool, presses: list,
        shots: list, environment: list, wav: bool = False) -> int:
    iso = build(config, project, environment)
    xemu = expand(config["tools"].get("xemu", "/Applications/xemu.app/Contents/MacOS/xemu"))
    out = ROOT / "build/xbox/runs" / time.strftime("%Y%m%d-%H%M%S")
    out.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="xemu-"))
    xemu_config = write_xemu_config(config, iso, work)
    serial_port, qmp_port = free_port(), free_port()

    # the serial port's socket: xemu connects to it as a client
    listener = socket.socket()
    listener.bind(("127.0.0.1", serial_port))
    listener.listen(1)

    # COM1 is the kernel debugger's when the kernel sees the SuperIO chip
    # (Cerbios's hybrid kernel): its packets go to a file; the program's
    # log is on COM2 (port/xbox/common/xbox_log.c)
    command = [str(xemu), "-config_path", str(xemu_config),
               "-device", "lpc47m157",
               "-serial", f"file:{out / 'com1.bin'}",
               "-serial", f"tcp:127.0.0.1:{serial_port}",
               "-qmp", f"tcp:127.0.0.1:{qmp_port},server,nowait"]
    if headless:
        command += ["-display", "none"]
    if wav:
        # the sound card's output to a file instead of the Mac's speakers
        # (xemu names no audio backend of its own to capture from)
        command += ["-audio", f"driver=wav,path={out / 'sound.wav'}"]
    if gdb:
        command += ["-s", "-S"]
        print("gdb: target remote localhost:1234 (the CPU waits for 'continue')")
    print(" ".join(command))
    xemu_log = (out / "xemu.txt").open("w")
    process = subprocess.Popen(command, stdout=xemu_log, stderr=subprocess.STDOUT)

    # xemu's QMP has no screendump (it draws with its own renderer): frames
    # will come from the program itself, over the network
    deadline = time.time() + timeout
    status = 2
    qmp = None
    try:
        connection = None
        listener.settimeout(0.5)
        while connection is None and time.time() < deadline and process.poll() is None:
            try:
                connection, _ = listener.accept()
            except socket.timeout:
                pass
        if connection is not None:
            try:
                qmp = Qmp(qmp_port, deadline)
            except OSError:
                print("(no QMP: xemu cannot be stopped cleanly)")
            if presses:
                threading.Thread(target=press_buttons, args=(presses, time.time()), daemon=True).start()
            if shots:
                threading.Thread(target=take_shots, args=(sorted(shots), time.time(), out), daemon=True).start()
            status = stream_log(connection, out / "log.txt", process, deadline)
        if status != 0:
            reason = "xemu exited" if process.poll() is not None else f"the {timeout:.0f} s timeout"
            print(f"(no '{DONE_MARKER}': {reason}; xemu's own log is {out.relative_to(ROOT) / 'xemu.txt'})")
    finally:
        if qmp is not None and process.poll() is None:
            try:
                qmp.command("quit")
            except OSError:
                pass
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
        shutil.rmtree(work, ignore_errors=True)
    if wav:
        finish_wav(out / "sound.wav")
    print(f"run saved in {out.relative_to(ROOT)}")
    return status


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


def stream_log(connection: socket.socket, path: Path, process: subprocess.Popen, deadline: float) -> int:
    """prints and saves the program's serial lines, its frames as PNGs; 0 at the done marker"""
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
                print(line, flush=True)
                log.write(line + "\n")
                log.flush()
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
                            help="record the sound as sound.wav in the run's folder (48 kHz stereo)")
    run_parser.add_argument("--env", action="append", default=[], metavar="NAME=VALUE",
                            help="an environment variable for the run, such as HALO_GPU_TRACE=1500 "
                                 "(D:\\environment.txt); repeatable")
    args = parser.parse_args()
    config = load_config()
    if args.command == "doctor":
        sys.exit(doctor(config))
    if args.command == "build":
        print(build(config, args.project.resolve()))
        return
    sys.exit(run(config, args.project.resolve(), args.timeout, args.headless, args.gdb, parse_presses(args.press),
            args.shot, run_environment(args), args.wav))


if __name__ == "__main__":
    main()
