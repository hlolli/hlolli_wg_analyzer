# Event bundle version 1

An event bundle stores measured and inferred facts about audio without tying
them to MIDI, Csound, LilyPond, MusicXML, or one model. Version 1 uses the
schema name `hwa-events` and schema version `1`.

This document defines the saved form. A bundle reader must reject a bundle
that breaks any rule below.

## Directory layout

```text
take.hwa-events/
  manifest.json
  events.jsonl
  traces.jsonl
  audio/                 optional
    ...
  traces/                optional
    ...
```

The three files at the bundle root are required. `events.jsonl` and
`traces.jsonl` may be empty. The `audio/` and `traces/` directories are absent
when the bundle has no payload of that kind.

`manifest.json` holds bundle facts, audio facts, providers, warnings, counts,
and the file inventory. Each non-empty line in a JSON Lines file holds one
complete JSON object. Blank lines, byte-order marks, duplicate keys in
schema-owned objects, and text outside a JSON value are invalid. Text files
use UTF-8 and line feed line endings.

The file inventory lists both JSONL indexes and every audio or trace payload
that the bundle data names. Listed files that do not exist are invalid.
Readers ignore other files, and those files are not part of the bundle. A
directory entry is not a file inventory entry.

## Common data rules

JSON objects have only the keys named in this document. All named keys are
required unless their table says otherwise. A missing value uses the stated
empty or `null` form; a writer does not drop its key.

IDs and counts are JSON integers from `0` through `9007199254740991`. IDs
start at `1`; `0` is not a saved ID. IDs are unique within their own list.
References state which list they use.

An `i64` named value may range from `-9007199254740991` through
`9007199254740991`. This range keeps every saved integer exact in C and in a
browser.

A double is an IEEE 754 binary64 value. It must be finite. NaN and positive or
negative infinity are invalid. Writers use a period as the decimal mark and
write enough digits for reading the number back to yield the same binary64
value. Readers must not round a saved double to display precision before they
return it.

Strings may contain any JSON string data except a null character. An empty
string has a stated meaning where this document permits it. Runtime limits
may cap string size, row count, file size, nesting depth, and total work, but
those limits do not change the saved meaning.

## `manifest.json`

The manifest is one JSON object with these keys:

| Key | Type | Rule |
| --- | --- | --- |
| `schema` | string | Must equal `hwa-events`. |
| `schema_version` | integer | Must equal `1`. |
| `tool_version` | string | Version of the writer. It does not select the schema. |
| `counts` | object | Exact row and child counts. |
| `providers` | array | Provider objects. |
| `audio` | array | Audio objects. |
| `warnings` | array | Warning objects. |
| `files` | array | Index and referenced payload inventory. |

The `counts` object has exactly these integer keys:

| Key | What it counts |
| --- | --- |
| `providers` | Objects in `providers`. |
| `audio` | Objects in `audio`. |
| `events` | Lines in `events.jsonl`. |
| `values` | Named values in all event rows. |
| `traces` | Lines in `traces.jsonl`. |
| `trace_refs` | Trace references in all event rows. |
| `warnings` | Objects in `warnings`. |

The saved counts must match the data. A mismatch is invalid.

### Providers

A provider names the code or model that made values. A parser, fixed rule,
analyzer method, or trained model can be a provider.

| Key | Type | Rule |
| --- | --- | --- |
| `id` | ID | Unique provider ID. |
| `name` | string | Non-empty stable provider name. |
| `version` | string | Non-empty provider or method version. |
| `model_sha256` | string | Lower-case SHA-256 of the model bytes, or empty when no model applies. |
| `settings` | object | Settings that affect the result. An empty object is valid. |

`settings` may hold nested JSON objects, arrays, strings, booleans, nulls, and
finite numbers. Its keys and values do not extend the event bundle structure.
The provider owns their meaning.

### Audio

Each audio object has these keys:

| Key | Type | Rule |
| --- | --- | --- |
| `id` | ID | Unique audio ID. |
| `kind` | string | `source-recording`, `derived-audio`, or `instrument-stem`. |
| `name` | string | Non-empty display name. |
| `relative_path` | string | Bundled WAVE path, or empty only for an external source recording. |
| `path_hint` | string | Optional user hint, written as an empty string when absent. It is inert. |
| `sha256` | string | Lower-case SHA-256 of the exact audio file bytes. |
| `file_bytes` | integer | Exact audio file size in bytes. |
| `format` | object | WAVE clock and format facts. |
| `source_recording_id` | ID or null | Required for derived audio and stems; null for a source recording. |

The reader never opens `path_hint`. It may name the old input path, a URL, or
other display text. A caller must bind external audio before another command
opens it.

A non-empty `relative_path` starts with `audio/` and names one inventory
entry. Its hash and size must match that entry. Derived audio and instrument
stems are always bundled. Their `source_recording_id` points straight to a
`source-recording`, not to another derived file.

Version 1 keeps one source clock. Derived audio and stems have the same sample
rate and frame count as their source recording. They may have a different
channel count or sample encoding. Sample `n` in either file refers to sample
`n` on the source clock.

