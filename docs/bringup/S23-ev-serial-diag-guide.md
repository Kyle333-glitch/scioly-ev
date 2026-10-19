# S23 — EV serial diagnostic operator guide (future hardware use)

**Status: procedure for a later session. Offline only — no board, no port,
no upload performed in S23. This document is not authorization for powered
operation, an upload, or any part of Block E/F.**

Tool: `tools/serial_diag/ev_serial_diag.py` (stdlib + pyserial only; baud
fixed at 115200; read-only allowlist; no upload; no 1200-baud trigger).
Tests: `python -m unittest tools.serial_diag.tests.test_ev_serial_diag -v`
(23/23 offline, fake transport, no COM port opened).

**Which Python needs pyserial:** whichever interpreter is used to invoke the
tool must be able to `import serial`. On this host `python` resolves to the
PlatformIO virtualenv (`C:\Users\danie\.platformio\penv\Scripts\python.exe`,
3.11.7) which already provides pyserial 3.5 as a PlatformIO dependency; a
stock Windows Python (e.g. 3.13) does not, and needs `pip install pyserial`
before it can run the tool.

**Dependency preflight (run before any hardware session, opens nothing):**

```
python -c "import sys; print(sys.executable)"
python -c "import serial; print(serial.VERSION)"
python -m pip show pyserial
```

Expect the first line to name the interpreter being used, the second to print
`3.5` (or another installed version), and the third to show the installed
location. If `import serial` fails, install into that same interpreter with
`python -m pip install pyserial` and re-run the preflight; do not proceed to
`--list` until it passes.

Read first: `docs/bringup/S21-hardware-return-serial-runbook.md` (transport
vs. firmware-selection vs. command-syntax isolation) and
`docs/bringup/P-H0-nano-upload-serial.md` (prerequisites, monitor must be
closed before any upload).

## 1. Obtain electrical clearance for USB-only connection

1. Complete the Block E unpowered inspection per
   `docs/bringup/E0-unpowered-inspection-protocol.md`; record it in the
   inspection record. Do not proceed on software say-so.
2. Confirm with the supervisor: USB-only session, motor supply **off and
   disconnected**, wheels off the floor / drivetrain unloaded, kill switch
   (rail disconnect) in reach.
3. Confirm the flashed image with the team (production `nano_r4` vs. bench).
   The tool reports `variant=` from the board; it cannot prove what *should*
   be flashed — compare against the upload log (`Download done.`) and
   `git rev-parse HEAD`.

## 2. Identify the enumerated USB port

1. Connect USB only. On Windows run:
   `python tools/serial_diag/ev_serial_diag.py --list`
   (opens nothing; lists `COMx` ports via pyserial).
2. Cross-check with `pio device list`: expect the Nano R4 application port
   (`USB VID:PID=2341:0074`). The bootloader (`2341:0374`) is a different
   device and means the board is not in application mode.
3. Confirm no other terminal, monitor, or script holds the port. Never guess
   `COM3` — use the port just enumerated.

## 3. Start passive transcript capture

Default session listens passively first, then sends `boot version status`:

```
python tools/serial_diag/ev_serial_diag.py --port COM3 --transcript logs/s23-session.log
```

* Opens at 115200 with `DTR=1 RTS=1` (S21 §5: assert both; wait ~300 ms is
  built into the passive window). Opening the port sends no `reset` (or any
other) command string and never uses the 1200-baud bootloader trigger (baud is
  fixed at 115200 with no option to change it). Whether asserting DTR/RTS or
  USB enumeration itself causes the Nano R4 to reset is board-specific and
  remains unverified — treat any greeting after open as a transport observation,
  not proof of what caused a reboot.
* Passively captures ~2.5 s before transmitting (greeting `BOOT,...` and any
  `EVS3` telemetry). Tune with `--passive-seconds 5` for a beacon check
  (>10 s captures the 10 s silence-beacon rule; see S21 §2).
* Every received boundary is timestamped in the transcript
  (`<seconds> RX <line>`, `TX <cmd>`, `SYS <note>`), including empty-line
  markers from CRLF handling.

## 4. Confirm the board sends a boot or version response

Expected on any current repo image once the MCU sees DTR high:

```
BOOT,fw=ev-nano-r4,rev=<sha|unknown>,dirty=<0|1>,variant=<production|bench|bench-watchdog>,cfg=<hex>,cfg_schema=1,...,ready=1
```

* `boot` re-sends that line on demand in every state — cheapest proof the
  host can both send and receive.
* `version` answers `VERSION,fw=...,rev=...,dirty=...,variant=...,cfg=...`.
* Even unsupported spellings (e.g. bare `health`/`ticks`, which this tool
  refuses to send) would answer `ERR,cmd=none,reason=UnknownCommand` on a
  working path — so **silence is never a spelling issue** (S21R).

## 5. Identify the installed firmware variant

