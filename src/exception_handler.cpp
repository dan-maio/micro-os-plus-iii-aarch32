/**
 * @file    exception_handler.cpp
 * @brief   Exception Handler Implementation for ARM Cortex-A7
 */

#include "exception_handler.hpp"
#include "uart.hpp"
#include "mmu.hpp"
#include <cstdint>

namespace exception {

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
    uart::uart1 << "+--------------------------------------------------+\n";
}

void print_hex32(const char* label, std::uint32_t value) {
    uart::uart1 << "  " << label << ": 0x";
    uart::uart1.print_hex(value, 8);
    uart::uart1 << "\n";
}

extern "C" unsigned port_cpu_id(void);

void print_register_dump(const ExceptionContext* ctx, std::uint32_t fault_pc, std::uint32_t spsr) {
    uart::uart1 << "  Register Dump (Core ";
    uart::uart1.print_hex(port_cpu_id(), 1);
    uart::uart1 << "):\n";
    uart::uart1 << "    R0 =0x"; uart::uart1.print_hex(ctx->r0, 8);
    uart::uart1 << "  R1 =0x"; uart::uart1.print_hex(ctx->r1, 8);
    uart::uart1 << "  R2 =0x"; uart::uart1.print_hex(ctx->r2, 8);
    uart::uart1 << "  R3 =0x"; uart::uart1.print_hex(ctx->r3, 8);
    uart::uart1 << "\n";

    uart::uart1 << "    R4 =0x"; uart::uart1.print_hex(ctx->r4, 8);
    uart::uart1 << "  R5 =0x"; uart::uart1.print_hex(ctx->r5, 8);
    uart::uart1 << "  R6 =0x"; uart::uart1.print_hex(ctx->r6, 8);
    uart::uart1 << "  R7 =0x"; uart::uart1.print_hex(ctx->r7, 8);
    uart::uart1 << "\n";

    uart::uart1 << "    R8 =0x"; uart::uart1.print_hex(ctx->r8, 8);
    uart::uart1 << "  R9 =0x"; uart::uart1.print_hex(ctx->r9, 8);
    uart::uart1 << "  R10=0x"; uart::uart1.print_hex(ctx->r10, 8);
    uart::uart1 << "  R11=0x"; uart::uart1.print_hex(ctx->r11, 8);
    uart::uart1 << "\n";

    uart::uart1 << "    R12=0x"; uart::uart1.print_hex(ctx->r12, 8);
    uart::uart1 << "  PC =0x"; uart::uart1.print_hex(fault_pc, 8);
    uart::uart1 << "\n";

    uart::uart1 << "    SPSR=0x"; uart::uart1.print_hex(spsr, 8);
    uart::uart1 << " [";
    uart::uart1 << ((spsr & (1 << 31)) ? 'N' : 'n');
    uart::uart1 << ((spsr & (1 << 30)) ? 'Z' : 'z');
    uart::uart1 << ((spsr & (1 << 29)) ? 'C' : 'c');
    uart::uart1 << ((spsr & (1 << 28)) ? 'V' : 'v');
    uart::uart1 << "] Mode=";
    std::uint32_t mode = spsr & 0x1F;
    switch (mode) {
        case 0x10: uart::uart1 << "USR"; break;
        case 0x11: uart::uart1 << "FIQ"; break;
        case 0x12: uart::uart1 << "IRQ"; break;
        case 0x13: uart::uart1 << "SVC"; break;
        case 0x17: uart::uart1 << "ABT"; break;
        case 0x1B: uart::uart1 << "UND"; break;
        case 0x1F: uart::uart1 << "SYS"; break;
        default:   uart::uart1 << "???"; break;
    }
    uart::uart1 << " IRQ="; uart::uart1 << ((spsr & 0x80) ? "off" : "on");
    uart::uart1 << " FIQ="; uart::uart1 << ((spsr & 0x40) ? "off" : "on");
    uart::uart1 << "\n";
}

void print_instruction_at(std::uint32_t address) {
    volatile std::uint32_t* ptr = reinterpret_cast<volatile std::uint32_t*>(address & ~3);
    std::uint32_t instruction = *ptr;

    uart::uart1 << "  Instruction at 0x";
    uart::uart1.print_hex(address, 8);
    uart::uart1 << ": 0x";
    uart::uart1.print_hex(instruction, 8);
    uart::uart1 << "\n";
}

