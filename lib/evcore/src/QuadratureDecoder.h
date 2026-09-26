#pragma once

#include <cstdint>

namespace ev {

// ---------------------------------------------------------------------------
// A-channel CHANGE x2 incremental quadrature decoding - pure logic
// ---------------------------------------------------------------------------
//
// What this module is
//
// * One incremental encoder with two channels, A and B, in quadrature.
// * x2 resolution: one count per transition of the A channel, on both of its
//   edges. The B level is read with the transition and decides the sign.
// * Pure, deterministic integer logic: no Arduino header, no pin read, no
//   interrupt, no time source, no float, no allocation.
// * Raw encoder ticks only. Counts per revolution, gearing, wheel geometry and
//   distance conversion are deliberately absent from this layer and must not be
//   added to it. Those facts are UNVERIFIED (VERIFIED_STATE.md #4, #5, #6) and
//   belong to the configuration/odometry layers once they are measured.
//
// What this module is not
//
// * It is not the pin/ISR layer. A later stage samples A and B on each
//   A-channel change (attachInterrupt ... CHANGE) and hands those two levels to
//   update(). That wrapper is the only place Arduino is allowed, and keeping
//   this module free of it is what makes the decoding host-testable now, with
//   no hardware on the bench.
// * It does not decide which sense is physical forward travel. Nothing in this
//   module may be marked VERIFIED for direction: the mapping from Positive or
//   Negative to forward motion is UNVERIFIED (VERIFIED_STATE.md #4) and is
//   settled only by hand-rotating a real encoder and recording the count.
//
// Quadrature convention used here (software convention, not a physical fact)
//
// The 2-bit (A, B) state walks a Gray cycle, one encoder edge per step:
//
//     00 -> 10 -> 11 -> 01 -> 00        Positive sense in this module
//
// Stepping that cycle the other way is Negative sense. Two of the four edges in
// a cycle change A (A rises into 10 and falls into 01) and two change B, so an
// A-change x2 decode counts exactly twice per full cycle.
//
// Why the sign must come from both channels
//
// On the Positive cycle the two counted A edges land in different states, 10 and
// 01: B is low at one of them and high at the other while the sense is the same.
// The sign therefore comes from the whole (A, B) state an A edge lands in, not
// from B alone. Reading B alone - or B plus the edge type - is the classic
// defect: it turns one of the two A edges upside down, so a full revolution
// cancels to zero and the count still looks plausible while being useless.

// The sense one decoded A transition contributes to the tick count. These are
// a software counting convention; nothing here maps them onto physical forward
// or reverse travel.
enum class QuadratureTickSense : int8_t {
    Negative = -1,  // one count the other way around the quadrature cycle
    None = 0,       // not an A transition: contributes no count
    Positive = 1,   // one count forwards around the quadrature cycle
};

// The sense contributed by an A transition that lands in the (A, B) state
// `aLevel`/`bLevel`. Both levels are required: the four landing states split
// into two senses by parity, so this is not a function of B alone.
//
//   00 and 11 (equal levels, even parity) -> Negative
//   10 and 01 (different levels, odd parity) -> Positive
//
// All four landing states are reachable by a legal A edge, so no A-channel
// transition is undecodable in an A-change x2 scheme and none may be rejected.
// The only thing this layer can observe as anomalous is a sample in which A did
// not change at all, which is handled by update() below.
inline QuadratureTickSense tickSenseForLandingState(bool aLevel, bool bLevel) {
    if (aLevel == bLevel) {
        return QuadratureTickSense::Negative;
    }
    return QuadratureTickSense::Positive;
}

// Decoder state: the levels most recently seen and the signed tick count.
//
// There is no clock, no filtering and no debounce: whether the signal is clean
// enough to count is a hardware question (VERIFIED_STATE.md #5), and this layer
// must report what the channels did without inventing a signal-quality policy.
class QuadratureDecoder {
public:
    // Feeds the A and B levels as sampled at one instant and returns the sense
    // of the transition that sample represents.
    //
    // Intended caller: the future pin/ISR layer, once per A-channel change. It
    // is deliberately safe to call with a sample in which A has not changed - a
    // duplicate or redundant call, or a sample from a caller that also reports
    // B transitions - because a spurious call must never move the count. Such a
    // call contributes None, is counted in ignoredUpdates(), and still refreshes
    // the tracked B level so the next real A edge is decoded against the current
    // state rather than a stale one.
    QuadratureTickSense update(bool aLevel, bool bLevel) {
        if (!hasReference_) {
            // The first sample after construction or reset() only establishes
            // the reference state; it never counts. There is no previous state
            // to compare against, and no power-on channel level may be assumed
            // (both channels can legitimately sit high at rest). The vehicle
            // layer can establish the reference once during initialization,
            // while the drivetrain is not moving.
            hasReference_ = true;
            aLevel_ = aLevel;
            bLevel_ = bLevel;
            return QuadratureTickSense::None;
        }

        const bool aTransition = (aLevel != aLevel_);
        aLevel_ = aLevel;
        bLevel_ = bLevel;

        if (!aTransition) {
            ++ignoredUpdates_;
            return QuadratureTickSense::None;
        }

        const QuadratureTickSense sense = tickSenseForLandingState(aLevel, bLevel);
        ticks_ += static_cast<int8_t>(sense);
        ++countedEdges_;
        return sense;
    }

    // Cumulative signed count of decoded A transitions, in raw encoder ticks.
    // Zero after construction and after reset(). int32_t is deliberate: it
    // matches SensorSnapshot::motorEncoderTicks, and a single run of this event
    // cannot come near its range.
    int32_t ticks() const { return ticks_; }

    // Number of A transitions decoded since construction/reset(). ticks() is the
    // signed sum of exactly these edges, so comparing the two distinguishes a
    // count that is progressing from one that is oscillating.
    uint32_t countedEdges() const { return countedEdges_; }

    // Number of update() calls that were not an A transition. Expected whenever
    // the caller also reports B transitions or repeats a sample; a large value
    // relative to countedEdges() is a signal-quality question for bring-up
    // (VERIFIED_STATE.md #5), not something this layer adjudicates.
    uint32_t ignoredUpdates() const { return ignoredUpdates_; }

    // True once a reference state exists, i.e. after the first update() since
    // construction or reset(). Until then no transition can be decoded and
    // ticks() stays 0.
    bool hasReference() const { return hasReference_; }

    // The levels most recently seen. Meaningful only once hasReference() is true.
    bool aLevel() const { return aLevel_; }
    bool bLevel() const { return bLevel_; }

    // Returns the decoder to the state it has after construction: no reference,
    // no count, no remembered levels. Nothing resumes automatically and no
    // previous position survives, so a run always starts from a fresh reference.
    void reset() {
        hasReference_ = false;
        aLevel_ = false;
        bLevel_ = false;
        ticks_ = 0;
        countedEdges_ = 0;
        ignoredUpdates_ = 0;
    }

private:
    bool hasReference_ = false;
    bool aLevel_ = false;
    bool bLevel_ = false;
    int32_t ticks_ = 0;
    uint32_t countedEdges_ = 0;
    uint32_t ignoredUpdates_ = 0;
};

}  // namespace ev
