/**
 * @file sdmmc.hpp
 * @brief Bare-metal SD-card (SDMMC) driver for the Luckfox Lyra B (RK3506)
 *
 * @details
 * The RK3506 SDMMC host at 0xFF480000 is a Synopsys DesignWare Mobile Storage
 * Host Controller ("dw_mmc").  This driver is a compact, freestanding C++
 * re-implementation of the U-Boot `drivers/mmc/dw_mmc.c` + `mmc.c` bring-up
 * sequence, cut down to what a bare-metal StateOS image actually needs:
 *
 *   - controller reset + FIFO/PIO configuration (no IDMAC descriptors),
 *   - the SD (not eMMC) card-identification state machine,
 *   - single/multi block read and write over the DWMCI_DATA FIFO (PIO).
 *
 * It is deliberately polled and DMA-free so it can run from any of the three
 * cores without touching the MMU page tables or the GIC.  The clock/IOMUX
 * bring-up mirrors `sdcard.md` (CRU + GPIO3A pinmux).
 *
 * TUNING: the only board-specific number is kBusHz — the frequency of the
 * cclk_src_sdmmc clock that CRU_CLKSEL_CON49 selects.  The card divider is
 * derived from it.  If init fails, that is the first knob to check.
 *
 * The full vendor U-Boot MMC sources this was distilled from are kept verbatim
 * under sdcard/reference/uboot-mmc/ for cross-checking register semantics.
 *
 * Reference: Synopsys DesignWare MSHC databook; RK3506 TRM §SDMMC; U-Boot
 * drivers/mmc/{dw_mmc.c,mmc.c,mmc-uclass.c}.
 */

#pragma once

#include <cstdint>

