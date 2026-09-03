# Analyze events version 1

`analyze-events` reads one named WAVE file and writes one new
`hwa-events` version 1 directory. It finds voiced spans in solo or mostly
monophonic audio. It saves note candidates, frame traces, source facts, and
the settings that affect those results.

The saved form follows [Event bundle version 1](event-bundle-v1.md).

## Command

```sh
hlolli-wg-analyzer [ANALYSIS OPTIONS] analyze-events INPUT.wav \
  --output NEW.hwa-events
```

`INPUT.wav` and `NEW.hwa-events` must be named paths, not `-`. The output
path must name a directory that does not exist. The command does not accept
`--replace` or `--json`.
It returns `0` on success, `1` when analysis or writing fails, and `2` for bad
command use.

The command accepts these analysis options:

| Option | Default | Rule |
| --- | ---: | --- |
| `--channel N` | none | Analyze one existing, one-based input channel. It conflicts with `--mixdown`. |
| `--mixdown` | off | Analyze an equal mono mix. |
| `--block-frames N` | `4096` | Decode 1 through 1,048,576 frames per block. |
| `--frame-size N` | `2048` | Use a power of two from 256 through 16,384 samples. |
| `--hop-size N` | `512` | Use 1 through `frame-size` samples. |
| `--silence-threshold DBFS` | `-60` | Use a finite value from -200 through 0 dBFS. |
| `--max-bytes N` | `17179869184` | Cap input file bytes. `N` must be positive. |
| `--max-frames N` | `2000000000` | Cap decoded sample frames. `N` must be positive. |
| `--max-work-bytes N` | `268435456` | Cap analysis work storage. `N` must be positive. |
| `--max-transforms N` | `200000` | Cap spectral transforms. `N` must be positive. |
| `--max-track-points N` | `200000` | Cap frame results held by the analysis pass. `N` must be positive. |

With neither channel option, `channel_mode` is `keep`. The analyzer keeps all
input channels for level and spectrum work, but it still makes one event
track. It takes each frame's pitch from the channel with the most power.
`--mixdown` can cancel channels that have opposite phase.

The command rejects other global analysis options. This includes
`--max-spectrum-values`, `--max-lag`, and `--true-peak-oversample`. Score,
alignment, item, physical-check, run, and experiment options are also invalid.
Version 1 has no command flags for its note rules.

## Source row and provider

The bundle has one `source-recording` audio row with ID `1`:

- `name` is the last path part of `INPUT.wav`;
- `relative_path` is empty, so the command does not copy the WAVE file;
- `path_hint` is the input path as supplied on the command line;
- `sha256` hashes the exact input file bytes;
- `file_bytes` is the exact regular-file size;
- `format` holds the parsed WAVE format and source sample clock.

`path_hint` is inert. A reader does not open it. The bundle saves it verbatim,
so it may contain a local path. A later tool must bind the source again and
check its hash. The analyzer copies the bounded input from one open file handle
to a private temporary file, checks that copy against the still-open input,
then hashes and analyzes the private copy. It fails before feature analysis if
the source bytes or size changed while it made the copy.

The sole provider has ID `1`, name
`org.hlolli.monophonic-analysis`, version `1`, and an empty model hash. Its
`settings` object has these keys:

| Key | Default | Meaning |
| --- | ---: | --- |
| `algorithm` | `"monophonic-v1"` | Fixed rule set named by this document. |
| `channel_mode` | `"keep"` | `"keep"`, `"select"`, or `"mix"`. |
| `selected_channel` | `0` | One-based channel for `"select"`; otherwise `0`. |
| `frame_size` | `2048` | Analysis window size in samples. |
| `hop_size` | `512` | Distance between window starts in samples. |
| `silence_threshold_dbfs` | `-60` | Minimum frame RMS for an active point. |
| `min_pitch_confidence` | `0.60` | Minimum pitch score for an active point. |
| `pitch_split_semitones` | `0.75` | Minimum stable pitch jump that splits an event. |
| `onset_split_strength` | `0.65` | Minimum local onset peak that splits an event. |
| `min_note_points` | `2` | Minimum active points in an event. |
| `max_gap_points` | `1` | Most inactive points joined between active points. |

The last five settings are fixed in version 1. Decode block size and resource
caps do not appear in `settings` because they do not change an accepted
result.

## Trace grid and payloads

Let `N` be the source frame count, `F` the frame size, and `H` the hop size.
The saved trace point count is:

```text
P = 0                              when N < F
P = 1 + floor((N - F) / H)        when N >= F
```

Point `i` measures the full half-open window `[i * H, i * H + F)`. All traces
use `first_sample = 0`, `hop_samples = H`, `window_samples = F`,
`point_count = P`, and `value_width = 1`.

The feature pass may form zero-filled windows at the end of the input. The
writer drops every window that is not wholly inside the source. If `P` is
zero, it writes no trace rows or trace payloads. Otherwise it writes all nine
traces:

| ID | Name | Unit | Invalid value |
| ---: | --- | --- | ---: |
| 1 | `rms-dbfs` | `dBFS` | Always finite; zero power is `-300`. |
| 2 | `pitch-hz` | `Hz` | `0` |
| 3 | `pitch-confidence` | `ratio` | `0` |
| 4 | `pitch-valid` | `bool` | `0` |
| 5 | `onset-strength` | `ratio` | Always finite. |
| 6 | `spectral-centroid-hz` | `Hz` | `0` |
| 7 | `spectral-rolloff-85-hz` | `Hz` | `0` |
| 8 | `spectral-flatness` | `ratio` | `0` |
| 9 | `spectrum-valid` | `bool` | `0` |

