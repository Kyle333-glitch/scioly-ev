#include <Arduino.h>

#include <IntervalTimer.h>

#include "SerialTransport.h"
#include "VehicleRuntime.h"
#include "WatchdogCheckpoint.h"

// ---------------------------------------------------------------------------
// Firmware entry point: the cooperative shell
// ---------------------------------------------------------------------------
//
// One pass of the loop is, in order:
//
//   1. `runtime.runPass(nowMs)` - the scheduling decision, and the control tick
//      when one is due. The tick runs the frozen stage order and is the only
//      thing that samples, decides, authorizes or writes the motor pins.
//   2. `watchdog.endOfPass(...)` - the one central watchdog health checkpoint.
//      It refreshes only when the pass finished and, if a tick ran, that tick
//      completed in order. No interrupt, no module and nothing else in this
//      firmware refreshes the watchdog.
//   3. Low-priority work, outside the control tick: the operator interface
//      (`runtime.serviceSerial`) and the status LED. Keeping it after the
//      checkpoint means a slow job costs cadence (reported as scheduler lateness)
//      without ever hiding the fact that the loop ran, and it means the serial
//      path can never refresh the watchdog or delay a due tick's checkpoint.
//
// What this build does NOT do, on purpose:
//
//   * it never arms, announces or starts a run by itself: those are commands an
//     operator types, each gated by `ev::OperatorPolicy`, and every value in
//     `VehicleConfig.h` is provisional, so the configuration-build checks refuse
//     them. The run policy stays in `Idle` and every drive request is refused;
//   * it enables no watchdog: `kWatchdogEnablementRequested` is false.
//
// The serial path is a *consumer* of the control path and never a participant: it
// reads commands, answers them, and moves queued telemetry records out, all
// without any call that can wait for a host (`SerialTransport.h`), and none of it
// can drive, arm or disarm except through the deliberate transitions listed above.
//
// Everything printed is descriptive only. A serial line, a build, or a quiet
// monitor is not evidence about the vehicle: no physical test has been performed
// on this firmware, and nothing below may be read as one.

namespace {

constexpr unsigned long kSerialBaud = 115200;
constexpr uint32_t kSerialWaitTimeoutMs = 2000;
constexpr uint32_t kStatusLedIntervalMs = 500;

evhw::VehicleRuntime runtime;
evhw::WatchdogCheckpoint watchdog;
IntervalTimer statusLedTimer(kStatusLedIntervalMs);
bool statusLedState = false;

// Low-priority work: the status LED. Deliberately outside the control tick, and
// deliberately last, so it cannot delay a due tick or the checkpoint.
void updateStatusLed(uint32_t nowMs) {
    if (statusLedTimer.elapsed(nowMs)) {
        statusLedState = !statusLedState;
        digitalWrite(LED_BUILTIN, statusLedState ? HIGH : LOW);
    }
}

// Nothing is printed here any more. The boot line (`BOOT,...`, staged by
// `runtime.begin()` before this point) and the `EVE1` boot event carry the same
// facts through the nonblocking transport, and `status`, `checks` and `version`
// answer the rest on request. A blocking `Serial.print()` of a banner is exactly
// what `SerialTransport.h` exists to avoid: the boot message must not depend on
// somebody watching the port.

}  // namespace

void setup() {
    // The status LED is the only peripheral configured before the vehicle layer,
    // and it cannot move anything.
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);

    // 1. The vehicle layer's safe initialization: the motor driver is
    //    de-energized before anything else exists to ask for motion.
    runtime.begin(millis());

    // 2. The central watchdog checkpoint. With enablement off it opens nothing;
    //    it still counts the passes the loop completes.
    watchdog.begin(millis());

    // 3. The serial port, last and after the hardware is already safe. The wait
    //    for a host is the one bounded initialization wait the nonblocking
    //    contract allows, and it is optional: the lines staged by the runtime are
    //    pumped by the loop whether or not a host is attached now.
    evhw::serialTransport::begin(kSerialBaud);
    const unsigned long startMs = millis();
    while (!Serial && millis() - startMs < kSerialWaitTimeoutMs) {
    }
}

void loop() {
    const uint32_t nowMs = millis();

    const evhw::PassOutcome outcome = runtime.runPass(nowMs);

    // The single watchdog health checkpoint. `refreshable()` is false only when a
    // control tick ran and did not complete in order.
    watchdog.endOfPass(outcome.refreshable(), nowMs);

    // Low-priority work, after the checkpoint: the operator interface first (a
    // command's answer and the queued records), then the status LED. Neither can
    // hold up a tick, and both are serviced whether or not anything is attached.
    runtime.serviceSerial(nowMs);

    updateStatusLed(nowMs);
}
