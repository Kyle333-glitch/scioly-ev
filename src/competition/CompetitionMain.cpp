#include <Arduino.h>

#include <StandaloneArming.h>
#include <StandaloneIndicator.h>

#include "../SerialTransport.h"
#include "../VehicleRuntime.h"
#include "../WatchdogCheckpoint.h"

// The competition image must never receive the bench-only watchdog-enablement
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
// Competition firmware entry point (`env:nano_r4-competition`)
// ---------------------------------------------------------------------------
//
// Preserves the full closed-loop competition controller, `VehicleRuntime`,
// `OperatorPolicy`, `ConfigurationBuildChecks`, `RunExecutionPlan`, and
// `SafetyAuthority`.
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
//      (`runtime.serviceSerial`) and the status LED.

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
    standaloneArming.reportPreparation(result.armed);
}

void updateStatusLed(uint32_t nowMs) {
    const ev::StandaloneIndicatorState state = ev::productionIndicatorState(
        runtimeRef().runPhase(), runtimeRef().armed(), runtimeRef().faultLatched(),
        lastTriggerVerdict.inactiveQualified);
    digitalWrite(LED_BUILTIN, ev::standaloneIndicatorLedOn(state, nowMs) ? HIGH : LOW);
}

}  // namespace

void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);

    runtimeRef().begin(millis());

    watchdog.begin(millis());

    evhw::serialTransport::begin(kSerialBaud);
    const unsigned long startMs = millis();
    while (!Serial && millis() - startMs < kSerialWaitTimeoutMs) {
    }

    watchdog.openIfRequested(millis());
}

void loop() {
    const uint32_t nowMs = millis();

    const evhw::PassOutcome outcome = runtimeRef().runPass(nowMs);

    watchdog.endOfPass(outcome.refreshable(), millis());

    runtimeRef().serviceSerial(nowMs);

    serviceStandaloneArming(nowMs);
    updateStatusLed(nowMs);
}
