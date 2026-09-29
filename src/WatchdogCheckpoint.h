#pragma once

#include <Arduino.h>

#include <WDT.h>

#include <WatchdogPlan.h>

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
//   * not the command/telemetry transport, and not the boot path.
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
// When the watchdog is opened, and why it is not opened in `begin()`
//
//   Opening the watchdog is a separate, deliberate call - `openIfRequested()` -
//   made at the **end** of `setup()`, after the one bounded initialization wait
//   the firmware performs. `begin()` deliberately does not open it.
//
//   The reason is the boot gap. A request handed to the timer at the start of
//   initialization has to survive the remainder of `setup()` (including the
//   optional bounded wait for a serial host) and the first main-loop pass before
//   its first refresh can arrive, because the only refresh point is the end of a
//   pass. A timeout chosen from steady-state refresh intervals alone can be
//   shorter than that gap, and the result is not a safe stop: it is a board that
//   resets before `setup()` returns, never boots, and cannot be re-uploaded
//   without a manual bootloader entry. Opening late makes the boot gap small and
//   bounded by construction, and `timeToFirstRefreshMs()` records what it
//   actually was so H0W can enter it in the timeout decision
//   (`ev::timeoutIsQualified()`).
//
//   Opening late costs nothing in coverage: nothing before the first pass is a
//   control cycle, and the interval that matters - the loop's own cadence - is
//   covered from the first pass onwards.
//
// Enablement is one centralized setting, it is OFF, and it is per-environment
//
//   `kWatchdogEnablementRequested` is the single place enablement is decided, and
//   it is **false** in the vehicle image. The watchdog has never been observed on
//   this target: H0W (the on-target watchdog check) has not been run, so nothing
//   here may claim that the watchdog starts, counts, refreshes or resets.
//   Compiling and linking the adapter - which this stage does, so the mechanism is
//   proven to build - is not validation of any behavior.
//
//   The value comes from a build define, `EV_BENCH_WATCHDOG_ENABLEMENT_REQUESTED`,
//   which only the bench environment that runs H0W sets
//   (`[env:bench-watchdog]` in `platformio.ini`). The reason it is a define rather
//   than a literal to edit is that this header is compiled into **both** images:
//   editing it to enable the watchdog for a bench qualification would silently
//   change the vehicle build's decision in the same commit, and the operator
//   checking "is the bench build enabled" could not tell which decision they had
//   just made. As an environment choice, enabling it for a bench qualification
//   cannot touch the vehicle image at all - and `src/main.cpp` refuses to compile
//   if the production image ever receives the define, so the production flip stays
//   a separate, deliberate change.
//
//   With the setting false, neither `openIfRequested()` nor `endOfPass()` ever
//   calls into the timer. The counters still run, so a bench session can see how
//   many passes the loop finished and how many would have been withheld before
//   the timeout is chosen.
//
// The requested timeout is a placeholder, and it is checked at compile time
//
//   `kWatchdogTimeoutMs` is UNVERIFIED, and the two gaps it must cover are
//   placeholders too (no measurement of either exists yet). What is *not* a
//   placeholder is the rule: the static assertions below refuse to compile a
//   build that requests enablement with a timeout that does not cover the boot
//   gap and the worst refresh interval with a margin. Flipping enablement is
//   therefore not enough to get a too-short timeout onto the board - the numbers
//   have to be brought up to the measured rule first, which is exactly the
//   deliberate act H0W describes.
//
// Static check (from the repository root):
//
//     grep -rn "WDT" src/
//
// must match only this file.

