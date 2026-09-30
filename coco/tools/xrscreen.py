#!/usr/bin/env python3
"""Capture the CoCo 32x16 text screen ($0400-$05FF) from a running XRoar
instance via its GDB remote stub (-gdb, default port 65520), and optionally
poll until a target string appears.

IMPORTANT - one xrscreen.py session per XRoar instance. XRoar's GDB stub
only tolerates a single client connection per run: once a connection is
closed, a second, separate connection from a fresh process reliably hangs
(TCP connect succeeds, but no reply ever arrives to the first command sent).
So all the work this script does - one-shot screen read, or polling for
several strings in turn - happens on ONE TCP connection held open for the
whole run. Do not invoke this script twice against the same XRoar instance;
if you need to observe more state, add more --wait-for targets to a single
invocation. See coco/README.md for the reasoning and a worked example.

Also note: while a command below is talking to the GDB stub, the emulated
CPU is halted (frozen) - including its DriveWire traffic to picoco-host.
Every poll cycle re-resumes the CPU before sleeping, and every exit path
(success, timeout, or error) resumes the CPU (sends a GDB 'c' continue)
before closing the socket, so the emulator is left running when this
script exits.

Usage:
  xrscreen.py [--host HOST] [--port PORT]
              [--wait-for TEXT [--wait-for TEXT ...]]
              [--timeout SECONDS] [--interval SECONDS]
              [--connect-timeout SECONDS]
  xrscreen.py --selftest

With no --wait-for, connects, takes one screenshot, resumes, and prints it.
With one or more --wait-for TEXT, polls (read / continue / sleep / interrupt
/ read again, all on the one connection) until each TEXT appears on screen,
in order, printing the screen at each match; the CPU keeps running between
matches, so this is how to watch a running BASIC program reach several
states in turn (e.g. the same "OK" prompt reappearing after each of several
commands finishes). Exits 0 if every target was found, 1 if any timed out
(printing the last screen seen for that target first).

No third-party dependencies: stdlib socket only, speaks just enough of the
GDB remote serial protocol to read memory ('m'), continue ('c'), and send
an interrupt (raw \\x03, expecting a stop-reply like "S02").
"""
import argparse
import socket
import time

SCREEN_ADDR = 0x0400
SCREEN_LEN = 0x0200  # 32 * 16 = 512 bytes


def checksum(data: bytes) -> str:
    return "%02x" % (sum(data) % 256)


def format_packet(body: bytes) -> bytes:
    return b"$" + body + b"#" + checksum(body).encode("ascii")


def send_packet(sock: socket.socket, body: bytes) -> None:
    sock.sendall(format_packet(body))


def recv_ack(sock: socket.socket) -> None:
    # Expect a single '+' ack byte (skip any stray bytes before it).
    while True:
        c = sock.recv(1)
        if not c:
            raise ConnectionError("connection closed waiting for ack")
        if c == b"+":
            return


def recv_packet(sock: socket.socket) -> bytes:
    # Read until '$', then until '#', then the 2-char checksum.
    buf = b""
    while True:
        c = sock.recv(1)
        if not c:
            raise ConnectionError("connection closed waiting for packet start")
        if c == b"$":
            break
    while True:
        c = sock.recv(1)
        if not c:
            raise ConnectionError("connection closed reading packet body")
        if c == b"#":
            break
        buf += c
    sock.recv(2)  # checksum, not verified
    sock.sendall(b"+")  # ack
    return buf


# How long to wait for a single GDB stub reply (e.g. the stop-reply after an
# interrupt) once connected. A busy interpreted BASIC loop can go a good
# while between points where XRoar's GDB stub checks for and services debug
# commands, so this needs to be generous - much more than the per-poll
# --interval sleep.
# ponytail: fixed ceiling; raise this (or make it a flag) if a target program
# can legitimately run uninterrupted for minutes rather than tens of seconds.
GDB_REPLY_TIMEOUT = 30.0


def connect_with_retry(host: str, port: int, timeout: float) -> socket.socket:
    """A fresh XRoar instance sometimes isn't listening yet a few seconds
    after launch, so retry the connect itself (not just reads) for up to
    `timeout` seconds."""
    deadline = time.monotonic() + timeout
    last_exc = None
    while time.monotonic() < deadline:
        try:
            sock = socket.create_connection((host, port), timeout=5)
            sock.settimeout(GDB_REPLY_TIMEOUT)
            return sock
        except OSError as exc:
            last_exc = exc
            time.sleep(0.2)
    raise ConnectionError(f"could not connect to {host}:{port}: {last_exc}")


def read_memory(sock: socket.socket, addr: int, length: int) -> bytes:
    send_packet(sock, f"m{addr:x},{length:x}".encode("ascii"))
    recv_ack(sock)
    reply = recv_packet(sock)
    if reply.startswith(b"E"):
        raise RuntimeError(f"GDB stub error reply: {reply!r}")
    return bytes.fromhex(reply.decode("ascii"))


def resume(sock: socket.socket) -> None:
    """Send a GDB 'continue' and wait for its ack. Does NOT wait for a stop
    reply - the target is now running and won't send one until it next
    halts."""
    send_packet(sock, b"c")
    recv_ack(sock)


