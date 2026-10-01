#!/usr/bin/env python3
"""Relay PiCoCo's bridge port (CDC0, `becker bridge`) to a TCP DriveWire
server, optionally adding network-style latency. Stdlib only.

Usage: becker_relay.py <port> [--host 127.0.0.1] [--tcp 65504]
                       [--delay MS] [--jitter MS]

--delay adds MS of one-way latency in both directions (so round trip is
2*MS); --jitter adds a uniform 0..MS extra on top of each chunk.
Prints one line per burst (a burst ends after 1 s idle) with byte counts
and duration, so DIR/LOADM timings can be compared across delays.
"""
import argparse
import collections
import fcntl
import os
import random
import select
import socket
import sys
import termios
import time


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    iflag, oflag, cflag, lflag, ispeed, ospeed, cc = termios.tcgetattr(fd)
    cflag &= ~(termios.PARENB | termios.CSTOPB | termios.CSIZE)
    cflag |= termios.CREAD | termios.CLOCAL | termios.CS8
    cc[termios.VMIN] = 0
    cc[termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW,
                      [0, 0, cflag, 0, termios.B115200, termios.B115200, cc])
    # The firmware only reads CDC0 while DTR is up (tud_cdc_n_connected).
    fcntl.ioctl(fd, termios.TIOCMBIS, termios.TIOCM_DTR.to_bytes(4, sys.byteorder))
    return fd


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--tcp", type=int, default=65504)
    ap.add_argument("--delay", type=float, default=0.0, help="one-way ms")
    ap.add_argument("--jitter", type=float, default=0.0, help="0..ms extra")
    ap.add_argument("--reconnect", action="store_true",
                    help="keep running: wait for the port to come back after a Pico reboot (every CoCo reset reboots it)")
    ap.add_argument("--log", help="append every chunk as '<t> U|D <hex>' (U = CoCo to server)")
    a = ap.parse_args()
    while True:
        try:
            if os.path.exists(a.port):
                relay(a)
        except OSError as e:                      # port vanished (Pico reboot) or server not up yet
            print(f"relay stopped: {e}", flush=True)
        if not a.reconnect:
            return
        time.sleep(1)


def relay(a):
    ser = open_port(a.port)
    sock = socket.create_connection((a.host, a.tcp))
    sock.setblocking(False)
    log = open(a.log, "a", buffering=1) if a.log else None
    print(f"relay {a.port} <-> {a.host}:{a.tcp} delay {a.delay} ms jitter {a.jitter} ms", flush=True)

    # Two delay lines: (deliver_at, bytes). Order is preserved per direction.
    to_srv = collections.deque()
    to_coco = collections.deque()
    n_up = n_down = 0
    burst_start = last_io = None

    def lat():
        return (a.delay + random.uniform(0, a.jitter)) / 1000.0

    while True:
        now = time.monotonic()
        timeout = 1.0
        for q in (to_srv, to_coco):
            if q:
                timeout = max(0.0, min(timeout, q[0][0] - now))
        r, _, _ = select.select([ser, sock], [], [], timeout)
        now = time.monotonic()
        if r and burst_start is None:
            burst_start = now
            n_up = n_down = 0
        if ser in r:
            data = os.read(ser, 4096)
            if data:
                to_srv.append((now + lat(), data))
                if log: log.write(f"{now:.4f} U {data.hex()}\n")
                n_up += len(data)
        if sock in r:
            data = sock.recv(4096)
            if not data:
                print("server closed", flush=True)
                return
            to_coco.append((now + lat(), data))
            if log: log.write(f"{now:.4f} D {data.hex()}\n")
            n_down += len(data)
        while to_srv and to_srv[0][0] <= now:
            sock.sendall(to_srv.popleft()[1])
        while to_coco and to_coco[0][0] <= now:
            buf = to_coco.popleft()[1]
            while buf:
                try:
                    w = os.write(ser, buf)
                except BlockingIOError:
                    w = 0
                if w == 0:
                    time.sleep(0.001)
                buf = buf[w:]
        if r:
            last_io = now
        elif burst_start is not None and now - last_io >= 1.0:
            print(f"burst: coco->srv {n_up} B, srv->coco {n_down} B, "
                  f"{last_io - burst_start:.2f} s", flush=True)
            burst_start = None


if __name__ == "__main__":
    main()
