# Viola fit adapter

This adapter builds one local bundle for one open viola string. Each bundle
varies only that string's passive loss time. It keeps the public Csound opcode
and fixed model format unchanged.

## Controlled recording intake

`recording_session.py` is a read-only intake gate for future private recording
evidence. It does not copy audio, print paths, write a receipt, or read or alter
a viola model. Run it with an absolute manifest path:

```sh
python3 -B adapters/hlolli_wg_viola/recording_session.py validate \
  --manifest /private/viola-session/manifest.json
```

The strict `hwa-viola-recording-session` version 1 object has these fields:

```json
{
  "schema": "hwa-viola-recording-session",
  "schema_version": 1,
  "session_id": "viola-session-01",
  "source_family": "controlled-viola-01",
  "split": "check",
  "recording_setup": {
    "instrument_id": "viola-01",
    "bow_id": "bow-01",
    "performer_id": "performer-01",
    "room_id": "room-01",
    "recorder_id": "recorder-01",
    "audio_interface_id": "interface-01",
    "clock_source": "interface-internal"
  },
  "tuning": {
    "a4_hz": 440.0,
    "reference": {
      "pitch": "A4",
      "frequency_hz": 440.0,
      "measurement_method": "strobe-tuner-01"
    }
  },
  "strings": [
    {"id": "c", "open_pitch": "C3", "length_m": 0.4, "measurement_method": "steel-rule-01"},
    {"id": "g", "open_pitch": "G3", "length_m": 0.4, "measurement_method": "steel-rule-01"},
    {"id": "d", "open_pitch": "D4", "length_m": 0.4, "measurement_method": "steel-rule-01"},
    {"id": "a", "open_pitch": "A4", "length_m": 0.4, "measurement_method": "steel-rule-01"}
  ],
  "channels": [
    {"id": "bridge-mic", "role": "instrument", "transducer_id": "mic-01", "input_id": "input-01"}
  ],
  "source_files": [
    {
      "id": "source-01",
      "path": "audio/source-01.wav",
      "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      "layout": {
        "sample_rate_hz": 96000,
        "bits_per_sample": 24,
        "frame_count": 960000,
        "channel_ids": ["bridge-mic"]
      }
    }
  ],
  "takes": [
    {
      "id": "pizz-c-01",
      "kind": "passive-pizzicato",
      "file_id": "source-01",
      "start_frame": 0,
      "frame_count": 96000,
      "string_id": "c",
      "pitch": "C3"
    },
    {
      "id": "arco-g-01",
      "kind": "steady-arco",
      "file_id": "source-01",
      "start_frame": 96000,
      "frame_count": 96000,
      "string_id": "g",
      "pitch": "G3",
      "articulation": "sustain",
      "bow": {
        "force_n": 1.0,
        "speed_m_per_s": 0.2,
        "bridge_distance_m": 0.04,
        "position_ratio": 0.1,
        "direction": "down-bow",
        "measurement_methods": {
          "force": "load-cell-01",
          "speed": "motion-track-01",
          "bridge_distance": "steel-rule-01"
        }
      }
    },
    {"id": "room-01", "kind": "room-tone", "file_id": "source-01", "start_frame": 192000, "frame_count": 96000},
    {"id": "sync-01", "kind": "sync", "file_id": "source-01", "start_frame": 288000, "frame_count": 9600}
  ],
  "processing": []
}
```

IDs and object fields are closed and duplicate-free. Source paths are
normalized relative POSIX paths beneath the manifest directory. Every source
must be a regular non-symlink RIFF/WAVE file whose exact bytes match its
SHA-256 and declared frame/channel layout; only nonempty 96 kHz integer PCM24
is accepted. Take frame regions must stay inside their source. Passive takes
have string and pitch fields but no bow object. Steady arco takes also have a
named articulation and positive physical bow measurements; force is at most
100 N, speed at most 20 m/s, and bridge distance is below the measured string
length. `position_ratio` must equal `bridge_distance_m / length_m` within
`1e-9` relative or `1e-12` absolute tolerance. Bow direction is `up-bow` or
`down-bow`; every force, speed, and bridge-distance method is named.

Success prints one deterministic, path-free JSON line containing the exact
manifest-byte SHA-256, channel/source/take counts (including take kinds), and
the accepted PCM layout. Failure prints no path and exits nonzero. Source-file
inspection is cached for repeated bindings. The validator writes nothing.

The four target contracts are separate:

