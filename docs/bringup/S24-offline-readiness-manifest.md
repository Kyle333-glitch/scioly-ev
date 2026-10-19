# S24 — Offline software readiness manifest (no hardware)

**Status: software reproducibility record only. No board, no port, no upload,
no motor operation performed in S24. This document is not authorization for
powered operation and does not claim any hardware validation.**

## 1. Exact revision

- HEAD: `3c925353e0b5a942834345d5ba6d993f0cb411fc` (plus S24 working-tree
  edits in `docs/bringup/S23-ev-serial-diag-guide.md` and this manifest,
  uncommitted at time of writing — record `git rev-parse HEAD` at session).
- Branch: `main`, tracking `origin/main`, **ahead 45, behind 0**.
- Remote `origin/main`: `a0de094c7cf67b1b13a86682745fe75c1a31757b`.
- Working tree at S24 start: clean except two preserved historical untracked
  audits: `docs/audits/2026-10-06-software-audit.md`,
  `docs/audits/2026-10-08-general-codebase-audit.md` (never modified, never
  committed by S24).
- Tags: none. Stash: empty. Worktrees: 9 (main plus 8 linked stage worktrees;
  see `git worktree list`). Recovery refs: `recovery/d32-d34-v5-snapshot`,
  `recovery/s2-overscope-snapshot`, plus `refs/cline/checkpoints/*`
  (unmodified by S24).

## 2. Build environments and intended roles (`platformio.ini`)

| Env | Role |
|---|---|
| `nano_r4` (default; `pio run`) | Production vehicle image. Excludes `src/bench/`. |
| `bench` (`pio run -e bench`) | Bench-only instruments (B1 loop/watchdog, B2 bounded drive, B3 raw ticks, B4 inputs) + temporary H1A diagnostic. Never the vehicle image. |
| `bench-watchdog` (`pio run -e bench-watchdog`) | Watchdog-qualification image. Same bench tree, no H1A diagnostic. |
| `native` (`pio test -e native`) | Host-side pure-logic unit tests from `lib/` only. No board. |

Platform pinned: `renesas-ra@1.9.0` (pins Arduino core + FSP).

## 3. Current test counts (S24 re-measured, this tree)

- Native firmware tests: `pio test -e native` — **868/868 passed, 43 suites**.
- Serial-diagnostic tests: `python -m unittest
  tools.serial_diag.tests.test_ev_serial_diag -v` — **23/23 OK**
  (fake transport, no COM port opened).
- Rollout-calibration tests: `python -m unittest
  tools.calibration.tests.test_rollout_scale -v` — **30/30 OK** (synthetic
  fixtures, no vehicle measurement).
- Compiles: `pio run -e nano_r4` SUCCESS (RAM 7500/32768, Flash 79912/262144);
  `pio run -e bench` SUCCESS (RAM 7708/32768, Flash 87724/262144);
  `pio run -e bench-watchdog` SUCCESS (RAM 7692/32768, Flash 87120/262144).
  Zero warnings reported by the build output tails.
- Syntax/diff: `arm-none-eabi-g++ -fsyntax-only lib/evcore/src/*.h` exit 0
  (only expected `#pragma once in main file` notes); `python -m py_compile`
  on both utilities OK; `git diff --check` clean.

## 4. Python and PlatformIO requirements

- PlatformIO Core **6.2.0**, Python **3.11.7** (penv).
- `python` on this host resolves to the PlatformIO virtualenv
  (`C:\Users\danie\.platformio\penv\Scripts\python.exe`) which provides
  **pyserial 3.5** as a PlatformIO dependency.
- A stock Windows Python (e.g. 3.13) does **not** provide pyserial —
  `pip install pyserial` into that same interpreter is required.
- Preflight (opens nothing): `python -c "import sys; print(sys.executable)"`,
  `python -c "import serial; print(serial.VERSION)"`,
  `python -m pip show pyserial`. See the S23 guide §0.

## 5. Utility locations

- Serial diagnostic: `tools/serial_diag/ev_serial_diag.py`
  (guide: `docs/bringup/S23-ev-serial-diag-guide.md`).
- Rollout calibration: `tools/calibration/rollout_scale.py` with template
  `tools/calibration/rollout_trials_template.csv`
  (tests: `tools/calibration/tests/test_rollout_scale.py`).
