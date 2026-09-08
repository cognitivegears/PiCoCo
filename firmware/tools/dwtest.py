#!/usr/bin/env python3
"""DriveWire protocol exerciser for picoco-host. Stdlib only."""
import argparse
import os
import socket
import sys

DW_HDBDOS_SECTORS = 630


class DW:
    def __init__(self, host, port, timeout=2.0):
        self.s = socket.create_connection((host, port), timeout=timeout)

    def recv(self, n):
        buf = b''
        while len(buf) < n:
            chunk = self.s.recv(n - len(buf))
            if not chunk:
                raise RuntimeError(f'connection closed, wanted {n} got {len(buf)}')
            buf += chunk
        return buf

    def dwinit(self):
        # Client byte 0x80 (not a real drive number, so the server's
        # hdbdos-auto-off rule for NitrOS-9/CoCoBoot/LWOS clients doesn't
        # fire and clobber whatever --hdbdos mode this run wants).
        self.s.sendall(b'\x5a\x80')
        assert self.recv(1) != b'\x00'

    def read(self, drive, lsn):
        self.s.sendall(bytes([0x52, drive]) + lsn.to_bytes(3, 'big'))
        rc = self.recv(1)[0]
        if rc != 0:
            return rc, None, None
        r = self.recv(258)
        return rc, r[0:2], r[2:]

    def readex(self, drive, lsn, corrupt=False):
        self.s.sendall(bytes([0xD2, drive]) + lsn.to_bytes(3, 'big'))
        data = self.recv(256)
        cs = sum(data) & 0xFFFF
        if corrupt:
            cs ^= 1
        self.s.sendall(cs.to_bytes(2, 'big'))
        rc = self.recv(1)[0]
        return rc, data

    def write(self, drive, lsn, data, corrupt=False):
        cs = sum(data) & 0xFFFF
        if corrupt:
            cs ^= 1
        self.s.sendall(bytes([0x57, drive]) + lsn.to_bytes(3, 'big') + data + cs.to_bytes(2, 'big'))
        return self.recv(1)[0]

    def time(self):
        self.s.sendall(b'\x23')
        return self.recv(6)


def header_offset(path, size):
    """Matches dw_disk_open's header detection for the RAW/JVC/VDK cases
    (OS9 sniffing is skipped: not reachable with the raw .dsk images this
    test builds)."""
    with open(path, 'rb') as f:
        hdr = f.read(4)
    if len(hdr) >= 4 and hdr[0:2] == b'dk':
        return int.from_bytes(hdr[2:4], 'little')
    rem = size % 256
    return rem if rem else 0


def addr(hdbdos, drive, lsn):
    """Wire (drive, lsn) for a logical (drive, lsn), per the HDB-DOS
    drive-by-LSN convention: lsn' = lsn + drive*630, drive byte 0."""
    if hdbdos:
        return 0, lsn + drive * DW_HDBDOS_SECTORS
    return drive, lsn


def check_dwinit(dw):
    dw.dwinit()
    return True


def check_image(dw, image, drive, hdbdos):
    size = os.path.getsize(image)
    offset = header_offset(image, size)
    sectors = (size - offset) // 256
    ok = True
    with open(image, 'rb') as f:
        f.seek(offset)
        for lsn in range(sectors):
            expect = f.read(256)
            d, l = addr(hdbdos, drive, lsn)
            rc, data = dw.readex(d, l)
            if rc != 0 or data != expect:
                print(f'    sector {lsn}: rc={rc:#x} match={data == expect}')
                ok = False
            if (lsn + 1) % 100 == 0:
                print(f'    ...{lsn + 1}/{sectors}')
    return ok


def check_read_checksum(dw, drive, hdbdos):
    d, l = addr(hdbdos, drive, 0)
    rc, cksum, data = dw.read(d, l)
    return rc == 0 and int.from_bytes(cksum, 'big') == (sum(data) & 0xFFFF)


def check_readex_corrupt(dw, drive, hdbdos):
    d, l = addr(hdbdos, drive, 0)
    rc, _ = dw.readex(d, l, corrupt=True)
    return rc == 0xF3


def check_unmounted(dw, hdbdos):
    d, l = addr(hdbdos, 3, 0)
    rc, cksum, data = dw.read(d, l)
    return rc == 0xF6 and cksum is None and data is None


def check_read_unmounted_one_byte(dw, hdbdos):
    """READ of an unmounted drive replies with exactly one byte (the error
    code) and nothing else follows on the wire."""
    d, l = addr(hdbdos, 3, 0)
    dw.s.sendall(bytes([0x52, d]) + l.to_bytes(3, 'big'))
    rc = dw.recv(1)[0]
    if rc != 0xF6:
        return False
    prev_timeout = dw.s.gettimeout()
    dw.s.settimeout(0.5)
    try:
        dw.s.recv(4096)  # any bytes, even b'' from a closed socket, mean it wasn't silent
        return False
    except socket.timeout:
        return True
    finally:
        dw.s.settimeout(prev_timeout)


def check_dwinit_nonzero(dw):
    """DWINIT replies with a non-zero protocol version byte (spec requires
    non-zero; DW4 sends 4)."""
    dw.s.sendall(b'\x5a\x80')
    r = dw.recv(1)
    return r != b'\x00'


def check_scratch(dw, drive, hdbdos):
    pattern = bytes([0xA5] * 256)
    d, l = addr(hdbdos, drive, 7)
    if dw.write(d, l, pattern) != 0:
        return False
    rc, data = dw.readex(d, l)
    if rc != 0 or data != pattern:
        return False
    if dw.write(d, l, pattern, corrupt=True) != 0xF3:
        return False
    rc, data = dw.readex(d, l)
    return rc == 0 and data == pattern


def check_time(dw):
    t = dw.time()
    return t[0] >= 126


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=65504)
    ap.add_argument('--image', required=True)
    ap.add_argument('--drive', type=int, default=0)
    ap.add_argument('--scratch')
    ap.add_argument('--hdbdos', action='store_true')
    args = ap.parse_args()

    dw = DW(args.host, args.port)
    fails = 0

    def run(n, name, fn):
        nonlocal fails
        try:
            ok = fn()
        except Exception as e:
            ok = False
            print(f'({n}) {name}: FAIL ({e})')
        else:
            print(f'({n}) {name}: {"ok" if ok else "FAIL"}')
        if not ok:
            fails += 1

    run(1, 'dwinit', lambda: check_dwinit(dw))
    run(2, 'image sectors match', lambda: check_image(dw, args.image, args.drive, args.hdbdos))
    run(3, 'read checksum', lambda: check_read_checksum(dw, args.drive, args.hdbdos))
    run(4, 'readex corrupt -> E_CRC', lambda: check_readex_corrupt(dw, args.drive, args.hdbdos))
    run(5, 'unmounted drive -> E_NOTRDY', lambda: check_unmounted(dw, args.hdbdos))
    if args.scratch:
        # ponytail: no --scratch-drive flag exists; the scratch image is
        # assumed mounted one drive number above --drive, matching the
        # brief's integration example (t.dsk on 0, s.dsk on 1).
        run(6, 'scratch write/readex/corrupt', lambda: check_scratch(dw, args.drive + 1, args.hdbdos))
    run(7, 'time', lambda: check_time(dw))
    run(8, 'read of unmounted drive is exactly one byte', lambda: check_read_unmounted_one_byte(dw, args.hdbdos))
    run(9, 'dwinit replies non-zero', lambda: check_dwinit_nonzero(dw))

    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
