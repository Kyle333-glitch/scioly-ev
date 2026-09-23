# Rules source

The firmware's per-run configuration is validated against the official event
rules. This file records where those values came from so a reader can check them.

## Source document

- File: `docs/rules/2027-div-c-electric-vehicle.pdf`
- Document: Science Olympiad **2027 Division C Rules** (the full Division C manual)
- Event: **Electric Vehicle C**, pages **C26-C31**

The PDF itself is a third-party copyrighted document. The team keeps its own
copy; it is not meant to be the project's copy of record. If the file is absent,
obtain it from the official Science Olympiad rules page or from the printed
rules manual, and check the revision date: the event rules can be corrected
mid-season.

## Values the firmware relies on

`lib/evcore/src/RunConfig.h` (`ev::rules2027`) is the authoritative copy of
these numbers in code; the table below is a convenience mirror. If the two ever
disagree, the code and the rules document win and this table is the bug.

| Value | Rule | Official limits |
|-------|------|-----------------|
| Target Distance | 8.h | 5.00-9.00 m from the Start Point |
| Target Distance interval | 8.h | 0.25 m Regional/Invitational, 0.10 m State, 0.01 m National |
| Target Time | 8.i | 10.0-20.0 s |
| Target Time interval | 8.i | 0.5 s at all tournaments |
| Bottle Line Distance | 8.j | 1.00-3.00 m beyond the Target Point |
| Bottle Line interval | 8.j | same interval as Target Distance |
| Bottle placement | 7.c.ii.(5) | 2.0 m Regional/Invitational, 3.0 m State, 4.0 m National from the Start Point |

Related rules that the firmware does not yet encode, recorded here so they are
not lost:

| Value | Rule | Official limits |
|-------|------|-----------------|
| Vehicle length | 3.d | at most 80.0 cm |
| Vehicle width | 3.e | at most 35.0 cm |
| Measurement Point height | 8.f.ii | at most 1.0 cm above the track |
| Start placement tolerance | 4.d.vii.(1) | MP within approximately 1 cm of the Start Point |
| Complete Stop | 8.p | stationary 3 or more seconds; not part of Run Time |
| Run Time | 8.o | from actuation to complete stop, recorded in seconds |
| Event Time | 2.c, 4.d.i.(1) | 8 minutes, at most 2 runs |
| Battery pack | 3.a, 8.b | at most 8 AA batteries, 1.2-1.5 V each, no lithium or lead |

## Rules status: UNVERIFIED against corrections

This extraction has not been checked against the current Rules Corrections or
Rules Clarifications pages, which take effect as if printed in the rules. Before
a competition build is frozen, re-check these values against the current
corrections and update `ev::rules2027` and this table together.
