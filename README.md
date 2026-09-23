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

- `src/` - vehicle firmware. The only place allowed to use Arduino headers.
- `lib/` - pure logic modules with no Arduino dependency, covered by host tests.
- `test/` - host-side unit tests (not shipped in the firmware).
- `scripts/` - build-tooling helpers.
- `docs/` - reference material, including the event rules the firmware is built
  against.

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
full progression is listed in `AGENTS.md` (section 3).

- **P0** - development environment, project truth/provenance, and the host-test
  path. Complete; no vehicle behavior.
- **P1 (current)** - pure-core data contracts and rule-valid run configuration
  in `lib/evcore/`. Still no vehicle behavior.
- **P2 (next)** - arming authority and motor-driver bring-up with the drivetrain
  unloaded.

Do not build later-stage vehicle behavior before the earlier stage has been
validated.

## Current state

The development environment, the project authority files, and the host-test path
exist. `lib/evcore/` holds the shared data vocabulary and the rule-validated run
configuration. There is no motor, encoder, controller, state machine, telemetry
or serial-command code, and no vehicle hardware behavior has been validated.
