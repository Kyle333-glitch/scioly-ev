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
//   it is **false** in the vehicle image. H0W (the on-target watchdog check) has
//   been run and recorded (D26-D30), but on the **bench** image and under a bench
//   load: the path opens at the chosen timeout, counts, refreshes only from this
//   checkpoint, resets a deliberately hung loop, and the board re-uploads after
//   that reset. It has **not** been observed under a loaded run, which is why
//   `kWatchdogTimeoutMs` is recorded as bench-qualified and provisional (H0W
//   Step 8, `data/prereg/P-H0W-step8-2026-09-29.md`), and why enablement in the
//   vehicle image stays a separate, deliberate change. Compiling and linking the
//   adapter - which every stage does, so the mechanism is proven to build - is
//   still not validation of any behavior.
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
// The requested timeout is measured, and it is checked at compile time
//
//   `kWatchdogTimeoutMs` and the two gaps it must cover are measurements now, not
//   placeholders (D26/D27, re-taken in D30; the numbers and their evidence are on
//   the constants below). What the static assertions add on top of them is the
//   *rule*: a build that requests enablement with a timeout that does not cover
//   the boot gap and the worst refresh interval by the margin does not compile.
//   Flipping enablement is therefore not enough to get a too-short timeout onto
//   the board - the numbers have to satisfy the rule, which is exactly the
//   deliberate act H0W describes. Note that the rule is applied to the **requested**
//   value while the timer programs a longer realized period (174 ms for the 114 ms
//   request, measured in D30), so the realized margin is larger than the asserted
//   one.
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

// The timeout, in milliseconds, and the measured facts behind it.
//
// These are no longer placeholders. They are the numbers H0W measured on the
// board (DEVELOPMENT_LOG.md D26/D27; captures in
// `data/evidence/P-H0W-2026-09-28/`), and the rule below is asserted against them:
//
//   * `kBootToFirstRefreshBudgetMs = 0` - the measured gap is **below one
//     millisecond**. `first_refresh_ms` reported 0 in every enabled soak, because
//     the watchdog is opened at the very end of `setup()` and the first pass
//     follows immediately. Zero here is a measurement at millisecond resolution,
//     not an unmeasured absence: the true value is somewhere in [0, 1) ms.
//   * `kWorstRefreshIntervalBudgetMs = 14` - the longest interval observed
//     between two consecutive refreshes, identical (14 ms) across every healthy
//     load condition measured: telemetry off, the highest admissible rate with the
//     host reading, the same rate with the host deliberately not reading, and
//     after a watchdog reset. It is one early pass' cost, not a periodic stall:
//     the loop itself runs at ~46,000 passes/s, so a refresh gap is normally 0 ms
//     at this resolution and this value never grew over 190 s of soaking.
//   * `kWatchdogPlanMarginMs = 100` - fixed in advance in the H0W
//     pre-registration (`data/prereg/P-H0W-2026-09-28.md`), before any of the
//     numbers above existed, as five times the nominal 20 ms control period.
//   * `kWatchdogTimeoutMs = 114` - `ev::requiredWatchdogTimeoutMs()`'s answer for
//     those inputs (14 + 100), asserted below so the request cannot drift away
//     from the rule that chose it.
//
// The requested value is **not** the realized one: the timer rounds the request up
// to the next achievable prescaler/reload pair. **Measured (D30): the realized
// period for this 114 ms request is 174 ms** - `WDTCR.TOPS=3` (16384 cycles),
// `CKS=5` (/256), PCLKB 24 MHz, i.e. 16384 x 256 / 24 MHz = 174.76 ms - reported by
// `bench wdt` from the registers and by `wdt_realized_ms` from the library. It is
// deliberately read from those rather than from the reset latency, because the
// host's latency measurement is quantized by its serial read window, which is the
// same order as the period itself (D30). The margin this value leaves over the
// worst measured gap is what makes the rounding harmless, and in realized terms
// that margin is 174 - 14 = 160 ms, not the 100 ms the rule was applied to.
constexpr uint32_t kWatchdogTimeoutMs = 114U;
constexpr uint32_t kBootToFirstRefreshBudgetMs = 0U;
constexpr uint32_t kWorstRefreshIntervalBudgetMs = 14U;
constexpr uint32_t kWatchdogPlanMarginMs = 100U;

