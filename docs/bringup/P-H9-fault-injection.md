# P-H9 - Fault injection

**Primary class:** P (physical behaviour check).
**Also contains:** E (electrical faults under current limit) and S (the fault
vocabulary and latching).
**Order:** after H3 and H7.
**Answers:** does each fault path stop motion, name the fault, and stay latched
until deliberately cleared **in a live session** (the latch is RAM state; a reboot
clears it while booting de-energized and unarmed - see the note below); what does
the firmware currently do about electrical faults it does not yet map; and what
happens when the encoder is **lost or stops changing while the motor is still
driven** - the case that can make the zero-speed gate report a stop the vehicle is
not making.
**Bench instrumentation required:** none beyond the shipped interface for the
state-machine faults; a current-limited supply and a means of briefly loading the
driver for the electrical faults. **The driver-fault part has no subject on the
assembled vehicle (D32):** the external nFAULT signal is not wired
(`kPlannedDriverFaultCapability` is `NotInstalled`), so the firmware never claims
a `FaultCode::DriverFaultBar` and there is no line to trip. That step applies only
to a vehicle that wires the line and has measured its polarity; on this one it is
recorded as not applicable rather than deferred.

**Inject one fault at a time.** Each trial is a single cause with everything else
nominal. Record the exact command sequence and the exact response.

## Prerequisites

- H3 passed (the direction and the driver mapping are known) and H7 passed (the
  stop and reversal paths work).
- A running configuration that can be armed, so faults can be injected during a
  real run; note the pre-registered per-trial stop conditions.
- A reversible, non-destructive way to interrupt or hold the encoder: a jumper on
  one signal lead, a connector that can be unplugged, or a means of holding the
  encoder shaft still while the motor drives. It must be undoable without damage,
  because every injected fault is repeated.
- Kill switch primed; current limit set; runout clear.
- A pre-registered list of the faults to inject, and the expected stop behavior
  for each - including the sensing faults below, whose expected behavior may be
  "no enforcement exists", which is recorded rather than assumed to be a stop.

## Setup

A runnable configuration (both `announce` and `arm` succeed), the vehicle in the
configuration the sub-test requires, telemetry capturing, the kill switch
primed, and the current limit set. For every injection, write down the expected
outcome **before** running it, so a surprise reads as a surprise.

## Sub-tests - software/state-machine faults

Map each injection to the `FaultCode` vocabulary in `lib/evcore/src/VehicleState.h`.
Inject, record the named fault, the phase timeline, and whether the motor command
went to the de-energized state.

| Injection | Expected vocabulary |
|---|---|
| `config set` an out-of-range field, then `announce` | refusal (`ValueOutOfRange`), no arm |
| provisional calibration value present, then `arm` | arming refused, naming the failing check |
| missing safety limits present, then `arm` | arming refused, naming the failing check |
| a phase held past its configured bound | `PhaseTimeout`, motion stopped |
| a travel leg that reaches brake entry and then stops advancing (the taper's zero point short of the leg's landmark) | `TravelStalled`, motion stopped - applies only from brake entry, in `Approach`/`Capture`/`Push`/`Reverse`/`FinalApproach`. Like every run-phase row here it needs a run, so it requires the same calibration promotion as the others; the bound itself is a placeholder (`kPlannedTravelStallTimeoutS` is `Provisional`), and until it is measured no run policy validates |
| a stop that cannot be verified (keep the shaft moving) | `ReversalNotVerified`, motion stopped |
| `disarm` during a run | `DisarmedDuringRun`, motion stopped |
| a sequencing inconsistency, if one can be provoked | `RunSequencingError` |
| a run held past its validated maximum duration (S10) | `RunDurationExceeded`, motion stopped - **only while `maxRunDurationS` is `Measured`**; the shipped limit is `Provisional`, so a placeholder can never stop a run and this injection is unexercisable on the shipped configuration |
| an out-of-range effort presented (bench-only injection) | `InvalidMotorEffort`, refused |

For `RunSequencingError`, if there is no honest way to provoke it from outside the
firmware, record it as **unexercised**. Do not invent an internal fault path just
to light up the code.

## Sub-tests - electrical faults

1. **Driver `nFAULT` trip - NOT APPLICABLE to the assembled vehicle (D32):** the
   external nFAULT signal is not installed, so there is no `nfault_low` reading to
   record and no firmware response to observe. The step below is preserved for a
   vehicle that wires the line. With the current limit deliberately low, provoke a
   trip (a brief stall or a low ITRIP setting) and record what `nfault_low` does,
   how long it stays low, whether the driver recovers by itself, and what the
   outputs do meanwhile. **The firmware currently produces no `FaultCode` from
   this line** (D13 decision 4): record what actually happens, and record the
   fault identity and stop policy as a **pending software change** - not as
   passing.
