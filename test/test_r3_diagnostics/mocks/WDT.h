#pragma once

// Host-side mock of the Arduino watchdog-timer API (`WDT.h`) sufficient to
// compile `src/WatchdogCheckpoint.h` and `src/bench/BenchConsole.h` for host
// tests. The timer never runs here: `begin()` records the request and
// `getTimeout()` reports a canned realized period.

#include <cstdint>

class WDTimer {
  public:
    unsigned long begin(unsigned long timeoutMs) {
        requestedMs = timeoutMs;
        opened = true;
        return timeoutMs;
    }

    void refresh() { ++refreshes; }

    unsigned long getTimeout() { return realizedMs; }

    unsigned long requestedMs = 0UL;
    unsigned long realizedMs = 0UL;
    unsigned long refreshes = 0UL;
    bool opened = false;
};

extern WDTimer WDT;