The `format` object has exactly these keys:

| Key | Type | Rule |
| --- | --- | --- |
| `container` | string | `riff` or `rf64`. |
| `encoding` | string | `pcm` or `ieee-float`. |
| `channels` | integer | Positive channel count. |
| `sample_rate_hz` | integer | Positive frames per second. |
| `bits_per_sample` | integer | Positive stored bits per sample. |
| `valid_bits_per_sample` | integer | Positive and no greater than `bits_per_sample`. |
| `block_align` | integer | Positive WAVE frame size in bytes. |
| `channel_mask` | integer | WAVE channel mask; `0` means unspecified. |
| `frames` | integer | Number of sample frames in the source clock. |
| `data_bytes` | integer | Number of encoded audio data bytes. |
| `duration_seconds` | double | `frames / sample_rate_hz`, saved with round-trip precision. |

The fields record facts from WAVE parsing. They do not replace WAVE header
checks when a later command opens the audio.

A bundle has at least one audio object, including at least one source
recording.

### Warnings

A warning records a valid but weak or partial result. It does not make the
bundle invalid.

| Key | Type | Rule |
| --- | --- | --- |
| `id` | ID | Unique warning ID. |
| `code` | string | Non-empty stable machine name. |
| `message` | string | Direct human-readable detail. |
| `event_id` | ID or null | Event affected by the warning, or null for a bundle-wide warning. |

A non-null `event_id` must resolve.

### File inventory

Each file object has these keys:

| Key | Type | Rule |
| --- | --- | --- |
| `relative_path` | string | Unique safe path below the bundle root. |
| `file_bytes` | integer | Exact file size. |
| `sha256` | string | Lower-case SHA-256 of the exact file bytes. |

The inventory always includes `events.jsonl` and `traces.jsonl`. It also
includes each bundled audio and trace payload. It does not include
`manifest.json`, so the manifest has no self-hash.

A reader computes the SHA-256 of the exact `manifest.json` bytes and exposes
that value as the manifest hash. A rewrite may change this hash even when the
logical data stays equal. Audio and trace objects repeat their payload hash
and byte size; both copies must agree with the inventory.

## `events.jsonl`

Each line describes one performance event. An event can be a note, rest,
trill, phrase, noise span, or another named occurrence.

| Key | Type | Rule |
| --- | --- | --- |
| `id` | ID | Unique event ID. |
| `kind` | string | Non-empty event kind. |
| `source_recording_id` | ID | Source recording that owns the sample clock. |
| `evidence_audio_id` | ID or null | Audio measured for this event; null means the source recording. |
| `parent_id` | ID or null | Containing event, or null. |
| `start_sample` | integer | First included sample frame on the source clock. |
| `end_sample` | integer | First excluded sample frame on the source clock. |
| `voice` | string | Voice label, or empty when absent. |
| `part` | string | Part label, or empty when absent. |
| `score_event_id` | string | Opaque ID from supplied score data, or empty when absent. |
| `values` | array | Named value objects. |
| `trace_refs` | array | Trace reference objects. |

`source_recording_id` must point to audio of kind `source-recording`.
`evidence_audio_id`, when set, must point to that source recording or to audio
derived from it.

### Sample bounds, overlap, and nesting

Event bounds are exact and half-open: `[start_sample, end_sample)`. They obey
all these rules:

- `start_sample < end_sample`;
- `end_sample` is no greater than the source recording's frame count;
- no conversion through seconds or beats may change either bound.

Two events may overlap. Overlap means polyphony or concurrent activity and is
not an error. Overlap does not create a parent link.

A non-null `parent_id` must name another event on the same source clock. The
parent span must contain the full child span. Parent links must not form a
cycle and must stay within the reader's nesting limit. Peers may overlap even
when neither contains the other.

### Named values

Named values carry facts and candidates without adding a new fixed event key.
Each value object has these keys:

| Key | Type | Rule |
| --- | --- | --- |
| `name` | string | Non-empty stable name. |
| `kind` | string | `text`, `f64`, `i64`, or `bool`. |
| `value` | typed JSON value | Its JSON type must match `kind`. |
| `unit` | string | Unit name, or empty when none applies. |
| `basis` | string | `observation`, `inference`, or `score`. |
| `score` | double or null | Method score in `[0, 1]`, or null when none was given. |
| `provider_id` | ID or null | Provider that made or read the value, or null. |
| `selected` | boolean | True when the producer chose this candidate. |

`observation` means a value measured from the event's evidence audio. It does
not state why the sound occurred. `inference` means a rule or model estimated
the value. `score` means supplied score material stated the value. The numeric
`score` key rates a method result; it is separate from the `score` basis.

Names use lower-case ASCII dot-separated parts. Each part starts with a letter
and then uses letters, digits, `_`, or `-`. Names beginning with `hwa.` are
reserved for this project. Other producers use a name they control, such as
`org.example.bow-force`.

An event may hold several values with the same name. These are competing
candidates and must use the same kind and unit. At most one may have
`selected` set to true. No selected candidate means the method did not accept
a guess; the unselected candidates may still be useful. Array order has no
meaning. A non-null `provider_id` must resolve.

