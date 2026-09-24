#!/usr/bin/env python3
"""Capture the CoCo 32x16 text screen ($0400-$05FF) from a running XRoar
instance via its GDB remote stub (-gdb, default port 65520) and print it
as 16 lines of 32 characters.

Usage: xrscreen.py [host] [port]
  default host 127.0.0.1, default port 65520

No third-party dependencies: stdlib socket only, speaks just enough of
the GDB remote serial protocol to send an 'm' (read memory) packet.
"""
import socket
import sys

SCREEN_ADDR = 0x0400
SCREEN_LEN = 0x0200  # 32 * 16 = 512 bytes


def checksum(data: bytes) -> str:
    return "%02x" % (sum(data) % 256)


def send_packet(sock: socket.socket, body: bytes) -> None:
    pkt = b"$" + body + b"#" + checksum(body).encode("ascii")
    sock.sendall(pkt)


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


def read_memory(sock: socket.socket, addr: int, length: int) -> bytes:
    send_packet(sock, f"m{addr:x},{length:x}".encode("ascii"))
    recv_ack(sock)
    reply = recv_packet(sock)
    if reply.startswith(b"E"):
        raise RuntimeError(f"GDB stub error reply: {reply!r}")
    return bytes.fromhex(reply.decode("ascii"))


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


def main() -> int:
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 65520
    with socket.create_connection((host, port), timeout=5) as sock:
        data = read_memory(sock, SCREEN_ADDR, SCREEN_LEN)
    print(render(data))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
