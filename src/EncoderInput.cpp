#include "EncoderInput.h"

namespace evhw {
namespace encoderInput {
namespace {

// The one decoder in the firmware, with static storage duration because the
// interrupt handler has to reach it. Only this adapter mutates it: the ISR
// updates it, and main-context reset/reference operations mask interrupts.
// Main-loop readers use snapshotTicks(); no mutable decoder escapes the adapter.
ev::QuadratureDecoder g_decoder;

// The interrupt handler.
//
// Two digital reads and one pure decoder update. Nothing else belongs here: no
// serial output, no ADC, no state machine, no controller, no floating point, no
// allocation (see the header).
void encoderIsr() {
    g_decoder.update(digitalRead(kEncoderA) == HIGH, digitalRead(kEncoderB) == HIGH);
}

// The interrupt-off section used by snapshotTicks(). PRIMASK is saved and
// restored rather than unconditionally enabled, so a caller that was already
// inside a critical section keeps it.
inline uint32_t saveAndDisableInterrupts() {
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
}

inline void restoreInterrupts(uint32_t primask) {
    __set_PRIMASK(primask);
}

}  // namespace

void begin() {
    pinMode(kEncoderA, INPUT_PULLUP);
    pinMode(kEncoderB, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(kEncoderA), encoderIsr, CHANGE);
    resetAndReestablishReference();
}

void reestablishReference() {
    // Sample actual levels rather than assuming idle LOW. After reset this
    // establishes a reference without counting; with an existing reference it
    // is an ordinary update. Masking prevents ISR interleaving, not changes to
    // electrical inputs: the two digitalRead calls are still sequential.
    const uint32_t primask = saveAndDisableInterrupts();
    g_decoder.update(digitalRead(kEncoderA) == HIGH, digitalRead(kEncoderB) == HIGH);
    restoreInterrupts(primask);
}

void resetAndReestablishReference() {
    // The coherent new-run boundary for the ISR-shared decoder: the reset (six
    // field writes the ISR could interleave with) and the reference sample are
    // one critical section, so the state an interrupt sees before and after is a
    // complete one. Restore exactly the prior PRIMASK, including an outer
    // caller's disabled state. No formatting, allocation or blocking belongs here.
    //
    // Reset does NOT prove standstill. Stationary mechanics are required for
    // trustworthy physical attribution: pin reads are sequential, pending edges
    // can coalesce, and motion during this boundary need not be preserved.
    // A pending callback may see the newly sampled A level and count no edge.
    const uint32_t primask = saveAndDisableInterrupts();
    g_decoder.reset();
    g_decoder.update(digitalRead(kEncoderA) == HIGH, digitalRead(kEncoderB) == HIGH);
    restoreInterrupts(primask);
}

TickSnapshot snapshotTicks() {
    TickSnapshot snapshot;
    const uint32_t primask = saveAndDisableInterrupts();
    snapshot.ticks = g_decoder.ticks();
    snapshot.countedEdges = g_decoder.countedEdges();
    snapshot.ignoredUpdates = g_decoder.ignoredUpdates();
    restoreInterrupts(primask);
    return snapshot;
}

}  // namespace encoderInput
}  // namespace evhw
