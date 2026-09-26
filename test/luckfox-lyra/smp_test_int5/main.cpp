// smp_test_int5 — Cortex-M0 → A7 mailbox interrupts (AMP doorbell)
//
// The first test that brings the RK3506's Cortex-M0 "baseband" core into play
// and uses the hardware Mailbox peripheral as a cross-processor interrupt
// source. The M0 fires a mailbox doorbell every ~40 ms to A7 cores 1 and 2;
// each A7 core, in its mailbox ISR, reports the event to core 0 over a shared
// ring; core 0 owns the console and the LED.
//
//   Cortex-M0  ──TIMER0 ~40ms──▶ Mailbox CH1 ──SPI──▶ A7 core 1 ISR ─┐
//                              └▶ Mailbox CH2 ──SPI──▶ A7 core 2 ISR ─┤
//                                                                     ▼
//   A7 core 0: logger thread — "led on"/"led off" every 500 ms         report ring
//              + drains the ring: "core N received int from Cortex-M0 (#k)"
//
// Two provenances for the doorbell, selected at build time:
//   * default            — the real M0 firmware (m0/) drives the mailboxes.
//   * -DSMP_TEST_INT5_SELFTRIG — a core-0 thread writes the B2A DATA registers
//                          itself every 40 ms, exercising the ENTIRE A7-side
//                          path (discovery, routing to cores 1/2, cross-core
//                          reporting, logging) WITHOUT depending on the M0
//                          booting. This is the bring-up/validation stage.
//
// Discovery caution (lesson D3/D5): the reference doc claims "SPI 170 -> GIC
// IRQ 202 (170+32)", but on this SoC a Rockchip "SPI n" is the GIC ID n
// directly (UART1's "SPI 67" is GIC ID 67, not 99). So the mailbox GIC IDs are
// NOT hardcoded — each is discovered at runtime by a controlled A7 write that
// latches the channel's GICD_ISPENDR bit, then routed with set_target().

#include <cmsis-plus/rtos/os.h>
#include <cmsis-plus/diag/trace.h>
#include <rk3506.hpp>
#include <uart.hpp>
#include <gic.hpp>
#include <exception_handler.hpp>
#include <smp.hpp>

extern "C" unsigned port_cpu_id(void);

// ---------------------------------------------------------------------------
// RK3506 Mailbox peripheral (BB<->AP). Four channels, 0x1000 apart.
// B2A = Baseband(M0)-to-AP(A7): writing B2A_DATA raises that channel's AP IRQ.
// ---------------------------------------------------------------------------
namespace mbox {
  inline constexpr std::uintptr_t BASE   = 0xFF290000u;
  inline constexpr std::uintptr_t STRIDE = 0x1000u;
  inline constexpr std::uintptr_t B2A_INTEN  = 0x10;
  inline constexpr std::uintptr_t B2A_STATUS = 0x14;   // W1C
  inline constexpr std::uintptr_t B2A_CMD    = 0x18;
  inline constexpr std::uintptr_t B2A_DATA   = 0x1C;   // write triggers the IRQ

  static inline volatile uint32_t& reg(uint32_t ch, uintptr_t off)
  { return *reinterpret_cast<volatile uint32_t*>(BASE + ch * STRIDE + off); }

  // Enable this channel's B2A "tx-done" interrupt to the AP (normal CMD+DATA
  // mode). Bit 8 = trig mode, bit 0 = tx-done inten.
  static inline void enable(uint32_t ch) { reg(ch, B2A_INTEN) = (0x0101u << 16) | 0x0101u; }
  static inline void fire(uint32_t ch, uint32_t cmd, uint32_t data)
  { reg(ch, B2A_CMD) = cmd; reg(ch, B2A_DATA) = data; }   // DATA write raises IRQ
  static inline uint32_t status(uint32_t ch) { return reg(ch, B2A_STATUS); }
  static inline void clear(uint32_t ch)
  {
    uint32_t val = reg(ch, B2A_STATUS);
    reg(ch, B2A_STATUS) = val;
    reg(ch, B2A_STATUS) = (val << 16) | val;
    rk3506::dsb();
  }
}

// Channel → core assignment (user request: M0 interrupts cores 1 and 2)
static constexpr uint32_t CH_CORE1 = 1;   // mailbox channel 1 -> A7 core 1
static constexpr uint32_t CH_CORE2 = 2;   // mailbox channel 2 -> A7 core 2

