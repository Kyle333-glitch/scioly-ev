#pragma once

#include <Arduino.h>

#include <BenchCommand.h>
#include <BenchDrive.h>
#include <BenchLineAccounting.h>
#include <BoundedText.h>
#include <DriverFault.h>
#include <Motion.h>
#include <MotorArbitration.h>
#include <SerialCommandParser.h>
#include <TelemetryRecord.h>

#include "ResetCause.h"
#include "WatchdogRegisters.h"

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
//      written over its fail-safe write. A pass on which the instrument stops
//      holding the driver - its own expiry, its stall latch, or an arbitration
//      revocation - writes the de-energized state in that pass, before the pass
//      ends, rather than depending on a later control tick (M1).
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
// D35: reverted the temporary 300-permille tournament override back to 150; a
// higher ceiling needs a recorded reason, not a bring-up override.
constexpr uint16_t kMaxEffortPermille = 150U;

// The longest a single bench command may last, milliseconds.
constexpr uint32_t kMaxCommandDurationMs = 500U;

// How long the encoder may show no change while commanded to drive before the
// command is aborted as a latched fault.
//
// **400 ms, not the 1000 ms this shipped with (H3).** The bounded command is at
// most 500 ms, so a 1000 ms window could never elapse while effort was applied:
// the bounded expiry always fired first and the latch was unreachable for exactly
// the condition it exists to catch - a stalled drivetrain, an unwired encoder, or
// a command the drivetrain cannot follow. Lengthening the powered command was
// rejected (the powered duration is not a convenience), so the window came down
// instead. The margin below the 500 ms ceiling is 100 ms, five of the 20 ms
// control periods, which is room for the latch to be *observed* before the
// expiry with headroom for the loop's own lateness.
//
// It is a declared software budget, not a measured stopping behaviour and not a
// claim about any real rotor: it bounds how long a stalled or unwired motor may
// be driven, and a false abort at the smallest duty is a safe-direction annoyance
// rather than a hazard. Whether a healthy very slow rotation produces edges
// inside 400 ms is a physical question H3 answers at the bench, and that
// qualification is still outstanding.
constexpr uint32_t kStallWindowMs = 400U;

// Commands allowed per arm. Not a rate limit: a bound on how long a confused
// console can keep asking for effort without a fresh, deliberate arm.
constexpr uint32_t kMaxCommandsPerArm = 8U;

// The one limit set the console builds its instrument from, as a constexpr value,
// so the reachability relationship below is a compile-time property of the
// shipped numbers rather than a comment.
constexpr ev::BenchDriveLimits kDriveLimits(kMaxEffortPermille, kMaxCommandDurationMs,
                                            kStallWindowMs, kMaxCommandsPerArm);

// H3: the shipped stall window must be short enough to trip inside one command.
// An edit to either number that makes the latch unreachable again fails the build
// here, instead of silently disarming the protection.
static_assert(ev::benchStallProtectionReachable(kDriveLimits),
              "H3: the bench stall window must be shorter than the longest command");

#if defined(EV_BENCH_H1A_DIAGNOSTIC)
// TEMPORARY - the ceiling for the H1A awake-EN-low hold (`bench h1a-brake`).
// Longer than the drive ceiling on purpose: the hold is static - no effort, EN
// at zero duty - and it exists so a meter can be read without chasing a 500 ms
// window. It still expires on its own, and `bench stop` ends it.
constexpr uint32_t kH1aHoldMaxMs = 30000U;
#endif

// The capacity every readout in this console is composed against.
//
// One capacity rather than one per readout, because they all share the
// transport's line slots and every slot has the same length (`ev::LineFramer`,
// which holds one active and at most one waiting line since S15's M2 repair).
//
// **The derived worst case of the widest readout, `healthLine()`, is 562
// characters** - every `uint32` field at its full ten digits, which the
// free-running counters really can reach (`passes` needs roughly 26 h at the
// measured ~46,000 passes/s; `last_refresh_ms` reaches ten digits at the
// `millis()` wrap). That was **86 characters over** the 448 a previous shared
// capacity held. S8 raised the capacity to 576 for the telemetry sample's own
// derived worst case (567), so the readout now fits with 14 characters to spare -
// but the room is still finite and still shared, so the two reasons this number
// is recorded stand:
//
//   * `stageLostReadoutNotice()` - a readout that does not fit is dropped whole
//     and counted (`respond()`), and the counter that would say so,
//     `responses_lost`, is a field of the health line itself; the notice keeps
//     that loss visible on the wire rather than only in a counter that travelled
//     on the line that was lost;
//   * this number, recorded so the next field added has to look at it. The owed
//     fix is to split `health` (the watchdog/loop half and the console/interface
//     half), not to trim the schema. That is a clarity/schema matter now rather
//     than a loss-prevention one.
//
// Note also the shape of the headroom: the non-obvious part is that it is
// *unbounded* growth, not a fixed gap. Every other consumer of a shared line slot
// states its worst case against it
// (`ev::kOperatorResponseWorstCaseChars`, `lib/evcore/src/OperatorResponse.h`);
// this console used to state none, which is why the widest line in the firmware
// was the one with no bound behind it.
constexpr uint32_t kReadoutLineCapacity = ev::kSerialLineCapacity;

