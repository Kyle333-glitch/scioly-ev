# P-H0 session 1 - Nano R4 upload, boot and serial link

- protocol: `docs/bringup/P-H0-nano-upload-serial.md`
- date: 2026-09-28
- operator: danie (team); captures scripted by the agent, commands chosen by the
  agent and reviewed in the session terminal
- board: Arduino Nano R4, USB serial `350D331C5A333131021133354B573462`
- host: Windows, PlatformIO Core 6.2.0, `dfu-util 0.11-arduino4`, pyserial 3.5
  from `~/.platformio/penv`
- flashed tree: `git rev-parse HEAD` = `8e42d13b69e6fcbb79275ff7699ffe4c9608fcc3`
  (clean working tree; no firmware source was changed in this session)
- images: `env:nano_r4` (vehicle, `fw=ev-nano-r4-p7b`) and `env:bench`
- motor supply: **not connected**. Wheels unloaded. Nothing moved; no driver,
  encoder or drivetrain behaviour was exercised.
- log entry: `DEVELOPMENT_LOG.md` D21. Rows added: `VERIFIED_STATE.md`, on-target
  section.

## The captures in this directory

Captures are copied **unchanged** from `data/raw/` (`\r\n` line endings kept).
`h0-boot1.log` is the largest (210 kB) and is kept in full because the drop
counters it carries are the evidence about a host that is not reading.

| file | what it contains |
|---|---|
| `toolchain.session.txt` | re-run transcript: `pio --version`, the penv Python/pyserial versions, `pio device list`, `dfu-util -l` in application mode |
| `h0-boot1.log` | the first boot after the driver fix: the boot record, ~37 s of telemetry with no host attached, then 14 s of commands (`version`, `status`, `checks`, `config show`, `help`, `announce`, `arm`) |
| `h0-config.log` | `telemetry off/status`, `config show`, `config set` (valid), `config show`, `config check`, `checks`, `announce`, `arm`, then the malformed / out-of-range / junk inputs and `status` after each |
| `h0-rate.log` | `telemetry on`, `telemetry status`, `telemetry rate 10`, `telemetry rate 100`, `telemetry rate 0.5` |
| `h0-cycle-2.log` ... `h0-cycle-5.log` | boot record + `version` after upload-triggered reboots 2-5 |
| `h0-powercycle.log` | boot record + `version` after the physical USB power cycle |
| `h0-cycle-7.log` | boot record + `version` after the vehicle image was restored to the board |
| `h0-bench-a.log`, `h0-bench-b.log` | `bench health` (B1) shortly after the bench-image boot, and again 25 s later |

There is no `measurements.csv`: this session made no hand measurement. Every
number in it is firmware output or host tool output.

## The toolchain steps, verbatim

These were printed to the session terminal and are transcribed here, not tee'd
to a file at the time. They are the reason the session could not start, and the
reason it eventually could.

Before any driver was installed, the board was in application mode and the
uploader could not reach the bootloader:

```
$ pio run -t upload --upload-port COM3
CURRENT: upload_protocol = dfu
Uploading .pio\build\nano_r4\firmware.bin
Cannot open DFU device 2341:0074 found on devnum 5 (LIBUSB_ERROR_NOT_FOUND)
No DFU capable USB device available
*** [upload] Error 74
========================= [FAILED] Took 51.07 seconds =========================
```

Windows, with the board in bootloader mode, reported no driver for it:

```
Status       : Error
FriendlyName : Nano R4 DFU
InstanceId   : USB\VID_2341&PID_0374\350D331C5A333131021133354B573462
DEVPKEY_Device_ProblemCode : 28
DEVPKEY_Device_HardwareIds : {USB\VID_2341&PID_0374&REV_0100, USB\VID_2341&PID_0374}
```

`USB\VID_2341&PID_0374&REV_0100` is exactly the hardware ID that Arduino's own
`renesas.inf` - already on disk in the installed PlatformIO framework package at
`~/.platformio/packages/framework-arduinorenesas-uno/drivers/` - matches
(`DeviceIDNanoR4DFU = "VID_2341&PID_0374&Rev_0100"`). The operator approved an
elevated run of that package's own installer:

```
dpinst-amd64.exe /SE /SW /SA          (drivers/, elevated, UAC approved by the operator)
```

42 s later the bootloader became usable, and stayed usable for the rest of the
session:

```
Found DFU: [2341:0374] ver=0100, devnum=7, cfg=1, intf=0, path="1-3", alt=0,
           name="@CodeFlash /0x00000000/8*2Ka,120*2Kg", serial="350D331C5A333131021133354B573462"
Found DFU: [2341:0374] ver=0100, devnum=7, cfg=1, intf=0, path="1-3", alt=1,
           name="@DataFlash /0x08000000/8*1Kg",  serial="350D331C5A333131021133354B573462"
```

## Upload attempts, verbatim tally

PlatformIO's DFU uploader performs no 1200-bps touch, so every attempt below
except the first was preceded by a host-issued touch (open the port at 1200 bps
with DTR low; `cores/arduino/usb/SerialUSB.cpp` `CheckSerialReset()` ->
`goBootloader()`).

| # | command | result |
|---|---|---|
| 1 | `pio run -t upload --upload-port COM3` | **FAILED**, Error 74 - no bootloader mode, no driver (above) |
| 2 | `pio run -t upload` (after the driver install) | SUCCESS 5.65 s, `Download done.`, 65964 bytes, `Done!` |
| 3-6 | `pio run -t upload` x4 | SUCCESS 4.95 s / 5.17 s / 4.89 s / 4.67 s |
| 7 | `pio run -e bench -t upload` | **FAILED**, Error 74 - `No DFU capable USB device available`; the board was left not enumerating (below) |
| 8 | `pio run -e bench -t upload` (retry) | SUCCESS 5.24 s |
| 9 | `pio run -t upload` (restore vehicle image) | SUCCESS 4.94 s |

After attempt 7 the board was on the bus but would not enumerate:

```
Status       : Error
FriendlyName : Unknown USB Device (Device Descriptor Request Failed)
InstanceId   : USB\VID_0000&PID_0002\5&28FFA4CE&0&3
```

`Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match 'VID_2341' }`
returned **nothing**, and `pio device list` was empty, so no software path could
reach it. The operator unplugged the USB cable, waited, and plugged it back into
the same port with the same cable; COM3 returned with the same VID/PID/serial
and `h0-powercycle.log` shows the fresh boot. The cause of the non-enumeration is
**not determined** (see D21 F-1/F-2).

## Facts this session's evidence backs

- the bootloader-mode DFU interface is usable only with Arduino's `renesas.inf`
  driver installed; before that, Windows problem code 28 and `dfu-util` cannot
  open the device (`toolchain.session.txt`, transcript above);
- `pio run -t upload` cannot enter the bootloader by itself; a host-issued
  1200-bps touch is required (source inspection plus attempts 1 and 2 above);
- seven boots were observed (attempts 2-6, 8, 9); each produced exactly one
  `EVE1,...,event=Boot` record and reported
  `VERSION,fw=ev-nano-r4-p7b,telemetry_schema=1`;
- on target the control period is 20 ms, telemetry defaults to enabled at stride
  1 (50 Hz), `telemetry rate 10` gave 90 consecutive samples exactly 100 ms
  apart, `telemetry off` stopped the samples, and `rate 100` / `rate 0.5` were
  refused as `AboveControlCadence` / `BelowMinimum` (`h0-rate.log`);
- with no host attached the sample and event lanes drop records rather than
  block: `h0-boot1.log` opens with `drop_s=1790` accumulated across ~37 s, and
  only the queue's 8-record capacity of stale records was flushed on reconnect;
- the documented command set answered in the documented shapes, and every
  malformed input was refused with a named reason while `status` stayed
  `phase=Idle` (`h0-config.log`);
- B1 reports `late_ms`/`dropped` for the last control tick, not a worst case, so
  `h0-bench-a.log` + `h0-bench-b.log` are two samples 25 s apart (both
  `late_ms=0 dropped=0 violations=0 dt_ms=20`), not a worst-case over the soak.
  (`worst_late_ms`/`dropped_total` were added to B1 afterwards, in D22; they have
  not been read from a board yet.)

## What this session does NOT establish

- **Upload reliability.** The pre-registered count was not met: 7 successes and
  one unexplained failure (attempt 7) in eight attempts on the same procedure,
  and the failure needed a physical power cycle. P-H0's decision rule says a
  failed upload blocks the stage, so H0 is recorded as **not passed** by this
  session (a repeat later the same day passed it - see
  `../P-H0-repeat-2026-09-28/`, D25).
- **The `BOOT,...` line.** P-H0 Step 1 and `src/main.cpp` both expect it; it did
  not appear in any of the seven boots observed, while the `EVE1` boot record
  did. Cause not determined (D21 F-4; addressed and re-verified in D22, and
  changed again in D23 - see below).
- Anything about the vehicle. No motor supply was connected, and no driver,
  encoder, drivetrain, battery or sensor behaviour was exercised. Rows #1-#12 of
  `VERIFIED_STATE.md` are untouched.