namespace sdcard {

// ============================================================================
// Base addresses (RK3506) — matches sdcard.md
// ============================================================================

inline constexpr std::uintptr_t kSdmmcBase = 0xFF480000U;  ///< DW-MMC host

// cclk_src_sdmmc frequency set by hardware_init(): the 24 MHz OSC (CCLK_SDMMC
// _SEL_24M, divider 1). A *known* source is the whole point — the DesignWare
// card clock = kBusHz / (2 * CLKDIV), so an exact kBusHz gives an exact clock.
inline constexpr std::uint32_t kBusHz      = 24000000U;     ///< 24 MHz OSC source
inline constexpr std::uint32_t kInitHz     = 400000U;       ///< ID clock (400 kHz)
inline constexpr std::uint32_t kXferHz     = 12000000U;     ///< default-speed (24MHz/2)

inline constexpr std::uint32_t kBlockSize  = 512U;          ///< SD sector size

// Response/data sample phase (in units of 90 degrees). Once the CMD/DAT lines
// have proper pull-ups (see hardware_init step E), a uniform 0 deg samples every
// response type (R1/R2/R6/R7 and R3/OCR) and read data cleanly. The exotic
// per-response phases we once needed were a symptom of the floating lines, not a
// real requirement. kSamplePhaseR3 is kept separate only for the diagnostic scans.
inline constexpr std::uint32_t kSamplePhase   = 0;         ///< 0 deg: all responses + read data
inline constexpr std::uint32_t kSamplePhaseR3 = 0;         ///< 0 deg: R3 (same as the rest)
// Command/data DRIVE phase (TIMING_CON0), 90 deg: adds output delay so the card
// meets its hold time. With pull-ups + this drive phase, block writes pass CRC.
inline constexpr std::uint32_t kDrivePhase    = 1;         ///< 90 deg drive

// ============================================================================
// DesignWare MSHC register offsets (from U-Boot include/dwmmc.h)
// ============================================================================

/// DesignWare MSHC register offsets from kSdmmcBase (U-Boot include/dwmmc.h).
enum Reg : std::uint32_t {
    CTRL      = 0x000, ///< Control (resets, interrupt enable)
    PWREN     = 0x004, ///< Power enable
    CLKDIV    = 0x008, ///< Card clock divider
    CLKSRC    = 0x00C, ///< Clock source select
    CLKENA    = 0x010, ///< Clock enable
    TMOUT     = 0x014, ///< Response/data timeout
    CTYPE     = 0x018, ///< Card bus-width type
    BLKSIZ    = 0x01C, ///< Block size
    BYTCNT    = 0x020, ///< Byte count
    INTMASK   = 0x024, ///< Interrupt mask
    CMDARG    = 0x028, ///< Command argument
    CMD       = 0x02C, ///< Command (with start bit)
    RESP0     = 0x030, ///< Response 0
    RESP1     = 0x034, ///< Response 1
    RESP2     = 0x038, ///< Response 2
    RESP3     = 0x03C, ///< Response 3
    MINTSTS   = 0x040, ///< Masked interrupt status
    RINTSTS   = 0x044, ///< Raw interrupt status
    STATUS    = 0x048, ///< Status (FIFO/busy)
    FIFOTH    = 0x04C, ///< FIFO threshold
    CDETECT   = 0x050, ///< Card detect
    WRTPRT    = 0x054, ///< Write protect
    TCBCNT    = 0x05C, ///< Transferred CIU byte count
    TBBCNT    = 0x060, ///< Transferred host byte count
    DEBNCE    = 0x064, ///< Debounce
    USRID     = 0x068, ///< User ID
    VERID     = 0x06C, ///< Version ID
    HCON      = 0x070, ///< Hardware configuration
    UHS_REG   = 0x074, ///< UHS-1 register
    BMOD      = 0x080, ///< Bus mode (IDMAC)
    DATA      = 0x200, ///< Data FIFO (PIO)
};

// CTRL bits
inline constexpr std::uint32_t CTRL_RESET       = 1u << 0;  ///< Controller reset
inline constexpr std::uint32_t CTRL_FIFO_RESET  = 1u << 1;  ///< FIFO reset
inline constexpr std::uint32_t CTRL_DMA_RESET   = 1u << 2;  ///< DMA reset
inline constexpr std::uint32_t CTRL_INT_ENABLE  = 1u << 4;  ///< Global interrupt enable
inline constexpr std::uint32_t CTRL_RESET_ALL   = CTRL_RESET | CTRL_FIFO_RESET | CTRL_DMA_RESET; ///< All three resets

// CMD bits
inline constexpr std::uint32_t CMD_RESP_EXP     = 1u << 6;   ///< Response expected
inline constexpr std::uint32_t CMD_RESP_LONG    = 1u << 7;   ///< Long (136-bit) response
inline constexpr std::uint32_t CMD_CHECK_CRC    = 1u << 8;   ///< Check response CRC
inline constexpr std::uint32_t CMD_DATA_EXP     = 1u << 9;   ///< Data transfer expected
inline constexpr std::uint32_t CMD_RW           = 1u << 10;  ///< 1 = write
inline constexpr std::uint32_t CMD_SEND_STOP    = 1u << 12;  ///< Auto send STOP after transfer
inline constexpr std::uint32_t CMD_PRV_DAT_WAIT = 1u << 13;  ///< Wait for previous data to finish
inline constexpr std::uint32_t CMD_INIT         = 1u << 15;  ///< send 80 init clocks
inline constexpr std::uint32_t CMD_UPD_CLK      = 1u << 21;  ///< update clock regs only
inline constexpr std::uint32_t CMD_USE_HOLD_REG = 1u << 29;  ///< Route CMD/DAT through the hold register
inline constexpr std::uint32_t CMD_START        = 1u << 31;  ///< Start command (self-clearing)

// Interrupt-status bits (RINTSTS)
inline constexpr std::uint32_t INT_RE    = 1u << 1;   ///< response error
inline constexpr std::uint32_t INT_CDONE = 1u << 2;   ///< command done
inline constexpr std::uint32_t INT_DTO   = 1u << 3;   ///< data transfer over
inline constexpr std::uint32_t INT_TXDR  = 1u << 4;   ///< TX FIFO data request
inline constexpr std::uint32_t INT_RXDR  = 1u << 5;   ///< RX FIFO data request
inline constexpr std::uint32_t INT_RCRC  = 1u << 6;   ///< response CRC error
inline constexpr std::uint32_t INT_DCRC  = 1u << 7;   ///< data CRC error
inline constexpr std::uint32_t INT_RTO   = 1u << 8;   ///< response timeout
inline constexpr std::uint32_t INT_DRTO  = 1u << 9;   ///< data read timeout
inline constexpr std::uint32_t INT_ALL   = 0xFFFFFFFFu; ///< All interrupt bits (write-1-clear mask)

// STATUS bits
inline constexpr std::uint32_t STAT_BUSY        = 1u << 9;   ///< data busy
inline constexpr std::uint32_t STAT_FIFO_EMPTY  = 1u << 2;   ///< FIFO empty
inline constexpr std::uint32_t STAT_FIFO_FULL   = 1u << 3;   ///< FIFO full
inline constexpr std::uint32_t FIFO_COUNT_SHIFT = 17;        ///< Shift for the FIFO-count field in STATUS
inline constexpr std::uint32_t FIFO_COUNT_MASK  = 0x1FFFu;   ///< Mask for the FIFO-count field

// CTYPE (bus width)
inline constexpr std::uint32_t CTYPE_1BIT = 0;         ///< 1-bit bus
inline constexpr std::uint32_t CTYPE_4BIT = 1u << 0;   ///< 4-bit bus

// ============================================================================
// SD/MMC protocol commands (from U-Boot include/mmc.h)
// ============================================================================

/// Standard SD/MMC command indices (U-Boot include/mmc.h).
enum Cmd : std::uint32_t {
    GO_IDLE_STATE       = 0,   ///< CMD0  reset to idle
    ALL_SEND_CID        = 2,   ///< CMD2  request CID
    SEND_RELATIVE_ADDR  = 3,   ///< CMD3  ask card to publish its RCA
    SWITCH_FUNC         = 6,   ///< CMD6  switch function
    SELECT_CARD         = 7,   ///< CMD7  select/deselect card
    SEND_IF_COND        = 8,   ///< CMD8  voltage/interface condition
    SEND_CSD            = 9,   ///< CMD9  request CSD
    STOP_TRANSMISSION   = 12,  ///< CMD12 stop multi-block transfer
    SEND_STATUS         = 13,  ///< CMD13 card status
    SET_BLOCKLEN        = 16,  ///< CMD16 set block length
    READ_SINGLE_BLOCK   = 17,  ///< CMD17 read one block
    READ_MULTIPLE_BLOCK = 18,  ///< CMD18 read multiple blocks
    WRITE_SINGLE_BLOCK  = 24,  ///< CMD24 write one block
    WRITE_MULTI_BLOCK   = 25,  ///< CMD25 write multiple blocks
    APP_CMD             = 55,  ///< CMD55 next command is app-specific (ACMD)
};

/// Application-specific (ACMD) command indices, preceded by CMD55.
enum ACmd : std::uint32_t {
    SET_BUS_WIDTH = 6,   ///< ACMD6
    SEND_OP_COND  = 41,  ///< ACMD41
};

// OCR bits
inline constexpr std::uint32_t OCR_BUSY = 0x80000000u;  ///< power-up done
inline constexpr std::uint32_t OCR_HCS  = 0x40000000u;  ///< high capacity (SDHC/SDXC)

/// Response-type shorthand used by SdMmc::command().
enum class Resp : std::uint8_t {
    None, ///< No response
    R1,   ///< Normal response
    R1b,  ///< Normal response with busy
    R2,   ///< CID/CSD (long) response
    R3,   ///< OCR response
    R6,   ///< Published RCA response
    R7,   ///< Card interface condition response
};

/// Bring-up stages recorded in SdMmc::step() so a failed init() reports
/// exactly where it stopped (invaluable for on-hardware debugging).
enum class Step : std::uint8_t {
    Start,     ///< before any hardware access
    CtrlReset, ///< controller reset
    ClockInit, ///< 400 kHz ID clock configured
    Cmd0,      ///< CMD0 go-idle
    Cmd8,      ///< CMD8 interface condition
    Acmd41,    ///< ACMD41 operating condition
    Cmd2,      ///< CMD2 all-send-CID
    Cmd3,      ///< CMD3 send RCA
    Cmd9,      ///< CMD9 send CSD
    Cmd7,      ///< CMD7 select card
    Acmd6,     ///< ACMD6 set bus width
    Cmd16,     ///< CMD16 set block length
    ClockXfer, ///< transfer clock configured
    Done,      ///< identification complete
};

/// Human-readable name for a Step (for logging). @param s Stage. @return Static name string.
inline const char* step_name(Step s) noexcept {
    switch (s) {
        case Step::Start:     return "start";
        case Step::CtrlReset: return "ctrl-reset";
        case Step::ClockInit: return "clock@400kHz";
        case Step::Cmd0:      return "CMD0 go-idle";
        case Step::Cmd8:      return "CMD8 if-cond";
        case Step::Acmd41:    return "ACMD41 op-cond";
        case Step::Cmd2:      return "CMD2 all-send-cid";
        case Step::Cmd3:      return "CMD3 send-rca";
        case Step::Cmd9:      return "CMD9 send-csd";
        case Step::Cmd7:      return "CMD7 select";
        case Step::Acmd6:     return "ACMD6 bus-width";
        case Step::Cmd16:     return "CMD16 blocklen";
        case Step::ClockXfer: return "clock@25MHz";
        case Step::Done:      return "done";
    }
    return "?";
}

// ============================================================================
// Driver
// ============================================================================

/// One result row of SdMmc::diag_scan().
struct ScanResult {
    std::uint8_t  sample_nineties;  ///< sample phase / 90 (0,1,2,3 -> 0/90/180/270 deg)
    bool          hold;             ///< USE_HOLD_REG on/off
    std::uint32_t rints;            ///< CMD8 RINTSTS
    std::uint32_t resp;             ///< CMD8 RESP0
    bool          clean;            ///< no RE/RCRC/RTO and resp echo == 0xAA
};

/**
 * @class SdMmc
 * @brief Polled, DMA-free SD-card driver over the RK3506 DW-MMC host.
 *
 * Typical use:
 * @code
 *   sdcard::SdMmc sd;
 *   if (sd.init()) {
 *       std::uint8_t sector[512];
 *       sd.read_block(0, sector);      // read the MBR
 *   }
 * @endcode
 */
class SdMmc {
public:
    SdMmc() = default;
    SdMmc(const SdMmc&) = delete;
    SdMmc& operator=(const SdMmc&) = delete;

