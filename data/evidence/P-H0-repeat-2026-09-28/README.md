# P-H0 repeat session - Nano R4 upload, boot and serial link

- protocol: `docs/bringup/P-H0-nano-upload-serial.md`
- pre-registration: `data/prereg/P-H0-repeat-2026-09-28.md` (written at
  `2026-09-28T22:07:04-07:00`, before the first upload of this series)
- date: 2026-09-28, 22:07-22:16 local
- operator: danie; uploads and captures driven by the agent through this session's
  terminal, with the operator present and able to power-cycle the board
- repository: `git rev-parse HEAD` = `8e42d13b69e6fcbb79275ff7699ffe4c9608fcc3`
  (the D21-D24 work is uncommitted working tree)
- images:
  - vehicle `env:nano_r4`:
    `sha256 = cfd288d455c2cf6c4d15f4b236827ee3e217f40ffe623b71ef301c265b38b788`
    (66836 bytes)
  - bench `env:bench`:
    `sha256 = 71f42e40417d2dcd780607f0e0d6ab04715bea1f992ae37ab46c29e22773e9a7`
- board: Arduino Nano R4, `USB VID:PID=2341:0074`,
  `SER=350D331C5A333131021133354B573462`, `COM3`
- motor supply: **off and disconnected** for the whole session. Wheels unloaded,
  drivetrain untouched. Nothing moved; no driver, encoder or battery was
  connected.
- purpose: H0's own decision rule blocked the stage on upload reliability, and
  H0W's entry gate requires a passed H0. This session is the evidence for that
  gate; log entry D25.

## The captures in this directory

Raw captures are copied unchanged from `data/raw/` (`\r\n` line endings kept).
Every one was produced by `data/raw/h0repeat_serial.py` (scratch, gitignored),
which writes received bytes verbatim and reports when each `BOOT,...` line
arrived and the observed telemetry spacing - so the greeting and cadence claims
below come from timestamps in the file, not from an impression.

| file | what it contains |
|---|---|
| `h0repeat-uploads.log` | all 14 upload commands with their full output: 10 `pio run -t upload`, 3 `pio run -e bench -t upload`, 1 restore |
| `h0repeat-bench-health.log` | `env:bench`, 32 s, host reading: two `bench health` readings 25.5 s apart (B1) |
| `h0repeat-attach-vehicle.log` | vehicle image, 26 s, **no commands**: attach after an 8 s unhosted boot |
| `h0repeat-inspect-vehicle.log` | vehicle image: `version`, `status`, `checks`, `boot`, `config show`, `help` |
| `h0repeat-config-vehicle.log` | vehicle image, 68 s: the configuration set and the malformed/out-of-range set, **three rounds** |
| `h0repeat-telemetry-vehicle.log` | vehicle image, 66 s: `telemetry status/off/on`, `rate 10`, `rate 100`, `rate 0.5` and a 46 s default-rate soak |
| `h0repeat-inspect2-vehicle.log` | vehicle image: `config clear`, `config show`, `disarm`, `status`, `reset`, `status`, `config check` |
| `h0repeat-reattach-vehicle.log` | vehicle image: reattach after 20 s with nobody reading |

## Upload tally, verbatim

| # | command | result |
|---|---|---|
| 1-10 | `pio run -t upload` (vehicle) | **10 of 10 SUCCESS**, no manual touch |
| 11-13 | `pio run -e bench -t upload` | **3 of 3 SUCCESS**, no manual touch |
| 14 | `pio run -t upload` (restore vehicle) | SUCCESS, no manual touch |

Every attempt shows the same shape: `nano_touch: touching COM3 at 1200 bps
(attempt 1/2)` -> `nano_touch: bootloader present; uploading` -> `Download
done.` / `Done!`, exit status 0, and `COM3` re-enumerating afterwards with the
same VID/PID/serial. The hook never needed its second touch attempt. The expected
`OSError(22, 'A device which does not exist was specified.')` was printed on
every touch (the CDC port goes away while the board resets); it is not a failure.
**F-3 did not recur**, and no attempt left the board non-enumerating.

## What the captures show

### The boot line, the beacon, and `boot` (vehicle image, this source)

| capture | `BOOT,...` lines (seconds after the capture opened) |
|---|---|
| attach (no commands, 26 s) | **0.05** (first line), 10.02, 20.06 |
| inspect (6 commands, 26 s) | **0.06** (first line), 6.08 (the `boot` command), 20.05 |
| config (3 rounds, 68 s) | **0.05** (first line), then one per round |
| telemetry (66 s) | 6 lines, one at the start and one per ~10 s of silence |
| inspect2 (22 s) | **0.05** (first line), plus a beacon |
| reattach (after 20 s unhosted) | **0.05** (first line), 10.03 |
| bench health (32 s, bench image) | **0.06** (first line), 10.02, 20.05, 30.03 |

- **The attach edge works**: in all six vehicle captures and the bench capture, a
  host attaching to a board that had booted with nobody reading received
  `BOOT,...` as its **first line**. D22 saw this twice in three attaches; it is now
  7 of 7 in one session.
