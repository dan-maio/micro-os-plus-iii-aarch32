/*
 * linker.ld - µOS++ / StateOS for the Luckfox Lyra B (RK3506, 3x Cortex-A7).
 * Runs entirely in DRAM at 0x00200000.
 */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

/* Stack sizes */
SVC_STACK_SIZE  = 0x200000;  /* 2 MiB  main/task stack (MAIN_TOP)          */
IRQ_STACK_SIZE  = 0x100000;  /* 1 MiB  IRQ-mode stack                      */
FIQ_STACK_SIZE  = 0x2000;    /* 8 KiB  FIQ-mode stack                      */
ABT_STACK_SIZE  = 0x10000;   /* 64 KiB Abort-mode stack                    */
UND_STACK_SIZE  = 0x10000;   /* 64 KiB Undef-mode stack                    */

/* Cores 1/2 (bare-metal workers) stack sizes */
W_SVC_STACK_SIZE = 0x4000;   /* 16 KiB SVC stack per worker core           */
W_IRQ_STACK_SIZE = 0x2000;   /* 8 KiB                                      */
W_FIQ_STACK_SIZE = 0x1000;
W_ABT_STACK_SIZE = 0x1000;
W_UND_STACK_SIZE = 0x1000;

HEAP_SIZE = 0x01000000;      /* 16 MiB newlib heap                          */
HEAP_BASE = 0x06400000;      /* 100 MB region of the 128 MB DDR, Normal map */

MEMORY
{
   /* The MMU maps DRAM Normal up to 0x08000000 (mmu.cpp kDramTop = 128 MiB),
    * and smp.hpp validates stack pointers against that same 0x08000000 top.
    * Starting at 0x00200000, the region must therefore be 0x07E00000 long --
    * the old 0x08000000 length ran 2 MiB past the mapped window, so anything
    * the linker placed in the tail would translation-fault. */
      RAM (rwx) : ORIGIN = 0x00200000, LENGTH = 0x07E00000    /* 126 MiB window */
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

    /* C++ global constructors */
    .init_array : ALIGN(4) {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    } > RAM

    /* ARM exception unwind tables */
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

    /* --- Core 0 stacks (µOS++) --- */
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

    /* --- Cores 1 and 2 stacks --- */
    .stack_workers (NOLOAD) : ALIGN(16) {
        . = . + W_ABT_STACK_SIZE;
        __abt_stack_top_core1 = .;
        . = . + W_UND_STACK_SIZE;
        __und_stack_top_core1 = .;
        . = . + W_FIQ_STACK_SIZE;
        __fiq_stack_top_core1 = .;
        . = . + W_IRQ_STACK_SIZE;
        __irq_stack_top_core1 = .;
        . = . + W_SVC_STACK_SIZE;
        __stack_top_core1 = .;

        . = . + W_ABT_STACK_SIZE;
        __abt_stack_top_core2 = .;
        . = . + W_UND_STACK_SIZE;
        __und_stack_top_core2 = .;
        . = . + W_FIQ_STACK_SIZE;
        __fiq_stack_top_core2 = .;
        . = . + W_IRQ_STACK_SIZE;
        __irq_stack_top_core2 = .;
        . = . + W_SVC_STACK_SIZE;
        __stack_top_core2 = .;
        __stack = .; /* top stack address for µOS++ */
    } > RAM

    /* Shared MMU L1 translation table */
    .ttb (NOLOAD) : ALIGN(0x4000) {
        __ttb_base = .;
        . = . + 0x4000;
    } > RAM

    /* USB DMA window: Normal NON-CACHEABLE */
    .dma_nc (NOLOAD) : ALIGN(0x100000) {
        __dma_nc_start = .;
        *(.dma_nc*)
        . = ALIGN(64);
        __dma_pool_start = .;
        . = __dma_nc_start + 0x200000;
        __dma_nc_end = .;
    } > RAM

    _end = .;

    /* Newlib heap */
    .heap HEAP_BASE (NOLOAD) : {
        _Heap_Begin = .;
        __heap_start = .;
        . = . + HEAP_SIZE;
        __heap_end = .;
        _Heap_Limit = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.fini_array*)
    }
}
