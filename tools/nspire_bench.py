#!/usr/bin/env python3
"""
nspire_bench.py: plays the demo (port/nspire/src/nspire_demo.c) on the
calculator from the computer, with no one at it, and reports its time.

	python3 tools/nspire_bench.py --upload --tag rect-path [--runs 2]

--upload sends build/nspire/halo.tns as /q_demo.tns (and as /halo.tns and
/halo_demo.tns, for playing by hand). Each run then, through
build/nspire/nspire_link:

  - closes the "Document Sent" box the last transfer left (found in a
    screenshot by its border);
  - types q in the file browser, which selects q_demo, the only name under q,
    and enter, which launches it;
  - waits for the calculator to leave the USB bus (Halo running) and to come
    back (the demo over: it quits by itself);
  - fetches halo_log.txt.tns into build/nspire/bench/<tag>-<n>.txt and prints
    its "demo: done" line and the profile's sections averaged over the demo.

The calculator must be on, unlocked, showing the file browser of My
Documents, with the demo recorded (halo_demo_save.tns and
halo_demo_input.tns, made with r).
"""

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

LINK = "build/nspire/nspire_link"
SHOT = Path("build/nspire/bench/screen.ppm")

KEY_ENTER = "0d1000"
KEY_Q = "716200"


def link(*args, timeout=60):
    """nspire_link with a time limit: (exit status, output)."""
    try:
        done = subprocess.run([LINK, *args], capture_output=True, text=True, timeout=timeout)
        return done.returncode, done.stdout + done.stderr
    except subprocess.TimeoutExpired:
        return -1, "timed out"


def connected():
    status, _ = link("ls", "/", timeout=30)
    return status == 0


def screen():
    """The screen: (width, height, rgb bytes), or None."""
    status, output = link("shot", str(SHOT))
    if status:
        print(output.strip())
        return None
    data = SHOT.read_bytes()
    _, size, _, pixels = data.split(b"\n", 3)
    width, height = map(int, size.split())
    return width, height, pixels


def dialog_showing(image):
    """A message box: its left border, a line down the middle of the screen
    that no row of the file list draws."""
    width, height, pixels = image
    for x in range(70, 80):
        drawn = 0
        for y in range(70, 165):
            r, g, b = pixels[(y * width + x) * 3:(y * width + x) * 3 + 3]
            selected = b > 200 and r < 100
            if (r, g, b) != (255, 255, 255) and not selected:
                drawn += 1
        if drawn > 80:
            return True
    return False


def close_dialogs():
    for _ in range(4):
        image = screen()
        if image is None:
            return False
        if not dialog_showing(image):
            return True
        link("key", KEY_ENTER)
        time.sleep(1.5)
    return False


def others_under_q():
    status, output = link("ls", "/")
    names = [line.split()[-1] for line in output.splitlines() if line.strip()]
    return [name for name in names if name.lower().startswith("q") and name.lower() != "q_demo.tns"]


def averages(log):
    """The profile's sections, averaged over the blocks printed while the
    demo played."""
    start = log.find("demo: playing")
    end = log.find("demo: done")
    lines = log[start:end].splitlines() if start >= 0 and end >= 0 else []
    totals, order, blocks, inside = {}, [], 0, False
    for line in lines:
        if re.match(r"^frame \d+:.*clears", line):
            blocks += 1
            inside = True
            continue
        match = re.match(r"^ +(.+): (\d+) ms$", line)
        if inside and match:
            name, value = match.group(1), int(match.group(2))
            if name not in totals:
                totals[name] = 0
                order.append(name)
            totals[name] += value
        elif not line.startswith(" "):
            inside = False
    return blocks, [(name, totals[name] / blocks) for name in order] if blocks else []


def launch():
    """q and enter in the file browser, again until the calculator leaves the
    bus (a box shown late, or a key lost, has kept it from starting before)."""
    for attempt in range(3):
        if not close_dialogs():
            sys.exit("the screen shows a box that would not close (build/nspire/bench/screen.ppm)")
        link("key", KEY_Q)
        time.sleep(2)
        link("key", KEY_ENTER)
        sent = time.time()
        while time.time() - sent < 45:
            time.sleep(5)
            if not connected():
                return
        print(f"  still on USB after enter (try {attempt + 1}); again", flush=True)
    sys.exit("the demo did not start (build/nspire/bench/screen.ppm)" if screen() else "the demo did not start")


def run(tag, index, out_dir):
    launch()
    launched = time.time()
    print(f"run {index}: launched at {time.strftime('%H:%M:%S')}", flush=True)

    # over: back on the bus
    while not connected():
        if time.time() - launched > 30 * 60:
            sys.exit("the calculator has been off USB for 30 minutes: stuck?")
        time.sleep(15)
    time.sleep(3)

    log_path = out_dir / f"{tag}-{index}.txt"
    status, output = link("get", "/halo_log.txt.tns", str(log_path), timeout=300)
    if status:
        sys.exit(f"could not fetch the log: {output.strip()}")
    log = log_path.read_text(errors="replace")
    done = re.search(r"demo: done: .*", log)
    print(f"run {index}: {done.group(0) if done else 'no demo: done line (crashed?)'}")
    for line in re.findall(r"demo: out of step.*", log):
        print(f"run {index}: {line}")
    crash = re.search(r"Halo crashed.*|exception.*caught.*", log, re.IGNORECASE)
    if crash:
        print(f"run {index}: {crash.group(0)}")
    print(f"run {index}: log in {log_path}")
    return log


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--upload", action="store_true", help="send build/nspire/halo.tns first")
    parser.add_argument("--tag", default=time.strftime("%m%d-%H%M"), help="name of the logs kept")
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--sections", action="store_true", help="print the profile's sections")
    arguments = parser.parse_args()

    out_dir = Path("build/nspire/bench")
    out_dir.mkdir(parents=True, exist_ok=True)
    if not connected():
        sys.exit("no calculator on USB")
    clash = others_under_q()
    if clash:
        sys.exit(f"other files start with q, which typing q would select instead: {clash}")
    if arguments.upload:
        for remote in ("/q_demo.tns", "/halo_demo.tns", "/halo.tns"):
            for attempt in range(3):
                status, output = link("put", "build/nspire/halo.tns", remote, timeout=600)
                print(output.strip())
                if not status:
                    break
            else:
                sys.exit(f"could not send {remote}")

    for index in range(1, arguments.runs + 1):
        log = run(arguments.tag, index, out_dir)
        if arguments.sections:
            blocks, sections = averages(log)
            print(f"sections, ms a frame, over {blocks} profile blocks:")
            for name, value in sections:
                if value >= 2:
                    print(f"  {value:6.0f}  {name}")


if __name__ == "__main__":
    main()
