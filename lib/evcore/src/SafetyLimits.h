#pragma once

#include "Units.h"

#include <type_traits>

namespace ev {

// Ceilings the vehicle must never exceed. Kept separate from
// VehicleCalibration (what the vehicle does measure) and from RunConfig (what
// the event supervisor announces).
//
// Like calibration, every value is supplied by the caller with its evidence.
// None can be filled in yet: no motor, driver or battery behavior has been
// measured. isFullyEstablished() is expected to stay false until real
// measurements exist, and a later arming authority must refuse to arm while it
// is false.
struct SafetyLimits {
    // Highest effort the firmware may ever command, permille of full scale
    // (0-1000).
    Sourced<uint16_t> maxEffortPermille;

    // Highest estimated speed allowed in each direction, metres per second.
    Sourced<double> maxForwardVelocityMps;
    Sourced<double> maxReverseVelocityMps;

    // Longest a single powered run may last, seconds.
    Sourced<double> maxRunDurationS;

    // Lowest battery voltage at which motion is allowed, volts.
    Sourced<double> minBatteryV;

    // True only when every limit is backed by recorded measurement.
    //
    // This is a data-completeness check, not an arming decision; deciding when
    // the vehicle may be armed belongs to a later stage.
    constexpr bool isFullyEstablished() const {
        return maxEffortPermille.isMeasured() && maxForwardVelocityMps.isMeasured() &&
               maxReverseVelocityMps.isMeasured() && maxRunDurationS.isMeasured() &&
               minBatteryV.isMeasured();
    }
};

// Pin the provenance contract: a safety ceiling with an unstated source is a
// compile error. This matters most here, because a guessed limit cannot be
// distinguished from a measured one at runtime.
static_assert(!std::is_default_constructible<SafetyLimits>::value,
              "no silent defaults; every value must be supplied with its source");

}  // namespace ev