def interrupt_and_wait_stopped(sock: socket.socket) -> None:
    """Send a raw GDB interrupt byte and wait for the resulting stop-reply
    packet (e.g. "S02")."""
    sock.sendall(b"\x03")
    recv_packet(sock)  # e.g. b"S02"; contents not otherwise used


def read_screen(sock: socket.socket) -> bytes:
    return read_memory(sock, SCREEN_ADDR, SCREEN_LEN)


def decode_byte(b: int) -> str:
    if b & 0x80:
        return "#"  # semigraphics
    c = b & 0x3F
    ch = chr(c + 0x40) if c < 0x20 else chr(c)
    if not (b & 0x40):
        # inverse video: mark by lowercasing
        ch = ch.lower()
    return ch


def render(data: bytes) -> str:
    lines = []
    for row in range(16):
        chunk = data[row * 32:(row + 1) * 32]
        lines.append("".join(decode_byte(b) for b in chunk))
    return "\n".join(lines)


def wait_for(sock: socket.socket, target: str, timeout: float, interval: float,
             halt_first: bool = False) -> tuple:
    """Poll (read / continue / sleep / interrupt / read again) on the given,
    already-open connection until `target` appears in the rendered screen,
    or `timeout` seconds elapse. Leaves the CPU halted either way (caller
    resumes when ready to move on or exit). Returns (found: bool, screen: str).

    Pass halt_first=True when the CPU is currently running (e.g. this is not
    the first wait_for() call on this connection, and a previous call left it
    running via resume()) - a fresh connection's target is already halted, so
    the very first call on a connection should leave this False. When True, a
    short sleep happens before the halt, so the text that satisfied the
    previous wait_for() call has a chance to change before we look again
    (otherwise a still-on-screen match from the previous target could cause
    an instant, meaningless "found" here)."""
    deadline = time.monotonic() + timeout
    if halt_first:
        time.sleep(interval)
        interrupt_and_wait_stopped(sock)
    screen = render(read_screen(sock))
    while target not in screen:
        if time.monotonic() >= deadline:
            return False, screen
        resume(sock)
        time.sleep(interval)
        interrupt_and_wait_stopped(sock)
        screen = render(read_screen(sock))
    return True, screen


def selftest() -> int:
    # checksum(): known GDB packet bodies.
    assert checksum(b"") == "00", checksum(b"")
    assert checksum(b"c") == "63", checksum(b"c")  # ord('c') = 99 = 0x63
    assert checksum(b"m400,200") == "bf", checksum(b"m400,200")
    assert format_packet(b"c") == b"$c#63"

    # decode_byte(): normal text, inverse text, semigraphics.
    assert decode_byte(0x41) == "A"  # bit6 set (normal), c=0x01 -> 'A'
    assert decode_byte(0x01) == "a"  # bit6 clear (inverse) -> lowercased
    assert decode_byte(0x80) == "#"  # bit7 set -> semigraphics
    assert decode_byte(0xFF) == "#"

    # render(): one row spelling "OK" left-padded with spaces (0x60 = space,
    # normal video: c=0x20 -> chr(0x20) = ' ').
    row = bytes([0x60] * 32)
    row = bytes([0x4F, 0x4B]) + row[2:]  # 'O'=0x4F -> c=0x0F -> 'O'; 'K'=0x4B -> c=0x0B -> 'K'
    data = row + bytes([0x60] * 32 * 15)
    lines = render(data).split("\n")
    assert lines[0].startswith("OK"), lines[0]
    assert len(lines) == 16
    assert all(len(line) == 32 for line in lines)

    print("selftest OK")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=65520)
    ap.add_argument("--wait-for", dest="wait_for", action="append", default=[],
                     metavar="TEXT",
                     help="poll until TEXT appears on screen; repeatable, "
                          "checked in order on one connection")
    ap.add_argument("--timeout", type=float, default=15.0,
                     help="seconds to wait for each --wait-for target [15]")
    ap.add_argument("--interval", type=float, default=0.5,
                     help="seconds to let the CPU run between polls [0.5]")
    ap.add_argument("--connect-timeout", type=float, default=5.0,
                     help="seconds to retry connecting to XRoar's GDB stub [5]")
    ap.add_argument("--selftest", action="store_true",
                     help="run internal checksum/decode self-checks and exit "
                          "(no XRoar connection needed)")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    sock = connect_with_retry(args.host, args.port, args.connect_timeout)
    try:
        if not args.wait_for:
            screen = render(read_screen(sock))
            print(screen)
            return 0

        exit_code = 0
        for i, target in enumerate(args.wait_for):
            found, screen = wait_for(sock, target, args.timeout, args.interval,
                                      halt_first=(i > 0))
            label = f"[{i + 1}/{len(args.wait_for)}] wait-for {target!r}"
            print(f"--- {label}: {'found' if found else 'TIMEOUT'} ---")
            print(screen)
            if not found:
                exit_code = 1
                break
            resume(sock)  # let the machine keep running toward the next target
        return exit_code
    finally:
        # Every exit path leaves the emulator running: resume before closing.
        try:
            resume(sock)
        except OSError:
            pass
        sock.close()


if __name__ == "__main__":
    raise SystemExit(main())
