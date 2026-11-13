#!/usr/bin/env python3
"""wifi-tcp-host.py - the host side of the rpi-zero-2w wifi-tcp-server test.

Connects to the board's TCP echo server (port 7), sends a deterministic
pattern, checks that every byte comes back, closes, and prints the size and
CRC-32 of what it sent. The board prints the same two numbers for what it
received and saved to tests/WIFIRX.BIN on its SD card.

    wifi-tcp-host.py 192.168.1.65                 the board's address
    wifi-tcp-host.py --serial /dev/ttyACM1        read the address from the
                                                 board's console (115200 8N1)
    wifi-tcp-host.py 192.168.1.65 --file FILE     send a file instead of the pattern
    options: --size BYTES (default 262144)  --port N (default 7)
             --timeout SECONDS to wait for the console line (default 90)

With --serial the script keeps reading the console after the transfer and
compares its CRC-32 with the one the board reports. Standard library only.
"""
import argparse
import os
import re
import socket
import sys
import termios
import time
import zlib


def pattern(size):
    """The LCG the SD tests use, so a file can be regenerated anywhere."""
    out = bytearray(size)
    x = 0x5A325731  # "Z2W1"
    for i in range(size):
        x = (x * 1664525 + 1013904223) & 0xFFFFFFFF
        out[i] = x >> 24
    return bytes(out)


def open_serial(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = attrs[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def read_lines(fd, seconds):
    """Yield console lines for up to `seconds`."""
    deadline = time.monotonic() + seconds
    pending = b""
    while time.monotonic() < deadline:
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            chunk = b""
        if not chunk:
            time.sleep(0.02)
            continue
        pending += chunk
        while b"\n" in pending:
            line, pending = pending.split(b"\n", 1)
            yield line.decode("ascii", "replace").rstrip("\r")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("address", nargs="?")
    parser.add_argument("--serial")
    parser.add_argument("--size", type=int, default=262144)
    parser.add_argument("--file", help="send this file instead of the pattern")
    parser.add_argument("--port", type=int, default=7)
    parser.add_argument("--timeout", type=float, default=90.0)
    args = parser.parse_args()
    if not args.address and not args.serial:
        parser.error("give the board's address or --serial PORT")

    fd = open_serial(args.serial) if args.serial else None
    address = args.address
    if not address:
        print(f"waiting for the board on {args.serial} ...", flush=True)
        for line in read_lines(fd, args.timeout):
            print(f"  board: {line}", flush=True)
            found = re.search(r"TCP server ready at (\d+\.\d+\.\d+\.\d+):(\d+)", line)
            if found:
                address, args.port = found.group(1), int(found.group(2))
                break
            if "RESULT:" in line:
                break
        if not address:
            print("FAIL: the board never announced its TCP server")
            return 1

    if args.file:
        with open(args.file, "rb") as source:
            data = source.read()
    else:
        data = pattern(args.size)
    crc = zlib.crc32(data)
    print(f"connecting to {address}:{args.port}, sending {len(data)} bytes ...", flush=True)
    started = time.monotonic()
    echoed = bytearray()
    with socket.create_connection((address, args.port), timeout=10) as sock:
        sock.settimeout(20)
        sent = 0
        # Send and receive in step, so neither side's window fills.
        while sent < len(data) or len(echoed) < len(data):
            if sent < len(data):
                sent += sock.send(data[sent:sent + 16384])
            want = min(len(data) - len(echoed), 65536)
            if want and (sent == len(data) or sent - len(echoed) >= 32768):
                piece = sock.recv(want)
                if not piece:
                    break
                echoed += piece
        sock.shutdown(socket.SHUT_WR)
    elapsed = time.monotonic() - started

    ok = bytes(echoed) == data
    rate = len(data) * 8 / elapsed / 1e6 if elapsed > 0 else 0.0
    print(f"sent {len(data)} bytes, CRC-32 {crc:08x}; echo "
          f"{'identical' if ok else 'DIFFERENT (%d bytes back)' % len(echoed)}; "
          f"{elapsed:.2f} s, {rate:.1f} Mbit/s each way")
    if not ok:
        print("FAIL: the echo does not match")
        return 1

    if fd is not None:
        board_crc = None
        verdict = None
        for line in read_lines(fd, 180):
            print(f"  board: {line}", flush=True)
            found = re.search(r"received (\d+) bytes, CRC-32 ([0-9a-f]{8})", line)
            if found:
                board_crc = (int(found.group(1)), int(found.group(2), 16))
            if "RESULT:" in line:
                verdict = line
                break
        if board_crc != (len(data), crc):
            print(f"FAIL: the board reports {board_crc}, expected ({len(data)}, {crc:08x})")
            return 1
        if verdict is None or "PASS" not in verdict:
            print("FAIL: the board did not report PASS")
            return 1
        print("PASS: echo identical, board CRC-32 matches, stream saved on the SD card")
    else:
        print("PASS: echo identical (compare the CRC-32 with the board's console)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
