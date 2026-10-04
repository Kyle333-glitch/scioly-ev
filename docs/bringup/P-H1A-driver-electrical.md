# P-H1A - Driver electrical logic, motor disconnected

**Primary class:** E (electrical gate).
**Order:** after H0. Independent of H0W, but run H0W first so a reset is already
characterized.
**Answers:** is the harness wired as `src/PinMap.h` claims; what do the three
driver inputs do at the pins; what is the driver's output state for each input
combination including the awake-`EN`-low state; what is the PWM behaviour.
**Bench instrumentation required:** none beyond the shipped de-energized startup
and the level observations. Where a specific input combination must be held, use
B2's bench-only path or a bench fixture, not the production run path.

**This protocol cannot establish motor polarity (#3).** There is no motor
connected, so nothing turns. Polarity is measured in H3, once the vehicle's
forward direction has been defined mechanically.

## Prerequisites

- H0 passed.
- **The motor is disconnected from the driver outputs**, both leads. Record how
  (which connector), because a wrong reassembly invalidates H3.
- Wheels off the floor; drivetrain unloaded.
- Bench supply for the motor rail with a **low** current limit (record it), plus
  a meter. The MCU is powered as it will be in the vehicle.
- The firmware image identity recorded (`VERSION,fw=...`, `git rev-parse HEAD`).

## Setup, step A0 - wiring continuity, power off

With all power removed, continuity-check every pin named in `src/PinMap.h`
against the physical harness: encoder A/B, PH, EN, nSLEEP, trigger. The nFAULT
line is not wired on this vehicle (D32), and the map's battery/current-sense
constants are not installed on it either, so there is no harness line to check
for them. Record the table (MCU pin -> net -> connector -> device pin). A wrong
`EN`/`PH` pin is a direction and speed surprise, which is why this is first and
with power off. Re-check after any rewire.

## Setup, step A1 - idle levels, driver asleep

Power the MCU only (motor supply off is fine for level measurement, on and
current-limited if the driver needs its rail to drive its inputs). With the
firmware at rest, measure `nSLEEP`, `PH`, `EN` and `nFAULT` at the MCU pins. This
is the firmware's de-energized startup state; record whether the pins actually do
what `MotorDriver.h` drives (`nSLEEP` low, `PH` low, `EN` at zero duty) and
whether `nFAULT` reads high through the planned pull-up. Record the logic levels
in volts, and whether any line needs a level shifter or a pull-up/down at all.

## Setup, step A2 - truth table, motor disconnected

With a low supply current limit, apply each row for a short bounded time and
record the *output* state measured at the two output terminals (voltage to
ground and terminal-to-terminal, plus the supply current):

| `nSLEEP` | `PH` | `EN` | Intent to test |
|---|---|---|---|
| low | low | 0 | Disabled (the safe state) |
| high | low | 0 | awake with no drive - **brake or high-Z?** |
| high | high | 0 | awake, other PH, no drive |
| high | low | low duty | drive, one direction |
| high | high | low duty | drive, the other direction |
| high | each | 10% / 50% / 100% | duty response |

For the awake-`EN`-low row, a state where both outputs are pulled low is
electrically different from a high-impedance state; record which it is, because
`docs/hardware/motor-driver-drv8874.md` explicitly does not claim which. Do not
label the row "brake" or "coast" until the measurement says so.

## Held constant

Supply voltage and current limit, probe setup and meter, firmware image, ambient
conditions, wiring.

## Varied

One input combination at a time, as in the table; nothing else changes between
rows.

## Measurements and logs

- continuity table from A0;
- pin voltages for each driven level in A1 and A2;
- output-terminal state and supply current for each table row;
- the measured PWM frequency at `EN`, and the smallest duty that produces a
  measurable output change;
- whether the driver is genuinely asleep in the Disabled row (supply current);
- the `nFAULT` idle level in each row;
- the raw capture of any telemetry so the firmware's view of the levels can be
  compared with the meter.

## Repetitions

Three full passes over the table, with the meter reconnected each pass, so a
wiring error is distinguishable from a measurement error. Continuity once, then
re-checked after any rewire.

## Safe-stop (abort) conditions

Nothing is connected to the outputs, so the hazard is the supply, not motion.
Abort and disconnect on any unexpected current, heat, oscillation or smell. The
current limit is the primary protection.

## Acceptance / decision rule

Pass requires: the continuity table matches `src/PinMap.h` (or the map is
corrected and re-verified); the three repeats agree on the output state for every
row; the Disabled combination produces no output; the awake-`EN`-low state is
identified as brake or high-Z; the `EN` PWM frequency and duty response are
recorded. If the measured mapping contradicts the plan in
`docs/hardware/motor-driver-drv8874.md`, the fix is the plan table or the wiring -
never a sign added elsewhere.

## If it passes

- `VERIFIED_STATE.md` #1 (wiring and pin assignments, for the pins checked) and
  #2 (driver modes / truth table, for the level-to-output mapping measured).
- Update the tables in `docs/hardware/motor-driver-drv8874.md` and mark the
  resolved open questions there (the awake-`EN`-low state, the PWM behaviour, the
  logic levels).
- Log in `DEVELOPMENT_LOG.md`.
- #3 stays UNVERIFIED: it needs a motor.

## If it fails or is inconclusive

A wiring error is the first suspect, then a logic-level mismatch, then a driver
board fault. Do not proceed to any powered-motor protocol (H3 and later) until
this passes, because every direction and speed result downstream would be built
on an unverified mapping.
