# `usb_test` — DWC2 USB gadget file transfer (Raspberry Pi Zero 2 W)

`usb_test` presents the Zero 2 W to a Linux host as a **vendor-specific USB
bulk device** (VID:PID `1d6b:0104`), reads files off the bulk OUT endpoint,
stores them on the SD card, and re-sends every received file to the UART
**and** the ARM semihosting console as a framed hexdump. After every stored
file — and again at the end of a run — it lists the folder it wrote into.

It is the reference application for the port's USB device-mode driver
(`include/usb_dwc2.hpp`, `src/usb_dwc2.cpp`) and a genuine µOS++ SMP program.

## Wiring — the one thing to get right

The Zero 2 W has **two micro-USB sockets**:

| Socket | Silkscreen | Purpose |
|---|---|---|
| left  | `PWR IN` | power only — no data lines to the SoC |
| right | `USB`    | the OTG port — D+/D− go straight to the DWC2 core |

**Power the board from `PWR IN` and run the data cable from the host PC to the
`USB` socket.** On the wrong socket the board boots and prints normally but
never enumerates — a failure with no error message.

## Multithreaded design

Once the device is configured, four application threads run alongside the USB
service thread:

| Thread | Core | Job |
|---|---|---|
| `usb`   | 0 (pinned) | `usb_dwc2::service_thread_body()` — the ISR-driven service loop and the command loop. Blocks in `ep_read`/`ep_write`. |
| `sd`    | any | the **only** thread that touches the card, the FatFs volume or the flatfs volume. Owns the mount; serves `put`/`get`/`list` requests. |
| `uart`  | any | the **only** writer of the PL011 console: drains a message queue of file chunks and status lines. |
| `semi`  | any | the **only** writer of the ARM semihosting channel: same queue discipline. |
| `led`   | any | blinks the user LED: a burst of 3 blinks, each 40 ms lit then 40 ms dark, then a 300 ms dark gap, repeating. On the Zero 2 W that is the onboard green ACT LED (`LED_PIN=29`, the Makefile default); `make LED_PIN=16` drives header pin 36 instead, for an externally wired LED. The pattern is tunable with `LED_BLINKS`, `LED_ON_MS`, `LED_OFF_MS` and `LED_GAP_MS`. |
| `w0`,`w1` | any | unpinned load generators; keep every core scheduling and produce the per-core SMP tally. |

`uart.hpp` is a polled driver with no locking, so exactly one thread writes it;
the same holds for the semihosting channel. The other threads communicate with
the writers only through µOS++ `message_queue`s.

Storage follows the same single-owner rule. The `usb` thread never touches the
card: it posts a request to `sd` and blocks until the answer comes back, so the
protocol reply still carries the real result of the write rather than an
optimistic guess. The mount is issued the same way, from `os_main`, before the
USB core is connected — a card probe running between `usb_dwc2::init()` and the
service thread's first `arm_ep0_setup()` was the original enumeration hang.

## Where the files go

**On hardware (`make HW=1`, which is what `hw.sh` builds), the microSD is the
BOOT card**: an MBR plus a FAT32 partition carrying `config.txt` and
`kernel7.img`/`kernel8.img` at its root. So the app **mounts** that partition
and never formats it, and every file it writes lives under `tests/`. Nothing at
the FAT root is ever touched.

`flatfs` must not be used on a boot card at all: its superblock is LBA 0, which
on that card is the MBR. It is used only for the QEMU builds, where a dedicated
raw `disk.img` is attached and flatfs owns the whole device.

FatFs is built with `FF_USE_LFN = 0` (there is no `ffunicode.c` in the tree),
so the volume only accepts 8.3 names. Host names are folded to 8.3 — the same
way for `put`, `get` and `list`, so a GET issued with the original host name
still finds the file — and the console prints the mapping rather than renaming
the file behind your back:

```
stored: send_file.py -> tests/SEND_F~1.PY (8.3)
```

With no card, or an unmountable one, the app falls back to `RamUartSink`: the
file is kept in a 128 KiB RAM buffer and still streamed to both consoles.

## Host side

```bash
pip install pyusb
```

Non-root access needs a udev rule (the device is a test identity; this is the
Linux Foundation test range and must never ship as a product VID/PID):

