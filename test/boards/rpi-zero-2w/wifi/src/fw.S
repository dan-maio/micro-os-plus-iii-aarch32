/* fw.S - the CYW43436 firmware, NVRAM and regulatory (CLM) data, linked into
 * the image. The files are Broadcom/Cypress/Synaptics binaries as shipped by
 * Raspberry Pi (RPi-Distro firmware-nonfree, brcm80211/brcm); see
 * fw/README.md for their origin and licence.
 *
 * The Zero 2 W was built with two chip variants: both images are here and
 * the driver picks one by the chip's revision.
 */
	.section .rodata.cyw43_fw, "a"

	.macro blob name, file
	.balign 8
	.global \name
\name:
	.incbin "\file"
	.global \name\()_end
\name\()_end:
	.byte 0
	.endm

	blob cyw43_fw_43436,     "fw/brcmfmac43436-sdio.bin"
	blob cyw43_nvram_43436,  "fw/brcmfmac43436-sdio.txt"
	blob cyw43_clm_43436,    "fw/brcmfmac43436-sdio.clm_blob"
	blob cyw43_fw_43436s,    "fw/brcmfmac43436s-sdio.bin"
	blob cyw43_nvram_43436s, "fw/brcmfmac43436s-sdio.txt"
