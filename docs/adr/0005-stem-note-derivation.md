# Derive notes per stem without changing the source clock

Status: accepted.

The stem-note workflow reads one complete instrument-stem bundle, prepares
each stem for note inference, and writes one new event bundle. Its command is:

```sh
hlolli-wg-analyzer infer-stem-note-events STEMS.hwa-events \
  --model BASIC_PITCH.onnx --output NEW.hwa-events
```

The workflow keeps the source recording and stem WAVE files on their original
44100 Hz clock. For each stem it runs
`org.hlolli.note-events-on-audio-v1`. That provider uses transform
`stereo-average-blackman-sinc-127-decimate-2-v1` to make temporary mono 22050
Hz audio, runs Basic Pitch, and maps each returned half-open bound back by an
exact factor of two. It does not save the temporary audio. The workflow then
copies those 44100 Hz bounds unchanged to the shared source clock.

Each note names the stem as its evidence audio and the stem's full-span
`instrument-region` as its parent. The output keeps one
`org.hlolli.basic-pitch-audio-provider` provider row for each stem run. This
keeps the raw input, prepared input hash, transform, nested model settings,
runtime facts, and note values tied to the run that made them; see
[Stem note events version 1](../stem-note-events-v1.md).

We rejected one note-provider run on the source mix because it cannot state
which stem supplied the evidence. We rejected one shared provider row for all
stems because one row would hide the separate model inputs and runs. We also
rejected saving the 22050 Hz work file as derived audio because version 1 of
the event bundle requires derived audio to keep the source sample rate and
frame count.

This step adds evidence links, not accuracy claims. Instrument labels and
notes remain inferences. Stem bleed can duplicate notes or cause false
pitched events, and HTDemucs's six broad classes cannot separate an orchestra
into one source per instrument. Exact hashes, sample indexes, and mappings
make the result checkable without presenting those model guesses as measured
physical facts.
