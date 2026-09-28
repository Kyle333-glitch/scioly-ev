# Pre-registration records (committed)

Every protocol in `docs/bringup/` asks for a criterion to be **pre-registered**
before the first trial: the spread within which repeated trials count as
agreeing, the soak duration, the acceptance band, the number of repetitions. A
criterion invented after seeing the data is not a criterion, and a criterion that
lives only in someone's head cannot be shown to have existed first.

This directory is where those criteria are written down, before the session, in a
form that is committed and timestamped by the commit that adds it.

## One file per session or protocol

```
data/prereg/P-H4-2026-10-03.md
```

```markdown
# Pre-registration - P-H4 rollout

- protocol: P-H4
- planned session date: 2026-10-03
- operator:
- runner/vehicle build: git rev-parse HEAD = <sha>  (record after flashing)

## Criteria fixed before the first trial

| Item | Value | Where it comes from |
|---|---|---|
| repeatability spread for D/N | `<= 3 mm` between trials | smallest disagreement that would change the chosen scale |
| repetitions per direction | 5 | enough to see one outlier; not a statistical claim |
| smallest effect worth detecting | 10 mm | the accuracy the event target needs |
| excluded-trial rule | skid or course re-measure only | decided before running |

## Why these numbers

One or two sentences: what decision the criterion drives, and what effect size
makes the extra repeats worth it (`AGENTS.md` section 28 - the smallest test that
answers the question).

## Deviations

Filled in during the session, if any. A deviation is a recorded fact, not a
failure; changing a criterion after seeing data is a failure.
```

## Relationship to the rest of the record

- the criteria here are referenced from `DEVELOPMENT_LOG.md` and from the
  session README in `data/evidence/`;
- the numbers a protocol asks for at "repetitions" come from here, so the count
  is derived from the criterion and not chosen for convenience;
- these files are not evidence about the vehicle: they are the plan the evidence
  is judged against.
