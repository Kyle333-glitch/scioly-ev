# P-H0 - Nano R4 upload, boot and serial link

**Primary class:** S (on-target software check).
**Also contains:** E observations (driver levels only - the motor supply is off).
**Order:** first. Every other protocol is blocked by this one.
**Answers:** does this image upload and reboot reliably; does the operator
interface behave over a real host; what is the observed control cadence; how does
a real host draining (or not draining) the port affect the loop.
**Bench instrumentation required:** B1 (`bench health` in the bench image,
`pio run -e bench -t upload`) for the cadence step and for the
boot-to-first-refresh gap; the rest needs nothing beyond the shipped interface,
and the `BOOT,...` line reports which image and configuration a boot is running.

**This protocol does not establish** any electrical or physical fact about the
vehicle. It also does not establish motor-output safety on a reset - that is
H1A. There is no motor supply connected here.

## Prerequisites

- `pio run` succeeds on the exact tree being flashed. Record `git rev-parse HEAD`
  and the `VERSION,fw=...` line the board reports.
- The motor supply is **off and disconnected**. Wheels off the floor. Driver
  outputs unloaded.
- A serial capture path that writes to a file under `data/raw/`.
- (Pre-register) the counts that define "reliable": how many consecutive uploads
  and how many consecutive boot cycles must succeed, and the soak duration.

## Setup

Connect only USB. `pio run -t upload` (and `pio run -e bench -t upload`) perform
the 1200-bps touch themselves: `scripts/nano_upload_touch.py` is a pre-upload
action in `platformio.ini`, because this platform's own DFU uploader does not
touch the port (D21 F-1). A manual touch is no longer part of the procedure - but
a witnessed upload whose hook report says the bootloader was never found is
itself a finding to record, not something to work around silently. If the hook
cannot touch at all, record that and touch by hand, and say so in the log.

Open the port at 115200 (`pio device monitor` with logging to a file). Leave
telemetry off for the boot and refusal steps; turn it on only for the cadence and
host-load steps.

## Held constant

Firmware image hash, host machine, USB cable and port, baud, telemetry state for
each step, operator.

## Varied

- reset method: upload-triggered reset, power cycle, and (once) a repeat upload
  with no change;
- telemetry state: off, default rate, then the highest admissible rate;
- host behaviour: reading the port, and deliberately not reading it.

## Measurements and logs

1. **Upload and boot.** Run the pre-registered number of uploads and power
   cycles. Record the upload output, the count of successes, the time from reset
   to the first `BOOT,...` line, and whether that line is the first line a host
   sees when it attaches - including a host that attaches *well after* boot, which
   is the case that failed on the first session (D21 F-4). The boot line is owed
   to a host that is actually reading, so the rules to check are, in D23's terms:
   the greeting is owed to the first host that attaches (D22 verified this on
   target, twice in three attaches), it is re-owed when **either** control line
   newly goes high, and it is re-owed again after `ev::HostGreetPolicy::kSilenceMs`
   (10 s) of host silence, repeating at that cadence while the silence continues.
   Because of the last rule, **each capture must run for longer than 10 s**: a
   capture whose first line is `BOOT,...` shows the attach edge and not the
   beacon, so record the two cases separately - a repeated `BOOT,...` appearing
   mid-capture is the beacon, and its absence in a long-enough capture is a
   finding. Record what each attach actually produced rather than what is
   expected of it. Record `BOOT,...` verbatim: it reports the firmware
   id, the cadence, whether watchdog enablement is requested, whether every
   configuration check passed, whether the regulator enabled and whether the
   vehicle is armed - the facts that say which image and which configuration this
   boot is running.
2. **Inspection, verbatim.** Record the exact bytes of `version`, `status`,
   `checks`, `boot`, `config show`, `plan`, `help` - `boot` (D23) re-sends the boot
   line on demand and its answer *is* that line, so it must match the greeting
   verbatim, and `plan` (D46/S9) reports the run layer's current execution plan, or
   the plan module's own named reason there is none (with the shipped configuration
   it is a named refusal, and on the assembled vehicle it cannot be anything else).
   Then record `announce` and `arm`: with the
   shipped provisional configuration both must be refused, and the refusal must
   name the first failing check. **Do not assume which check** - record the one
   named. A refusal naming a check that is clearly not the first failure is a
   finding to diagnose, not to relabel.
3. **Configuration and rule validation.** `config set` with a level, distance,
   time and bottle line; echo with `config show` and `config check`. Then a
   malformed field and an out-of-range field: each must be refused with a named
   error, never silently clamped, and - for the out-of-range field - `config show`
   afterwards must still report the *previous* configuration, because a refused
   value is now refused outright rather than held (D21 F-5, fixed in D22). Record
   each error name verbatim, including the new
   `ERR,cmd=config set,reason=ValueOutOfRange,error=<rule error>`.
