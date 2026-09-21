/* m0_blob.S — embed the compiled Cortex-M0 firmware into the A7 image.
 * The A7 copies [m0_fw_start, m0_fw_end) to System SRAM (0xFFF80000) before
 * releasing the M0. m0/m0_fw.bin is produced by `make -C m0` (the A7 Makefile
 * builds it first). */
        .section .rodata.m0fw, "a"
        .align 4
        .global m0_fw_start
        .global m0_fw_end
m0_fw_start:
        .incbin "m0/m0_fw.bin"
m0_fw_end:
