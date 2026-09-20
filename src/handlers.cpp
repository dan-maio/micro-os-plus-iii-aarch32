/**
 * @file handlers.cpp
 * @brief Exception / IRQ handlers for µOS++ on RK3506 (Luckfox Lyra B)
 */

#include <cstdint>

extern "C" {

void handle_undefined_instruction(void* ctx, std::uint32_t fault_pc);
void handle_prefetch_abort(void* ctx, std::uint32_t fault_pc, std::uint32_t ifsr, std::uint32_t ifar);
void handle_data_abort(void* ctx, std::uint32_t fault_pc, std::uint32_t dfsr, std::uint32_t dfar);
uint32_t* port_isr_handler(uint32_t* current_sp);

} // extern "C"

// ============================================================================
// __libc_init_array — C++ static constructor runner
// ============================================================================

extern "C" [[gnu::weak]] void __libc_init_array()
{
    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    for (auto* fn = __init_array_start; fn != __init_array_end; ++fn)
        (*fn)();
}

extern "C" {

extern void (*__fini_array_start[])(void) __attribute__((weak));
extern void (*__fini_array_end[])(void) __attribute__((weak));

void os_run_fini_array(void)
{
    int count = __fini_array_end - __fini_array_start;
    for (int i = count; i > 0; i--)
    {
        __fini_array_start[i - 1]();
    }
}

}

// ============================================================================
// Exception handlers
// ============================================================================

extern "C" [[gnu::naked]] void undefined_handler()
{
    __asm__ volatile(
        "sub    lr, lr, #4              \n"   /* fault_pc = LR - 4 */
        "push   {r0-r12, lr}            \n"   /* 14 words: r0..r12, adjusted LR */
        "mrs    r0, spsr                \n"
        "push   {r0}                    \n"   /* [sp+0]=SPSR, [sp+4..56]=r0..lr */

        "add    r0, sp, #4              \n"   /* r0 = ExceptionContext* */
        "ldr    r1, [sp, #56]           \n"   /* r1 = fault_pc */
        "bl     handle_undefined_instruction\n"

        "cmp    r0, #0                  \n"
        "beq    .Lundef_halt            \n"

        /* Non-zero: resume at returned address */
        "mov    lr, r0                  \n"
        "pop    {r0}                    \n"
        "msr    spsr_cxsf, r0           \n"
        "pop    {r0-r12}                \n"
        "add    sp, sp, #4              \n"   /* skip saved lr */
        "movs   pc, lr                  \n"   /* return; CPSR = SPSR */

        ".Lundef_halt:                  \n"
        "wfi                            \n"
        "b      .Lundef_halt            \n"
    );
}

extern "C" [[gnu::naked, gnu::weak]] void svc_handler()
{
    __asm__ volatile(
        ".Lsvc_loop:        \n"
        "b      .Lsvc_loop  \n"
    );
}

extern "C" [[gnu::naked]] void prefetch_handler()
{
    __asm__ volatile(
        "sub    lr, lr, #4              \n"
        "push   {r0-r12, lr}            \n"
        "mrs    r0, spsr                \n"
        "push   {r0}                    \n"

        "mrc    p15, 0, r2, c5, c0, 1  \n"   /* r2 = IFSR */
        "mrc    p15, 0, r3, c6, c0, 2  \n"   /* r3 = IFAR */
        "add    r0, sp, #4              \n"   /* r0 = ExceptionContext* */
        "ldr    r1, [sp, #56]           \n"   /* r1 = fault_pc */
        "bl     handle_prefetch_abort   \n"

        "cmp    r0, #0                  \n"
        "beq    .Lprefetch_halt         \n"

        "mov    lr, r0                  \n"
        "pop    {r0}                    \n"
        "msr    spsr_cxsf, r0           \n"
        "pop    {r0-r12}                \n"
        "add    sp, sp, #4              \n"
        "movs   pc, lr                  \n"

        ".Lprefetch_halt:               \n"
        "wfi                            \n"
        "b      .Lprefetch_halt         \n"
    );
}

