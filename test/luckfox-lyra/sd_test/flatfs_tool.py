#!/usr/bin/env python3
"""
flatfs_tool.py — host companion for the rpi-zero-2w `flatfs` (see
micro-os-plus-iii-rpi-zero-2w/include/flatfs.hpp and src/flatfs.cpp).

This module reproduces the *exact* on-disk layout the firmware uses, so a Linux
host can:

  * make-seed <img>   build a fresh disk image, format it as flatfs and store
                      `seed.bin` (deterministic pattern).  The firmware then
                      mounts this image and verifies seed.bin  ("host -> dev").
  * verify <img>      after the firmware ran, re-read the volume and check the
                      files the firmware wrote (hello.txt, data1.bin,
                      data2.bin, big.bin)  ("dev -> host").
  * dump <img>        print the volume superblock + file table.

Layout (little-endian, 512-byte sectors):
    block 0             superblock
    blocks 1 .. 1+B-1   allocation bitmap (1 bit per data block)
    blocks 1+B .. ds-1  file table (16 x 32-byte entries per block)
    blocks ds .. end    file data (contiguous per file)

Run:  python3 flatfs_tool.py {make-seed|verify|dump} <image>
"""

import os
import struct
import sys

SECTOR = 512
MAGIC = b"FLATFS01"
LAYOUT_VERSION = 1

TABLE_BLOCKS = 16
ENTRY_BYTES = 32
ENTRIES_PER_BLOCK = SECTOR // ENTRY_BYTES  # 16
MAX_FILES = TABLE_BLOCKS * ENTRIES_PER_BLOCK
NAME_MAX = 19
FLAG_VALID = 1

# Superblock field offsets
SB_MAGIC = 0
SB_VERSION = 8
SB_BLOCK_SIZE = 12
SB_TOTAL = 16
SB_DATA_START = 20
SB_BITMAP_BLOCKS = 24
SB_TABLE_BLOCKS = 28
SB_MAX_FILES = 32
SB_FREE = 36

# Entry field offsets
E_NAME = 0
E_START = 20
E_SIZE = 24
E_FLAGS = 28

# Expected files written by the firmware (dev -> host verification)
HELLO_TEXT = b"Hello from flatfs on micro-os-plus-iii!\n"

# seed.bin parameters (host -> dev)
SEED_NAME = "seed.bin"
SEED_SIZE = 100000
SEED_SEED = 0x51A7E51

MASK32 = 0xFFFFFFFF


def fill(seed, n):
    """Deterministic byte pattern; must match fill_pattern() in sd_test/main.cpp."""
    x = seed & MASK32
    out = bytearray()
    for _ in range(n):
        x = (x * 1664525 + 1013904223) & MASK32
        out.append((x >> 24) & 0xFF)
    return bytes(out)


# ---------------------------------------------------------------------------
# Geometry / image
# ---------------------------------------------------------------------------
def geom_for(total_blocks):
    b = 1
    while b * (SECTOR * 8) < total_blocks - 1 - TABLE_BLOCKS - b:
        b += 1
    data_start = 1 + b + TABLE_BLOCKS
    data_blocks = total_blocks - data_start
    return {
        "total_blocks": total_blocks,
        "table_blocks": TABLE_BLOCKS,
        "max_files": MAX_FILES,
        "bitmap_blocks": b,
        "data_start": data_start,
        "data_blocks": data_blocks,
    }


def open_image(path, create_size=0):
    if create_size:
        with open(path, "wb") as f:
            f.truncate(create_size)  # sparse: near-zero disk usage
    total = os.path.getsize(path) // SECTOR
    f = open(path, "r+b")
    return f, geom_for(total)


def write_sb(f, geom, free_blocks):
    sb = bytearray(SECTOR)
    sb[SB_MAGIC:SB_MAGIC + 8] = MAGIC
    struct.pack_into("<I", sb, SB_VERSION, LAYOUT_VERSION)
    struct.pack_into("<I", sb, SB_BLOCK_SIZE, SECTOR)
    struct.pack_into("<I", sb, SB_TOTAL, geom["total_blocks"])
    struct.pack_into("<I", sb, SB_DATA_START, geom["data_start"])
    struct.pack_into("<I", sb, SB_BITMAP_BLOCKS, geom["bitmap_blocks"])
    struct.pack_into("<I", sb, SB_TABLE_BLOCKS, geom["table_blocks"])
    struct.pack_into("<I", sb, SB_MAX_FILES, geom["max_files"])
    struct.pack_into("<I", sb, SB_FREE, free_blocks)
    f.seek(0)
    f.write(sb)


def read_sb(f):
    f.seek(0)
    sb = f.read(SECTOR)
    if len(sb) < SECTOR:
        return None
    if sb[SB_MAGIC:SB_MAGIC + 8] != MAGIC:
        return None
    g = geom_for(struct.unpack_from("<I", sb, SB_TOTAL)[0])
    g["data_start"] = struct.unpack_from("<I", sb, SB_DATA_START)[0]
    g["bitmap_blocks"] = struct.unpack_from("<I", sb, SB_BITMAP_BLOCKS)[0]
    g["table_blocks"] = struct.unpack_from("<I", sb, SB_TABLE_BLOCKS)[0]
    g["max_files"] = struct.unpack_from("<I", sb, SB_MAX_FILES)[0]
    g["free_blocks"] = struct.unpack_from("<I", sb, SB_FREE)[0]
    return g


def entry_offset(geom, idx):
    block = 1 + geom["bitmap_blocks"] + idx // ENTRIES_PER_BLOCK
    off = idx % ENTRIES_PER_BLOCK
    return block * SECTOR + off * ENTRY_BYTES


