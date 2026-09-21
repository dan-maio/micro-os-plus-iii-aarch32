#!/usr/bin/env python3
"""
host_xfer.py — drive the µOS++ usb_test USB gadget from a Linux host (pyusb).

Vendor-specific bulk class, VID:PID 1d6b:0104, interface 0, EP 0x81 IN /
0x01 OUT. The wire protocol is the one in usb_test/protocol.hpp.

  * PING reports liveness (and the sink the device selected).
  * PUT  sends a file; the device stores it (flatfs on SD, or RAM) and streams
         it to the UART/semihosting consoles.
  * GET  reads a file back.
  * LIST lists stored files.

With no arguments it runs the size matrix from the design spec: 0, 1, 511,
512, 513, 1024, 65536, 1048576 — bracketing the short-packet boundary, the
zero-length-packet rule, the empty transfer and multi-block flatfs writes.

Requires pyusb and, for non-root use, the udev rule documented in README.md.
"""
import argparse
import struct
import sys
import time
import zlib

try:
    import usb.core  # noqa: F401
except ImportError:
    sys.exit("pyusb is required: pip install pyusb")

VID, PID = 0x1D6B, 0x0104
PRODUCT = "1d6b:0104"

CMD_MAGIC = 0x58534F55  # 'UOSX'
RSP_MAGIC = 0x52534F55  # 'UOSR'
EP_IN, EP_OUT = 0x81, 0x01
CMD_FMT = "<IBBH20sI"
RSP_FMT = "<IIII"

OP_PING, OP_PUT, OP_GET, OP_LIST = 1, 2, 3, 4

STATUS_STR = {
    0: "ok",
    1: "not_found",
    2: "no_space",
    3: "io_error",
    4: "too_large",
    5: "bad_name",
}


def die(msg):
    sys.exit(msg)


def find_device():
    import usb.core

    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        die(
            f"{PRODUCT} not found. Connect the host to the Zero 2W's OTG "
            "micro-USB port (the one labelled USB, not PWR IN) with usb_test "
            "running, then check `lsusb -d " + PRODUCT + "`."
        )
    try:
        dev.set_configuration()
    except usb.core.USBError as e:
        if getattr(e, "errno", None) == 13:
            die(
                "permission denied: install the udev rule from README.md "
                "(/etc/udev/rules.d/99-uos-usbtest.rules) or run with sudo"
            )
        raise
    return dev


_in_mps = {}


def in_max_packet(dev):
    """wMaxPacketSize of the bulk IN endpoint: 64 at full speed, 512 at high.

    The device decides this (USB_FORCE_FS), so read it rather than assume it.
    """
    key = id(dev)
    if key not in _in_mps:
        import usb.util

        mps = 0
        try:
            intf = dev.get_active_configuration()[(0, 0)]
            ep = usb.util.find_descriptor(intf, bEndpointAddress=EP_IN)
            if ep is not None:
                mps = int(ep.wMaxPacketSize)
        except Exception:
            mps = 0
        _in_mps[key] = mps or 64
    return _in_mps[key]


def drain_zlp(dev):
    """Consume the zero-length packet that terminates an exact-multiple IN.

    ep_write() on the device appends a ZLP whenever the payload is an exact
    multiple of wMaxPacketSize, which is the right thing for a host that asks
    for more bytes than the device sends. This protocol is length-prefixed, so
    read_exact() asks for exactly the payload and the ZLP is left queued.

    Leaving it queued is not merely untidy: the device blocks in ep_write()
    until that packet is taken, so it never returns to the command loop and
    never re-arms bulk OUT. The next command's payload write then NAKs until
    the host times out -- which looks like a failure of the NEXT transfer
    size, not of the one that actually sent the ZLP.
    """
    try:
        dev.read(EP_IN, in_max_packet(dev), timeout=200)
    except usb.core.USBError:
        pass


def send_command(dev, op, name="", length=0, flags=0):
    nb = name.encode("ascii", "replace")[:20]
    nb += b"\0" * (20 - len(nb))
    pkt = struct.pack(CMD_FMT, CMD_MAGIC, op, flags, 0, nb, length)
    try:
        dev.write(EP_OUT, pkt, timeout=5000)
    except usb.core.USBError as e:
        die(f"USB write failed: {e}")


def read_exact(dev, n, timeout=30000):
    chunks = []
    got = 0
    while got < n:
        try:
            b = dev.read(EP_IN, n - got, timeout=timeout)
        except usb.core.USBTimeoutError:
            die(f"timed out waiting for {n - got} more bytes")
        b = bytes(b)
        if not b:
            break
        chunks.append(b)
        got += len(b)
    return b"".join(chunks)


