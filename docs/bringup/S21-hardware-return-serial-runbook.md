# S21 - Hardware-return serial console and encoder diagnostic runbook

**Prepared offline, with no board, no port and no upload.** Everything below is
read from source at `a35aa41` plus the existing repository records. It is a
procedure for a later session, not a result, and it is **not** authorization for
powered operation, an upload, or any part of Block E/F.

- Scope of this document: identify the running image, use the correct diagnostic
  command syntax, read encoder counts by hand, and troubleshoot a silent port
  **without** issuing any arm, drive or run command.
- Not in scope: bypassing the Block E electrical inspection, the motor-supply
  disconnect rule, the arming gates, or `P-H3`'s requirements. Those are unchanged.
- The encoder measurements themselves belong to
  [`P-H2`](P-H2-encoder-manual.md); this runbook only covers reaching the readout.

---

## 1. Serial-interface matrix, by firmware environment

| | `nano_r4` (vehicle, `pio run`) | `bench` (`pio run -e bench`) | `bench-watchdog` |
|---|---|---|---|
| Entry point | `src/main.cpp` | `src/bench/BenchMain.cpp` | `src/bench/BenchMain.cpp` |
| Build identity `variant=` | `production` | `bench` | `bench-watchdog` |
| Production verbs | yes | forwarded, unchanged | forwarded, unchanged |
| `bench ...` verbs | **none** (not compiled) | yes | yes |
| `bench h1a-brake` | never | **present** - `env:bench` currently sets `EV_BENCH_H1A_DIAGNOSTIC=1` | absent (does not inherit the define) |
| Watchdog enablement requested | no | no | yes (`EV_BENCH_WATCHDOG_ENABLEMENT_REQUESTED`) |
| Can report raw encoder ticks | **no** | yes, `bench ticks` (B3) | yes, `bench ticks` |
| Line prefix of its own output | `BOOT`, `OK`, `ERR`, `STATUS`, `CONFIG`, `CHECK`, `PLAN`, `TELEMETRY`, `VERSION`, `EVS`, `EVE` | the same, plus `BENCH` | the same, plus `BENCH` |

Common to every image (source: `src/main.cpp`, `src/bench/BenchMain.cpp`,
`src/SerialTransport.h`, `lib/evcore/src/SerialLineFraming.h`):

- `Serial.begin(115200)`; this board's `Serial` is USB-CDC, so **the baud setting
  is accepted and ignored**. 115200 is only what the host should be told.
- One line per command, terminated by **`\n` or `\r`**; CRLF is framed as one line
  plus one empty line (no answer for the empty one). The firmware's own replies
  end in **`\r\n`**.
- No echoed input, no prompt, no banner printed from `setup()`.
- Framing is strict: printable ASCII only, one space between tokens, exact
  lowercase spelling, ≤96 characters per line. A tab, a double space, a leading or
  trailing space, or a missing terminator is refused or never framed.
- At most one command is parsed per loop pass and one reply is staged per command.

### Production command vocabulary (verified in `lib/evcore/src/SerialCommandParser.h`)

```
help  version  status  checks  boot  plan  announce  arm  disarm  reset
config show  config check  config clear
config set level=<regional|state|national> distance=<m> time=<s> bottle_line=<m>
telemetry on  telemetry off  telemetry status  telemetry rate <hz>
```

Anything else is answered `ERR,cmd=none,reason=UnknownCommand`. That includes
**`health`**, **`ticks`**, `bench`, `bench health` and `bench ticks` on the vehicle
image - refused by name, not ignored. There is no production spelling that returns
raw encoder ticks: the `EVS` sample carries derived `pos_m`, not a tick count.

### Bench vocabulary (`lib/evcore/src/BenchCommand.h`, `src/bench/BenchConsole.h`)

```
bench            (help)          bench help        bench health
bench ticks      bench drive-status                 bench inputs / bench trigger
bench pins       bench serial    bench wdt         bench resetsrc [clear]
bench arm        bench stop      bench reset
bench drive <fwd|rev> <permille> <ms>              (bounded; motion)
[-DEV_BENCH_H1A_DIAGNOSTIC only] bench h1a-brake <ms>   (temporary, `env:bench`)
```

- Read-only diagnostics that are safe to type as a first contact: `bench health`,
  `bench ticks`, `bench serial`, `bench pins`, `bench inputs`, `bench wdt`,
  `bench resetsrc`, `bench drive-status`. None of them arm, drive or reset anything.
- `health`/`ticks` **without** the `bench` prefix are not bench verbs; the console
  forwards them and production refuses them. Seeing
  `ERR,cmd=none,reason=UnknownCommand` in answer to `ticks` is therefore proof that
  the port works and the console is namespaced - not proof of a dead link.
