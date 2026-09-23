# Software Architecture

**Status: FROZEN CONTRACTS.**

This file states only decisions the project has already committed to. It is not a
roadmap, a research log, or a design history. Design exploration and why-decisions
belong in `DEVELOPMENT_LOG.md`. Physical facts and their evidence belong in
`VERIFIED_STATE.md`.

Changing a contract in this file is a deliberate act. Record the reason in
`DEVELOPMENT_LOG.md` in the same change.

---

## 1. Target contract

- The vehicle firmware targets the **Arduino Nano R4** through PlatformIO
  environment `nano_r4` (Arduino framework). `pio run` builds this firmware.
- Competition behavior runs entirely on the microcontroller. No runtime
  dependency on a host computer, network, cloud service, or external API.
- The toolchain is not replaced without a specific technical reason and
  explicit approval.

## 2. Layering and dependency contract

Two source roots exist, and their responsibilities do not overlap:

- `src/` — vehicle firmware. This is the only place allowed to include Arduino
  headers (`Arduino.h`, `Serial`, `pinMode`, `millis()`, and so on).
- `lib/` — pure logic modules. **`lib/` code must not include Arduino headers,
  and must not depend on `src/`.**

Dependency direction is one-way: `src/` → `lib/`. Never `lib/` → `src/` and never
`lib/` → Arduino.

`test/` holds host tests. It is not shipped code and is never included by the
vehicle firmware.

## 3. Host-testability contract

- Any module placed in `lib/` must compile under PlatformIO environment
  `native` and must have at least one host test in `test/`.
- Host tests run with `pio test -e native`.
- The `native` environment must never compile `src/` (`test_build_src = no`).
  Arduino-dependent code cannot and must not be forced into host tests.
- Host tests verify logic only. A passing host test is never evidence that the
  vehicle behaves correctly; see `VERIFIED_STATE.md`.

## 4. Nonblocking execution contract

- The main loop must not block on long `delay()` calls.
- Elapsed-time logic uses `millis()` with **unsigned subtraction**, which is
  correct across `uint32_t` wraparound.
- Short waits required by hardware during initialization are the only
  permitted exception, and they must be bounded.
- The control loop must stay responsive enough to detect faults, update
  measurements, advance state, update motor commands, and emit telemetry.

## 5. Safety contract

These invariants are frozen and are not negotiable for convenience:

- The motor is commanded OFF or disabled during boot, during initialization,
  after reset, after an unrecoverable error, when an invalid state is detected,
  before entering an armed state, and after a command/communication timeout.
- The firmware never resumes a previous motor command after reboot or reset.
- Powered motion requires an intentional transition into an armed/run state.
- Diagnostic and test entry points obey the same constraints. Malformed or
  unexpected input must never produce uncontrolled motion.

## 6. Units and naming contract

- Identifiers for physical quantities carry their units, and the unit is never
  implied: `targetDistanceM`, `targetTimeS`, `velocityMps`, `batteryV`,
  `maxEffortPermille`.
- Pure-core code uses SI: metres, seconds, metres per second, volts. The event
  rules are written in these units, so rule values and firmware values stay
  directly comparable. `AGENTS.md` section 10 gives millimetre examples, which
  remain valid for code written in millimetres, but a single module never mixes
  prefixes internally.
- Clock readings taken from `millis()` keep an `Ms` suffix and stay in
  milliseconds (`elapsedMs`), because that is what the platform provides. A
  timestamp is a clock reading, not a physical duration; durations use seconds.
- Ambiguous names such as `dist`, `time`, `speedVal`, and `offset` are not used.
- Numeric literals that represent calibration, rule, or hardware facts are not
  scattered through logic; see sections 7 and 8.

## 7. Calibration, limits, and provenance contract

- Fixed hardware configuration, measured vehicle calibration
  (`VehicleCalibration`), safety ceilings (`SafetyLimits`), and per-run event
  parameters (`RunConfig`) are separate concepts with separate types. They are
  never merged into one bag of values.
- Every calibration or limit value carries its evidence in the type
  (`Sourced<T>` with `ValueSource`): `Measured`, `RuleDerived`, or
  `Provisional`. There are no default values, so an unmeasured quantity cannot
  be silently filled in.
- A `Provisional` value is a placeholder, never a measured fact. It stays
  visible as such and is never sufficient to authorize powered motion.
  `VehicleCalibration::isFullyMeasured()` and
  `SafetyLimits::isFullyEstablished()` exist so a later stage can refuse to arm
  on incomplete data. They are data-completeness checks, not the arming
  decision, and not a substitute for the arming authority.

## 8. Rule source contract

- The authoritative event rules are the official 2027 Division C rules at
  `docs/rules/`. Values taken from them cite the rule number where they are
  defined.
- Rule-derived numbers live in exactly one place, `ev::rules2027`. No other
  code restates a rule value locally.
- Per-run configuration validation has exactly one entry point,
  `ev::validateRunConfig()`. No other module re-implements those checks.
- A rule citation establishes what the event requires. It is never evidence
  about the vehicle: physical facts remain governed by `VERIFIED_STATE.md`.

## 9. Telemetry contract

- Telemetry is machine-readable and emitted in a form that does not disturb
  timing-sensitive behavior.
- Serial logging is never performed inside an interrupt handler.
- Serial output is not flooded so heavily that it changes control-loop timing.

## 10. Deliberately undecided

The following are **not** decided and must not be pre-built or guessed. They
depend on facts tracked in `VERIFIED_STATE.md`:

- fixed hardware configuration: pin assignments, wiring, logic levels;
- the motor driver interface, its truth table, and how `MotorMode` maps onto it;
- encoder interpretation (counts per revolution, x1/x2/x4, direction sign);
- conversion from encoder counts to vehicle displacement, and every measured
  calibration value;
- every `SafetyLimits` value;
- control strategy and any gains;
- run state transitions and thresholds. The `CompetitionState` names exist only
  as vocabulary and may change with the physical design;
- telemetry schema;
- whether any additional sensing (ground/dead-wheel encoder, IMU) is justified.

Adding any of these requires either verified evidence or an explicit statement
that the value is an unverified, configurable placeholder.
