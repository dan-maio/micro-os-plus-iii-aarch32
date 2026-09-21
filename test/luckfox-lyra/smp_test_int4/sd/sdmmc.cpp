/**
 * @file sdmmc.cpp  (smp_test_int4 interrupt-driven copy)
 * @brief RK3506 DW-MMC SD-card driver — test-local copy of src/sdcard/sdmmc.cpp
 *        with an interrupt-driven WRITE data-transfer-over completion path.
 *
 * Distilled from U-Boot drivers/mmc/dw_mmc.c (host) and mmc.c (SD state
 * machine). Bring-up and reads remain PIO/polled (they run before the
 * scheduler and GIC routing exist). When set_irq_mode(true) is armed, the
 * WRITE path fills the FIFO and then blocks on sd_completion_wait() until the
 * DW-MMC DTO interrupt (serviced by sd_irq_service() on the routed core)
 * releases it — the multi-millisecond card-programming wait no longer spins a
 * whole core. The shared src/sdcard/sdmmc.cpp is left untouched (project
 * policy); this file is compiled in its place for smp_test_int4.
 */

#include "sdmmc.hpp"

namespace sdcard {

// ---------------------------------------------------------------------------
// Interrupt-completion plumbing.
// ---------------------------------------------------------------------------
namespace {
volatile std::uint32_t g_sd_isr_status = 0;   ///< RINTSTS captured by the ISR
}

// Weak no-op defaults — the application overrides these with semaphore ops.
// The weak wait falls back to a bounded spin so the driver still works if the
// app forgets to wire them (defensive, never hangs the board).
extern "C" __attribute__((weak)) void sd_completion_post(void) {}
extern "C" __attribute__((weak)) void sd_completion_reset(void) { g_sd_isr_status = 0; }
extern "C" __attribute__((weak)) void sd_completion_wait(void)
{
    for (std::uint32_t i = 0; i < 3'000'000u; ++i)
        if (g_sd_isr_status) break;
}

extern "C" std::uint32_t sd_irq_last_status(void) { return g_sd_isr_status; }

extern "C" void sd_irq_service(void)
{
    volatile std::uint32_t& rints =
        *reinterpret_cast<volatile std::uint32_t*>(kSdmmcBase + RINTSTS);
    std::uint32_t m = rints;
    constexpr std::uint32_t kDone = INT_DTO | INT_DCRC | INT_DRTO;
    if (m & kDone) {
        g_sd_isr_status = m;
        rints = kDone;                 // write-1-clear the bits we consumed
        sd_completion_post();
    } else {
        // Not a completion we own (e.g. a stray TXDR/RXDR): clear it so the
        // GIC line deasserts and we don't storm.
        rints = m & ~kDone;
    }
}

namespace {

// Rough busy-loop timeout; the exact count is not critical because every wait
// also has a hard bail-out.  Kept modest so a stuck poll fails fast (and prints
// diagnostics) instead of looking like a multi-second hang.
inline constexpr std::uint32_t kSpin = 300'000;

inline void nop_delay(std::uint32_t n) noexcept {
    for (std::uint32_t i = 0; i < n; ++i) __asm__ volatile("nop");
}

// Masked-write helper for Rockchip CRU/GRF registers (upper 16 = write mask).
inline void masked_write(std::uintptr_t addr, std::uint16_t mask, std::uint16_t val) noexcept {
    *reinterpret_cast<volatile std::uint32_t*>(addr) =
        (static_cast<std::uint32_t>(mask) << 16) | (val & mask);
}

}  // namespace

// ============================================================================
// Clock + pinmux bring-up (mirrors sdcard.md::sdmmc_hardware_init)
// ============================================================================

void hardware_init() noexcept {
    constexpr std::uintptr_t CRU_BASE  = 0xFF9A0000U;
    constexpr std::uintptr_t CLKSEL49  = CRU_BASE + 0x03C4;
    constexpr std::uintptr_t GATE17    = CRU_BASE + 0x0844;
    constexpr std::uintptr_t SOFTRST17 = CRU_BASE + 0x0A44;

    constexpr std::uintptr_t GPIO3_IOC = 0xFF4D8000U;
    constexpr std::uintptr_t IOMUX_A_0 = GPIO3_IOC + 0x0060;  // A0..A3
    constexpr std::uintptr_t IOMUX_A_1 = GPIO3_IOC + 0x0064;  // A4..A7

    // A. Un-gate hclk_sdmmc (bit7) and cclk_src_sdmmc (bit6): write 0 to enable.
    masked_write(GATE17, 0x00C0, 0x0000);

    // B. cclk_src_sdmmc source select + divider (CRU_CLKSEL_CON49).
    //    sel  = bits[14:13], 0 = 24 MHz OSC (CCLK_SDMMC_SEL_24M per RK3506 CRU)
    //    div  = bits[12:7],  field = divider-1
    //    We pick the 24 MHz oscillator with divider 1 -> cclk_src_sdmmc = 24 MHz.
    //    This is exactly what U-Boot does for the ID phase (24 MHz % 400 kHz == 0)
    //    and makes kBusHz a *known* 24 MHz so the DesignWare CLKDIV yields a true
    //    400 kHz init clock. GPLL was avoided because its rate here is unknown,
    //    which made the real ID clock several MHz too fast and the card silent.
    masked_write(CLKSEL49, 0x7F80, 0x0000);

    // C. Pulse hresetn_sdmmc (bit7).
    masked_write(SOFTRST17, 0x0080, 0x0080);
    nop_delay(1000);
    masked_write(SOFTRST17, 0x0080, 0x0000);

    // D. Route GPIO3_A0..A5 to func1 (SDMMC CLK/CMD/D0..D3).
    masked_write(IOMUX_A_0, 0xFFFF, 0x1111);  // A0..A3
    masked_write(IOMUX_A_1, 0x00FF, 0x0011);  // A4,A5

    // E. Configure pull-ups on CMD and D0..D3, pull-none on CLK (GPIO3 Pull reg at 0xFF4D8230).
    masked_write(GPIO3_IOC + 0x0230, 0x0FFF, 0x0554);
}

// ============================================================================
// Low-level host helpers
// ============================================================================

bool SdMmc::ctrl_reset(std::uint32_t mask) noexcept {
    wr(CTRL, mask);
    for (std::uint32_t i = 0; i < kSpin; ++i)
        if ((rd(CTRL) & mask) == 0) return true;
    return false;
}

// Program the card clock to <= hz (DesignWare: fcard = kBusHz / (2*div)).
bool SdMmc::set_clock(std::uint32_t hz) noexcept {
    // Disable clock, then update.
    wr(CLKENA, 0);
    // Issue "update clock registers only" command. Note: NO CMD_PRV_DAT_WAIT —
    // that would stall the update until DAT0 is non-busy, but on this board DAT0
    // reads busy after card selection, which would hang the transfer-clock
    // switch. A clock change between operations has no data pending to wait for.
    auto upd = [&]() -> bool {
        wr(CMD, CMD_START | CMD_UPD_CLK);
        for (std::uint32_t i = 0; i < kSpin; ++i)
            if ((rd(CMD) & CMD_START) == 0) return true;
        return false;
    };
    if (!upd()) return false;

    std::uint32_t div = (hz == 0) ? 0 : ((kBusHz + (2 * hz) - 1) / (2 * hz));
    wr(CLKDIV, div);
    wr(CLKSRC, 0);
    if (!upd()) return false;

    wr(CLKENA, 1 /*enable*/ );  // low-power mode off so 4-bit data works
    if (!upd()) return false;
    return true;
}

// Send one command and (for R2) collect the 4 response words in U-Boot's order
// (resp_out[0]=RESP3 .. resp_out[3]=RESP0) so CSD parsing matches mmc.c.
bool SdMmc::command(std::uint32_t idx, std::uint32_t arg, Resp resp,
                    std::uint32_t extra_flags, std::uint32_t* resp_out) noexcept {
    // Wait until the previous data transfer (if any) is no longer busy.
    for (std::uint32_t i = 0; i < kSpin; ++i)
        if ((rd(STATUS) & STAT_BUSY) == 0) break;

    wr(RINTSTS, INT_ALL);  // clear stale interrupts

    // USE_HOLD_REG routes cmd/response through a phase-delayed hold register; on
    // Rockchip that phase is only valid once the DLL is configured. Toggleable
    // via use_hold_reg_ so diag_scan() can try both.
    std::uint32_t flags = idx | CMD_START | extra_flags;
    if (use_hold_reg_) flags |= CMD_USE_HOLD_REG;
    switch (resp) {
        case Resp::None: break;
        case Resp::R2:   flags |= CMD_RESP_EXP | CMD_RESP_LONG | CMD_CHECK_CRC; break;
        case Resp::R3:   flags |= CMD_RESP_EXP; break;             // OCR: no CRC
        case Resp::R1b:  flags |= CMD_RESP_EXP | CMD_CHECK_CRC | CMD_PRV_DAT_WAIT; break;
        default:         flags |= CMD_RESP_EXP | CMD_CHECK_CRC; break;  // R1/R6/R7
    }

    // R3 (OCR) latches at a different sample phase than CRC responses on this
    // controller, so pick the phase per response type when enabled.
    if (phase_per_resp_)
        set_sample_phase(resp == Resp::R3 ? kSamplePhaseR3 : kSamplePhase);

    wr(CMDARG, arg);
    wr(CMD, flags);

    // Wait for command-done.
    std::uint32_t mask = 0;
    bool done = false;
    for (std::uint32_t i = 0; i < kSpin; ++i) {
        mask = rd(RINTSTS);
        if (mask & INT_CDONE) { done = true; break; }
    }
    last_rints_ = mask;
    if (resp != Resp::None) last_resp_ = rd(RESP0);  // capture even on error, for debug
    if (!done) return false;
    // A response timeout is always fatal. But R3 (OCR, e.g. ACMD41) carries no
    // valid CRC and a reserved '111111' command-index field, so the controller
    // *always* raises RE (response/index error) and RCRC on it — those must be
    // ignored for CRC-less responses. Only enforce RE/RCRC when a CRC is expected.
    if (mask & INT_RTO) { wr(RINTSTS, INT_ALL); return false; }
    // Only a CRC error (RCRC) is fatal for CRC-checked responses. RE (response
    // framing/index) is *not* enforced: the response data + CRC can come back
    // valid while RE flags a not-quite-centred start bit. Data integrity is
    // verified by the block read/write test.
    const bool crc_checked = (resp != Resp::None) && (resp != Resp::R3) && !tolerant_cmd_;
    if (crc_checked && (mask & INT_RCRC)) { wr(RINTSTS, INT_ALL); return false; }

    if (resp_out && resp != Resp::None) {
        if (resp == Resp::R2) {
            resp_out[0] = rd(RESP3);
            resp_out[1] = rd(RESP2);
            resp_out[2] = rd(RESP1);
            resp_out[3] = rd(RESP0);
        } else {
            resp_out[0] = rd(RESP0);
        }
    }
    wr(RINTSTS, INT_CDONE);
    return true;
}

// CMD55 + ACMD<idx>.  rca_ carries the card address for the APP_CMD.
bool SdMmc::app_command(std::uint32_t idx, std::uint32_t arg, Resp resp,
                        std::uint32_t* resp_out) noexcept {
    std::uint32_t r1 = 0;
    bool ok = command(APP_CMD, rca_, Resp::R1, 0, &r1);
    cmd55_ok_ = ok;
    cmd55_rints_ = last_rints_;
    cmd55_resp_ = last_resp_;
    // We do NOT require reading CMD55's response cleanly: the card only needs to
    // *receive* it to enter app-command mode. At the R3 sample phase used for the
    // ACMD41 block, CMD55's own R1 may be flagged; proceed as long as the card
    // did respond (not a timeout — RTO would mean the card never saw it).
    if (!ok && (last_rints_ & INT_RTO)) return false;
    return command(idx, arg, resp, 0, resp_out);
}

void SdMmc::set_sample_phase(std::uint32_t nineties) noexcept {
    std::uint32_t raw = nineties & 0x3;
    wr(0x134 /*SDMMC_TIMING_CON1*/, (raw << 1) | (0x7ffu << 17));
}

void SdMmc::set_drive_phase(std::uint32_t nineties) noexcept {
    std::uint32_t raw = nineties & 0x3;
    wr(0x130 /*SDMMC_TIMING_CON0*/, (raw << 1) | (0x7ffu << 17));
}

// Sweep the command drive-phase, checking CMD55 status for COM_CRC_ERROR.
int SdMmc::diag_scan_drive(ScanResult* out, int max) noexcept {
    hardware_init();
    ctrl_reset(CTRL_RESET_ALL);
    wr(PWREN, 1);
    wr(INTMASK, 0);
    wr(RINTSTS, INT_ALL);
    wr(TMOUT, 0xFFFFFFFF);
    wr(FIFOTH, (0x6u << 28) | (0x7Fu << 16) | 0x80u);
    wr(CTYPE, CTYPE_1BIT);
    wr(BLKSIZ, kBlockSize);
    set_clock(kInitHz);
    set_sample_phase(kSamplePhase);   // known-good response sampling
    use_hold_reg_ = true;
    nop_delay(10000);
    rca_ = 0;

    int n = 0;
    for (std::uint32_t drive = 0; drive < 4; ++drive) {
        if (n >= max) return n;
        set_drive_phase(drive);

        ctrl_reset(CTRL_FIFO_RESET);
        wr(RINTSTS, INT_ALL);
        command(GO_IDLE_STATE, 0, Resp::None, CMD_INIT, nullptr);
        nop_delay(10000);
        std::uint32_t r7 = 0;
        command(SEND_IF_COND, 0x1AA, Resp::R7, 0, &r7);
        nop_delay(2000);
        std::uint32_t st = 0;
        command(APP_CMD, 0, Resp::R1, 0, &st);   // CMD55; status in st

        out[n].sample_nineties = static_cast<std::uint8_t>(drive);  // = drive phase here
        out[n].hold = true;
        out[n].rints = last_rints_;
        out[n].resp = st;
        // Good drive = card accepted CMD55 (APP_CMD bit5) with no COM_CRC_ERROR (bit23).
        out[n].clean = ((st & 0x00800000u) == 0) && ((st & 0x20u) != 0);
        ++n;
    }
    return n;
}

// Diagnostic scan: probe CMD8 across sample-phase x hold-reg to find a config
// where the response samples cleanly.
int SdMmc::diag_scan(ScanResult* out, int max) noexcept {
    hardware_init();
    ctrl_reset(CTRL_RESET_ALL);
    wr(PWREN, 1);
    wr(INTMASK, 0);
    wr(RINTSTS, INT_ALL);
    wr(TMOUT, 0xFFFFFFFF);
    wr(FIFOTH, (0x6u << 28) | (0x7Fu << 16) | 0x80u);
    wr(CTYPE, CTYPE_1BIT);
    wr(BLKSIZ, kBlockSize);
    set_clock(kInitHz);
    nop_delay(10000);

    int n = 0;
    for (std::uint32_t phase = 0; phase < 4; ++phase) {
        for (int h = 0; h < 2; ++h) {
            if (n >= max) return n;
            set_sample_phase(phase);
            use_hold_reg_ = (h != 0);

            ctrl_reset(CTRL_FIFO_RESET);
            wr(RINTSTS, INT_ALL);
            command(GO_IDLE_STATE, 0, Resp::None, CMD_INIT, nullptr);
            nop_delay(10000);
            std::uint32_t r = 0;
            command(SEND_IF_COND, 0x1AA, Resp::R7, 0, &r);

            out[n].sample_nineties = static_cast<std::uint8_t>(phase);
            out[n].hold = use_hold_reg_;
            out[n].rints = last_rints_;
            out[n].resp = last_resp_;
            out[n].clean = ((last_rints_ & (INT_RE | INT_RCRC | INT_RTO)) == 0) &&
                           ((last_resp_ & 0xFF) == 0xAA);
            ++n;
        }
    }
    return n;
}

// Sweep sample phase, running the full ACMD41 handshake, recording the OCR.
int SdMmc::diag_scan_acmd41(ScanResult* out, int max) noexcept {
    hardware_init();
    ctrl_reset(CTRL_RESET_ALL);
    wr(PWREN, 1);
    wr(INTMASK, 0);
    wr(RINTSTS, INT_ALL);
    wr(TMOUT, 0xFFFFFFFF);
    wr(FIFOTH, (0x6u << 28) | (0x7Fu << 16) | 0x80u);
    wr(CTYPE, CTYPE_1BIT);
    wr(BLKSIZ, kBlockSize);
    set_clock(kInitHz);
    use_hold_reg_ = true;
    nop_delay(10000);
    rca_ = 0;

    int n = 0;
    for (std::uint32_t phase = 0; phase < 4; ++phase) {
        if (n >= max) return n;
        set_sample_phase(phase);

        ctrl_reset(CTRL_FIFO_RESET);
        wr(RINTSTS, INT_ALL);
        command(GO_IDLE_STATE, 0, Resp::None, CMD_INIT, nullptr);
        nop_delay(10000);
        std::uint32_t r7 = 0;
        command(SEND_IF_COND, 0x1AA, Resp::R7, 0, &r7);
        bool v2 = (r7 & 0xFF) == 0xAA;
        std::uint32_t arg = 0x00FF8000u | (v2 ? OCR_HCS : 0);

        // A few ACMD41 iterations to give the card time to answer.
        std::uint32_t ocr = 0;
        for (int t = 0; t < 20; ++t) {
            std::uint32_t st = 0;
            command(APP_CMD, 0, Resp::R1, 0, &st);
            command(SEND_OP_COND, arg, Resp::R3, 0, &ocr);
            if (ocr & OCR_BUSY) break;
            nop_delay(20000);
        }

        out[n].sample_nineties = static_cast<std::uint8_t>(phase);
        out[n].hold = true;
        out[n].rints = last_rints_;
        out[n].resp = ocr;
        out[n].clean = (ocr & 0x00FF8000u) != 0;   // OCR carries a voltage window
        ++n;
    }
    return n;
}

// ============================================================================
// Card identification
// ============================================================================

bool SdMmc::init() noexcept {
    present_ = high_capacity_ = false;
    rca_ = 0;
    capacity_bytes_ = 0;
    // Drive commands through the hold register so the drive phase in TIMING_CON0
    // is applied to command/data output (without it the card reported
    // COM_CRC_ERROR — the command TX was mis-timed).
    use_hold_reg_ = true;
    // With proper pull-ups a single sample phase works for every response, so we
    // do not switch phase per response type. (The flag remains only so the
    // diagnostic scans can drive the phase manually.)
    phase_per_resp_ = false;

    hardware_init();

    // Full controller reset + FIFO/DMA reset.
    step_ = Step::CtrlReset;
    if (!ctrl_reset(CTRL_RESET_ALL)) return false;
    wr(PWREN, 1);
    wr(INTMASK, 0);        // we poll RINTSTS directly
    wr(RINTSTS, INT_ALL);
    wr(TMOUT, 0xFFFFFFFF); // max data/response timeout
    // FIFO threshold: MSIZE=6, RX watermark = half, TX watermark = half.
    wr(FIFOTH, (0x6u << 28) | (0x7Fu << 16) | 0x80u);
    wr(CTYPE, CTYPE_1BIT);
    wr(BLKSIZ, kBlockSize);

    step_ = Step::ClockInit;
    if (!set_clock(kInitHz)) return false;
    set_sample_phase(kSamplePhase);  // 0deg: samples all responses + read data
    set_drive_phase(kDrivePhase);    // 90deg: output hold margin -> clean DATA writes
    nop_delay(10000);

    // CMD0: go idle (send 80 init clocks the first time).
    step_ = Step::Cmd0;
    if (!command(GO_IDLE_STATE, 0, Resp::None, CMD_INIT, nullptr)) return false;
    nop_delay(10000);

    // CMD8: voltage check.  0x1AA = 2.7-3.6V + check pattern 0xAA.
    step_ = Step::Cmd8;
    std::uint32_t r = 0;
    bool sd_v2 = command(SEND_IF_COND, 0x1AA, Resp::R7, 0, &r) && ((r & 0xFF) == 0xAA);
    last_resp_ = r;

    // ACMD41: negotiate OCR until busy bit clears.  Set HCS if SD v2.
    step_ = Step::Acmd41;
    set_sample_phase(kSamplePhaseR3);              // same 0deg phase as the rest
    std::uint32_t ocr_arg = 0x00FF8000u | (sd_v2 ? OCR_HCS : 0);
    std::uint32_t ocr = 0;
    bool ready = false;
    for (std::uint32_t tries = 0; tries < 200; ++tries) {
        if (!app_command(SEND_OP_COND, ocr_arg, Resp::R3, &ocr)) return false;
        last_resp_ = ocr;
        if (ocr & OCR_BUSY) { ready = true; break; }
        nop_delay(40000);   // ~a few hundred us/iter -> tens of ms total budget
    }
    set_sample_phase(kSamplePhase);                // restore default sample phase
    if (!ready) return false;
    high_capacity_ = (ocr & OCR_HCS) != 0;

    // CMD2: all-send-CID (R2).
    step_ = Step::Cmd2;
    std::uint32_t cid[4];
    if (!command(ALL_SEND_CID, 0, Resp::R2, 0, cid)) return false;

    // CMD3: SD publishes its RCA in bits [31:16] of R6.
    step_ = Step::Cmd3;
    if (!command(SEND_RELATIVE_ADDR, 0, Resp::R6, 0, &r)) return false;
    rca_ = r & 0xFFFF0000u;  // stored pre-shifted for use as an argument

    // CMD9: read CSD (R2) and derive capacity — parsing per mmc.c.
    step_ = Step::Cmd9;
    std::uint32_t csd[4];
    if (!command(SEND_CSD, rca_, Resp::R2, 0, csd)) return false;
    std::uint32_t read_bl_len = 1u << ((csd[1] >> 16) & 0xF);
    std::uint64_t csize, cmult;
    if (high_capacity_) {
        csize = ((csd[1] & 0x3F) << 16) | ((csd[2] & 0xFFFF0000u) >> 16);
        cmult = 8;
    } else {
        csize = ((csd[1] & 0x3FF) << 2) | ((csd[2] & 0xC0000000u) >> 30);
        cmult = (csd[2] & 0x00038000u) >> 15;
    }
    capacity_bytes_ = ((csize + 1) << (cmult + 2)) * read_bl_len;

    // CMD7: select the card -> transfer state.
    step_ = Step::Cmd7;
    if (!command(SELECT_CARD, rca_, Resp::R1b, 0, &r)) return false;

    // ACMD6: switch to 4-bit bus width, then tell the host.
    step_ = Step::Acmd6;
    if (!app_command(SET_BUS_WIDTH, 0x2 /*4-bit*/, Resp::R1, &r)) return false;
    wr(CTYPE, CTYPE_4BIT);

    // CMD16: force 512-byte block length (harmless on SDHC, required on SDSC).
    step_ = Step::Cmd16;
    if (!command(SET_BLOCKLEN, kBlockSize, Resp::R1, 0, &r)) return false;

    // Bump to the default-speed transfer clock (kXferHz = 12 MHz from the
    // 24 MHz OSC via CLKDIV=1). The old caution here — that a post-selection
    // clock switch hangs — predated two fixes: set_clock() now omits
    // CMD_PRV_DAT_WAIT (so the update never stalls on a busy DAT0) and every
    // wait loop in set_clock()/xfer() is bounded (returns false, never hangs).
    // The sd_bench firmware proved 12 MHz read+write-integrity clean at
    // ~2.7 MB/s (~29x the 400 kHz ID clock) on this board. Best-effort: if the
    // update times out we simply stay at the safe 400 kHz clock.
    step_ = Step::ClockXfer;
    set_clock(kXferHz);

    step_ = Step::Done;
    present_ = true;
    return true;
}

// ============================================================================
// PIO data transfer
// ============================================================================

void SdMmc::enable_hw_interrupts() noexcept {
    // Unmask only the data-completion sources; command()/reads keep polling
    // the raw RINTSTS, so leaving CDONE/RTO masked from the interrupt line is
    // exactly what we want (only DTO/data-errors raise the GIC line).
    wr(RINTSTS, INT_ALL);                       // clear stale
    wr(INTMASK, INT_DTO | INT_DCRC | INT_DRTO);
    wr(CTRL, rd(CTRL) | CTRL_INT_ENABLE);       // global controller int enable
}

bool SdMmc::wait_data_ready() noexcept {
    for (std::uint32_t i = 0; i < kSpin; ++i)
        if ((rd(STATUS) & STAT_BUSY) == 0) return true;
    return false;
}

bool SdMmc::wait_card_ready() noexcept {
    std::uint32_t status = 0;
    for (std::uint32_t i = 0; i < 10000; ++i) {
        if (command(SEND_STATUS, rca_, Resp::R1, 0, &status)) {
            std::uint32_t state = (status >> 9) & 0xFU;
            bool ready = (status & (1U << 8)) != 0;
            if (ready && state == 4) return true;
        }
        nop_delay(1000);
    }
    return false;
}

bool SdMmc::xfer(std::uint32_t lba, void* buf, std::uint32_t count, bool write) noexcept {
    if (!present_ || count == 0) return false;

    // Re-apply the phase immediately before the transfer (setting it once in
    // init() is not enough — intervening activity leaves the phase delay stale).
    // IMPORTANT: for a WRITE set ONLY the drive phase, matching exactly what the
    // diag scan did when it was clean. Writing the sample-phase register after it
    // re-latches the shared phase DLL and disturbs the drive phase.
    if (xfer_set_phase_) {
        if (write) set_drive_phase(kDrivePhase);
        else       set_sample_phase(kSamplePhase);
    }

    // SDHC/SDXC address by block; SDSC addresses by byte.
    std::uint32_t addr = high_capacity_ ? lba : lba * kBlockSize;
    const bool multi = count > 1;

    // Best-effort: wait for any prior programming to finish, but do NOT fail if
    // DAT0 reads busy -- on this board DAT0 has no pull-up and floats "busy" when
    // idle. Transfer completion is detected via the DTO interrupt below.
    wait_data_ready();
    ctrl_reset(CTRL_FIFO_RESET);
    wr(RINTSTS, INT_ALL);
    // Arm the completion latch BEFORE the data command so a fast DTO cannot
    // race the fill-then-wait sequence below (see the write DTO wait).
    if (irq_mode_ && write) {
        g_sd_isr_status = 0;
        sd_completion_reset();
    }
    wr(BLKSIZ, kBlockSize);
    wr(BYTCNT, kBlockSize * count);

    std::uint32_t idx = write ? (multi ? WRITE_MULTI_BLOCK : WRITE_SINGLE_BLOCK)
                              : (multi ? READ_MULTIPLE_BLOCK : READ_SINGLE_BLOCK);
    std::uint32_t flags = CMD_DATA_EXP | (write ? CMD_RW : 0) | (multi ? CMD_SEND_STOP : 0);

    std::uint32_t r1 = 0;
    if (!command(idx, addr, Resp::R1, flags, &r1)) {
        xfer_stage_ = 0; xfer_rints_ = last_rints_;   // command failed
        return false;
    }

    std::uint32_t words = (kBlockSize * count) / 4;
    auto* p = static_cast<std::uint32_t*>(buf);

    if (write) {
        for (std::uint32_t w = 0; w < words; ++w) {
            // Wait for FIFO space using the FIFO count.
            // A safe depth limit is 128 words (since the FIFO is 256 words deep).
            for (std::uint32_t i = 0; i < kSpin; ++i) {
                std::uint32_t st = rd(STATUS);
                std::uint32_t fifo_count = (st >> FIFO_COUNT_SHIFT) & FIFO_COUNT_MASK;
                if (fifo_count < 128) break;
            }
            wr(DATA, p[w]);
        }
    } else {
        std::uint32_t guard = 0;
        for (std::uint32_t w = 0; w < words; ) {
            std::uint32_t st = rd(STATUS);
            if (st & STAT_FIFO_EMPTY) {
                // Bail out if the data never arrives (error/timeout).
                std::uint32_t m = rd(RINTSTS);
                if (m & (INT_DCRC | INT_DRTO)) return false;
                if ((m & INT_DTO) && (st & STAT_FIFO_EMPTY)) return false;  // done but short
                if (++guard > kSpin) return false;
                continue;
            }
            guard = 0;
            std::uint32_t avail = (st >> FIFO_COUNT_SHIFT) & FIFO_COUNT_MASK;
            while (avail-- && w < words) p[w++] = rd(DATA);
        }
    }

    // Wait for data-transfer-over.
    //
    // Interrupt path (writes only, once armed): the FIFO is already loaded and
    // the controller is clocking it to the card; block on the ISR-posted
    // semaphore instead of spinning a whole core through the multi-ms
    // programming time. g_sd_isr_status carries the RINTSTS the ISR captured.
    // Reads and the pre-scheduler bring-up keep the polled loop below.
    bool dto = false;
    std::uint32_t m = 0;
    if (irq_mode_ && write) {
        sd_completion_wait();                 // blocks until DTO/error IRQ
        m = g_sd_isr_status;
        if (m & (INT_DCRC | INT_DRTO)) { xfer_stage_ = 1; xfer_rints_ = m; return false; }
        if (m & INT_DTO) dto = true;
    } else {
        for (std::uint32_t i = 0; i < kSpin; ++i) {
            m = rd(RINTSTS);
            if (m & (INT_DCRC | INT_DRTO)) { xfer_stage_ = write ? 1 : 2; xfer_rints_ = m; return false; }
            if (m & INT_DTO) { dto = true; break; }
        }
    }
    if (!dto) { xfer_stage_ = write ? 3 : 4; xfer_rints_ = m; return false; }  // no DTO
    xfer_stage_ = -1; xfer_rints_ = m;   // success: record final data RINTSTS
    wr(RINTSTS, INT_ALL);

    // Explicit STOP for multi-block (host auto-stop is also enabled above).
    if (multi) {
        std::uint32_t r = 0;
        command(STOP_TRANSMISSION, 0, Resp::R1b, 0, &r);
    }
    // Wait for card programming to finish using CMD13 SEND_STATUS (only for writes).
    if (write) {
        wait_card_ready();
    }
    return true;
}

// Post-init: read LBA0 at each sample phase (tolerant of the command response)
// to find the phase where the DATA lines transfer cleanly (no DCRC).
int SdMmc::diag_scan_dataread(ScanResult* out, int max, void* scratch) noexcept {
    tolerant_cmd_ = true;
    int n = 0;
    for (std::uint32_t phase = 0; phase < 4 && n < max; ++phase) {
        set_sample_phase(phase);
        bool ok = xfer(0, scratch, 1, /*write=*/false);
        out[n].sample_nineties = static_cast<std::uint8_t>(phase);
        out[n].hold = use_hold_reg_;
        out[n].rints = xfer_rints_;
        out[n].resp = *static_cast<std::uint32_t*>(scratch);  // first word read
        out[n].clean = ok && ((xfer_rints_ & INT_DCRC) == 0);
        ++n;
    }
    tolerant_cmd_ = false;
    set_sample_phase(kSamplePhase);
    return n;
}

// Post-init (DESTRUCTIVE at lba): write buf at each DRIVE phase, checking DCRC.
int SdMmc::diag_scan_datawrite(ScanResult* out, int max, std::uint32_t lba,
                               const void* buf) noexcept {
    tolerant_cmd_ = true;
    xfer_set_phase_ = false;   // this scan controls the drive phase itself
    int n = 0;
    for (std::uint32_t drive = 0; drive < 4 && n < max; ++drive) {
        set_drive_phase(drive);
        bool ok = xfer(lba, const_cast<void*>(buf), 1, /*write=*/true);
        out[n].sample_nineties = static_cast<std::uint8_t>(drive);  // = drive phase
        out[n].hold = use_hold_reg_;
        out[n].rints = xfer_rints_;
        out[n].resp = static_cast<std::uint32_t>(xfer_stage_);
        out[n].clean = ok && ((xfer_rints_ & INT_DCRC) == 0);
        ++n;
    }
    tolerant_cmd_ = false;
    xfer_set_phase_ = true;
    set_drive_phase(kDrivePhase);
    return n;
}

bool SdMmc::read_blocks(std::uint32_t lba, void* dst, std::uint32_t count) noexcept {
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (xfer(lba, dst, count, /*write=*/false)) return true;
        ctrl_reset(CTRL_FIFO_RESET);
        wr(RINTSTS, INT_ALL);
        wait_card_ready();
    }
    return false;
}

bool SdMmc::write_blocks(std::uint32_t lba, const void* src, std::uint32_t count) noexcept {
    // Retry on a transient DCRC: on this board the first write after selection
    // tends to fail its CRC, but a reset + retry succeeds (the diag scan only ever
    // saw clean writes after earlier writes had already run).
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (xfer(lba, const_cast<void*>(src), count, /*write=*/true)) return true;
        ctrl_reset(CTRL_FIFO_RESET);
        wr(RINTSTS, INT_ALL);
        wait_card_ready();
        nop_delay(20000);
    }
    return false;
}

}  // namespace sdcard
