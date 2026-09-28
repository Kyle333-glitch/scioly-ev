# Evidence captures (committed)

This directory is the committed home for the **raw observations** that back a
promotion in `VERIFIED_STATE.md`. It exists because a protocol's record has to
outlive the bench laptop: `data/raw/` is scratch and is gitignored, so a capture
that lives only there is not actually kept, and a fact whose evidence cannot be
re-read is a fact that cannot be checked.

`data/raw/` holds anything and may be deleted. This directory holds only what a
record points at.

## What goes here

For each session that promotes a fact, one directory:

```
data/evidence/<protocol>-<session>/
    README.md            what was run, when, by whom, and which facts it backs
    <capture>.log        the verbatim serial capture, exactly as saved
    measurements.csv     the hand measurements, same file the tooling reads
```

Copy the capture and the measurement CSV here **unchanged** - do not trim it,
reformat it, or paste a report in place of it. A `data/reports/` tree is derived
and reproducible; it is not the record and does not belong here.

The `README.md` in the session directory names the facts the evidence supports
(e.g. `VERIFIED_STATE.md` #6) and points at the `DEVELOPMENT_LOG.md` entry.

## What must not go here

- generated reports or plots (they are reproducible from the capture);
- editor scratch files, host paths, or anything machine-specific;
- anything that is not a measurement or a verbatim observation.

## Size

Captures here are text and are kept small on purpose: record at the protocol's
admissible rate for the shortest time that answers the question, and let the
analyzer summarize the rest. If a capture is genuinely large, say so in the
session README and record why it is kept in full.