def read_entry(f, geom, idx):
    f.seek(entry_offset(geom, idx))
    raw = f.read(ENTRY_BYTES)
    if len(raw) < ENTRY_BYTES:
        return None
    flags = struct.unpack_from("<I", raw, E_FLAGS)[0]
    if not (flags & FLAG_VALID):
        return None
    name = raw[E_NAME:E_NAME + NAME_MAX + 1].split(b"\0")[0].decode("ascii")
    start, size = struct.unpack_from("<II", raw, E_START)
    return {"name": name, "start": start, "size": size}


def write_entry(f, geom, idx, name, start, size):
    raw = bytearray(ENTRY_BYTES)
    raw[E_NAME:E_NAME + len(name)] = name.encode("ascii")
    struct.pack_into("<I", raw, E_START, start)
    struct.pack_into("<I", raw, E_SIZE, size)
    struct.pack_into("<I", raw, E_FLAGS, FLAG_VALID)
    f.seek(entry_offset(geom, idx))
    f.write(raw)


def bitmap_seek(geom, block_index):
    """absolute sector of a data-region block index"""
    return (geom["data_start"] + block_index) * SECTOR


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------
def cmd_make_seed(path):
    # 4 GiB, sparse -> SDHC card; power of two so every QEMU version (incl.
    # the raspi3b AArch64 shim path, which rejects non-power-of-2 sizes) will
    # accept the image. total = size / 512 = 8388608 sectors.
    size = 4 * 1024**3
    f, geom = open_image(path, create_size=size)

    # Format: cleared bitmap + empty table + superblock.
    bitmap_region = bytearray(geom["bitmap_blocks"] * SECTOR)
    table_region = bytearray(geom["table_blocks"] * SECTOR)
    f.seek(1 * SECTOR)
    f.write(bitmap_region)                       # blocks 1 .. 1+B-1
    f.seek((1 + geom["bitmap_blocks"]) * SECTOR)
    f.write(table_region)                        # file table blocks

    # Write seed.bin at the first free data blocks (start of the data region).
    data = fill(SEED_SEED, SEED_SIZE)
    nsec = (SEED_SIZE + SECTOR - 1) // SECTOR
    free = geom["data_blocks"] - nsec
    bitmap = bytearray(geom["bitmap_blocks"] * SECTOR)
    for i in range(nsec):
        byte, bit = divmod(i, 8)
        bitmap[byte] |= 1 << bit
    f.seek(1 * SECTOR)
    f.write(bitmap)
    f.seek(bitmap_seek(geom, 0))
    f.write(data)
    if nsec * SECTOR > len(data):
        f.write(b"\0" * (nsec * SECTOR - len(data)))

    write_entry(f, geom, 0, SEED_NAME, geom["data_start"], SEED_SIZE)
    write_sb(f, geom, free)
    f.close()
    print(f"made {path}: {geom['total_blocks']} sectors, seed.bin "
          f"({SEED_SIZE} B) written.")


def list_entries(f, geom):
    out = []
    for i in range(geom["max_files"]):
        e = read_entry(f, geom, i)
        if e:
            out.append(e)
    return out


def cmd_verify(path):
    f, _g = open_image(path)
    g = read_sb(f)
    if g is None:
        print("verify FAIL: not a flatfs volume")
        return 1

    print(f"superblock: total={g['total_blocks']} data_start={g['data_start']} "
          f"bitmap={g['bitmap_blocks']} free={g['free_blocks']}")

    entries = list_entries(f, g)
    if not entries:
        print("verify FAIL: volume is empty (firmware never wrote files?)")
        return 1

    ok = True
    for e in entries:
        # Expected content by name.
        if e["name"] == "hello.txt":
            want = HELLO_TEXT
        elif e["name"] in ("data1.bin", "data2.bin", "big.bin"):
            seeds = {"data1.bin": 0x11111111, "data2.bin": 0x22222222,
                     "big.bin": 0x33333333}
            want = fill(seeds[e["name"]], e["size"])
        elif e["name"] == SEED_NAME:
            want = fill(SEED_SEED, e["size"])
        else:
            print(f"  ? unknown file {e['name']}")
            ok = False
            continue

        # Read the file bytes back (rounded to whole sectors).
        nsec = (e["size"] + SECTOR - 1) // SECTOR
        f.seek(e["start"] * SECTOR)
        got = f.read(nsec * SECTOR)[:e["size"]]

        good = (len(got) == e["size"]) and (got == want)
        ok &= good
        print(f"  [{'PASS' if good else 'FAIL'}] {e['name']:<20} "
              f"start={e['start']:>8} size={e['size']}")

    print(f"verify {'PASS' if ok else 'FAIL'} ({len(entries)} files)")
    return 0 if ok else 1


def cmd_dump(path):
    f, _g = open_image(path)
    g = read_sb(f)
    if g is None:
        print("dump: not a flatfs volume")
        return 1
    print(f"superblock: total={g['total_blocks']} data_start={g['data_start']} "
          f"bitmap_blocks={g['bitmap_blocks']} table_blocks={g['table_blocks']} "
          f"max_files={g['max_files']} free={g['free_blocks']}")
    for e in list_entries(f, g):
        print(f"  {e['name']:<20} start={e['start']:>8} size={e['size']}")
    return 0


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd, path = argv[0], argv[1]
    if cmd == "make-seed":
        return cmd_make_seed(path)
    if cmd == "verify":
        return cmd_verify(path)
    if cmd == "dump":
        return cmd_dump(path)
    print(f"unknown command {cmd}")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
