#pragma once

#include <stdint.h>

// Nonblocking recurring-interval helper.
//
// This is pure integer logic with no Arduino dependency, so it builds both on
// the Nano R4 target and in the host-side `native` test environment.
//
// The caller supplies a monotonic millisecond timestamp (the same value
// `millis()` returns). This class never reads time itself, which keeps it
// deterministic and testable. Comparisons use unsigned subtraction so the
// logic stays correct across `uint32_t` wraparound at ~49.7 days.
class IntervalTimer {
public:
    explicit IntervalTimer(uint32_t intervalMs) : intervalMs_(intervalMs) {}

    void setIntervalMs(uint32_t intervalMs) { intervalMs_ = intervalMs; }
    uint32_t intervalMs() const { return intervalMs_; }

    // Returns true at most once per `intervalMs()`, measured from the previous
    // firing. The first call only establishes a reference point and returns
    // false. An interval of 0 fires on every call after the first.
    bool elapsed(uint32_t nowMs) {
        if (!started_) {
            started_ = true;
            lastFiredMs_ = nowMs;
            return false;
        }
        if (static_cast<uint32_t>(nowMs - lastFiredMs_) >= intervalMs_) {
            lastFiredMs_ = nowMs;
            return true;
        }
        return false;
    }

    // Re-arms the timer so the next firing is one full interval after nowMs.
    void reset(uint32_t nowMs) {
        started_ = true;
        lastFiredMs_ = nowMs;
    }

private:
    uint32_t intervalMs_;
    uint32_t lastFiredMs_ = 0;
    bool started_ = false;
};
