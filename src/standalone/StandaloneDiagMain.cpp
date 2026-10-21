#include <Arduino.h>

#include <StandaloneDiagnostic.h>

#include "MotorDriver.h"
#include "PinMap.h"
#include "SerialTransport.h"
#include "VehicleRuntime.h"
#include "WatchdogCheckpoint.h"

// ---------------------------------------------------------------------------
// The standalone diagnostic image - `env:standalone-diag`
// ---------------------------------------------------------------------------
//
// What this image is for
//
//   One supervised, wheels-raised test of the *battery-powered* vehicle with no
//   laptop attached: power on, wait for the steady ready indication, touch the
//   physical pencil trigger, watch one short motor pulse, and see the indicator
//   settle into its "finished" pattern. It proves the trigger, the driver enable
//   path and the onboard supply - and nothing about distance, braking or
//   repeatability.
//
// Why it is a separate environment rather than a define
//
//   A motor activation outside a competition run is exactly the capability the
//   production image must not contain. `src/main.cpp` and this file are mutually
//   exclusive build artefacts: `env:nano_r4` and `env:bench` exclude
//   `src/standalone/`, and this environment excludes both other entry points
//   (`platformio.ini`). A source tree that is not compiled cannot run, which is
//   a stronger guarantee than a macro.
//
// What it keeps from the production entry point
//
//   * The same boot order: the motor driver is de-energized first, before the
//     encoder, the inputs or the serial port exist; the watchdog checkpoint is
//     recorded; the serial port is opened last with the one bounded wait; the
//     watchdog is opened at the very end.
//   * The same control chain: `VehicleRuntime` runs unchanged, so the same
//     scheduler, the same frozen tick order, the same safety boundary and the
//     same fault latches are live. This image simply never arms a competition
//     run, and the bounded pulse is arbitrated against the runtime exactly as the
//     bench instrument is (`ev::benchApplyVerdict()`).
//
// Where the bounded pulse is applied
//
//   Ownership is decided *before* the control pass, so the tick's own motor write
//   yields and each pass has exactly one writer (`MotorDriver.h`). The pulse is
//   written *after* the pass, and the moment ownership is released the driver is
//   explicitly de-energized - including on passes where no control tick ran, when
//   nothing else would have written the pins.
//
// This image is an instrument, not a competition build. Its identity reports
// `variant=standalone-diag`, and no run in this image is a competition run.

static_assert(EV_FIRMWARE_VARIANT == 4,
              "src/standalone/ is the standalone diagnostic image only; build env:standalone-diag");

// The production watchdog-enablement define belongs to `env:bench-watchdog` and
// must never reach this image: this mode's stop path is the bounded expiry in
// `ev::BenchDrive`, which the loop evaluates, and a watchdog reset must not be
// confused with it.
static_assert(!evhw::kWatchdogEnablementRequested,
              "the standalone diagnostic image must not request watchdog enablement");

namespace {

constexpr unsigned long kSerialBaud = 115200;
constexpr uint32_t kSerialWaitTimeoutMs = 2000;

evhw::VehicleRuntime& runtimeRef() {
    static evhw::VehicleRuntime instance;
    return instance;
}
evhw::WatchdogCheckpoint watchdog;
ev::StandaloneDiagnosticController diagnostic;

// Whether the previous pass held the motor pins. Dropping ownership without a
// write would leave the last drive applied on a tickless pass, so the release
// is an explicit de-energize.
bool motorPinsHeld = false;

}  // namespace

void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);

    // 1. Safe initialization, in the vehicle layer's order: the driver is
    //    de-energized before anything else exists to ask for motion.
    runtimeRef().begin(millis());

    // 2. The watchdog checkpoint's boot instant. It opens nothing here.
    watchdog.begin(millis());

    // 3. The serial port, last of the hardware, with the one bounded wait. It is
    //    optional: this image is designed for no host at all, and the wait ends
    //    after `kSerialWaitTimeoutMs` whether or not anything is attached.
    evhw::serialTransport::begin(kSerialBaud);
    const unsigned long startMs = millis();
    while (!Serial && millis() - startMs < kSerialWaitTimeoutMs) {
    }

    // 4. The watchdog, opened last and only if enablement were requested (it is
    //    not, in this image).
    watchdog.openIfRequested(millis());
}

void loop() {
    const uint32_t nowMs = millis();

    // The raw active-LOW trigger level. `SensorInputs::begin()` (inside
    // `runtime.begin()`) configured D12 with its pull-up; this is a direct read,
    // not a second input abstraction.
    const bool triggerActive = digitalRead(evhw::kStartTrigger) == LOW;

    // Decided before the tick: may this pass write the motor pins at all?
    const ev::StandaloneDiagnosticStep step = diagnostic.update(
        nowMs, triggerActive, runtimeRef().telemetry().encoderTicks, runtimeRef().armed(),
        runtimeRef().faultLatched(), runtimeRef().runUnderway(),
        runtimeRef().orderViolationDetected());
    evhw::motorDriver::setMotorPinsOwned(step.ownsMotorPins);

    // One pass of the production chain, unchanged. Its own motor write yields
    // while the diagnostic holds the pins, and de-energizes the driver otherwise.
    const evhw::PassOutcome outcome = runtimeRef().runPass(nowMs);

    // The single watchdog checkpoint, as in production.
    watchdog.endOfPass(outcome.refreshable(), millis());

    // Low-priority work: the operator interface when a host happens to be
    // attached. Nothing here is required for the diagnostic to run.
    runtimeRef().serviceSerial(nowMs);

    // The bounded pulse goes to the driver last: after the tick's own write, and
    // only on a pass the arbitration granted.
    if (step.ownsMotorPins) {
        evhw::motorDriver::applyBench(diagnostic.drive().output());
    } else if (motorPinsHeld) {
        // Ownership released: a tick may not have run on this pass, so this is
        // the write that ends the pulse.
        evhw::motorDriver::failSafe();
    }
    motorPinsHeld = step.ownsMotorPins;
    evhw::motorDriver::setMotorPinsOwned(false);

    digitalWrite(LED_BUILTIN,
                 ev::standaloneIndicatorLedOn(diagnostic.indicatorState(), nowMs) ? HIGH : LOW);
}
