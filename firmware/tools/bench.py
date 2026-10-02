#!/usr/bin/env python3
"""Run the on-device self-tests over the USB console. Stdlib only.

Usage: bench.py [--port /dev/cu.usbmodemXXXX3] [--skip-if-absent]
Exit 0 pass, 1 fail, 77 skipped (no Pico console found and --skip-if-absent).
The console is CDC1: the *second* of the two usbmodem nodes a PiCoCo creates.
"""
import argparse
import glob
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pconsole import open_port  # noqa: E402

TIMEOUT_S = 60.0  # per command; `bus selftest` takes ~2 s


def run(fd, cmd):
    os.write(fd, (cmd + "\r\n").encode())
    lines, buf = [], b""
    deadline = time.monotonic() + TIMEOUT_S
    while time.monotonic() < deadline:
        try:
            chunk = os.read(fd, 256)
        except BlockingIOError:
            chunk = b""
        if not chunk:
            time.sleep(0.02)
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.rstrip(b"\r").decode(errors="replace")
            lines.append(line)
            if line == "ok" or line.startswith("err"):
                return lines
    lines.append("<timeout>")
    return lines


def find_port():
    nodes = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
    return nodes[1] if len(nodes) >= 2 else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--skip-if-absent", action="store_true")
    a = ap.parse_args()
    port = a.port or find_port()
    if not port:
        print("no Pico console found")
        return 77 if a.skip_if_absent else 1
    fd = open_port(port)
    ok = True
    for line in run(fd, "version"):
        print(line)
    out = run(fd, "bus selftest")
    for line in out:
        print(line)
        if "FAIL" in line or line.startswith("err") or line == "<timeout>":
            ok = False
    if not any(l == "selftest fast pass" for l in out):
        ok = False
    # A saved config may have loaded a ROM the self-test cleared; replay it.
    # Fire-and-forget: a reboot drops the USB connection before replying "ok".
    os.write(fd, b"reboot\r\n")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