// Discovered GIC IDs for the two channels (0 = not found yet)
static volatile uint32_t g_mbox_irq[3] = {0, 0, 0};   // index by mailbox channel

// ---------------------------------------------------------------------------
// Cortex-M0 boot. Sequence distilled from the RK3506 vendor SDK
// (u-boot mach-rockchip/rk3506/rk3506.c :: fit_standalone_release):
//
//   1. Copy the M0 firmware to System SRAM at physical 0xFFF84000. The SDK
//      comment: "map 0 to sram, enable TCM mode for sram; 0xfff84000 for
//      sram". System SRAM is Device-mapped on the A7 (uncached), so the copy
//      is visible to the M0 with no cache maintenance. It sits above the A7
//      secondary-boot handshake area (0xFFF80000) — reuse only AFTER cores
//      1/2 have joined.
//   2. Map SRAM to the M0's local 0x0 as TCM + set the MCU code-start address.
//      *** On RK3506 this is a SECURE operation done by ATF via the SIP call
//      sip_smc_mcu_config(BUSMCU_0, MCU_CODE_START_ADDR, 0xFFF84000). The
//      exact secure SGRF register is inside the closed ATF binary
//      (rkbin/RKTRUST), NOT in the readable SDK — this is the one open item
//      for the real-M0 path (see docs/SMP_TEST_INT5.md). Until it is known,
//      use the SELFTRIG build, which needs none of this. ***
//   3. Un-gate the M0 clocks (CRU_GATE_CON05 = 0x0C000000: bits 10 hclk_m0 +
//      11 swclktck_m0), program the M0 systime calibration (GRF_SOC_CON36),
//      and release the M0 via PMU (PMU_INT_MASK_CON = 0x00060004:
//      mcu_rst_dis_cfg=1, glb_int_mask_mcu=0). Note: the release is through
//      the PMU, not a CRU soft-reset.
// ---------------------------------------------------------------------------
extern "C" const unsigned char m0_fw_start[];
extern "C" const unsigned char m0_fw_end[];

static constexpr uintptr_t M0_SRAM_BASE = 0xFFF80000u;   // MCU code start (SRAM Base)
static constexpr uintptr_t SGRF         = 0xFF960000u;   // secure GRF (from TEE)

static void raw_write(uintptr_t addr, uint32_t val)
{ *reinterpret_cast<volatile uint32_t*>(addr) = val; }

static void m0_load_firmware()
{
  const uint32_t* src = reinterpret_cast<const uint32_t*>(m0_fw_start);
  volatile uint32_t* dst = reinterpret_cast<volatile uint32_t*>(M0_SRAM_BASE);
  uint32_t bytes = static_cast<uint32_t>(m0_fw_end - m0_fw_start);
  for (uint32_t i = 0; i < (bytes + 3) / 4; ++i) dst[i] = src[i];
  rk3506::dsb();

  // TCM map + MCU code-start. This is what the vendor does via the secure SIP
  // call sip_smc_mcu_config(BUSMCU_0, MCU_CODE_START_ADDR, 0xFFF80000); the
  // handler was reverse-engineered from the closed TEE binary
  // (rkbin/bin/rk35/rk3506_tee_v1.25.bin @ file 0x89b8). It writes SGRF:
  //   +0x0C masked control: TCM-select bits 3/4/5 then remap bits 6/7/8;
  //   +0x20 = code-start address; +0x24 = enable; +0x0C final.
  // The TEE reached SGRF via its MMIO mapper; this port is flat-mapped and
  // secure, so we write 0xFF960000 directly.
  raw_write(SGRF + 0x0C, 0x00080038u);   // tcm_sel bit3
  raw_write(SGRF + 0x0C, 0x00100038u);   // tcm_sel bit4
  raw_write(SGRF + 0x0C, 0x00200038u);   // tcm_sel bit5
  raw_write(SGRF + 0x0C, 0x004001C0u);   // remap bit6
  raw_write(SGRF + 0x0C, 0x008001C0u);   // remap bit7
  raw_write(SGRF + 0x0C, 0x010001C0u);   // remap bit8
  raw_write(SGRF + 0x20, 0xFFF80000u);   // MCU code-start address
  raw_write(SGRF + 0x24, 0x00000001u);   // enable
  raw_write(SGRF + 0x0C, 0x02000000u);   // final
  rk3506::dsb();
}