namespace evhw {

// Defaulted here so exactly one expression decides the value in every image, and
// so a build that does not define it is unambiguously disabled. Only
// `[env:bench-watchdog]` sets it (`platformio.ini`); the vehicle image never does,
// and `src/main.cpp` asserts that.
#ifndef EV_BENCH_WATCHDOG_ENABLEMENT_REQUESTED
#define EV_BENCH_WATCHDOG_ENABLEMENT_REQUESTED 0
#endif

// The one centralized setting. Turning this on is a deliberate act that requires
// H0W to have been run and recorded in `DEVELOPMENT_LOG.md` and
// `VERIFIED_STATE.md`; it must not be flipped to make a test pass. This stage's
// value is false in both shipped images: `env:bench-watchdog` is built once, for
// H0W, and is not what `pio run` or `pio run -e bench` produces.
constexpr bool kWatchdogEnablementRequested = EV_BENCH_WATCHDOG_ENABLEMENT_REQUESTED != 0;

// The planned timeout, in milliseconds. UNVERIFIED: no measurement of the loop's
// worst-case pass and no measurement of the boot gap exist yet, and no watchdog
// behavior has been observed. H0W chooses this value with
// `ev::requiredWatchdogTimeoutMs()` and records the realized period separately,
// because the timer rounds the request up to the next achievable period.
constexpr uint32_t kWatchdogTimeoutMs = 250U;

// The planned boot-to-first-refresh budget, milliseconds. UNVERIFIED
// placeholder: the real value is whatever `timeToFirstRefreshMs()` reports in
// H0W's soak, and until then this is a guess about how long the end of `setup()`
// and the first pass take. It is small on purpose - the watchdog is opened after
// the bounded serial wait, and this build performs no other wait.
constexpr uint32_t kBootToFirstRefreshBudgetMs = 50U;

// The planned worst healthy refresh interval, milliseconds. UNVERIFIED
// placeholder: the real value is the worst interval between two consecutive
// refreshes across every healthy load condition, which is what H0W's soak
// measures. It is expected to be a little above the 20 ms control period, since
// a pass costs the period plus its own work.
constexpr uint32_t kWorstRefreshIntervalBudgetMs = 40U;

// The margin the timeout must leave above the larger of the two gaps,
// milliseconds. UNVERIFIED placeholder; H0W records the margin it used.
constexpr uint32_t kWatchdogPlanMarginMs = 25U;

// The same rule H0W applies, asserted against the requested values. A build that
// requests enablement with a timeout that cannot survive boot does not compile.
constexpr ev::WatchdogQualification kWatchdogQualification{kBootToFirstRefreshBudgetMs,
                                                           kWorstRefreshIntervalBudgetMs,
                                                           kWatchdogPlanMarginMs};
static_assert(!kWatchdogEnablementRequested ||
                  (ev::timeoutCoversBoot(kWatchdogTimeoutMs, kWatchdogQualification) &&
                   ev::timeoutCoversRefreshInterval(kWatchdogTimeoutMs, kWatchdogQualification)),
              "the requested watchdog timeout must be longer than both the boot-to-first-refresh "
              "gap and the worst refresh interval (P-H0W); the boot gap is the one a timeout "
              "chosen from steady-state intervals alone gets wrong");

// The checkpoint. One instance, owned by the firmware entry point.
class WatchdogCheckpoint {
public:
    // Records the boot instant and clears the counters. Opens nothing, and
    // touches no timer: opening is `openIfRequested()`, called once at the end of
    // `setup()`.
    void begin(uint32_t nowMs) {
        bootMs_ = nowMs;
        lastRefreshMs_ = nowMs;
        opened_ = false;
        openAttempted_ = false;
        openRefused_ = false;
        hasFirstRefresh_ = false;
        timeToFirstRefreshMs_ = 0U;
    }

    // Opens the watchdog when, and only when, enablement is requested. Called
    // once, at the end of `setup()`, after the bounded initialization work - see
    // the chronology note in the file header. Records whether the request was
    // made, whether the timer accepted it, and when, so a refused open is visible
    // instead of silent (`WDT.begin()` returns zero for a period the timer cannot
    // produce).
    //
    // Calling it twice does not re-open and does not re-stamp: the attempt is
    // recorded once, so a later call cannot move the instant the boot gap is
    // measured from.
    void openIfRequested(uint32_t nowMs) {
        if (openAttempted_) {
            return;
        }
        openAttempted_ = true;
        if (!kWatchdogEnablementRequested) {
            opened_ = false;
            return;
        }
        opened_ = WDT.begin(kWatchdogTimeoutMs) != 0;
        openRefused_ = !opened_;
        openedAtMs_ = nowMs;
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
        if (!hasFirstRefresh_) {
            // The boot gap, measured rather than assumed: how long the board took
            // from the watchdog being opened to the first refresh. H0W enters this
            // in the timeout choice.
            hasFirstRefresh_ = true;
            timeToFirstRefreshMs_ = nowMs - openedAtMs_;
        }
    }

    // True only when the watchdog was actually opened.
    bool opened() const { return opened_; }

    bool enablementRequested() const { return kWatchdogEnablementRequested; }

    // True when `openIfRequested()` has run. False means the boot path has not
    // reached its open point; with enablement requested that would be a boot-path
    // bug, and H0W's first readout shows it.
    bool openAttempted() const { return openAttempted_; }

    // True when the timer refused the requested period. Visible, never silent.
    bool openRefused() const { return openRefused_; }

    uint32_t requestedTimeoutMs() const { return kWatchdogTimeoutMs; }

    // Passes the loop ended, refreshes actually performed, and passes that were
    // not refreshable. With enablement off, the last two are the evidence H0W
    // needs: they show the loop's health without claiming any watchdog behavior.
    uint32_t passes() const { return passes_; }
    uint32_t refreshes() const { return refreshes_; }
    uint32_t withheldRefreshes() const { return withheldRefreshes_; }
    uint32_t lastRefreshMs() const { return lastRefreshMs_; }

    // The measured interval from the open to the first refresh, milliseconds; 0
    // until the first refresh has happened. The number H0W enters as
    // `bootToFirstRefreshMs` in `ev::WatchdogQualification`.
    uint32_t timeToFirstRefreshMs() const { return timeToFirstRefreshMs_; }
    bool hasTimeToFirstRefresh() const { return hasFirstRefresh_; }

private:
    bool opened_ = false;
    bool openAttempted_ = false;
    bool openRefused_ = false;
    bool hasFirstRefresh_ = false;
    uint32_t bootMs_ = 0U;
    uint32_t openedAtMs_ = 0U;
    uint32_t timeToFirstRefreshMs_ = 0U;
    uint32_t passes_ = 0U;
    uint32_t refreshes_ = 0U;
    uint32_t withheldRefreshes_ = 0U;
    uint32_t lastRefreshMs_ = 0U;
};

}  // namespace evhw