Read `variant=` from the `BOOT`/`VERSION` lines (tool prints
`variant: <...>` at the end):

| `variant=` | meaning | bench verbs? |
|---|---|---|
| `production` | `env:nano_r4` vehicle image | refused by design; tool skips them unsent |
| `bench` | `env:bench` | allowed (`bench health`, `bench serial`, `bench ticks`) |
| `bench-watchdog` | `env:bench-watchdog` | allowed |
| `unknown` / absent | old or non-repo image | tool treats as unidentified; record and stop |

`bench health` additionally reports `image=bench`. A `conflict:` verdict
means mixed variant fields were seen — record the transcript, trust nothing.

## 6. Run permitted diagnostics

Production-safe (sent first):

```
--commands boot version status
```

Bench readouts (sent only after `variant=bench[-watchdog]` is seen;
skipped unsent otherwise):

```
--commands "bench health" "bench serial" "bench ticks"
```

What each proves: `boot`/`version` = identity + bidirectional path;
`status` = phase/armed plus the S19 serial counters
(`serial_abandoned/refused/oversize/sent/staged`) distinguishing "firmware
never received it" from "reply staged but lost"; `bench health` = loop/
watchdog + console counters; `bench serial` = shared-transport accounting;
`bench ticks` = raw encoder snapshot (B3, no distance conversion).

Never type `arm`, `drive`, `bench arm`, `bench drive`, `bench h1a-brake`,
`announce`, `reset`, or `config set` as a connectivity test — the tool
refuses them before opening the port, and no flag overrides that.

## 7. Save the transcript for troubleshooting

* The `--transcript` file is the session record: attach it to the run log
  with the port, cable, host, image hash (`git rev-parse HEAD`), and the
  `VERSION` line verbatim.
* Feed captures to the offline validators (`tools/telemetry/analyze.py`, `tools/p0_log_completeness.py`) for schema and completeness checks — offline analysis only, never a safety qualification.
* Keep raw logs under `data/raw/` (git-ignored scratch); promote only curated evidence per `docs/bringup/README.md`.

## 8. Stop safely if communication fails

* Missing port (`not found`, run `--list`): stop — cable, USB port, power, or enumeration issue. Says nothing about firmware.
* Occupied port (`access denied`): close the other monitor/terminal/script holding it, then retry once. Never force-open.
* Empty response (port opened, no bytes): check DTR reporting in the transcript, reseat cable, try a second client (`pio device monitor -b 115200`) before suspecting the image. Record the transcript either way.
* Timeout on a command: the tool reports which command got no reply and closes the port. For a passive-only re-run, use bare `--commands` (no values: opens the port, captures the passive window, sends nothing) and compare against the default probe (flag omitted entirely: sends `boot version status`). A command list containing only refused verbs (e.g. `arm`) is refused before opening, so it is not a passive check.
* Disconnect mid-session (`port disappeared`): the transcript is preserved up to the drop. Re-enumerate with `--list` (the COM number may change after re-enumeration), inspect cable/power, and log the event — a disappearing port is a transport finding, not a command result.
* To stop any session: Ctrl-C, close the tool (it always closes the port), then remove USB. Confirm the motor supply is still off. Never use `arm`/`drive`/`reset` to "see if the link works".

## Future H2 encoder-reading example (not performed in S23)

When the bench image is confirmed (`variant=bench`) in a supervisor-cleared, USB-only, motor-supply-disconnected session with wheels free to turn by hand:

```
python tools/serial_diag/ev_serial_diag.py --port COM3 --commands "bench health" "bench ticks" --transcript logs/h2-ticks-baseline.log
```

1. Record `ticks_raw`, `counted_edges`, `ignored_updates` untouched.
2. Repeat `bench ticks` without touching anything — values must be identical.
3. Turn the wheel 10 full revolutions by hand in the marked forward direction, wait ~1 s, read `bench ticks` again; compute `delta` and `ticks/rev`.
4. Repeat in reverse; record sign with direction. Repeat the pair at least three times; do one closed-loop (there-and-back) run.
5. Never convert ticks to distance here — CPR, gear ratio, and wheel circumference are unmeasured (`VERIFIED_STATE.md` rows #4–#6).

Full procedure: `docs/bringup/P-H2-encoder-manual.md` and S21 runbook §4. This example was not executed in S23 (no hardware).

## Limitations requiring real hardware

* DTR/RTS propagation through the Windows USB-CDC stack is unverified until a `BOOT` or command reply is observed on the board.
* Opening the port deliberately sends no reset command and never touches the 1200-baud bootloader path, but any board-specific reset triggered by DTR/RTS assertion or USB enumeration itself remains unverified on the Nano R4.
* Silence vs. image mismatch cannot be separated offline; the tool reports what it saw, never what the board did.
* `variant=unknown` (legacy or non-repo image) and `conflict:` verdicts require on-target follow-up, not software overrides.
* Closing the monitor before any upload still applies (`docs/limitations.md`).

