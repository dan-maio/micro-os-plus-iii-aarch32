/*
 * linker.ld - µOS++ for the Raspberry Pi Zero 2W (BCM2837, 4x Cortex-A53).
 * kernel7.img is loaded by the GPU firmware at 0x8000 and runs from DRAM.
 */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

/* Core 0 (µOS++) banked stack sizes */
SVC_STACK_SIZE  = 0x100000;  /* 1 MiB  main/task stack (MAIN_TOP)          */
IRQ_STACK_SIZE  = 0x40000;   /* 256 KiB IRQ-mode stack                     */
FIQ_STACK_SIZE  = 0x2000;    /* 8 KiB  FIQ-mode stack                      */
ABT_STACK_SIZE  = 0x8000;    /* 32 KiB Abort-mode stack                    */
UND_STACK_SIZE  = 0x8000;    /* 32 KiB Undef-mode stack                    */

/* Secondary cores 1-3 banked stack sizes (used in Phase 2) */
W_SVC_STACK_SIZE = 0x40000;  /* 256 KiB SVC stack per secondary core       */
W_IRQ_STACK_SIZE = 0x20000;  /* 128 KiB                                    */
W_FIQ_STACK_SIZE = 0x1000;
W_ABT_STACK_SIZE = 0x2000;
W_UND_STACK_SIZE = 0x2000;

HEAP_SIZE = 0x04000000;      /* 64 MiB newlib/µOS++ heap                    */

MEMORY
{
   /* Linked at 0x10000 so ONE binary runs on both QEMU raspi2b (-kernel loads
    * at 0x10000) and real hardware (config.txt kernel_address=0x010000).
    * 0x10000 .. 0x1FFFFFFF : ~512 MiB DRAM. */
   RAM (rwx) : ORIGIN = 0x00010000, LENGTH = 0x1FFF0000
}

SECTIONS
{
    . = ORIGIN(RAM);

    .text : ALIGN(4) {
        KEEP(*(.text.boot))     /* exception_vectors + _start must be first */
        *(.text*)
    } > RAM

    .rodata : ALIGN(4) {
        *(.rodata*)
    } > RAM

    .init_array : ALIGN(4) {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    } > RAM

    .ARM.exidx : {
        __exidx_start = .;
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        __exidx_end = .;
    } > RAM

    .ARM.extab : {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > RAM

    .data : ALIGN(4) {
        *(.data*)
    } > RAM

    .bss (NOLOAD) : ALIGN(8) {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    } > RAM

    /* --- Core 0 banked stacks (µOS++) --- */
    .stack (NOLOAD) : ALIGN(16) {
        . = . + ABT_STACK_SIZE;
        __abt_stack_top = .;
        . = . + UND_STACK_SIZE;
        __und_stack_top = .;
        . = . + FIQ_STACK_SIZE;
        __fiq_stack_top = .;
        . = . + IRQ_STACK_SIZE;
        __irq_stack_top = .;
        . = . + SVC_STACK_SIZE;
        __stack_top = .;
        _stack_top = __stack_top;
    } > RAM

    /* --- Secondary cores 1-3 banked stacks (Phase 2) --- */
    .stack_workers (NOLOAD) : ALIGN(16) {
        . = . + W_ABT_STACK_SIZE; __abt_stack_top_core1 = .;
        . = . + W_UND_STACK_SIZE; __und_stack_top_core1 = .;
        . = . + W_FIQ_STACK_SIZE; __fiq_stack_top_core1 = .;
        . = . + W_IRQ_STACK_SIZE; __irq_stack_top_core1 = .;
        . = . + W_SVC_STACK_SIZE; __stack_top_core1 = .;

        . = . + W_ABT_STACK_SIZE; __abt_stack_top_core2 = .;
        . = . + W_UND_STACK_SIZE; __und_stack_top_core2 = .;
        . = . + W_FIQ_STACK_SIZE; __fiq_stack_top_core2 = .;
        . = . + W_IRQ_STACK_SIZE; __irq_stack_top_core2 = .;
        . = . + W_SVC_STACK_SIZE; __stack_top_core2 = .;

        . = . + W_ABT_STACK_SIZE; __abt_stack_top_core3 = .;
        . = . + W_UND_STACK_SIZE; __und_stack_top_core3 = .;
        . = . + W_FIQ_STACK_SIZE; __fiq_stack_top_core3 = .;
        . = . + W_IRQ_STACK_SIZE; __irq_stack_top_core3 = .;
        . = . + W_SVC_STACK_SIZE; __stack_top_core3 = .;
    } > RAM

    /* Shared MMU L1 translation table (16 KiB, 16 KiB-aligned) */
    .ttb (NOLOAD) : ALIGN(0x4000) {
        __ttb_base = .;
        . = . + 0x4000;
    } > RAM

    /* Heap (µOS++ free store) */
    .heap (NOLOAD) : ALIGN(16) {
        _Heap_Begin = .;
        __heap_start = .;
        . = . + HEAP_SIZE;
        __heap_end = .;
        _Heap_Limit = .;
    } > RAM

    _end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.fini_array*)
    }
}
