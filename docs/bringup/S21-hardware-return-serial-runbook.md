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

- Expected first line after opening a port when the device detects DTR asserted:
  `BOOT,fw=ev-nano-r4,rev=<sha|unknown>,dirty=<0|1>,variant=<production|bench|bench-watchdog>,cfg=<8 hex>,cfg_schema=1,schema=...,cadence_ms=20,watchdog=<0|1>,checks_all=<0|1>,...,ready=1`.
  `variant=` is the image identification; `bench health` also reports `image=bench`.
- `boot` re-sends that line on request, in every state, and is the cheapest proof
  that the host can both send and receive.
- If this project's firmware is running and the serial path (RX framing, loop
  servicing, and DTR-gated TX) is functioning, even unsupported commands such as
  bare `health` or `ticks` produce `ERR,cmd=none,reason=UnknownCommand` — never
  silence.
- A quiet port with a blinking LED can still indicate an unverified DTR path at
  the Windows driver/USB-CDC/firmware boundary, an unterminated write or read
  timing mismatch on the host, or a flashed image that is not one of this
  repository's builds; it is not evidence about the vehicle's physical state.

---

## 3. Prior observations, root-cause analysis (corrected in S21R)

### Session evidence

In the earlier PowerShell session, the host explicitly executed:

```powershell
$port = [System.IO.Ports.SerialPort]::new("COM3", 115200)
$port.DtrEnable = $true
$port.ReadTimeout = 1000
$port.Open()
```

`COM3` opened, and the session sent `health` and `ticks`; both returned **"No
response received."** A subsequent retry using `bench health` and `bench ticks`
failed to open `COM3` because the device was no longer connected.

S21 originally suggested that the initial silence probably resulted from
PowerShell leaving `DtrEnable` false by default. **That explanation conflicts
with the actual session transcript above, where `$port.DtrEnable = $true` was
explicitly set before `$port.Open()`, and is retracted here.**

### Verified facts vs. unverified links and unknowns

| Fact / question | Status | Basis |
|---|---|---|
| Host-side `$port.DtrEnable = $true` was set before `$port.Open()`, and `COM3` opened | **Confirmed (session fact)** | User's PowerShell session transcript (`SerialPort::new("COM3", 115200)`, `$port.DtrEnable = $true`, `$port.ReadTimeout = 1000`, `$port.Open()`) |
| Sending `health` and `ticks` in that first session returned "No response received" | **Confirmed (session fact)** | User's PowerShell session transcript |
| The later `bench health` / `bench ticks` retry failed to open `COM3` because the device was absent | **Confirmed (session fact)** | `COM3` was no longer present; no port opened and no bytes were exchanged, so that retry carries no firmware information |
| Production firmware (`env:nano_r4`) has no `health`, `ticks`, or `bench ...` commands | **Confirmed (source)** | `SerialCommandParser.h` vocabulary; offline test `test_diagnostic_verbs_are_not_production_commands` |
| Bench firmware (`env:bench`, `env:bench-watchdog`) requires the `bench` prefix (`bench health`, `bench ticks`) and forwards bare `health`/`ticks` to the production parser | **Confirmed (source)** | `BenchCommand.h`, `BenchConsole.h`; offline tests `test_the_diagnostic_verbs_require_the_bench_prefix` and `test_no_bench_line_is_a_production_command` |
| If either production or bench firmware from this tree receives and processes a newline-terminated `health` or `ticks` and the serial output path is functioning, it stages and emits `ERR,cmd=none,reason=UnknownCommand` (plus `EVE3` `CommandRejected` if telemetry drains) | **Confirmed (source)** | `BenchConsole::handleLine()` forwards `BenchVerb::None` to `VehicleRuntime::handleCommandLine()` → `rejectWithoutCommand()` → `formatOperatorRefusal()` |
| Production cannot report raw encoder ticks by any command or telemetry field | **Confirmed (source)** | Command table plus `TelemetryFormat.h` sample fields |
| Firmware TX (`Serial.write()`) and `BOOT` staging require the core's USB-CDC DTR state (`static_cast<bool>(Serial)` / `connected()`) to read high on the MCU | **Confirmed (source)** | `SerialTransport.h` (`HostLineState`), `VehicleRuntime::serviceStagedText()` (`hostAttached_ = state.dtr`), `HostGreetPolicy.h` |
| Whether Windows and the USB-CDC driver actually propagated DTR assertion to the device on that `Open()` | **Unverified** | Host property assignment is confirmed, but no USB bus trace or device-side line-state readout was captured (cf. D22 F-4 on Windows CDC line-state transitions) |
| Whether the firmware detected the asserted DTR signal (`hostAttached_ == true`) | **Unverified** | Requires device-side observation (a received `BOOT` or command reply) |
| Which firmware image was installed on the board (`nano_r4`, `bench`, `bench-watchdog`, or an older/non-repo sketch) | **Unknown** | No `BOOT`, `VERSION`, or `BENCH` line was received to report `variant=` or `image=bench` |
| Whether the device actually received and parsed the `health` and `ticks` commands | **Unknown** | No reply or counter readout (`STATUS` `cmd_rejected` / `bench health` `lines_forwarded` / `bench serial`) was returned |

### Candidate explanations for the initial silence (hypotheses only, unverified)

Because a functioning production or bench image from this repository would have
answered a framed `health` or `ticks` command with
`ERR,cmd=none,reason=UnknownCommand` (and would also have emitted `BOOT,...` and
default-enabled `EVS3` telemetry within the 1000 ms read window once DTR was seen
high after `setup()`), command spelling alone **cannot** explain "No response
received." The remaining candidate mechanisms are hypotheses to be separated when
hardware returns, not conclusions:

