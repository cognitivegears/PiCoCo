#!/usr/bin/env python3
"""Remaster an EOU CoCo SDC hard-disk image to boot over the Becker port.

    eou_becker.py 63SDC.VHD 63BECKER.VHD      (6309 image)
    eou_becker.py 68SDC.VHD 68BECKER.VHD      (6809 image)
    eou_becker.py --selftest

Needs toolshed's `os9` on PATH. Everything swapped in comes from the image
itself: the kernel track it boots with (LSN 612) keeps its REL and Krn and
gets boot_dw_becker as Boot; the boot file is BOOTS/OS9Boot.dw with
dwio_becker and ddx0 (/DD = DriveWire drive 0), the CoCo SDC modules
dropped because their registers overlap the Becker port at $FF41/$FF42.
The CPU build is read off the image (MODULES/6309L2 or 6809L2), not the
file name. See firmware/TEST_PLAN.md section H.1.
"""
import re, shutil, subprocess, sys, tempfile, os

DROP = {"RBSuper", "llcocosdc", "H1"}
KTRACK_LSN, KTRACK_LEN = 612, 4608      # track 34, 18 sectors


def os9(*a):
    return subprocess.run(["os9", *a], check=True, stdout=subprocess.PIPE, text=True).stdout


def modules(blob):
    """Split concatenated OS-9 modules into [(name, bytes)]."""
    out, i = [], blob.find(b"\x87\xcd")       # kernel track: REL has a preamble
    pre = blob[:i]
    while i + 6 <= len(blob) and blob[i:i + 2] == b"\x87\xcd":
        size = int.from_bytes(blob[i + 2:i + 4], "big")
        n = i + int.from_bytes(blob[i + 4:i + 6], "big")
        name = bytearray()
        while True:
            name.append(blob[n] & 0x7F)
            if blob[n] & 0x80:
                break
            n += 1
        out.append((name.decode(), blob[i:i + size]))
        i += size
    return pre, out, blob[i:]


def main(src, dst):
    shutil.copyfile(src, dst)
    cpu = re.search(r"(6[38]09)L2", os9("dir", f"{dst},MODULES"))
    if not cpu:
        sys.exit("eou_becker: no MODULES/6309L2 or 6809L2 on the image")
    mod = f"MODULES/{cpu.group(1)}L2/MODULES"
    with tempfile.TemporaryDirectory() as t:
        def get(path):
            f = os.path.join(t, os.path.basename(path))
            os9("copy", "-r", f"{dst},{path}", f)
            if not os.path.exists(f):           # os9 copy exits 0 on a missing path
                sys.exit(f"eou_becker: {path} is not on the image")
            return open(f, "rb").read()

        with open(dst, "rb") as f:
            f.seek(KTRACK_LSN * 256)
            kpre, ktrack, ktail = modules(f.read(KTRACK_LEN))
        assert kpre[:2] == b"OS", kpre
        _, boot, _ = modules(get("BOOTS/OS9Boot.dw"))
        new = {"Boot": get(f"{mod}/BOOTTRACK/boot_dw_becker"),
               "dwio": get(f"{mod}/RBF/dwio_becker.sb"),
               "DD": get(f"{mod}/RBF/ddx0.dd")}

        def swap(mods, names):
            have = {n for n, _ in mods}
            assert names <= have, f"missing {names - have}"
            return b"".join(new.get(n, b) if n in names else b
                            for n, b in mods if n not in DROP)

        kt = kpre + swap(ktrack, {"Boot"}) + ktail
        assert len(kt) == KTRACK_LEN, len(kt)
        bf = swap(boot, {"dwio", "DD"})

        # Reuse BOOTS/OS9Boot.dw's extent (the bitbanger set is no use on a
        # PiCoCo): `os9 gen` fragments a new file on this image and the boot
        # module needs it contiguous. The file keeps its old length, zero-padded.
        st = os9("fstat", f"{dst},BOOTS/OS9Boot.dw")
        segs = re.findall(r"\(\$([0-9A-F]+)\)\s+(\d+) sectors", st)
        assert len(segs) == 1, f"OS9Boot.dw fragmented: {segs}"
        data, room = int(segs[0][0], 16), int(segs[0][1]) * 256
        assert len(bf) <= room, (len(bf), room)
        with open(dst, "r+b") as f:
            f.seek(data * 256)
            f.write(bf.ljust(room, b"\0"))
            f.seek(0x15)                                    # DD.BT, DD.BSZ
            f.write(data.to_bytes(3, "big") + len(bf).to_bytes(2, "big"))
            f.seek(KTRACK_LSN * 256)                        # track 34
            f.write(kt)
        # EOU's SDC/GIME-X detection writes $64/$00 pairs to $FF42, the Becker
        # data port, and desyncs DriveWire. Its own switch for Becker machines
        # (the CoCo3FPGA flag in every env.file) skips that detection.
        img = open(dst, "rb").read()
        assert b"COCO3FPGA=0" in img
        open(dst, "wb").write(img.replace(b"COCO3FPGA=0", b"COCO3FPGA=1"))
        # The SDC startup asks for the time (`setime`); the DriveWire one does not.
        get("BOOTS/startup.dw")                 # lands in t/startup.dw
        os9("copy", "-r", os.path.join(t, "startup.dw"), f"{dst},startup")
        # ponytail: no separate SWAPBOOT set; the "dw" set is now the Becker one.
        # Picking another set in SWAPBOOT un-does the boot pointer; re-run this.
    print(f"{dst}: {cpu.group(1)} image, {len(bf)} byte boot file at LSN {data}")


def selftest():
    # OS-9 module header: $87CD, 2-byte size, 2-byte name offset; the name's
    # last byte has bit 7 set. Literal layout from the OS-9 module format.
    def mod(name, size):
        head = b"\x87\xcd" + size.to_bytes(2, "big") + b"\x00\x09xyz"
        return (head + name[:-1].encode() + bytes([ord(name[-1]) | 0x80])).ljust(size, b"\0")
    pre, mods, tail = modules(b"OS -\x12\x05" + mod("REL", 40) + mod("Boot", 64) + b"\x01\x02")
    assert pre == b"OS -\x12\x05", pre
    assert [(n, len(b)) for n, b in mods] == [("REL", 40), ("Boot", 64)], mods
    assert tail == b"\x01\x02", tail
    print("selftest ok")


if __name__ == "__main__":
    selftest() if sys.argv[1:] == ["--selftest"] else main(*sys.argv[1:3])
