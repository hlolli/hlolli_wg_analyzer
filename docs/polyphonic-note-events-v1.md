# Polyphonic note events version 1

This document defines the first trained-model task. The task name is
`org.hlolli.polyphonic-note-events-v1`. An ONNX Runtime build implements it
with provider `org.hlolli.basic-pitch-onnx`, version `1`:

```sh
hlolli-wg-analyzer infer-note-events INPUT.wav \
  --model MODEL.onnx --output NEW.hwa-events
```

Version 1 accepts a mono 22050 Hz WAVE file and a caller-supplied ONNX model
that matches the official Basic Pitch tensor contract. It does not resample,
mix channels, download a model, or choose a model for the caller. The model
must hold its weights in the same file; external-data files are not accepted
or included in the model hash.

## Input and identity

One request has one `audio/wav` input with role `source-recording`. That WAVE
file supplies the only source clock. The current command accepts no score,
label, or other context input.

The caller pins the provider name, provider version, and SHA-256 of the exact
model bytes. The provider must reject any mismatch. A runtime fallback may
change the runtime or backend, but it must not change those three values.

## Output

The provider returns one valid `hwa-events` version 1 bundle. It has one
external `source-recording` audio row that matches the request, one provider
row, and zero or more `note` performance events. It has no derived audio or
instrument stems. Note events may overlap.

Every note has exact half-open bounds `[start_sample, end_sample)` on the
source clock. It has no parent and names the source recording as its evidence
audio. Its `voice`, `part`, and `score_event_id` are empty unless the provider
copies them from explicit, hashed context that the caller marked as trusted.
A model guess is not trusted context.

Every note has one selected `pitch-hz` value with these fields:

- kind `f64`, a finite value greater than zero, and unit `Hz`;
- basis `inference` and the task provider's ID;
- a method score in `[0, 1]`;
- `selected` set to true.

`pitch-hz` is the equal-tempered centre of the decoded MIDI note bin, with
A4 set to 440 Hz. It is a class label written in hertz for downstream score
tools. It is not a measured acoustic frequency, tuning offset, or pitch-bend
track.

A note may also have unselected `pitch-hz` candidates. Each candidate uses
the same kind, unit, basis, provider, and score rules. A note may have
`instrument` text candidates with an empty unit, inference basis, the task
provider's ID, and a score in `[0, 1]`. At most one instrument candidate may
be selected. Candidate array order has no meaning.

Zero notes is a valid result. Version 1 does not require traces. The adapter
returns note events only. It does not return pitch bends, separate sources,
write stems, infer rhythm, tempo, beats, meter, voices, parts, score links,
trills, or other ornaments.

The decoder uses these defaults:

| Setting | Command option | Default |
| --- | --- | ---: |
| Onset threshold | `--onset-threshold` | `0.5` |
| Frame threshold | `--frame-threshold` | `0.3` |
| Minimum note frames | `--minimum-note-frames` | `11` |
| Energy tolerance frames | `--energy-tolerance-frames` | `11` |

Inferred onsets and the Melodia pass are on in version 1. The saved task
settings record all six decoder choices. `--max-note-events` limits output;
it does not change the threshold settings.

The C Melodia pass follows Spotify's decoder but uses half-open spans. It
keeps the last active frame instead of subtracting one extra frame from the
exclusive end. This one-frame correction means its event ends can differ
from the stock Python decoder.

## Model-frame mapping

The decoder first expresses each note as a non-empty half-open range of
non-negative model-frame boundaries `[a, b)`. Model frame zero lines up with
source sample zero after the adapter removes window padding and cropped
frames.

The version 1 window schedule is fixed:

| Item | Value |
| --- | ---: |
| Left zero padding | 3840 samples |
| Model input | 43844 samples |
| Model output | 172 frames |
| Crop | 15 frames from each side |
| Kept output | 142 frames |
| Next-window step | 36352 samples |

The step is `142 * 256 = 36352` samples. This differs from the 36164-sample
step in Spotify's stock window code. The stock step and 142 kept frames do
not form one fixed 256-sample grid. Version 1 uses 36352 so each kept model
frame maps to exactly one source span of 256 samples. This is a stated adapter
choice, not a claim that the stock schedule behaves the same.

The decoder exposes `floor(N / 256)` frames. A final source tail shorter than
256 samples has no model frame of its own. Saved event ends still clip to `N`.

Let the source have `N` frames at sample rate `S`. Let the model frame rate be
the exact positive ratio `R_num / R_den` frames per second. The mapping rule
`model-frame-boundary-start-floor-end-ceil-clip-v1` is:

```text
start_sample = min(N, floor(a * S * R_den / R_num))
end_sample   = min(N, ceil (b * S * R_den / R_num))
```

The adapter uses checked integer arithmetic, not floating-point seconds. It
drops a result when clipping leaves `start_sample >= end_sample`. These saved
bounds are exact source indexes derived from model estimates; they do not
claim sample-perfect acoustic onset or release.

## Canonical provider settings

The saved provider `settings` object records the whole run in this key order:

