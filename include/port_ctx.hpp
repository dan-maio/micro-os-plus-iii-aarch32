/*
 * port_ctx.hpp - the saved-context frame the AArch32 port's assembly builds.
 *
 * Shared by src/rtos/os-core.cpp (which creates and switches frames) and by
 * each board's port_isr.cpp (which validates one before switching to it).
 * The layout is a fact about the ISA and about this port's own assembly, not
 * about any board, so there is one copy.
 */
#pragma once

#include <cstddef>
#include <cstdint>

/* Saved context frame, low address (= the saved SP) first. Must mirror the
 * push/vpush order in irq_handler() (handlers.cpp) and port_ctx_switchHandler()
 * (context_switch.cpp), which build the frame back-to-front:
 *
 *   push  {r0-r12, lr}   ->  r[13] + lr   (highest addresses)
 *   push  {r2, lr}       ->  cpsr, pc
 *   push  {r2, r3}       ->  fpscr, pad
 *   vpush {d16-d31}      ->  vfp_hi
 *   vpush {d0-d15}       ->  vfp          (lowest, SP points here)
 *
 * Both halves of the VFP/NEON register file are saved: d0-d15 are the AAPCS
 * argument/callee-saved doubles, but d16-d31 (q8-q15) are equally live in
 * NEON code, and a *preemptive* switch can land anywhere -- saving only the
 * low half silently corrupts a thread interrupted mid-NEON. */
struct ctx_t
{
  uint32_t vfp[32];    /* d0-d15  stored as 32 x 32-bit words (128 bytes) */
  uint32_t vfp_hi[32]; /* d16-d31 stored as 32 x 32-bit words (128 bytes) */
  uint32_t fpscr;
  uint32_t pad;    /* alignment padding — matches push {r2, r3} in context switch */
  uint32_t cpsr;
  uint32_t pc;
  uint32_t r[13]; /* r0 - r12 */
  uint32_t lr;
};

/* Derived from ctx_t so the frame checks below cannot drift away from the
 * layout the assembly actually builds. */
inline constexpr std::size_t ctx_cpsr_word
    = offsetof (ctx_t, cpsr) / sizeof (uint32_t);
inline constexpr std::size_t ctx_size_bytes = sizeof (ctx_t);
