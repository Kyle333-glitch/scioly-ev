# P-H3 - First powered drive, drivetrain unloaded

**Primary class:** P (physical behaviour check).
**Also contains:** E (conservative effort, current limit, kill switch) and S
(the firmware's reported command matches what the driver does).
**Order:** after H1A, H1B and H2. This is the first time the motor is powered.
**Answers:** whether the bench stop path actually stops the motor before the first
powered command (Step 0); which PH level turns the wheel in the vehicle's forward
direction (motor polarity, #3); whether the tick sign under power agrees with the
hand-driven sign; the smallest effort that moves the shaft from rest; what the
driver does at zero command and after de-energizing.
**Bench instrumentation required:** B2 (bench-only bounded diagnostic drive) and
B3 (raw tick readout).

This is the protocol the whole bring-up sequence is aimed at. Until it passes, no
direction, speed or distance result downstream means anything.

## Prerequisites

- H1A and H1B passed; the motor is now **reconnected** to the driver outputs -
  record the reconnection and re-check continuity of the two output leads against
  A0's table.
- H2 passed: the forward-direction convention is documented and the hand-driven
  tick sign is recorded.
- A bench build containing B2 and B3; the production image must not contain B2.
- B2 in that build implements the safety model the index requires: a command that
  expires on its own, and a fault that latches the motor off until a deliberate
  `reset`. This protocol's Step 0 demonstrates both before any powered command.
- Wheels **off the floor** (drivetrain unloaded), nothing loose or hanging near
  the shaft, eye protection.
- Supply current limit set conservatively and recorded; physical kill switch
  (motor supply disconnect) within reach and proven to work before the first
  powered step.
- (Pre-register) the increment step and the criterion for "the shaft moved".

## Setup

Driver connected, motor connected, drivetrain unloaded. Telemetry on, capture to
a file.

## Step 0 - demonstrate the stop path, before any powered command

An unloaded shaft is lighter, not harmless: the first powered test still needs a
stop that works without the operator's hand. Do this first, with the wheels off
the floor and the kill switch in reach, and record each result. A powered trial
whose stop path has not been shown to work is not a trial that may be run.

1. **De-energized baseline.** With the driver de-energized, confirm the shaft does
   not move and the firmware reports the de-energized motor mode.
2. **Bounded expiration.** Command the smallest duty B2 allows and then issue
   *nothing*. Confirm the command expires on its own after its pre-registered
   interval and the shaft stops - the timer, not the operator, is the stop.
3. **Latched fault.** Provoke B2's fault (for example, hold the shaft so the
   encoder stops changing while B2 commands it), confirm the motor de-energizes,
   and confirm it **stays** de-energized through a further attempt to drive until a
   deliberate `reset`. A fault that clears itself, or that a resent command
   re-arms, fails this step.

Record the observed stop behaviour and the interval from command to de-energize
for each step. If any of the three is not demonstrated, stop here: the only
remaining stop would be the operator's hand, and that is not a stop path.

Only then apply, through B2, one bounded command at a time with a fixed short
dwell and a de-energizing step between commands.

## Held constant

Supply voltage and current limit, load (unloaded), dwell, firmware image,
temperature (let the motor return to ambient between runs).

## Varied

- one PH level at a time, at the same low duty;
- duty, one increment at a time, from the smallest level the driver responds to,
  upward, never jumping to a large value.

## Measurements and logs

For each command: the commanded duty and direction; the observed shaft
direction (relative to the forward convention); the signed raw tick delta; the
supply current; the supply voltage sag; `nfault_low`; whether motion started at
the commanded value or after a deadband.

Also record:

- the result of each Step 0 demonstration (de-energized baseline, bounded
  expiration, latched fault) with the command-to-de-energize interval;
- the **smallest duty that turns the shaft from rest** (repeat for a stable
  value) - this is the scale's origin and is a fact about this driver+motor;
- the behaviour at duty 0 and immediately after de-energizing: does the shaft
  keep rotating (coast) or stop promptly (brake)? This is observed with the
  drivetrain unloaded and is recorded as observed;
- whether any command produced motion while the firmware reported the driver
  de-energized - that would be a fault, not a data point.

## Repetitions

At least three passes for the first look; then the minimum-duty determination at
least five times; the coast/brake observation at least three times. Interleave
directions so a thermal or battery drift is visible.

## Safe-stop (abort) conditions

Abort immediately - cut the motor supply - on: any direction other than the one
commanded; runaway or unexpected acceleration; a repeated current-limit trip;
heat, smell or abnormal noise; a command/response mismatch; or anything that
moves while the firmware reports the driver de-energized. Do not "see what
happens" past a first surprise.

## Acceptance / decision rule

Pass requires: **all three Step 0 demonstrations passed**, including a latched
fault that stays off until a deliberate `reset`; the observed shaft direction for
each PH level is recorded consistently across repetitions; the tick sign under
power agrees with the hand-driven sign for the same physical direction; the
minimum drive duty is recorded with its spread; no motion occurred while
de-energized; the de-energized behaviour (coast or brake, as observed) is
recorded.

## If it passes

- #3 (motor polarity): record which PH level turns the wheel in the vehicle's
  forward direction. This is a wiring fact, and the fix for a contradiction is
  the plan or the wiring, never a sign added elsewhere.
- From that one measurement, set the three encodings of the polarity fact
  together - `kPlannedPositiveCommandDirection`, `kPlannedPositiveEffortMode`
  and `kPlannedPositiveTicksDirection` - and expect
  `CommandPolarityAgrees`/`TickPolarityAgrees` to read true afterward. Because
  these are measurements, a `ValueSource` of `Measured` requires the raw data
  recorded with them.
- Record the minimum drive duty and the de-energized behaviour as new facts.
- Log in `DEVELOPMENT_LOG.md`.

## If it fails or is inconclusive

Escalation order: a mis-wired output lead (the most likely cause of a wrong
direction - check it against A0 before touching polarity values); a driver input
mismatch (revisit H1A); a mechanical binding; then a firmware path that presents
the wrong command. Do not "fix" a wrong direction by editing a sign in software
before the wiring is confirmed, and do not proceed to H4 with an unresolved
direction question.
