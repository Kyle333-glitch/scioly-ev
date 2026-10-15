# P0 — Remaining Software Gaps (P0–P3, source-backed)

**Base `20aa4a1`. No hardware, no upload, no promotion, no push. Entries cite live
source; theoretical defects without a wasted-session mechanism were not made into work.**

## P0 — blocks safe or useful immediate hardware work: NONE FOUND

No defect found that stops supervised unpowered Block E (E0 + H0 repeat + H0W
USB-only + H1A-A0/A1 + H1B 1/2/4 + H2-hand). The arm gate refuses by design, the
bench/production separation is symbol-verified, and S19 closed the known
invisible-loss hole (M16). If a P0 emerges during Block E (e.g. H1A contradicts
`src/PinMap.h`), stop and file a repair spec — do not work around it in the lab.

## P1 — likely to require repeating early hardware tests if not fixed first

| ID | Finding | Evidence | Impact | Fix feasible SW-only? | Work / tests | Fix now? |
|---|---|---|---|---|---|---|
| P1-1 | No flashed-binary attestation; `rev/dirty/cfg` identifies source, not the installed image (R3/L3 honest limit) | `FirmwareIdentity.h::appendFirmwareIdentity()`; `scripts/ev_build_identity.py` (generated, clean-removed); `docs/limitations.md` | Wrong-image session (bench flashed for production test) passes paper review until behavior diverges; repeats H1A/H2 | Doc/procedure only (no code can attest its own flash) | S: add `VERSION`-after-flash + `bench health image=` check to P-H0 + P0 manifest (done in P0 §6); host test: none (procedure) | YES — done as documentation (this audit); no firmware change |
| P1-2 | Host-side log gaps have no offline checker; S19 device counters need a host counterpart | S19 adds `serial_abandoned` etc. (`OperatorResponse.h`, `BenchConsole.h`); no repo tool consumes them offline | Early sessions uninterpretable (missing reply vs refused command); repeats measurements | Yes — offline validator (no serial, no motion) | S: `tools/p0_log_completeness.py` + 6-case self-test (this audit) | YES — implemented below (§10-permitted) |
| P1-3 | `S19V` independent acceptance missing; `E0V` has no standalone artifact | Search 2026-10-15: no `S19V`/`E0V` file in `.agent-work/reviews/`, no log entry; E0V cited inline only | Cannot claim S19/E0V independently accepted; Block E review must treat S19/E0 as single-party until re-reviewed | Yes — schedule independent re-review (read-only) | S: one read-only audit prompt (S19R/E0VR) before Block F, not before Block E | YES as process — do not block Block E; block Block F on it |

## P2 — wait for physical measurements

| ID | Finding | Evidence | Impact | Fix feasible SW-only? | Work / tests | Fix now? |
|---|---|---|---|---|---|---|
| P2-1 | 0.5 s feedback allowance unvalidated (`kPlannedEncoderProgressTimeoutS`, Provisional) | `src/VehicleConfig.h:356-369`; `EncoderProgressMonitor`; R23I notes it as placeholder | Wrong timeout → late trip (damage) or nuisance trip (invalid runs); H5/H7/H9 must set it | No — needs H5/H7/H9 data | M: characterize stall/creep/noise, then promote with citation | NO (measure first) |
| P2-2 | Single-tick noise vs motion unresolved | `QuadratureDecoder` x2 + ISR snapshot; pull-up placeholder (`src/EncoderInput.h`) | One bad edge looks like motion or masks a stall | No — needs H2/H5 scope/count-delta data | M: retention + repeat counts + optional scope | NO |
| P2-3 | Signed count/overflow: wrap-safe time arithmetic proven, tick-accumulation bound needs loaded proof | `ControlScheduler` wrap-safe; `RunDurationGuard` unsigned `nowMs-runStartedAtMs`; scenario sweep host-only | Long runs could accumulate error unnoticed | Partial — host bounds exist; loaded proof needs H4/H7 | M: H4/H7 long-run comparison vs tape | NO |
| P2-4 | Watchdog loaded observability (H0W-S8) owed | `P-H0W` Step 8; `WatchdogPlan` compile-time rule only | USB-only WDT proof does not cover loaded cadence extremes | No — needs loaded run | M: re-run H0W-S8 after H7 | NO (scheduled) |
| P2-5 | Serial reply delivery under load (host-received proof) | S19 device-side only; `HostGreetPolicy` silence/edge limits (`docs/limitations.md`) | Backpressure loss misread as refusal/acceptance | Partial — P0 validator detects gaps offline; delivery proof needs loaded capture | S+M: validator (done) + H3/H7 loaded captures | Validator YES, rest NO |
| P2-6 | PI/braking/reversal/rollout/slip/bottle (rows #6/#8/#9/#10/#11) | All Provisional; `P-H4..H8`; informal 2026-10-02 only | Cannot arm/run; expected — the H-series exists to measure them | No | Per-protocol H-series | NO |

## P3 — optional / low-value cleanup

- P3-1: Pre-S19 captures cannot be re-analyzed for `serial_abandoned` (field did
  not exist). No action — note provenance when citing old logs.
- P3-2: `docs/audits/` untracked (intentional user-owned preservation). No action —
  leave untracked; do not commit user history in P0.
- P3-3: `origin/main` 38 behind local `main`. No action — do not push per P0 limits.
- P3-4: General-audit orphans/probes already dispositioned (R-series). No new work.

## Disposition of the §8 prompt list

Signed/overflow → P2-3. Single-tick noise → P2-2. 0.5 s allowance → P2-1.
Watchdog observability → P2-4. Serial delivery → P2-5. Identity/attestation →
P1-1. First-session evidence → P1-2 (fixed) + manifest. Outstanding audits →
P1-3 (+ R-series closed). No P0.
