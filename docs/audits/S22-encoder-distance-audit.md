# S22 — Encoder-to-distance offline audit (2026-10-18)

Offline-only. No hardware access, no COM access, no uploads, no motor
operation. Starting HEAD: `68fe555`. No calibration data was fabricated and
no theoretical value was promoted: the user-mentioned `22.9414` does not
appear anywhere in the repository (verified by search) and was not used.

## 1. Source-grounded architecture map

```
pins/ISR (src/EncoderInput.h/.cpp, PinMap.h: A=D2, B=D4, INPUT_PULLUP placeholder)
  A-CHANGE interrupt -> ev::QuadratureDecoder (lib/evcore/src/QuadratureDecoder.h)
  x2 decode, A-change only; B level decides sign; first sample is reference-only
  counters: int32 ticks, uint32 countedEdges / ignoredUpdates
        |
snapshotTicks() — PRIMASK-guarded coherent TickSnapshot (coherence, not tearing)
        |
VehicleRuntime::update (src/VehicleRuntime.cpp ~L150-210), per control tick:
  a. odometryTravel_ = distanceTravelledM(runOriginTicks_, ticks,
                                          calibration_.distancePerEncoderTickM)
     (lib/evcore/src/Odometry.h) — delta-only, int64 difference, double product
  b. estimate_ = VelocityEstimator::update(nowMs, ticks, same scale)
     (lib/evcore/src/VelocityEstimator.h) — sliding window, same scale rule
  c. axisPosition = runAxisPositionM(origin, ticks, same scale,
                                     positiveTicksDirection())
     (lib/evcore/src/RunAxis.h) — THE ONLY place tick-sign polarity is applied
  d. remainingToTargetM = remainingDistanceTo(targetPoint, axisPos, legForward_)
     legRemainingM = remainingDistanceToClamped(legExecutionTargetM(plan, leg), ...)
     (RunExecutionPlan.h owns the leg target; RunAxis never chooses one)
  e. MotionProfile taper + WithinBrakeOffset from legRemainingM vs brakeOffsetM
     (MotionProfile.h); EncoderProgressMonitor + readiness verdicts gate use
        |
telemetry EVS3 pos_m / vel_mps / rem_m at 4 decimals (= 0.1 mm LSB)
  (TelemetryRecord.h kTelemetryFieldDecimals=4, TelemetryFormat.h, BoundedText)
```

Key configuration facts (all Provisional, all fail-closed):

- `VehicleCalibration { distancePerEncoderTickM, forward/reverseBrakeOffsetM }`
  (`lib/evcore/src/VehicleCalibration.h`); shipped placeholders in
  `src/VehicleConfig.h`: scale `0.0001 m/tick` (= 0.1 mm/tick),
  brake offsets `0.05 m` each.
- `positiveTicksDirection()` (`VehicleConfig.h:673`) returns the provisional
  `kPlannedPositiveTicksDirection`; `tickPolarityAgrees()` (SafetyLimits.h)
  requires tick polarity to equal the command polarity before arming, which is
  what makes the estimator's tick-convention velocity comparable to the
  profile's command-convention target.
- No CPR/PPR, gear ratio, wheel diameter or wheel circumference exists anywhere
  in code: the encoder is treated as a motor encoder whose counts convert to
  travel only through the measured `distancePerEncoderTickM` (rollout, H4).
  `pos_m` is therefore a derived distance needing an unmeasured scale — P-H2
  correctly forbids using it before H4; raw ticks (B3) are the H2/H4 evidence.

## 2. Mathematical correctness assessment

- **Delta vs absolute:** `distanceTravelledM` uses only
  `(int64)current - (int64)origin`; any origin is legal. Full int32-range
  pairs are exact (no UB), and the int64 delta is always exactly
  representable as double (well inside 2^53). Verified by test.
- **Sign:** positive tick delta gives positive odometry distance (tick
  convention, not a forward claim). `runAxisPositionM` applies the measured
  polarity exactly once, to travel, never to a position — so a non-zero origin
  cannot invert the sign. Provisional polarity yields a diagnostic number but
  `usable=false`. Verified by test, including flip-symmetry.
- **Scale representation:** strictly positive (`!(x > 0)` refuses zero and
  negative together), finite, and `Measured`-only for `valid=true`.
  Provisional/RuleDerived still compute a diagnostic number but never validate.
  Negative scale can never silently invert direction. Verified by sweep test.
- **Units:** metres everywhere in firmware (`distanceM`, `positionM`,
  `remainingDistanceM`, `brakeOffsetM`); millimetres appear only in docs and
  the 4-decimal telemetry rendering (0.1 mm LSB). With the placeholder scale,
  1 tick = exactly 1 telemetry LSB; with a real ~1.3 mm/tick scale,
  quantization (~1.3 mm) dominates telemetry resolution — correct direction,
  no hidden precision.