static void m0_activate()
{
  raw_write(0xFF9A0814u, 0x0C000000u);  // CRU_GATE_CON05: un-gate hclk_m0 + swclktck_m0
  raw_write(0xFF288090u, 0x0BCD3D80u);  // GRF_SOC_CON36: M0 systime calibration
  rk3506::dsb();
  raw_write(0xFF90000Cu, 0x00060004u);  // PMU_INT_MASK_CON: release the M0
  rk3506::dsb();
}

// ---------------------------------------------------------------------------
// Statistics + cross-core report ring (ISR on core 1/2 -> logger on core 0)
// ---------------------------------------------------------------------------
static volatile uint32_t g_int_from_core[3] = {0, 0, 0};   // counts per A7 core
static volatile uint32_t g_mbox_irq_core[3] = {0, 0, 0};   // which core serviced

struct Report { uint8_t core; uint32_t count; uint32_t cmd; uint32_t data; };
static constexpr uint32_t RQ_SIZE = 128;                   // power of two
static Report            g_rq[3][RQ_SIZE];                 // core-separated queues
static volatile uint32_t g_rq_head[3] = {0, 0, 0};         // ISR writes
static volatile uint32_t g_rq_tail[3] = {0, 0, 0};         // logger reads

static void report_push(uint8_t core, uint32_t count, uint32_t cmd, uint32_t data)
{
  uint32_t head = g_rq_head[core];
  uint32_t next = (head + 1) & (RQ_SIZE - 1);
  if (next == g_rq_tail[core]) return;                     // full: drop
  g_rq[core][head] = { core, count, cmd, data };
  g_rq_head[core] = next;
}

// ---------------------------------------------------------------------------
// LED (GPIO1_A0)
// ---------------------------------------------------------------------------
static void led_init()
{
  using namespace rk3506;
  cru::CLKGATE_CON22::clear_bits(0x0002);
  cru::CLKGATE_CON03::clear_bits(0x0300);
  cru::SOFTRST_CON22::clear_bits(0x0002);
  cru::SOFTRST_CON03::clear_bits(0x0300);
  gpio1_ioc::GPIO1A_IOMUX_SEL0::write(0x000F, 0x0000);
  gpio1::SWPORT_DDR_L::write(0x0001, 0x0001);
}
static void led_on()  { rk3506::gpio1::SWPORT_DR_L::write(0x0001, 0x0001); }
static void led_off() { rk3506::gpio1::SWPORT_DR_L::write(0x0001, 0x0000); }

// ---------------------------------------------------------------------------
// External IRQ handler — mailbox doorbells on cores 1 and 2.
// The GIC delivers each channel's SPI to its routed core; that core reads the
// mailbox payload, W1C-clears the status, bumps its counter, and reports to
// core 0 through the ring. (Cores 1/2 never touch the UART.)
// ---------------------------------------------------------------------------
extern "C" void port_external_irq_handler(uint32_t iar)
{
  uint32_t irq_id = iar & 0x3FF;
  unsigned cpu = port_cpu_id();

  for (uint32_t ch = 1; ch <= 2; ++ch)
    {
      if (g_mbox_irq[ch] != 0 && irq_id == g_mbox_irq[ch])
        {
          uint32_t cmd  = mbox::reg(ch, mbox::B2A_CMD);
          uint32_t data = mbox::reg(ch, mbox::B2A_DATA);
          mbox::clear(ch);                       // W1C so the next pulse latches
          g_mbox_irq_core[cpu] = g_mbox_irq_core[cpu] + 1;
          uint32_t n = g_int_from_core[cpu] + 1;
          g_int_from_core[cpu] = n;
          report_push(static_cast<uint8_t>(cpu), n, cmd, data);
          return;
        }
    }
}

// ---------------------------------------------------------------------------
// GIC pending-register discovery (lesson D5): a level IRQ sets its ISPENDR bit
// on assertion even while disabled at the distributor.
// ---------------------------------------------------------------------------
static constexpr uint32_t PEND_WORDS = 8;      // GIC IDs 0..255
static uint32_t g_pend_base[PEND_WORDS];

