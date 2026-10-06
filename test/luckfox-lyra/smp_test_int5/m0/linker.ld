/* linker.ld — RK3506 Cortex-M0 firmware for smp_test_int5.
 *
 * Linked at 0x00000000: the M0 fetches its initial MSP/PC from the vector
 * table at offset 0, aliased by Rockchip to the MCU code SRAM. If board
 * discovery shows the M0 code space is aliased elsewhere, change ORIGIN here
 * and LOAD_ADDR in openocd_m0.cfg together. The firmware is tiny (a few hundred
 * bytes of code + a 4 KB stack), well inside the MCU SRAM budget. */

ENTRY(Reset_Handler)

/* System SRAM is 48 KB (0xFFF80000..0xFFF8C000), attached to the M0 as TCM at
 * its local 0x0. Split: 32 KB code/vectors + 16 KB data/stack. */
MEMORY
{
    CODE (rx)  : ORIGIN = 0x00000000, LENGTH = 0x00008000   /* 32 KB code/vectors */
    DATA (rwx) : ORIGIN = 0x00008000, LENGTH = 0x00004000   /* 16 KB data + stack */
}

_estack = ORIGIN(DATA) + LENGTH(DATA);   /* top of stack (full-descending) */

SECTIONS
{
    .isr_vector : { KEEP(*(.isr_vector)) } > CODE
    .text       : { *(.text*) *(.rodata*) } > CODE

    .data : {
        _sdata = .;
        *(.data*)
        _edata = .;
    } > DATA AT > CODE

    .bss : {
        _sbss = .;
        *(.bss*) *(COMMON)
        _ebss = .;
    } > DATA

    /* This minimal firmware uses no initialized .data and clears no .bss
     * (only g_seq, which starts at 0 and is set at runtime); the reset path
     * jumps straight to main(). Keep .data/.bss empty by construction. */
}