- `bench reset` and production `reset` both zero the decoder's tick count (see §4).
- Never type `bench arm`, `bench drive`, `bench h1a-brake`, `arm`, `announce` or
  `reset` as a connectivity test.

---

## 2. Startup and output behaviour (production and bench alike)

1. `runtime.begin()` de-energizes the driver, attaches the encoder interrupt,
   samples inputs and **owes** a `BOOT` line. Nothing is printed yet.
2. `Serial` is opened, then there is one bounded wait of up to 2 s for a host.
3. The loop runs. A due `BOOT` is staged **only when the host's DTR line reads
   asserted** - staged lines cannot move otherwise, because this core's
   `Serial.write()` gates on `connected()` (DTR). The greeting is re-owed after
   10 s of host silence, and on a newly asserted DTR *or* RTS.
4. A typed command's answer is staged as an urgent line and pumped; with no host
   asserting DTR the write accepts zero bytes and the reply stays staged, so the
   host sees **nothing at all** while the firmware still parsed the command.

Consequences for a session:

- Expected first line after opening a port with DTR asserted:
  `BOOT,fw=ev-nano-r4,rev=<sha|unknown>,dirty=<0|1>,variant=<production|bench|bench-watchdog>,cfg=<8 hex>,cfg_schema=1,schema=...,cadence_ms=20,watchdog=<0|1>,checks_all=<0|1>,...,ready=1`.
  `variant=` is the image identification; `bench health` also reports `image=bench`.
- `boot` re-sends that line on request, in every state, and is the cheapest proof
  that the host can both send and receive.
- A quiet port with a blinking LED is a **host-side or DTR problem**, not evidence
  about the firmware's state.

---

## 3. Prior observations, root-cause analysis

Facts, from source only:

| Observation | Certainty | Basis |
|---|---|---|
| Production firmware has no `health` or `ticks` command | Confirmed | `SerialCommandParser.h` vocabulary; new offline test `test_diagnostic_verbs_are_not_production_commands` |
| Production would answer either with `ERR,cmd=none,reason=UnknownCommand` | Confirmed | `VehicleRuntime::rejectWithoutCommand()` → `formatOperatorRefusal()` |
| `bench health` / `bench ticks` exist only in the bench image | Confirmed | `README.md` build-graph guarantee, `env:bench`/`env:nano_r4` `build_src_filter`; new test `test_no_bench_line_is_a_production_command` |
| A reply cannot leave the port unless the host asserts **DTR** | Confirmed | `SerialTransport.h` (`HostLineState`, write gate), `VehicleRuntime::serviceStagedText()` (`hostAttached_ = state.dtr`) |
| The `BOOT` line is not staged at all while DTR reads low | Confirmed | same |
| Production cannot report raw encoder ticks by any command or telemetry field | Confirmed | command table plus `TelemetryFormat.h` sample fields |
| The later `bench health`/`bench ticks` attempt with COM3 absent says nothing about the firmware | Confirmed | no port, no bytes exchanged |
| The host used a client that does not assert DTR/RTS | **Probable, unverified** | a .NET `System.IO.Ports.SerialPort` defaults `DtrEnable`/`RtsEnable` to false; no capture of the client's settings exists. This is the single explanation that matches "commands typed, absolutely nothing back" |
| The flashed image was the bench image | **Undetermined** | requires `variant=` / `image=bench` from the board |
| The command line was sent without a terminator, or the client never read | **Undetermined** | host-side; no capture exists |

Leading explanation, stated as a hypothesis: with no DTR the firmware parses and
answers into a port that cannot transmit, so the session looks identical to a dead
board. The remedy is a host that asserts DTR (and RTS), not a firmware change. No
source defect has been demonstrated, and none is patched in S21.

---

## 4. Encoder tick readout - what to collect, and its semantics

Read path: pin interrupt on **A CHANGE** → `ev::QuadratureDecoder` (x2: one count
per A transition, sign from the landing (A,B) state) → `snapshotTicks()` (masked,
coherent `ticks`/`counted_edges`/`ignored_updates`) → `bench ticks`:

```
BENCH,ticks=1,raw=1,ticks_raw=<signed decimal>,counted_edges=<n>,ignored_updates=<n>
```

Meaning of the fields (source: `QuadratureDecoder.h`, `EncoderInput.h`):

- `ticks_raw` - signed `int32_t`, **session-cumulative**, zeroed at boot by
  `encoderInput::begin()`, and zeroed again by a `reset` command (production, or
  forwarded through the bench console). It is not zeroed per command.