- **The silence rule fires and repeats**: a beacon arrived ~10.0 s after the last
  delivery (or the last host byte) in every capture long enough to contain one,
  and repeated. This is the D23 behaviour that had never been seen on a board.
- **The byte rule is restart-only, as the corrected wording says (D24).** In the
  26 s inspect capture, commands were typed at 1.55, 3.03, 4.52, 6.03, 8.03 and
  10.02 and **no** `BOOT,...` appeared near 10.06 (the deadline the attach would
  have produced on its own); the next one arrived at 20.05, i.e. 10 s after the
  last host byte. A greeting per command would have put a line before each answer.
- **`boot` answers with the same line the greeting sends**, verbatim:
  `BOOT,fw=ev-nano-r4-p7b,schema=1,cadence_ms=20,watchdog=0,checks_all=0,regulator=0,armed=0,telemetry=1,stride=1,ready=1`.
- The boot line is a **live status line**, not a frozen boot record: after
  `telemetry rate 10` was accepted, later beacons in the same boot reported
  `stride=5`.

### Configuration, refusals and malformed input (three identical rounds)

| input | answer (all three rounds) |
|---|---|
| `config set level=state distance=7.50 time=15.0 bottle_line=2.00` | `OK,cmd=config set,level=state,target_m=7.5000,time_s=15.0000,bottle_line_m=2.0000,bottle_start_m=3.0000,complete=1,rules=1,error=None` |
| `config show` / `config check` | the same `CONFIG,...` line |
| `announce` | `ERR,cmd=announce,reason=PolicyNotUsable,error=-,first_fail=RunPolicyUsable` |
| `arm` | `ERR,cmd=arm,reason=NoRunAnnounced` |
| **`config set distance=9.50`** (rule range 5.00-9.00) | **`ERR,cmd=config set,reason=ValueOutOfRange,error=TargetDistanceOutOfRange`**, and the next `config show` still reported `target_m=7.5000,...,rules=1` - **F-5's fix, confirmed on target** (D22 changed it; the first session had held the value) |
| `config set distance=999999999999999999999` | `ERR,cmd=none,reason=ValueOutOfRange` |
| `config set distance=abc` | `ERR,cmd=none,reason=InvalidValue` |
| `config se` | `ERR,cmd=none,reason=UnknownSubcommand` |
| `syntax junk here` | `ERR,cmd=none,reason=UnknownCommand` |
| `status extra tokens` | `ERR,cmd=none,reason=WrongTokenCount` |
| `config set level=galactic ...` | `ERR,cmd=none,reason=UnknownLevel` |

After every round, `status` reported `phase=Idle,armed=0,motor=Disabled,
effort_permille=0,fault=None,authz=AuthorizedStop`, with `config_complete=1,
config_rules=1` once the valid configuration was set: no malformed input and no
refusal changed the state. Each rejection also produced its `EVE1` event
(`event=CommandRejected,detail=<name>`), alongside `ConfigSet`, `AnnounceRefused`,
`ConfigCleared`, `ChainReset` and `TelemetryEnabled`.

The remaining verbs were exercised in the second inspection capture: `config
clear` -> `OK,cmd=config clear` and an empty `CONFIG,...complete=0`, `disarm` ->
`OK,cmd=disarm`, `reset` -> `OK,cmd=reset` with `event=ChainReset`, and `status`
and `config check` afterwards. Combined with D21's session, every verb in `help`
has now been answered on a board. `checks` reported `all=0,count=11,
first=ConfigurationValid,mask=63427` with every check named and valued, and
`help` lists `boot`.

### Telemetry: default state, rate changes and spacing

- At boot: `TELEMETRY,enabled=1,stride=1,rate_hz=50.0000,drop_s=1785,drop_e=0` -
  telemetry is on by default at 50 Hz, and `drop_s` had already accumulated while
  no host was attached.