2. **Low battery - NOT APPLICABLE to the assembled vehicle (D32/S10):** there is
   no battery-sense circuit, so there is no voltage to compare and no arming or
   runtime gate to exercise - `kPlannedBatteryVoltageCapability` is `NotInstalled`,
   which makes the arming-time `Measured`-`minBatteryV` requirement **not
   applicable** rather than outstanding. This step is preserved for a vehicle that
   installs the sensing: run with a supply below the planned minimum and record the
   raw `batt_counts` and whether any enforcement stops the run. Even then there is
   **no runtime voltage comparison in the firmware**, so record the behaviour and
   mark enforcement pending - never passing.

## Sub-tests - sensing faults during motion

These are the highest-value injections in this protocol, because the stop before a
reversal is verified from the same estimator these tests break: a lost or stuck
encoder can let the zero-speed gate report a stop the vehicle is not making. Both
are injected **while the motor is driven** (B2 or an armed run), never at rest,
and each is expected to stop the motor - if it does not, that is a hard finding.

1. **Encoder loss during motion.** With the motor driven, interrupt one encoder
   signal (or disconnect the encoder). Record: whether `vel_ok` drops, whether the
   no-edge timeout is reached, how long the motor keeps driving at commanded
   effort, and whether any fault is named. A position estimate that stops changing
   while effort continues is exactly the condition a zero-speed gate must not
   mistake for a stop.
2. **Stuck encoder during motion.** With the motor driven, hold the encoder (or
   the wheel that drives it) so ticks stop while the vehicle keeps moving. Record
   whether the firmware claims zero speed, whether it claims a stop, and how long
   the motor keeps driving. A claimed stop with the vehicle still moving is the
   finding this protocol exists to expose.

For either, if no `FaultCode` covers it (check `lib/evcore/src/VehicleState.h`),
record the observed behavior and "encoder-fault enforcement pending" as a
**pending software change** - the same honest outcome the electrical faults use.
Do not mark it passing, and do not continue to H10 with the zero-speed gate known
to be breakable while unguarded.

## Held constant

Everything except the one fault being injected: the configuration, the course,
the supply voltage and current limit, the firmware image, and the run profile.

## Varied

Exactly one injected fault at a time, from the pre-registered list.

## Measurements and logs

Per trial, record: the exact command sequence; the named refusal or fault
(verbatim); the phase timeline from `EVE3` (schema 3); whether the motor command
became de-energized, and how long after the fault; whether `reset` cleared the
latch; and the vehicle's physical state (stationary, or still moving - the latter
is a hard finding). **The latch is RAM state and a reboot deliberately forgets it
while staying de-energized and unarmed** (`AGENTS.md` section 5: a reset never
resumes motion), so a power cycle's effect on the latch is *not* a criterion. What
is recorded about a reboot is the safe direction of that volatility: the motor
comes up off, no command resumes, and a fresh deliberate `reset` (plus a fresh
`bench arm` for a bench command) is required. The persistent diagnosis is the
preserved capture, not the latch (C7).

## Repetitions

At least three per injected fault, since a latched fault's behavior should be
deterministic but the timing to de-energize need not be.

## Safe-stop (abort) conditions

Kill switch primed. Abort on any injected fault that does **not** stop motion, or
that leaves motion continuing after the fault is named. Stop with the kill
switch, then reset. Do not continue injecting faults while an earlier fault is
unresolved.

## Acceptance / decision rule

Pass requires: every latched fault stops motion; each named fault matches its
injection; a fault never keeps the motor energized (a reboot clears the RAM latch
but boots de-energized, unarmed, and resumes nothing - `AGENTS.md` section 5);
the time from fault to de-energize is recorded. Electrical and sensing faults are
recorded for what the firmware actually does; where enforcement does not exist,
the protocol reports "enforcement pending", which is an honest outcome, not a
failure of the test - **except** for a sensing fault that leaves the vehicle
moving while the firmware believes it has stopped, which is a hard finding and a
block on H10 until the gate is guarded.

## If it passes

- Record the electrical `nFAULT` behaviour (only on a vehicle that wires the line)
  and the low-battery observation (only on a vehicle that installs the sensing) as
  new facts. On the assembled vehicle both have **no subject** (D32/S10): the
  driver doc's `nFAULT` question is resolved as *not installed*, not as owing a
  measurement.
- Record the fault-to-`FaultCode` mapping as verified **only** for the faults
  actually injected.
- Log in `DEVELOPMENT_LOG.md`. #1-#12 do not change except through the driver
  truth-table entry (#2) if the trip behaviour contradicts the plan.

## If it fails or is inconclusive

In order: a measurement/observation mistake (was the fault really injected?); a
configuration mistake (the run was not actually armed); a wiring/electrical
issue; a firmware defect in the latch, the naming, the stop mapping, or the
encoder-loss handling; then the control strategy. A fault that leaves the vehicle
moving - including a guardless zero-speed gate - is the highest-severity finding
in this repository: stop and fix it before any further testing.
