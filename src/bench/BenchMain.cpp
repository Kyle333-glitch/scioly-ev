#include <Arduino.h>

#include <IntervalTimer.h>

#include "BenchConsole.h"

// ---------------------------------------------------------------------------
// The bench image's entry point (B1-B4) - not the vehicle's
// ---------------------------------------------------------------------------
//
// Why this file exists instead of a flag inside `main.cpp`
//
//   The bench tree must be impossible to include in a production image, and that
//   guarantee has to live in the build graph rather than in a macro: a macro that
//   is merely absent from the production build could be defined by accident, while
//   a source file the production environment does not compile cannot run at all.
//   So `env:nano_r4` compiles `src/` without `bench/` and this file is not in it,
//   and `env:bench` compiles `src/` without the production `main.cpp` and adds
//   this one (`platformio.ini`). Neither image contains the other's entry point.
//
// What it keeps from the production entry point, deliberately
//
//   * The same boot order, for the same reason: the motor driver is de-energized
//     first, before the encoder, the inputs or the serial port exist to ask for
//     anything; the watchdog checkpoint is recorded; the serial port comes last;
//     and the watchdog is opened at the very end, after the one bounded wait, so
//     its first refresh cannot be late (`WatchdogCheckpoint.h`).
//   * The same loop order around the control pass: the pass, then the single
//     watchdog checkpoint, then the operator output, then the low-priority jobs.
//
// What it adds, and where
//
//   The bench console reads its own lines (so it can recognize the `bench`
//   verbs). Whether the bench may write the motor pins is decided **before** the
//   tick (`benchConsole::beginPass`, which asks `ev::benchApplyVerdict()`); the
//   write itself is applied **after** the pass, after the tick has written the
//   authority's own command. That ordering is the backstop: a bench command cannot
//   outlive the bench code, because the production side keeps writing the
//   de-energized state behind it - the tick's own stage-9 write on passes where
//   a tick runs and the bench does not own the pass, and `beginPass()`/
//   `applyOutput()`'s direct de-energized write on the pass the instrument
//   stops holding the driver (its expiry, stall latch or revocation), which
//   covers exactly the passes the tick does not: tickless passes, and passes
//   the bench owns (whose tick write is deliberately skipped). And while the
//   bench owns the driver its write is the pass's only one, so the pins are
//   not commanded asleep and awake once per control period.
//
//   It also carries H0W Step 3's deliberate hang hook, below. It is a compile-time
//   constant on purpose: a hang that any serial line could trigger is a hang that a
//   stray line triggers, and hanging is the one thing this image does that no
//   command should be able to do by accident.
//
// This image is a test rig. Nothing it reports is evidence about the vehicle until
// a protocol records it (`docs/bringup/README.md`).

