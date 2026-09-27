#pragma once

#include <Arduino.h>

#include <WDT.h>

#include <cstdint>

// ---------------------------------------------------------------------------
// The central watchdog health checkpoint
// ---------------------------------------------------------------------------
//
// One rule, one place
//
//   The vehicle's watchdog is refreshed from exactly one point in the firmware:
//   the end of a main-loop pass that finished, and whose control tick - if one
//   ran - completed every stage in the frozen order. Nothing else may refresh it:
//
//   * not an interrupt handler (the encoder ISR in `EncoderInput.cpp` cannot
//     reach this module at all, and does no such thing);
//   * not the motion controller, the run state machine, the safety boundary, the
//     motor driver or the telemetry stage - a module that could refresh the
//     watchdog by itself would be able to hide its own hang;
//   * not the future command/telemetry transport.
//
//   The mechanism is structural rather than documented: `endOfPass()` is only
//   reachable from the loop, and the loop passes it the verdict of the pass it
//   just completed. A hang anywhere in the pass - including in the control tick,
//   in a driver write or in a low-priority job - therefore stops the refreshes and
//   lets the watchdog do its job.
//
// What a withheld refresh means
//
//   A pass is refreshable when it completed. A control tick that ran out of order
//   (see `ev::ControlTickOrderGuard`) makes its pass unrefreshable, which is the
//   point: firmware whose control cycle ran wrongly should not be trusted to keep
//   feeding the watchdog. The watchdog's own reset then returns the vehicle to a
//   de-energized state, because the driver is re-initialized de-energized before
//   anything else runs.
//
// Enablement is one centralized setting, and it is OFF
//
//   `kWatchdogEnablementRequested` is the single place production enablement is
//   decided. It is **false**, and the watchdog has never been observed on this
//   target: H0W (the on-target watchdog check) has not been run, so nothing here
//   may claim that the watchdog starts, counts, refreshes or resets. Compiling
//   and linking the adapter - which this stage does, so the mechanism is proven to
//   build - is not validation of any behavior.
//
//   With the setting false, `begin()` does not open the watchdog and `endOfPass()`
//   never calls `refresh()`. The counters still run, so a bench session can see
//   how many passes the loop finished and how many would have been withheld
//   before the timeout is chosen.
//
// Static check (from the repository root):
//
//     grep -rn "WDT" src/
//
// must match only this file.

namespace evhw {

// The one centralized production setting. Turning this on is a deliberate act
// that requires H0W to have been run and recorded in `DEVELOPMENT_LOG.md` and
// `VERIFIED_STATE.md`; it must not be flipped to make a test pass.
constexpr bool kWatchdogEnablementRequested = false;

// The planned timeout, in milliseconds. UNVERIFIED: no measurement of the loop's
// worst-case pass exists yet, and no watchdog behavior has been observed. It is
// long enough for a 50 Hz control tick plus a low-priority job to be sampled once
// only if H0W says so.
constexpr uint32_t kWatchdogTimeoutMs = 250U;

// The checkpoint. One instance, owned by the firmware entry point.
class WatchdogCheckpoint {
public:
    // Opens the watchdog when, and only when, enablement is requested. Records
    // whether it was actually opened, so a refused open is visible instead of
    // silent.
    void begin(uint32_t nowMs) {
        lastRefreshMs_ = nowMs;
        if (!kWatchdogEnablementRequested) {
            opened_ = false;
            return;
        }
        opened_ = WDT.begin(kWatchdogTimeoutMs) != 0;
        if (opened_) {
            ++refreshes_;
            lastRefreshMs_ = nowMs;
        }
    }

    // The single refresh path. `passCompleted` must be this pass's own verdict:
    // the loop reached its end and the control tick it ran, if any, completed in
    // the frozen order.
    void endOfPass(bool passCompleted, uint32_t nowMs) {
        ++passes_;
        if (!passCompleted) {
            ++withheldRefreshes_;
            return;
        }
        if (!opened_) {
            return;
        }
        WDT.refresh();
        ++refreshes_;
        lastRefreshMs_ = nowMs;
    }

    // True only when the watchdog was actually opened.
    bool opened() const { return opened_; }
    bool enablementRequested() const { return kWatchdogEnablementRequested; }

    // Passes the loop ended, refreshes actually performed, and passes that were
    // not refreshable. With enablement off, the last two are the evidence H0W
    // needs: they show the loop's health without claiming any watchdog behavior.
    uint32_t passes() const { return passes_; }
    uint32_t refreshes() const { return refreshes_; }
    uint32_t withheldRefreshes() const { return withheldRefreshes_; }
    uint32_t lastRefreshMs() const { return lastRefreshMs_; }

private:
    bool opened_ = false;
    uint32_t passes_ = 0U;
    uint32_t refreshes_ = 0U;
    uint32_t withheldRefreshes_ = 0U;
    uint32_t lastRefreshMs_ = 0U;
};

}  // namespace evhw
