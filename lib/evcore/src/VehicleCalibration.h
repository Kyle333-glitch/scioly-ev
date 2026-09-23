#pragma once

#include "Units.h"

#include <type_traits>

namespace ev {

// Properties measured from one specific vehicle. Every value must come from a
// recorded physical test (see VERIFIED_STATE.md).
//
// There are no defaults on purpose. A value that has not been measured must be
// labelled ValueSource::Provisional, and a provisional value must never
// authorize powered motion.
struct VehicleCalibration {
    // Effective forward travel per motor-encoder tick, in metres. Depends on
    // the encoder interpretation (counts per revolution, x1/x2/x4, gearing) and
    // on rollout, none of which is verified yet.
    Sourced<double> distancePerEncoderTickM;

    // Extra travel after commanding a forward stop, in metres. Comes from
    // braking-distance characterization.
    Sourced<double> forwardBrakeOffsetM;

    // Extra travel after commanding a reverse stop, in metres.
    Sourced<double> reverseBrakeOffsetM;

    // True only when every value is backed by a recorded measurement.
    //
    // This is a data-completeness check, not an arming decision; deciding when
    // the vehicle may be armed belongs to a later stage.
    constexpr bool isFullyMeasured() const {
        return distancePerEncoderTickM.isMeasured() && forwardBrakeOffsetM.isMeasured() &&
               reverseBrakeOffsetM.isMeasured();
    }
};

// Pin the provenance contract: a calibration with an unstated source is a
// compile error, not something to be filled in later.
static_assert(!std::is_default_constructible<VehicleCalibration>::value,
              "no silent defaults; every value must be supplied with its source");

}  // namespace ev
