/*
 * startup.S - Multicore startup for µOS++ / StateOS on the Luckfox Lyra B (RK3506,
 *             3x Cortex-A7).
 *
 * All three cores enter at _start (core 0 from the bootloader, cores 1 and 2
 * released later via the SRAM mailbox + CRU reset protocol) and are dispatched
 * by MPIDR:
 *
 *   core 0 : full cache invalidate, stacks, .bss clear, mmu_init -> main()
 *   core 1 : stacks, mmu_enable                                  -> boot_core1()
 *   core 2 : stacks, mmu_enable                                  -> boot_core2()
 *
 * IMPORTANT (from the validated StateOS SMP port): the defensive full
 * set/way D-cache invalidate runs on CORE 0 ONLY. Secondaries are released
 * from clean BootROM state while core 0 runs with live dirty data in the
 * SHARED L2 - a set/way invalidate from them would discard core 0's dirty
 * lines without writeback and corrupt memory system-wide.
 */
	.section .text.boot, "ax"
	.arm

	.align 5
	.global exception_vectors
exception_vectors:
	ldr	pc, =_start                 @ 0x00: Reset / Entry
	ldr	pc, =undefined_handler      @ 0x04: Undefined instruction
	ldr	pc, =svc_handler            @ 0x08: Supervisor call
	ldr	pc, =prefetch_handler       @ 0x0C: Prefetch Abort
	ldr	pc, =data_handler           @ 0x10: Data Abort
	nop                                 @ 0x14: Reserved
	ldr	pc, =irq_handler            @ 0x18: IRQ (preemptive context switch)
	ldr	pc, =fiq_handler            @ 0x1C: FIQ
	.ltorg

	.global _start
_start:
	/* Mask IRQ + FIQ while we set up. Cores 1/2 keep them masked forever. */
	cpsid	if

	/* Initialize CNTFRQ to 24 MHz (per-core) */
	ldr	r0, =24000000
	mcr	p15, 0, r0, c14, c0, 0

	/* Enable VFP/NEON (per-core, banked): CPACR CP10+CP11 full access,
	 * then FPEXC.EN. Must precede any C++ code (hard-float ABI). */
	mrc	p15, 0, r0, c1, c0, 2
	orr	r0, r0, #(0xF << 20)
	mcr	p15, 0, r0, c1, c0, 2
	isb
	mov	r0, #(1 << 30)
	vmsr	fpexc, r0

	/* Install VBAR (banked per core). */
	ldr	r0, =exception_vectors
	mcr	p15, 0, r0, c12, c0, 0
	isb

	/* Dispatch by MPIDR core ID (bits [1:0]). */
	mrc	p15, 0, r0, c0, c0, 5
	and	r0, r0, #3
	cmp	r0, #0
	beq	core0_start
	cmp	r0, #1
	beq	core1_start
	cmp	r0, #2
	beq	core2_start

	/* Unknown core: park. */
hang_other:
	wfe
	b	hang_other

/* ---------------------------------------------------------------------------
 * Core 0 — Primary core
 * ------------------------------------------------------------------------- */
core0_start:
	/* ---- DEFENSIVE BOOT (hot-entry = cold-boot), CORE 0 ONLY -------------
	 * A debugger hot load may enter here with the OLD image's MMU/caches
	 * possibly ON and L1D/L2 full of its STALE DIRTY lines. Force SCTLR
	 * M/C/I off, then invalidate every data/unified cache level by set/way
	 * (CLIDR walk), the I-cache, branch predictor and TLBs.
	 * Secondaries MUST NOT do this: they boot while core 0 runs with live
	 * dirty data in the shared L2. */
	mrc	p15, 0, r0, c1, c0, 0
	bic	r0, r0, #(1 << 0)		@ M: MMU off
	bic	r0, r0, #(1 << 2)		@ C: D-cache off
	bic	r0, r0, #(1 << 12)		@ I: I-cache off
	mcr	p15, 0, r0, c1, c0, 0
	isb
	mrc	p15, 1, r0, c0, c0, 1		@ CLIDR
	ands	r3, r0, #0x07000000
	mov	r3, r3, lsr #23			@ LoC * 2
	beq	5f
	mov	r10, #0				@ level * 2