- Telemetry validators (offline analysis only):
  `tools/telemetry/analyze.py`, `tools/p0_log_completeness.py`.

## 6. Approved hardware-return runbooks (supervised use only)

- `docs/bringup/S21-hardware-return-serial-runbook.md` (transport vs.
  firmware-selection vs. command-syntax isolation; corrected root cause S21R).
- `docs/bringup/S23-ev-serial-diag-guide.md` (passive-first read-only tool
  operation; S24 documentation fixes only, no implementation change).
- `docs/bringup/P-H0-nano-upload-serial.md` (upload/boot/serial link gate).
- `docs/bringup/E0-unpowered-inspection-protocol.md` + Block E records
  (mandatory electrical clearance before any USB session).
- `docs/bringup/P-H1A-driver-electrical.md`, `P-H1B-sense-inputs.md`,
  `P-H2-encoder-manual.md`, `P-H4-rollout.md`, `P-H7-distance-braking-reversal.md`
  and sibling P-protocols for staged bring-up. No protocol is authorization to
  power the motor outside its own prerequisites.

## 7. Hardware-dependent unknowns (all UNVERIFIED)

All twelve `VERIFIED_STATE.md` rows remain UNVERIFIED: #1 wiring/pins (no
continuity), #2 driver truth table, #3 motor polarity, #4 encoder direction
sign, #5 signal quality, #6 rollout mm/tick, #7 sensor scaling (battery/current
sensing not installed — no measurement path), #8 gains, #9 braking, #10 wheel
slip, #11 bottle interaction, #12 dead-wheel need. 2026-10-02/03 informal
observations (D36) change nothing. DTR/RTS propagation, flashed-image identity,
and any board-specific reset on open remain unverified until a `BOOT`/reply is
observed on target.

## 8. Outstanding safety gates (must still refuse)

- `arm`/`announce` refuse with the first failing check: every calibration,
  limit, run-policy and control value is still `Provisional`
  (`docs/limitations.md`); `arm` must name the gate, never assume it.
- Motor commanded OFF/disabled during boot/init/reset/error/invalid-state and
  before any armed state; no auto-resume after reboot; powered motion only via
  intentional armed/run transition (AGENTS.md §5).
- Serial tool sends read-only allowlist only (`boot version status` +
  gated `bench health/serial/ticks`); `arm/drive/bench arm/bench drive/
  bench h1a-brake/announce/reset/config set` are refused before opening.
- H1A/H3 prerequisites (motor disconnected or unloaded, wheels off floor,
  supply off/limited) still apply; Block E inspection still gates USB sessions.

## 9. Temporary H1A diagnostic configuration and restrictions

- Present only in `env:bench` via `build_flags =
  -DEV_BENCH_H1A_DIAGNOSTIC=1 -DEV_FIRMWARE_VARIANT=1` (`platformio.ini`);
  absent from `env:nano_r4` and `env:bench-watchdog`.
- Exposes `bench h1a-brake` (holds the awake-EN-low combination for meter
  measurement per `P-H1A`). `src/main.cpp` refuses to compile if the define
  leaks into production.
- Temporary: remove the line (with the diagnostic) once H1A is recorded.
  Its presence is not permission to run it; never in a production build.

## 10. Deferred directional-drift investigation

- Lateral heading/drift characterization (and any IMU or dead-wheel escalation
  for it) is **deferred**: no new sensing, estimation, or veer-correction work
  in S24 per constraints. Addition requires a measured problem, magnitude, and
  evidence the existing system cannot address it (AGENTS.md §20).
- Straight-tracking remains an unmeasured physical property (rows #8–#10);
  no software constant was added for it.

## 11. Software reproducibility vs. hardware readiness

- **Software reproducibility (established S24):** this exact tree rebuilds all
  three firmware images with no warnings and passes 868 native + 23 serial-diag
  + 30 calibration tests offline. That proves the code compiles and its
  host-testable logic behaves — nothing more.
- **Hardware readiness (not established):** no upload, no COM port, no board
  observation in S24; all twelve hardware rows UNVERIFIED; safety gates still
  refuse; staged supervised bring-up (Block E → H0 → H1A/H1B → H2 → …) is still
  owed. A clean build must never be read as physical validation.