4. **Malformed input never reaches the chain.** Send junk, an over-long line, too
   many tokens, a huge number and a half-finished command. Each produces a named
   error; no `OK` is produced for a command that was not applied; `status` still
   reports `phase=Idle`.
5. **Telemetry shape and rate.** Confirm `EVS3` samples and `EVE3` events appear (the
   current schema is version 3, `ev::kTelemetrySchemaVersion`; the H0/H0W-era
   captures record the then-current `EVS1`/`EVE1`, and a sample now also carries the
   run's own timeline fields);
   `telemetry off` stops the samples; `telemetry rate <hz>` changes the observed
   spacing to a whole number of ticks; the drop counters stay at zero while the
   host keeps up.
6. **Host stops reading.** With telemetry on, stop reading the port (close the
   monitor) and leave the board running. Record whether the sample cadence
   (`dt_ms` in the last samples before) is unchanged, whether the drop counters
   rise, and whether reconnecting produces a burst of stale lines.
7. **Cadence.** With telemetry at its default rate, capture a soak and read the
   `dt_ms` distribution. This step and Step 6 are the two the watchdog timeout
   choice leans on, so they are not optional: B1 (`bench health` in the bench
   image, `pio run -e bench -t upload`) reports the pass/refresh/withheld counts,
   the measured boot-to-first-refresh gap, the last tick's `late_ms`/`dropped`,
   and - since D22 - the session's own extremes `worst_late_ms` and
   `dropped_total`. Read the extremes, and read them twice: two readings of
   `dropped_total` that differ mean the loop is still losing deadlines, which a
   single spot value cannot show. Repeat at the highest admissible rate, and
   record the worst case across both - never the average.

## Repetitions

Pre-registered uploads and power cycles (suggest at least 10 each); the refusal
and malformed-input steps repeated at least 3 times; the host-load step twice;
the cadence soak long enough that the pre-registered number of control ticks has
elapsed.

## Safe-stop (abort) conditions

The board cannot command motion here. Abort if the motor supply is found
connected, if anything moves, or if the driver levels are not the de-energized
combination at rest. To stop: remove USB; confirm the motor supply is off.

## Acceptance / decision rule

Pass requires all of: the pre-registered upload/reboot counts met, with each
upload entering the bootloader without a manual touch; the boot line being the
first line a newly attached host sees, including a host that attaches after boot,
**and** - in a capture longer than `ev::HostGreetPolicy::kSilenceMs` - a repeated
`BOOT,...` line from the silence rule, with `boot` answered by that same line
verbatim (D23); a host attaching when the firmware already believed one was
attached is recorded as an observation and counted neither way, because no
host-side signal distinguishes it;
every documented command answered in the documented shape; every malformed input
named and rejected with no state change, including an out-of-range value leaving
the held configuration untouched; cadence showing no dropped deadlines and no
unexplained lateness at the chosen telemetry rate, with `dropped_total` and
`worst_late_ms` reporting the extremes. If any upload fails, or the board reboots
without a boot cause, do not proceed - that is a defect in the boot path or the
host link.

## If it passes

- Add on-target rows for the upload/boot reliability, the host-drain behaviour of
  the link, and the observed cadence and lateness (none of these has a row yet).
  The cadence numbers are per image: a deployment measure from one of these steps
  means nothing unless the capture says which image produced it (`BOOT,...` has no
  `image` field - record the upload command - while `bench health` reports
  `image=bench`).
- Record which check the `arm` refusal names, verbatim.
- Log in `DEVELOPMENT_LOG.md`. No numbered hardware row (#1-#12) changes.

## If it fails or is inconclusive

First suspects, in the escalation order of `AGENTS.md` section 31: the bounded
boot wait in `setup()`, the transport's `availableForWrite()` gating, and port
enumeration or cable/port choice - then a firmware defect. Do not proceed to H0W
or any powered protocol until H0 passes.

If the board is left on the bus but not enumerating (D21 F-3: no COM port, no
`VID_2341` device, `Unknown USB Device (Device Descriptor Request Failed)`), the
pre-upload hook prints its own classification of that state before `dfu-util`
errors out - record that output verbatim, then power cycle the board. The cause is
still undetermined; a second occurrence with the same cable and port is what would
turn it from an anomaly into a diagnosis, and it is worth a deliberate retry of
the identical command afterwards to see whether the failure is sticky.
