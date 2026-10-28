#!/usr/bin/env python3
"""Host smoke test for picoco-dos.rom.

Checks the image: size, the DK header, the vector table ($C002 init, [$C004]
DSKCON, [$C006] variables), the CoCo 3 patch zones and the MIT header in
every ROM source. With --xroar it also boots the image headless in XRoar on a
CoCo 1 (Color BASIC 1.0, Extended BASIC 1.0), a CoCo 2 and a CoCo 3
(stdout trace, no network) and checks that the banner
goes out through CHROUT and that BASIC then reads the keyboard through our
RAM hook, which is where it sits at the OK prompt.

Copyright (c) 2026 Nathan Byrd
SPDX-License-Identifier: MIT
"""
import glob
import os
import re
import subprocess
import sys

ROM_BASE = 0xC000
ROM_SIZE = 8192
BANNER = b"PICOCO-DOS 0.1\r"
RVEC4 = 0x016A


def rom_target(d, off, what):
    assert d[off] == 0x7E, f"{what}: no JMP at ${ROM_BASE + off:04X}"
    tgt = int.from_bytes(d[off + 1:off + 3], "big")
    assert ROM_BASE <= tgt < ROM_BASE + ROM_SIZE, f"{what}: target ${tgt:04X} is outside the ROM"
    return tgt


def check_image(path):
    d = open(path, "rb").read()
    assert len(d) == ROM_SIZE, f"size {len(d)}, want {ROM_SIZE}"
    assert d[0:2] == b"DK", "no DK signature at $C000"
    assert d[2] == 0x20, "$C002 must be a 2-byte BRA so the vectors sit at $C004"
    assert d[3] < 0x80 and 4 + d[3] < ROM_SIZE, "$C002 BRA target is outside the ROM"
    dskcon = int.from_bytes(d[4:6], "big")
    assert ROM_BASE <= dskcon < ROM_BASE + ROM_SIZE, f"[$C004] DSKCON ${dskcon:04X} is outside the ROM"
    assert d[4] != 0xD6, "byte $C004 is $D6: a CoCo 3 would patch $C0C6 instead of $C0D9"
    assert d[6:8] == b"\x00\xEA", "[$C006] must point at the DSKCON variables at $00EA"
    rom_target(d, 0xD9, "$C0D9 exit")
    assert not any(d[0xC6:0xD9]), "$C0C6-$C0D8 must be zero (CoCo 3 patch zone)"
    assert not any(d[0x8B4:0x8BF]), "$C8B4-$C8BE must be zero (CoCo 3 patch zone)"
    assert BANNER[:-1] in d, "banner text missing"
    for src in glob.glob(os.path.join(os.path.dirname(path) or ".", "*.asm")):
        assert "SPDX-License-Identifier: MIT" in open(src).read(), f"{src}: no MIT licence header"


TRACE = re.compile(r"^([0-9a-f]{4})\|\s+[0-9a-f]+\s+(\w+)\s+(\S*)\s+cc=\S+ a=([0-9a-f]{2})", re.I)


def boot_xroar(rom, machine, seconds, *roms):
    cmd = ["xroar", "-ui", "null", "-ao", "null", "-machine", machine, "-rompath",
           os.path.expanduser("~/.xroar/roms"), "-cart-rom", os.path.abspath(rom),
           "-trace", "-timeout", str(seconds), *roms]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    entered = hooked = False
    printed = bytearray()
    for line in p.stdout:
        m = TRACE.match(line)
        if not m:
            continue
        addr, op, arg, a = int(m[1], 16), m[2].upper(), m[3].lower(), int(m[4], 16)
        if addr == 0xC002:
            entered = True
        elif ROM_BASE <= addr < ROM_BASE + ROM_SIZE and op == "JSR" and arg == "[$a002]":
            printed.append(a)
        elif addr == RVEC4 and op == "JMP" and arg.startswith("$c"):
            hooked = True
            p.kill()
            break
    p.wait()
    assert entered, f"{machine}: BASIC never jumped to $C002 (DK not taken)"
    assert bytes(printed) == BANNER, f"{machine}: CHROUT saw {bytes(printed)!r}, want {BANNER!r}"
    assert hooked, f"{machine}: BASIC never read the keyboard through the hook at $016A (no OK prompt?)"
    print(f"{machine}: banner printed, OK prompt reached through the hook")


def main():
    xroar = "--xroar" in sys.argv
    rom = [a for a in sys.argv[1:] if a != "--xroar"][0]
    check_image(rom)
    print(f"{rom}: header, vectors, patch zones and licence headers ok")
    if xroar:
        boot_xroar(rom, "cocous", 6, "-bas", "bas10", "-extbas", "extbas10")  # a 1980 CoCo 1
        boot_xroar(rom, "coco2bus", 6)
        boot_xroar(rom, "coco3", 10)


if __name__ == "__main__":
    main()
