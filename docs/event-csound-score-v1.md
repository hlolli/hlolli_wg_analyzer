# Event bundle to Csound score version 1

This adapter makes a plain Csound score from note events. It is a first proof
that one event bundle can drive another output form. It does not aim to copy
the source sound.

The adapter takes one source-recording ID. It selects events that:

- use that source clock;
- have `kind` set to `note`;
- have one selected `pitch-hz` value of kind `f64` and unit `Hz`;
- have a pitch greater than zero and below the source Nyquist frequency.

It sorts notes by start sample, end sample, then event ID. Each score row uses
this form:

```text
i 1 p2_seconds p3_seconds p4_pitch_hz p5_level p6_event_id p7_start_sample p8_end_sample
```

`p2` and `p3` come from the exact sample bounds and source sample rate. The
writer uses enough decimal digits to read each finite double back unchanged.
`p6`, `p7`, and `p8` keep the event ID and exact half-open sample bounds. `p5`
uses a fixed generic level.

The adapter skips a note that has no one usable selected `pitch-hz` value. It
fails when the source has no renderable notes.

This output loses instrument sound, playing style, dynamics, nested event
meaning, candidates, traces, and all values other than selected pitch. Those
facts stay in the event bundle.

The score-text tests check numeric event IDs, exact sample bounds, all sort
keys, and skipped notes. The integration test saves and reads a four-note
bundle, writes this score, renders it with a small generic Csound instrument,
and runs the existing score alignment check on the result. It checks note
order, time bounds within 100 ms, and matched coverage above 0.90. It does not
recover IDs from audio or prove sample-exact rendered edges. It also does not
test timbre, ornaments, source separation, or physical accuracy.