def read_reply(dev):
    hdr = read_exact(dev, 16, timeout=10000)
    if len(hdr) != 16:
        die("short reply header")
    magic, status, length, crc = struct.unpack(RSP_FMT, hdr)
    if magic != RSP_MAGIC:
        die(f"bad reply magic 0x{magic:08x}")
    payload = read_exact(dev, length) if length else b""
    if length and (length % in_max_packet(dev)) == 0:
        drain_zlp(dev)
    if length and (zlib.crc32(payload) & 0xFFFFFFFF) != crc:
        die(
            f"payload CRC mismatch: got 0x{zlib.crc32(payload) & 0xFFFFFFFF:08x} "
            f"expected 0x{crc:08x}"
        )
    return status, payload


def do_ping(dev, terminate=False):
    send_command(dev, OP_PING, flags=1 if terminate else 0)
    return read_reply(dev)


def do_put(dev, name, data):
    send_command(dev, OP_PUT, name=name, length=len(data))
    if data:
        try:
            dev.write(EP_OUT, data, timeout=30000)
        except usb.core.USBError as e:
            die(f"PUT payload write failed: {e}")
    return read_reply(dev)


def do_get(dev, name):
    send_command(dev, OP_GET, name=name)
    return read_reply(dev)


def do_list(dev):
    send_command(dev, OP_LIST)
    return read_reply(dev)


def parse_list(payload):
    out = []
    i = 0
    while i < len(payload):
        j = payload.index(b"\0", i)
        name = payload[i:j].decode("ascii", "replace")
        size = struct.unpack_from("<I", payload, j + 1)[0]
        out.append((name, size))
        i = j + 5
    return out


def pattern(n, seed=0x5A):
    return bytes(((seed + i * 7) & 0xFF) for i in range(n))


def run_matrix(dev, args):
    status, payload = do_ping(dev)
    print(f"PING -> {STATUS_STR.get(status, status)}")
    if status != 0:
        die("device did not answer PING")

    sizes = [int(s) for s in args.sizes.split(",") if s.strip()]
    total = 0
    t0 = time.time()
    for rep in range(args.repeat):
        for size in sizes:
            data = pattern(size)
            st, _ = do_put(dev, args.name, data)
            if st != 0:
                die(f"PUT {size}: {STATUS_STR.get(st, st)}")
            st, got = do_get(dev, args.name)
            if st != 0:
                die(f"GET {size}: {STATUS_STR.get(st, st)}")
            if got != data:
                die(f"round-trip mismatch at {size} bytes "
                    f"(got {len(got)})")
            total += size
            print(f"  {size:>8} bytes  PUT+GET ok")
    dt = time.time() - t0

    st, listing = do_list(dev)
    if st != 0:
        die(f"LIST: {STATUS_STR.get(st, st)}")
    # HW_BUILD stores on the card's FAT32 through FatFs, which is built
    # without long-name support, so LIST reports 8.3 names in upper case
    # ("XFER.BIN"). The QEMU build uses flatfs, which keeps the name as sent.
    names = {n.upper(): s for n, s in parse_list(listing)}
    want = args.name.upper()
    if want not in names:
        die(f"LIST did not report {args.name} (saw: {', '.join(sorted(names)) or 'nothing'})")
    print(f"LIST -> {names[want]} bytes for {args.name}")

    bps = (2 * total) / dt if dt > 0 else 0
    print(f"round-tripped {total} bytes x2 in {dt:.2f}s ({bps/1e6:.2f} MB/s)")
    do_ping(dev, terminate=True)
    print("RESULT: PASS")


def run_file(dev, args):
    with open(args.file, "rb") as f:
        data = f.read()
    name = args.name or args.file.split("/")[-1][:19]
    status, _ = do_ping(dev)
    print(f"PING -> {STATUS_STR.get(status, status)}")
    st, _ = do_put(dev, name, data)
    print(f"PUT {name} ({len(data)} bytes) -> {STATUS_STR.get(st, st)}")
    if st != 0:
        die("PUT failed")
    if args.get:
        st, got = do_get(dev, name)
        if st != 0 or got != data:
            die("GET back did not match")
        print("GET back: ok (byte-for-byte)")
    print("RESULT: PASS")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sizes", default="0,1,511,512,513,1024,65536,1048576",
                    help="comma-separated byte sizes for the matrix run")
    ap.add_argument("--name", default="xfer.bin", help="device-side file name")
    ap.add_argument("--repeat", type=int, default=1,
                    help="repeat the whole size matrix N times")
    ap.add_argument("--file", help="send this host file once (app mode)")
    ap.add_argument("--get", action="store_true",
                    help="with --file: read it back and compare")
    args = ap.parse_args()

    dev = find_device()
    try:
        if args.file:
            run_file(dev, args)
        else:
            run_matrix(dev, args)
    except usb.core.USBError as e:
        if getattr(e, "errno", None) == 13:
            die("permission denied: install the udev rule from README.md "
                "or run with sudo")
        die(f"USB error: {e}")


if __name__ == "__main__":
    main()
