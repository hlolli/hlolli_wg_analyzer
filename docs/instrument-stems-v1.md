# Instrument stems version 1

This document defines inference task `org.hlolli.instrument-stems-v1`. The
task separates one source recording into one or more estimated instrument
stems. It fixes the event-bundle result across models and runtimes.

The portable provider `org.hlolli.instrument-stem-provider`, version `1`,
implements the contract through an injected runner. The
`separate-instruments` command uses it with the pinned HTDemucs six-stem ONNX
adapter. The fake test runner and the model output make no accuracy claim.

## HTDemucs six-stem adapter

The first native runner accepts only one model artifact:

- file: `htdemucs_6s_fp16weights.onnx`;
- revision: `49df9b6989cf2150840ea65b0bef77a2e471b678`;
- byte size: `136428532`;
- SHA-256:
  `7ce55792e2231c93fbf92de95f5fd5b3a5e6c89f7db690dfd693e8f1dce56869`;
- input tensor: float32 `mix`, shape `[1, 2, 343980]`;
- output tensor: float32 `stems`, shape `[1, 6, 2, 343980]`.

The command accepts a mono or stereo 44100 Hz WAVE file. Each decoded sample
must be finite and in `[-1, 1]`. It copies mono into both model channels. It
writes six stereo float32 WAVE files in the model's fixed order: `drums`,
`bass`, `other`, `vocals`, `guitar`, and `piano`. The common provider sorts
those IDs before it assigns event and audio IDs.

The adapter follows the model publisher's fixed schedule. Each window has
343980 samples. Adjacent windows overlap by 85995 samples, so the stride is
257985. The last window gets zero padding on the right. A linear fade covers
each overlap, and the adapter divides the weighted sum by the matching weight
sum before it trims the result to the exact source frame count. The
publisher's fade starts at zero, so output sample zero is zero. This rule is
part of adapter version 1.

The six labels are broad source classes, not open-ended instrument detection.
Strings, winds, brass, speech, noise, room sound, and any source outside the
five named instrument classes may remain in `other` or bleed into another
stem. The upstream project calls the piano stem experimental and warns that
it may contain bleed and artifacts. The command cannot split an orchestra
into one stem per played instrument, remove all room sound, or prove that a
stem contains only its label.

## Request

The request has exactly one input. Its role is `source-recording`, its media
type is `audio/wav`, and `source_input_id` names it. No score, label, or other
context input is allowed. The input must be a valid WAVE file with at least
one sample frame. It supplies the sole source clock.

The caller chooses `source_recording_id` and pins provider
`org.hlolli.instrument-stem-provider`, version `1`, through the common
[inference provider](inference-provider-v1.md) request. It also pins the model
SHA-256 passed when the provider starts. A runner that uses a model has a
non-empty hash of the exact model bytes. A runner with no model uses an empty
hash. The task does not pick a model.

The task settings accept only an empty JSON object. JSON white space outside
strings may vary, but removing it must leave `{}`. Arrays, keys, duplicate
keys, and trailing data are errors. The provider records the normal form
through `hwa_inference_provenance_settings_build`, with `task_settings` set to
`{}`. Its saved settings name the task, seed, source input, runtime, backend,
fallback, and adapter hash.

The fixed seed remains part of the request and saved settings. An adapter that
uses random choices must derive all of them from that seed. Output order must
not depend on worker completion order, map order, directory order, or locale.

## Output shape

A successful provider returns one complete `hwa-events` version 1 bundle. It
contains exactly:

- one provider row with ID `1`, name
  `org.hlolli.instrument-stem-provider`, version `1`, and the pinned model
  hash;
- one external `source-recording` audio row;
- one or more bundled `instrument-stem` audio rows;
- one `instrument-region` event for each stem;
- one named value for each event;
- one payload binding for each stem;
- no other audio, events, values, traces, payloads, or warnings.

The source row keeps the request's `source_recording_id`. Its name, SHA-256,
byte size, and full WAVE format equal the selected input. Its `relative_path`
and `path_hint` are empty, and it has no payload binding. The provider does not
copy or change the source recording.

Each stem row has kind `instrument-stem`, points straight to the source row,
and names a bundled WAVE file. Its `path_hint` is empty. The row's format comes
from parsing the exact payload bytes. The row and payload binding state the
same relative path, byte size, and lower-case SHA-256.

## Instrument-region event

Each stem has exactly one event with kind `instrument-region`. The event
links a label to the stem; it does not claim that the instrument sounds in
every sample. Its fields are:

| Field | Value |
| --- | --- |
| `source_recording_id` | The request's source recording ID. |
| `evidence_audio_id` | This stem's audio ID. |
| `parent_id` | Null. |
| `start_sample` | `0`. |
| `end_sample` | The source frame count. |
| `voice`, `score_event_id` | Empty strings. |
| `part` | This stem's `stem_id`. |
| `trace_refs` | Empty. |

The event has exactly one value:

| Name | Kind | Value | Unit | Basis |
| --- | --- | --- | --- | --- |
| `instrument` | `text` | Provider class label | empty | `inference` |

The value names provider ID `1` and sets `selected` to true. Its schema
`score` is either null or a finite value in `[0, 1]`. When present, it rates
the provider's confidence that the label names the intended instrument or
instrument group. It is not a calibrated probability, separation-quality
score, gain, loudness, or share of the mixture.

The instrument value is the provider's exact class label. It must be 1 through
4,095 bytes of valid UTF-8. ASCII control bytes below `0x20` are invalid except
for tab. Version 1 does not set one label list across runners. A model runner
must document its label list and keep its meaning stable within its saved
provider, model, adapter, runtime, and backend facts. Consumers must not
equate labels from two such identities without an explicit mapping.