extern "C" [[gnu::naked]] void data_handler()
{
    __asm__ volatile(
        "sub    lr, lr, #8              \n"   /* fault_pc = LR - 8 for data abort */
        "push   {r0-r12, lr}            \n"
        "mrs    r0, spsr                \n"
        "push   {r0}                    \n"

        "mrc    p15, 0, r2, c5, c0, 0  \n"   /* r2 = DFSR */
        "mrc    p15, 0, r3, c6, c0, 0  \n"   /* r3 = DFAR */
        "add    r0, sp, #4              \n"   /* r0 = ExceptionContext* */
        "ldr    r1, [sp, #56]           \n"   /* r1 = fault_pc */
        "bl     handle_data_abort       \n"

        "cmp    r0, #0                  \n"
        "beq    .Ldata_halt             \n"

        "mov    lr, r0                  \n"
        "pop    {r0}                    \n"
        "msr    spsr_cxsf, r0           \n"
        "pop    {r0-r12}                \n"
        "add    sp, sp, #4              \n"
        "movs   pc, lr                  \n"

        ".Ldata_halt:                   \n"
        "wfi                            \n"
        "b      .Ldata_halt             \n"
    );
}

extern "C" [[gnu::naked, gnu::weak]] void fiq_handler()
{
    __asm__ volatile(
        ".Lfiq_loop:        \n"
        "b      .Lfiq_loop  \n"
    );
}

extern "C" [[gnu::naked]] void irq_handler()
{
    __asm__ volatile(
        "sub    lr, lr, #4              \n"   /* Adjust LR_irq to return PC */

        /* Save r0-r3 to IRQ stack temporarily */
        "push   {r0-r3}                 \n"

        /* Capture SPSR_irq and adjusted LR while still in IRQ mode */
        "mrs    r0, spsr                \n"   /* r0 = interrupted CPSR */
        "mov    r1, lr                  \n"   /* r1 = return PC */

        /* Switch to SVC mode — build frame on SVC (task) stack */
        "cps    #0x13                   \n"

        /* Save caller-saved r4-r12 and lr_svc */
        "push   {r4-r12, lr}            \n"   /* 10 words */

        /* Switch back to IRQ mode to retrieve r0-r3 from IRQ stack */
        "cps    #0x12                   \n"
        "pop    {r4-r7}                 \n"   /* r4=r0, r5=r1, r6=r2, r7=r3 */
        "cps    #0x13                   \n"

        /* Push orig r0-r3 on SVC stack */
        "push   {r4-r7}                 \n"   /* 4 words */

        /* Push CPSR (interrupted) and return PC */
        "push   {r0, r1}                \n"   /* [sp+0]=CPSR, [sp+4]=PC */

        /* Save VFP state — keep SP 8-byte aligned for vpush/vpop */
        "vmrs   r2, fpscr               \n"
        "push   {r2, r3}                \n"   /* FPSCR + pad to align */
        /* The FULL NEON register file. d16-d31 (q8-q15) are call-clobbered
         * under AAPCS, so a *function call* need not preserve them - but this
         * is a preemptive interrupt that can land mid-NEON, and the thread may
         * be resumed on another core. Saving only d0-d15 silently corrupts it. */
        "vpush  {d16-d31}               \n"   /* 32 words (128 bytes) */
        "vpush  {d0-d15}                \n"   /* 32 words (128 bytes) */

        /* Call C++ ISR handler with pointer to current context frame */
        "mov    r0, sp                  \n"
        "bl     port_isr_handler        \n"   /* returns (possibly new) SP in r0 */
        "mov    sp, r0                  \n"   /* switch to returned SP */

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
         * ran on this core before -- in particular the interrupted thread,
         * which may have been preempted between its own LDREX and STREX
         * (ARM ARM: CLREX on context switch). */
        "clrex                          \n"

        /* Restore VFP state */
        "vpop   {d0-d15}                \n"
        "vpop   {d16-d31}               \n"
        "pop    {r2, r3}                \n"
        "vmsr   fpscr, r2               \n"

        /* Restore CPSR and return PC */
        "pop    {r0, r1}                \n"   /* r0=CPSR, r1=PC */

        /* Load IRQ-mode SPSR and LR for atomic return */
        "cps    #0x12                   \n"
        "msr    spsr_cxsf, r0           \n"   /* SPSR_irq = saved CPSR */
        "mov    lr, r1                  \n"   /* LR_irq  = saved PC */
        "cps    #0x13                   \n"

        /* Restore general-purpose registers from SVC stack */
        "pop    {r0-r12, lr}            \n"

        /* Atomic return: PC = LR_irq, CPSR = SPSR_irq */
        "cps    #0x12                   \n"
        "movs   pc, lr                  \n"
    );
}