static void gic_pend_snapshot(uint32_t* dst)
{
  for (uint32_t i = 0; i < PEND_WORDS; ++i)
    dst[i] = *reinterpret_cast<volatile uint32_t*>(gic::gicd::ISPENDR_BASE + i * 4);
}
static uint32_t gic_find_new_spi(uint32_t exclude)
{
  for (uint32_t i = 1; i < PEND_WORDS; ++i)
    {
      uint32_t cur = *reinterpret_cast<volatile uint32_t*>(gic::gicd::ISPENDR_BASE + i * 4);
      uint32_t fresh = cur & ~g_pend_base[i];
      while (fresh)
        {
          uint32_t bit = static_cast<uint32_t>(__builtin_ctz(fresh));
          uint32_t id = i * 32 + bit;
          if (id != gic::irq::UART1 && id != exclude) return id;
          fresh &= fresh - 1;
        }
    }
  return 0;
}

// Discover one channel's GIC ID by a controlled A7 write that latches its
// pending bit (the channel's SPI is still disabled at the distributor).
static uint32_t mbox_discover(uint32_t ch, uint32_t exclude)
{
  mbox::enable(ch);
  mbox::clear(ch);
  gic_pend_snapshot(g_pend_base);
  mbox::fire(ch, 0xA5A50000u | ch, 0x0000BEEFu | (ch << 16));  // raises the SPI
  uint32_t found = 0;
  for (int i = 0; i < 200 && !found; ++i)
    { found = gic_find_new_spi(exclude); if (!found) for (int j=0;j<2000;++j) __asm__ volatile("nop"); }
  mbox::clear(ch);
  return found;
}

static void gic_clear_pending(uint32_t irq_id)
{
  uint32_t reg_index = irq_id / 32;
  uint32_t bit_offset = irq_id % 32;
  volatile auto* reg = reinterpret_cast<volatile uint32_t*>(0xFF581000u + 0x280 + reg_index * 4);
  *reg = (1U << bit_offset);
}


// ---------------------------------------------------------------------------
// Thread stacks
// ---------------------------------------------------------------------------
static os::rtos::thread::stack::element_t logger_stack[2048];
#if defined(SMP_TEST_INT5_SELFTRIG)
static os::rtos::thread::stack::element_t selftrig_stack[1024];
#endif

// ---------------------------------------------------------------------------
// Hardware init hooks
// ---------------------------------------------------------------------------
extern "C"
{
  void os_startup_initialize_hardware_early(void) { }

  extern char __heap_start[];
  extern char __heap_end[];
  extern char __fiq_stack_top[];
  extern char __irq_stack_top[];

  void os_startup_initialize_hardware(void)
  {
    uart::uart1.init();
    uart::uart1 << "\n\n";
    uart::uart1 << "+=============================================+\n";
    uart::uart1 << "|  Luckfox Lyra µOS++ SMP TEST INT5           |\n";
    uart::uart1 << "|  Cortex-M0 -> A7 mailbox doorbell (AMP)     |\n";
    uart::uart1 << "|  M0 pings cores 1 & 2; core 0 logs          |\n";
    uart::uart1 << "+=============================================+\n\n";
    os_startup_initialize_free_store(__heap_start, __heap_end - __heap_start);
    exception::init();
    led_init();
  }

  [[noreturn]] static void custom_main_trampoline(void)
  { int code = os_main(0, nullptr); std::exit(code); }

  extern os::rtos::thread* os_main_thread;

  extern "C" int main(void)
  {
    using namespace os::rtos;
#if defined(OS_HAS_INTERRUPTS_STACK)
    os::rtos::interrupts::stack()->set(
        reinterpret_cast<os::rtos::thread::stack::element_t*>(__fiq_stack_top),
        __irq_stack_top - __fiq_stack_top);
    os::rtos::interrupts::stack()->initialize();
#endif
    os::trace::printf("\nµOS++ IIIe version " OS_STRING_RTOS_IMPL_VERSION "\n");
    os::rtos::port::scheduler::greeting();
    scheduler::initialize();
    thread::attributes attr = thread::initializer;
    attr.th_stack_size_bytes = OS_INTEGER_RTOS_MAIN_STACK_SIZE_BYTES;
    os_main_thread = new thread("main",
                                reinterpret_cast<thread::func_t>(custom_main_trampoline),
                                nullptr, attr);
    os_startup_create_thread_idle();
    scheduler::start();
    return 0;
  }
}

