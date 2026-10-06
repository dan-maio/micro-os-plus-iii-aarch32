/**
 * @file    exception_handler.cpp
 * @brief   Exception Handler Implementation for ARM Cortex-A7
 */

#include "exception_handler.hpp"
#include "uart.hpp"
#include "mmu.hpp"
#include <cstdint>

namespace exception {

namespace {

// ---------------------------------------------------------------------------
// The fault dump's own console: the UART and NOTHING ELSE.
//
// uart1's normal path mirrors every line to the semihosting channel, which is
// a trap instruction. That is fine for a test's output and wrong for a fault
// handler, because a fault handler must not depend on anything that can
// itself fault:
//
//   * if the debugger is not attached, or is attached but has lost the link,
//     the trap is unhandled;
//   * if the trap encoding is wrong for the core -- HLT #0xF000 on an
//     ARMv7-A, where that word has no encoding -- it is an Undefined
//     Instruction.
//
// Either way the handler re-enters itself at the first thing it prints, and
// the board loops forever emitting the first line of a dump. That is exactly
// what a Luckfox Lyra did: a wall of "+---...---+" and not one word of the
// fault it was trying to report.
//
// So the dump goes out through puts_uart()/putc_uart(), which touch the UART
// only. The console the operator is watching is the one that matters when
// the debugger is the thing that broke.
// ---------------------------------------------------------------------------
struct FaultConsole
{
  const FaultConsole& operator<< (const char* str) const noexcept
  {
    uart::uart1.puts_uart (str);
    return *this;
  }

  const FaultConsole& operator<< (char c) const noexcept
  {
    if (c == '\n')
      {
        uart::uart1.putc_uart ('\r');
      }
    uart::uart1.putc_uart (c);
    return *this;
  }

  const FaultConsole& operator<< (std::uint32_t value) const noexcept
  {
    char out[11];
    int n = 0;
    if (value == 0)
      {
        out[n++] = '0';
      }
    else
      {
        char digits[10];
        int pos = 0;
        while (value > 0)
          {
            digits[pos++] = static_cast<char> ('0' + (value % 10));
            value /= 10;
          }
        while (pos > 0)
          {
            out[n++] = digits[--pos];
          }
      }
    out[n] = '\0';
    uart::uart1.puts_uart (out);
    return *this;
  }

  const FaultConsole& operator<< (int value) const noexcept
  {
    std::uint32_t mag = (value < 0)
        ? (0u - static_cast<std::uint32_t> (value))
        : static_cast<std::uint32_t> (value);
    if (value < 0)
      {
        uart::uart1.putc_uart ('-');
      }
    return *this << mag;
  }

