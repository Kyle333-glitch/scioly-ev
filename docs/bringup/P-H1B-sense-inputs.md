# P-H1B - Battery, current, nFAULT and trigger verification

**Primary class:** E (electrical gate).
**Also contains:** S (how the firmware reports each raw input).
**Order:** after H0 and H1A.
**Answers:** what does each analog input read against a measured reference; what
are the nFAULT and trigger idle and active levels; does the trigger bounce.
**Bench instrumentation required:** B4 (raw input readout, `bench inputs` in the
bench image); the battery, current and nFAULT raw values are also in the telemetry
sample, so either source may be recorded - but **say which one** in every reading,
because only the bench image reports the trigger's raw level.

**This protocol also unblocks arming.** The `DriverFaultPolarityVerified`
configuration check is false for as long as the driver's fault output has an
unverified polarity, and the arm gate refuses on it. That is deliberate: until
Part 3 has been read, no firmware path may claim a driver fault (`DriverFault.h`),
so an armed vehicle would be one whose fault input nobody reads.

**Scope.** This protocol establishes what each raw input reads. It deliberately
does **not** establish the current scaling (there is no load with the motor
disconnected) and does **not** establish what a real nFAULT trip does (that needs
a load and is P-H9). Those stay open until the protocols that can isolate them.

## Prerequisites

- H0 and H1A passed.
- Motor still disconnected from the driver outputs; driver rail powered with a
  low current limit; drivetrain unloaded.
- A bench supply whose voltage can be set and **measured** with a meter; the
  meter and its resolution recorded.
- Access to the trigger's two terminals; a way to pull the nFAULT line to ground
  and release it (the line is open-drain on the driver side, so pulling it low is
  safe).

## Setup

Firmware running, telemetry on at a rate high enough to see the input changes,
capture to a file. Note the power source of the MCU (USB or the pack), because
the ADC reference depends on it and the same configuration must be used in the
vehicle.

## Part 1 - Battery sense

Vary the supply across at least five voltages spanning the intended operating
range, including the pack's nominal and its planned minimum. At each, record the
supply voltage **measured at the sense point** with the meter and the
`batt_counts` the telemetry reports, three repeats each. Do not use the supply's
own display as the reference.

**Decision rule.** If a single stable linear relationship holds across the whole
measured range within the pre-registered tolerance, record it as the measured
scaling (#7) with the fit, the residuals and the raw pairs. If it does not hold,
record the raw pairs only and state that the conversion is **not established** -
do not fit two points into a curve, and do not invent a divider ratio. Until a
conversion exists in code, no reader may treat `batt_counts` as volts.

## Part 2 - Current sense

There is no load with the motor disconnected, so only the quiescent level is
measurable here. Record `curr_counts` at zero load for each supply voltage, and
state explicitly that the current scaling is unestablished and is deferred to H3
(with a load) and H9 (a deliberate trip). The DRV8874's current-sense output is
proportional to load current, but what that proportion is on this board is a fact
this protocol cannot supply.

## Part 3 - nFAULT input

1. **MCU input alone.** With the driver connected but not in a fault, pull the
   nFAULT line to ground and release it, at least ten times. Record the
   `nfault_low` the telemetry reports each time. This verifies the MCU input and
   its pull-up, and that the firmware sees the line change.
2. **Driver idle level.** With the driver awake and no fault, record the idle
   level and what `nfault_low` reports. Toggle `nSLEEP` and record whether the
   line stays high, releases, or does something else.
3. **Which level means fault.** From the two levels observed above, record the
   polarity this line uses: `ActiveLow` if a low level is the fault claim,
   `ActiveHigh` if a high level is. Do not assume the datasheet's expected
   polarity - the wiring is what is being measured (`VERIFIED_STATE.md` #1).
4. Record, explicitly, that a genuine overcurrent/thermal trip is **not**
   established here. That needs a load and belongs to P-H9. What this part
   establishes is the *level's meaning*, not the trip behaviour.

**If it passes:** set `kPlannedDriverFaultPolarity` in `src/VehicleConfig.h` from
step 3, as a **`Measured`** value with a citation (the two levels recorded here and
the capture under `data/evidence/`, per the promotion steps in
`docs/bringup/README.md`). After that, `checks` should report
`DriverFaultPolarityVerified=1`, and a fault claim on this line becomes a latched
`FaultCode::DriverFaultBar` that stops the vehicle - which P-H9 then observes end
to end. A value that is `Measured` without a citation fails
`MeasuredValuesCiteEvidence` and the arm gate still refuses.

## Part 4 - Start trigger

1. Record the idle level, the level with the contact closed, and the reported
   `startTriggerActive` for each, with B4.
2. Actuate the trigger at least twenty times and record the raw level transitions
   with timestamps (or, if B4 logs only level changes, the count of transitions
   per actuation). There is no debounce in the firmware; this measures whether
   one is needed.
3. **Decision rule.** If the bounce exceeds the pre-registered bound, a debounce
   is required before the trigger may start a run. That is a software
   prerequisite to close before H7; it is not something to approximate by
   lowering a trigger threshold.

## Held constant

Supply wiring, MCU power source, meter, firmware image, ambient conditions.

## Varied

One input at a time: the battery-sense supply voltage; the nFAULT line's level;
the trigger contact. Nothing else changes between readings.

## Measurements and logs

Per part: the metered supply voltage and the raw `batt_counts` (part 1); the
zero-load `curr_counts` (part 2); the `nfault_low` value for each line state and
the idle level with the driver awake (part 3); the `startTriggerActive` value and
the raw level transitions for each actuation (part 4). Record the meter, its
resolution and the MCU power source with every reading.

## Repetitions

Battery: at least five voltages x three repeats. nFAULT line-pull: at least ten.
Trigger: at least twenty actuations.

## Safe-stop (abort) conditions

Motor disconnected, supply current-limited. Abort on heat, unexpected current, or
if the driver rail sags. No motion is possible with the motor disconnected; if
anything moves, stop - the motor was not actually disconnected.

## Acceptance / decision rule

Pass requires: the battery raw pairs recorded over the range, with a
clearly-stated established or not-established verdict against the pre-registered
fit criterion; the zero-load current count recorded with the scaling marked
unestablished; the nFAULT idle level, release behaviour and MCU-input tracking
recorded with the trip behaviour marked unestablished; the trigger idle/active
levels recorded and the bounce quantified with a debounce-needed verdict.

## If it passes

- #7 only if the battery fit criterion was met; otherwise leave #7 UNVERIFIED
  with the raw pairs recorded in the log.
- Add rows for the measured trigger active level and the nFAULT idle level (new
  facts, or fold them into #1 as the input-level part of the wiring fact).
- Log in `DEVELOPMENT_LOG.md`, including the explicit "unestablished" statements,
  so a later reader does not mistake a recorded raw count for a measurement.

## If it fails or is inconclusive

A wrong sense point or a missing/incorrect pull-up is the first suspect, then a
divider or wiring error, then an ADC reference assumption. Do not implement a
conversion from an unconvincing fit, and do not let the trigger start a run until
its level and bounce are understood.