inline ev::BenchDriveLimits driveLimits() { return kDriveLimits; }

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
// whether the interface itself is doing what the operator expects. The
// contract is `ev::BenchLineCounters` (`lib/evcore/src/BenchLineAccounting.h`):
// every answered line is recorded exactly once - accepted, refused or
// forwarded - at the single dispatch choke points (`handleLine()` for lines,
// `service()` for framing errors), never in the leaf verbs. `received()` is
// derived (accepted + refused + forwarded); empty lines are excluded because
// they receive no answer, exactly as the operator path treats them.
inline ev::BenchLineCounters& lineCounters() {
    static ev::BenchLineCounters counters;
    return counters;
}

// The one recording choke point for dispatched lines. Exactly one call per
// answered line; a verb that records anywhere else double-counts.
inline void recordLineOutcome(ev::BenchLineOutcome outcome) {
    lineCounters().record(outcome);
}

inline uint32_t& lostResponses() {
    static uint32_t count = 0U;
    return count;
}

#if defined(EV_BENCH_H1A_DIAGNOSTIC)
// TEMPORARY - H1A electrical-gate diagnostic (remove after the H1A evidence).
// The state of the bounded awake-EN-low hold; what it is and why it exists is
// documented at `applyH1aAwakeHold()` below.
inline bool& h1aHoldActive() {
    static bool active = false;
    return active;
}

inline uint32_t& h1aHoldExpiresAtMs() {
    static uint32_t expiresAtMs = 0U;
    return expiresAtMs;
}

inline uint32_t& h1aHoldsIssued() {
    static uint32_t count = 0U;
    return count;
}
#endif

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

// The tombstone a dropped readout leaves behind.
//
// `respond()` drops a readout that does not fit, whole, and counts it - and the
// counter it counts, `responses_lost`, is a field of the `health` readout, so a
// health line that did not fit would take its own evidence of the drop with it and
// the interface would look healthy while going silent (`kReadoutLineCapacity`
// above records how close that is). This notice is constant and short, so it fits
// whatever the schema does, and it is composed into the same working buffer the
// dropped readout was: that readout is gone by definition, and the buffer is the
// only line buffer there is.
//
// It is a **notice, not an answer**: the readout it replaces was not sent, and the
// fix is to shorten the schema, not to retry. It is deliberately not routed
// through `respond()` - the drop is already counted there, and a notice that
// could be dropped would be no notice at all.
inline void stageLostReadoutNotice() {
    ev::BoundedText notice(serialTransport::port().workingBuffer(),
                           benchConfig::kReadoutLineCapacity);
    notice.appendText("BENCH,lost=1");
    notice.field("reason").appendText("ReadoutDidNotFit");
    if (notice.truncated()) {
        return;  // constant and 36 characters: not reachable, and not silent
    }
    if (!serialTransport::port().stage(notice.length(), true)) {
        return;  // the transport is busy; the counter still records the drop
    }
    serialTransport::port().pump();
}

