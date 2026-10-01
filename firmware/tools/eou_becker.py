#!/usr/bin/env python3
"""Remaster an EOU CoCo SDC hard-disk image to boot over the Becker port.

    eou_becker.py 63SDC.VHD 63BECKER.VHD

Needs toolshed's `os9` on PATH. Everything swapped in comes from the image
itself: boot_dw_becker for the kernel track; dwio_becker and ddx0 (/DD =
DriveWire drive 0) for the boot file, with the CoCo SDC modules dropped
because their registers overlap the Becker port at $FF41/$FF42. See
firmware/TEST_PLAN.md section H.1.
"""
import re, shutil, subprocess, sys, tempfile, os

MOD = "MODULES/6309L2/MODULES"          # 6809 image: 6809L2
DROP = {"RBSuper", "llcocosdc", "H1"}
KTRACK_LSN, KTRACK_LEN = 612, 4608      # track 34, 18 sectors


def os9(*a):
    subprocess.run(["os9", *a], check=True)


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
    mod = MOD.replace("6309", "6809") if "68" in os.path.basename(src) else MOD
    shutil.copyfile(src, dst)
    with tempfile.TemporaryDirectory() as t:
        def get(path):
            f = os.path.join(t, os.path.basename(path))
            os9("copy", "-r", f"{dst},{path}", f)
            return open(f, "rb").read()

        kpre, ktrack, ktail = modules(get("KERNEL_TRACKS/kernel.dw"))
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
        st = subprocess.run(["os9", "fstat", f"{dst},BOOTS/OS9Boot.dw"],
                            check=True, capture_output=True, text=True).stdout
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
            assert f.read(2) == b"OS"
            f.seek(KTRACK_LSN * 256)
            f.write(kt)
        # EOU's SDC/GIME-X detection writes $64/$00 pairs to $FF42, the Becker
        # data port, and desyncs DriveWire. Its own switch for Becker machines
        # (the CoCo3FPGA flag in every env.file) skips that detection.
        img = open(dst, "rb").read()
        assert b"COCO3FPGA=0" in img
        open(dst, "wb").write(img.replace(b"COCO3FPGA=0", b"COCO3FPGA=1"))
        # The SDC startup asks for the time (`setime`); the DriveWire one does not.
        sf = os.path.join(t, "startup.dw")
        os9("copy", "-r", f"{dst},BOOTS/startup.dw", sf)
        os9("copy", "-r", sf, f"{dst},startup")
        # ponytail: no separate SWAPBOOT set; the "dw" set is now the Becker one.
        # Picking another set in SWAPBOOT un-does the boot pointer; re-run this.

if __name__ == "__main__":
    main(*sys.argv[1:3])
