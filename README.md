# Science Olympiad Electric Vehicle

Firmware and supporting tooling for the 2026-2027 Science Olympiad Division C Electric Vehicle.

## Hardware baseline

- Controller: Arduino Nano R4
- Build system: PlatformIO
- Framework: Arduino

## Authority files

Read these before changing firmware:

- `AGENTS.md` - process, safety rules, and development philosophy.
- `SOFTWARE_ARCHITECTURE.md` - frozen architecture contracts (layering, safety
  invariants, naming, calibration).
- `VERIFIED_STATE.md` - which hardware facts are actually verified, and what
  evidence is required to verify them.
- `DEVELOPMENT_LOG.md` - append-only history of accepted milestones and
  decisions.
- `docs/rules/README.md` - the official event rules the run configuration is
  validated against.

A successful build is not evidence that the vehicle works. Physical
measurements override assumptions.

## Project layout

- `src/` - vehicle firmware. The only place allowed to use Arduino headers. It
  holds the cooperative runtime shell (`VehicleRuntime.*`), the provisional pin
  map (`PinMap.h`), the motor driver adapter (`MotorDriver.h`), the encoder
  interrupt (`EncoderInput.*`), the raw input adapters (`SensorInputs.h`), the
  watchdog checkpoint (`WatchdogCheckpoint.h`), the non-blocking serial line
  transport (`SerialTransport.h`) and the vehicle configuration
  (`VehicleConfig.h`).
- `lib/` - pure logic modules with no Arduino dependency, covered by host tests.
- `test/` - host-side unit tests (not shipped in the firmware).
- `scripts/` - build-tooling helpers.
- `docs/` - reference material, including the event rules the firmware is built
  against and the planned hardware interfaces.
- `docs/hardware/` - the planned hardware interface and the bench tests that
  would replace it with measured facts. Nothing in it is evidence about the
  vehicle yet.

## Building

Build the Nano R4 firmware:

    pio run

Upload when the Nano R4 is connected:

    pio run -t upload

Serial monitor:

    pio device monitor

## Host tests

Logic in `lib/` is tested on the development computer, independent of the
vehicle:

    pio test -e native

The `native` environment builds `lib/` and `test/` only; it never compiles the
Arduino `src/` tree. If no system C/C++ compiler is installed, PlatformIO's
managed MinGW toolchain is used automatically.

Host tests prove logic only. They are never a substitute for testing on the
real vehicle.

## Staged workflow

Work proceeds in small stages, each leaving a known-good version behind. The
full progression is listed in `AGENTS.md` (section 3), and the accepted ones are
recorded in `DEVELOPMENT_LOG.md`.

- **P0** - development environment, project truth/provenance, and the host-test
  path.
- **P1-P5** - the pure core in `lib/evcore/`: data contracts and rule-valid run
  configuration, the final permission boundary, quadrature decoding,
  tick-to-distance arithmetic, velocity estimation and control, target shaping,
  controlled reversal, and the competition run state machine.
- **P6** - the scripted whole-run scenario and fault harness in
  `test/test_scenario/`.
- **P7A** - the cooperative runtime shell and the Nano R4 hardware boundary: the
  control tick and its scheduler, the encoder interrupt, the raw input adapters,
  the motor driver adapter, and the single central watchdog health checkpoint. No
  physical behavior is claimed by any of it.
- **P7B (current)** - the operator interface and machine-readable telemetry: the
  bounded serial command parser, the operator policy that admits or refuses each
  command, the response formatter, the versioned telemetry schema with its drop
  counters, and the non-blocking line transport. Both are consumers of the control
  chain; neither can change a run.
- **Next** - physical bring-up: the pending tests recorded in
  `DEVELOPMENT_LOG.md` (D13, D14) and in `VERIFIED_STATE.md`, starting with wiring
  continuity and the motor driver's idle levels and truth table.

Do not build later-stage vehicle behavior before the earlier stage has been
validated.

## Current state

The pure core, the runtime shell and the operator interface exist, and the
firmware builds for the Nano R4. On startup the vehicle layer drives the motor
driver to its de-energized state before anything else runs, then runs a 50 Hz
control tick in a frozen stage order. After each pass it services the serial
port: a staged reply goes out first, then at most one received command is applied
and answered, then the telemetry queue drains.

It cannot move: every calibration value, safety limit and gain in
`src/VehicleConfig.h` is a `Provisional` placeholder, the regulator refuses to
enable, no run has been announced, and the run policy stays in `Idle`. The serial
interface can report `status`, `checks`, `version` and the held configuration,
and can ask to `announce` or `arm` - but with the placeholder configuration every
arming intent is refused, and the refusal names the check that failed. No command
commands an effort or a direction, and none starts a run. The watchdog is
deliberately disabled pending its own on-target check.

The vehicle still has no measured behavior: no motor, encoder, sensor, trigger,
watchdog, serial timing or bottle mechanism behavior has been observed on
hardware. See `VERIFIED_STATE.md`.
