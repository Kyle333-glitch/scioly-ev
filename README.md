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
- `tools/` - PC-side analysis tooling. `tools/telemetry/` parses the firmware's
  telemetry schema, computes descriptive metrics, writes reproducible SVG charts
  and compares a run's believed endpoints against hand measurements. Standard
  library only; see `tools/telemetry/README.md`. It describes what the firmware
  reported and reaches no conclusion about the vehicle.
- `docs/` - reference material, including the event rules the firmware is built
  against and the planned hardware interfaces.
- `docs/hardware/` - the planned hardware interface and the bench tests that
  would replace it with measured facts. Nothing in it is evidence about the
  vehicle yet.
- `docs/bringup/` - the exact physical bring-up and experiment protocols, one per
  pending hardware fact, each with prerequisites, held and varied variables, the
  measurements and repetitions, safe-stop conditions, the acceptance rule, and
  which `VERIFIED_STATE.md` entry it may update. Pre-written procedures, not
  evidence, and not run.

## Building

Build the Nano R4 firmware:

    pio run

Upload when the Nano R4 is connected:

    pio run -t upload

Serial monitor:

    pio device monitor

## Bench instrumentation (not the vehicle image)

The bring-up protocols in `docs/bringup/` need readouts and one bounded motion
command that the vehicle image deliberately does not contain. Those live in
`src/bench/` and are built only by the `bench` environment:

    pio run -e bench -t upload

`pio run` never builds it, the production image does not contain it, and neither
image contains the other's entry point (`platformio.ini`, `SOFTWARE_ARCHITECTURE.md`
section 17). A bench session's lines all start with `BENCH`; `bench health`
reports `image=bench`, and the analyzer treats a `BENCH` line as an ignored line,
not as telemetry.

The verbs are `bench help`, `bench arm`, `bench drive <fwd|rev> <permille> <ms>`,
`bench stop`, `bench reset`, `bench health`, `bench drive-status`, `bench ticks`
and `bench inputs`. Nothing is applied before a deliberate `bench arm`; every
command expires on its own; an abort or an encoder that stops changing while
driving latches until `bench reset`; and the physical kill switch is still the
primary stop in every powered step. No protocol may use the bench drive until its
bounded expiration and its latched fault have been demonstrated (that is H3's
first step).

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
- **P7B** - the operator interface and machine-readable telemetry: the bounded
  serial command parser, the operator policy that admits or refuses each command,
  the response formatter, the versioned telemetry schema with its drop counters,
  and the non-blocking line transport. Both are consumers of the control chain;
  neither can change a run.
- **P8A** - PC-side analysis tooling in `tools/telemetry/`, so a capture
  can be interpreted as soon as hardware arrives: the schema mirror, the strict
  parser, descriptive metrics, reproducible SVG charts and the endpoint
  comparison. It is validated against clearly-labelled synthetic fixtures and
  makes no claim about any vehicle.
- **P8B** - exact physical bring-up and experiment protocols in
  `docs/bringup/`: upload/boot/serial link, the watchdog qualification and its
  timeout choice, the driver electrical checks, the sense inputs, hand-driven
  encoder counting, the first powered drive, rollout and slip, the velocity
  trace, tuning, distance/braking/reversal, the bottle, fault injection, and
  full-run repeatability. Each protocol isolates one unknown and names what it
  may record. None of it is evidence and none of it has been run.
- **P9A (current)** - pre-hardware readiness validation: every native test, the
  Nano R4 build and the ARM cross-check over `lib/` re-run clean from a fresh
  build, the P8 tool suite re-run, and a static audit of the tree (no debug
  leftovers, no writer of the motor pins outside the adapter, no blocking call,
  no dynamic allocation, no placeholder marked `Measured`). It adds no behavior
  and changes no status: every `VERIFIED_STATE.md` row stays UNVERIFIED and the
  watchdog stays disabled pending H0W. It found no firmware or tooling defect -
  only a stale evidence tally, corrected in `DEVELOPMENT_LOG.md` D17.
- **P9A audit fixes** - a follow-up audit returned two findings and one recorded
  observation, closed in `DEVELOPMENT_LOG.md` D18. The profile's zero-elapsed-time
  hold can no longer hand back a target in the direction a same-cycle reversal
  just left, and the absent-`VelocityEstimate` shape now has one owner with
  `WarmingUp` pinned at compile time. The observation - that the final stop's
  dwell can be credited across a control-loop stall - is not a firmware defect, so
  no behavior changed; its bound is now a required H0W measurement. Every
  `VERIFIED_STATE.md` row stays UNVERIFIED.
- **P9D / bench instrumentation** - an adversarial first-power software review
  returned findings that are now closed in code (`DEVELOPMENT_LOG.md` D19). The
  watchdog is opened at the end of initialization instead of during it, so a
  requested timeout can no longer expire before the first refresh, and the rule
  and the requested constants are asserted at compile time. The driver's fault
  line is now interpreted from a configured polarity that refuses to guess, and a
  measured value that cites no evidence fails a configuration check. The stop
  dwell can no longer be credited across a control-loop stall. And bench
  instrumentation B1-B4 is **built** (`src/bench/`, `env:bench`): read-only
  health/tick/input readouts and one bounded, latched bench drive, excluded from
  the production image by the build graph
  (`SOFTWARE_ARCHITECTURE.md` section 17). No physical test has been run and every
  `VERIFIED_STATE.md` hardware row is still UNVERIFIED.
- **P9D, second pass (D20)** - a follow-up review of the bench/production seam
  found and closed the composition defects: the bench layer and the control tick
  no longer both write the motor pins in one pass (the bench takes pin ownership
  for the pass, and `failSafe()` never yields), the bench's permission to write is
  now a host-tested predicate that refuses on a latched fault, a violated tick
  order, an armed run or a run under way - not the inverted `!armed` test it used
  to be - and a refusal aborts and latches the bench instrument. H0W's missing
  deliberate-hang hook exists (`kDeliberateHangAfterPasses`), watchdog enablement
  is a per-environment build define (`env:bench-watchdog`) that the vehicle image
  refuses to compile, and H3 now states that the firmware acts on `nFAULT` not at
  all until H1B's polarity is promoted - so the supply's current limit and the
  kill switch are the stop authority. See `DEVELOPMENT_LOG.md` D20.
- **Next** - physical bring-up, executing those protocols in order: P-H0 (upload,
  boot and serial link), then its watchdog qualification P-H0W, then the driver
  electrical checks P-H1A and P-H1B.

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

The one way to apply effort is the bench image (`pio run -e bench`), which is not
the vehicle image: it contains the bench console and the bounded, latched bench
drive, and the vehicle image contains neither. That command is what the
bring-up protocols will use, and it may not be used until its bounded expiration
and its latched fault have been demonstrated at the bench. H0W additionally needs
the watchdog enabled, which is a separate bench environment - `pio run -e
bench-watchdog` - rather than an edit to a header both images compile.

The vehicle still has no measured behavior: no motor, encoder, sensor, trigger,
watchdog, serial timing or bottle mechanism behavior has been observed on
hardware. See `VERIFIED_STATE.md`.
