/*
 * board-contract.cpp - the facts a board MUST state, checked at compile time.
 *
 * This file produces no code. It exists so that adding a board to this port
 * fails LOUDLY and IMMEDIATELY when the board has not said something the
 * shared sources need -- instead of quietly inheriting the previous board's
 * answer.
 *
 * That distinction is the whole reason the file exists. When this port had
 * one board, every board fact that had been written into a shared file was
 * indistinguishable from an architecture fact: it compiled, it ran, it was
 * correct. The second board turned each one into a bug, and the bugs were
 * silent far more often than loud:
 *
 *   MMU_DRAM_CACHEABLE   a switch defaulting to 0 that the predecessor
 *                        overrode to 1 in all thirteen of its Makefiles. The
 *                        CMake port did not carry the -D, so the default
 *                        applied for the first time ever: DRAM mapped
 *                        non-cacheable and non-shareable, caches off, and a
 *                        synchronous external abort on the kernel's first
 *                        LDREX. A default nothing had ever selected.
 *
 *   PORT_RAM_BASE/END    context_switch.cpp bounds-checked the incoming stack
 *                        pointer against the first board's DRAM window. The
 *                        second board's window was INSIDE it, so the check
 *                        passed and nothing looked wrong. It could as easily
 *                        have been outside.
 *
 *   SEMIHOST trap        HLT #0xF000 is an ARMv8 encoding. On the ARMv7-A
 *                        board the word is UNDEFINED, so the first character
 *                        printed took an exception -- and the fault handler
 *                        printed through the same path, so it took the same
 *                        exception again, forever.
 *
 *   PORT_GREETING        the scheduler announced every board as the first one.
 *
 * Three of those four were silent. A compile error is the only kind of
 * reminder that cannot be missed, so anything a shared source needs from a
 * board is listed below with NO default anywhere in the shared tree.
 *
 * Adding a fact here is a deliberate act: it breaks every board that has not
 * yet answered, which is exactly what it is for.
 */

#include <cstdint>

// The headers a board supplies. Including them here is what makes the checks
// below meaningful -- these are the same headers the shared sources get.
#include <smp.hpp>       // PORT_RAM_BASE, PORT_RAM_END, OS_NCPU (via os-decls)
#include <uart.hpp>      // PORT_BANNER_*

// ---------------------------------------------------------------------------
// 1. The memory map.
// ---------------------------------------------------------------------------
#ifndef PORT_RAM_BASE
#  error "board contract: define PORT_RAM_BASE in boards/<board>/include/smp.hpp -- the low end of the DRAM window a stack pointer is validated against. Do not borrow another board's."
#endif
#ifndef PORT_RAM_END
#  error "board contract: define PORT_RAM_END in boards/<board>/include/smp.hpp -- the high end (exclusive) of that window."
#endif

// ---------------------------------------------------------------------------
// 2. How many CPUs the scheduler has. Set by the build (uos_add_app NCPU n),
//    because it is a property of the board, not of the ISA.
// ---------------------------------------------------------------------------
#ifndef OS_NCPU
#  error "board contract: OS_NCPU must reach the compiler -- set NCPU in the board's branch of test/CMakeLists.txt."
#endif

// ---------------------------------------------------------------------------
// 3. What the board calls itself. PORT_GREETING comes from the port's
//    CMakeLists (_board_define); the PORT_BANNER_* come from the board's
//    uart.hpp and are what the shared test applications print.
// ---------------------------------------------------------------------------
#ifndef PORT_GREETING
#  error "board contract: define PORT_GREETING in the board's branch of CMakeLists.txt -- the scheduler's greeting line."
#endif
#ifndef PORT_BANNER_LONG
#  error "board contract: define PORT_BANNER_LONG in boards/<board>/include/uart.hpp."
#endif
#ifndef PORT_BANNER_SHORT
#  error "board contract: define PORT_BANNER_SHORT in boards/<board>/include/uart.hpp."
#endif
#ifndef PORT_BANNER_ISA
#  error "board contract: define PORT_BANNER_ISA in boards/<board>/include/uart.hpp."
#endif
#ifndef PORT_BANNER_CPU
#  error "board contract: define PORT_BANNER_CPU in boards/<board>/include/uart.hpp."
#endif

// ---------------------------------------------------------------------------
// 4. Which semihosting trap this board's DEBUGGER expects. There is no safe
//    default: the two encodings are not interchangeable, and picking the
//    wrong one is an undefined instruction on the first character printed.
//    A board states its answer by defining SEMIHOST_TRAP_HLT or not, and
//    says so out loud either way by defining SEMIHOST_TRAP_CHOSEN.
// ---------------------------------------------------------------------------
#if defined(SEMIHOST)
#  ifndef SEMIHOST_TRAP_CHOSEN
#    error "board contract: a SEMIHOST build must choose a trap. Define SEMIHOST_TRAP_CHOSEN in the board's branch of CMakeLists.txt, plus SEMIHOST_TRAP_HLT if the board is debugged through an OpenOCD `aarch64` target (HLT #0xF000); leave it out for a `cortex_a` target (SVC 0x123456)."
#  endif
#endif

// ---------------------------------------------------------------------------
// 5. SMP-only facts. A single-CPU board must NOT be asked for these -- this
//    port is shared with boards that have one core, and requiring an IPI
//    number from them would be requiring them to describe hardware they do
//    not have.
// ---------------------------------------------------------------------------
#if OS_NCPU > 1
#  ifndef OS_SMP_IPI_SGI
#    error "board contract: a multi-CPU board must define OS_SMP_IPI_SGI -- the interrupt one core uses to ask another to reschedule. GIC boards give an SGI number; the BCM2837 gives a local mailbox index."
#  endif
#endif

// ---------------------------------------------------------------------------
// The checks a preprocessor cannot make.
// ---------------------------------------------------------------------------
static_assert (PORT_RAM_BASE < PORT_RAM_END,
               "board contract: PORT_RAM_BASE must be below PORT_RAM_END");
static_assert (OS_NCPU >= 1,
               "board contract: OS_NCPU must be at least 1");
static_assert (OS_NCPU <= 8,
               "board contract: OS_NCPU above 8 -- check the board's NCPU, the "
               "per-core arrays in the port are sized from it");