```json
{"task":"org.hlolli.polyphonic-note-events-v1","seed":"00000000000000000000","inputs":[{"id":"source","role":"source-recording","media_type":"audio/wav","name":"take.wav","bytes":"00000000000000000000","sha256":"..."}],"runtime":{"name":"onnxruntime","version":"...","backend":"CPUExecutionProvider","fallback":"","adapter_sha256":"..."},"task_settings":{"thresholds":{"energy_tolerance_frames":11,"frame_threshold":0.29999999999999999,"minimum_note_frames":11,"onset_threshold":0.5},"decoder":{"infer_onsets":true,"melodia":true},"model_frame_rate":{"numerator":11025,"denominator":128},"mapping_rule":"model-frame-boundary-start-floor-end-ceil-clip-v1","window_schedule":{"crop_frames_each_side":15,"input_samples":43844,"kept_frames":142,"left_pad_samples":3840,"output_frames":172,"step_samples":36352}}}
```

The adapter writes this JSON without extra white space. It follows these
rules:

- `seed` is the request seed as exactly 20 decimal digits. It
  remains present when the method uses no random choice.
- `inputs` lists every request input, sorted by the UTF-8 bytes of `id`. Each
  row records its exact ID, role, media type, display name, lower-case
  SHA-256, and byte size. Byte size is exactly 20 decimal digits. Its keys
  stay in the order shown. Display names must use valid UTF-8.
- `runtime` records the runtime name, its exact version, the backend that
  produced the result, any stated fallback, and the adapter hash. An empty
  `fallback` means no fallback occurred. Its keys stay in the order shown.
- `task_settings.thresholds` records every threshold that can change decoded
  events. Its
  keys use ascending UTF-8 byte order. An adapter must not rely on an omitted
  default.
- `task_settings.model_frame_rate` stores the reduced positive integer ratio
  used by the mapping formula. Its keys stay in the order shown.
- `task_settings.mapping_rule` has the exact value shown above.
- `task_settings.decoder` and `task_settings.window_schedule` record the
  fixed version 1 decoder switches and window schedule.
- `task_settings` uses its shown key order and no extra white space. The
  provider must normalize it before it calls the shared provenance writer.

Provider name, provider version, and model hash stay in the provider row, not
in `settings`. The source audio row also retains the WAVE clock and source
hash. The repeated input entry makes all non-source context part of the saved
run record.

## Basic Pitch adapter

The adapter checks the Basic Pitch model input and output names, float types,
and shapes before it runs. It reads note and onset activations and converts
them to overlapping note events. It does not use contour output for pitch
bends in version 1.

| Role | Tensor name | Float32 shape |
| --- | --- | --- |
| Input audio | `serving_default_input_2:0` | `[1, 43844, 1]` |
| Note frames | `StatefulPartitionedCall:1` | `[1, 172, 88]` |
| Note onsets | `StatefulPartitionedCall:2` | `[1, 172, 88]` |
| Pitch contours | `StatefulPartitionedCall:0` | `[1, 172, 264]` |

The model declaration may use a dynamic batch dimension. Each adapter call
still uses batch size one. The adapter requires all four declarations even
though it reads only note frames and onsets in version 1.

The command hashes the exact source and model bytes. The provider row stores
the model hash. Its canonical settings store the source hash, byte count,
runtime version, CPU backend, adapter hash, thresholds, decoder switches, and
window schedule. `--expect-model-sha256 HEX` can pin the supplied model before
the runtime opens it. The build computes the adapter hash from the adapter,
decoder, clock, and provenance source files. The event score is the mean note
activation over its decoded span; it is not a calibrated probability.

The command applies one monotonic, cooperative deadline to checking, model
windows, decoding, and output checks. It checks between these phases; it
cannot stop a phase that is already running. After a late phase returns, the
provider rejects its result and starts no later phase or model window.
This deadline starts after the model file and ONNX session open. The model
byte cap applies during that earlier step, but the provider deadline does not.

Spotify documents Basic Pitch and its limits in the
[official project](https://github.com/spotify/basic-pitch). The fixed model
sizes and rates come from its
[constants](https://github.com/spotify/basic-pitch/blob/fa5997af0a8210982619003269994a1be25eddf3/basic_pitch/constants.py),
and the comparison schedule comes from its
[inference code](https://github.com/spotify/basic-pitch/blob/fa5997af0a8210982619003269994a1be25eddf3/basic_pitch/inference.py).
The tensor names come from its
[model loader](https://github.com/spotify/basic-pitch/blob/fa5997af0a8210982619003269994a1be25eddf3/basic_pitch/models.py).
Its note conversion is described by the
[official decoder source](https://github.com/spotify/basic-pitch/blob/fa5997af0a8210982619003269994a1be25eddf3/basic_pitch/note_creation.py).
The C decoder derives from that Apache-2.0 source. Its retained license and
notice are in `third_party/basic-pitch/`.
This adapter supports the task contract. It does not prove orchestra
transcription, instrument labels, source separation, or physical accuracy.