- **Precision/rounding:** double arithmetic; comparisons in tests use 1e-9 m
  tolerances. `positionFromOriginM` refuses non-finite origins and
  origin+travel overflow. Velocity is `ticks * scale / windowS` with a
  finiteness check. No integer division, no float.
- **Zero/short:** zero delta is 0.0 m and still Valid; 1 tick at 0.1 mm/tick
  is 0.0001 m. Sub-tick distances are unrepresentable by construction
  (quantization, not a bug).
- **Overflow:** scale-product overflow → `DistanceIsNotFinite`, 0.0, invalid.
  `msFromSeconds` saturates; `sampleAge` uses unsigned subtraction (wrap-safe).
- **Resets/origins:** decoder `reset()` clears reference+counters; the pure
  chain reset (`MotionChainReset.h`) plus `EncoderInput::
  resetAndReestablishReference()` form the new-run boundary (PRIMASK-safe,
  no phantom tick, both signs preserved — pinned by test_encoder_reset).
  Origin changes are exact (split-span sums to the whole — new S22 test).
- **Consistency:** odometry, run axis and estimator consume the same
  `Sourced<double>` through the single `distanceScaleStatus()` rule, so they
  cannot drift apart on scale validity. New S22 test pins same-fixture
  agreement plus polarity-flip symmetry.
- **Boundary rounding:** `WithinBrakeOffset` is `!(remaining > brakeOffset)`,
  so exact equality counts as inside. 1 tick flips the verdict at the
  boundary (new S22 test: 500 ticks @0.1 mm/tick = 0.05 m → inside;
  501 ticks → outside). No clamping hides overshoot in the unclamped
  `remainingDistanceTo`; the clamped variant is used only for taper sizing.
- **Drift:** nothing in this pipeline corrects lateral veer; distance control
  is along-track only. The observed rightward veer is out of scope as ordered.

Verdict: the pipeline is mathematically correct and numerically robust as
written. No conversion defect was found (details in §3).

## 3. Defects and limitations

