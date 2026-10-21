# P-H1B - Battery, current, nFAULT and trigger verification

> **Part 3 is not applicable to the assembled vehicle (D32):** the external
> nFAULT signal is not wired. Parts 1, 2 and 4 still apply.

**Primary class:** E (electrical gate).
**Also contains:** S (how the firmware reports each raw input).
**Order:** after H0 and H1A.
**Answers:** what does each analog input read against a measured reference; what
are the nFAULT and trigger idle and active levels; does the trigger bounce.
**Bench instrumentation required:** B4 (raw input readout, `bench inputs` in the
bench image); the battery, current and nFAULT raw values are also in the telemetry
sample, so either source may be recorded - but **say which one** in every reading,
because only the bench image reports the trigger's raw level.

> **Superseded in part (D32, 2026-10-03).** The assembled vehicle does **not**
> wire the DRV8874's nFAULT output to the Nano. `vehicleConfig::
> kPlannedDriverFaultCapability` is `NotInstalled`, the V5 pin map has no pin
> for the line (`kMotorFaultBar` is `kNoPin`), `SensorInputs.h` does not
> configure or sample any nFAULT pin, and the capability-aware
> `DriverFaultConfigurationValid` check treats Part 3's polarity measurement as
> **not applicable** rather than outstanding - so on this vehicle Part 3 has no
> subject and cannot unblock anything. The rest of this protocol (Parts 1, 2 and 4) stands unchanged, and
> the arming semantics below are preserved for a vehicle that *does* wire the
> line. This note supersedes the protocol text; it does not rewrite it.

**This protocol also unblocks arming.** The `DriverFaultConfigurationValid`
configuration check is false for as long as a vehicle that *has* the line leaves
its polarity unverified, and the arm gate refuses on it. That is deliberate: until
Part 3 has been read, no firmware path may claim a driver fault (`DriverFault.h`),
so an armed vehicle would be one whose fault input nobody reads. (On the assembled
vehicle the line is not installed, so the check passes on the capability instead
of on a measurement - see the note above.)

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
- Access to the trigger's two terminals; on a vehicle that wires the **nFAULT**
  line, a way to pull it to ground and release it (it is open-drain on the driver
  side, so pulling it low is safe). On the assembled vehicle there is no line
  (`kMotorFaultBar` is `kNoPin`, D32), so Part 3 is recorded **N/A** and this
  prerequisite is not consumed.

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

## Part 3 - nFAULT input (on a vehicle that wires the line; **N/A on the assembled vehicle**, D32)

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
`docs/bringup/README.md`), **and** set `kPlannedDriverFaultCapability` back to
`Installed`. After that, `checks` should report
`DriverFaultConfigurationValid=1`, and a fault claim on this line becomes a latched
`FaultCode::DriverFaultBar` that stops the vehicle - which P-H9 then observes end
to end. A value that is `Measured` without a citation fails
`MeasuredValuesCiteEvidence` and the arm gate still refuses.

## Part 4 - Start trigger

1. Record the idle level, the level with the contact closed, and the reported
   `startTriggerActive` for each, with B4.
2. Actuate the trigger at least twenty times and record the raw level transitions
   with timestamps (or, if B4 logs only level changes, the count of transitions
   per actuation). Since the standalone work the run-start path is
   **edge-driven and debounced** (`lib/evcore/src/StartTrigger.h`): 100 ms of
   continuous release to qualify after boot/reset, then 40 ms of continuous
   contact to accept one press. The raw transitions measured here are what
   validate those two intervals; B4 still reports the unfiltered level.
3. **Decision rule.** If the measured bounce is longer than the 40 ms contact
   debounce, raise `ev::kDefaultStartTriggerConfig` (through
   `vehicleConfig::startTriggerConfig()`) to a bound the measurement supports,
   and re-run the standalone trigger test. It is not something to approximate by
   lowering a trigger threshold, and the value must not be shortened to make a
   press feel more responsive.

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

Battery: at least five voltages x three repeats. nFAULT line-pull: at least ten
**on a vehicle that wires the line (N/A on the assembled vehicle, D32)**.
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
recorded **on a vehicle that wires the line (N/A on the assembled vehicle,
D32; Parts 1, 2 and 4 still pass without it)** with the trip behaviour marked
unestablished; the trigger idle/active
levels recorded and the bounce quantified with a debounce-needed verdict.

## If it passes

- #7 only if the battery fit criterion was met; otherwise leave #7 UNVERIFIED
  with the raw pairs recorded in the log.
- Add rows for the measured trigger active level (new fact, or fold it into #1
  as the input-level part of the wiring fact); the nFAULT idle level on a
  vehicle that wires the line, N/A on the assembled vehicle (D32).
- Log in `DEVELOPMENT_LOG.md`, including the explicit "unestablished" statements,
  so a later reader does not mistake a recorded raw count for a measurement.

## If it fails or is inconclusive

A wrong sense point or a missing/incorrect pull-up is the first suspect, then a
divider or wiring error, then an ADC reference assumption. Do not implement a
conversion from an unconvincing fit, and do not let the trigger start a run until
its level and bounce are understood.