Each payload is at `traces/NAME.f64le`. It holds `P` finite IEEE 754 binary64
values in little-endian byte order and has exactly `P * 8` bytes. The validity
traces use the numeric values `0.0` and `1.0`. A zero pitch or spectrum value
has no meaning unless its matching validity trace is `1.0`.

`rms-dbfs` uses mean power across the analyzed channels. Spectrum values use
the mean channel power spectrum. Pitch uses windowed autocorrelation on the
loudest analyzed channel for that frame. The pitch search covers 40 through
2,000 Hz, subject to the frame size and sample rate. `pitch-valid` needs an
internal pitch confidence of at least `0.30`.

`onset-strength` uses the following sum, clamped to `[0, 1]`:

```text
0.40 * clamp(2 * spectral_flux)
+ 0.25 * energy_rise
+ 0.20 * phase_change
+ 0.15 * pitch_change
```

## Event rules

The analyzer scans saved trace points from first to last. A point is active
only when all these checks pass:

- RMS is at least `silence_threshold_dbfs`;
- pitch is valid, finite, and positive;
- pitch confidence is at least `min_pitch_confidence`.

It groups active points and joins at most one inactive point between them.
It drops trailing inactive points. A group needs at least two active points.

The analyzer then splits a group at a valid pitch jump or onset peak. A
pitch split needs four adjacent active points. Each two-point side must vary
by no more than `0.375` semitone, and the geometric mean pitch must change by
at least `0.75` semitone. An onset split needs an active point whose strength
is at least `0.65`, at least the prior value, and greater than the next value.
A flat peak therefore splits at its last equal-valued point.
A split must leave enough points and active support for the two-point minimum.
A final short part joins the prior event when one exists.

The result has one non-overlapping `note` event per segment, in source order.
Event IDs start at `1`. Each event has source recording and evidence audio ID
`1`, no parent, and empty `voice`, `part`, and `score_event_id` strings.

Each event refers to all nine traces. Its trace range covers every segment
point, including a joined inactive gap point. The role equals the trace name.

### Sample bounds

Event bounds use the source sample clock and are half-open. For a segment from
trace point `f` through trace point `l`, define:

```text
center(i) = i * H + floor(F / 2)
left      = floor(H / 2)
right     = H - left
```

The start is `0` when `f` is the first trace point; otherwise it is
`center(f) - left`. The end is `N` when `l` is the last saved trace point;
otherwise it is `center(l) + right`, clipped to `N`.

These bounds divide the frame-center grid. They estimate note bounds; they are
not sample-exact acoustic onset or release points. A last event can extend
from its last full trace window to sample `N`. Its values still use only its
referenced trace points.

### Event values

Every value is a selected `f64` from provider `1`.

| Name | Unit and basis | Aggregation | Method score |
| --- | --- | --- | --- |
| `pitch-hz` | `Hz`, inference | Confidence-weighted geometric mean of active pitch points. | Mean confidence of the included pitch points. |
| `rms-dbfs` | `dBFS`, observation | Convert each point from dBFS to power, take the mean, then convert back to dBFS. | none |
| `onset-strength` | `ratio`, observation | Maximum combined onset strength. | none |
| `spectral-centroid-hz` | `Hz`, observation | Mean over spectrum-valid points. | none |
| `spectral-rolloff-85-hz` | `Hz`, observation | Mean over spectrum-valid points. | none |
| `spectral-flatness` | `ratio`, observation | Mean over spectrum-valid points. | none |

RMS, onset, and spectrum aggregation include joined inactive gap points. The
writer omits all three spectrum values when the segment has no spectrum-valid
point. An event therefore has either three or six values.

If no segment passes the rules, the bundle has no events and has one warning
with code `no-active-events` and message `No voiced span met the current level,
pitch confidence, and duration settings.` Full-window traces still exist when
`P` is positive.

## Determinism and limits

Version 1 uses no model, seed, random choice, or unordered reduction. It scans
points in order, assigns fixed trace IDs, and assigns event IDs in source
order. JSON uses the C numeric locale, and trace values use fixed little-endian
binary64 encoding. Runs with the same input path, input bytes, options, build,
and platform produce the same saved bytes. Version 1 does not promise
identical floating-point bytes across platforms or compiler math libraries.

The feature pass requires a sample rate from 8,000 through 768,000 Hz and at
most 1,024 analyzed channels. It budgets
`ceil(N / H)` track points, including its zero-filled tail points. With `C`
analyzed channels, it budgets `ceil(N / H) * (C + 2)` transforms. Select and
mix modes use `C = 1`; keep mode uses the input channel count. These checks can
fail even though the saved full-window count `P` is smaller.

The command also uses the fixed event-bundle limits. The main defaults are an
8 MiB manifest, 1 GiB for each JSONL index, 16 GiB for each payload, 64 GiB for
the whole bundle, and 1 GiB for retained decoded bundle data. Row caps are
4,096 audio rows, 10,000,000 events, 100,000,000 values, 100,000 traces,
100,000,000 trace links, 4,096 providers, and 100,000 warnings. The segment
list must also fit the 1 GiB bundle work cap. These limits have no command
flags in version 1.

## What version 1 does not do

This command targets a solo or mostly monophonic, voiced WAVE recording with
one dominant pitch at a time. It can save frame-level level, pitch, onset, and
three broad spectrum measures. It does not test whether two recordings match.

It does not perform source separation or polyphonic note tracking. It does
not infer note names, MIDI, rhythm, tempo, beats, bars, meter, key, voice,
part, MusicXML, LilyPond, or a score. It does not read score context.

It does not deconvolve a recording or estimate an impulse response. It does
not infer a physical cause such as bow force, string motion, material, body
mode, or room response. Its observations describe the recorded signal. They
do not prove why that signal occurred or that a model is physically accurate.
