#!/usr/bin/env python3
"""Send commands to the PiCoCo console over its serial port and print the
replies. Stdlib only (no pyserial).

Usage: pconsole.py <port> 'cmd' ['cmd' ...]
"""
import os
import sys
import termios
import time

TIMEOUT_S = 6.0


def open_port(path):
    # O_NONBLOCK: opening a tty node blocks waiting for carrier detect on
    # macOS otherwise; the VMIN/VTIME read loop below still works non-blocking.
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    iflag, oflag, cflag, lflag, ispeed, ospeed, cc = termios.tcgetattr(fd)
    iflag = 0
    oflag = 0
    cflag &= ~(termios.PARENB | termios.CSTOPB | termios.CSIZE)
    cflag |= termios.CREAD | termios.CLOCAL | termios.CS8
    lflag = 0
    cc[termios.VMIN] = 0
    cc[termios.VTIME] = 1
    termios.tcsetattr(fd, termios.TCSANOW,
                       [iflag, oflag, cflag, lflag, termios.B115200, termios.B115200, cc])
    return fd


def send_and_wait(fd, cmd):
    os.write(fd, (cmd + "\r\n").encode())
    buf = b""
    deadline = time.monotonic() + TIMEOUT_S
    while time.monotonic() < deadline:
        try:
            chunk = os.read(fd, 256)
        except BlockingIOError:
            chunk = b""
        if not chunk:
            time.sleep(0.05)
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.rstrip(b"\r").decode(errors="replace")
            print(line)
            if line == "ok" or line.startswith("err"):
                return
    print("<timeout>")


def main():
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} <port> 'cmd' ['cmd' ...]", file=sys.stderr)
        sys.exit(1)
    fd = open_port(sys.argv[1])
    try:
        for cmd in sys.argv[2:]:
            send_and_wait(fd, cmd)
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
