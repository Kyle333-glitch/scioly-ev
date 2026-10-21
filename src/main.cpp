#include <Arduino.h>

#include <StandaloneArming.h>
#include <StandaloneIndicator.h>

#include "SerialTransport.h"
#include "VehicleRuntime.h"
#include "WatchdogCheckpoint.h"

// The production image must never receive the bench-only watchdog-enablement
// define. `kWatchdogEnablementRequested` is shared by both images
// (`WatchdogCheckpoint.h`), so a build that set
// `EV_BENCH_WATCHDOG_ENABLEMENT_REQUESTED` while compiling *this* entry point
// would change the vehicle's watchdog decision from what looks like a bench
// build option. H0W decides whether production enablement is warranted, and
// carrying that decision into the vehicle image is a separate, deliberate change
// to this file - which is why the guard is here and not a comment.
static_assert(!evhw::kWatchdogEnablementRequested,
              "the production image must not request watchdog enablement; build "
              "env:bench-watchdog for the H0W qualification (P-H0W)");

// The H1A electrical-gate diagnostic (`bench h1a-brake`) is a temporary,
// bench-only instrument: it can force the driver's awake-EN-low combination,
// which is a state the production run path reaches only through its own stop
// logic. It must never reach the vehicle image, and this guard makes the build
// say so instead of a comment.
#if defined(EV_BENCH_H1A_DIAGNOSTIC)
#error "EV_BENCH_H1A_DIAGNOSTIC is bench-only (H1A) and must never be defined for the vehicle image"
#endif

// The tournament-only emergency image (`env:emergency`, removed in D35) must
// never leak into the vehicle image. It was a standalone entry point with its
// own build filter, and it is recoverable from the D35 preservation snapshot;
// no define may turn this entry point into it.
#if defined(EV_EMERGENCY_IMAGE) || defined(DEV_EMERGENCY_IMAGE)
#error "emergency image defines are removed (D35) and must never be defined for the vehicle image"
#endif

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
//
// The watchdog itself, when enablement requests it, is opened once at the very
// end of `setup()` - after the bounded initialization work, not before it - so
// the boot-to-first-refresh gap cannot exceed the timeout. See
// `WatchdogCheckpoint.h`.
//   3. Low-priority work, outside the control tick: the operator interface
//      (`runtime.serviceSerial`) and the status LED. Keeping it after the
//      checkpoint means a slow job costs cadence (reported as scheduler lateness)
//      without ever hiding the fact that the loop ran, and it means the serial
//      path can never refresh the watchdog or delay a due tick's checkpoint.
//
// Standalone (battery-only) operation
//
//   This image is the laptop-free one. Nothing below needs a USB host, a DTR
//   line, `Serial.available()` or a typed command:
//
//     * after boot the motor is de-energized, and it stays de-energized until a
//       deliberate onboard gesture arms a run;
//     * the gesture is a 1.5 s HOLD of the physical start trigger
//       (`ev::StandaloneArmingGate`). It loads the competition preset
//       (`ev::standalonePreset`), announces it and arms it through exactly the
//       same rule validation, configuration-build checks and
//       `ev::SafetyAuthority` a typed `announce`/`arm` uses. A refused arm
//       (any Provisional calibration value - today's state) leaves the vehicle
//       unable to move;
//     * once armed, a separate short, debounced PRESS of the same trigger starts
//       the run. The trigger's edge is produced by `ev::StartTriggerDetector`,
//       so a trigger held during boot starts nothing and a held pencil cannot
//       retrigger a run;
//     * the onboard LED reports the state without a monitor: heartbeat = waiting,
//       steady = armed and ready, blink = running, double blink = finished,
//       fast blink = fault;
//     * the serial interface is still there for diagnosis when a host happens to
//       be attached, and a typed `disarm` still stops the vehicle. Neither is
//       required for any of the above.
//
//   What standalone mode does NOT do: it does not bypass a check, invent a
//   measurement or turn a Provisional value into an armed run. Until the
//   calibration in `docs/standalone-ev-operation.md` section E exists, the arm is
//   refused and the LED keeps the heartbeat - the vehicle is safe, not ready.
//
// What this build does NOT do, on purpose:
//
//   * it never announces or arms from a boot: arming needs the deliberate hold
//     gesture above, and every value in `VehicleConfig.h` is provisional, so the
//     configuration-build checks refuse it today. The run policy stays in `Idle`
//     and every drive request is refused;
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

evhw::VehicleRuntime& runtimeRef() {
    static evhw::VehicleRuntime instance;
    return instance;
}
evhw::WatchdogCheckpoint watchdog;

// The deliberate onboard arming gesture. Constructed in the "nothing prepared"
// state, and only ever moved by the physical trigger.
ev::StandaloneArmingGate standaloneArming;

