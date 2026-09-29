#pragma once

#include <cstdint>

namespace ev {

// ---------------------------------------------------------------------------
// Watchdog timeout qualification - the pure rule H0W applies
// ---------------------------------------------------------------------------
//
// What this module is
//
// * The arithmetic behind choosing a watchdog timeout: the requested timeout has
//   to be longer than *every* gap the watchdog is asked to survive, and there
//   are two of them, not one.
//
//   1. **Boot to first refresh.** The watchdog is opened once, deliberately, at
//      the end of initialization, and the first refresh happens at the end of the
//      first completed main-loop pass. The interval between those two events
//      includes the rest of `setup()` and the first pass. A timeout shorter than
//      that interval resets the board *during* initialization, before the loop
//      has run once - the board never comes up, and the failure looks like a
//      bricked board rather than like a badly chosen timeout.
//   2. **The worst healthy refresh interval.** Between two consecutive completed
//      passes, under every load condition that is healthy. This is the interval
//      H0W measures directly.
//
//   A budget that covers only the second is the mistake this module exists to
//   make impossible to express by accident: the boot gap is not a "consecutive
//   refresh" interval, so a rule written in terms of consecutive refreshes
//   cannot see it.
//
// What this module is not
//
// * It is not a measurement. Every number it takes is supplied by the caller;
//   nothing here has been measured on the vehicle, and none of these functions
//   may be cited as evidence about the target's timer.
// * It is not the place the timeout is requested. The single requested value
//   lives with the watchdog adapter's configuration (the vehicle layer's
//   `WatchdogCheckpoint.h`), which is also where the compile-time form of this
//   rule is asserted.
// * It does not model the library's quantization. The timer rounds the requested
//   period *up* to the next achievable prescaler/reload pair, so the realized
//   period is the requested value or slightly longer. H0W records the realized
//   period; this module reasons about the request.
//
// Kind of change this is
//
//   Policy arithmetic over measured inputs, in `lib/` because it is pure and
//   host-testable and because the protocol that produces the numbers
//   (`docs/bringup/P-H0W-watchdog-qualification.md`) and the firmware that
//   consumes them must not be able to disagree about what "covered" means.

// The measured facts a timeout choice rests on. Deliberately no default
// constructor: a timeout chosen without numbers behind it must be impossible to
// write, not merely inconvenient.
//
// A zero in either gap is a statement, not an absence: it says the corresponding
// interval is instantaneous, which no real loop is. The functions below accept it
// (the boot gap of a build that opens the watchdog after its last pass is
// genuinely small) but a caller that has not measured must not pass zero.
struct WatchdogQualification {
    constexpr WatchdogQualification(uint32_t bootToFirstRefreshMs_,
                                    uint32_t worstRefreshIntervalMs_,
                                    uint32_t marginMs_)
        : bootToFirstRefreshMs(bootToFirstRefreshMs_),
          worstRefreshIntervalMs(worstRefreshIntervalMs_),
          marginMs(marginMs_) {}

    // Longest observed interval from the watchdog being opened to the first
    // refresh, milliseconds.
    uint32_t bootToFirstRefreshMs;

    // Longest observed interval between two consecutive refreshes, across every
    // healthy load condition, milliseconds. H0W records the worst of them, never
    // the average.
    uint32_t worstRefreshIntervalMs;

    // How much room the choice leaves above the larger of the two gaps, in
    // milliseconds. A margin of zero is accepted and means "no margin", which is
    // a decision a caller may make explicitly; it is not a default.
    uint32_t marginMs;
};

// The larger of the two gaps: the interval a timeout must be strictly longer
// than.
constexpr uint32_t worstWatchdogGapMs(const WatchdogQualification& qualification) {
    return qualification.bootToFirstRefreshMs > qualification.worstRefreshIntervalMs
               ? qualification.bootToFirstRefreshMs
               : qualification.worstRefreshIntervalMs;
}

// The smallest timeout that covers both gaps with the stated margin, saturating
// at the largest representable millisecond value rather than wrapping. Saturation
// matters: a wrapped number here would be a *shorter* timeout than the gap it is
// supposed to cover, which is the exact failure this module prevents.
constexpr uint32_t requiredWatchdogTimeoutMs(const WatchdogQualification& qualification) {
    // One expression, because a `constexpr` function body in this project's
    // standard is a single return statement.
    return worstWatchdogGapMs(qualification) > 0xFFFFFFFFu - qualification.marginMs
               ? 0xFFFFFFFFu
               : worstWatchdogGapMs(qualification) + qualification.marginMs;
}

// True when `timeoutMs` is strictly longer than the boot-to-first-refresh gap.
// Strictly, because a timeout exactly equal to the gap expires at the same
// instant the first refresh is due, and which of the two the hardware does first
// is not something this firmware may assume.
constexpr bool timeoutCoversBoot(uint32_t timeoutMs, const WatchdogQualification& qualification) {
    return timeoutMs > qualification.bootToFirstRefreshMs;
}

// True when `timeoutMs` is strictly longer than the worst healthy refresh
// interval.
constexpr bool timeoutCoversRefreshInterval(uint32_t timeoutMs,
                                            const WatchdogQualification& qualification) {
    return timeoutMs > qualification.worstRefreshIntervalMs;
}

// Both of the above, plus the margin. This is the rule H0W's decision step
// applies and the rule the firmware's requested timeout is asserted against.
constexpr bool timeoutIsQualified(uint32_t timeoutMs, const WatchdogQualification& qualification) {
    return timeoutCoversBoot(timeoutMs, qualification) &&
           timeoutCoversRefreshInterval(timeoutMs, qualification) &&
           timeoutMs >= requiredWatchdogTimeoutMs(qualification);
}

}  // namespace ev
