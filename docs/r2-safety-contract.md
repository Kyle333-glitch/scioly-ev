# R2 Safety and Configuration Contract

Stage R2 closed five audit findings as one host-tested software contract.
This note states the contracts for the team and the integrator. It makes no
physical claim: every duration below is software logic until measured on the
vehicle, and powered Block F remains gated.

## M18 — arithmetic refusal (`lib/evcore/src/VelocityController.h`)

Finite gains can overflow their terms (`1e308 * 10 = +inf`) and opposite
infinities cancel to NaN. The regulator now refuses any cycle whose
feed-forward, error, proportional, integral, or demanded term is non-finite
with status `ArithmeticNotFinite` and a `Disabled` output, before clamping,
rounding, or application. Precedence: configuration, invalid target,
invalid measurement, then arithmetic (which overrides frozen-stamp
regulation). Finite saturation and zero-gain behavior are unchanged, and no
gain cap was invented. Diagnostic terms on refusal are zeroed.

## M3 — timeout/dwell headroom

- Reversal: `reversalTimeoutS` must strictly exceed the verification
  `dwellS` (`ReversalConfigError::TimeoutWithoutDwellHeadroom`). The timeout
  is evaluated before the verification step, so equality already loses.
- Final stop: `finalStopTimeoutS` must strictly exceed the stop-verification
  `dwellS`
  (`RunPhaseConfigError::FinalStopTimeoutWithoutDwellHeadroom`).
- Malformed values keep their earlier verdicts; millisecond saturation
  stays the safe direction. No calibration value changed.

## S18R-M1 — sustained-invalid-feedback bound

Every accepted-powered cycle without verified progress evidence (frozen,
wrong-sign, or invalid sample) consumes one allowance -
`encoderProgressTimeoutS`, accumulated by control `dtMs` (saturating,
stamp-free) - and latches `EncoderFeedbackLost` at the bound
(`lib/evcore/src/EncoderProgressMonitor.h`). Only genuine
expected-direction progress resets; unpowered cycles freeze; placeholder
bound/polarity still pause; startup baselines the full allowance; direction
flips and software reset cannot renew it (reset runs only at the new-run
boundary with no accepted power outstanding). Composed software timing
(fixture values): stale data revokes accepted power within the 60 ms
freshness window; frozen power faults at the 0.5 s bound with same-cycle
Disable at the driver boundary; dropouts end at phase timeouts, never at
the feedback bound. The 0.5 s duration itself is still an unvalidated
placeholder (P-H5/H7/H9). Mock I/O proves the software command, never
electrical disable timing.

## M2 — watchdog stamps (`src/main.cpp`, `src/bench/BenchMain.cpp`,
## `src/WatchdogCheckpoint.h`)

The loop re-reads `millis()` for `endOfPass`, so the recorded refresh time
is the refresh event's, not the pass-start reading. No enablement change.

## L4 — scheduler wait (`lib/evcore/src/ControlScheduler.h`)

The not-due wait is the unsigned deadline-minus-now distance. Half-range
eligibility is unchanged; the INT32_MIN evaluation is now defined
(not due, wait 2^31 ms).

## Validation and files

36 new regression tests (native 776/776, 38 suites); three firmware builds
SUCCESS with zero warnings (flash +~730 B for the guards, RAM unchanged);
telemetry 388 unchanged with 4 pre-existing skips. Changed sources: the
five lib headers above plus the watchdog entry points and one comment-only
wiring rationale in `src/VehicleRuntime.cpp`. Tests: the six matching
suites. One prior assertion was updated with evidence (invalid samples no
longer pause under a validated bound - that pause was the defect).

## Still required (physical)

Feedback-bound duration, encoder polarity/edges, driver disable timing,
stop-window validation, overspeed policy, kill switch/current limit - then
Block F readiness can be judged. Nothing in R2 unblocks it alone.
