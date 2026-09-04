# Inference provider version 1

The inference provider interface lets an inference method return event data
without adding a model runtime to the analyzer core. The first version is a
private C interface in `src/inference_provider.h`. It has no saved job format
or general provider-selection command yet. The `analyze-events` command uses
the same output and payload checks for its built-in C method. The
`infer-note-events` uses the interface through the Basic Pitch ONNX adapter.
`separate-instruments` uses it through the instrument-stem provider and
HTDemucs ONNX adapter. `infer-stem-note-events` composes those saved stems
with a raw-audio Basic Pitch provider.

## Request

One request names one task and a bounded list of path-free inputs. Each input
has a unique ID, a role, a media type, a SHA-256 value, and a byte source with
a known size. Version 1 allows 1 through 4096 inputs.

`source_input_id` picks one input as the source recording. That input supplies
the request's sole sample clock. Other inputs can hold score or other context.
They do not add clocks.

The request fields are:

| Field | Meaning |
| --- | --- |
| `task` | Stable, namespaced task name. |
| `settings_json` | Complete task settings as one JSON object. |
| `expected_provider_name` | Exact provider name the caller chose. |
| `expected_provider_version` | Exact provider version the caller chose. |
| `expected_model_sha256` | Exact model hash, or an empty string when no model applies. |
| `seed` | Fixed seed for methods that use random values. |
| `source_recording_id` | Event-bundle audio ID for the source clock. |
| `source_input_id` | ID of the input that supplies the source clock. |
| `inputs`, `input_count` | Named, role-based input list and its bounded count. |
| `source_format` | Checked WAVE format and clock facts. |
| `max_input_file_bytes` | Hard cap for each input byte source. |
| `max_input_bytes` | Hard cap for the sum of all input byte sources. |
| `timeout_milliseconds` | Nonzero cooperative task deadline supplied to the adapter. |
| `output_limits` | Hard caps for returned bundle rows and payloads. |

`hwa_inference_request_validate` checks the provider descriptor, pinned
identity, strict UTF-8 settings JSON, input names and roles, per-file and total
byte caps, every input hash, the sole source role, and the source WAVE bytes
against the declared format. Input display names and provider identity strings
must use valid UTF-8. A provider may infer overlapping events from the source.
It must state all event bounds
as exact, half-open sample ranges on that clock.

The provider descriptor holds a name, version, and model hash that stay fixed
for its life. `start` must reject a request if any expected identity field does
not match. This rule stops a provider from using a fallback model without the
caller's knowledge. A result's provider row must state the same identity.

Version 1 first checks that settings hold valid JSON. Each provider documents
the object and spellings it accepts, then writes its normal form to the
provider row. The fixed provider accepts an empty object with any JSON white
space and records it as `{}`. The Basic Pitch provider requires the canonical
key order and value spellings made by `hwa_basic_pitch_task_settings_build`,
allows JSON white space outside strings, and records the form without that
white space. It rejects missing, extra, or reordered fields. The instrument-
stem provider accepts only an empty object, permits JSON white space outside
it, and records `{}`.

The request, input rows, strings, byte-source fields, and byte content remain
owned by the caller. They stay valid and unchanged until the caller frees the
task. A provider may copy them during `start`, but callers cannot depend on
that.

## Task life cycle

`start` first sets its task output to null. On success, it returns one opaque
task that the caller must free once. On failure, it leaves the task null,
keeps no request data, and needs no cleanup.

`poll` checks a task without waiting. A successful poll returns either
`HWA_INFERENCE_PENDING` with no output or `HWA_INFERENCE_READY` with one
borrowed output. A ready output remains valid until `task_free`. More polls may
return the same ready output.

A failed poll reports pending and clears the output pointer. The failure ends
polling. The caller must still pass the task to `task_free` once.

`task_free` owns all task cleanup. When called on pending work, it must cancel
the work before it stops using request data. A provider must not call into the
analyzer after `task_free` returns. It must accept a null task so callers can
use one cleanup path after `start`.

The caller must free every task before it destroys the provider. Provider
descriptors have a `destroy` callback for shared context such as a model
session or worker. `hwa_inference_provider_destroy` calls it once and clears
the descriptor. The callback accepts a null context.

This life cycle permits a native provider to finish in `start` and permits a
web provider to finish after an asynchronous model call. It does not require
threads, child processes, or a model runtime.

