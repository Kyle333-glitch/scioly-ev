#pragma once

#include <cstdint>

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
};

// A physical value together with the evidence behind it. There is no default
// constructor on purpose: every use has to state where the number came from.
template <typename T>
struct Sourced {
    constexpr Sourced(T value_, ValueSource source_) : value(value_), source(source_) {}

    constexpr bool isMeasured() const { return source == ValueSource::Measured; }

    T value;
    ValueSource source;
};

}  // namespace ev