- `telemetry off` -> `enabled=0`; `telemetry on` -> `enabled=1`; both accepted.
- Spacing, measured over 2488 samples in one capture: **2305 consecutive 20 ms
  intervals** at the default rate (46 s, 2300 control ticks), then **180 intervals
  of exactly 100 ms** after `telemetry rate 10` (`stride=5`); the only other
  deltas were the two gaps at the off/on transitions (2040 ms and 9300 ms, the
  capture's own read window). Every sample carried `dt_ms=20`.
- `telemetry rate 100` -> `ERR,cmd=telemetry rate,reason=AboveControlCadence`;
  `rate 0.5` -> `reason=BelowMinimum`.
- `drop_s` reset with the off/on cycle and stayed at 0 for the rest of the
  capture while the host kept up.

### Host stops reading

After 20 s with nobody attached, the reattach capture opened with `BOOT,...`
(first line, t=0.05) and flushed exactly **8 stale records** (`t_ms=264253` ..
`264953`, the queue's lane capacity, 100 ms apart because `rate 10` was still
set), then jumped 19.7 s to current records (`t_ms=284657` ...). `drop_s` was 365
at that point and rose only while nobody was reading; `drop_e` stayed 0.

### B1 and the cadence extremes (bench image, F-6)

Two `bench health` readings 25.5 s apart, host reading, default 50 Hz telemetry:

```
1st: BENCH,health=1,image=bench,enable=0,open_attempted=1,opened=0,open_refused=0,timeout_ms=250,
     passes=826688,refreshes=0,withheld=0,last_refresh_ms=0,first_refresh_ms=0,control_ticks=773,
     dt_ms=20,late_ms=0,dropped=0,worst_late_ms=1980,dropped_total=99,violations=0,armed=0,
     lines_accepted=0,lines_refused=0,responses_lost=0
2nd: same, except passes=2041483, control_ticks=2046
```

- The session extremes F-6 asked for are now readable: `worst_late_ms=1980`,
  `dropped_total=99`.
- **They did not grow**: both fields are identical across 25.5 s of healthy
  50 Hz telemetry with the host reading, while the last tick's `late_ms`/`dropped`
  were 0/0 in both readings and `violations=0`.
- Where they come from is explained by another measurement in this session: the
  board's first control tick after an **unhosted** boot is ~1980 ms late and ~99
  deadlines elapse during it, because `setup()`'s bounded wait for a host runs its
  full 2000 ms (every unhosted-boot capture opens with a sample at `dt_ms=2000`,
  and `t_ms=2000`). The extremes are that boot gap, not steady-state lateness.
- With enablement off, `first_refresh_ms` is 0, so the *boot-to-first-refresh* gap
  is still not obtainable from `env:bench`; that stays H0W Step 1's measurement.
- `enable=0,open_attempted=1,opened=0,open_refused=0` is the disabled build's
  expected shape: the open point ran and did not open.

## A finding about the bench image (not the vehicle)

In the bench capture a `bench health` command was typed at t=27.02 and the next
beacon still arrived at 30.03 - exactly 10.0 s after the previous beacon, as
though the host had said nothing. Cause, read from source rather than inferred:
`src/bench/BenchMain.cpp`'s loop calls `runtime.serviceStagedText(nowMs)` and
`evhw::benchConsole::service(...)`, and the console owns its **own** byte loop
(`src/bench/BenchConsole.h`), forwarding non-bench lines to
`runtime.handleCommandLine(...)` instead of running
`VehicleRuntime::serviceSerial()` - which is where `greeter_.observeHostByte()` is
called. In the bench image the greeting's byte input therefore never fires, and
beacons arrive on the pure silence cadence. Operator verbs still work in that
image (the console forwards them). The vehicle image's byte rule was observed
working in the inspect capture above. Recorded because H0W runs the bench image:
there, a beacon is evidence about the silence rule only.

## H0's acceptance rule, clause by clause

| Clause | Result |
|---|---|
| pre-registered upload/reboot counts met, each upload entering the bootloader with no manual touch | **met**: 10/10 vehicle, 3/3 bench, 0 manual touches, F-3 did not recur |
| the boot line is the first line a newly attached host sees, including a host attaching after boot | **met**: 7 of 7 attaches across both images |
| in a capture longer than `kSilenceMs`, a repeated `BOOT,...` line from the silence rule | **met**: beacons at ~10 s cadence in five vehicle captures and the bench capture |
| `boot` answered by that same line verbatim | **met** |
| a host attaching when the firmware already believed one was attached | **not observed this session** (D21 saw the condition once); counted neither way, as the protocol says |
| every documented command answered in the documented shape | **met** (this session plus D21 cover every verb in `help`) |
| every malformed input named and rejected with no state change, including an out-of-range value leaving the held configuration untouched | **met**, three identical rounds |
| cadence showing no dropped deadlines and no unexplained lateness at the chosen telemetry rate, with `dropped_total`/`worst_late_ms` reporting the extremes | **met for the running loop, with the boot gap recorded as a finding**: `late_ms`/`dropped` were 0/0 and the extremes did not grow over 25.5 s, but the session extremes are `worst_late_ms=1980` / `dropped_total=99`, all of it the bounded boot wait before the loop starts (see above). It is recorded, not averaged away. |
| no failed upload; no reboot without a boot cause | **met**: 14/14 uploads; every boot observed had its own `EVE1,...,event=Boot` |

**H0 passes**, with two recorded notes: the "attaching while a host was already
believed attached" condition was not observed (counted neither way), and the
cadence extremes are non-zero and explained by the boot gap rather than by the
running loop. F-3 remains an open limitation - not reproduced in 14 consecutive
uploads, cause still undetermined.

## What this session does NOT establish

- **Any vehicle fact.** No motor supply, no driver, no encoder, no battery, no
  drivetrain. Rows #1-#12 of `VERIFIED_STATE.md` are untouched, and no statement
  about motor-output safety follows from anything here (that is H1A's).
- **A root cause for F-3.** One occurrence ever, unreproduced in 14 uploads.
- **Watchdog behavior.** Nothing about the watchdog was exercised: the vehicle
  image ships it disabled, and the bench capture above is the *disabled* build.
- **Anything about a second board, a different cable or a different host.** Every
  observation in this repository comes from this one Nano R4 on this one port.
- **Repeatability of the greeting in the one condition the protocol counts
  neither way** (attach while a host was already believed attached).