- x2 decoding: four quadrature states per electrical cycle, **two counted A
  edges** per cycle. `ticks_raw` is not a x4 count.
- Which sign is physical forward travel is **unmeasured** (`VERIFIED_STATE.md`
  #4). Record the sign together with the direction the wheel was turned; never
  assume `+` is forward.
- `counted_edges` is the number of A transitions decoded; `ticks_raw` is their
  signed sum. `ignored_updates` counts decoder updates that were not A
  transitions. The ISR is attached to A only, so a small value is normal and a
  large one is a signal-quality question for H2, not an error to explain away.
- No CPR, gear ratio or wheel circumference is assumed anywhere; do not convert
  ticks to distance in this session.

### Collection procedure (unpowered; motor supply off and disconnected)

Preconditions: supervisor-cleared USB-only session, Block E inspection unchanged,
wheels off the floor, drivetrain free to turn by hand, forward direction marked on
the chassis, a visible mark on wheel or axle. Then, with the **bench** image:

1. Open the port with DTR asserted; wait for `BOOT,...variant=bench...`.
2. `bench health` → expect `image=bench`; record the whole line.
3. `bench ticks` → record `ticks_raw`, `counted_edges`, `ignored_updates`.
4. `bench ticks` again **without touching anything** → the three values must be
   identical. A change here is a finding (noise, or motion), not rounding.
5. Turn the wheel **10 full revolutions by hand in the marked forward direction**,
   then stop and wait ~1 s. `bench ticks` → record the new values; compute
   `delta = ticks_raw - previous` and `ticks/rev = delta / 10`.
6. Repeat step 5 in the reverse direction. Record the sign with the direction.
7. Repeat the forward/reverse pair at least three times; also rotate one way and
   back to the mark once and record the closed-loop net delta.
8. `bench ticks` a final time; the values must still agree with the last reading.

Do not power the motor, do not arm, and do not use `pos_m` as a substitute: it is
derived through an unmeasured scale. This procedure produces raw counts only; it
cannot calibrate distance, and no software test here substitutes for it.

---

## 5. No-response troubleshooting ladder (no motion commands)

Work top to bottom; each step is read-only.

1. **Port present?** `pio device list` - expect `USB VID:PID=2341:0074`. No port
   means USB/enumeration/cable, not firmware. (The earlier COM3-is-missing error
   was exactly this and carried no firmware information.)
2. **Is the firmware running?** The status LED toggles every 500 ms in both images.
   No blink → not this firmware, or a hung/reset loop.
3. **Open with line state asserted.** Prefer `pio device monitor -b 115200`
   (DTR/RTS asserted, CRLF supported). If using a raw
   `System.IO.Ports.SerialPort` in PowerShell, set `DtrEnable = $true`,
   `RtsEnable = $true`, `NewLine = "`r`n"`, and read until timeout. Defaults leave
   both lines low and the firmware mute by design.
4. **Expect `BOOT,...` within ~10 s** of opening. If it does not arrive, type
   `boot` + Enter; an answer proves RX and TX, no answer keeps the question host-side.
5. **Check the host's own habits:** correct COM port, terminator actually sent
   (`WriteLine`, not `Write`), no local-echo confusion, monitor not holding the
   port while another tool opens it.
6. **Identify the image:** `version` → `variant=`; on a bench image `bench health`
   → `image=bench`. If `version` reports `UnknownCommand`, the image is not one of
   this tree's builds.
7. **Use the right spelling:** production `health`/`ticks` is a refusal by design;
   on a bench image the readouts are `bench health` / `bench ticks`. A named `ERR`
   line means the serial path works.
8. **Only if all of the above answered:** proceed to `bench ticks` and §4.

Never use `arm`, `bench arm`, `bench drive`, `bench h1a-brake`, `announce` or
`reset` to "see if the link works". A 1200-bps touch is an upload mechanism, not a
diagnostic. The monitor must be closed before any upload (`docs/limitations.md`).

---

## 6. What this runbook does not establish

- No firmware behavior, encoder count, direction, or calibration is verified by
  this document or by the S21 native tests.
- A `BENCH` line is a report of what the firmware believed; it is not a
  measurement until a protocol records it with hand measurements beside it
  (`docs/bringup/README.md`).
- Software coherence (`snapshotTicks()`, the masked reset/reference boundary) does
  not prove mechanical standstill and does not preserve moving electrical edges.
- The `env:bench` image currently contains the temporary H1A instrument. If that
  image is flashed, `bench help` will list `h1a-brake`; it must never reach a
  production build, and its presence is not permission to run it.