// The trigger's verdict as of the last control tick, kept so the arming gate and
// the LED can be serviced on passes where no tick ran.
ev::StartTriggerVerdict lastTriggerVerdict{};

// The onboard arming gesture, serviced every pass after the control tick. The
// hold is timed against `nowMs` here rather than inside the tick because a
// gesture is a human-scale interval (1.5 s), not a control interval.
void serviceStandaloneArming(uint32_t nowMs) {
    // A firmware `reset` (typed over USB when a host happens to be attached) is the
    // new-run boundary: if this gesture had prepared a run and the runtime is back
    // at `Idle`, disarmed, the gate returns to "nothing prepared" so a fresh hold
    // is required. It never re-arms by itself - the next 1.5 s hold is still a
    // deliberate act. A finished or faulted run is not `Idle`, so it is not
    // re-armed here either; that still needs a power cycle (or `reset`).
    if (standaloneArming.prepared() && !runtimeRef().armed() &&
        runtimeRef().runPhase() == ev::RunPhase::Idle) {
        standaloneArming.reset();
    }

    lastTriggerVerdict = runtimeRef().startTriggerVerdict();
    const ev::StandaloneArmVerdict arm = standaloneArming.update(nowMs, lastTriggerVerdict);
    if (!arm.requestPrepare) {
        return;
    }
    const evhw::StandalonePrepareResult result = runtimeRef().prepareStandaloneRun(nowMs);
    // The gate is told the authority's answer, not the attempt's: a refused arm
    // leaves the vehicle un-prepared and waiting, exactly as it is.
    standaloneArming.reportPreparation(result.armed);
}

// Low-priority work: the status LED. Deliberately outside the control tick, and
// deliberately last, so it cannot delay a due tick or the checkpoint. It is the
// only status a battery-only operator has, so its pattern is the mapping in
// `StandaloneIndicator.h` and nothing ad hoc.
void updateStatusLed(uint32_t nowMs) {
    const ev::StandaloneIndicatorState state = ev::productionIndicatorState(
        runtimeRef().runPhase(), runtimeRef().armed(), runtimeRef().faultLatched(),
        lastTriggerVerdict.inactiveQualified);
    digitalWrite(LED_BUILTIN, ev::standaloneIndicatorLedOn(state, nowMs) ? HIGH : LOW);
}

// Nothing is printed here any more. The boot line (`BOOT,...`, staged by
// `runtime.begin()` before this point) and the `EVE3` boot event carry the same
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
    runtimeRef().begin(millis());

    // 2. The central watchdog checkpoint. It records the boot instant here but
    //    opens nothing yet: opening happens at the end of this function, after
    //    the bounded wait below, so a requested timeout can never expire during
    //    initialization (see `WatchdogCheckpoint.h`).
    watchdog.begin(millis());

    // 3. The serial port, last and after the hardware is already safe. The wait
    //    for a host is the one bounded initialization wait the nonblocking
    //    contract allows, and it is optional: the lines staged by the runtime are
    //    pumped by the loop whether or not a host is attached now.
    evhw::serialTransport::begin(kSerialBaud);
    const unsigned long startMs = millis();
    while (!Serial && millis() - startMs < kSerialWaitTimeoutMs) {
    }

    // 4. The watchdog, opened last and only when enablement requests it. This is
    //    the one place the timer is started: everything that could wait during
    //    initialization is already behind us, so the gap until the first pass's
    //    refresh is small and bounded, and the interval the watchdog actually
    //    covers is the loop's own cadence from that first pass onwards. With
    //    enablement off - the shipped setting - this call does nothing at all;
    //    the checkpoint still counts passes for the bench.
    watchdog.openIfRequested(millis());
}

void loop() {
    const uint32_t nowMs = millis();

    const evhw::PassOutcome outcome = runtimeRef().runPass(nowMs);

    // The single watchdog health checkpoint. `refreshable()` is false only when a
    // control tick ran and did not complete in order. The stamp is read again
    // here - it is the refresh event's own time (R2/M2), not the pass-start
    // reading above: the checkpoint measures its inter-refresh gaps from the
    // instant the refresh is decided, at millisecond resolution, so a long
    // pass is measured as a long gap rather than hidden inside a stale stamp.
    watchdog.endOfPass(outcome.refreshable(), millis());

    // Low-priority work, after the checkpoint: the operator interface first (a
    // command's answer and the queued records), then the status LED. Neither can
    // hold up a tick, and both are serviced whether or not anything is attached.
    runtimeRef().serviceSerial(nowMs);

    // The onboard arming gesture, then the status indicator that reports its
    // outcome. Neither can command motion: preparing a run only reaches the
    // motor through the armed control tick, and the LED is an output only.
    serviceStandaloneArming(nowMs);
    updateStatusLed(nowMs);
}
