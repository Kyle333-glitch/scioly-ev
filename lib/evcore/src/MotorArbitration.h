#pragma once

#include <cstdint>

#include "SerialCommandParser.h"

namespace ev {

// ---------------------------------------------------------------------------
// Who owns the motor driver - the one decision that lets the bench write it
// ---------------------------------------------------------------------------
//
// What this module is
//
// * Which of the firmware's two writers may command the driver in a pass. There
//   is exactly one production writer - the control tick's stage 9, which writes
//   the authority's command - and exactly one bench writer, the bench console's
//   bounded output. On a bench image both exist in the same pass.
// * Pure and host-testable: it reads no clock, touches no pin, and takes every
//   input as an argument. It is the composition seam between the bench
//   instrument and the production chain, which is the one seam that has no
//   runtime in a host test - so the decision is extracted here to be tested
//   (`test_bench_arbitration`).
//
// Why the obvious test is wrong
//
//   The first version of this decision was `bench is driving && !armed`. That is
//   not the same question, and the difference is the dangerous direction:
//
//   * a latched fault clears arming (`SafetyAuthority::authorize()` latches the
//     fault and clears `armed_`), so a faulted vehicle reads as `!armed`;
//   * a violated control-tick order calls the driver's fail-safe write and then
//     disarms, so the pass that just refused to trust its own cycle also reads as
//     `!armed`.
//
//   In both states `!armed` is true and the motor is *not* free. `armed` answers
//   "is a run using the motor", and the bench's question is "may I use the motor
//   at all" - which is answered by asking about the conditions that take the
//   motor away, not by inferring them from arming.
//
// What it is not
//
// * It does not apply anything, and it is not an authority. `ev::BenchDrive`
//   still owns the arm, the ceilings, the budget and its own latch
//   (`BenchDrive.h`); this module only says whether that instrument's output may
//   reach the pins in this pass.
// * It does not replace the physical kill switch. It decides between two writers
//   of one firmware; it has no opinion about the motor supply.
// * It is compiled into both images (it is in `lib/`), but the production image
//   contains no caller: the only caller is `src/bench/`, which the production
//   environment does not compile. A predicate with no caller applies nothing.

// Why the bench may not write the driver this pass. `None` is the only value that
// permits a write; every other value names the condition that outranks it, most
// severe first (the order the checks are applied in below, and the order
// `benchApplyRefusalName()` reports).
enum class BenchApplyRefusal : uint8_t {
    None = 0,
    // A fault is latched by the run policy or by the safety authority. The
    // firmware has already decided powered motion is not allowed; the bench does
    // not get to overrule it.
    FaultLatched = 1,
    // The last control tick's stage order was violated (since boot). Firmware
    // whose control cycle ran wrongly does not get a bench command applied over
    // its fail-safe write.
    OrderViolation = 2,
    // A run is in a phase that must stay armed (`ev::isRunPhaseUnderway`). The run
    // path is using the motor.
    RunUnderway = 3,
    // A run is armed but has not started travelling. The run's own authorized
    // command is what must reach the driver; two sources asking for effort in one
    // cycle is exactly what the single-writer discipline exists to prevent.
    RunArmed = 4,
    // Nothing outranks the bench: the instrument simply is not commanding a drive,
    // so there is nothing to apply. Not a refusal an operator caused.
    InstrumentNotDriving = 5,
};

inline const char* benchApplyRefusalName(BenchApplyRefusal refusal) {
    switch (refusal) {
        case BenchApplyRefusal::None:
            return "None";
        case BenchApplyRefusal::FaultLatched:
            return "FaultLatched";
        case BenchApplyRefusal::OrderViolation:
            return "OrderViolation";
        case BenchApplyRefusal::RunUnderway:
            return "RunUnderway";
        case BenchApplyRefusal::RunArmed:
            return "RunArmed";
        case BenchApplyRefusal::InstrumentNotDriving:
            return "InstrumentNotDriving";
    }
    return "?";
}

struct BenchApplyVerdict {
    // Invariant: `mayApply` is true exactly when `refusal == None`.
    bool mayApply;
    BenchApplyRefusal refusal;
};

// May the bench write the motor driver this pass?
//
// `benchDriving` is the bench instrument's own verdict (`BenchDriveOutput::
// isDriving()`); `runArmed`, `faultLatched`, `runUnderway` and
// `orderViolationDetected` are the vehicle runtime's, and each is a separate fact
// on purpose - collapsing them into one "the run is not using the motor" flag is
// how the inverted gate happened.
inline BenchApplyVerdict benchApplyVerdict(bool benchDriving, bool runArmed, bool faultLatched,
                                           bool runUnderway, bool orderViolationDetected) {
    if (faultLatched) {
        return {false, BenchApplyRefusal::FaultLatched};
    }
    if (orderViolationDetected) {
        return {false, BenchApplyRefusal::OrderViolation};
    }
    if (runUnderway) {
        return {false, BenchApplyRefusal::RunUnderway};
    }
    if (runArmed) {
        return {false, BenchApplyRefusal::RunArmed};
    }
    if (!benchDriving) {
        return {false, BenchApplyRefusal::InstrumentNotDriving};
    }
    return {true, BenchApplyRefusal::None};
}

// Which production commands the bench must revoke its own output for before it
// forwards them (H1).
//
// A bench session forwards every line that is not a bench verb to the production
// operator interface, and two production commands must not run while a bench
// drive is applied:
//
//   * `disarm` - the production authority is *already* disarmed during a bench
//     drive (the arbitration refuses an armed run), so its `disarm()` changes
//     nothing the bench can see while the console answers `OK,cmd=disarm`. An
//     operator reading that acknowledgment as a stop is reading it correctly in
//     every other mode and wrongly here;
//   * `reset` - which rewrites the shared motion/encoder state a live bench
//     command is running against.
//
// The console does not refuse them: it revokes its own output first (latched, and
// de-energized in the same pass) and then forwards the command, so the operator's
// stop is honoured and the production command runs against a driver that is
// already de-energized. This predicate is the seam, extracted here so the host
// suite can pin the exact command set.
inline bool benchRevokesForProductionCommand(OperatorCommandId id) {
    return id == OperatorCommandId::Disarm || id == OperatorCommandId::Reset;
}

}  // namespace ev