// ---------------------------------------------------------------------------
// Core 0 logger thread: LED on/off every 500 ms + drains the report ring.
// Sole owner of the UART (cores 1/2 only enqueue reports).
// ---------------------------------------------------------------------------
static void* logger_thread_func(void*)
{
  using uart::uart1;
  bool on = false;
  unsigned tick = 0;
  for (;;)
    {
      // Drain reports quickly (poll ~every 20 ms so 40 ms doorbells surface).
      os::rtos::sysclock.sleep_for(20);
      for (uint32_t c = 1; c <= 2; ++c)
        {
          while (g_rq_tail[c] != g_rq_head[c])
            {
              Report r = g_rq[c][g_rq_tail[c]];
              g_rq_tail[c] = (g_rq_tail[c] + 1) & (RQ_SIZE - 1);
              uart1 << "core " << static_cast<int>(r.core)
                    << " received int from Cortex-M0  (#" << static_cast<int>(r.count)
                    << ", data=0x";
              uart1.print_hex(r.data, 8);
              uart1 << ")\n";
            }
        }


      if (++tick >= 25)               // 25 * 20 ms = 500 ms
        {
          tick = 0;
          on = !on;
          uint32_t m0_seq = *(volatile uint32_t *)0xFFF88000u;
          if (on) { led_on();  uart1 << "led on  | M0 seq="; }
          else    { led_off(); uart1 << "led off | M0 seq="; }
          uart1 << static_cast<int>(m0_seq) << "\n";
        }

    }
  return nullptr;
}

#if defined(SMP_TEST_INT5_SELFTRIG)
// Bring-up stand-in for the M0: fire both channels every ~40 ms from core 0.
static void* selftrig_thread_func(void*)
{
  uint32_t seq = 0;
  for (;;)
    {
      os::rtos::sysclock.sleep_for(40);
      mbox::fire(CH_CORE1, 0x0C010000u | (seq & 0xFFFF), 0x1D1D0000u | (seq & 0xFFFF));
      mbox::fire(CH_CORE2, 0x0C020000u | (seq & 0xFFFF), 0x2D2D0000u | (seq & 0xFFFF));
      seq++;
    }
  return nullptr;
}
#endif

