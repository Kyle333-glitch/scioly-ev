#pragma once

#include <Arduino.h>

#include <BenchCommand.h>
#include <BenchDrive.h>
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
// D35: reverted the temporary 300-permille tournament override back to 150; a
// higher ceiling needs a recorded reason, not a bring-up override.
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
// transport's single line buffer and one buffer has one length.
//
// **The derived worst case of the widest readout, `healthLine()`, is 534
// characters** - every `uint32` field at its full ten digits, which the
// free-running counters really can reach (`passes` needs roughly 26 h at the
// measured ~46,000 passes/s; `last_refresh_ms` reaches ten digits at the
// `millis()` wrap). That is **86 characters over** the 448 in
// `ev::kSerialLineCapacity`. The widest line any capture has actually contained
// is 389, so the overflow is not reachable in a bench session - but it is
// reachable in the limit, and the failure it produces is the one failure this
// console must not have: a readout that does not fit is dropped whole and
// counted (`respond()`), and the counter that would say so, `responses_lost`, is
// a field of the health line itself.
//
// Two things answer that here:
//
//   * `stageLostReadoutNotice()` - a dropped readout leaves a short, constant
//     notice in its place, so the loss is visible on the wire rather than only
//     in a counter that travelled on the line that was lost;
//   * this number, recorded so the next field added has to look at it. The owed
//     fix is to split `health` (the watchdog/loop half and the console/interface
//     half), not to trim the schema. Until that happens this comment is the
//     record of how much room is left, and there is none.
//
// Note also the shape of the headroom: the non-obvious part is that it is
// *unbounded* growth, not a fixed gap. Every other consumer of the shared buffer
// states its worst case against it
// (`ev::kOperatorResponseWorstCaseChars`, `lib/evcore/src/OperatorResponse.h`);
// this console used to state none, which is why the widest line in the firmware
// was the one with no bound behind it.
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
inline void applyResetCause(const ev::BenchTokens& tokens) {
    if (tokens.count > 3U) {
        ++detail::refusedLines();
        refuseLine("TooManyTokens");
        return;
    }
    if (tokens.count == 3U &&
        !ev::benchTokenEquals(tokens.token(2), tokens.tokenLength(2), "clear")) {
        ++detail::refusedLines();
        refuseLine("UnknownResetCauseArgument");
        return;
    }
    resetCauseLine(tokens.count == 3U);
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

#if defined(EV_BENCH_H1A_DIAGNOSTIC)
    // At most one bench instrument may hold the driver. The diagnostic refuses a
    // drive while it holds; this refuses a drive while the hold is applied.
    if (detail::h1aHoldActive()) {
        ++detail::refusedLines();
        refuseLine("H1AHoldInProgress");
        return;
    }
#endif

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
#if defined(EV_BENCH_H1A_DIAGNOSTIC)
    // `bench stop` means stopped, including the temporary H1A hold.
    if (detail::h1aHoldActive()) {
        detail::h1aHoldActive() = false;
        motorDriver::failSafe();
    }
#endif
    ackLine("bench_stop");
    driveLine();
}

// `bench reset` - the deliberate exit from a latch. Leaves the instrument
// disarmed: re-arming is a separate decision.
inline void applyReset() {
    detail::drive().reset();
#if defined(EV_BENCH_H1A_DIAGNOSTIC)
    if (detail::h1aHoldActive()) {
        detail::h1aHoldActive() = false;
        motorDriver::failSafe();
    }
#endif
    ackLine("bench_reset");
    driveLine();
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

inline void applyH1aAwakeHold(const ev::BenchTokens& tokens, uint32_t nowMs) {
    if (tokens.count == 2U) {
        h1aHoldLine(nowMs);
        return;
    }
    if (tokens.count != 3U) {
        ++detail::refusedLines();
        refuseLine("Usage: bench h1a-brake <ms>");
        return;
    }
    uint32_t durationMs = 0U;
    if (!ev::parseUnsignedDecimal(tokens.token(2), tokens.tokenLength(2), durationMs)) {
        ++detail::refusedLines();
        refuseLine("DurationNotADecimal");
        return;
    }
    if (durationMs == 0U) {
        ++detail::refusedLines();
        refuseLine("DurationZero");
        return;
    }
    if (durationMs > benchConfig::kH1aHoldMaxMs) {
        ++detail::refusedLines();
        refuseLine("DurationAboveCeiling");
        return;
    }
    if (detail::h1aHoldActive()) {
        ++detail::refusedLines();
        refuseLine("H1AHoldInProgress");
        return;
    }
    if (detail::drive().output().isDriving()) {
        ++detail::refusedLines();
        refuseLine("H1ADriveActive");
        return;
    }
    if (detail::drive().state() != ev::BenchDriveState::Armed) {
        ++detail::refusedLines();
        refuseLine("H1ANotArmed");
        return;
    }
    detail::h1aHoldActive() = true;
    detail::h1aHoldExpiresAtMs() = nowMs + durationMs;
    ++detail::h1aHoldsIssued();
    ++detail::acceptedLines();
    h1aHoldLine(nowMs);
}
#endif  // EV_BENCH_H1A_DIAGNOSTIC

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

#if defined(EV_BENCH_H1A_DIAGNOSTIC)
    // The temporary H1A verb is recognized here, before `ev::benchVerbFor()`: it
    // stays out of `lib/` (and therefore out of the table both images share) on
    // purpose - it is not a bench capability, it is one measurement's instrument.
    // An over-long line is still refused before anything acts on it.
    if (tokens.count >= 2U &&
        ev::benchTokenEquals(tokens.token(0), tokens.tokenLength(0), "bench") &&
        ev::benchTokenEquals(tokens.token(1), tokens.tokenLength(1), "h1a-brake")) {
        if (tokens.overflow) {
            ++detail::refusedLines();
            refuseLine("TooManyTokens");
            return;
        }
        applyH1aAwakeHold(tokens, nowMs);
        return;
    }
#endif

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
        case ev::BenchVerb::Pins:
            pinsLine();
            return;
        case ev::BenchVerb::ResetCause:
            applyResetCause(tokens);
            return;
        case ev::BenchVerb::Wdt:
            watchdogLine();
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

    bool benchOutputActive = instrument.output().isDriving();
#if defined(EV_BENCH_H1A_DIAGNOSTIC)
    // The temporary H1A hold is the bench's other output. It expires here, on
    // the pass clock, so it can never outlive its own bound; at most one of the
    // two instruments is active at a time (`applyDrive`/`applyH1aAwakeHold`).
    if (detail::h1aHoldActive()) {
        if (static_cast<int32_t>(nowMs - detail::h1aHoldExpiresAtMs()) >= 0) {
            detail::h1aHoldActive() = false;
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
    if (!verdict.mayApply) {
        if (instrument.output().isDriving()) {
            instrument.abort();
            arbitration.ownsMotor = false;
            refuseLine(ev::benchApplyRefusalName(verdict.refusal));
        }
#if defined(EV_BENCH_H1A_DIAGNOSTIC)
        else if (detail::h1aHoldActive()) {
            // Something outranks the diagnostic (a latched fault, a violated
            // tick order, an armed run or a run under way): the hold ends and
            // the pins are left to the tick's own write this pass.
            detail::h1aHoldActive() = false;
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
