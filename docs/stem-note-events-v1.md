# Stem note events version 1

This document defines derived-bundle workflow
`org.hlolli.stem-note-events-v1`. It runs note inference on each instrument
stem while keeping all saved event bounds on the original recording's clock.
The command is:

```sh
hlolli-wg-analyzer infer-stem-note-events STEMS.hwa-events \
  --model BASIC_PITCH.onnx --output NEW.hwa-events
```

`STEMS.hwa-events` is a directory bundle made by the
`org.hlolli.instrument-stems-v1` task. `NEW.hwa-events` must not exist. The
command copies the checked stem payloads into the new directory bundle and
adds note events. It does not change the input bundle.

The workflow runs raw-audio task `org.hlolli.note-events-on-audio-v1` once
for each stem. The first native adapter uses provider
`org.hlolli.basic-pitch-audio-provider`, version `1`, and a caller-supplied
Basic Pitch ONNX model. This provider owns the fixed audio preparation and
wraps the Basic Pitch task defined in
[Polyphonic note events version 1](polyphonic-note-events-v1.md).

## Input bundle

The input must be one complete `org.hlolli.instrument-stems-v1` result. It
has one external source recording, one or more bundled instrument stems, and
one full-span `instrument-region` event for each stem. Each region's evidence
audio names its stem. The command rejects a bundle with derived audio, note
events, traces, warnings, unmatched stems or regions, or other rows outside
that task's result shape.

The source recording and every stem use a 44100 Hz clock and have the same
positive frame count. With the first native adapter, each stem must be a
stereo float32 WAVE file. The reader checks every payload path, byte size,
SHA-256, WAVE header, and source link. The raw-audio provider checks decoded
samples before it sends that stem to Basic Pitch. The command rejects a
missing, extra, changed, or non-finite stem.

The command does not need to open the external source recording. Its audio
row stays external and unchanged. It reads the bundled stems named by the
manifest. It processes them in ascending stem-ID order, not directory order.

The pinned HTDemucs adapter currently makes six stereo stems: `drums`,
`bass`, `other`, `vocals`, `guitar`, and `piano`. These are broad model
classes. The workflow also accepts another conforming instrument-stem result
whose stems match the chosen note provider. The first native provider still
requires stereo float32 44100 Hz WAVE files; it does not require those six
stem names.

## Fixed audio preparation

The raw-audio provider accepts a stereo float32 stem at 44100 Hz. Basic Pitch
takes mono audio at 22050 Hz, so the provider makes one temporary mono float32
WAVE file by this fixed process:

1. Decode samples in source-frame order. A stereo frame becomes the
   arithmetic mean of its left and right samples. The command does not
   normalize, clamp, or change gain.
2. Apply transform
   `stereo-average-blackman-sinc-127-decimate-2-v1`. Its resampler uses 127
   Blackman-windowed sinc taps, a cutoff of 0.225 cycles per 44100 Hz input
   sample, zero extension at both ends, and a coefficient sum normalized to
   one.
3. Keep output sample `m` centred on input sample `2 * m`. For `N` source
   frames, write exactly `ceil(N / 2)` mono frames at 22050 Hz.

The fixed coefficient table and summation order are part of transform version
1. The adapter source hash pins their exact implementation. A runtime must
not replace this process with a platform resampler under the same ID.

The provider hashes the exact stem WAVE and temporary WAVE bytes. Its
canonical settings retain the raw input facts, transform ID, prepared audio
hash and byte size, and the full nested settings from the Basic Pitch child
provider. The wrapper row also retains the raw-audio task name, model
identity, and runtime facts. It returns one wrapper provider row even when it
finds no notes.

The 22050 Hz WAVE file exists only as bounded work input. The output bundle
does not retain it as audio or as a payload. Version 1 of the event-bundle
schema requires every derived audio row to share its source's sample rate and
frame count, so saving this work file as derived audio would break the source
clock rule.

## Note mapping and links

Suppose the Basic Pitch child returns a half-open range `[a, b)` on the
temporary 22050 Hz clock. Let `N` be the stem's 44100 Hz frame count. The
raw-audio provider maps it with checked integer arithmetic:

```text
start_sample = 2 * a
end_sample   = min(N, 2 * b)
```

It rejects an overflow and drops no valid non-empty provider event. The
raw-audio result now uses the stem's 44100 Hz clock. The stem-note workflow
copies those integer bounds unchanged because sample `n` in a stem already
means sample `n` in the original source. Neither step converts the bounds
through seconds, beats, or a rounded decimal value.

Each new event has these fields:

| Field | Value |
| --- | --- |
| `kind` | `note` |
| `source_recording_id` | The original source recording. |
| `evidence_audio_id` | The stem analysed for this provider run. |
| `parent_id` | That stem's full-span `instrument-region` event. |
| `start_sample`, `end_sample` | The mapped 44100 Hz half-open range. |
| `voice`, `part`, `score_event_id` | Empty strings. |
| `trace_refs` | Empty. |

