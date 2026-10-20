# Nano R4 USB startup investigation — 2026-10-19

## Status
Observed USB startup failure mitigated by deferring VehicleRuntime
construction until setup(). Fix committed as e31c82d.

This is a software startup/USB result, not a vehicle electrical
safety qualification.

## Original failure
- Production firmware uploaded successfully through DFU.
- The Nano then failed to enumerate as USB CDC (COM3).
- Windows sometimes showed no corresponding USB device.
- Physical USB power cycling and double-RESET recovered DFU.
- Minimal USB diagnostic firmware restored serial enumeration.

## Controlled experiments
- Minimal USB diagnostic: passed.
- A0, no VehicleRuntime object: passed (operator-reported).
- A1, global VehicleRuntime object: failed USB enumeration.
- A2, construct runtime after USB startup: passed.
- A3, construct inside setup() before Serial.begin(): passed.
- A4, call runtime.begin() before Serial.begin(): passed.
- A5, add production runPass(): passed.
- A6, full production entry point with deferred runtime: passed.

## Production change
Commit: e31c82d
File: src/main.cpp

Replaced the globally constructed VehicleRuntime object with a
function-local static, first constructed when setup() calls
runtimeRef().begin(millis()).

The watchdog, control loop and serial service were otherwise
preserved.

## Validation
- Host tests: 868/868 passed, 43 suites.
- Production build: successful.
- RAM: 7508 / 32768 bytes.
- Flash: 79952 / 262144 bytes.
- DFU upload: successful.
- USB CDC COM3: restored after upload.
- Production version/status/checks: observed responding.
- Firmware reported Idle, unarmed, and motor disabled.
- Cold-boot and USB-reconnection checks: operator-reported passed.

## Limitations
- Exact constructor failure mechanism is not established.
- Distinctness from or relationship to earlier F-3 USB incidents
  has not been proven.
- Dropped telemetry samples were observed; these do not establish
  a USB disconnect.
- Hardware wiring, driver truth table, polarity and powered
  behavior remain unverified under VERIFIED_STATE.md.
- Motor supply remained disconnected for these tests.

## Next gate
Perform H1A wiring continuity and de-energized pin-level checks.
Do not perform powered motor tests until the required electrical
and safety prerequisites have passed with recorded evidence.
