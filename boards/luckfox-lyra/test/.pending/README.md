# Lyra tests waiting on a dependency

These six are the predecessor's, carried over byte for byte from
`micro-os-plus-iii-smp-old/cortex-a7/micro-os-plus-iii-cortex-a7/`. They are
here rather than one directory up because each needs something that has not
been carried over yet, and `test/*` is globbed — a directory that cannot link
would break the board's build for everyone.

They are **not** to be rewritten. Each becomes buildable the moment its
dependency arrives, by `git mv` back into `boards/luckfox-lyra/test/`.

| test | needs | where it is in the reference |
|---|---|---|
| `smp_test5` | `fatfs::File` / `fatfs::Dir` — FatFs R0.15a compiled as C++ inside `namespace fatfs` | `src/sdcard/fatfs_cpp/{ff.hpp,ff.cpp,ffsystem.cpp,ffunicode.cpp}` |
| `smp_test7` | the same | the same |
| `smp_test_int4` | the same, plus `sd_irq_service` | the same, plus `src/sdcard/sdmmc.*` (already carried, as `devices/soc/rk3506/`) |
| `smp_test6` | the RK3506 DWC2 **device** stack: `usb_gadget_init`, `rk3506::usb::dwc2::Driver`, `usb_vendor_start_read_queue` | `src/usb/` (13 files) + `include/usb/` (10 files) |
| `smp_test_int3` | the same, through its `usb1i3_*` entry points | the same |
| `smp_test_int5` | `m0_fw_start` / `m0_fw_end` — the Cortex-M0 firmware image linked in as a blob | the predecessor's M0 build |

The workspace already has a C FatFs (`micro-os-plus-iii-devices/fatfs/`, the
same R0.15a). It is **not** interchangeable with the reference's: that one is
namespaced and wraps `Dir`/`File` in RAII classes, so `ff.h` behind an
`ff.hpp` shim does not satisfy these sources. Carrying the C++ copy means a
second 7100-line FatFs in the workspace, which is a decision to take
deliberately, not a detail to slip in.