The Basic Pitch and instrument-stem providers start a monotonic deadline
before request checking. They check it between costly phases. Basic Pitch
checks before and after every model window. The HTDemucs runner starts its own
cooperative deadline and checks it between windows; its provider also checks
after the runner returns. A deadline cannot halt request checking, decoding,
output building, or a synchronous `OrtRun` while that call runs. After an
overrun returns, the code discards the late result and starts no later phase
or model window.

The commands open and check each model session before they start the provider
deadline. `--max-model-bytes` bounds the model read; the deadline does not
bound ONNX session creation.

## Output

One output contains a complete `HWAEventBundle` aggregate and zero or more
byte-backed payload bindings. The provider owns the rows and bytes until
`task_free` and does not change them while the caller can read them. Before it
reports ready, it calls `hwa_inference_output_validate_for_request`. The
analyzer calls the same function before it accepts the result. The provider
does not edit a saved input or final output directory.

The shared check validates the aggregate, then requires exactly one payload
for each non-empty audio `relative_path` and each trace `relative_path`. It
rejects missing, extra, and repeated bindings. It reads each payload in full,
checks its stated size and SHA-256 value, enforces the per-file cap, and keeps
the exact sum of payload bytes within `max_bundle_bytes`. The row limits on
audio and traces also bound the payload count.

`max_manifest_bytes` and `max_index_bytes` apply only when JSON files exist.
The full `max_bundle_bytes` check also needs their exact saved sizes. Only
bundle serialization can enforce those three saved-file checks in full. The
in-memory output check does not guess encoded JSON sizes; its payload sum check
is a required lower bound on the final bundle size.

The request-bound output check first runs the base bundle and payload check.
It then requires one matching provider identity, one source audio row that
matches the selected input and every source-format field, one sample clock for
events, traces, and derived audio, and the matching provider ID on every
inference value.

The private seam has no serialized request format. Its result data uses the
same `hwa-events/v1` rows as a saved event bundle. A payload binding pairs one
bundle-relative path with one bounded byte source. The save bridge validates
the output, reads each source through `read_at`, writes a new bundle, and
checks the saved result. It does not treat a byte source's display name as a
path.

`hwa_inference_provenance_settings_build` makes a stable provider settings
object for a checked request. It records the task, a fixed-width seed, every
input name, role, type, byte size, and hash in input-ID order, runtime facts,
and normalized task settings. A real provider must use this form or a stricter
documented form so a saved bundle retains all inputs that affected the run.

`hwa_inference_frame_span_to_samples` maps a half-open model-frame span to the
source clock with checked integer arithmetic. It floors the start, rounds the
end up, clips the end to the source, and rejects an empty result. The first
polyphonic task fixes this rule in
[Polyphonic note events version 1](polyphonic-note-events-v1.md).

The common interface contains no tensor names, tensor shapes, model paths,
runtime handles, or execution-provider options. Each adapter owns audio
preparation, model inputs, model outputs, thresholds, and conversion into
events and traces.

## Fixed provider

The first adapter exists only to prove the interface. It accepts:

- task `org.hlolli.fixed-note`;
- an empty JSON settings object;
- expected provider `org.hlolli.fixed-inference`, version `1`, and no model
  hash;
- 1 through 4096 inputs with unique IDs;
- a selected `audio/wav` input with role `source-recording`;
- extra named inputs, such as `score-context`, which it ignores;
- a source with at least 192 frames;
- output limits that admit its whole result.

It builds and checks its fixed result during `start`. Its first valid poll
returns `HWA_INFERENCE_PENDING`; its second and later polls return
`HWA_INFERENCE_READY`. Freeing it after the pending poll tests cancellation.
The ready result contains:

- provider `org.hlolli.fixed-inference`, version `1`, with no model hash;
- the unchanged source ID, name, hash, byte size, and WAVE format;
- one `note` event over `[64, 192)`;
- one selected inference named `pitch-hz`;
- value `440.00000000000006` with unit `Hz`;
- method score `0.875` and provider ID `1`;
- no payload, trace, or warning.

The fake does not inspect the audio samples. It makes no claim about musical
or physical accuracy. It checks the source name against the output work cap
before it allocates result strings.

## Basic Pitch ONNX provider

The first model provider accepts:

- task `org.hlolli.polyphonic-note-events-v1`;
- provider `org.hlolli.basic-pitch-onnx`, version `1`;
- the SHA-256 of the exact caller-supplied model bytes;
- one mono 22050 Hz WAVE source input;
- the fixed tensor and window contract in
  [Polyphonic note events version 1](polyphonic-note-events-v1.md).

