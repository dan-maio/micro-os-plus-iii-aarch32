# SD Card (SDMMC) support — Luckfox Lyra B (RK3506)

Bare-metal SD-card driver for the StateOS port. This folder is dropped into all
three builds (AMP root, `SMP/`, `SMP_FINE/`) unchanged.

## Hardware

The RK3506 SDMMC host at **`0xFF480000`** is a **Synopsys DesignWare Mobile
Storage Host Controller ("dw_mmc")** — *not* an SDHCI controller. That matters:
the register map below is the DesignWare one (`CTRL`, `CMD`, `RINTSTS`, `FIFO`),
distilled from U-Boot `drivers/mmc/dw_mmc.c`, **not** `sdhci.c`.

| Block            | Base         | Used for                                    |
|------------------|--------------|---------------------------------------------|
| SDMMC (dw_mmc)   | `0xFF480000` | Command/response, FIFO PIO data             |
| CRU              | `0xFF9A0000` | `GATE_CON17`, `CLKSEL_CON49`, `SOFTRST_CON17` |
| GPIO3 IOC        | `0xFF4D8000` | IOMUX for SDMMC pins                         |

Pins: `GPIO3_A0..A5` → func1 = `SDMMC_CLK, CMD, D0, D1, D2, D3`.

## Files

| File/Folder        | Purpose                                                        |
|--------------------|----------------------------------------------------------------|
| `sdmmc.hpp`        | Register map, SD command set, `sdcard::SdMmc` driver API        |
| `sdmmc.cpp`        | DW-MMC host + SD identification + PIO block read/write          |
| `sdcard_demo.cpp`  | Optional UART self-test (`sdcard::demo()`) — not auto-wired     |
| `fatfs/`           | C version of the FatFs R0.15a filesystem library               |
| `fatfs_cpp/`       | Modernized C++23 FatFs engine with RAII wrappers (`fs::File`/`fs::Dir`)|
| `tests/`           | Low-level block I/O and FatFs C/C++ filesystem integration tests|
| `reference/uboot-mmc/` | Verbatim U-Boot `drivers/mmc/` sources this was distilled from |

## API

```cpp
#include "../sdcard/sdmmc.hpp"

sdcard::SdMmc sd;
if (sd.init()) {                       // clocks + pinmux + SD ID sequence
    alignas(4) uint8_t sect[512];
    sd.read_block(0, sect);            // read one 512-byte LBA
    sd.read_blocks(100, buf, 8);       // read 8 consecutive sectors
    sd.write_block(200, sect);         // write one sector
    uint64_t bytes = sd.capacity_bytes();
}
```

`init()` runs the standard SD bring-up: `CMD0 → CMD8 → ACMD41 (loop) → CMD2 →
CMD3 → CMD9 (CSD/capacity) → CMD7 → ACMD6 (4-bit) → CMD16`, then steps the card
clock from 400 kHz up to 25 MHz. Transfers are **polled PIO** through the
`DWMCI_DATA` FIFO — no IDMAC descriptors, no interrupts, no MMU changes — so it
runs from any of the three Cortex-A7 cores.

## The one knob you must tune: `kBusHz`

`sdmmc.hpp` assumes the `cclk_src_sdmmc` source is **100 MHz** (`kBusHz`). The
DesignWare card clock is `kBusHz / (2 × CLKDIV)`. If `init()` fails or reads are
garbage, this is almost always wrong — measure/derive the actual PLL rate that
`CRU_CLKSEL_CON49` selects and update `kBusHz`.

## Wiring into a build

The `Makefile` uses an explicit source list, so add one line to `CXX_SOURCES`:

```make
CXX_SOURCES += ../sdcard/sdmmc.cpp
# optional self-test:
CXX_SOURCES += ../sdcard/sdcard_demo.cpp
```

(Root build: use `sdcard/...` — no `../`.) Then call `sdcard::demo()` from your
main thread after `uart::uart1.init()`. The include path already reaches
`sdcard/` via the relative `#include "../sdcard/sdmmc.hpp"`.

## Provenance

Reference vendor sources: `/…/Luckfox_Lyrs_B/SDK/u-boot/drivers/mmc/`. The
DesignWare register semantics come from `include/dwmmc.h`; the SD state machine
and CSD-capacity math come from `mmc.c`. Those files are copied under
`reference/uboot-mmc/` so you can cross-check any register offset or quirk.