1:	add	r2, r10, r10, lsr #1
	mov	r1, r0, lsr r2
	and	r1, r1, #7			@ cache type at this level
	cmp	r1, #2
	blt	4f				@ no data/unified cache here
	mcr	p15, 2, r10, c0, c0, 0		@ CSSELR = level
	isb
	mrc	p15, 1, r1, c0, c0, 0		@ CCSIDR
	and	r2, r1, #7
	add	r2, r2, #4			@ log2(line size)
	movw	r4, #0x3ff
	ands	r4, r4, r1, lsr #3		@ ways - 1
	clz	r5, r4				@ way shift
	movw	r7, #0x7fff
	ands	r7, r7, r1, lsr #13		@ sets - 1
2:	mov	r9, r4				@ way loop
3:	orr	r11, r10, r9, lsl r5
	orr	r11, r11, r7, lsl r2
	mcr	p15, 0, r11, c7, c6, 2		@ DCISW: invalidate set/way
	subs	r9, r9, #1
	bge	3b
	subs	r7, r7, #1			@ set loop
	bge	2b
4:	add	r10, r10, #2
	cmp	r3, r10
	bgt	1b
5:	mov	r10, #0
	mcr	p15, 2, r10, c0, c0, 0		@ CSSELR back to L1
	dsb
	mcr	p15, 0, r10, c7, c5, 0		@ ICIALLU
	mcr	p15, 0, r10, c7, c5, 6		@ BPIALL
	mcr	p15, 0, r10, c8, c7, 0		@ TLBIALL
	dsb
	isb
	/* ---- end defensive boot ------------------------------------------- */

	/* Banked stacks: IRQ, FIQ, ABT, UND, then SVC (= µOS++ MAIN_TOP). */
	cps	#0x12
	ldr	sp, =__irq_stack_top
	cps	#0x11
	ldr	sp, =__fiq_stack_top
	cps	#0x17
	ldr	sp, =__abt_stack_top
	cps	#0x1B
	ldr	sp, =__und_stack_top
	cps	#0x13
	ldr	sp, =__stack_top

	/* Zero .bss (core 0 only). */
	ldr	r0, =__bss_start
	ldr	r1, =__bss_end
	movs	r2, #0
1:
	cmp	r0, r1
	bcs	2f
	str	r2, [r0], #4
	b	1b
2:
	/* Initialize MMU and enable it. */
	bl	mmu_init

	/* Call early hardware init */
	bl	os_startup_initialize_hardware_early

	/* Call main hardware init */
	bl	os_startup_initialize_hardware

	/* Run C++ global constructors (.init_array). */
	bl	__libc_init_array

	/* Jump to main() */
	bl	main

hang_core0:
	wfe
	b	hang_core0

/* ---------------------------------------------------------------------------
 * Core 1 / Core 2 — secondary cores: per-core stacks, enable shared MMU,
 *                   then jump to boot_core1 / boot_core2.
 * NO set/way cache maintenance here (see header comment): BootROM leaves
 * these cores with clean caches; mmu_enable() invalidates I-cache, branch
 * predictor and TLBs (all core-local) and sets ACTLR.SMP before enabling
 * MMU + caches, which joins the coherency domain.
 * ------------------------------------------------------------------------- */
core1_start:
	cps	#0x12
	ldr	sp, =__irq_stack_top_core1
	cps	#0x11
	ldr	sp, =__fiq_stack_top_core1
	cps	#0x17
	ldr	sp, =__abt_stack_top_core1
	cps	#0x1B
	ldr	sp, =__und_stack_top_core1
	cps	#0x13
	ldr	sp, =__stack_top_core1
	bl	mmu_enable
	bl	boot_core1
hang_core1:
	wfe
	b	hang_core1

core2_start:
	cps	#0x12
	ldr	sp, =__irq_stack_top_core2
	cps	#0x11
	ldr	sp, =__fiq_stack_top_core2
	cps	#0x17
	ldr	sp, =__abt_stack_top_core2
	cps	#0x1B
	ldr	sp, =__und_stack_top_core2
	cps	#0x13
	ldr	sp, =__stack_top_core2
	bl	mmu_enable
	bl	boot_core2
hang_core2:
	wfe
	b	hang_core2

	.size	_start, . - _start
