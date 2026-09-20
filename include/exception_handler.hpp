/**
 * @file exception_handler.hpp
 * @brief Exception Handler for ARM Cortex-A7 (Luckfox Lyra)
 */

#pragma once

#include <cstdint>

namespace exception {

enum class FaultStatus : std::uint32_t {
    AlignmentFault          = 0b00001,
    DebugEvent              = 0b00010,
    AccessFlagSection       = 0b00011,
    IcacheMaintenance       = 0b00100,
    TranslationSection      = 0b00101,
    AccessFlagPage          = 0b00110,
    TranslationPage         = 0b00111,
    SyncExternalAbort       = 0b01000,
    DomainSection           = 0b01001,
    DomainPage              = 0b01011,
    TranslationTableWalk1   = 0b01100,
    PermissionSection       = 0b01101,
    TranslationTableWalk2   = 0b01110,
    PermissionPage          = 0b01111,
    AsyncExternalAbort      = 0b10110,
    AsyncParityError        = 0b11000,
    SyncParityError         = 0b11001,
    Unknown                 = 0b11111,
};

struct ExceptionContext {
    std::uint32_t r0;
    std::uint32_t r1;
    std::uint32_t r2;
    std::uint32_t r3;
    std::uint32_t r4;
    std::uint32_t r5;
    std::uint32_t r6;
    std::uint32_t r7;
    std::uint32_t r8;
    std::uint32_t r9;
    std::uint32_t r10;
    std::uint32_t r11;
    std::uint32_t r12;
    std::uint32_t saved_lr;
};

enum class ExceptionType : std::uint32_t {
    UndefinedInstruction = 0,
    PrefetchAbort        = 1,
    DataAbort            = 2,
};

struct FaultInfo {
    ExceptionType type;
    std::uint32_t fault_address;
    std::uint32_t fault_status;
    std::uint32_t instruction;
    std::uint32_t domain;
    bool          is_write;
    bool          is_external;
};

struct TestMode {
    volatile bool          active;
    volatile bool          exception_caught;
    volatile std::uint32_t continue_address;
    volatile ExceptionType last_exception;
    volatile std::uint32_t fault_address;
    volatile std::uint32_t test_line;
    const char* volatile   test_file;
};

extern TestMode test_mode;

void init();
bool run_tests();
const char* fault_status_string(FaultStatus status);
const char* exception_type_string(ExceptionType type);

#define EXCEPTION_TEST_BEGIN(continue_addr) \
    do { \
        exception::test_mode.active = true; \
        exception::test_mode.exception_caught = false; \
        exception::test_mode.continue_address = reinterpret_cast<std::uint32_t>(continue_addr); \
        exception::test_mode.test_line = __LINE__; \
        exception::test_mode.test_file = __FILE__; \
        __asm__ volatile("dsb sy" ::: "memory"); \
    } while(0)

#define EXCEPTION_TEST_END() \
    do { \
        exception::test_mode.active = false; \
        __asm__ volatile("dsb sy" ::: "memory"); \
    } while(0)

#define EXCEPTION_WAS_CAUGHT() (exception::test_mode.exception_caught)

extern "C" {
std::uint32_t handle_undefined_instruction(ExceptionContext* ctx, std::uint32_t fault_pc);
std::uint32_t handle_prefetch_abort(ExceptionContext* ctx, std::uint32_t fault_pc,
                                     std::uint32_t ifsr, std::uint32_t ifar);
std::uint32_t handle_data_abort(ExceptionContext* ctx, std::uint32_t fault_pc,
                                 std::uint32_t dfsr, std::uint32_t dfar);
}

} // namespace exception