    /// Bring up clocks/pinmux, reset the host, and run SD identification.
    /// @return true if a card was identified and is ready for transfers.
    bool init() noexcept;

    /**
     * @brief Diagnostic: probe CMD8 across sample-phase × hold-reg combinations.
     * @param out Array receiving one row per combination.
     * @param max Capacity of @p out.
     * @return Number of rows written. Used to find a working response-sampling config.
     */
    int diag_scan(ScanResult* out, int max) noexcept;

    /**
     * @brief Diagnostic: sweep the command drive phase (CMD0/CMD8/CMD55), find a working TX phase.
     * @param out Array receiving one row per drive phase.
     * @param max Capacity of @p out.
     * @return Number of rows written.
     */
    int diag_scan_drive(ScanResult* out, int max) noexcept;

    /**
     * @brief Diagnostic: sweep sample phase over the full CMD0/CMD8/CMD55/ACMD41 handshake.
     * @param out Array receiving one row per sample phase.
     * @param max Capacity of @p out.
     * @return Number of rows written; finds the phase where the ACMD41 OCR is valid.
     */
    int diag_scan_acmd41(ScanResult* out, int max) noexcept;

    /// Set the response sample-phase (degree = nineties*90). Debug/tuning. @param nineties Phase in 90-degree units.
    void set_sample_phase(std::uint32_t nineties) noexcept;
    /// Set the command/data drive-phase (degree = nineties*90). Debug/tuning. @param nineties Phase in 90-degree units.
    void set_drive_phase(std::uint32_t nineties) noexcept;

