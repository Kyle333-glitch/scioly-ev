#pragma once

#include <Arduino.h>

#include <BenchCommand.h>
#include <BenchDrive.h>
#include <BoundedText.h>
#include <Motion.h>
#include <MotorArbitration.h>
#include <SerialCommandParser.h>
#include <TelemetryRecord.h>

#include "../EncoderInput.h"
#include "../MotorDriver.h"
#include "../PinMap.h"
#include "../SensorInputs.h"
#include "../SerialTransport.h"
#include "../VehicleRuntime.h"
#include "../WatchdogCheckpoint.h"

namespace evhw {
namespace benchConsole {

// ---------------------------------------------------------------------------
// The bench console - the only motion-capable code outside the production stack
// ---------------------------------------------------------------------------
//
// What this is
//
// * The command surface of bench instruments B1-B4 (`docs/bringup/README.md`):
//   the loop/watchdog health readout (B1), the bounded diagnostic drive (B2), the
//   raw encoder tick readout (B3) and the raw trigger readout (B4). Each of those
//   is something a bring-up protocol needs to see and the production image
//   deliberately cannot report.
// * Bench-only by construction. This file lives in `src/bench/`, which the
//   production environment does not compile (`platformio.ini`: `env:nano_r4`
//   excludes `<bench/>`, `env:bench` excludes the production `main.cpp` and
//   includes this tree). The guarantee is in the build graph, not in a macro - a
//   macro that is merely absent could be defined by accident, while a source tree
//   that is not compiled cannot run at all.
//
// The safety model, in the order it is enforced
//
//   1. `ev::BenchDrive` decides what may be applied: a deliberate arm, a bounded
//      duration, effort and direction ceilings, a per-arm command budget, and a
//      latched fault on abort or on an encoder that stops changing
//      (`lib/evcore/src/BenchDrive.h`). Every one of those is host-tested.
//   2. `ev::benchApplyVerdict()` (`lib/evcore/src/MotorArbitration.h`, pure and
//      host-tested) decides whether the bench may write the driver at all, and it
//      is asked **before** the control tick runs: a latched fault, a violated
//      control-tick order, an armed run and a run under way each outrank the bench
//      and abort the instrument. `!armed()` is deliberately *not* the test - a
//      latched fault clears arming and an order violation disarms, so the states
//      that most need the motor off are exactly the ones `!armed()` cannot tell
//      from a free motor.
//   3. While the verdict permits it, the bench owns the driver for the pass
//      (`motorDriver::setMotorPinsOwned()`), so the tick's own write is skipped and the
//      three pins see one writer's intent instead of a sleep/wake toggle once per
//      control period. The bench's write happens **after** the tick; ownership is
//      released on the next pass, or immediately if the tick ran out of order, and
//      the tick resumes writing - so a bench command can never outlive the bench
//      code, and a pass that failed its stage order never has a bench command
//      written over its fail-safe write.
//   4. The physical kill switch - the motor-supply disconnect - remains the
//      primary stop. Nothing here replaces it, and no protocol that uses it may
//      run with the switch out of reach.
//
// What is deliberately not here
//
// * No command that resumes anything after boot or reset: `bench arm` is required
//   in every session, and `bench reset` clears the latch and leaves the
//   instrument disarmed.
// * No `bench disable` that silently keeps driving: the abort latches.
// * No attempt to interpret what the bench measures. It reports raw counts and
//   levels; what they mean is decided by the protocol that recorded them.
//
// Line format
//
//   Every line this console emits starts with `BENCH`. The telemetry analyzer
//   treats a line that does not start with the telemetry root as an ignored line
//   (`tools/telemetry/evtelemetry/parse.py`), so bench chatter cannot be mistaken
//   for control telemetry or corrupt an analysis.

// ---------------------------------------------------------------------------
// Bench policy - bench choices, not vehicle facts
// ---------------------------------------------------------------------------
//
// These are limits for a first powered test on a bench, not measurements and not
// tuning. None of them may be promoted into `VehicleConfig.h`: that file is about
// the vehicle, and these are about how much a test rig is allowed to ask for. H3
// records what was used (`docs/bringup/P-H3-first-low-duty-drive.md`), and a later
// protocol may change them - deliberately, in one place, with the reason recorded.
namespace benchConfig {

// The highest effort any bench command may ask for, permille of full scale. Small
// on purpose: the first powered test is the first time the drivetrain moves.
constexpr uint16_t kMaxEffortPermille = 150U;

// The longest a single bench command may last, milliseconds.
constexpr uint32_t kMaxCommandDurationMs = 500U;

// How long the encoder may show no change while commanded to drive before the
// command is aborted as a latched fault. Wide relative to the control period: a
// very slow rotation still produces edges well inside this, and a false abort is
// a safe-direction annoyance rather than a hazard. H3 records whether it was
// adequate at the smallest duty used.
constexpr uint32_t kStallWindowMs = 1000U;

// Commands allowed per arm. Not a rate limit: a bound on how long a confused
// console can keep asking for effort without a fresh, deliberate arm.
constexpr uint32_t kMaxCommandsPerArm = 8U;

// How long the bench trigger readout holds a level change, so a momentary contact
// is visible on the next read rather than only in a log at the exact instant.
// (Recorded here as policy; the level itself is always the live pin.)
constexpr uint32_t kReadoutLineCapacity = ev::kSerialLineCapacity;

inline ev::BenchDriveLimits driveLimits() {
    return ev::BenchDriveLimits(kMaxEffortPermille, kMaxCommandDurationMs, kStallWindowMs,
                                kMaxCommandsPerArm);
}

}  // namespace benchConfig

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

namespace detail {

// The one bench drive instrument. Static storage, like every other module-owned
// object in this firmware, and reached only through the functions below.
inline ev::BenchDrive& drive() {
    static ev::BenchDrive instrument(benchConfig::driveLimits());
    return instrument;
}

// The console's own line reader, separate from the runtime's. The runtime's is
// private and belongs to the operator interface; this one exists so the bench can
// decide what a line is before handing the rest to that interface.
inline char* lineBuffer() {
    static char buffer[ev::kCommandLineCapacity + 1U];
    return buffer;
}

inline ev::CommandLineReader& lineReader() {
    static ev::CommandLineReader reader(lineBuffer(), ev::kCommandLineCapacity);
    return reader;
}

// Counters of the console's own outcomes: evidence for a bench session about
// whether the interface itself is doing what the operator expects.
inline uint32_t& acceptedLines() {
    static uint32_t count = 0U;
    return count;
}

inline uint32_t& refusedLines() {
    static uint32_t count = 0U;
    return count;
}

inline uint32_t& lostResponses() {
    static uint32_t count = 0U;
    return count;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------
//
// There is none here. Splitting the line, recognizing the verb and parsing the
// one verb that takes arguments are `lib/evcore/src/BenchCommand.h`, which is pure
// and host-tested (`test_bench_command`): the parts of this console that can be
// wrong in ways a bench session would not notice are the parts that can be tested
// without a bench. What remains below is dispatch and I/O.

// ---------------------------------------------------------------------------
// Answers
// ---------------------------------------------------------------------------

// A signed count, written as a sign followed by the magnitude. `BoundedText` has
// no signed writer on purpose - every fixed-point field in a telemetry record is a
// magnitude with a unit - so the one place a raw signed count is printed says so
// itself. INT32_MIN is negated in 64 bits, so it cannot overflow.
inline void appendSignedTicks(ev::BoundedText& text, int32_t value) {
    if (value < 0) {
        text.appendChar('-');
        const int64_t magnitude = -static_cast<int64_t>(value);
        text.appendUnsigned(static_cast<uint32_t>(magnitude));
        return;
    }
    text.appendUnsigned(static_cast<uint32_t>(value));
}

// Stages one composed line. The transport is the production line transport, so
// bench answers share the port, the buffer and the drop discipline with the rest
// of the firmware: nothing here can block waiting for a host.
inline void respond(ev::BoundedText& text) {
    if (text.truncated()) {
        ++detail::lostResponses();
        return;
    }
    if (!serialTransport::port().stage(text.length(), true)) {
        ++detail::lostResponses();
        return;
    }
    serialTransport::port().pump();
}

inline void refuseLine(const char* reason) {
    ev::BoundedText text(serialTransport::port().workingBuffer(),
                         benchConfig::kReadoutLineCapacity);
    text.appendText("BENCH,refused=1");
    text.field("reason").appendText(reason);
    respond(text);
}

inline void ackLine(const char* verb) {
    ev::BoundedText text(serialTransport::port().workingBuffer(),
                         benchConfig::kReadoutLineCapacity);
    text.appendText("BENCH,ack=1");
    text.field("cmd").appendText(verb);
    respond(text);
}

// B1 - the loop and watchdog health readout.
//
// `passes`, `refreshes` and `withheld` are the checkpoint's counters; `opened`
// says whether the timer accepted the request, and `open_refused` makes a refused
// open visible rather than silent. `ticks`, `dt_ms`, `late_ms` and `dropped` are
// the scheduler's own numbers from the runtime's last snapshot, which is what H0
// and H0W record for cadence, and `first_refresh_ms` is the measured
// boot-to-first-refresh gap the timeout choice has to cover.
inline void healthLine(const VehicleRuntime& runtime, const WatchdogCheckpoint& watchdog) {
    const TelemetrySnapshot& snapshot = runtime.telemetry();
    ev::BoundedText text(serialTransport::port().workingBuffer(),
                         benchConfig::kReadoutLineCapacity);
    text.appendText("BENCH,health=1");
    text.field("image").appendText("bench");
    text.field("enable").appendBool(watchdog.enablementRequested());
    text.field("open_attempted").appendBool(watchdog.openAttempted());
    text.field("opened").appendBool(watchdog.opened());
    text.field("open_refused").appendBool(watchdog.openRefused());
    text.field("timeout_ms").appendUnsigned(watchdog.requestedTimeoutMs());
    text.field("passes").appendUnsigned(watchdog.passes());
    text.field("refreshes").appendUnsigned(watchdog.refreshes());
    text.field("withheld").appendUnsigned(watchdog.withheldRefreshes());
    text.field("last_refresh_ms").appendUnsigned(watchdog.lastRefreshMs());
    text.field("first_refresh_ms").appendUnsigned(watchdog.timeToFirstRefreshMs());
    text.field("control_ticks").appendUnsigned(snapshot.controlTicks);
    text.field("dt_ms").appendUnsigned(snapshot.dtMs);
    text.field("late_ms").appendUnsigned(snapshot.lateByMs);
    text.field("dropped").appendUnsigned(snapshot.droppedDeadlines);
    text.field("violations").appendUnsigned(snapshot.orderViolations);
    text.field("armed").appendBool(runtime.armed());
    // The console's own interface counters, so a session can tell "I typed it and
    // nothing happened" from "it was refused, and why" without re-reading the log.
    text.field("lines_accepted").appendUnsigned(detail::acceptedLines());
    text.field("lines_refused").appendUnsigned(detail::refusedLines());
    text.field("responses_lost").appendUnsigned(detail::lostResponses());
    respond(text);
}

// B2 - the bench drive state, and the answer to a command.
inline void driveLine() {
    const ev::BenchDrive& instrument = detail::drive();
    ev::BoundedText text(serialTransport::port().workingBuffer(),
                         benchConfig::kReadoutLineCapacity);
    text.appendText("BENCH,drive=1");
    text.field("state").appendText(ev::benchDriveStateName(instrument.state()));
    text.field("status").appendText(ev::benchDriveStatusName(instrument.status()));
    text.field("mode").appendText(ev::telemetryMotorModeName(instrument.output().mode()));
    text.field("effort_permille").appendUnsigned(instrument.output().magnitudePermille());
    text.field("fault").appendBool(instrument.faultLatched());
    text.field("commands").appendUnsigned(instrument.commandsIssued());
    text.field("stalls").appendUnsigned(instrument.stallFaults());
    text.field("ceiling_permille").appendUnsigned(benchConfig::kMaxEffortPermille);
    text.field("max_duration_ms").appendUnsigned(benchConfig::kMaxCommandDurationMs);
    text.field("stall_window_ms").appendUnsigned(benchConfig::kStallWindowMs);
    text.field("budget").appendUnsigned(benchConfig::kMaxCommandsPerArm);
    respond(text);
}

// B3 - the raw encoder readout. Raw signed ticks and the decoder's own progress
// counters, which is what H2, H3, H4 and H5 need and what the telemetry sample
// deliberately does not carry (it carries a distance that needs a scale nobody has
// measured yet).
inline void ticksLine() {
    const encoderInput::TickSnapshot ticks = encoderInput::snapshotTicks();
    ev::BoundedText text(serialTransport::port().workingBuffer(),
                         benchConfig::kReadoutLineCapacity);
    text.appendText("BENCH,ticks=1");
    text.field("raw").appendText("1");
    text.field("ticks_raw");
    appendSignedTicks(text, ticks.ticks);
    text.field("counted_edges").appendUnsigned(ticks.countedEdges);
    text.field("ignored_updates").appendUnsigned(ticks.ignoredUpdates);
    respond(text);
}

// B4 - the raw input readout. Levels and raw counts, never engineering units:
// the active level of the trigger, what the driver's fault line is doing, and the
// two ADC channels as counts. This is what H1B records.
inline void inputsLine() {
    const sensorInputs::RawSample raw = sensorInputs::sample();
    ev::BoundedText text(serialTransport::port().workingBuffer(),
                         benchConfig::kReadoutLineCapacity);
    text.appendText("BENCH,inputs=1");
    text.field("trigger_low").appendBool(raw.startTriggerActive);
    text.field("nfault_low").appendBool(raw.driverFaultBarLow);
    text.field("batt_counts").appendUnsigned(raw.batteryCounts);
    text.field("curr_counts").appendUnsigned(raw.currentCounts);
    respond(text);
}

inline void helpLine() {
    ev::BoundedText text(serialTransport::port().workingBuffer(),
                         benchConfig::kReadoutLineCapacity);
    text.appendText("BENCH,help=1");
    text.field("verbs")
        .appendText("health drive arm stop reset ticks inputs");
    respond(text);
}

// ---------------------------------------------------------------------------
// The bench verbs
// ---------------------------------------------------------------------------

// `bench arm` - the deliberate arm. Refused while an abort is latched, which is
// what makes "stop" mean stopped until someone looks at why.
inline void applyArm() {
    if (!detail::drive().arm()) {
        refuseLine("ArmRefusedFaultLatched");
        return;
    }
    ackLine("bench_arm");
}

// `bench drive <fwd|rev> <permille> <ms>` - one bounded command. Whether the line
// is well formed is `ev::parseBenchDriveRequest()`'s decision (host-tested);
// whether the request is within the bench ceilings is `ev::BenchDrive`'s, so the
// reason an operator sees always names the limit that refused it.
inline void applyDrive(const ev::BenchTokens& tokens, uint32_t nowMs) {
    const ev::BenchDriveRequest request = ev::parseBenchDriveRequest(tokens);
    if (request.error != ev::BenchArgumentError::None) {
        refuseLine(ev::benchArgumentErrorName(request.error));
        return;
    }

    const ev::BenchCommandRefusal refusal =
        detail::drive().command(nowMs, request.mode, request.permille, request.durationMs);
    if (refusal != ev::BenchCommandRefusal::None) {
        refuseLine(ev::benchCommandRefusalName(refusal));
        return;
    }

    ++detail::acceptedLines();
    driveLine();
}

// `bench stop` - the abort. De-energizes now and latches until `bench reset`.
inline void applyStop() {
    detail::drive().abort();
    ackLine("bench_stop");
    driveLine();
}

// `bench reset` - the deliberate exit from a latch. Leaves the instrument
// disarmed: re-arming is a separate decision.
inline void applyReset() {
    detail::drive().reset();
    ackLine("bench_reset");
    driveLine();
}

// One complete line, dispatched. A line that is not a bench verb is handed back to
// the production operator interface, so `status`, `checks`, `help` and the
// configuration verbs keep working in a bench session and a bench image cannot
// grow a second, diverging copy of them.
inline void handleLine(const char* line, uint16_t length, uint32_t nowMs,
                       VehicleRuntime& runtime, const WatchdogCheckpoint& watchdog) {
    const ev::BenchTokens tokens = ev::splitBenchTokens(line, length);
    if (tokens.count == 0U) {
        return;  // an empty line answers nothing, exactly as the operator path does
    }

    const ev::BenchVerb verb = ev::benchVerbFor(tokens);
    if (verb == ev::BenchVerb::None) {
        // Not a bench line: the production operator interface owns it, so `status`,
        // `checks`, the configuration verbs and the telemetry verbs keep working in
        // a bench session rather than being reimplemented here.
        runtime.handleCommandLine(line, length, nowMs);
        return;
    }

    // An over-long line is refused before dispatch: acting on the tokens that fit
    // is how a command nobody typed gets applied.
    if (tokens.overflow) {
        ++detail::refusedLines();
        refuseLine("TooManyTokens");
        return;
    }

    switch (verb) {
        case ev::BenchVerb::Help:
            helpLine();
            return;
        case ev::BenchVerb::Arm:
            applyArm();
            return;
        case ev::BenchVerb::Drive:
            applyDrive(tokens, nowMs);
            return;
        case ev::BenchVerb::Stop:
            applyStop();
            return;
        case ev::BenchVerb::Reset:
            applyReset();
            return;
        case ev::BenchVerb::Health:
            healthLine(runtime, watchdog);
            return;
        case ev::BenchVerb::DriveStatus:
            driveLine();
            return;
        case ev::BenchVerb::Ticks:
            ticksLine();
            return;
        case ev::BenchVerb::Inputs:
            inputsLine();
            return;
        case ev::BenchVerb::Unknown:
        case ev::BenchVerb::None:
            break;
    }

    ++detail::refusedLines();
    refuseLine("UnknownBenchVerb");
}

// ---------------------------------------------------------------------------
// The three things the entry point calls
// ---------------------------------------------------------------------------

// Reads what the host sent, bounded per pass, and applies at most one line - the
// same discipline the operator interface uses, for the same reason: one answer per
// pass, and no path that waits for a host.
inline void service(uint32_t nowMs, VehicleRuntime& runtime,
                    const WatchdogCheckpoint& watchdog) {
    uint8_t budget = serialTransport::kReadBytesPerPass;
    while (budget > 0U) {
        --budget;
        const int16_t received = serialTransport::port().readByte();
        if (received < 0) {
            break;
        }
        const ev::LineEvent event =
            detail::lineReader().push(static_cast<char>(received));
        if (event == ev::LineEvent::LineReady) {
            handleLine(detail::lineReader().line(), detail::lineReader().lineLength(), nowMs,
                       runtime, watchdog);
            break;  // one line per pass: the rest of the window waits
        }
        if (event == ev::LineEvent::LineTooLong) {
            ++detail::refusedLines();
            refuseLine("LineTooLong");
            break;
        }
        if (event == ev::LineEvent::InvalidCharacter) {
            ++detail::refusedLines();
            refuseLine("InvalidCharacter");
            break;
        }
        // EmptyLine: a bare terminator carries no command and is not an answer.
    }
}

// What this pass decided about the motor pins.
struct PassArbitration {
    // The bench holds the driver for this pass: the tick's write is skipped and the
    // bench's write happens after the tick.
    bool ownsMotor = false;
    // The verdict behind that decision, for the health line and the tests.
    ev::BenchApplyRefusal refusal = ev::BenchApplyRefusal::None;
};

namespace detail {
inline PassArbitration& passArbitration() {
    static PassArbitration arbitration;
    return arbitration;
}
}  // namespace detail

// Decides, before the control tick runs, whether the bench owns the motor for this
// pass. Call once per pass, immediately before `runtime.runPass()`.
//
// It also advances the instrument (`BenchDrive::update()`), so the expiry and the
// encoder-stall fault are evaluated from this pass's clock and count, and it sets
// or clears `motorDriver::setMotorPinsOwned()`.
//
// When the instrument is driving and the verdict refuses it, the instrument is
// **aborted** - latched, not silently disarmed - so the bench cannot quietly
// resume applying effort once the blocking condition clears. The operator sees the
// refusal and the reason.
inline void beginPass(uint32_t nowMs, const VehicleRuntime& runtime) {
    ev::BenchDrive& instrument = detail::drive();
    const ev::BenchDriveStatus status =
        instrument.update(nowMs, encoderInput::snapshotTicks().ticks);

    // A lapsed command is reported once, when it lapses, so a session's log shows
    // the expiry rather than only the absence of motion.
    if (status == ev::BenchDriveStatus::Expired ||
        status == ev::BenchDriveStatus::EncoderStallFault) {
        driveLine();
    }

    const ev::BenchApplyVerdict verdict =
        ev::benchApplyVerdict(instrument.output().isDriving(), runtime.armed(),
                              runtime.faultLatched(), runtime.runUnderway(),
                              runtime.orderViolationDetected());

    PassArbitration arbitration;
    arbitration.refusal = verdict.refusal;
    arbitration.ownsMotor = verdict.mayApply;
    if (instrument.output().isDriving() && !verdict.mayApply) {
        instrument.abort();
        arbitration.ownsMotor = false;
        refuseLine(ev::benchApplyRefusalName(verdict.refusal));
    }

    motorDriver::setMotorPinsOwned(arbitration.ownsMotor);
    detail::passArbitration() = arbitration;
}

// Writes the bench command after the tick, when the bench owns the motor, and
// releases ownership either way. Call once per pass, immediately after the tick's
// low-priority work (see `BenchMain.cpp` for the frozen order).
//
// Three cases, and only three:
//
//   * the pass's tick ran out of order (`!outcome.refreshable()`): the tick has
//     already written the de-energized state and disarmed. The instrument is
//     aborted and the bench releases the motor - a firmware whose control cycle ran
//     wrongly does not get a bench command applied over its fail-safe write;
//   * the bench owns the motor: this write is the pass's only write to the pins.
//     `output()` is the drive while a command is applied and the de-energized state
//     the instant it lapses, is aborted or is reset, so an expiry, a `bench stop`
//     or a `bench reset` reaches the pins in the same pass it is decided;
//   * otherwise: no write at all. The tick's write was the pass's write.
inline void applyOutput(const PassOutcome& outcome) {
    ev::BenchDrive& instrument = detail::drive();
    const PassArbitration arbitration = detail::passArbitration();

    if (!outcome.refreshable()) {
        if (arbitration.ownsMotor || instrument.output().isDriving()) {
            instrument.abort();
            refuseLine("OrderViolation");
        }
        motorDriver::setMotorPinsOwned(false);
        return;
    }

    if (arbitration.ownsMotor) {
        motorDriver::applyBench(instrument.output());
    }
    motorDriver::setMotorPinsOwned(false);
}

// The line H0W Step 3's deliberate hang stages immediately before it hangs, so a
// capture shows the pass the hang happened at (the last `bench health` readout may
// be older).
inline void reportDeliberateHang(uint32_t passesBeforeHang) {
    ev::BoundedText text(serialTransport::port().workingBuffer(),
                         benchConfig::kReadoutLineCapacity);
    text.appendText("BENCH,hang=1");
    text.field("image").appendText("bench");
    text.field("after_passes").appendUnsigned(passesBeforeHang);
    respond(text);
}

}  // namespace benchConsole
}  // namespace evhw
