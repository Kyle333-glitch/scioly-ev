#pragma once

#include <Arduino.h>

#include <bsp_api.h>

#include <cstdint>

namespace evhw {
namespace resetCause {

// ---------------------------------------------------------------------------
// The reset-status registers, read back by the MCU itself - bench-only
// ---------------------------------------------------------------------------
//
// Why this exists
//
//   P-H0W Step 3 requires the reason for a restart to be established, not
//   assumed: "whether the restart is attributable to the watchdog rather than to
//   something else ... If a reset-cause register is readable on this core, record
//   it; do not assume one exists". This is that register readout. Without it, the
//   attribution of a hang-induced restart rests on the reset latency matching the
//   requested timeout and on the absence of any other refresh path - inference
//   from timing, which is weaker than the silicon saying so.
//
// Where the register definition comes from
//
//   `bsp_api.h` -> `renesas.h` -> `R7FA4M1AB.h` (the vendor device header for
//   this exact part, `BSP_MCU_GROUP_RA4M1`) defines `R_SYSTEM` and the
//   `RSTSR0`/`RSTSR1` members and their bit masks. This file therefore invents no
//   address and no bit position: every offset and mask used below is the vendor's
//   own, and a wrong one is a compile error rather than a plausible-looking
//   number. The masks used are `R_SYSTEM_RSTSR0_PORF_Msk`,
//   `R_SYSTEM_RSTSR0_DPSRSTF_Msk`, and for RSTSR1 `WDTRF`, `IWDTRF`, `SWRF`,
//   `RPERF`, `REERF`, `BUSMRF`, `BUSSRF`, `SPERF`.
//
//   Note what the header does **not** say: it declares the registers, it does not
//   say who preserves them. The FSP ships prebuilt in this framework
//   (`variants/NANOR4/libs/libfsp.a`, no C sources in the variant tree), so a
//   source search over the framework cannot prove that startup leaves these
//   values alone - and the DFU bootloader runs before the sketch as well. Whether
//   the flags this reads are the flags the last reset latched is therefore an
//   open question until it is measured, which is exactly what the protocol's
//   before/after comparison is for.
//
// What this is not
//
//   * Not a control input. Nothing in the firmware branches on it, and no
//     safety-relevant behavior depends on it: it is a diagnostic readout, in
//     `src/bench/`, which the production environment does not compile
//     (`platformio.ini`). The vehicle image cannot contain it.
//   * Not a claim about the reset's *cause* on its own. A flag says the silicon
//     recorded that source; it does not say the vehicle was safe when it
//     happened, which is H1A's electrical question.
//
// Reads are passive; clearing is a separate, deliberate act
//
//   `read()` writes nothing, so a readout cannot destroy the evidence it reports,
//   and the protocol can read the same value twice and see the same answer.
//   `clear()` is the only write in this file, and it exists because of what was
//   measured on the board (2026-09-29, D30): **these flags latch across software
//   resets**. A board that has taken one watchdog reset reports `wdtrf=1` on
//   every later boot - across uploads, across a re-flash of a different image -
//   **until a genuine power cycle clears RSTSR1** (`porf=1` and nothing else,
//   D30), so a bare read cannot attribute a *new* reset. The attribution
//   experiment is therefore read, clear, observe, read: without the clear,
//   `wdtrf=1` after a deliberate hang is indistinguishable from a leftover from
//   a previous session, which is exactly what the first baseline capture showed.
struct Snapshot {
    // The raw register contents, reported so a decode can be checked and so a
    // flag this file does not name is not silently dropped.
    uint8_t rstsr0 = 0U;
    uint16_t rstsr1 = 0U;

    bool powerOnReset = false;             // RSTSR0.PORF - a power-on reset
    bool deepSoftwareReset = false;        // RSTSR0.DPSRSTF - deep software reset
    bool watchdogReset = false;            // RSTSR1.WDTRF - the WDT reset the vehicle uses
    bool independentWatchdogReset = false; // RSTSR1.IWDTRF - the IWDT, which this firmware never opens
    bool softwareReset = false;            // RSTSR1.SWRF - a software reset request
    bool ramParityError = false;           // RSTSR1.RPERF
    bool reeError = false;                 // RSTSR1.REERF
    bool busMasterError = false;           // RSTSR1.BUSMRF
    bool busSlaveError = false;            // RSTSR1.BUSSRF
    bool securityError = false;            // RSTSR1.SPERF
};

inline Snapshot read() {
    Snapshot snapshot;
    snapshot.rstsr0 = static_cast<uint8_t>(R_SYSTEM->RSTSR0);
    snapshot.rstsr1 = static_cast<uint16_t>(R_SYSTEM->RSTSR1);
    snapshot.powerOnReset = (snapshot.rstsr0 & R_SYSTEM_RSTSR0_PORF_Msk) != 0U;
    snapshot.deepSoftwareReset = (snapshot.rstsr0 & R_SYSTEM_RSTSR0_DPSRSTF_Msk) != 0U;
    snapshot.watchdogReset = (snapshot.rstsr1 & R_SYSTEM_RSTSR1_WDTRF_Msk) != 0U;
    snapshot.independentWatchdogReset = (snapshot.rstsr1 & R_SYSTEM_RSTSR1_IWDTRF_Msk) != 0U;
    snapshot.softwareReset = (snapshot.rstsr1 & R_SYSTEM_RSTSR1_SWRF_Msk) != 0U;
    snapshot.ramParityError = (snapshot.rstsr1 & R_SYSTEM_RSTSR1_RPERF_Msk) != 0U;
    snapshot.reeError = (snapshot.rstsr1 & R_SYSTEM_RSTSR1_REERF_Msk) != 0U;
    snapshot.busMasterError = (snapshot.rstsr1 & R_SYSTEM_RSTSR1_BUSMRF_Msk) != 0U;
    snapshot.busSlaveError = (snapshot.rstsr1 & R_SYSTEM_RSTSR1_BUSSRF_Msk) != 0U;
    snapshot.securityError = (snapshot.rstsr1 & R_SYSTEM_RSTSR1_SPERF_Msk) != 0U;
    return snapshot;
}

// Clears both registers, so a following `read()` reports only what has happened
// since this call. It writes zero to every bit - this family's reset-status flags
// are cleared by writing 0 to them - and the write is self-verifying: `read()`
// immediately afterwards shows whether it took effect, so a wrong guess about the
// idiom shows up as an unchanged value rather than as a plausible-looking number.
//
// The registers hold reset *status* only. Nothing about clocks, pins, timers or
// the watchdog's configuration lives here, so clearing them cannot change how the
// board runs, and no control path reads them (this file is bench-only).
inline void clear() {
    R_SYSTEM->RSTSR0 = 0U;
    R_SYSTEM->RSTSR1 = 0U;
}

}  // namespace resetCause
}  // namespace evhw