The parent link states which estimated stem supplied the note evidence. The
parent region keeps the stem ID in its `part` field and its inferred class in
the `instrument` value. The child leaves `part` empty because a stem class is
not a score part or a distinct player.

The workflow copies the raw note event's named values and changes their
provider reference to the wrapper provider row saved for that stem. The
native Basic Pitch child returns one selected `pitch-hz` value for each note.
The value uses `inference` basis, a finite positive binary64 value in `Hz`,
and a method score in `[0, 1]`. The bundle writer saves the binary64 value
with round-trip precision; it does not round it for display.

The `pitch-hz` value is the equal-tempered centre of a decoded pitch class.
It is not a direct frequency measurement, tuning curve, pitch bend, or proof
of the sounding source. The event bounds are exact saved indexes derived
from a model result. They do not claim a sample-perfect acoustic onset or
release.

## Output shape and order

The output preserves the input bundle's source row, stem rows, stem payload
bytes, stem provider rows, region events, named values, and IDs. It adds one
raw-audio note provider row per stem and zero or more note events. It adds no
audio, traces, trace references, or warnings.

New provider IDs are the lowest unused positive IDs, assigned in stem-ID
order. New event IDs are the lowest unused positive IDs. The workflow keeps
each provider's event order and appends one stem's events before the next
stem's events. It updates each copied value's provider ID and each note's
evidence and parent links after assigning IDs.

Given the same provider results, the fixed merge produces the same IDs, links,
row order, and payload bytes. The saved provider facts state the runtime and
backend. The contract does not promise equal model output across runs,
runtimes, or hardware.

Zero notes on one or every stem is a valid result. The output still includes
one raw-audio note provider row for each stem. Note spans may overlap within
one stem and across stems. The workflow does not merge, remove, or rank notes
across stems.

## Limits and failure

Let the input have `P` providers, `A` audio rows, `E` events, `V` values, and
`S` stems. Let all provider runs return `Q` notes. With the native Basic Pitch
adapter, each new note has one value. A successful result therefore has:

```text
providers = P + S
audio     = A
events    = E + Q
values    = V + Q
```

These totals must fit the caller's event-bundle limits and the JSON-safe ID
range. `--max-note-events` caps `Q` across the whole workflow, not once per
stem. A cap never asks the workflow to truncate a valid provider result. If
the whole result does not fit, the command fails.

The common model, input, payload, row, string, work, saved-bundle, and nesting
limits apply. The temporary WAVE for one stem has
`4 * ceil(N / 2)` data bytes plus its header. The workflow handles stems one
at a time and closes each temporary file before it starts the next stem.
Saving the result also needs space for a full copy of the input stem payloads.
ONNX Runtime's model, session, graph, and internal run buffers sit outside
`max-work-bytes`.

One cooperative workflow deadline covers input checks, preparation, all
provider runs, and the in-memory merge. Each provider run receives only the
time left. The deadline starts after the command opens and checks the model.
It cannot stop one file, hash, or model call while that call runs, but it
checks the deadline before it starts the next phase or stem. Saving the final
bundle uses normal bounded file I/O and is not part of this deadline.

One bad stem, one provider error, one limit, or one deadline failure stops the
whole workflow before saving. A successful command writes a complete new
directory bundle. The writer removes paths it made after a handled write or
check failure. A stopped process or storage failure can leave a partial new
directory. The workflow never reports a partial set of stems or notes as a
valid result.

## Meaning and limits

The bundle separates checked facts from model guesses:

- WAVE format, file hashes, byte sizes, source-frame indexes, and link targets
  are checked facts.
- Instrument labels, note spans, pitch classes, and method scores are model
  inferences. Their values use `inference` basis, never `observation`.
- A score is a method score. It is not a calibrated chance that the claim is
  true unless a later provider states and tests that meaning.

Running note inference per stem gives downstream tools a clear evidence link.
It does not make the stem clean or the note correct. Bleed can put the same
note in several stems. Drums, room sound, coughs, and model artifacts can
produce pitched false positives. The workflow makes no cross-stem correction.

HTDemucs's six labels do not split an orchestra into one stem per instrument.
Strings, winds, brass, and other sources can share `other` or bleed into
another class. The command does not identify each player, remove all room or
microphone sound, or promise orchestra-level source separation.

Version 1 does not infer rhythm, tempo, meter, key, voices, score parts,
chords, trills, ornaments, dynamics, bow force, physical causes, or score
links. It does not write MIDI, Csound, LilyPond, or MusicXML. A later tool may
derive those forms from the preserved note, stem, source-clock, and provider
facts and may lose detail as part of that conversion.
