#include <uart.hpp>
#include <smp.hpp>

// Paste a macro's *value* into the inline asm, not its name.
#define UOS_STR_(x) #x
#define UOS_STR(x)  UOS_STR_(x)

extern "C" void port_ctx_switch_corrupt(uint32_t new_sp, uint32_t old_sp)
{
    __asm__ volatile("cpsid if" ::: "memory");
    uart::uart1 << "\n!!! COOPERATIVE CONTEXT SWITCH CORRUPT STACK POINTER !!!\n";
    uart::uart1 << "  new_sp=0x"; uart::uart1.print_hex(new_sp, 8);
    uart::uart1 << "  old_sp=0x"; uart::uart1.print_hex(old_sp, 8);
    uart::uart1 << "\n";
    while (1) __asm__ volatile("wfi");
}

extern "C" void* port_switch_stacks(void* sp);

extern "C" [[gnu::naked]] void port_ctx_switchHandler()
{
    __asm__ volatile(
        /* Capture original CPSR before disabling interrupts */
        "mrs    r2, cpsr                \n"
        "cpsid  i                       \n"

        /* Save integer context */
        /* r0-r12 + lr: 14 words (56 bytes) */
        "push   {r0-r12, lr}            \n"
        /* CPSR + PC (PC = lr = the caller's return address): 2 words */
        "push   {r2, lr}                \n"

        /* Save VFP context — keep SP 8-byte aligned for vpush/vpop */
        /* FPSCR + padding: 2 words */
        "vmrs   r2, fpscr               \n"
        "push   {r2, r3}                \n"
        /* The FULL NEON register file: d16-d31 (q8-q15) are call-clobbered
         * under AAPCS, but this is a *preemptive* switch that can land mid-
         * NEON, and the thread may resume on a different core. 32 words each. */
        "vpush  {d16-d31}               \n"
        /* d0-d15: 32 words (128 bytes) — SP aligned */
        "vpush  {d0-d15}                \n"

        /* Select next task: port_switch_stacks(current_sp) → r0 = new task's SP */
        "mov    r0, sp                  \n"
        "bl     port_switch_stacks      \n"

        /* Safety check: is new_sp inside this board's DRAM window? The bounds
         * are a board fact (boards/<board>/include/smp.hpp), not an ISA one.
         * They used to be the Pi's literals in this shared file, which on any
         * other board is a check against the wrong memory map -- wider here,
         * so it passed, but it would have been narrower just as easily. */
        "ldr    r1, =" UOS_STR (PORT_RAM_BASE) " \n"
        "cmp    r0, r1                  \n"
        "blo    .Lcorrupt_sp            \n"
        "ldr    r1, =" UOS_STR (PORT_RAM_END) " \n"
        "cmp    r0, r1                  \n"
        "bhs    .Lcorrupt_sp            \n"

        "mov    sp, r0                  \n"   /* install new task's stack */

        /* SMP deferred publish: only NOW - with SP off the outgoing task's
         * stack - may that task become claimable by other cores. The C
         * scheduler staged {&old->stack_ptr, frame} in per-CPU slots. */
        "mrc    p15, 0, r0, c0, c0, 5   \n"
        "and    r0, r0, #3              \n"
        "movw   r1, #:lower16:_smp_pub_addr \n"
        "movt   r1, #:upper16:_smp_pub_addr \n"
        "ldr    r2, [r1, r0, lsl #2]    \n"
        "cmp    r2, #0                  \n"
        "beq    8f                      \n"
        "movw   r3, #:lower16:_smp_pub_val \n"
        "movt   r3, #:upper16:_smp_pub_val \n"
        "ldr    r3, [r3, r0, lsl #2]    \n"
        "str    r3, [r2]                \n"
        "mov    r3, #0                  \n"
        "str    r3, [r1, r0, lsl #2]    \n"
        "dmb                            \n"
        "8:                             \n"

        /* Drop any LDREX reservation before entering the incoming thread, so
         * a STREX there cannot succeed against a reservation taken by whoever
         * ran on this core before (ARM ARM: CLREX on context switch). */
        "clrex                          \n"

        /* Restore VFP context */
        "vpop   {d0-d15}                \n"
        "vpop   {d16-d31}               \n"
        "pop    {r2, r3}                \n"
        "vmsr   fpscr, r2               \n"

        /* Restore integer context — IRQ-mode atomic return */
        /* Pop saved CPSR and PC */
        "pop    {r0, r1}                \n"   /* r0 = CPSR, r1 = PC */
        /* Set up IRQ-mode registers for the atomic movs pc, lr */
        "cps    #0x12                   \n"   /* enter IRQ mode */
        "msr    spsr_cxsf, r0           \n"   /* SPSR_irq ← saved CPSR */
        "mov    lr, r1                  \n"   /* LR_irq  ← saved PC */
        "cps    #0x13                   \n"   /* back to SVC mode */
        /* Restore general-purpose registers */
        "pop    {r0-r12, lr}            \n"
        /* Atomic branch: restores CPSR from SPSR_irq, PC from LR_irq */
        "cps    #0x12                   \n"   /* IRQ mode */
        "movs   pc, lr                  \n"   /* CPSR ← SPSR_irq, PC ← LR_irq */

        ".Lcorrupt_sp:                  \n"
        "cpsid  if                      \n"
        "mov    r1, sp                  \n"   /* r1 = old_sp */
        "bl     port_ctx_switch_corrupt \n"
    );
}
