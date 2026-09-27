#include "EncoderInput.h"

namespace evhw {
namespace encoderInput {
namespace {

// The one decoder in the firmware, with static storage duration because the
// interrupt handler has to reach it. It is written only by the ISR and read only
// through the snapshot below (and through `decoder()` for the chain calls that
// own per-run state), so no other code has to reason about a changing count.
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
    reestablishReference();
}

void reestablishReference() {
    // The reference sample must be taken with the same two levels the decoder
    // will see next, and it never counts (QuadratureDecoder.h). Reading the pins
    // here rather than assuming both are low keeps this honest even if the idle
    // levels turn out to be the opposite of what the plan guesses.
    g_decoder.update(digitalRead(kEncoderA) == HIGH, digitalRead(kEncoderB) == HIGH);
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

ev::QuadratureDecoder& decoder() { return g_decoder; }

}  // namespace encoderInput
}  // namespace evhw