```bash
sudo tee /etc/udev/rules.d/99-uos-usbtest.rules >/dev/null <<'EOF'
SUBSYSTEM=="usb", ATTR{idVendor}=="1d6b", ATTR{idProduct}=="0104", MODE="0666"
EOF
sudo udevadm control --reload
```

## Running under QEMU (expected: `SKIP`)

QEMU models the DWC2 core as a *host* controller only, so the device-mode wait
in `usb_dwc2::init()` expires and the test reports:

```
RESULT: SKIP (no USB device mode)
```

```bash
cd 32b/usb_test && ./run.sh raspi2b      # or: ./run.sh raspi3b
cd 64b/usb_test && ./run.sh
```

The run also exercises the VideoCore mailbox (`USB power domain: on`), and —
because no disk image is attached — the `RamUartSink` selection path.

## Running on hardware

```bash
cd 32b/usb_test                 # or 64b/usb_test
make clean && make SEMIHOST=1
./hw.sh 300 &                   # J-Link + OpenOCD, semihosted console
sleep 20
python3 send_file.py myfile.bin --get
```

`hw.sh` builds and loads the image over JTAG; its console is the OpenOCD log.
On the host, while it runs:

```bash
lsusb -d 1d6b:0104
lsusb -v -d 1d6b:0104 | grep -E "bInterfaceClass|bEndpointAddress|wMaxPacketSize"
# expect bInterfaceClass 255, EP 0x81 IN and 0x01 OUT, wMaxPacketSize 0x0200
```

At **high speed** the bulk `wMaxPacketSize` reads 512; 64 would mean the link
fell back to full speed.

## The send-a-file app

`send_file.py` is the simple application: it sends one host file, the device
writes it to the SD card and re-sends it to the UART/semihosting console.

```bash
python3 send_file.py myfile.bin                 # store + hexdump to console
python3 send_file.py myfile.bin --get           # also read it back and compare
python3 send_file.py myfile.bin --name fw.bin   # choose the device-side name
python3 send_file.py myfile.bin --check 1a2b3c4d  # verify the streamed CRC-32
```

The console stream looks like:

```
---- BEGIN myfile.bin (4096 bytes) ----
usb-rx myfile.bin 00000000  4d 5a 90 00 03 00 00 00 04 00 00 00 ff ff 00 00  ...
...
---- END myfile.bin crc32=1a2b3c4d ----
```

The `crc32=` value is the IEEE reflected CRC-32 (the same as Python's
`zlib.crc32`), so the host can verify the bytes the device actually stored.
Each 128-byte chunk is queued to the `uart` and `semi` threads as it arrives,
so the console stream is produced concurrently with the transfer.

## The full round-trip matrix

`host_xfer.py` runs the design's size matrix, PUTting each size, GETting it
back, byte-comparing and checking the reply CRC-32:

```bash
python3 host_xfer.py                                   # 0,1,511,512,513,1024,65536,1048576
python3 host_xfer.py --repeat 100                      # ~100 MiB stability run (Task 9)
python3 host_xfer.py --sizes 512,1024 --name z.bin     # the ZLP-sensitive sizes
```

The 512 and 1024 cases exercise the zero-length-packet rule, the 0-byte case
the empty transfer, and 65536/1048576 multi-block `flatfs` writes. The device
prints `RESULT: PASS` only when no CRC mismatch occurred, at least one command
was served, and all but at most one core was active during the run
(the SMP tally). A `100 MiB` run with no `!!! CORRUPT CONTEXT FRAME !!!` from
the port's tripwire is the stability gate.

## Troubleshooting

| Symptom | Check |
|---|---|
| `lsusb` shows nothing | data cable in the `USB` socket, not `PWR IN`; `USB power domain: on` in the console |
| `permission denied` | install the udev rule above, or run `send_file.py` with sudo |
| enumerates, then bulk times out at exact 512-byte sizes | the zero-length-packet path (`usb_dwc2::ep_write`) — do not drop the 512/1024 sizes from the matrix |
| `RESULT: FAIL (only N cores active)` | host/board under load; re-run unloaded |
| `!!! CORRUPT CONTEXT FRAME !!!` | hard stop: the USB ISR disturbed the SMP context switch |
