/*
 * startup.S - Multicore startup for µOS++ on the Raspberry Pi Zero 2W
 *             (BCM2837, 4x Cortex-A53, AArch32 / kernel7.img).
 *
 * Entry conditions differ by environment:
 *   - Real hardware: the GPU firmware/armstub enters kernel7.img on CORE 0 in
 *     HYP mode; cores 1-3 are held by the armstub spin loop (released later in
 *     Phase 2 by writing their mailbox-3 set register).
 *   - QEMU raspi3b: ALL four cores are started at _start simultaneously.
 *
 * Dispatch by MPIDR.Aff0:
 *   core 0 : HYP->SVC, SMP/timer enable, defensive cache invalidate, stacks,
 *            .bss clear, mmu_init, hardware init -> main()
 *   core 1-3 : HYP->SVC, SMP enable, then park in secondary_park polling their
 *            RAM spin slot (__smp_spin[core]); Phase 2 fills the slot to launch
 *            them into boot_coreN. (In Phase 1 they simply idle.)
 *
 * The defensive full set/way D-cache invalidate runs on CORE 0 ONLY: a
 * set/way invalidate from a secondary while core 0 holds live dirty lines in
 * the shared L2 would discard them without writeback.
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
	/* Mask IRQ + FIQ while we set up. */
	cpsid	if

	/* --- If we entered in HYP mode (real Pi firmware), drop to SVC.
	 * Skip entirely if already in SVC (e.g. some stubs / QEMU secondaries). */
	mrs	r0, cpsr
	and	r1, r0, #0x1F
	cmp	r1, #0x1A                   @ 0x1A = HYP mode
	bne	1f

	/* We entered in HYP (EL2), as the Pi firmware does for 32-bit kernels.
	 *
	 * EL2-only setup that MUST be done here:
	 *   - CNTHCTL (mcr p15,4,..,c14,c1,0): grant NS-EL1 access to the
	 *     physical counter + timer (EL1PCTEN|EL1PCEN).  Without this the
	 *     kernel's timer ISR would UNDEF when it writes CNTP_TVAL at EL1.
	 * Do NOT touch NSACR (EL3-only) or CPUECTLR here — the firmware armstub
	 * has already set SMPEN/FP access at EL3, and NSACR would UNDEF from
	 * non-secure EL2 (VBAR not installed yet -> silent hang). */

	mrc	p15, 4, r2, c14, c1, 0      @ CNTHCTL_EL2
	orr	r2, r2, #0x3                @ EL1PCTEN | EL1PCEN
	mcr	p15, 4, r2, c14, c1, 0
	isb

	/* Drop HYP -> SVC via SPSR_hyp/ELR_hyp + eret. */
	bic	r0, r0, #0x1F
	orr	r0, r0, #0x13               @ SVC mode
	orr	r0, r0, #0xC0               @ keep I+F masked
	msr	spsr_hyp, r0
	ldr	r0, =1f
	msr	elr_hyp, r0
	eret
1:
	/* Ensure SVC mode explicitly (harmless if already SVC). */
	cps	#0x13

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

	/* Dispatch by MPIDR core ID (bits [1:0]). */
	mrc	p15, 0, r0, c0, c0, 5
	and	r0, r0, #3
	cmp	r0, #0
	beq	core0_start
	b	secondary_park              @ cores 1-3

/* ---------------------------------------------------------------------------
 * Core 0 — Primary core
 * ------------------------------------------------------------------------- */
core0_start:
	/* ---- DEFENSIVE BOOT (hot-entry = cold-boot), CORE 0 ONLY ---------- */
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
	/* Initialize MMU and enable it (also joins caches to coherency). */
	bl	mmu_init

	bl	os_startup_initialize_hardware_early
	bl	os_startup_initialize_hardware
	bl	__libc_init_array
	bl	main

hang_core0:
	wfe
	b	hang_core0

/* ---------------------------------------------------------------------------
 * Secondary cores (1-3) — park until Phase 2 releases them.
 *
 * Each polls __smp_spin[core] (a RAM slot in .bss). When core 0 writes a
 * launch entry there and SEVs, the core jumps to it (Phase 2). In Phase 1 the
 * slot stays 0 and the core simply idles here, sharing nothing yet.
 * ------------------------------------------------------------------------- */
	.global secondary_park
secondary_park:
	mrc	p15, 0, r4, c0, c0, 5       @ r4 = core id
	and	r4, r4, #3
	ldr	r5, =__smp_spin
1:	wfe
	ldr	r6, [r5, r4, lsl #2]        @ r6 = __smp_spin[core]
	cmp	r6, #0
	beq	1b
	bx	r6                          @ enter coreN_entry (r4=core)

	.size	_start, . - _start

/* ---------------------------------------------------------------------------
 * Per-core secondary entries (Phase 2). Each sets its banked stacks, enables
 * the shared MMU + caches on this core, then jumps to boot_coreN ->
 * port_smp_secondary_start. start_secondary_cores() writes the matching entry
 * into __smp_spin[core] and SEVs.
 * ------------------------------------------------------------------------- */
	.global core1_entry, core2_entry, core3_entry
core1_entry:
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
1:	wfe
	b	1b

core2_entry:
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
1:	wfe
	b	1b

core3_entry:
	cps	#0x12
	ldr	sp, =__irq_stack_top_core3
	cps	#0x11
	ldr	sp, =__fiq_stack_top_core3
	cps	#0x17
	ldr	sp, =__abt_stack_top_core3
	cps	#0x1B
	ldr	sp, =__und_stack_top_core3
	cps	#0x13
	ldr	sp, =__stack_top_core3
	bl	mmu_enable
	bl	boot_core3
1:	wfe
	b	1b
