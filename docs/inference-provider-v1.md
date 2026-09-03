# Inference provider version 1

The inference provider interface lets an inference method return event data
without adding a model runtime to the analyzer core. The first version is a
private C interface in `src/inference_provider.h`. It has no saved job format
or provider-selection command yet. The `analyze-events` command uses the same
output and payload checks for its built-in C method.

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
| `output_limits` | Hard caps for returned bundle rows and payloads. |

The caller checks input hashes and the source WAVE format before `start`. A
provider may infer overlapping events from the source. It must state all event
bounds as exact, half-open sample ranges on that clock.

The provider descriptor holds a name, version, and model hash that stay fixed
for its life. `start` must reject a request if any expected identity field does
not match. This rule stops a provider from using a fallback model without the
caller's knowledge. A result's provider row must state the same identity.

Version 1 treats settings as JSON data, not as an exact input spelling. Each
provider parses the object it supports and writes its normal form to the
provider row. The fixed provider accepts an empty object with any JSON white
space and records it as `{}`.

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

## Output

One output contains a complete `HWAEventBundle` aggregate and zero or more
byte-backed payload bindings. The provider owns the rows and bytes until
`task_free` and does not change them while the caller can read them. Before it
reports ready, it calls `hwa_inference_output_validate`. The analyzer calls the
same function before it accepts the result. The provider does not edit a saved
input or final output directory.

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

The private seam has no serialized request format. Its result data uses the
same `hwa-events/v1` rows as a saved event bundle. A payload binding pairs one
bundle-relative path with one bounded byte source. The save bridge validates
the output, reads each source through `read_at`, writes a new bundle, and
checks the saved result. It does not treat a byte source's display name as a
path.

This means a saved result does not yet retain the request task, seed, or hashes
for non-source inputs such as score context. The provider row keeps the
provider, model, and normalized settings, and the audio row keeps the source
recording hash. That is not full run provenance. A saved provenance design is
required before a real provider ships.

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

## Deferred work

This version does not start a process, load ONNX, merge bundles, or separate
sources. A native process adapter and a web adapter should pass the same
provider tests before this private interface is made public. A real
asynchronous provider also needs a wait and deadline contract; repeated busy
polling is not enough.

ONNX adapters will own model loading and model-specific processing. WebNN is
an optional execution choice, not an analyzer promise. A provider must report
a failure or use a stated fallback when its model cannot run there.

## Files and tests

`src/inference_provider.h` holds the private seam. `src/inference.c` holds the
shared output check, save bridge, and fixed adapter. `tests/inference_tests.c`
uses a valid small WAVE file with its true hash. It checks the exact result,
payload hashes, binding counts, size and work limits, settings, identity, score
context, pending-to-ready polling, cancellation, and provider cleanup.
The production library includes this code so built-in analysis and later
adapters can use the same checked output path.