| Target | Model path | Render string | Iowa fit pitch | Iowa fit A4 |
| --- | --- | ---: | ---: | ---: |
| C3 | `strings[0].loss_time_constant_seconds` | 1 | 130.58241487372985 Hz | 439.22513824997105 Hz |
| G3 | `strings[1].loss_time_constant_seconds` | 2 | 195.58538558422308 Hz | 439.0743450444911 Hz |
| D4 | `strings[2].loss_time_constant_seconds` | 3 | 292.96845778349154 Hz | 438.95671359865264 Hz |
| A4 | `strings[3].loss_time_constant_seconds` | 4 | 441.08823856808476 Hz | 441.08823856808476 Hz |

The table's pitches come from the fixed Iowa pizzicato fit cuts in the Stage 3
source receipt. Each roster case has its own measured pitch. The adapter sets
the constructor value to `440 * case pitch / nominal open pitch` and renders
that same pitch. The probe checks that the tuned open string and render pitch
agree. Cases do not share tuning.

The current C, G, D, and A loss baselines are 1.15, 1.90, 0.85, and 0.45
seconds. A 2026-09-03 cross-corpus experiment chose them as perceptual
whole-tail duration compromises, not as measurements of physical string loss.
The older Stage 3 receipts and their frozen bundles retain the former
0.25-second baseline and remain valid historical runs. New bundles use the
current model values above.

For each scalar search job, the frozen renderer:

1. checks every fixed tool, source, model, schema, score, and Csound header;
2. writes the candidate value to a scratch copy of `viola-v1.json`;
3. runs the fixed-model generator on a scratch C source;
4. compiles a private test module;
5. creates the viola with the target's numeric A4 value;
6. seeds the named open-string rail and renders only its dry passive tail; and
7. removes the scratch tree after it writes the one declared WAV.

The fit and check WAV files stay outside both repositories and outside Git.
The bundle binds them by path and SHA-256 but does not copy them. Keep the
bundle outside Git too because its renderer holds local tool paths.

## Selection roster

Use `--roster` for a real search. The roster is a private JSON file with this
exact shape:

```json
{
  "schema": "hwa-viola-passive-tail-roster",
  "schema_version": 1,
  "target": "g3",
  "cases": [
    {
      "id": "iowa2012-g3-pizz-ff-open",
      "source_family": "iowa-2012",
      "split": "fit",
      "path": "/private/viola/iowa-g3.wav",
      "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      "frequency_hz": 195.58538558422308
    },
    {
      "id": "rwc-v1-g3-pizz-f-open",
      "source_family": "rwc-variation-1",
      "split": "fit",
      "path": "/private/viola/rwc-v1-g3.wav",
      "sha256": "123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0",
      "frequency_hz": 195.7
    },
    {
      "id": "best-music-tools-g3-a442-event02",
      "source_family": "best-music-tools-a442",
      "split": "check",
      "path": "/private/viola/g3-a442.wav",
      "sha256": "23456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef01",
      "frequency_hz": 195.8
    }
  ]
}
```

The roster lists accepted passive tails only. Keep rejected-tail evidence in
the private analysis receipt, not in this file. Use these exact family and
split pairs:

| Source family | Split | Total objective weight |
| --- | --- | ---: |
| `iowa-2012` | `fit` | 0.5 |
| `rwc-variation-1` | `fit` | 0.5 |
| `rwc-variation-2` | `check` | 1.0 for C, D, and A |
| `best-music-tools-a442` | `check` | 1.0 for G |

The adapter divides each total by that family's case count. The fit score then
gives Iowa and RWC variation 1 equal weight. The selector's check weight is
0.5, so RWC variation 2 has the same final coefficient as either fit family.

The checked Stage 3 receipts fix these case counts:

| Target | Iowa 2012 fit | RWC variation 1 fit | RWC variation 2 check | BEST MUSIC TOOLS check |
| --- | ---: | ---: | ---: | ---: |
| C3 | 1 | 3 | 1 to 3 | 0 |
| G3 | 1 | 1 | 0 | 1 |
| D4 | 1 | 2 | 1 to 3 | 0 |
| A4 | 1 | 2 | 1 to 3 | 0 |

The builder rejects a missing or extra fit tail and blocks a target with no
allowed check tail. G requires exactly one `best-music-tools-a442` tail; the
other targets require one to three RWC variation-2 tails. It also rejects
unknown fields, duplicate JSON keys, duplicate case IDs, duplicate audio
hashes, wrong family splits, relative paths, wrong hashes, and bad WAVE
layouts. The roster and every WAVE path must be absolute and resolve outside
the analyzer and viola repositories. The selection output directory must stay
outside those repositories too. The builder hashes the exact bounded roster
bytes, parses those same bytes, and checks the path again after parsing. It
gets each WAVE hash and layout from one streamed read, then checks the roster
and every recording once more before publish.

