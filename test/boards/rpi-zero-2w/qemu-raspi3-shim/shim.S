/*
 * shim.S - AArch64 -> AArch32 bootstrap for running the 32-bit µOS++ Pi
 *          Zero 2W kernel under QEMU's 64-bit `raspi3b` machine (real
 *          Cortex-A53). Not used on real hardware (there config.txt
 *          arm_64bit=0 boots AArch32 directly).
 *
 * QEMU raspi3b loads this as -kernel at 0x80000 and starts CORE 0 here in
 * AArch64 EL2; cores 1-3 wait in the firmware spin-table, each polling a
 * release word, then jump (in AArch64) to the address written there.
 *
 * The shim:
 *   - core 0: release cores 1-3 (point their spin-table word at this shim),
 *             then drop itself to AArch32 SVC at 0x10000 (the AArch32 _start,
 *             loaded separately via `-device loader,addr=0x10000`).
 *   - cores 1-3: likewise drop to AArch32 SVC at 0x10000, where the normal
 *             AArch32 startup routes them (by MPIDR) into secondary_park to
 *             await the kernel's own __smp_spin + SEV release.
 */
	.section .text.boot, "ax"
	.global _start
_start:
	/* Every core: join the SMP coherency domain BEFORE enabling caches in
	 * the AArch32 kernel. CPUECTLR_EL1.SMPEN (bit 6) — A53 encoding
	 * S3_1_C15_C2_1. On real hardware the firmware enters HYP and startup.S
	 * sets this; here we enter EL1 (SVC) so the shim must do it. */
	mrs	x2, S3_1_c15_c2_1
	orr	x2, x2, #(1 << 6)
	msr	S3_1_c15_c2_1, x2
	isb

	/* Allow AArch32 EL1 access to the physical timer/counter
	 * (CNTHCTL_EL2.EL1PCEN | EL1PCTEN = bits[1:0]). */
	mrs	x2, cnthctl_el2
	orr	x2, x2, #0x3
	msr	cnthctl_el2, x2

	mrs	x0, mpidr_el1
	and	x0, x0, #3
	cbnz	x0, drop_to_aarch32        // secondaries: skip the release step

	/* Core 0: release cores 1-3 from QEMU's AArch64 spin-table. The
	 * raspi3 firmware release addresses are 0xE0/0xE8/0xF0 (core*8 + 0xD8).
	 * Write this shim's entry there and SEV so they jump back here. */
	adr	x1, _start
	mov	x2, #0xE0
	str	x1, [x2]                   // core 1 release word
	mov	x2, #0xE8
	str	x1, [x2]                   // core 2
	mov	x2, #0xF0
	str	x1, [x2]                   // core 3
	dsb	sy
	sev

drop_to_aarch32:
	/* EL1 will be AArch32: HCR_EL2.RW = 0. */
	mrs	x1, hcr_el2
	bic	x1, x1, #(1 << 31)
	msr	hcr_el2, x1

	/* Return to AArch32 SVC (0x13) with I/F/A masked (0x1C0). */
	mov	x1, #0x1D3
	msr	spsr_el2, x1

	/* Enter the AArch32 kernel at 0x10000. */
	movz	x1, #0x0000
	movk	x1, #0x0001, lsl #16       // 0x00010000
	msr	elr_el2, x1
	eret