  void print_hex (std::uint32_t value, int width = 8) const noexcept
  {
    static constexpr char hex_chars[] = "0123456789ABCDEF";
    if (width < 1)
      width = 1;
    if (width > 8)
      width = 8;
    char out[9];
    int n = 0;
    for (int i = width - 1; i >= 0; --i)
      {
        out[n++] = hex_chars[(value >> (i * 4)) & 0xF];
      }
    out[n] = '\0';
    uart::uart1.puts_uart (out);
  }
};

inline constexpr FaultConsole fault_out{};

} // namespace

TestMode test_mode = {
    .active           = false,
    .exception_caught = false,
    .continue_address = 0,
    .last_exception   = ExceptionType::UndefinedInstruction,
    .fault_address    = 0,
    .test_line        = 0,
    .test_file        = nullptr,
};

const char* fault_status_string(FaultStatus status) {
    switch (status) {
        case FaultStatus::AlignmentFault:        return "Alignment fault";
        case FaultStatus::DebugEvent:            return "Debug event";
        case FaultStatus::AccessFlagSection:     return "Access flag fault (section)";
        case FaultStatus::IcacheMaintenance:     return "I-cache maintenance fault";
        case FaultStatus::TranslationSection:    return "Translation fault (section)";
        case FaultStatus::AccessFlagPage:        return "Access flag fault (page)";
        case FaultStatus::TranslationPage:       return "Translation fault (page)";
        case FaultStatus::SyncExternalAbort:     return "Synchronous external abort";
        case FaultStatus::DomainSection:         return "Domain fault (section)";
        case FaultStatus::DomainPage:            return "Domain fault (page)";
        case FaultStatus::TranslationTableWalk1: return "Translation table walk (L1)";
        case FaultStatus::PermissionSection:     return "Permission fault (section)";
        case FaultStatus::TranslationTableWalk2: return "Translation table walk (L2)";
        case FaultStatus::PermissionPage:        return "Permission fault (page)";
        case FaultStatus::AsyncExternalAbort:    return "Asynchronous external abort";
        case FaultStatus::AsyncParityError:      return "Async parity/ECC error";
        case FaultStatus::SyncParityError:       return "Sync parity/ECC error";
        default:                                 return "Unknown fault";
    }
}

const char* exception_type_string(ExceptionType type) {
    switch (type) {
        case ExceptionType::UndefinedInstruction: return "Undefined Instruction";
        case ExceptionType::PrefetchAbort:        return "Prefetch Abort";
        case ExceptionType::DataAbort:            return "Data Abort";
        default:                                  return "Unknown Exception";
    }
}

namespace {

void print_separator() {
    fault_out << "+--------------------------------------------------+\n";
}

void print_hex32(const char* label, std::uint32_t value) {
    fault_out << "  " << label << ": 0x";
    fault_out.print_hex(value, 8);
    fault_out << "\n";
}

extern "C" unsigned port_cpu_id(void);

void print_register_dump(const ExceptionContext* ctx, std::uint32_t fault_pc, std::uint32_t spsr) {
    fault_out << "  Register Dump (Core ";
    fault_out.print_hex(port_cpu_id(), 1);
    fault_out << "):\n";
    fault_out << "    R0 =0x"; fault_out.print_hex(ctx->r0, 8);
    fault_out << "  R1 =0x"; fault_out.print_hex(ctx->r1, 8);
    fault_out << "  R2 =0x"; fault_out.print_hex(ctx->r2, 8);
    fault_out << "  R3 =0x"; fault_out.print_hex(ctx->r3, 8);
    fault_out << "\n";

    fault_out << "    R4 =0x"; fault_out.print_hex(ctx->r4, 8);
    fault_out << "  R5 =0x"; fault_out.print_hex(ctx->r5, 8);
    fault_out << "  R6 =0x"; fault_out.print_hex(ctx->r6, 8);
    fault_out << "  R7 =0x"; fault_out.print_hex(ctx->r7, 8);
    fault_out << "\n";

    fault_out << "    R8 =0x"; fault_out.print_hex(ctx->r8, 8);
    fault_out << "  R9 =0x"; fault_out.print_hex(ctx->r9, 8);
    fault_out << "  R10=0x"; fault_out.print_hex(ctx->r10, 8);
    fault_out << "  R11=0x"; fault_out.print_hex(ctx->r11, 8);
    fault_out << "\n";

    fault_out << "    R12=0x"; fault_out.print_hex(ctx->r12, 8);
    fault_out << "  PC =0x"; fault_out.print_hex(fault_pc, 8);
    fault_out << "\n";

    fault_out << "    SPSR=0x"; fault_out.print_hex(spsr, 8);
    fault_out << " [";
    fault_out << ((spsr & (1 << 31)) ? 'N' : 'n');
    fault_out << ((spsr & (1 << 30)) ? 'Z' : 'z');
    fault_out << ((spsr & (1 << 29)) ? 'C' : 'c');
    fault_out << ((spsr & (1 << 28)) ? 'V' : 'v');
    fault_out << "] Mode=";
    std::uint32_t mode = spsr & 0x1F;
    switch (mode) {
        case 0x10: fault_out << "USR"; break;
        case 0x11: fault_out << "FIQ"; break;
        case 0x12: fault_out << "IRQ"; break;
        case 0x13: fault_out << "SVC"; break;
        case 0x17: fault_out << "ABT"; break;
        case 0x1B: fault_out << "UND"; break;
        case 0x1F: fault_out << "SYS"; break;
        default:   fault_out << "???"; break;
    }
    fault_out << " IRQ="; fault_out << ((spsr & 0x80) ? "off" : "on");
    fault_out << " FIQ="; fault_out << ((spsr & 0x40) ? "off" : "on");
    fault_out << "\n";
}

void print_instruction_at(std::uint32_t address) {
    volatile std::uint32_t* ptr = reinterpret_cast<volatile std::uint32_t*>(address & ~3);
    std::uint32_t instruction = *ptr;

    fault_out << "  Instruction at 0x";
    fault_out.print_hex(address, 8);
    fault_out << ": 0x";
    fault_out.print_hex(instruction, 8);
    fault_out << "\n";
}

void print_test_location() {
    if (test_mode.test_file != nullptr) {
        fault_out << "  Test Location: " << test_mode.test_file
                    << ":" << test_mode.test_line << "\n";
    }
}

} // anonymous namespace

namespace {

inline std::uint32_t read_spsr() {
    std::uint32_t spsr;
    __asm__ volatile("mrs %0, spsr" : "=r"(spsr));
    return spsr;
}

} // anonymous namespace

extern "C" std::uint32_t handle_undefined_instruction(
    ExceptionContext* ctx,
    std::uint32_t fault_pc)
{
    std::uint32_t spsr = read_spsr();

    std::uint32_t cpacr;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 2" : "=r"(cpacr));

    std::uint32_t fpexc = 0xDEADDEAD;
    std::uint32_t fpscr = 0xDEADDEAD;
    std::uint32_t cp10_access = (cpacr >> 20) & 0x3;
    std::uint32_t cp11_access = (cpacr >> 22) & 0x3;
    if (cp10_access != 0) {
        __asm__ volatile("vmrs %0, fpexc" : "=r"(fpexc));
        if (fpexc & (1u << 30)) {
            __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
        }
    }

    print_separator();
    fault_out << "| EXCEPTION: Undefined Instruction               |\n";
    print_separator();

    print_hex32("Faulting PC", fault_pc);
    print_instruction_at(fault_pc);

    print_hex32("CPACR", cpacr);
    fault_out << "    CP10=" << cp10_access << " CP11=" << cp11_access;
    if (cp10_access == 3 && cp11_access == 3)
        fault_out << " (full access)\n";
    else if (cp10_access == 0 && cp11_access == 0)
        fault_out << " (NO ACCESS)\n";
    else
        fault_out << " (restricted)\n";

    if (fpexc != 0xDEADDEAD) {
        print_hex32("FPEXC", fpexc);
        fault_out << "    EN=" << ((fpexc >> 30) & 1)
                     << " EX=" << ((fpexc >> 31) & 1)
                     << " DEX=" << ((fpexc >> 29) & 1)
                     << ((fpexc & (1u << 30)) ? " (VFP enabled)" : " (VFP DISABLED!)")
                     << "\n";
        if (fpscr != 0xDEADDEAD) {
            print_hex32("FPSCR", fpscr);
            fault_out << "    Traps:";
            if (fpscr & (1u << 8))  fault_out << " IOE";
            if (fpscr & (1u << 9))  fault_out << " DZE";
            if (fpscr & (1u << 10)) fault_out << " OFE";
            if (fpscr & (1u << 11)) fault_out << " UFE";
            if (fpscr & (1u << 12)) fault_out << " IXE";
            if (fpscr & (1u << 15)) fault_out << " IDE";
            if (!(fpscr & 0x9F00))  fault_out << " none";
            fault_out << "  Flags:";
            if (fpscr & (1u << 0))  fault_out << " IOC";
            if (fpscr & (1u << 1))  fault_out << " DZC";
            if (fpscr & (1u << 2))  fault_out << " OFC";
            if (fpscr & (1u << 3))  fault_out << " UFC";
            if (fpscr & (1u << 4))  fault_out << " IXC";
            if (fpscr & (1u << 7))  fault_out << " IDC";
            if (!(fpscr & 0x9F))    fault_out << " none";
            fault_out << "\n";
        }
    } else {
        fault_out << "  FPEXC: unreadable (CP10/CP11 denied by CPACR)\n";
    }

    print_register_dump(ctx, fault_pc, spsr);

    if (test_mode.active) {
        print_test_location();
        fault_out << "  [TEST MODE] Continuing execution...\n";
        print_separator();

        test_mode.exception_caught = true;
        test_mode.last_exception = ExceptionType::UndefinedInstruction;
        test_mode.fault_address = fault_pc;

        return test_mode.continue_address;
    }

    fault_out << "  [FATAL] System halted.\n";
    print_separator();

    return 0;
}

extern "C" std::uint32_t handle_prefetch_abort(
    ExceptionContext* ctx,
    std::uint32_t fault_pc,
    std::uint32_t ifsr,
    std::uint32_t ifar)
{
    std::uint32_t spsr = read_spsr();

    print_separator();
    fault_out << "| EXCEPTION: Prefetch Abort                      |\n";
    print_separator();

    print_hex32("Faulting PC", fault_pc);
    print_hex32("IFSR", ifsr);
    print_hex32("IFAR", ifar);

    std::uint32_t fs = ((ifsr >> 10) & 1) << 4 | (ifsr & 0xF);
    fault_out << "  Fault: " << fault_status_string(static_cast<FaultStatus>(fs)) << "\n";

    print_register_dump(ctx, fault_pc, spsr);

    if (test_mode.active) {
        print_test_location();
        fault_out << "  [TEST MODE] Continuing execution...\n";
        print_separator();

        test_mode.exception_caught = true;
        test_mode.last_exception = ExceptionType::PrefetchAbort;
        test_mode.fault_address = ifar;

        return test_mode.continue_address;
    }

    fault_out << "  [FATAL] System halted.\n";
    print_separator();

    return 0;
}

extern "C" std::uint32_t handle_data_abort(
    ExceptionContext* ctx,
    std::uint32_t fault_pc,
    std::uint32_t dfsr,
    std::uint32_t dfar)
{
    std::uint32_t spsr = read_spsr();

    print_separator();
    fault_out << "| EXCEPTION: Data Abort                          |\n";
    print_separator();

    print_hex32("Faulting PC", fault_pc);
    print_hex32("DFSR", dfsr);
    print_hex32("DFAR", dfar);

    std::uint32_t fs = ((dfsr >> 10) & 1) << 4 | (dfsr & 0xF);
    fault_out << "  Fault: " << fault_status_string(static_cast<FaultStatus>(fs)) << "\n";

    bool is_write = (dfsr >> 11) & 1;
    fault_out << "  Access: " << (is_write ? "Write" : "Read") << "\n";

    print_instruction_at(fault_pc);
    print_register_dump(ctx, fault_pc, spsr);

    if (test_mode.active) {
        print_test_location();
        fault_out << "  [TEST MODE] Continuing execution...\n";
        print_separator();

        test_mode.exception_caught = true;
        test_mode.last_exception = ExceptionType::DataAbort;
        test_mode.fault_address = dfar;

        return test_mode.continue_address;
    }

    fault_out << "  [FATAL] System halted.\n";
    print_separator();

    return 0;
}

void init() {
    test_mode.active = false;
    test_mode.exception_caught = false;
    test_mode.continue_address = 0;
    test_mode.test_line = 0;
    test_mode.test_file = nullptr;

    fault_out << "Exception handlers initialized.\n";
}

} // namespace exception