1. **Unverified DTR propagation or firmware detection across the USB-CDC link:**
   Although `$port.DtrEnable = $true` was set in PowerShell, `$port.RtsEnable`
   was not explicitly set, and whether the Windows `usbser`/CDC stack delivered a
   SET_CONTROL_LINE_STATE with DTR=1 that `static_cast<bool>(Serial)` observed on
   the RA4M1 is unverified. If the MCU continued to read DTR low, `Serial.write()`
   accepted zero bytes and both `BOOT` and `ERR,cmd=none,reason=UnknownCommand`
   remained silent.
2. **Unknown flashed firmware image or non-running loop:** The board may not have
   been running a current `nano_r4` or `bench` image from this repository (or its
   main loop was not servicing USB-CDC), in which case this tree's parser and
   greeter were not what was executing.
3. **Host-side write framing or read timing in the script after `Open()`:** If
   `health`/`ticks` were sent without a `\r` or `\n` terminator (e.g.,
   `Write(...)` without a newline rather than `WriteLine(...)`), `CommandLineReader`
   would hold the partial line without dispatching `LineReady`; or if the port
   opened during the 2 s `setup()` wait / USB enumeration transition or was closed
   before a line was read back, the 1000 ms read could time out without proving
   what the firmware did.
4. **Transient USB/cable disconnect already in progress:** The device was absent on
   the very next connection attempt (`COM3` missing), so an intermittent physical
   or USB enumeration drop during the first session cannot be ruled out.

No source-code defect has been demonstrated from these observations, and no
firmware code is changed in S21 or S21R.

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

## 5. Hardware-return troubleshooting procedure (read-only, reproducible, no motion commands)

Work top to bottom. Every step is read-only, uses no motion/arm/reset command, and
isolates **transport**, **firmware selection**, and **command syntax** in that
order.

1. **Confirm USB enumeration and single-process ownership (Transport - host OS):**
   Run `pio device list` and confirm the Nano R4 appears with
   `USB VID:PID=2341:0074` on the expected `COMx` port, and that no other
   terminal, monitor, or script holds the port.
   - *If absent:* stop; this is a cable, USB port, power, or enumeration issue
     (like the second prior retry where `COM3` had disappeared) and says nothing
     about firmware.
2. **Observe the status LED before sending anything (Firmware loop liveness):**
   In `nano_r4`, `bench`, and `bench-watchdog`, `LED_BUILTIN` toggles every
   500 ms inside `loop()`.
   - *If not blinking at ~1 Hz:* the board is either not running an image from
     this repository or is stuck before/inside `loop()`.
3. **Open the port reproducibly and listen passively before typing (Transport - DTR/TX):**
   Prefer `pio device monitor -b 115200` (or, in PowerShell, open with
   `DtrEnable = $true`, `RtsEnable = $true`, `NewLine = "`n"`,
   `ReadTimeout = 2500`, wait ~300 ms after `Open()`, and drain incoming lines for
   2–3 s before writing).
   - *Expected on any current repo image once the MCU sees DTR high:* an initial
     `BOOT,fw=ev-nano-r4,...,variant=<production|bench|bench-watchdog>,...,ready=1`
     greeting (or periodic `EVS3,...` telemetry samples every 20 ms while
     telemetry is in its default enabled state).
4. **Probe bidirectional RX/TX with `boot` and `version` (Transport vs. Firmware selection):**
   Send `boot` followed by `\n` (or `\r\n`), then send `version`. Both are
   read-only and accepted in every state by **all three** environments
   (`nano_r4`, `bench`, `bench-watchdog`).
   - *If `BOOT,...` and `VERSION,fw=ev-nano-r4,rev=...,dirty=...,variant=<production|bench|bench-watchdog>,cfg=...` return:*
     both RX framing and DTR-gated TX are verified, and **`variant=`**
     conclusively identifies which firmware environment is flashed.
   - *If `ERR,cmd=none,reason=UnknownCommand` returns to `version`:* serial RX/TX
     works, but the flashed firmware is not a current build from this tree.
   - *If nothing returns at all (and no `EVS3` lines appear):* the failure is in
     transport/DTR propagation, host line termination, or a non-repo flashed
     image — **not** `bench` vs. `production` command spelling. Test with a
     second client (`pio device monitor -b 115200`) before suspecting the image.
5. **Verify command-syntax and namespace behavior (Command syntax vs. Firmware selection):**
   Once `version` has identified `variant=`:
   - If `variant=production`: `status` and `checks` are valid read-only queries.
     Typing `health`, `ticks`, `bench health`, or `bench ticks` will return
     `ERR,cmd=none,reason=UnknownCommand` by design (proving the parser and TX
     path work, and confirming that raw encoder ticks are unavailable until
     `env:bench` is flashed in an authorized session).
   - If `variant=bench` or `variant=bench-watchdog`: type **`bench health`**,
     **`bench serial`**, and **`bench ticks`** (with the required `bench `
     prefix). Bare `health` or `ticks` is forwarded to the production parser and
     returns `ERR,cmd=none,reason=UnknownCommand` (`lines_forwarded` increments in
     `bench health`).
6. **Check serial-path counters if any reply was missed:**
   Read `status` (`cmd_accepted`, `cmd_rejected`, `responses_lost`,
   `serial_abandoned`, `serial_sent`, `serial_staged`) and, on a bench image,
   `bench health` (`lines_accepted`, `lines_refused`, `lines_forwarded`,
   `responses_lost`) plus `bench serial`. These counters distinguish "the
   firmware never received a framed line" from "the firmware parsed the line and a
   reply was staged or abandoned."
7. **Only after `variant=bench` and `bench health` (`image=bench`) are confirmed:**
   Proceed to `bench ticks` and the unpowered manual encoder procedure in §4.

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