The runner also gives each result a `stem_id`. This is a unique lower-case
ASCII machine name, not the instrument class. It is 1 through 127 bytes, its
first byte is `a` through `z`, and each later byte is `a` through `z`, `0`
through `9`, `-`, or `_`. Two stems may have the same instrument label, but
they may not have the same `stem_id`. The output copies it exactly into the
audio name and event `part`, and uses it unchanged as the payload basename.

## Fixed order, IDs, and paths

Before it assigns IDs, the provider sorts stems by the exact `stem_id` bytes.
Duplicate IDs are an error. The source audio row comes first. Stem audio rows
and events then use the sorted stem order.

| Field | Value |
| --- | --- |
| Payload path | `audio/STEM_ID.wav` |
| Audio name | `STEM_ID` |
| Event `part` | `STEM_ID` |

Stem number `K` is its one-based place in the sorted list. Its event ID is
`K`. Its audio ID is the lowest positive JSON-safe integer not used by the
source row or an earlier stem. This keeps IDs fixed even when the caller chose
a source ID other than `1`.

Given the same set of stem IDs and exact result data, these rules produce the
same rows, IDs, and paths. They do not promise equal model output across
runtimes or hardware. The saved runtime and backend facts state which run made
the result.

## Portable runner boundary

`hwa_instrument_stem_provider_init` wraps one synchronous runner. It copies
the runner's runtime name, version, backend, fallback, model hash, and adapter
hash. The runner receives the checked source byte stream, source format, seed,
and cooperative timeout. It returns an owned array of stem IDs, instrument
labels, optional label scores, and WAVE byte streams.

When a label score is absent, the runner sets `score_valid` to false and the
numeric score field to zero. When present, it sets `score_valid` to true and
supplies a finite value in `[0, 1]`.

The runner keeps the array, its strings, byte-source fields, and bytes valid
and unchanged until the provider frees the task. The provider sorts and checks
the rows, parses and hashes every WAVE, builds the bundle, and runs the shared
request-bound output check before it reports ready. It calls the runner's
result cleanup once on each returned array, including a partial array from a
failed run. It destroys the runner context when the provider is destroyed.

The provider finishes in `start`, so a valid first poll is ready. It checks the
deadline around the runner, after sorting and provenance work, around each stem
parse and hash, and around the shared output check. The deadline stays
cooperative: it cannot stop one synchronous runner, parse, hash, or validation
call while that call runs.

## WAVE payloads and source clock

Each stem payload must pass the same bounded WAVE parser used for source
recordings. It has the same sample rate and frame count as the source. Sample
`n` in a stem refers to sample `n` in the source. An adapter must remove any
model padding and restore the exact source length before it returns.

A stem may use a different channel count, sample encoding, valid-bit count,
channel mask, or container kind from the source. The audio row records those
facts. A returned stem does not trim, shift, or stretch the source clock.

The output check requires exactly one payload binding for each stem path and
no binding for the external source. It reads each payload, checks its exact
size and SHA-256, and rejects a missing, extra, or repeated path. Saving the
bundle copies those checked bytes into a new directory and checks them again.

## Limits and errors

Let `N` be the stem count. A request can succeed only when:

```text
1 <= N
N + 1 <= max_audio_files
N <= max_events
N <= max_values
1 <= max_providers
```

`N`, the assigned event IDs, and the assigned audio IDs must also fit their C
types and the event bundle's JSON-safe ID range. The request's output limits,
not this task, set the smaller practical stem cap.

Each stem file must fit `max_payload_file_bytes`. The sum of stem bytes must
fit the in-memory lower bound for `max_bundle_bytes`; the saved manifest,
JSONL files, and payloads must fit the full bundle limits. Rows, strings,
canonical settings, and provider work must fit both the provider's startup
work cap and the request's `output_limits.max_work_bytes`. The common input
byte caps and cooperative deadline also apply.

Each stereo float32 RIFF stem uses eight data bytes per source frame. RIFF's
32-bit size field therefore caps the source at 536870907 frames, about
3 hours, 22 minutes, and 54 seconds at 44100 Hz. Smaller byte, frame, work, or
bundle limits can stop a run sooner.

The HTDemucs adapter counts its input, output, overlap, decode, and result
buffers against `max_work_bytes`. It streams completed WAVE samples through
temporary files. ONNX Runtime's session, graph, and internal run buffers sit
outside that cap. The model publisher reports about 1.1 GB of run-time memory
on an Apple M4 Pro.

The six temporary stem files use 48 bytes per source frame plus their headers.
Saving the bundle copies those files to the private output directory before it
closes the temporary files. Plan for one full stem set on the temporary-file
system and one on the output file system. On one volume, peak use can approach
96 bytes per source frame plus headers and bundle data. `max_work_bytes` does
not cap disk use.

Limits never ask a provider to drop a stem or shorten a payload. If the full
valid result does not fit, the task fails and returns no partial bundle.

Zero stems is a task error. It is not a valid empty result and not a warning.
The task also fails on a wrong task or pinned identity, a nonconforming input,
non-empty settings, a missing or duplicate stem ID, a malformed stem WAVE,
another sample clock, a bad hash or size, a payload binding error, a missing
or extra stem event, bad event bounds or links, bad values, a limit, a
deadline, or an allocation or file fault. One bad stem fails the whole
result.

## Out of scope

This contract does not promise clean isolation, remove room sound, or prove
that a label is true. It does not find note spans, pitch, rhythm,
tempo, meter, ornaments, voices, score links, or physical causes. It does not
merge bundles or make MIDI, Csound, LilyPond, or MusicXML.

The `infer-stem-note-events` workflow can run note inference against each
bundled stem and merge the results on the shared source clock. It preserves
each event's stem evidence link and provider identity. See [Stem note events
version 1](stem-note-events-v1.md).