namespace {

constexpr unsigned long kSerialBaud = 115200;
constexpr uint32_t kSerialWaitTimeoutMs = 2000;
constexpr uint32_t kStatusLedIntervalMs = 500;

evhw::VehicleRuntime runtime;
evhw::WatchdogCheckpoint watchdog;
IntervalTimer statusLedTimer(kStatusLedIntervalMs);
bool statusLedState = false;

void updateStatusLed(uint32_t nowMs) {
    if (statusLedTimer.elapsed(nowMs)) {
        statusLedState = !statusLedState;
        digitalWrite(LED_BUILTIN, statusLedState ? HIGH : LOW);
    }
}

// ---------------------------------------------------------------------------
// The deliberate hang hook - H0W Step 3's instrument
// ---------------------------------------------------------------------------
//
// `P-H0W-watchdog-qualification.md` Step 3 requires the main loop to be stalled on
// purpose: nothing else in the firmware refreshes the watchdog, so a hung loop
// stops refreshing, and the watchdog's reset is then observed. This is that
// instrument, and it is the only way to produce that condition.
//
// Set `kDeliberateHangAfterPasses` to a non-zero number of completed loop passes to
// hang this loop inside a pass, after that many more passes have finished. Zero is
// the shipped value: the hook is inert and this image behaves exactly as before.
//
// It is a compile-time constant, and deliberately not a console verb: changing it
// is a rebuild, which is the deliberate act H0W describes, and no serial input can
// hang the board by accident. It lives in `src/bench/`, which the production
// environment does not compile (`platformio.ini`), so the vehicle image cannot
// contain it.
//
// **The motor supply must be off and disconnected before this is set non-zero.**
// H0W runs with the motor supply off; a hang with effort applied would be a stall
// with the only bounded-expiry path (the loop) stopped. The bounded expiry in
// `ev::BenchDrive` is evaluated by the loop, so a hang suspends it too - which is
// exactly why H0W's hang step is not a powered step.
// H0W Step 3: set to a non-zero pass count to hang the loop. NOTE the unit: a
// "pass" here is one loop *iteration*, and the loop is free-running (~46,000/s
// measured, D26) - it is not a 20 ms control tick. `460000U` therefore hangs
// about **10 s** after the first pass, not 2.5 hours. `0U` is the shipped value
// and the one this tree is left at; the hang campaign set it to 460000 and put it
// back (D27).
constexpr uint32_t kDeliberateHangAfterPasses = 0U;

uint32_t hangPassesRemaining = 0U;
bool hangCountdownStarted = false;
// The spin counter keeps the endless loop below from being optimized away, and its
// value is visible to a debugger. Nothing here reads a clock or touches a timer.
volatile uint32_t hangSpinCount = 0U;

// Never returns, and feeds no watchdog: no I/O, no clock, no timer.
void hangForever() {
    for (;;) {
        ++hangSpinCount;
    }
}

// Called once per completed pass. Counts down and, at zero, stages a record of the
// pass the hang happened at and hangs.
void serviceDeliberateHang() {
    if (kDeliberateHangAfterPasses == 0U) {
        return;
    }
    if (!hangCountdownStarted) {
        hangCountdownStarted = true;
        hangPassesRemaining = kDeliberateHangAfterPasses;
    }
    if (hangPassesRemaining > 0U) {
        --hangPassesRemaining;
        return;
    }
    evhw::benchConsole::reportDeliberateHang(kDeliberateHangAfterPasses);
    hangForever();
}

}  // namespace

void setup() {
    // The status LED is the only peripheral configured before the vehicle layer,
    // and it cannot move anything.
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);

    // 1. The vehicle layer's safe initialization, in its own order: the driver is
    //    de-energized before anything else exists to ask for motion.
    runtime.begin(millis());

    // 2. The watchdog checkpoint's boot instant. It opens nothing here; see step
    //    4 below.
    watchdog.begin(millis());

    // 3. The serial port, last of the hardware, with the one bounded wait this
    //    firmware allows.
    evhw::serialTransport::begin(kSerialBaud);
    const unsigned long startMs = millis();
    while (!Serial && millis() - startMs < kSerialWaitTimeoutMs) {
    }

    // 4. The watchdog, opened at the end of initialization and only when
    //    enablement requests it - which for H0W is what the bench build is for.
    watchdog.openIfRequested(millis());
}

void loop() {
    const uint32_t nowMs = millis();

    // Decided before the tick, on purpose: the arbitration says whether the bench
    // may hold the motor pins for this pass, and one writer per pass is the point
    // (`BenchConsole.h`, `MotorDriver.h`). It also advances the bounded command's
    // own expiry and stall watch.
    evhw::benchConsole::beginPass(nowMs, runtime);

    // One pass of the production control chain, unchanged: this image runs the
    // same scheduler, the same frozen tick order and the same safety boundary as
    // the vehicle image. What the bench adds is instrumentation around it, not a
    // different chain. The tick's own motor write yields to the bench while the
    // bench holds ownership, and de-energizes the driver otherwise.
    const evhw::PassOutcome outcome = runtime.runPass(nowMs);

    // The single watchdog checkpoint, as in production.
    watchdog.endOfPass(outcome.refreshable(), nowMs);

    // Low-priority work, in the production order: the operator's own output first,
    // then the console's read and dispatch, then the telemetry drain - so an
    // operator's answer outranks the log, and a bench verb outranks it too.
    runtime.serviceStagedText(nowMs);
    evhw::benchConsole::service(nowMs, runtime, watchdog);

    // The bounded bench command goes to the driver last - after the tick's own
    // write - and only on a pass the arbitration granted. `bench stop` and `bench
    // reset` take effect in the pass they are read, through this write.
    evhw::benchConsole::applyOutput(outcome);

    runtime.drainTelemetry();

    updateStatusLed(nowMs);

    // H0W Step 3's hook. Inert unless `kDeliberateHangAfterPasses` is set.
    serviceDeliberateHang();
}
