#pragma once

#include <Arduino.h>

#include <QuadratureDecoder.h>

#include "PinMap.h"

// ---------------------------------------------------------------------------
// The encoder interrupt wrapper - the only interrupt in this firmware
// ---------------------------------------------------------------------------
//
// What this module is
//
// * The pin/ISR layer the decoding module deliberately left out: it attaches one
//   A-channel CHANGE interrupt and hands the two channel levels to the
//   already-tested pure decoder (`ev::QuadratureDecoder`, x2 decoding, A-change
//   only). No counting, sign, filtering or scaling logic is duplicated here.
// * The owner of the one decoder instance in the firmware. The ISR needs to
//   reach it, so it has static storage. No decoder reference escapes this
//   adapter; the main loop reads only the coherent snapshot below.
// * A safe snapshot for the control cycle. `snapshotTicks()` reads the decoder's
//   counters with interrupts briefly disabled, so the cycle acts on one coherent
//   instant instead of three fields read at three different times. A Cortex-M
//   int32 load is itself atomic, so this is about coherence between fields, not
//   about tearing.
//
// What the ISR is allowed to do, and why
//
//   The handler reads two digital pins and updates its private decoder. That
//   is all. It performs no serial I/O, no ADC, no state machine, no controller,
//   no floating point, no allocation and no blocking wait, and it calls nothing
//   from the safety, run or telemetry layers - any of those would turn one
//   interrupt into a latency and re-entrancy hazard for every other part of the
//   firmware. It is short enough that the main loop's brief interrupt-off section
//   is the only mutual exclusion the design needs.
//
// What this module does NOT do
//
// * It does not decide which tick direction is forward travel. That is
//   UNVERIFIED (`VERIFIED_STATE.md` #4) and belongs to the configuration.
// * It does not filter, debounce or validate the signal. Whether the encoder is
//   clean enough to count is a measurement (`VERIFIED_STATE.md` #5); the decoder
//   reports what the channels did, including ignored non-A transitions.
// * It does not convert ticks to distance or speed. That is `Odometry.h` and
//   `VelocityEstimator.h` with a measured scale.

namespace evhw {
namespace encoderInput {

// The snapshot the control cycle acts on: the signed tick count plus the
// decoder's own progress counters, read as one coherent instant.
struct TickSnapshot {
    int32_t ticks;
    uint32_t countedEdges;
    uint32_t ignoredUpdates;
};

// Configures the two encoder pins, attaches the A-channel CHANGE interrupt and
// takes the first sample.
//
// The first sample after construction or a reset only establishes the decoder's
// reference level: it never counts, because there is no previous state to compare
// against (power-on levels are not known). This must therefore run while the
// drivetrain is known not to be moving. Reset/reference operations preserve the
// caller's PRIMASK; software coherence does not establish mechanical standstill.
//
// Note on the pull-ups: on this core `attachInterrupt()` re-uses the pin's current
// configuration, so the pins are set to `INPUT_PULLUP` before attaching. Whether
// the encoder outputs need a pull-up at all is a wiring fact that is UNVERIFIED;
// the pull-up is a bench placeholder and one of the first things to check.
void begin();

// Samples levels without resetting counters or touching interrupt attachment.
// With an existing reference this is an ordinary decoder update (it may count).
// The pin reads/update are ISR-excluded, not electrically simultaneous. Call
// with stationary mechanics for trustworthy attribution; normally use the
// combined reset/reference operation below for a new measurement epoch.
void reestablishReference();

// The coherent new-run decoder boundary: resets the decoder and re-takes the
// reference sample as one interrupt-masked critical section
// (`QuadratureDecoder::reset()` plus the reference read). Call it with the
// vehicle stationary for physical attribution, after the pure chain reset.
// The guard excludes ISR interleaving and restores exactly the prior PRIMASK;
// it does not freeze inputs, preserve every physical edge, or prove a stop.
void resetAndReestablishReference();

// The coherent tick snapshot described above.
TickSnapshot snapshotTicks();

}  // namespace encoderInput
}  // namespace evhw
