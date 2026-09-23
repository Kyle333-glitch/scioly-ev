#pragma once

#include <cstdint>

namespace ev {

// Run states.
//
// This is vocabulary only: no transition logic exists in this stage, and the
// list may change as the physical design becomes clear (AGENTS.md section 12).
// SafeDisarmed is intentionally the zero value, so a default-initialized state
// is the safe one.
enum class CompetitionState : uint8_t {
    SafeDisarmed = 0,
    Ready = 1,
    ForwardAccel = 2,
    ForwardCruise = 3,
    ForwardDecel = 4,
    BottleApproach = 5,
    BottlePush = 6,
    ForwardBrake = 7,
    Settle = 8,
    ReverseAccel = 9,
    ReverseCruise = 10,
    FinalApproach = 11,
    FinalBrake = 12,
    Finished = 13,
    Fault = 14,
};

enum class FaultCode : uint8_t {
    // No fault. The value a default-constructed FaultState carries.
    None = 0,
    // The announced run configuration failed rule validation.
    InvalidRunConfig,
    // A required calibration value is still provisional.
    ProvisionalCalibration,
    // A required safety limit is still provisional.
    MissingSafetyLimits,
    // A commanded motor effort was invalid.
    InvalidMotorEffort,
};

// A fault together with when it happened and whether it is latched. Fault
// handling behavior belongs to a later stage; this stage only carries the data.
struct FaultState {
    FaultCode code = FaultCode::None;
    bool latched = false;
    uint32_t raisedAtMs = 0;
};

}  // namespace ev