The native runner uses ONNX Runtime's CPU execution provider. It loads the
model from checked bytes and checks the model's input and output names, float
types, and shapes. The portable provider owns windowing, decoding, sample
mapping, event creation, limits, and provenance. This keeps model policy out
of the shared inference interface.

The provider finishes in `start`, so its first valid poll is ready. It returns
zero or more overlapping `note` events. It returns no payload or trace.

Run it with:

```sh
hlolli-wg-analyzer infer-note-events INPUT.wav \
  --model MODEL.onnx --output NEW.hwa-events
```

`--expect-model-sha256 HEX` can reject the wrong model bytes. The saved
provider row and settings record the model, adapter, source, runtime, backend,
decoder, and window facts that can affect the result. CMake derives the
adapter hash from the source files that implement the adapter, decoder, clock,
and saved provenance.

## Instrument stem provider

The portable separation adapter accepts task
`org.hlolli.instrument-stems-v1` and provider
`org.hlolli.instrument-stem-provider`, version `1`. It wraps one synchronous
runner and converts its named WAVE results into checked, bundled instrument
stems. It fixes row order, IDs, paths, instrument-region links, label scores,
payload hashes, limits, provenance, and ownership. The full contract is in
[Instrument stems version 1](instrument-stems-v1.md).

The HTDemucs adapter supplies the first model-backed runner. It accepts the
pinned six-stem ONNX graph, applies its fixed window schedule, and writes
stereo float32 WAVE payloads through temporary files. Run it with:

```sh
hlolli-wg-analyzer separate-instruments INPUT.wav \
  --model htdemucs_6s_fp16weights.onnx \
  --output NEW.hwa-events
```

The fake runner proves the payload boundary. Neither runner makes a claim
that a label is true or that a stem is clean.

## Raw-audio Basic Pitch provider

Provider `org.hlolli.basic-pitch-audio-provider`, version `1`, accepts task
`org.hlolli.note-events-on-audio-v1`. It takes one stereo float32 44100 Hz
WAVE source, makes a fixed mono 22050 Hz work file, calls the Basic Pitch
provider, and maps the note bounds back to the source clock. Its output still
has one source row and one provider row, so the common request-bound checks
apply.

`infer-stem-note-events` calls this provider once per saved instrument stem
and merges the checked results. The merge keeps one provider row per run and
links each note to its stem and full-span instrument region. See [Stem note
events version 1](stem-note-events-v1.md).

## Deferred work

The default build has no model-runtime dependency. A native process adapter
and a web adapter should pass the same provider tests before this private
interface is made public. A real asynchronous provider also needs a wait and
deadline contract; repeated busy polling is not enough.

ONNX adapters own model loading and model-specific processing. WebNN may
become an execution choice after a web adapter passes the same contract and
result tests. Version 1 does not promise WebNN support or equal results across
execution providers. A provider must report a failure or use a stated
fallback when its model cannot run on a chosen backend.

## Files and tests

`src/inference_provider.h` holds the private seam. `src/inference.c` holds the
shared request and output checks, save bridge, and fixed adapter.
`src/inference_clock.c` holds exact clock mapping and monotonic deadline
checks, and
`src/inference_provenance.c` holds saved run settings.
`src/basic_pitch_provider.c` holds portable windowing, decoding, event
creation, and provenance. `src/inference_basic_pitch_onnx.c` holds the native
ONNX Runtime runner. `tests/inference_tests.c` uses a valid small WAVE file
with its true hash. It checks the exact fixed-provider result, payload hashes,
binding counts, input and output limits, settings, identity, score context,
source binding, pending-to-ready polling, cancellation, and provider cleanup.
Separate tests check the clock, provenance, Basic Pitch decoder, provider,
and ONNX boundary. `src/instrument_stem_provider.c` holds the portable stem
adapter, and `tests/instrument_stem_provider_tests.c` checks its fake runner,
order, IDs, paths, scores, WAVE payloads, limits, ownership, and saved bundle
round trip. `src/htdemucs.c` holds the portable six-stem input, overlap, and
WAVE output code. `src/inference_htdemucs_onnx.c` checks and runs the pinned
native graph. The fake-runner and opt-in real-model tests cover this adapter.
The production library includes the portable code so later runtimes can use
the same checked output path.