Build a selection bundle with the roster and explicit local tools:

```sh
python3 -B adapters/hlolli_wg_viola/adapter.py build \
  --roster /private/path/g3-passive-roster.json \
  --viola-root /path/to/hlolli_wg_viola \
  --csound /path/to/csound \
  --csound-library /path/to/libcsound64 \
  --c-compiler /path/to/cc \
  --python /absolute/path/to/python3 \
  --csound-include-dir /path/to/csound/include \
  --output-dir /private/path/viola-g3-passive-bundle
```

`--roster` cannot be combined with `--target`, a single reference, or a
single-reference frequency. It also rejects `--sample-count`. Roster searches
use this fixed loss-time grid in seconds:

```text
0.10 0.15 0.25 0.35 0.45 0.55 0.70 0.85 1.00 1.15 1.30
1.45 1.60 1.75 1.90 2.05 2.20 2.50 3.00 4.00 5.00
```

Fit-side decay times set the range before the check search. A real C-string
render preflight then rejected 0.02 and 0.05 seconds because each gave less
than the scorer's fixed 0.20 seconds of tail support. The 0.10-second render
passed with 0.269 seconds of support, 35.66 dB of range, and 1.12 dB line
residual. This keeps every search point valid without changing the shared
decay rule.

The output adds a sorted `roster.json` to the usual `experiment.json`,
`fit.json`, `bindings.json`, `renderer`, and `receipt.json`. The experiment,
fit manifest, bindings, frozen renderer cases, and receipt all come from the
checked roster.

Roster selection also fixes objective loss at 2 or less, model/reference T60
from 0.5 through 2, model/reference support at 0.5 or more, and no rise in mean
check loss from the baseline. The baseline sets the check limit but cannot be
chosen. A search with no accepted nonbaseline point writes a failed result and
cannot write a profile.

## Joint passive diagnostic

After the four scalar searches, use `--joint-passive-diagnostic` with one
checked roster at a time. This mode renders the 21 loss times above at each
common nut-and-bridge cutoff scale in `0.5 1 2 4`. The fixed grid has 84
points. It does not select a point or write a model profile.

```sh
python3 -B adapters/hlolli_wg_viola/adapter.py build \
  --roster /private/path/g3-passive-roster.json \
  --joint-passive-diagnostic \
  --viola-root /path/to/hlolli_wg_viola \
  --csound /path/to/csound \
  --csound-library /path/to/libcsound64 \
  --c-compiler /path/to/cc \
  --python /absolute/path/to/python3 \
  --csound-include-dir /path/to/csound/include \
  --output-dir /private/path/viola-g3-joint-diagnostic
```

For a standalone build, the published path defaults to `--output-dir`. An
outer atomic publisher must also pass the final bundle path:

```sh
  --output-dir /private/path/.r2.prepare/bundles/g3 \
  --published-output-dir /private/path/r2/bundles/g3
```

`--published-output-dir` is joint-only. It must be a new canonical absolute
path outside both repositories and have the same last path part as
`--output-dir`. A distinct published path cannot sit below the build path.
The builder records the canonical build parent and its file identity, then
checks the parent and absent build path again before its own rename. Before
the outer rename, check `receipt.json` as follows:

1. `build_output_dir` equals the canonical staging bundle;
2. `published_output_dir` equals the canonical final bundle;
3. `diagnostic_module_path` equals the final bundle plus
   `diagnostic_module_name`; and
4. the regular, non-link staging module with that name has the SHA-256 in the
   `diagnostic_module` resource row.

Do not call the frozen renderer's `--describe` before the outer rename. It
checks the final path and has no staging fallback. After the rename, require
`--describe` from a copied renderer to report and rehash the final module
before running a job.

The builder copies the checked canonical source into its private staging
directory and compiles one module with `HLOLLI_WG_VIOLA_TEST_API=1`. Every
job reuses that module. Jobs do not run the model generator or compiler. The
renderer binds the module's final absolute bundle path and hash, so analyzer
copies of the renderer still use the checked module. It uses
`tests/fit_passive_joint.csd` with
`FIT_PASSIVE_TAU`, `FIT_CUTOFF_SCALE`, and `FIT_TOTAL_SECONDS=16`. It checks
the probe's `WG_PASSIVE_JOINT_FACTS` line and accepts only a stereo 44.1 kHz
PCM24 file with exactly 705,600 frames.

