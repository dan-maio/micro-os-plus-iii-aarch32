# CYW43436 firmware files

Binary firmware for the Wi-Fi chip of the Raspberry Pi Zero 2 W, as shipped by
Raspberry Pi. They are not part of µOS++ and are not covered by its licence.

| File | Size | Used for |
|---|---|---|
| `brcmfmac43436s-sdio.bin` | 442211 | chip 43430, silicon revision 1 (the Synaptics variant) |
| `brcmfmac43436s-sdio.txt` | 1185 | its NVRAM (board description) |
| `brcmfmac43436-sdio.bin` | 416101 | chip 43430, silicon revision 2 and later |
| `brcmfmac43436-sdio.txt` | 1706 | its NVRAM |
| `brcmfmac43436-sdio.clm_blob` | 11209 | its regulatory (CLM) data |

Source: <https://github.com/RPi-Distro/firmware-nonfree>, branch `bookworm`,
`debian/config/brcm80211/brcm/`, downloaded 2026-11-13. On a Raspberry Pi OS
card the same files are in `/lib/firmware/brcm/`; for the Zero 2 W Linux
loads them under the names `brcmfmac43430-sdio.raspberrypi,model-zero-2-w.*`
(the 43436s set) and `brcmfmac43430b0-sdio.raspberrypi,model-zero-2-w.*`
(the 43436 set).

Licence: Broadcom/Cypress/Synaptics binary firmware licences, as recorded in
that repository (`debian/config/brcm80211/LICENSE` and the `copyright` file).
Redistribution is permitted in binary form under those terms; read them
before publishing a repository that contains these files.

`../fw.S` links all five into the test image; the driver picks a set by the
chip's revision.