| # | Finding | Severity | Confidence |
|---|---------|----------|------------|
| L1 | Encoder polarity, CPR/x2 interpretation vs wheel revs, gear ratio, rollout mm/tick, slip, braking offsets, gains: all UNVERIFIED (VERIFIED_STATE #3–#12). Firmware correctly refuses to arm; nothing to fix in code. | High (blocks arming by design) | Certain (source + VERIFIED_STATE) |
| L2 | `positionFromOriginM` documents an unreconciled convention gap (physical origin vs tick-convention travel); production path uses `runAxisPositionM` instead, so the gap is contained, not a live bug. | Low (documentation-level) | High |
| L3 | Stale doc line: `P-H7-distance-braking-reversal.md` says the reverse brake offset "has no consumer" — superseded by the D35 per-direction fix (see DEVELOPMENT_LOG S1 row). Doc-only staleness; code is correct. Not touched in S22 (scoped diff). | Low | High |
| L4 | Telemetry 0.1 mm LSB vs ~1.3 mm/tick real quantization: logs cannot show sub-tick motion. Expected consequence of the design, not a defect; noted so analysts do not over-read `pos_m`. | Info | High |
| L5 | No offline tick→scale calculator existed; scale arithmetic lived only in prose (`travelM / ticks`). Addressed by the new tested utility (§4), not a firmware change. | Low (workflow gap) | Certain |

No genuine source-level defect requiring a production-firmware change was
found. In particular: no motor-gain, braking, arming, watchdog, electrical
or H1A change was made; veer was not investigated; no auto-calibration was
added to live firmware.

## 4. Minimal justified changes + regression tests

1. **New native suite `test_distance_consistency` (7 cases):** same-fixture
   agreement across odometry/run-axis/velocity; polarity-flip symmetry;
   fractional-scale linearity (synthetic 0.001339 m/tick × 1494 ticks);
   origin-change exactness; single-tick brake-boundary flip; zero-delta;
   consistent refusal of bad scales across all three modules. Covers the
   seams the per-module suites do not pin. All synthetic fixtures, labelled.
2. **New offline utility `tools/calibration/rollout_scale.py`** (+ 13-case
   unittest `tools/calibration/tests/test_rollout_scale.py` + CSV template
   `tools/calibration/rollout_trials_template.csv`): computes per-trial
   `travel_m / |tick_delta|`, mean/min/max/std, residuals vs the mean, sign
   agreement per trial, division-by-zero and invalid-row rejection, and a
   consistency flag (2% band + sign agreement). Reuses the repo's stdlib-only,
   no-install convention. Template units are explicit (metres, ticks).
3. **This audit record.** No production firmware modified.

## 5. Exact offline test results (executed, not claimed)

- `pio test -e native` (full): **868/868 succeeded** (baseline at S22 start
  was 861/861; +7 new consistency cases), ~1m29s.
- `pio test -e native -f test_distance_consistency`: **7/7 PASSED**.
- `python -m unittest discover -s tools/calibration/tests -t .`: **13/13 OK**.
- `pio run` (vehicle `nano_r4`): **SUCCESS** (Flash 79904/262144).
- `pio run -e bench`: **SUCCESS**.
- `python tools/calibration/rollout_scale.py
  tools/calibration/rollout_trials_template.csv`: exits 0, reports the
  template's example rows (NOT measurements) with mean scale + residuals.
- Existing telemetry suite (`tools/telemetry/tests`) untouched; no firmware
  source changed, so no upload/serial/hardware validation was possible or
  attempted.

## 6. Hardware-return calibration procedure (concise)

Prerequisites: Block E PASS, H0/H0W green, H1A/H1B recorded, H2 sign +
ticks/rev recorded, H3 powered-sign agreement, bench B3 available, tape
(≥2 m, mm resolution), flat straight surface, kill switch + runout for
powered parts. Pre-register every acceptance band before running.

1. **H2 (hand):** `bench ticks`; forward/reverse/forward hand rolls, slow +
   brisk, ≥10 revs × 5 runs per direction; record sign per direction,
   ticks/rev + spread, return-to-mark net, hysteresis in ticks at several
   wheel positions. Promotes `kPlannedPositiveTicksDirection` (+ #4, #5
   inputs). Never use `pos_m` here.
2. **H4 Part A (unpowered rollout):** hand-push between ground marks over the
   measured span D (≥2 m), mark-to-mark tick span N (line crossings, not
   stationary snapshots); ≥5 trials per direction. Record
   `trial_id,start_ticks,end_ticks,travel_m,direction,surface,notes` per
   Appendix template. Run
   `python tools/calibration/rollout_scale.py <trials>.csv`; check
   `consistent=yes`, spread within the pre-registered band, residuals small.
   Promote the **Part A mean** to `kPlannedDistancePerEncoderTickM` (Measured
   + evidence pointer); Part B never enters the scale.
3. **H4 Part B (powered slip check):** bounded `bench drive` at the H3
   minimum and near cruise, same marks; record D, N, effort triple, supply,
   `countedEdges/ignoredUpdates`, command-to-first-tick interval per wake
   state. A-vs-B systematic difference is the #10 slip record, never averaged
   into the scale.
4. **H4 Part C + H5/H7:** hysteresis vs forward/reverse difference; velocity
   trace for minimum cruise + ceilings; per-direction braking offsets (H7
   Part B is authoritative); near-zero threshold/dwell on the run path.
   Promote each constant only with capture + derivation + VERIFIED_STATE row
   + log entry + `citesEvidence()` (P-H14 rules; `checks` must show the gate
   that opened). Keep rejected runs with reasons.

## 7. Data-collection template

`tools/calibration/rollout_trials_template.csv` (also via
`python tools/calibration/rollout_scale.py --print-template`):

```
trial_id,start_ticks,end_ticks,travel_m,direction,surface,notes
```

- `trial_id`: repetition identifier (e.g. `A-fwd-1`).
- `start_ticks`, `end_ticks`: raw signed B3 counts at line crossings (ticks).
- `travel_m`: tape-measured span in **metres** (not mm).
- `direction`: `forward` | `reverse` (physical travel).
- `surface`, `notes`: setup info (surface, tire pressure, build/mass,
  instrument + resolution).
- The tool rejects zero tick span, non-positive/non-finite travel, bad
  directions, and missing columns; it flags (never hides) sign disagreement
  and >2% spread trials.

## 8. Facts not verifiable without hardware

Polarity of forward travel (#3/#4); signal quality, missed/extra counts
(#5); CPR vs x2-counted ticks per wheel rev; gear ratio; wheel diameter/
circumference; effective mm/tick rollout (#6); slip (#10); braking offsets
and settle (#9); gains (#8); bottle interaction (#11); dead-wheel benefit
(#12); wiring continuity (#1); driver truth table (#2); battery/current
scaling (#7, uninstalled); tick hysteresis on the surface; minimum reliable
cruise and ceilings under load; stop-verification threshold/dwell adequacy;
timing-model validity; anything about veer, tracking straight, or stopping
accuracy. The informal 2026-10-02 observations (~1494 ticks, ~1.339 mm/tick,
direction signs) remain observations, not evidence, and establish none of
the above.

## 9. Changed-file list and final Git status

Scoped S22 changes (production firmware untouched):

- `test/test_distance_consistency/test_main.cpp` (new, 7 native cases)
- `tools/calibration/rollout_scale.py` (new offline utility)
- `tools/calibration/tests/test_rollout_scale.py` (new, 13 unittest cases)
- `tools/calibration/tests/__init__.py` (new package marker)
- `tools/calibration/rollout_trials_template.csv` (new template)
- `docs/audits/S22-encoder-distance-audit.md` (this record)

Preserved untouched: `docs/audits/2026-10-06-software-audit.md`,
`docs/audits/2026-10-08-general-codebase-audit.md`, all branches/worktrees/
history (no destructive git operations; no motor-control, safety, H1A, or
tuning changes per §5 constraints).
