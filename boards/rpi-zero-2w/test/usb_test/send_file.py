#!/usr/bin/env python3
"""
send_file.py — send one host file to the µOS++ usb_test gadget.

The device (core-0 USB thread) reads the file from the bulk OUT endpoint,
writes it to the SD card through flatfs, and re-sends it to its UART and
semihosting consoles as a framed canonical hexdump:

    ---- BEGIN <name> (<n> bytes) ----
    usb-rx <name> 00000000  xx xx xx ...
    ...
    ---- END <name> crc32=<hex> ----

With no SD card the box falls back to RamUartSink, which keeps the file in
RAM (up to 128 KiB) and still streams it to the consoles.

Requires pyusb and the udev rule documented in README.md. Wraps host_xfer.py.
"""
import argparse
import sys

from host_xfer import STATUS_STR, do_get, do_ping, do_put, find_device


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("file", help="host file to send")
    ap.add_argument("--name", help="device-side name (default: basename, 19 chars max)")
    ap.add_argument("--get", action="store_true",
                    help="read the file back over USB and compare")
    ap.add_argument("--check", metavar="CRC32",
                    help="hex CRC-32 the host expects the device to report")
    ap.add_argument("--terminate", action="store_true",
                    help="send the terminating PING so the device prints its "
                         "tally and RESULT (hw.sh watches for it)")
    args = ap.parse_args()

    try:
        with open(args.file, "rb") as f:
            data = f.read()
    except OSError as e:
        sys.exit(f"cannot read {args.file}: {e}")

    name = args.name or args.file.replace("\\", "/").split("/")[-1][:19]
    if not name:
        sys.exit("empty device-side name")

    dev = find_device()
    st, _ = do_ping(dev)
    print(f"device: {STATUS_STR.get(st, st)}")
    if st != 0:
        sys.exit("device did not answer PING")

    print(f"sending {args.file} -> device:{name} ({len(data)} bytes) ...")
    st, _ = do_put(dev, name, data)
    if st != 0:
        sys.exit(f"PUT failed: {STATUS_STR.get(st, st)}")
    print("stored on device; read the UART/semihosting console for the hexdump")

    if args.check is not None:
        import zlib

        want = int(args.check, 16) & 0xFFFFFFFF
        got = zlib.crc32(data) & 0xFFFFFFFF
        if want != got:
            sys.exit(f"CRC-32 mismatch: host {got:08x}, expected {want:08x}")
        print(f"CRC-32 {got:08x} ok")

    if args.get:
        st, got = do_get(dev, name)
        if st != 0:
            sys.exit(f"GET failed: {STATUS_STR.get(st, st)}")
        if got != data:
            sys.exit(f"read-back mismatch ({len(got)} vs {len(data)} bytes)")
        print("read-back: byte-for-byte match over USB")

    if args.terminate:
        do_ping(dev, terminate=True)
        print("sent terminating PING; the device tally/RESULT is on its console")

    print("RESULT: PASS")


if __name__ == "__main__":
    main()