void print_test_location() {
    if (test_mode.test_file != nullptr) {
        uart::uart1 << "  Test Location: " << test_mode.test_file
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
    uart::uart1 << "| EXCEPTION: Undefined Instruction               |\n";
    print_separator();

    print_hex32("Faulting PC", fault_pc);
    print_instruction_at(fault_pc);

    print_hex32("CPACR", cpacr);
    uart::uart1 << "    CP10=" << cp10_access << " CP11=" << cp11_access;
    if (cp10_access == 3 && cp11_access == 3)
        uart::uart1 << " (full access)\n";
    else if (cp10_access == 0 && cp11_access == 0)
        uart::uart1 << " (NO ACCESS)\n";
    else
        uart::uart1 << " (restricted)\n";

    if (fpexc != 0xDEADDEAD) {
        print_hex32("FPEXC", fpexc);
        uart::uart1 << "    EN=" << ((fpexc >> 30) & 1)
                     << " EX=" << ((fpexc >> 31) & 1)
                     << " DEX=" << ((fpexc >> 29) & 1)
                     << ((fpexc & (1u << 30)) ? " (VFP enabled)" : " (VFP DISABLED!)")
                     << "\n";
        if (fpscr != 0xDEADDEAD) {
            print_hex32("FPSCR", fpscr);
            uart::uart1 << "    Traps:";
            if (fpscr & (1u << 8))  uart::uart1 << " IOE";
            if (fpscr & (1u << 9))  uart::uart1 << " DZE";
            if (fpscr & (1u << 10)) uart::uart1 << " OFE";
            if (fpscr & (1u << 11)) uart::uart1 << " UFE";
            if (fpscr & (1u << 12)) uart::uart1 << " IXE";
            if (fpscr & (1u << 15)) uart::uart1 << " IDE";
            if (!(fpscr & 0x9F00))  uart::uart1 << " none";
            uart::uart1 << "  Flags:";
            if (fpscr & (1u << 0))  uart::uart1 << " IOC";
            if (fpscr & (1u << 1))  uart::uart1 << " DZC";
            if (fpscr & (1u << 2))  uart::uart1 << " OFC";
            if (fpscr & (1u << 3))  uart::uart1 << " UFC";
            if (fpscr & (1u << 4))  uart::uart1 << " IXC";
            if (fpscr & (1u << 7))  uart::uart1 << " IDC";
            if (!(fpscr & 0x9F))    uart::uart1 << " none";
            uart::uart1 << "\n";
        }
    } else {
        uart::uart1 << "  FPEXC: unreadable (CP10/CP11 denied by CPACR)\n";
    }

    print_register_dump(ctx, fault_pc, spsr);

    if (test_mode.active) {
        print_test_location();
        uart::uart1 << "  [TEST MODE] Continuing execution...\n";
        print_separator();

        test_mode.exception_caught = true;
        test_mode.last_exception = ExceptionType::UndefinedInstruction;
        test_mode.fault_address = fault_pc;

        return test_mode.continue_address;
    }

    uart::uart1 << "  [FATAL] System halted.\n";
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
    uart::uart1 << "| EXCEPTION: Prefetch Abort                      |\n";
    print_separator();

    print_hex32("Faulting PC", fault_pc);
    print_hex32("IFSR", ifsr);
    print_hex32("IFAR", ifar);

    std::uint32_t fs = ((ifsr >> 10) & 1) << 4 | (ifsr & 0xF);
    uart::uart1 << "  Fault: " << fault_status_string(static_cast<FaultStatus>(fs)) << "\n";

    print_register_dump(ctx, fault_pc, spsr);

    if (test_mode.active) {
        print_test_location();
        uart::uart1 << "  [TEST MODE] Continuing execution...\n";
        print_separator();

        test_mode.exception_caught = true;
        test_mode.last_exception = ExceptionType::PrefetchAbort;
        test_mode.fault_address = ifar;

        return test_mode.continue_address;
    }

    uart::uart1 << "  [FATAL] System halted.\n";
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
    uart::uart1 << "| EXCEPTION: Data Abort                          |\n";
    print_separator();

    print_hex32("Faulting PC", fault_pc);
    print_hex32("DFSR", dfsr);
    print_hex32("DFAR", dfar);

    std::uint32_t fs = ((dfsr >> 10) & 1) << 4 | (dfsr & 0xF);
    uart::uart1 << "  Fault: " << fault_status_string(static_cast<FaultStatus>(fs)) << "\n";

    bool is_write = (dfsr >> 11) & 1;
    uart::uart1 << "  Access: " << (is_write ? "Write" : "Read") << "\n";

    print_instruction_at(fault_pc);
    print_register_dump(ctx, fault_pc, spsr);

    if (test_mode.active) {
        print_test_location();
        uart::uart1 << "  [TEST MODE] Continuing execution...\n";
        print_separator();

        test_mode.exception_caught = true;
        test_mode.last_exception = ExceptionType::DataAbort;
        test_mode.fault_address = dfar;

        return test_mode.continue_address;
    }

    uart::uart1 << "  [FATAL] System halted.\n";
    print_separator();

    return 0;
}

void init() {
    test_mode.active = false;
    test_mode.exception_caught = false;
    test_mode.continue_address = 0;
    test_mode.test_line = 0;
    test_mode.test_file = nullptr;

    uart::uart1 << "Exception handlers initialized.\n";
}

} // namespace exception