// The same rule H0W applies, asserted against the requested values. A build that
// requests enablement with a timeout that cannot survive boot does not compile.
//
// It asserts the **whole** rule (`timeoutIsQualified`), not just the two coverage
// clauses: the margin is part of the decision H0W's Step 6 applies, and a build
// that dropped it here would be enforcing a rule the protocol does not state.
// With the numbers above the assertion is `114 > 0 && 114 > 14 && 114 >= 114`.
constexpr ev::WatchdogQualification kWatchdogQualification{kBootToFirstRefreshBudgetMs,
                                                           kWorstRefreshIntervalBudgetMs,
                                                           kWatchdogPlanMarginMs};
static_assert(!kWatchdogEnablementRequested ||
                  ev::timeoutIsQualified(kWatchdogTimeoutMs, kWatchdogQualification),
              "the requested watchdog timeout must be longer than both the boot-to-first-refresh "
              "gap and the worst refresh interval, by the margin (P-H0W); the boot gap is the one "
              "a timeout chosen from steady-state intervals alone gets wrong");
static_assert(kWatchdogTimeoutMs == ev::requiredWatchdogTimeoutMs(kWatchdogQualification),
              "the requested watchdog timeout must be the smallest value the rule allows, so a "
              "hand-edited request cannot quietly leave the measured margin behind (P-H0W Step 6)");

// The checkpoint. One instance, owned by the firmware entry point.
class WatchdogCheckpoint {
public:
    // Clears the counters and the refresh history, and starts the refresh
    // interval at `nowMs`. Opens nothing, and touches no timer: opening is
    // `openIfRequested()`, called once at the end of `setup()`.
    //
    // The boot instant itself is not kept: nothing reads it. The two instants
    // this class is asked about are the open and the first refresh
    // (`openedAtMs_`, `timeToFirstRefreshMs_`), and holding a third that no
    // caller wants would be a place for a stale value to look like a
    // measurement.
    void begin(uint32_t nowMs) {
        lastRefreshMs_ = nowMs;
        opened_ = false;
        openAttempted_ = false;
        openRefused_ = false;
        hasFirstRefresh_ = false;
        timeToFirstRefreshMs_ = 0U;
        worstRefreshGapMs_ = 0U;
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
    // the frozen order. `nowMs` must be the refresh event's own clock reading -
    // the loop re-reads `millis()` for this call (R2/M2) - because the
    // inter-refresh gaps below are measured from it: a pass-start stamp would
    // report every refresh as older than it is and hide one pass duration of
    // gap. Resolution is one millisecond, which is the clock's own.
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
        // The interval the timeout has to cover, measured rather than inferred
        // (H0W Step 2): the gap from the *last refresh* to this one. Measured from
        // the previous refresh rather than from the previous pass, so a withheld
        // or skipped refresh shows up here as a larger gap - which is exactly what
        // would eventually trip the timer. It costs nothing: `nowMs` is already a
        // parameter, and no timer or peripheral is read. Resolution is one
        // millisecond, which is more than enough for a timeout measured in
        // hundreds of milliseconds; a sub-millisecond gap (the normal case for a
        // free-running loop) reads 0 rather than being invented. The first refresh
        // is excluded, because the boot-to-first-refresh gap is its own number
        // (`timeToFirstRefreshMs()`) and mixing the two would hide which one is
        // large.
        if (hasFirstRefresh_) {
            const uint32_t gapMs = nowMs - lastRefreshMs_;
            if (gapMs > worstRefreshGapMs_) {
                worstRefreshGapMs_ = gapMs;
            }
        }
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

    // The longest interval observed between two consecutive refreshes,
    // milliseconds, excluding the first refresh (see `endOfPass()`). This is the
    // "worst healthy refresh interval" `ev::WatchdogQualification` wants, read
    // from the loop rather than derived from two `health` lines separated by an
    // unknown amount of work. Zero means either that enablement is off or that no
    // gap of a millisecond or more has happened - the two are told apart by
    // `refreshes()`.
    uint32_t worstRefreshGapMs() const { return worstRefreshGapMs_; }

private:
    bool opened_ = false;
    bool openAttempted_ = false;
    bool openRefused_ = false;
    bool hasFirstRefresh_ = false;
    uint32_t openedAtMs_ = 0U;
    uint32_t timeToFirstRefreshMs_ = 0U;
    uint32_t worstRefreshGapMs_ = 0U;
    uint32_t passes_ = 0U;
    uint32_t refreshes_ = 0U;
    uint32_t withheldRefreshes_ = 0U;
    uint32_t lastRefreshMs_ = 0U;
};

}  // namespace evhw
