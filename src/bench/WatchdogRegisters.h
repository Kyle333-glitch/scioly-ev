#pragma once

#include <Arduino.h>

#include <r_wdt.h>

#include <cstdint>

namespace evhw {
namespace watchdogRegisters {

// ---------------------------------------------------------------------------
// What the watchdog hardware is actually programmed with - bench-only
// ---------------------------------------------------------------------------
//
// Why this exists
//
//   P-H0W Step 6 and Step 7 both turn on the same distinction: the requested
//   timeout is not the realized period. "Do not treat `kWatchdogTimeoutMs` as the
//   realized period" (Step 1), "record the realized period" (Step 7). The library
//   can state what it computed (`WDTimer::getTimeout()`, reported as
//   `wdt_realized_ms`), and this file reads the **hardware's own** configuration so
//   that statement can be checked against the registers the timer was given.
//
// Where the definitions come from
//
//   `r_wdt.h` -> `bsp_api.h` -> `renesas.h` -> `R7FA4M1AB.h` (the vendor device
//   header for this part) defines `R_WDT`, the `WDTCR`/`WDTSR` members and the bit
//   masks used below. No address and no bit position is invented here.
//
//   Deliberately NOT decoded here: which timeout period or clock division a given
//   `TOPS`/`CKS` value selects. That mapping lives in the hardware manual, not in a
//   header this build can see, so this readout reports the raw field values and
//   lets the record do the decoding against the vendor's own enumerations
//   (`wdt_timeout_t`, `wdt_clock_division_t` in `r_wdt_api.h`, which the WDT
//   library's lookup tables are checked against). A period computed here from a
//   half-remembered table would be exactly the kind of plausible-looking number
//   `AGENTS.md` section 4 forbids.
//
//   `pclkbHz()` comes from the FSP's own clock API - the same accessor the WDT
//   library uses to turn milliseconds into cycles - so the clock half of any period
//   calculation is the vendor's number rather than this file's guess.
//
// What it is not
//
//   * Not a control input: nothing branches on it, and it is in `src/bench/`, which
//     the production environment does not compile.
//   * Not a statement that the watchdog *works*. That is the hang campaign's
//     evidence; this is a description of how the timer was configured.
//
// Reads are passive: nothing here writes a watchdog register. `WDTSR.CNTVAL`
// sampled between two refreshes shows the down-counter, so a caller can see the
// countdown the timeout is measured against.
struct Snapshot {
    uint16_t wdtcr = 0U;  // raw WDT Control Register
    uint16_t wdtsr = 0U;  // raw WDT Status Register
    // The WDTCR fields, taken with the vendor's own masks.
    uint8_t tops = 0U;  // [1..0]  timeout period selection
    uint8_t cks = 0U;   // [7..4]  clock division ratio selection
    uint8_t rpes = 0U;  // [9..8]  window end position selection
    uint8_t rpss = 0U;  // [13..12] window start position selection
    // The live down-counter and its two flags.
    uint16_t counterValue = 0U;  // [13..0]
    bool underflow = false;      // [14] the counter reached zero
    bool refreshError = false;   // [15] a refresh arrived outside the window
};

inline Snapshot read() {
    Snapshot snapshot;
    snapshot.wdtcr = static_cast<uint16_t>(R_WDT->WDTCR);
    snapshot.wdtsr = static_cast<uint16_t>(R_WDT->WDTSR);
    snapshot.tops = static_cast<uint8_t>((snapshot.wdtcr & R_WDT_WDTCR_TOPS_Msk) >> R_WDT_WDTCR_TOPS_Pos);
    snapshot.cks = static_cast<uint8_t>((snapshot.wdtcr & R_WDT_WDTCR_CKS_Msk) >> R_WDT_WDTCR_CKS_Pos);
    snapshot.rpes = static_cast<uint8_t>((snapshot.wdtcr & R_WDT_WDTCR_RPES_Msk) >> R_WDT_WDTCR_RPES_Pos);
    snapshot.rpss = static_cast<uint8_t>((snapshot.wdtcr & R_WDT_WDTCR_RPSS_Msk) >> R_WDT_WDTCR_RPSS_Pos);
    snapshot.counterValue =
        static_cast<uint16_t>((snapshot.wdtsr & R_WDT_WDTSR_CNTVAL_Msk) >> R_WDT_WDTSR_CNTVAL_Pos);
    snapshot.underflow = (snapshot.wdtsr & R_WDT_WDTSR_UNDFF_Msk) != 0U;
    snapshot.refreshError = (snapshot.wdtsr & R_WDT_WDTSR_REFEF_Msk) != 0U;
    return snapshot;
}

// PCLKB in Hz, from the FSP's own clock accessor ("private" here means "internal
// to the FSP", not "unavailable": the WDT library calls the same function with the
// same argument to compute a millisecond timeout in cycles).
inline uint32_t pclkbHz() {
    return R_FSP_SystemClockHzGet(FSP_PRIV_CLOCK_PCLKB);
}

}  // namespace watchdogRegisters
}  // namespace evhw
