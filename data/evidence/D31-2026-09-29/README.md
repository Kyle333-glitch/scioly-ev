# D31 - 2026-09-29 - the last on-target session

This directory holds the only capture from the last session in which the board was
reachable. **The board is now gone**, so this session cannot be repeated, extended
or corrected - what is not in here did not happen.

It is **ad-hoc and not pre-registered.** It belongs to no `docs/bringup/`
protocol, it closes no step, and it promotes no row on its own. It exists because
the D31 work was about to change the source, and a baseline reading of the board
was worth taking before that: it turned out to be the last chance to take one.

## Captures

| File | What it is |
|------|-----------|
| `d31-prechange-vehicle.log` | 13.0 s of the **unmodified vehicle image** streaming `EVS1` telemetry, captured immediately *before* the D31 source changes. Host time on the left, whole line on the right. 652 lines, 651 of them `EVS1`. |

The capture is the raw serial text with a host timestamp prefixed. Those
timestamps are the host's, not the MCU's, and they are only good to the serial
read window (a few hundred microseconds per line at 50 Hz); every quantity that
matters is read from the line's own fields.

## What it shows

Taken from the capture itself, not inferred:

- **The vehicle image was up and healthy and doing nothing.** `t_ms` runs
  `1521430 -> 1534430` continuously (13000 ms of samples over a 13.0 s capture,
  so no reset and no gap), `dt_ms=20` on every sample, `phase=Idle`, `armed=0`,
  `cmd=none`, `motor=Disabled`, `effort_permille=0` on all 651 samples. The board
  had been up **1521 s (25.4 min)** at the first sample.
- **`pos_ok=0` and `vel_ok=0` on every sample**, which is the configured and
  expected state: the distance scale is unmeasured, so the estimate is refused
  rather than reported as a number.
- **A `BOOT,...` line arrived mid-stream and was not a restart.** It is line 486,
  host time 9.699 s, between two `EVS1` samples whose `t_ms` are 1531110 and
  1531130 - continuous, exactly 20 ms apart. There were **zero `EVE1` lines** in
  the whole capture and no reset. This is the same fact D30 established from the
  other direction (`BOOT` is re-sent as the silence beacon every
  `kSilenceMs = 10000`, so in a bench or vehicle image it is **not** evidence that
  the firmware restarted; only `EVE1,...,event=Boot` is). This session reached the
  board with a still-running image and saw the beacon alone, which is what the
  D30 caveat predicts.
- **`batt_counts` wanders 588-594** with a drifting, not a regulated, value. No
  battery, driver, motor or encoder was connected at any point in this project's
  on-target sessions, so this is a **floating ADC input**. It is not a battery
  reading and must not be recorded as one.
- The `BOOT` line's own fields are the D30 source's: `fw=ev-nano-r4-p7b`,
  `schema=1`, `cadence_ms=20`, `watchdog=0`, `checks_all=0`, `regulator=0`,
  `armed=0`, `telemetry=1`, `stride=1`, `ready=1`.

The full line is 304 characters, the widest `EVS1` line in the capture, so the
capture is also a size sample for the telemetry consumer.

## What it does NOT show, and why that matters

- **Nothing about the D31 source.** The capture is the image *before* the changes.
  The D31 changes were built and hashed (`DEVELOPMENT_LOG.md` D31) but the changed
  **vehicle** image was never flashed, so it has no on-target observation at all.
- **Nothing about the bench image that was flashed afterwards.** `pio run -e bench
  -t upload` completed (`nano_touch` -> `Download done.` -> `Done!`, 72924 bytes),
  and then the capture of that image's readouts **failed**: `pyserial` raised
  `ClearCommError failed (PermissionError(13, 'The device does not recognize the
  command.'))` on the first read, and the device was gone from that point on. So
  the bench image's readouts after the D31 change - including `bench health` - were
  **never observed**, and no claim may be attached to them. In particular, D31's
  `stageLostReadoutNotice()` path is **built and not observed on hardware**.
- **Nothing about whether the changed vehicle image boots.** It was never flashed.
- **Nothing about the vehicle.** No motor supply, no driver, no encoder, no
  drivetrain was connected, exactly as in every other session. `armed=0` and
  `motor=Disabled` are what the interface *reported*; they are not a measurement of
  the hardware.

## Image identity

| Image | `sha256` of `firmware.bin` | Size | On target? |
|-------|---------------------------|------|-----------|
| vehicle, pre-D31 (`eb440b31...`) | `eb440b319a8a09aeb4ee82f0aa37f24a80f3f4a011b49d14639d895bc5d88d50` | 67020 B | **yes** - this capture. Left over from D30; nothing was flashed between D30 and this capture, so this is what produced it |
| vehicle, at D31 HEAD (`91f74a04...`) | `91f74a042f97b8af63b9c6e5d1f2fd8606a134e7e4ecd1ec83b6090051254b52` | 67020 B | **no** |
| bench, at D31 HEAD (`b6b1c455...`) | `b6b1c4551257baddfc1b3a1d3faa1c47f97aeef6b48c1ed242a43188d46aa022` | 72924 B | **flashed, not observed** - see above |
| bench-watchdog, at D31 HEAD (`271e8eba...`) | `271e8ebaf82c7e483ad0ee2e793a38c3395649bc5cd9ff626ca30a99029b2fa5` | 73252 B | **no** |

The first row is an inference from the record, not a measurement: the board was
left running `eb440b31...` at the end of D30, no upload occurred between then and
this capture, and the capture's 25.4 min of uptime is consistent with that. The
firmware cannot report its own hash.

## State the board was left in

**The board holds the bench image (`b6b1c455...`), not the vehicle image.** That
is a change from every previous handoff, which left the vehicle image running, and
it is recorded here because it is safety-relevant: the bench image contains
`bench drive`, a command that applies effort outside a run, and it is the image
whose own documentation says it must never be mistaken for the vehicle image
(`platformio.ini`). The board is no longer reachable, so it cannot be reflashed by
this project. If the unit is ever powered again, read `bench health` - it reports
`image=bench` - before treating it as the vehicle.

The hook `kDeliberateHangAfterPasses` was `0U` in the source at every point in
this session (verified before the build), so the flashed bench image does not hang.

## Reproducing the capture

The capture is committed as text. Re-running it is not possible: the board is
gone, and the capture is the record.