// Stages one composed line. The transport is the production line transport, so
// bench answers share the port, the buffer and the drop discipline with the rest
// of the firmware: nothing here can block waiting for a host.
//
// A readout that does not fit its line is dropped **whole** rather than sent
// short - a truncated line would hide the fields at its tail, and the tail is
// where a diagnosis lives - and `stageLostReadoutNotice()` leaves a visible trace
// of that in its place.
inline void respond(ev::BoundedText& text) {
    if (text.truncated()) {
        ++detail::lostResponses();
        stageLostReadoutNotice();
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
//
// `late_ms` and `dropped` describe the *last* control tick, so a single readout of
// them cannot answer "what was the worst case" (P-H0 Step 7 asks for exactly
// that). `worst_late_ms` and `dropped_total` are the session's running extremes
// over every tick since boot, from the scheduler's own counters, which is the
// pair a soak reports; read both twice to see the loop still making progress.
//
// `worst_refresh_gap_ms` is the same kind of extreme for the watchdog itself: the
// longest interval observed between two consecutive refreshes, which is the
// quantity H0W Step 2 asks for and the schedulers' lateness can only approximate
// (a refresh happens at the end of a pass, so its spacing is one pass wider than
// the tick lateness). It is recorded in the checkpoint, from the same `nowMs` the
// refresh already uses, so it adds a comparison per pass and no timer read - and
// it changes nothing about when the watchdog is refreshed.
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
    // The period the timer actually programmed, from the library's own
    // computation (`WDTimer::getTimeout()`, which returns the realized period of
    // the prescaler/reload pair it selected, rounded up from the request). This is
    // the number P-H0W Step 7 asks for: the requested `timeout_ms` is **not** the
    // realized period. It reports 0 when the timer was never opened, which
    // `opened` tells apart from a realized period of zero; the read is a pure
    // member read and touches no timer register.
    text.field("wdt_realized_ms").appendUnsigned(WDT.getTimeout());
    text.field("passes").appendUnsigned(watchdog.passes());
    text.field("refreshes").appendUnsigned(watchdog.refreshes());
    text.field("withheld").appendUnsigned(watchdog.withheldRefreshes());
    text.field("last_refresh_ms").appendUnsigned(watchdog.lastRefreshMs());
    text.field("first_refresh_ms").appendUnsigned(watchdog.timeToFirstRefreshMs());
    text.field("worst_refresh_gap_ms").appendUnsigned(watchdog.worstRefreshGapMs());
    text.field("control_ticks").appendUnsigned(snapshot.controlTicks);
    text.field("dt_ms").appendUnsigned(snapshot.dtMs);
    text.field("late_ms").appendUnsigned(snapshot.lateByMs);
    text.field("dropped").appendUnsigned(snapshot.droppedDeadlines);
    text.field("worst_late_ms").appendUnsigned(snapshot.worstLateByMs);
    text.field("worst_stall_ms").appendUnsigned(snapshot.worstStallMs);
    text.field("dropped_total").appendUnsigned(snapshot.droppedDeadlinesTotal);
    text.field("violations").appendUnsigned(snapshot.orderViolations);
    text.field("armed").appendBool(runtime.armed());
    // The console's own interface counters, so a session can tell "I typed it and
    // nothing happened" from "it was refused, and why" without re-reading the log.
    // `lines_forwarded` counts non-bench lines handed to the production
    // interface (whose own outcome the production counters own); it is neither
    // accepted nor refused here, so one command is never counted twice.
    text.field("lines_accepted").appendUnsigned(detail::lineCounters().accepted);
    text.field("lines_refused").appendUnsigned(detail::lineCounters().refused);
    text.field("lines_forwarded").appendUnsigned(detail::lineCounters().forwarded);
    text.field("responses_lost").appendUnsigned(detail::lostResponses());
    respond(text);
}

// B2 - the bench drive state, and the answer to a command.
// The direction this arm has fixed, for the `direction` field below: the fact the
// one-direction-per-arm refusal is made from, so a session can see *why* the other
// direction was refused without re-reading the source (H2).
inline const char* lockedDirectionName(ev::MotorMode mode) {
    if (mode == ev::MotorMode::DriveForward) {
        return "fwd";
    }
    if (mode == ev::MotorMode::DriveReverse) {
        return "rev";
    }
    return "none";
}

inline void driveLine() {
    const ev::BenchDrive& instrument = detail::drive();
    ev::BoundedText text(serialTransport::port().workingBuffer(),
                         benchConfig::kReadoutLineCapacity);
    text.appendText("BENCH,drive=1");
    text.field("state").appendText(ev::benchDriveStateName(instrument.state()));
    text.field("status").appendText(ev::benchDriveStatusName(instrument.status()));
    text.field("mode").appendText(ev::telemetryMotorModeName(instrument.output().mode()));
    text.field("effort_permille").appendUnsigned(instrument.output().magnitudePermille());
    text.field("direction").appendText(lockedDirectionName(instrument.armedDirection()));
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
// the active level of the trigger and the two ADC channels as counts. This is
// what H1B records.
//
// `nfault` and `nfault_low` say the truth about the driver's fault line rather
// than printing a level: the external nFAULT signal is **not installed** on this
// vehicle, so the readout reports `nfault=NotInstalled` and writes the level field
// as the missing marker. It deliberately does **not** call `digitalRead()` on
// `kMotorFaultBar` to "still show something" - a floating pin's level is not a
// measurement of anything, and a bench readout that printed it as `nfault_low=0`
// would be the single most misleading line this firmware could emit, because it
// would read as a healthy driver. `SensorInputs.h` is where the pin is left
// unconfigured and unsampled; this line reports that decision, it does not work
// around it.
//
// On a vehicle that wires the line, the same fields report `nfault=Installed` and
// the real level, which is what H1B Part 3 records.
inline void inputsLine() {
    const sensorInputs::RawSample raw = sensorInputs::sample();
    ev::BoundedText text(serialTransport::port().workingBuffer(),
                         benchConfig::kReadoutLineCapacity);
    text.appendText("BENCH,inputs=1");
    text.field("trigger_low").appendBool(raw.startTriggerActive);
    text.field("nfault").appendText(ev::driverFaultCapabilityName(raw.driverFaultCapability));
    text.field("nfault_low").appendText(
        ev::telemetryDriverFaultLevelName(raw.driverFaultCapability, raw.driverFaultBarLow));
    text.field("batt_counts").appendUnsigned(raw.batteryCounts);
    text.field("curr_counts").appendUnsigned(raw.currentCounts);
    respond(text);
}

// `bench pins` - the driver inputs as the MCU holds them (P-H0W Step 3).
//
// `source=pin_read` is part of the reading, not decoration: these are the levels
// the MCU drives as reported by its own IOPORT read (`digitalRead()` ->
// `R_IOPORT_PinRead()`), paired with the duty this adapter last wrote - a PWM duty
// cannot be read back from a pin. An external voltage measurement at the header is
// a different fact, owned by H1A. `deenergized=1` is `drv8874InputsAreDeEnergized`
// applied to those levels and that duty, i.e. the same rule the plan uses for a
// command, so a pin held the wrong way reports 0 instead of reading as safe.
inline void pinsLine() {
    const motorDriver::PinLevels levels = motorDriver::pinLevels();
    ev::BoundedText text(serialTransport::port().workingBuffer(),
                         benchConfig::kReadoutLineCapacity);
    text.appendText("BENCH,pins=1");
    text.field("source").appendText("pin_read");
    text.field("nsleep_high").appendBool(levels.nSleepHigh);
    text.field("phase_high").appendBool(levels.phaseHigh);
    text.field("enable_high").appendBool(levels.enableHigh);
    text.field("duty_permille_written").appendUnsigned(levels.dutyPermilleWritten);
    text.field("deenergized").appendBool(levels.deEnergized);
    respond(text);
}

// `bench resetsrc [clear]` - the reset-status registers, as the MCU holds them now
// (P-H0W Step 3: "if a reset-cause register is readable on this core, record it;
// do not assume one exists").
//
// `source=register_read` is part of the reading, not decoration: these are
// `R_SYSTEM->RSTSR0`/`RSTSR1` as the MCU reads them, and the decode uses the bit
// masks the vendor device header defines (`src/bench/ResetCause.h` explains and
// cites them). The raw values lead, so the decode can be checked and a flag this
// firmware does not name is not silently discarded.
//
// Plain `bench resetsrc` writes nothing. `bench resetsrc clear` clears both
// registers *first* and reports what they read afterwards, with `cleared` saying
// which of the two happened - because the flags latch (D30: a board that has taken
// one watchdog reset reports `wdtrf=1` on every later boot), a session that wants
// to attribute a reset must record the state, clear, cause the reset, and read
// again. The clear is deliberately not implicit: a readout must never destroy the
// evidence it was asked for.
//
// What a read does **not** establish on its own: that the value it returns is the
// one the last reset produced. Nothing in this firmware clears these flags, and the
// boot path before this code (the DFU bootloader, then FSP startup, which ships
// prebuilt in this framework) is not this firmware's to inspect - so attribution
// comes from the read/clear/observe/read sequence, never from one reading.
inline void resetCauseLine(bool clearFirst) {
    if (clearFirst) {
        resetCause::clear();
    }
    const resetCause::Snapshot cause = resetCause::read();
    ev::BoundedText text(serialTransport::port().workingBuffer(),
                         benchConfig::kReadoutLineCapacity);
    text.appendText("BENCH,resetsrc=1");
    text.field("source").appendText("register_read");
    text.field("cleared").appendBool(clearFirst);
    text.field("rstsr0").appendUnsigned(cause.rstsr0);
    text.field("rstsr1").appendUnsigned(cause.rstsr1);
    text.field("porf").appendBool(cause.powerOnReset);
    text.field("dpsrstf").appendBool(cause.deepSoftwareReset);
    text.field("wdtrf").appendBool(cause.watchdogReset);
    text.field("iwdtrf").appendBool(cause.independentWatchdogReset);
    text.field("swrf").appendBool(cause.softwareReset);
    text.field("rperf").appendBool(cause.ramParityError);
    text.field("reerf").appendBool(cause.reeError);
    text.field("busmrf").appendBool(cause.busMasterError);
    text.field("bussrf").appendBool(cause.busSlaveError);
    text.field("sperf").appendBool(cause.securityError);
    respond(text);
}

// `bench wdt` - what the watchdog hardware was actually programmed with (P-H0W
// Steps 6 and 7: "do not treat `kWatchdogTimeoutMs` as the realized period";
// "record the realized period").
//
// The health line reports the library's own computed period as
// `wdt_realized_ms`; this line reports the registers behind it, so the two can be
// checked against each other rather than trusted. `source=register_read` says the
// numbers are the MCU's own view, and the raw `WDTCR`/`WDTSR` values lead the
// decoded fields for the same reason the reset-cause line leads with raw values.
//
// `tops` and `cks` are **field values, not periods**: which period or division a
// value selects is hardware-manual knowledge, and this firmware reports the field
// rather than decoding it (`src/bench/WatchdogRegisters.h` explains). `pclkb_hz`
// is the vendor clock API's number, which is the half of a period calculation that
// cannot be guessed from the register contents.
//
// `cntval` is the live down-counter. Refreshes happen once per pass (tens of
// microseconds apart), so a healthy loop shows it near its reload value; reading it
// twice with a known gap is how a caller can see the countdown rate.
inline void watchdogLine() {
    const watchdogRegisters::Snapshot registers = watchdogRegisters::read();
    ev::BoundedText text(serialTransport::port().workingBuffer(),
                         benchConfig::kReadoutLineCapacity);
    text.appendText("BENCH,wdt=1");
    text.field("source").appendText("register_read");
    text.field("wdtcr").appendUnsigned(registers.wdtcr);
    text.field("tops").appendUnsigned(registers.tops);
    text.field("cks").appendUnsigned(registers.cks);
    text.field("rpes").appendUnsigned(registers.rpes);
    text.field("rpss").appendUnsigned(registers.rpss);
    text.field("wdtsr").appendUnsigned(registers.wdtsr);
    text.field("cntval").appendUnsigned(registers.counterValue);
    text.field("underflow").appendBool(registers.underflow);
    text.field("refresh_error").appendBool(registers.refreshError);
    text.field("pclkb_hz").appendUnsigned(watchdogRegisters::pclkbHz());
    text.field("wdt_realized_ms").appendUnsigned(WDT.getTimeout());
    respond(text);
}

// `bench resetsrc [clear]` - dispatch, including the argument check.
//
// An argument that is not `clear` is **refused**, not ignored: an operator who
// typed `clear`, saw a readout and believed the flags were reset would draw the
// wrong conclusion from the next read - and the whole point of this instrument is
// that a conclusion about a reset is drawn from these numbers.
//
// Returns whether the line was accepted. The caller records the outcome; this
// function only answers.
inline bool applyResetCause(const ev::BenchTokens& tokens) {
    if (tokens.count > 3U) {
        refuseLine("TooManyTokens");
        return false;
    }
    if (tokens.count == 3U &&
        !ev::benchTokenEquals(tokens.token(2), tokens.tokenLength(2), "clear")) {
        refuseLine("UnknownResetCauseArgument");
        return false;
    }
    resetCauseLine(tokens.count == 3U);
    return true;
}

inline void helpLine() {
    ev::BoundedText text(serialTransport::port().workingBuffer(),
                         benchConfig::kReadoutLineCapacity);
    text.appendText("BENCH,help=1");
    text.field("verbs")
        .appendText("health drive arm stop reset resetsrc wdt ticks inputs pins");
#if defined(EV_BENCH_H1A_DIAGNOSTIC)
    // The temporary H1A verb names itself here so a session capture cannot miss
    // that this image contains a non-shipping instrument.
    text.field("h1a_verbs")
        .appendText("h1a-brake (temporary H1A electrical gate, removed after H1A)");
#endif
    respond(text);
}

// ---------------------------------------------------------------------------
// The bench verbs
// ---------------------------------------------------------------------------

// `bench arm` - the deliberate arm, a transition from Disarmed only (M3).
// Refused while an abort is latched, which is what makes "stop" mean stopped until
// someone looks at why, and refused while already armed or driving, so a repeated
// arm can neither reset the per-arm budget nor cancel a live command.
//
// Returns whether the line was accepted. The caller records the outcome; this
// function only answers.
inline bool applyArm() {
    const ev::BenchArmRefusal refusal = detail::drive().arm();
    if (refusal != ev::BenchArmRefusal::None) {
        refuseLine(ev::benchArmRefusalName(refusal));
        return false;
    }
    ackLine("bench_arm");
    return true;
}

// `bench drive <fwd|rev> <permille> <ms>` - one bounded command. Whether the line
// is well formed is `ev::parseBenchDriveRequest()`'s decision (host-tested);
// whether the request is within the bench ceilings is `ev::BenchDrive`'s, so the
// reason an operator sees always names the limit that refused it.
//
// Returns whether the line was accepted. The caller records the outcome; this
// function only answers.
inline bool applyDrive(const ev::BenchTokens& tokens, uint32_t nowMs) {
    const ev::BenchDriveRequest request = ev::parseBenchDriveRequest(tokens);
    if (request.error != ev::BenchArgumentError::None) {
        refuseLine(ev::benchArgumentErrorName(request.error));
        return false;
    }

#if defined(EV_BENCH_H1A_DIAGNOSTIC)
    // At most one bench instrument may hold the driver. The diagnostic refuses a
    // drive while it holds; this refuses a drive while the hold is applied.
    if (detail::h1aHoldActive()) {
        refuseLine("H1AHoldInProgress");
        return false;
    }
#endif

    const ev::BenchCommandRefusal refusal =
        detail::drive().command(nowMs, request.mode, request.permille, request.durationMs);
    if (refusal != ev::BenchCommandRefusal::None) {
        refuseLine(ev::benchCommandRefusalName(refusal));
        return false;
    }

    driveLine();
    return true;
}

// `bench stop` - the abort. De-energizes now and latches until `bench reset`.
inline bool applyStop() {
    detail::drive().abort();
#if defined(EV_BENCH_H1A_DIAGNOSTIC)
    // `bench stop` means stopped, including the temporary H1A hold.
    if (detail::h1aHoldActive()) {
        detail::h1aHoldActive() = false;
        motorDriver::failSafe();
    }
#endif
    ackLine("bench_stop");
    driveLine();
    return true;
}

// `bench reset` - the deliberate exit from a latch. Leaves the instrument
// disarmed: re-arming is a separate decision.
inline bool applyReset() {
    detail::drive().reset();
#if defined(EV_BENCH_H1A_DIAGNOSTIC)
    if (detail::h1aHoldActive()) {
        detail::h1aHoldActive() = false;
        motorDriver::failSafe();
    }
#endif
    ackLine("bench_reset");
    driveLine();
    return true;
}

#if defined(EV_BENCH_H1A_DIAGNOSTIC)
// ---------------------------------------------------------------------------
// TEMPORARY - the H1A awake-EN-low hold (`bench h1a-brake [ms]`)
// ---------------------------------------------------------------------------
//
// P-H1A needs the driver's awake-EN-low combination (the plan's `Brake` levels)
// held still while the operator measures PH/EN/nSLEEP with a meter. The bounded
// bench drive cannot produce it - `ev::BenchDrive` accepts only a nonzero-effort
// fwd/rev command - and the production run path cannot be armed, so this verb
// exists for that measurement only. It is compiled into `env:bench` alone and
// is removed once H1A is recorded.
//
// It is bounded (its own expiry, checked on the pass clock), deliberate
// (`bench arm` is required, exactly as for the drive), screened by the same
// `ev::benchApplyVerdict()` - a latched fault, a violated tick order, an armed
// run or a run under way each end it - and it commands no effort: nSLEEP high,
// PH low, EN at zero duty. The physical kill switch is still the primary stop,
// and the motor must be disconnected for the H1A measurement it exists for.
//
// `bench h1a-brake` with no argument prints the hold's state instead.
inline void h1aHoldLine(uint32_t nowMs) {
    const int32_t remaining = static_cast<int32_t>(detail::h1aHoldExpiresAtMs() - nowMs);
    ev::BoundedText text(serialTransport::port().workingBuffer(),
                         benchConfig::kReadoutLineCapacity);
    text.appendText("BENCH,h1a=1");
    text.field("awake_en_low").appendBool(detail::h1aHoldActive());
    text.field("remaining_ms")
        .appendUnsigned(remaining > 0 ? static_cast<uint32_t>(remaining) : 0U);
    text.field("issued").appendUnsigned(detail::h1aHoldsIssued());
    text.field("ceiling_ms").appendUnsigned(benchConfig::kH1aHoldMaxMs);
    respond(text);
}

inline bool applyH1aAwakeHold(const ev::BenchTokens& tokens, uint32_t nowMs) {
    if (tokens.count == 2U) {
        // A state query, not a hold: answered like any other read-only verb.
        h1aHoldLine(nowMs);
        return true;
    }
    if (tokens.count != 3U) {
        refuseLine("Usage: bench h1a-brake <ms>");
        return false;
    }
    uint32_t durationMs = 0U;
    if (!ev::parseUnsignedDecimal(tokens.token(2), tokens.tokenLength(2), durationMs)) {
        refuseLine("DurationNotADecimal");
        return false;
    }
    if (durationMs == 0U) {
        refuseLine("DurationZero");
        return false;
    }
    if (durationMs > benchConfig::kH1aHoldMaxMs) {
        refuseLine("DurationAboveCeiling");
        return false;
    }
    if (detail::h1aHoldActive()) {
        refuseLine("H1AHoldInProgress");
        return false;
    }
    if (detail::drive().output().isDriving()) {
        refuseLine("H1ADriveActive");
        return false;
    }
    if (detail::drive().state() != ev::BenchDriveState::Armed) {
        refuseLine("H1ANotArmed");
        return false;
    }
    detail::h1aHoldActive() = true;
    detail::h1aHoldExpiresAtMs() = nowMs + durationMs;
    ++detail::h1aHoldsIssued();
    h1aHoldLine(nowMs);
    return true;
}
#endif  // EV_BENCH_H1A_DIAGNOSTIC

// True while any bench instrument holds the driver: the bounded drive or the
// temporary H1A hold. The production `disarm`/`reset` intercept (H1) is the caller
// that cares about both.
inline bool benchHoldsMotor() {
    if (detail::drive().output().isDriving()) {
        return true;
    }
#if defined(EV_BENCH_H1A_DIAGNOSTIC)
    if (detail::h1aHoldActive()) {
        return true;
    }
#endif
    return false;
}

// Revokes any bench-held output before a production command is forwarded (H1).
//
// The instrument is aborted - latched, so it cannot quietly resume applying effort
// once the command's effect has passed - and the driver's de-energized levels are
// written **directly**, so the stop reaches the pins in this pass and before the
// forwarded command can rewrite the shared motion/encoder state.
//
// `failSafe()` is the adapter's de-energize-only write and deliberately ignores the
// pass's ownership flag: this cannot suppress the forwarded command's own write
// later in the pass, and it cannot produce powered output, so it is not a second
// powered writer (`MotorDriver.h`).
inline void revokeBenchOutputForProductionCommand() {
    detail::drive().abort();
#if defined(EV_BENCH_H1A_DIAGNOSTIC)
    detail::h1aHoldActive() = false;
#endif
    motorDriver::failSafe();
    driveLine();
}

// One complete line, dispatched. A line that is not a bench verb is handed back to
// the production operator interface, so `status`, `checks`, `help` and the
// configuration verbs keep working in a bench session and a bench image cannot
// grow a second, diverging copy of them.
//
// Accounting (R3/L1): every path below records exactly one outcome through
// `detail::recordLineOutcome()` - accepted, refused or forwarded - and no leaf
// verb records anything itself. A forwarded production line is counted only
// as forwarded: its accepted/refused outcome belongs to the production
// counters, and counting it here too would count one command twice.
inline void handleLine(const char* line, uint16_t length, uint32_t nowMs,
                       VehicleRuntime& runtime, const WatchdogCheckpoint& watchdog) {
    const ev::BenchTokens tokens = ev::splitBenchTokens(line, length);
    if (tokens.count == 0U) {
        return;  // an empty line answers nothing, exactly as the operator path does
    }

    // Records one outcome and answers accordingly: the leaf's boolean says
    // whether it executed, and this is the only place that boolean becomes a
    // counter. A helper rather than a convention, so a verb added later that
    // forgets it fails to compile instead of silently skewing the session.
    const auto recordAccepted = [](bool accepted) {
        detail::recordLineOutcome(accepted ? ev::BenchLineOutcome::Accepted
                                           : ev::BenchLineOutcome::Refused);
    };

#if defined(EV_BENCH_H1A_DIAGNOSTIC)
    // The temporary H1A verb is recognized here, before `ev::benchVerbFor()`: it
    // stays out of `lib/` (and therefore out of the table both images share) on
    // purpose - it is not a bench capability, it is one measurement's instrument.
    // An over-long line is still refused before anything acts on it.
    if (tokens.count >= 2U &&
        ev::benchTokenEquals(tokens.token(0), tokens.tokenLength(0), "bench") &&
        ev::benchTokenEquals(tokens.token(1), tokens.tokenLength(1), "h1a-brake")) {
        if (tokens.overflow) {
            detail::recordLineOutcome(ev::BenchLineOutcome::Refused);
            refuseLine("TooManyTokens");
            return;
        }
        recordAccepted(applyH1aAwakeHold(tokens, nowMs));
        return;
    }
#endif

    const ev::BenchVerb verb = ev::benchVerbFor(tokens);
    if (verb == ev::BenchVerb::None) {
        // Not a bench line: the production operator interface owns it, so `status`,
        // `checks`, the configuration verbs and the telemetry verbs keep working in
        // a bench session rather than being reimplemented here.
        //
        // H1: with the bench holding the driver, two production commands are not
        // forwarded as they stand. `disarm` and `reset` are the only production
        // commands a bench session can type that would otherwise leave a bench
        // drive applied (the production authority is *already* disarmed during a
        // bench drive, so its own stop changes nothing the bench can see while the
        // console answers `OK,cmd=disarm`) or rewrite the shared motion/encoder
        // state a live bench command is running against. The bench revokes its own
        // output first, in this same dispatch, and then the command is forwarded -
        // so an acknowledged stop can never describe a bench drive that is still
        // applied.
        if (benchHoldsMotor()) {
            const ev::CommandParseResult parsed = ev::parseOperatorCommand(line, length);
            if (parsed.ok && ev::benchRevokesForProductionCommand(parsed.request.id)) {
                revokeBenchOutputForProductionCommand();
            }
        }
        detail::recordLineOutcome(ev::BenchLineOutcome::Forwarded);
        runtime.handleCommandLine(line, length, nowMs);
        return;
    }

    // An over-long line is refused before dispatch: acting on the tokens that fit
    // is how a command nobody typed gets applied.
    if (tokens.overflow) {
        detail::recordLineOutcome(ev::BenchLineOutcome::Refused);
        refuseLine("TooManyTokens");
        return;
    }

    // M3: the verb's own token bound, checked before dispatch. A trailing token on
    // a verb that takes none is a refusal and not something to ignore -
    // `bench arm <junk>` used to arm, and `bench reset <junk>` used to reset the
    // instrument.
    if (tokens.count > ev::benchVerbMaximumTokenCount(verb)) {
        detail::recordLineOutcome(ev::BenchLineOutcome::Refused);
        refuseLine("TooManyTokens");
        return;
    }

    switch (verb) {
        case ev::BenchVerb::Help:
            helpLine();
            detail::recordLineOutcome(ev::BenchLineOutcome::Accepted);
            return;
        case ev::BenchVerb::Arm:
            recordAccepted(applyArm());
            return;
        case ev::BenchVerb::Drive:
            recordAccepted(applyDrive(tokens, nowMs));
            return;
        case ev::BenchVerb::Stop:
            recordAccepted(applyStop());
            return;
        case ev::BenchVerb::Reset:
            recordAccepted(applyReset());
            return;
        case ev::BenchVerb::Health:
            healthLine(runtime, watchdog);
            detail::recordLineOutcome(ev::BenchLineOutcome::Accepted);
            return;
        case ev::BenchVerb::DriveStatus:
            driveLine();
            detail::recordLineOutcome(ev::BenchLineOutcome::Accepted);
            return;
        case ev::BenchVerb::Ticks:
            ticksLine();
            detail::recordLineOutcome(ev::BenchLineOutcome::Accepted);
            return;
        case ev::BenchVerb::Inputs:
            inputsLine();
            detail::recordLineOutcome(ev::BenchLineOutcome::Accepted);
            return;
        case ev::BenchVerb::Pins:
            pinsLine();
            detail::recordLineOutcome(ev::BenchLineOutcome::Accepted);
            return;
        case ev::BenchVerb::ResetCause:
            recordAccepted(applyResetCause(tokens));
            return;
        case ev::BenchVerb::Wdt:
            watchdogLine();
            detail::recordLineOutcome(ev::BenchLineOutcome::Accepted);
            return;
        case ev::BenchVerb::Unknown:
        case ev::BenchVerb::None:
            break;
    }

    detail::recordLineOutcome(ev::BenchLineOutcome::Refused);
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
            // A framing refusal, recorded with the reason on the wire: the
            // bytes never formed a command, so there is nothing to dispatch,
            // but the operator still got an answered refusal.
            detail::recordLineOutcome(ev::BenchLineOutcome::Refused);
            refuseLine("LineTooLong");
            break;
        }
        if (event == ev::LineEvent::InvalidCharacter) {
            detail::recordLineOutcome(ev::BenchLineOutcome::Refused);
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
// When the instrument stops holding the driver on this pass - its own expiry, its
// stall latch, or an arbitration revocation - the de-energized state is written
// here, in this pass (M1). The tick's stage-9 write is the only other writer, and
// it is skipped whenever the bench owns the pass and does not run at all on a pass
// with no control tick, so depending on it would leave the last command applied
// for a variable interval (and indefinitely under a stalled loop).
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
    const bool commandLapsed =
        (status == ev::BenchDriveStatus::Expired ||
         status == ev::BenchDriveStatus::EncoderStallFault);
    if (commandLapsed) {
        driveLine();
    }

    bool benchOutputActive = instrument.output().isDriving();
#if defined(EV_BENCH_H1A_DIAGNOSTIC)
    bool h1aHoldLapsed = false;
    // The temporary H1A hold is the bench's other output. It expires here, on
    // the pass clock, so it can never outlive its own bound; at most one of the
    // two instruments is active at a time (`applyDrive`/`applyH1aAwakeHold`).
    if (detail::h1aHoldActive()) {
        if (static_cast<int32_t>(nowMs - detail::h1aHoldExpiresAtMs()) >= 0) {
            detail::h1aHoldActive() = false;
            h1aHoldLapsed = true;
            h1aHoldLine(nowMs);
        } else if (!instrument.output().isDriving()) {
            benchOutputActive = true;
        }
    }
#endif

    const ev::BenchApplyVerdict verdict =
        ev::benchApplyVerdict(benchOutputActive, runtime.armed(), runtime.faultLatched(),
                              runtime.runUnderway(), runtime.orderViolationDetected());

    PassArbitration arbitration;
    arbitration.refusal = verdict.refusal;
    arbitration.ownsMotor = verdict.mayApply;

    // True when the instrument stopped holding the driver on this pass (M1). What
    // the pins must show then is the de-energized state, and this pass is where it
    // has to be written.
    bool benchReleasedThisPass = commandLapsed;
#if defined(EV_BENCH_H1A_DIAGNOSTIC)
    benchReleasedThisPass = benchReleasedThisPass || h1aHoldLapsed;
#endif

    if (!verdict.mayApply) {
        if (instrument.output().isDriving()) {
            instrument.abort();
            arbitration.ownsMotor = false;
            // M1: the de-energize lands in this pass rather than whenever the next
            // control tick happens to write. `failSafe()` ignores the ownership
            // flag, so a competing writer's own command still reaches the driver
            // later in the pass.
            motorDriver::failSafe();
            refuseLine(ev::benchApplyRefusalName(verdict.refusal));
        } else if (benchReleasedThisPass) {
            // M1: nothing else writes the de-energized state on this pass - the
            // tick's stage-9 write is skipped whenever the bench owns the pass and
            // does not run at all on a pass with no control tick.
            motorDriver::failSafe();
        }
#if defined(EV_BENCH_H1A_DIAGNOSTIC)
        else if (detail::h1aHoldActive()) {
            // Something outranks the diagnostic (a latched fault, a violated
            // tick order, an armed run or a run under way): the hold ends, the
            // driver is de-energized in this pass, and the pins are handed back.
            detail::h1aHoldActive() = false;
            motorDriver::failSafe();
            refuseLine(ev::benchApplyRefusalName(verdict.refusal));
            h1aHoldLine(nowMs);
        }
#endif
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
#if defined(EV_BENCH_H1A_DIAGNOSTIC)
        if (detail::h1aHoldActive()) {
            detail::h1aHoldActive() = false;
            refuseLine("OrderViolation");
        }
#endif
        motorDriver::setMotorPinsOwned(false);
        return;
    }

    if (arbitration.ownsMotor) {
#if defined(EV_BENCH_H1A_DIAGNOSTIC)
        if (detail::h1aHoldActive()) {
            motorDriver::applyH1aAwakeEnLow();
            motorDriver::setMotorPinsOwned(false);
            return;
        }
#endif
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