    /**
     * @brief Diagnostic (post-init): read LBA0 at each sample phase to find the data-line phase.
     * @param out     Array receiving one row per sample phase.
     * @param max     Capacity of @p out.
     * @param scratch A caller-provided buffer of at least 512 bytes.
     * @return Number of rows written; a row is clean when the DATA transfer has no DCRC.
     */
    int diag_scan_dataread(ScanResult* out, int max, void* scratch) noexcept;

    /**
     * @brief Diagnostic (post-init, DESTRUCTIVE at @p lba): sweep the data drive phase.
     * @param out Array receiving one row per drive phase.
     * @param max Capacity of @p out.
     * @param lba Scratch LBA to write (its contents are destroyed).
     * @param buf 512-byte payload to write at each phase.
     * @return Number of rows written; a row is clean when the card accepts the write (no DCRC).
     */
    int diag_scan_datawrite(ScanResult* out, int max, std::uint32_t lba, const void* buf) noexcept;

    /**
     * @brief Read consecutive 512-byte sectors.
     * @param lba   First logical block address.
     * @param dst   Destination buffer (>= count*512 bytes).
     * @param count Number of sectors to read.
     * @return true on success.
     */
    bool read_blocks(std::uint32_t lba, void* dst, std::uint32_t count) noexcept;
    /// Read a single sector. @param lba LBA. @param dst 512-byte buffer. @return true on success.
    bool read_block(std::uint32_t lba, void* dst) noexcept { return read_blocks(lba, dst, 1); }