### Trace references

Each trace reference has these keys:

| Key | Type | Rule |
| --- | --- | --- |
| `trace_id` | ID | Trace named in `traces.jsonl`. |
| `role` | string | Non-empty role of the trace for this event. |
| `first_point` | integer | First included trace point. |
| `point_count` | integer | Positive number of points. |

The half-open point range
`[first_point, first_point + point_count)` must fit in the named trace, and the
addition must not overflow. One event may refer to many traces, and one trace
may serve many events. A reference does not state that every trace window lies
inside the event. The role states why the caller linked it.

## `traces.jsonl`

Each line describes one dense, regular trace. The data stays in a separate
payload so a reader can inspect events without loading it.

| Key | Type | Rule |
| --- | --- | --- |
| `id` | ID | Unique trace ID. |
| `name` | string | Stable name using the named-value grammar. |
| `unit` | string | Unit shared by all values, or empty when none applies. |
| `relative_path` | string | Safe path below `traces/`. It cannot be empty. |
| `sha256` | string | Lower-case SHA-256 of the exact payload bytes. |
| `format` | string | `csv-f64` or `f64le`. |
| `source_recording_id` | ID | Source recording that owns the sample clock. |
| `first_sample` | integer | Start sample of the first analysis window. |
| `hop_samples` | integer | Positive distance between window starts. |
| `window_samples` | integer | Positive analysis window size. |
| `point_count` | integer | Positive number of trace points. |
| `value_width` | integer | Positive values per point. |
| `file_bytes` | integer | Exact payload size in bytes. |

Trace point `i` starts at
`first_sample + i * hop_samples`. Its half-open analysis window is
`[start, start + window_samples)`. Grid arithmetic must not overflow, and the
last window must end no later than the source recording's frame count.

An `f64le` payload is a row-major sequence of little-endian binary64 values.
Its size is exactly `point_count * value_width * 8` bytes. A `csv-f64` payload
has no header and has exactly `point_count` lines. Each line has exactly
`value_width` comma-separated finite doubles and a line feed. Both forms
reject NaN and infinity. CSV writers use enough decimal digits for exact
binary64 round trips.

The trace path, hash, and byte size must match one inventory entry.
`source_recording_id` must resolve to audio of kind `source-recording`.

## Safe paths and file checks

A safe relative path follows all these rules:

- it is valid UTF-8 and not empty;
- it uses `/`, never `\`, as its separator;
- it has no leading `/`, drive prefix, null character, empty part, `.` part,
  or `..` part;
- it names a regular file below the bundle root;
- no path part is a symbolic link;
- its spelling is unique in the inventory.

Readers reject a linked final file. On systems with `lstat`, they also reject
a linked payload directory. Writers require one explicit binding for every
copied payload. The public writer accepts regular source files. The private
inference bridge accepts bounded byte sources and reads them through
`read_at`. Both forms check the exact size, hash, and payload limit. Writers
create a new output directory, write it, then read and check the result. They
never replace an existing directory. A failed write removes files and
directories that it created when the operating system allows that cleanup.

## Validation and ownership

Reading a bundle also validates it. A schema error, bad JSON, missing row,
bad reference, changed file, failed hash, limit, allocation fault, or file
fault fails the whole read. The reader returns no partial aggregate.

The read result owns all event, value, trace, audio, provider, warning, and
string data. The matching free operation releases the full aggregate. Dense
trace samples and WAVE sample data stay on disk and are not part of that owned
memory.

`max_work_bytes` caps the decoded data retained in that result. The separate
`max_manifest_bytes` and `max_index_bytes` limits cap the raw input buffers used
while parsing. Validation also uses short-lived sorted indexes proportional to
the allowed row counts. That temporary index memory is extra to the retained
decoded total and is released before the call returns.

The C interface also validates a caller-owned in-memory bundle without file
work. This checks the same IDs, clocks, spans, links, values, trace grids, and
limits that apply before a write.

The command-line form validates a saved directory and prints its counts and
hash:

```sh
hlolli-wg-analyzer validate-event-bundle take.hwa-events
hlolli-wg-analyzer --json validate-event-bundle take.hwa-events
```

Success means that the data follows this schema. It does not mean that a note,
instrument guess, model value, or physical claim is true.

Writing takes one complete aggregate. It validates all links before it writes
the directory, then reads the saved bundle back. Analysis, transcription,
source separation, and score parsing happen before this call. They are not
event-bundle write steps.

## Version changes

Readers for this version accept only `schema` equal to `hwa-events` and
`schema_version` equal to `1`. `tool_version` and provider versions record who
wrote a result; they do not relax schema checks.

New named values, event kinds, trace roles, provider settings, voice names,
part names, and score IDs do not need a schema change. A change to required
keys, key meaning, data types, enum values, clock rules, path rules, or payload
encoding needs a new schema version. Version 1 readers reject unknown object
keys and unknown enum values instead of guessing their meaning.

IDs and references carry meaning. JSONL row order and array order do not,
apart from the fixed value order inside a trace point. The writer keeps caller
row order.
