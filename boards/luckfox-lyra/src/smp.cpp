#include "smp.hpp"
#include "rk3506.hpp"
#include "uart.hpp"

extern "C" {
volatile std::uint32_t g_core_stage[OS_NCPU] = {0};
volatile std::uint32_t g_core1_heartbeat = 0;
volatile std::uint32_t g_core2_heartbeat = 0;
}

namespace smp {

void start_secondary_cores()
{
    using namespace rk3506;

    uart::uart1 << "Core 0: setting up SRAM mailbox...\n";
    mailbox::TARGET::write(0x00200000u);
    mailbox::MAGIC::write(mailbox::MAGIC_VALUE);
    dsb();

    uart::uart1 << "Core 0: releasing core 1...\n";
    cru::SOFTRST_CON00::clear_bits(cru::softrst00::CORE1_RESETS);
    dsb();
    isb();
    sev();

    uart::uart1 << "Core 0: releasing core 2...\n";
    cru::SOFTRST_CON00::clear_bits(cru::softrst00::CORE2_RESETS);
    dsb();
    isb();
    sev();

    uart::uart1 << "Core 0: secondary-core startup signals sent.\n";
}

}
