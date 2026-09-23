#pragma once

#include <cstdint>

namespace ev {

// Raw inputs exactly as they were at one instant. No derived values and no unit
// conversion happen here.
struct SensorSnapshot {
    constexpr SensorSnapshot(uint32_t elapsedMs_, int32_t motorEncoderTicks_,
                             uint16_t batteryMv_, bool batteryReadingValid_)
        : elapsedMs(elapsedMs_),
          motorEncoderTicks(motorEncoderTicks_),
          batteryMv(batteryMv_),
          batteryReadingValid(batteryReadingValid_) {}

    // Clock reading from millis() when the snapshot was taken, milliseconds.
    uint32_t elapsedMs;

    // Signed cumulative motor-encoder count. The sign convention is UNVERIFIED
    // (VERIFIED_STATE.md #4).
    int32_t motorEncoderTicks;

    // Battery voltage in millivolts. Only meaningful when batteryReadingValid
    // is true; battery sensing is not verified yet.
    uint16_t batteryMv;
    bool batteryReadingValid;
};

// Motion estimate derived from a sensor snapshot and the vehicle calibration.
// A snapshot computed from provisional calibration is not usable for control,
// so validity travels with the data instead of being assumed by the consumer.
struct OdometrySnapshot {
    constexpr OdometrySnapshot(uint32_t elapsedMs_, double distanceM_, double velocityMps_,
                               bool valid_)
        : elapsedMs(elapsedMs_),
          distanceM(distanceM_),
          velocityMps(velocityMps_),
          valid(valid_) {}

    // Clock reading matching the source SensorSnapshot, milliseconds.
    uint32_t elapsedMs;

    // Distance travelled from the run origin, metres, signed by direction.
    double distanceM;

    // Estimated speed, metres per second, signed by direction.
    double velocityMps;

    // False when the estimate rests on missing sensor data or on provisional
    // calibration. Consumers must not use an invalid estimate for control.
    bool valid;
};

}  // namespace ev