    /**
     * @brief Write consecutive 512-byte sectors.
     * @param lba   First logical block address.
     * @param src   Source buffer (>= count*512 bytes).
     * @param count Number of sectors to write.
     * @return true on success.
     */
    bool write_blocks(std::uint32_t lba, const void* src, std::uint32_t count) noexcept;
    /// Write a single sector. @param lba LBA. @param src 512-byte buffer. @return true on success.
    bool write_block(std::uint32_t lba, const void* src) noexcept { return write_blocks(lba, src, 1); }

    [[nodiscard]] bool     present()        const noexcept { return present_; }        ///< @return true if a card is identified.
    [[nodiscard]] bool     high_capacity()  const noexcept { return high_capacity_; }  ///< @return true for SDHC/SDXC (block addressing).
    [[nodiscard]] std::uint64_t capacity_bytes() const noexcept { return capacity_bytes_; } ///< @return Card capacity in bytes.
    [[nodiscard]] std::uint32_t rca()        const noexcept { return rca_; }           ///< @return Relative card address (<<16).

    /**
     * @brief Reprogram the card clock to at most @p hz (fcard = kBusHz / (2*div)).
     *
     * Exposed so a benchmark or the init path can bump the transfer clock above
     * the 400 kHz ID clock. Bounded (never hangs).
     * @param hz Requested maximum clock in Hz.
     * @return false if the clock-update command times out.
     */
    bool set_transfer_clock(std::uint32_t hz) noexcept { return set_clock(hz); }

    // --- smp_test_int4 interrupt-driven completion ------------------------
    // When IRQ mode is on, the WRITE data-transfer-over (DTO) wait blocks on
    // an application semaphore posted by the DW-MMC ISR instead of spinning.
    // Card bring-up and reads stay polled (they run before the scheduler /
    // GIC routing are ready). See smp_test_int4/sd/sdmmc.cpp and the test doc.
    /// Enable/disable interrupt-driven write completion. @param on true to arm.
    void set_irq_mode(bool on) noexcept { irq_mode_ = on; }
    /// @return true if interrupt-driven write completion is armed.
    [[nodiscard]] bool irq_mode() const noexcept { return irq_mode_; }
    /// Unmask DTO + data errors at the controller (INTMASK + CTRL_INT_ENABLE).
    /// Call once, after mount, before set_irq_mode(true) and GIC routing.
    void enable_hw_interrupts() noexcept;

    /// @return Last bring-up stage reached; on a failed init() the stage that failed (pair with @ref step_name).
    [[nodiscard]] Step step() const noexcept { return step_; }

    /// @return Raw last-command response word 0 (debug aid after a failure).
    [[nodiscard]] std::uint32_t last_resp() const noexcept { return last_resp_; }

    /// @return RINTSTS snapshot from the last command() (debug aid).
    [[nodiscard]] std::uint32_t last_rints() const noexcept { return last_rints_; }
    [[nodiscard]] std::uint32_t dbg_cdetect() const noexcept { return rd(CDETECT); } ///< @return Live CDETECT register.
    [[nodiscard]] std::uint32_t dbg_status()  const noexcept { return rd(STATUS);  } ///< @return Live STATUS register.
    [[nodiscard]] std::uint32_t dbg_ctype()   const noexcept { return rd(CTYPE);   } ///< @return Live CTYPE register.
    [[nodiscard]] std::uint32_t dbg_clkdiv()  const noexcept { return rd(CLKDIV);  } ///< @return Live CLKDIV register.
    [[nodiscard]] std::uint32_t dbg_timing0() const noexcept { return rd(0x130);   } ///< @return Live TIMING_CON0 (drive phase).
    [[nodiscard]] std::uint32_t dbg_timing1() const noexcept { return rd(0x134);   } ///< @return Live TIMING_CON1 (sample phase).
    [[nodiscard]] int          dbg_xfer_stage() const noexcept { return xfer_stage_; } ///< @return Stage where xfer() last failed.
    [[nodiscard]] std::uint32_t dbg_xfer_rints() const noexcept { return xfer_rints_; } ///< @return RINTSTS at the last xfer() failure.
    [[nodiscard]] std::uint32_t dbg_cmd55_rints() const noexcept { return cmd55_rints_; } ///< @return CMD55 RINTSTS captured during ACMD41.
    [[nodiscard]] std::uint32_t dbg_cmd55_resp()  const noexcept { return cmd55_resp_;  } ///< @return CMD55 RESP0 captured during ACMD41.
    [[nodiscard]] bool          dbg_cmd55_ok()    const noexcept { return cmd55_ok_;    } ///< @return true if CMD55 succeeded during ACMD41.

private:
    // --- low-level register access ---
    static volatile std::uint32_t& reg(std::uint32_t off) noexcept {
        return *reinterpret_cast<volatile std::uint32_t*>(kSdmmcBase + off);
    }
    static std::uint32_t rd(std::uint32_t off) noexcept { return reg(off); }
    static void          wr(std::uint32_t off, std::uint32_t v) noexcept { reg(off) = v; }