The bundle contains `experiment.json`, `bindings.json`, `roster.json`,
`receipt.json`, `renderer`, and the test module. It has no `fit.json`. Run the
grid with every binding from `bindings.json`:

```sh
/path/to/hlolli-wg-analyzer \
  --max-experiment-run-evaluations 4000000000 \
  experiment /private/path/viola-g3-joint-diagnostic/experiment.json \
  --renderer /private/path/viola-g3-joint-diagnostic/renderer \
  --allow-run \
  --bind iowa2012-g3-pizz-ff-open=/private/path/iowa-g3.wav \
  --bind rwc-v1-g3-pizz-f-open=/private/path/rwc-v1-g3.wav \
  --bind best-music-tools-g3-a442-event02=/private/path/g3-a442.wav \
  --output /private/path/viola-g3-joint-results
```

Keep the higher run-visit cap in the saved command. Sixteen-second jobs can
exceed the default two-billion total. One three-case G roster makes 252
renders and about 0.994 GiB of raw PCM24 WAVE files. The checked four-roster,
20-case set makes 1,680 renders and 7,112,521,920 bytes, or about 6.624 GiB,
before other result files.

Use these renders only to inspect the joint grid. Do not pass this bundle to
selection or profile writing. After choosing fixed values with a named test,
build the fixed model and rerender every selected candidate against its fit
and check tails.

## Smoke and legacy modes

The two-reference command remains for render smoke tests. It writes `smoke`
as the bundle and renderer mode. Do not use it for a real selection:

```sh
python3 -B adapters/hlolli_wg_viola/adapter.py build \
  --target g3 \
  --viola-root /path/to/hlolli_wg_viola \
  --csound /path/to/csound \
  --csound-library /path/to/libcsound64 \
  --c-compiler /path/to/cc \
  --python /absolute/path/to/python3 \
  --csound-include-dir /path/to/csound/include \
  --reference-fit /private/path/g3-fit.wav \
  --reference-check /private/path/g3-check.wav \
  --fit-frequency-hz "$FIT_HZ" \
  --check-frequency-hz "$CHECK_HZ" \
  --output-dir /private/path/viola-g3-passive-bundle
```

Targets are `c3`, `g3`, `d4`, and `a4`. If `--target` is absent, the generic
options select C3. Both frequency options are required on the generic path.
Each value must map to a numeric A4 from 380 through 480 Hz.

The old `--reference-c3-fit` and `--reference-c3-check` options still build
the C3 contract and write `legacy` as the mode. That path uses the published
Iowa pitch for both cases because it has no tuning options. Keep it for old
bundle checks only.

References may use integer PCM16 or PCM24, one or two channels, at 44.1 kHz.
The scalar and smoke renderers write a four-second, non-silent, stereo
44.1 kHz PCM24 `model.wav`. The joint diagnostic writes the 16-second file
described above. The roster renderer accepts every frozen case and rejects a
case, split, binding, path, channel count, or hash that differs from the bundle.
Each case binds the resolved source path as well as its hash, so another copy
of the same bytes cannot replace it. Render-job JSON is parsed from the exact
bounded bytes whose SHA-256 the renderer checks throughout the job.
Scalar and smoke bundles pass `experiment.json`, `renderer`, `fit.json`, and
`bindings.json` to the analyzer. Joint diagnostic bundles have no `fit.json`.

The bundle hashes the Csound executable and its named core library. At build
time, it also checks the library path reported by the Darwin loader or `ldd`
on Linux. The current builder supports Darwin and Linux toolchains.

## Current limit

The fit manifest scores aligned decay shape and ignores gain, polarity, and
leading silence. RWC variation 2 supplies accepted C, D, and A check tails.
All three RWC variation 2 G tails fail the fixed 20 dB range gate and stay
rejected. The accepted Best Music Tools A442 open-G tail supplies the one G
check for the older directional diagnostic. One fit and one check tail would
still be insufficient for a physical G-loss claim; another controlled G tail
would be needed before making that claim. The 2026-09-03 fixed-model update
instead used a separately frozen perceptual whole-tail criterion with equal
Iowa, RWC, and OrchideaSOL corpus weight, and it makes no physical-loss claim.
RWC variation 3 stays sealed. The scalar adapter writes one chosen string
value at a time. The joint diagnostic does not choose or write values.
