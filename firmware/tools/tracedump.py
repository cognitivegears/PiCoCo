#!/usr/bin/env python3
"""Decodes `trace dump` output (lines of "seq idx R|W data", as printed by
the console's "trace dump [n]" command; seq is the event count since boot)
into a readable, address-mapped, DriveWire-aware log.

Usage: tracedump.py [FILE]   (reads stdin if FILE is omitted or "-")
"""
import sys

# DriveWire opcodes worth naming (see firmware/src/dw/dw.h).
OPS = {
    0x00: "NOP", 0x01: "NAMEOBJ_MOUNT", 0x02: "NAMEOBJ_CREATE",
    0x23: "TIME", 0x43: "SERREAD", 0x44: "SERGETSTAT", 0x45: "SERINIT",
    0x46: "PRINTFLUSH", 0x47: "GETSTAT", 0x49: "INIT", 0x50: "PRINT",
    0x52: "READ", 0x53: "SETSTAT", 0x54: "TERM", 0x57: "WRITE",
    0x5A: "DWINIT", 0x63: "SERREADM", 0x64: "SERWRITEM", 0x72: "REREAD",
    0x77: "REWRITE", 0xC3: "SERWRITE", 0xC4: "SERSETSTAT", 0xC5: "SERTERM",
    0xD2: "READEX", 0xF2: "REREADEX", 0xF8: "RESET3", 0xFE: "RESET2",
    0xFF: "RESET1",
}

# Ops whose first 4 payload bytes are drive + 3-byte big-endian LSN.
LSN_OPS = {0x52, 0x72, 0xD2, 0xF2, 0x57, 0x77}

BECKER_STATUS = 0x3F41
BECKER_DATA = 0x3F42
ROM_END = 0x2000


def op_name(op):
    return OPS.get(op, "UNKNOWN(%#04x)" % op)


def decode(lines, out):
    # ponytail: tracks only the 4 header bytes (drive+lsn) of a request; the
    # 256 data + 2 checksum bytes that follow a WRITE/REWRITE are re-read as
    # fresh opcodes afterward. Fine for a display tool; a full payload
    # tracker would need to duplicate dw.h's payload_len() table here.
    pending_op = None
    pending = []

    for line in lines:
        line = line.strip()
        if not line:
            continue
        # ponytail: skip anything that isn't a "seq idx R|W data" trace
        # line (e.g. the console's "ok"/"err ..." replies) instead of
        # requiring a pre-filtered input file.
        parts = line.split()
        if len(parts) != 4 or parts[2] not in ("R", "W"):
            continue
        seq_s, idx_s, rw, data_s = parts
        try:
            seq = int(seq_s)
            idx = int(idx_s, 16)
            data = int(data_s, 16)
        except ValueError:
            continue
        addr = 0xC000 + idx

        note = ""
        if idx < ROM_END:
            note = "ROM"
        elif idx == BECKER_STATUS:
            note = "BECKER_STATUS"
            if rw == "R":
                note += " data avail" if (data & 2) else " no data"
        elif idx == BECKER_DATA:
            note = "BECKER_DATA"
            if rw == "W":
                if pending_op is None:
                    note += " " + op_name(data)
                    if data in LSN_OPS:
                        pending_op = data
                        pending = []
                else:
                    pending.append(data)
                    if len(pending) == 4:
                        drive = pending[0]
                        lsn = (pending[1] << 16) | (pending[2] << 8) | pending[3]
                        note += " %s drive=%d lsn=%d" % (op_name(pending_op), drive, lsn)
                        pending_op = None

        out.write("%10d  $%04X  %s  %02X  %s\n" % (seq, addr, rw, data, note))


def main():
    if len(sys.argv) > 1 and sys.argv[1] != "-":
        with open(sys.argv[1]) as f:
            decode(f, sys.stdout)
    else:
        decode(sys.stdin, sys.stdout)


if __name__ == "__main__":
    main()