    // --- helpers (implemented in sdmmc.cpp) ---
    bool ctrl_reset(std::uint32_t mask) noexcept;
    bool set_clock(std::uint32_t hz) noexcept;
    bool command(std::uint32_t idx, std::uint32_t arg, Resp resp,
                 std::uint32_t extra_flags, std::uint32_t* resp_out) noexcept;
    bool app_command(std::uint32_t idx, std::uint32_t arg, Resp resp,
                     std::uint32_t* resp_out) noexcept;
    bool xfer(std::uint32_t lba, void* buf, std::uint32_t count, bool write) noexcept;
    bool wait_data_ready() noexcept;
    bool wait_card_ready() noexcept;

    // --- state ---
    bool          present_        = false;
    bool          high_capacity_  = false;   ///< SDHC/SDXC (block addressing)
    std::uint32_t rca_            = 0;        ///< relative card address (<<16)
    std::uint64_t capacity_bytes_ = 0;
    Step          step_           = Step::Start;  ///< last stage reached
    std::uint32_t last_resp_      = 0;        ///< last response word 0
    std::uint32_t last_rints_     = 0;        ///< last command RINTSTS snapshot
    std::uint32_t cmd55_rints_    = 0;        ///< CMD55 RINTSTS (debug)
    std::uint32_t cmd55_resp_     = 0;        ///< CMD55 RESP0 (debug)
    bool          cmd55_ok_       = false;    ///< CMD55 returned success (debug)
    bool          use_hold_reg_   = false;    ///< add CMD_USE_HOLD_REG to commands
    bool          phase_per_resp_ = false;    ///< command() picks sample phase by resp type
    int           xfer_stage_     = -1;       ///< where xfer() last failed (debug)
    std::uint32_t xfer_rints_     = 0;        ///< RINTSTS at xfer failure (debug)
    bool          tolerant_cmd_   = false;    ///< command() ignores RCRC/RE (debug/data scan)
    bool          xfer_set_phase_ = false;    ///< xfer() re-applies drive/sample phase
    bool          irq_mode_       = false;    ///< interrupt-driven write completion armed
};

// ---------------------------------------------------------------------------
// smp_test_int4 interrupt plumbing (C linkage so the port IRQ hook + main can
// reach it without pulling in the class).
// ---------------------------------------------------------------------------
extern "C" {
/// DW-MMC interrupt service — call from port_external_irq_handler on the core
/// the SD IRQ is routed to. Reads RINTSTS; on DTO/DCRC/DRTO it captures the
/// status, clears those bits, and calls sd_completion_post().
void sd_irq_service(void);

/// @return the RINTSTS word the ISR captured at the last completion.
std::uint32_t sd_irq_last_status(void);

/// Application-provided completion hooks (weak no-op defaults in sdmmc.cpp):
///   sd_completion_reset — drain the semaphore to 0 at transfer start, before
///                         any DTO can fire (kills the fill-vs-wait race);
///   sd_completion_wait  — block the calling thread until the ISR signals;
///   sd_completion_post  — called from the ISR to release the waiter.
/// The test wires these to a µOS++ counting semaphore.
void sd_completion_reset(void);
void sd_completion_wait(void);
void sd_completion_post(void);
}

/// One-shot bring-up used by init() and by callers that only need
/// the clock/pinmux brought up (mirrors sdcard.md::sdmmc_hardware_init).
void hardware_init() noexcept;

}  // namespace sdcard
