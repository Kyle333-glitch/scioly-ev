#pragma once

#include <cstdint>
#include <type_traits>

namespace ev {

// How the project knows a physical value. This is deliberately part of the
// type, not a comment: a placeholder must stay visibly a placeholder.
enum class ValueSource : uint8_t {
    // A placeholder. No measurement and no official citation behind it.
    // Never sufficient to authorize powered motion.
    Provisional = 0,
    // Taken from the official 2027 Division C rules; the citing rule number is
    // recorded at the definition.
    RuleDerived = 1,
    // Backed by a recorded physical measurement (see VERIFIED_STATE.md).
    Measured = 2,
    // Computed from measured/cited inputs by pure arithmetic (never a
    // measurement of the vehicle). Must cite the derivation; never relabelled
    // Measured. D35: the run's planned cruise is the first such value.
    Derived = 3,
};

// A physical value together with the evidence behind it. There is no default
// constructor on purpose: every use has to state where the number came from.
//
// `evidence` is the pointer to where that something is recorded - a
// `VERIFIED_STATE.md` row plus the capture under `data/evidence/`, a rule number,
// or a release. It may be null for a placeholder and must not be for anything
// else: `citesEvidence()` is the rule, and the configuration-build report carries
// its verdict so a promoted value that names no evidence cannot be armed on.
//
// The pointer is deliberately not a `std::string` or an owned buffer: this is a
// compile-time literal in a microcontroller's flash, the same way the rule
// citations beside the values are, and nothing here may allocate.
template <typename T>
struct Sourced {
    constexpr Sourced(T value_, ValueSource source_, const char* evidence_ = nullptr)
        : value(value_), source(source_), evidence(evidence_) {}

    constexpr bool isMeasured() const { return source == ValueSource::Measured; }

    constexpr bool isDerived() const { return source == ValueSource::Derived; }

    // True when this value's provenance is complete: a placeholder needs no
    // citation, and anything else has to name where it came from.
    //
    // A non-placeholder citation must be a *name*, not merely a non-null pointer
    // (audit L1): `evidence != nullptr` accepted the empty string, so a promoted
    // value could claim a citation that says nothing at all and still pass the
    // check the configuration-build report advertises. The pointer is still not
    // resolved here - the promotion review owns that - but an empty name is now
    // a hole the check itself closes.
    constexpr bool citesEvidence() const {
        return source == ValueSource::Provisional ||
               (evidence != nullptr && evidence[0] != '\0');
    }

    T value;
    ValueSource source;
    const char* evidence;
};

// Pin the "no silent defaults" contract at compile time rather than relying on
// the absence of a default constructor being noticed. A value without a source
// must be impossible to write, not merely inconvenient.
static_assert(!std::is_default_constructible<Sourced<double>>::value,
              "Sourced must never default-construct; unmeasured values must be explicit");

// Whole milliseconds for a duration expressed in seconds.
//
// Durations are seconds in the pure core (SOFTWARE_ARCHITECTURE.md section 6)
// while every elapsed-time comparison happens against a `millis()` reading, so
// the one conversion between the two belongs in one place instead of being
// retyped at every timer.
//
// A duration that is not a positive number is 0 (no time), never a large
// interval, and a duration longer than the clock can express is the longest
// representable interval, never a wrapped short one. A caller that means "no
// bound" states that with its own configuration, not by passing an infinity to
// a timer.
inline uint32_t msFromSeconds(double seconds) {
    if (!(seconds > 0.0)) {
        return 0U;
    }
    const double ms = seconds * 1000.0;
    return ms >= 4294967295.0 ? 0xFFFFFFFFu : static_cast<uint32_t>(ms);
}

}  // namespace ev