// ---------------------------------------------------------------------------
// os_main — boot thread on Core 0
// ---------------------------------------------------------------------------
int os_main(int argc, char* argv[])
{
  using namespace os::rtos;
  using uart::uart1;

  __asm__ volatile("cpsie i" ::: "memory");
  uart1 << "IRQs enabled (timer only)\n\n";

  // Un-gate all peripheral clocks in the CRU to ensure TIMER0 functional clock is running
  for (uint32_t offset = 0x0800; offset <= 0x09E0; offset += 4) {
    *(volatile uint32_t *)(0xFF9A0000u + offset) = 0xFFFF0000u;
  }
  rk3506::dsb();

  // Un-gate all peripheral clocks in the PMUCRU
  for (uint32_t offset = 0x0100; offset <= 0x01E0; offset += 4) {
    *(volatile uint32_t *)(0xFF900000u + offset) = 0xFFFF0000u;
  }
  rk3506::dsb();

  // Release all peripheral soft resets in the CRU
  for (uint32_t offset = 0x0A00; offset <= 0x0BE0; offset += 4) {
    *(volatile uint32_t *)(0xFF9A0000u + offset) = 0xFFFF0000u;
  }
  rk3506::dsb();




  // 1. Release A7 cores 1 and 2
  smp_install_boot_threads();
  smp::start_secondary_cores();
  uart1 << "Waiting for cores 1 and 2 to join...\n";
  int waited = 0;
  while ((g_core_stage[1] < 3 || g_core_stage[2] < 3) && waited < 3000)
    { sysclock.sleep_for(50); waited += 50; }
  uart1 << "cores: c1=" << g_core_stage[1] << " c2=" << g_core_stage[2]
        << " (after " << waited << "ms)\n\n";

  // 2. Discover the two mailbox channels' GIC IDs (D5), route CH1->core1,
  //    CH2->core2, and enable them.
  uart1 << "Discovering mailbox CH1/CH2 GIC IDs (ISPENDR diff)...\n";
  uint32_t id1 = mbox_discover(CH_CORE1, 0);
  uint32_t id2 = mbox_discover(CH_CORE2, id1);
  uart1 << "  CH1 -> GIC ID " << static_cast<int>(id1)
        << "   CH2 -> GIC ID " << static_cast<int>(id2) << "\n";
  if (id1) { g_mbox_irq[CH_CORE1] = id1; rk3506::dsb();
             gic_clear_pending(id1);
             gic::gic.set_target(id1, 1u << 1); gic::gic.enable_irq(id1); }
  if (id2) { g_mbox_irq[CH_CORE2] = id2; rk3506::dsb();
             gic_clear_pending(id2);
             gic::gic.set_target(id2, 1u << 2); gic::gic.enable_irq(id2); }
  uart1 << "  routed: CH1->core1, CH2->core2\n\n";

#if defined(SMP_TEST_INT5_SELFTRIG)
  uart1 << "SELF-TRIGGER mode: core 0 fires the mailboxes (M0 not required)\n\n";
#else
  // 3. Load + boot the Cortex-M0. Safe to reuse the System SRAM now that
  //    cores 1/2 have joined (the boot handshake no longer needs it).
  uint32_t fwlen = static_cast<uint32_t>(m0_fw_end - m0_fw_start);
  uart1 << "Loading M0 firmware (" << static_cast<int>(fwlen)
        << " bytes) to 0xFFF80000 + TCM attach...\n";
  m0_load_firmware();
  uart1 << "Activating Cortex-M0 (un-gate hclk_m0, release reset)...\n";
  m0_activate();
  uart1 << "M0 released; expecting ~40 ms mailbox doorbells\n\n";
#endif

  // 4. Core-0 logger thread
  thread::attributes attr = thread::initializer;
  attr.th_stack_address = logger_stack; attr.th_stack_size_bytes = sizeof(logger_stack);
  static thread logger_thread{ "logger", logger_thread_func, nullptr, attr };
  logger_thread.cpu_affinity(1u << 0);

#if defined(SMP_TEST_INT5_SELFTRIG)
  attr.th_stack_address = selftrig_stack; attr.th_stack_size_bytes = sizeof(selftrig_stack);
  static thread selftrig_thread{ "selftrig", selftrig_thread_func, nullptr, attr };
  selftrig_thread.cpu_affinity(1u << 0);
#endif

  uart1 << "Logger running on core 0. Cores 1/2 idle, waiting on mailbox IRQs.\n";
  uart1 << "---------------------------------------------------------------\n\n";

  // Run a bounded number of beats, then report and exit. The runner needs a
  // RESULT line and a semihosting stop, so the test must not sleep forever.
  for (unsigned it = 1; it <= 20; ++it)
    {
      sysclock.sleep_for(1000);
      uart1 << "beat " << static_cast<int>(it) << "/20\n";
    }

  // uart1 << putc() also writes the semihosting channel, so this reaches both
  // the terminal and the OpenOCD log the runner greps; the return goes through
  // the port's strong _Exit() (semihosting SYS_EXIT).
  uart1 << "\nRESULT: PASS\n";
  return 0;
}

// ---------------------------------------------------------------------------
// SMP idle threads for secondary cores (their mailbox ISR does the real work)
// ---------------------------------------------------------------------------
extern "C" { extern os::rtos::thread* os_idle_thread; }
namespace os { namespace rtos { namespace scheduler {
extern thread* os_idle_thread_core[3];
}}}

static os::rtos::thread::stack::element_t idle1_stack[1024];
static os::rtos::thread::stack::element_t idle2_stack[1024];
static os::rtos::thread* idle1_thread_ptr;
static os::rtos::thread* idle2_thread_ptr;

static void* secondary_idle_func(void*)
{
  os::rtos::this_thread::thread().priority(os::rtos::thread::priority::idle);
  while (true)
    {
      __asm__ volatile("dsb" ::: "memory");
      __asm__ volatile("wfi");
      os::rtos::this_thread::yield();
    }
  return nullptr;
}

extern "C" void smp_install_boot_threads(void)
{
  using namespace os::rtos;
  thread::attributes a1 = thread::initializer;
  a1.th_stack_address = idle1_stack; a1.th_stack_size_bytes = sizeof(idle1_stack);
  idle1_thread_ptr = new thread("idle1", secondary_idle_func, nullptr, a1);
  os::rtos::scheduler::os_idle_thread_core[1] = idle1_thread_ptr;

  thread::attributes a2 = thread::initializer;
  a2.th_stack_address = idle2_stack; a2.th_stack_size_bytes = sizeof(idle2_stack);
  idle2_thread_ptr = new thread("idle2", secondary_idle_func, nullptr, a2);
  os::rtos::scheduler::os_idle_thread_core[2] = idle2_thread_ptr;
}
