#!/usr/bin/env python3
"""Layout checks for tools/bas2rom.py: run with `python3 tools/test_bas2rom.py`."""
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bas2rom  # noqa: E402

TOOL = os.path.join(HERE, "bas2rom.py")
LOAD = 0x1E01
# 10 PRINT "HI" / 20 GOTO 20, tokenized as a disk .BAS (FF + length preamble)
LINE1 = bytes([0x1E, 0x0B, 0x00, 0x0A, 0x87, 0x22, 0x48, 0x49, 0x22, 0x00])
LINE2 = bytes([0x1E, 0x13, 0x00, 0x14, 0x81, 0x32, 0x30, 0x00])
PROG = LINE1 + LINE2 + b"\x00\x00"
BAS = b"\xFF" + len(PROG).to_bytes(2, "big") + PROG


def convert(tmp, name, data, *args):
    src = os.path.join(tmp, name)
    out = os.path.join(tmp, "out.rom")
    with open(src, "wb") as f:
        f.write(data)
    subprocess.run([sys.executable, TOOL, src, out, *args], check=True,
                   stdout=subprocess.DEVNULL)
    with open(out, "rb") as f:
        return f.read()


class Bas2RomTest(unittest.TestCase):
    def test_bas_is_16k_dk_with_program_after_stub(self):
        with tempfile.TemporaryDirectory() as tmp:
            rom = convert(tmp, "t.bas", BAS)
        self.assertEqual(len(rom), 16384)
        self.assertEqual(rom[:2], b"DK")
        self.assertEqual(bas2rom.orig_load_addr(PROG), LOAD)
        n = len(bas2rom.build_stub(LOAD, len(PROG)))
        self.assertEqual(rom[n:n + len(PROG)], PROG)

    def test_decb_bin_takes_load_and_exec_from_headers(self):
        seg1 = b"\x00\x00\x03\x3F\x00\x12\x34\x56"      # 3 bytes at $3F00
        seg2 = b"\x00\x00\x01\x3F\x03\xAB"              # 1 byte at $3F03, contiguous
        gap = b"\x00\x00\x01\x3F\x05\xAB"               # 1 byte at $3F05, gap of 2
        trailer = b"\xFF\x00\x00\x3F\x02"               # exec $3F02
        payload = b"\x12\x34\x56\xAB"
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "t.bin")
            with open(path, "wb") as f:
                f.write(seg2 + seg1 + trailer)             # out of order on disk
            self.assertEqual(bas2rom.load_decb(path), (0x3F00, 0x3F02, payload))
            with open(path, "wb") as f:
                f.write(seg1 + gap + trailer)
            with self.assertRaises(SystemExit):
                bas2rom.load_decb(path)
            rom = convert(tmp, "t.bin", seg1 + seg2 + trailer)
        self.assertEqual(len(rom), 16384)
        self.assertEqual(rom[:2], b"DK")
        n = len(bas2rom.build_ml_stub(0x3F00, len(payload), 0x3F02))
        self.assertEqual(rom[n - 3:n], b"\x7E\x3F\x02")  # JMP exec ends the stub
        self.assertEqual(rom[n:n + len(payload)], payload)

    def test_banked_pads_to_2_or_4_banks_refuses_5_and_reconstructs(self):
        with self.assertRaises(SystemExit):   # 5 banks: more than the switcher holds
            bas2rom.build_ml_banked(bytes(70000), 0x0E00, 0x0E00)
        for n, want_banks in ((20000, 2), (33000, 4)):
            payload = bytes((i * 7 + i // 251) & 0xFF for i in range(n))
            rom, nbanks, stublen = bas2rom.build_ml_banked(payload, 0x0E00, 0x0E00)
            self.assertEqual(nbanks, want_banks, n)
            self.assertEqual(len(rom), nbanks * 16384, n)
            self.assertEqual(rom[:2], b"DK")
            chunk0 = bas2rom.BANK_USABLE - stublen
            got = rom[stublen:stublen + chunk0]
            bank = 1
            while len(got) < n:
                take = min(bas2rom.BANK_USABLE, n - len(got))
                got += rom[bank * 16384:bank * 16384 + take]
                bank += 1
            self.assertEqual(got, payload, n)


if __name__ == "__main__":
    unittest.main()