## D22 follow-up (2026-09-28, same day): F-1 and F-4 re-tested on target

The five findings above were addressed in code (`DEVELOPMENT_LOG.md` D22), and two
of them were then re-tested on the same board, USB only, no motor supply:

- `d22-f1-upload-verify.log` - `pio run -t upload` with the hook: the touch, the
  bootloader appearing, and `dfu-util` flashing 66612 bytes to `Download done.` /
  `Done!`. Three uploads in this session all entered the bootloader with **no
  manual touch** (this is the last of the three). F-1's premise is now handled by
  the repository rather than by the operator.
- `d22-f4-attach-observations.txt` + `d22-f4-attach3.txt` - three late-attach
  captures on a board that had booted with nobody reading. Two were greeted with
  `BOOT,...` as the first line; **one was not**, because no attach transition was
  observed. So the boot line is verified for the first host that attaches, and a
  greeting per attach remains unproven. All three are recorded, including the one
  that did not behave as intended - nothing here was discarded. Note (D23): these
  observations belong to the **D22** image; the greeting was changed again the
  same day (below), so they are not a statement about the current image.

The board was left running the vehicle image (`fw=ev-nano-r4-p7b`) in application
mode on `COM3`. At this point H0 was still **not passed**: its decision rule
blocks on upload reliability over a pre-registered count, and this follow-up was
three uploads, not the pre-registered series. (Superseded later the same day: the
pre-registered repeat in `../P-H0-repeat-2026-09-28/` met every criterion and H0
passes - D25. The statements in this file are unchanged, and they belong to the
images named here.)

## D23 follow-up (2026-09-28, same day): F-4's residual, in code only

D22 re-armed the greeting on an attach *transition* and recorded honestly that one
of the three attaches above showed no transition: the presence flag stayed true,
so nothing was re-armed and no greeting was sent. D23 removes the dependence on
observing a detach. `serialTransport::hostLineState()` now reports **both** control
lines - DTR through the core's own `connected()` (still the exact predicate
`Serial.write()` gates on), RTS through the side-effect-free `Serial.rts()` - and
the greeting decision moved into the pure, host-tested `ev::HostGreetPolicy`
(`lib/evcore/src/HostGreetPolicy.h`). A greeting is owed when either control line
newly goes high, when any byte arrives from the host (which also restarts the
interval), and after `kSilenceMs = 10000` of host silence, repeating at that
cadence while the silence continues. A new `boot` command re-sends the same line
on demand, through the same formatter.

**No board was touched for this change**, so nothing here is an on-target
observation and the captures above are unchanged:

- `pio run` succeeded - 5972 B RAM / 66836 B flash, no warnings - and so did
  `pio run -e bench` and `-e bench-watchdog`; `pio test -e native` reported
  **517/517**, the D22 baseline's 510 plus 7 cases in `test_host_greet_policy`.
- Image for this source:
  `sha256 .pio/build/nano_r4/firmware.bin` =
  `cfd288d455c2cf6c4d15f4b236827ee3e217f40ffe623b71ef301c265b38b788`
  (two clean rebuilds of the final source agree).
- **Why the existing captures cannot speak to the beacon:** they ran 4-6 s, which
  is shorter than `kSilenceMs`. The next session's captures must run longer than
  10 s, and a repeated `BOOT,...` mid-capture (the beacon) must be recorded
  separately from a `BOOT,...` first line (the attach edge).
- The residual limit is unchanged and now has a number attached to it: a host that
  presents no control-line edge **and** never writes cannot be told apart from the
  previous reader, and the silence rule bounds that case rather than removing it.

### Correction (D24, same day) - one sentence above said more than the code does

The D23 paragraph above says a greeting is owed "when any byte arrives from the
host". It is not: `ev::HostGreetPolicy::observeHostByte()` **restarts the silence
interval** and does not owe a greeting, which is what
`test_a_host_byte_restarts_the_silence_interval` pins (and the same wording was
corrected in `HostGreetPolicy.h`'s header comment and in the `VERIFIED_STATE.md`
boot-line row). The record is left as written and corrected here rather than
rewritten. What the correction adds is a limitation, recorded in
`docs/limitations.md`: a reader that presents no control-line edge **and** types
continuously pushes the silence deadline out by `kSilenceMs` per byte, so it may
see no `BOOT,...` line for as long as it types; stopping for ten seconds, or
typing `boot`, is how the greeting is obtained. That behaviour has not been
observed on a board and is on the H0 repeat's list.
