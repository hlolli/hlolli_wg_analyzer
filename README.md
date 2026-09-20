# hlolli-wg-analyzer

`hlolli-wg-analyzer` is a C11 library and command-line tool for bounded,
read-only analysis of WAVE audio. It can inspect one recording, compare a
reference with a candidate, align audio, measure score-linked items, check
saved profiles, and build ranked reports for sound-model work.

The built-in signal analysis does not assume an instrument. Labels, roles,
physical elements, and probe names come from the input data. Model-backed
adapters may return the model's fixed label set.

Current version: 1.1.0.

## Build and test

You need a C11 compiler and CMake 3.20 or newer. The default library and CLI
have no third-party run-time dependency.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The CLI is `build/hlolli-wg-analyzer`. The build also creates the static
`hlolli_wg_analyzer_core` library.

For a strict build with warnings treated as errors:

```sh
cmake -S . -B build-strict \
  -DCMAKE_BUILD_TYPE=Release \
  -DHLOLLI_WG_ANALYZER_STRICT=ON \
  -DBUILD_TESTING=ON
cmake --build build-strict --parallel
ctest --test-dir build-strict --output-on-failure
```

Install the CLI, static library, and public header under a chosen prefix:

```sh
cmake --install build --prefix "$PWD/install"
```

## Quick start

```sh
build/hlolli-wg-analyzer inspect candidate.wav
build/hlolli-wg-analyzer --json inspect candidate.wav
build/hlolli-wg-analyzer compare reference.wav candidate.wav
build/hlolli-wg-analyzer --json compare reference.wav candidate.wav
build/hlolli-wg-analyzer export candidate.wav \
  --kind frames --output candidate-frames.csv
build/hlolli-wg-analyzer analyze-events candidate.wav \
  --output candidate.hwa-events
```

`inspect` and `compare` print text by default. Pass `--json` for JSON.
Unavailable JSON values are `null`, and the tool never writes NaN or infinity.
Exit code 0 means success, 1 means an input, analysis, or output error, and 2
means invalid command use.

Run `hlolli-wg-analyzer --help` for all options and limits.

## Commands

### Inspect and export

- `inspect` reports file format, levels, loudness, spectrum, activity, and
  stereo facts.
- `compare` analyzes two files and reports signed `candidate - reference`
  gaps.
- `export` writes frame tracks or a spectrogram as numeric CSV.

```sh
build/hlolli-wg-analyzer export candidate.wav \
  --kind spectrogram --output candidate-spectrum.csv
```

### Extract events and traces

```sh
build/hlolli-wg-analyzer analyze-events solo.wav \
  --output solo.hwa-events
build-onnx/hlolli-wg-analyzer infer-note-events piano.wav \
  --model nmp.onnx --output piano.hwa-events
build-onnx/hlolli-wg-analyzer separate-instruments quartet.wav \
  --model htdemucs_6s_fp16weights.onnx --output quartet-stems.hwa-events
build-onnx/hlolli-wg-analyzer infer-stem-note-events \
  quartet-stems.hwa-events --model nmp.onnx \
  --output quartet-notes.hwa-events
build/hlolli-wg-analyzer validate-event-bundle solo.hwa-events
build-onnx/hlolli-wg-analyzer --json inference-capabilities
```

`analyze-events` finds note candidates in solo or mostly monophonic voiced
audio. It saves sample bounds, level, pitch, onset, spectrum values, and nine
frame traces in a new directory. It does not infer rhythm, notation, parts, or
polyphonic voices. See [Analyze events version 1](docs/analyze-events-v1.md)
and [Event bundle version 1](docs/event-bundle-v1.md).

`infer-note-events` runs the opt-in Basic Pitch ONNX adapter on a mono 22050 Hz
WAVE file. It writes overlapping note events with exact source-sample bounds,
nominal equal-tempered pitch-bin centres, scores, and model provenance. It
does not measure fine tuning or infer rhythm, parts, instruments, or
ornaments.

`separate-instruments` runs the pinned HTDemucs six-stem ONNX adapter on a
mono or stereo 44100 Hz WAVE file. It writes stereo float32 WAVE stems named
`drums`, `bass`, `other`, `vocals`, `guitar`, and `piano`, with exact source
sample bounds and model provenance. Decoded input samples must be finite and
in `[-1, 1]`. The model is an estimate. In particular, the upstream six-stem
model calls its piano output experimental, and it does not split orchestral
families inside `other`.

`infer-stem-note-events` runs Basic Pitch once on each saved stem. It keeps
all note bounds on the original 44100 Hz clock and links each note to the
stem and instrument region that supplied its audio. It uses one fixed
downmix and downsample rule, then discards that work file. Stem bleed can
duplicate notes or cause false pitched events. See [Stem note events version
1](docs/stem-note-events-v1.md).

`inference-capabilities` reports whether the compiled CLI links ONNX Runtime
and contains each adapter. It does not find, open, or check a model file. The
default build does not link ONNX Runtime. See [ONNX Runtime build
option](docs/onnx-runtime-build.md).

The source-separation layer uses the checked instrument-stem provider
contract. It turns one source WAVE into clock-aligned stem payloads with
stable IDs, labels, hashes, and provenance. See
[Instrument stems version 1](docs/instrument-stems-v1.md).

### Export musical events

```sh
build/hlolli-wg-analyzer export-event-score notes.hwa-events \
  --format midi --output notes.mid
build/hlolli-wg-analyzer export-event-score notes.hwa-events \
  --format csound --output notes.sco
build/hlolli-wg-analyzer export-event-score notes.hwa-events \
  --format lilypond --tempo-bpm 120 --output notes.ly
build/hlolli-wg-analyzer export-event-score notes.hwa-events \
  --format musicxml --tempo-bpm 120 --output notes.musicxml
```

`export-event-score` reads selected `pitch-hz` note values from a checked
event bundle. Use `--source-id N` when the bundle has more than one source
recording. Each `(part, voice)` pair becomes a track. Unpitched notes are
omitted; invalid or ambiguous selected pitches cause an error. The command
leaves the bundle unchanged and refuses to overwrite an existing output.
`--output -` writes to stdout (binary for MIDI).

MIDI export needs no detected tempo or external tools. It writes a format-1
file at 32767 ticks per quarter and 500000 microseconds per quarter: a fixed
clock, not a tempo estimate. Sample bounds round to the nearest tick (about
15.26 microseconds), with ties up and a minimum duration of one tick. Pitch
rounds to the nearest A440 semitone in MIDI range 0–127. Velocity is fixed at
64; the export does not infer programs, pitch bends, key, meter, or ornaments.
It supports 15 tracks on distinct channels, skipping drum channel 10.
Same-key overlaps within a track cause an error, including overlaps caused
by rounding. A gap between MIDI events cannot exceed 268435455 ticks (about
68 minutes); each track must fit a 32-bit chunk length. These checks run
before writing any bytes.

Csound export keeps pitch in Hz and includes event IDs and sample bounds.
LilyPond export rounds pitches to semitones and timing to 128th notes at the
supplied tempo, using separate voices for overlaps.

MusicXML export writes an uncompressed MusicXML 4.0 partwise file using the
same timing grid. Each `(part, voice)` pair gets a part with one unmetered
measure; overlapping notes use separate voices. It does not infer bar lines,
key, instruments, or ornaments. Part labels use XML escaping; characters
that XML 1.0 forbids cause an error before any output. Pitch must round into
C0–G9 (MIDI 12–127). Note IDs use `E` followed by the event ID; comments keep
source sample bounds, pitch in Hz, and label bytes. This is a note-exchange
file, not a finished engraved score. The format uses
[MusicXML's unmetered time marker](https://www.w3.org/2021/06/musicxml40/musicxml-reference/elements/senza-misura/).

Keep the event bundle for exact values and information these output formats
omit.

### Align, segment, and measure

For one known note span, `note-phases` estimates attack, sustain, release, and
tail bounds without a score. It reports levels, spectral centroid, their rates
of change, and duration. An interrupted or unfinished tail has no usable
measurements. See [Note phases](docs/note-phases.md) for the JSON contract,
limits, and optional fitting objective.

```sh
build/hlolli-wg-analyzer note-phases solo.wav \
  --note-start-sample 44100 --note-end-sample 88200
```

```sh
build/hlolli-wg-analyzer align reference.wav candidate.wav \
  --output reference-candidate.hwa-align
build/hlolli-wg-analyzer align --score score.csv candidate.wav \
  --output candidate-score.hwa-align
build/hlolli-wg-analyzer align --score score.musicxml candidate.wav \
  --output candidate-xml.hwa-align
build/hlolli-wg-analyzer segment \
  --alignment candidate-score.hwa-align candidate.wav \
  --labels labels.csv --output candidate.hwa-items
build/hlolli-wg-analyzer measure \
  --items candidate.hwa-items candidate.wav \
  --output candidate.hwa-measures
```

`align` supports audio-to-audio and score-to-audio alignment. `segment` maps a
score alignment to checked item spans. `measure` derives per-item facts,
groups, and distributions.

Saved files keep method versions, source hashes, options, limits, and warnings.
Later commands require the caller to name source files again. They do not open
a path merely because an earlier artifact stored it.

The sample note manifest at `examples/note-manifest.csv` shows the accepted
score columns and event kinds.

### MusicXML score input

`align --score` accepts UTF-8 MusicXML `score-partwise` documents, compressed
`.mxl` files, and CSV manifests. It detects content, not filename extensions.
No conversion tool, XML/ZIP runtime dependency, or network access is needed.
The `.mxl` reader follows `META-INF/container.xml`, accepts stored and DEFLATE
members, checks CRCs and byte limits, and never extracts files to disk.

Use `import-score` to prepare structured score data without supplying audio:

```sh
build/hlolli-wg-analyzer import-score score.mxl --output score.json
build/hlolli-wg-analyzer import-score score.mxl --score-mode performance \
  --score-tempo-bpm 72 --output baseline.json
```

Output uses schema `hwa-musicxml-score`, version 1, with quarter-note time
units and `note`, `rest`, `grace`, `tempo`, `direction`, `control`, and `mark`
events. Positions and durations use full double precision. Each occurrence
keeps the source ID, written beat position/duration, measure ordinal, visit
number, part/voice/staff and XML byte span. Repeated IDs refer to the same
written source; the event index identifies a playback occurrence. Inapplicable
numeric output fields are `null`, not guessed values.

The reader handles parts, voices, staves, chords, rests, pickups, meter,
divisions changes, backup/forward, sound ties, chromatic transposition, and
tempo changes from `sound` or numeric metronome marks. It unfolds nested
repeats, numeric first/second endings, and measure-boundary D.C., D.S., coda
and fine jumps. Tempo returns to the written starting value when jumping.
Beat positions use
quarter-note units without the exporter's 128th-note grid. Pitch comes from
`pitch/alter`, not the printed accidental or key signature. Octave-shift
directions change engraving, not the pitch values already stored in MusicXML.
See the [MusicXML octave-shift definition](https://www.w3.org/2021/06/musicxml40/musicxml-reference/elements/octave-shift/).

If the score has no tempo at beat zero, alignment starts its score clock at
120 BPM and emits `musicxml_default_tempo`; this is a convention, not a tempo
estimate. `import-score --score-tempo-bpm N` sets the fallback; zero disables
it and leaves the score in beats without inventing a tempo. The memory reader
offers the same setting. `musicxml_written_timeline` reports that alignment uses timed notes
and rests, without turning grace notes, ornaments, dynamics, or pedal into
performed timing or controls. Parts keep distinct voice identities, and saved
matches identify their source measure, staff, and XML byte position. Existing
CSV behavior stays unchanged.

For browser/WASM callers, `hwa_musicxml_read()` accepts plain or compressed
bytes in memory and returns owned `HWAMusicXMLScore` data, including the
uncompressed `xml_data`. Event byte spans index this owned XML, so callers can
free the input buffer. Free the result with `hwa_musicxml_score_free()`.
`hwa_musicxml_read_file()` provides the native regular-file adapter. The
path-free portable build includes ZIP decoding and the same score reader.

Pedal controls use continuous MIDI values 0..127 and controller numbers 64
(damper), 66 (sostenuto), and 67 (soft). Explicit `sound` values win over the
displayed pedal mark. A pedal change emits ordered release/depress events.
Pedal controls do not extend note key-down durations; the player applies them.
Explicit note/sound dynamics preserve the MusicXML velocity convention.

Written mode is the default. `--score-mode performance` selects a baseline,
not a trained performer:

- Grace groups borrow explicit previous/following percentages when present,
  otherwise 1/8 of the following note (or the preceding note at a phrase end).
- Staccato uses half the note duration, staccatissimo a quarter, and accents
  raise the chosen velocity. Dynamics from pppp through ffff use a fixed table.
- Trills, mordents, turns and their inverted/delayed variants become note
  sequences. Trills default to eight notes per quarter beat; explicit `beats`
  and `trill-step` values take precedence. Baseline spacing is even.

Use `--score-grace-fraction`, `--score-trill-rate`, `--score-staccato-ratio`,
and `--score-default-velocity` to change these rules. Output records the policy.
Event `interpretation` is a bit mask: 1 means a baseline choice, 2 means explicit
XML playback data. Written positions remain separate from these choices.
`unrendered_marks` reports marks/attributes without a playback rule, including
grace `make-time`, ornament acceleration/uneven spacing/terminal turns,
hairpins, and free text. Their XML remains available; no performer-specific
interpretation is claimed.

This is a written-score reader, not a complete MusicXML player or an XML schema
validator. It rejects timewise scores, namespaced elements, cue/unpitched notes,
multi-measure rests, staff-specific or doubled transposition, and note playback
overrides. Navigation requires measure-boundary signs, matching parts and
closed numbered endings (1..32); sound `time-only`, numeric fine durations,
and `after-jump` repeats still require an unfolded input. ZIP64, encryption,
multi-disk archives and other compression methods are not accepted.
The memory reader retains
microtones, but the current alignment track requires integral MIDI pitches.
XML depth, node, event, measure-visit, input-byte, and work-byte limits bound parsing and expansion.
The `--score-max-*` import flags expose byte, work, event and visit limits. Internal
DTD/entity declarations are rejected; external DOCTYPE identifiers are accepted
without fetching them.

### Check isolated and harmonic decay

```sh
build/hlolli-wg-analyzer isolated-note note.wav \
  --expected-hz 196 --metrics pitch,passive-decay
build/hlolli-wg-analyzer --json harmonic-decay reference.wav model.wav \
  --expected-hz 196
```

`isolated-note` checks pitch and the whole-note passive tail.
`harmonic-decay` measures separate harmonic T60 values with fixed
`harmonic-decay-1` rules. It uses mean channel power, rejects a second pluck,
checks band noise and fit support, and needs at least four valid harmonics.
With two files it reports the shared harmonic count, coverage, median bias,
and RMS log-T60 error. Static gain does not change the slopes. Long room or
body modes still can, so this command is a diagnostic rather than a fit rule.

### Compare profiles and check evidence

```sh
build/hlolli-wg-analyzer compare-measures \
  reference.hwa-measures candidate.hwa-measures \
  --output reference-candidate.hwa-compare
build/hlolli-wg-analyzer check-physical \
  reference.hwa-measures candidate.hwa-measures \
  --output reference-candidate.hwa-physical
build/hlolli-wg-analyzer account-production \
  reference.hwa-measures reference.wav \
  candidate.hwa-measures candidate.wav \
  --output reference-candidate.hwa-production
```

`compare-measures` compares matched saved distributions. `check-physical` can
add WAVE evidence through explicit `--bind` roles. `account-production`
computes checked views of level, spectrum, dynamics, stereo, and optional room
evidence. It does not create corrected audio.

### Runs, experiments, and reports

```sh
build/hlolli-wg-analyzer analyze-run run.json \
  --bind reference=reference.wav \
  --bind candidate=candidate.wav \
  --output analysis.hwa-run
build/hlolli-wg-analyzer rank report.json \
  --bind reference=reference.wav \
  --bind candidate=candidate.wav
build/hlolli-wg-analyzer report report.json \
  --bind reference=reference.wav \
  --bind candidate=candidate.wav \
  --output report
```

Binding names must match IDs in the manifest. `analyze-run` checks declared
stems and probes. `rank` prints ranked gaps. `excerpt` creates checked
listening clips. `report` creates JSON, CSV, HTML, and any requested clips in a
new directory.

For one isolated pitched note, request pitch, passive decay, or both:

```sh
build/hlolli-wg-analyzer isolated-note note.wav \
  --expected-hz 41.20344461410875 \
  --metrics pitch,passive-decay
```

The JSON result reports requested and valid metric masks, named rejection
reasons, sample bounds, WAVE facts, and bounded work counts. A rejected metric
is a checked result, not an I/O error.

`experiment` runs a named renderer for a bounded parameter sweep:

```sh
build/hlolli-wg-analyzer experiment experiment.json \
  --renderer ./renderer --allow-run \
  --bind reference=reference.wav \
  --output experiment-results
```

The command starts the renderer only when both `--renderer` and `--allow-run`
are present.

`tools/instrument_fit.py` adds one checked selection step above an experiment.
It reads the saved `result.hwa-experiment`, joins fit and validation scores, and
can add a pitch-conditioned body-shape score or a gain-independent passive
decay score. Instrument adapters own rendering and any profile rules. The
selector owns neither Csound nor a model profile. The
[violin fit adapter](https://github.com/hlolli/hlolli_wg_violin/tree/main/tools/analyzer_adapter)
lives in the violin repository and uses this shared path. The
[cello open-string adapter](https://github.com/hlolli/hlolli_wg_cello/tree/main/tools/analyzer_adapter)
lives in the cello repository with its manifests, diagnostic tools, and tests. The
[viola passive-string adapter](https://github.com/hlolli/hlolli_wg_viola/tree/main/tools/analyzer_adapter)
lives in the viola repository with its manifests, diagnostics, and tests. The
[double-bass adapter](https://github.com/hlolli/hlolli_wg_double_bass/tree/main/tools/analyzer_adapter)
lives in the double-bass repository, including its fit manifests and tests.
It takes explicit paths to this repository's shared tools and analyzer binary.
See [Shared string-instrument modeling](docs/string-instrument-modeling.md) for
the violin, viola, cello, and double-bass work split.

```sh
python3 tools/instrument_fit.py select \
  --manifest instrument-fit.json \
  --experiment experiment-results/result.hwa-experiment \
  --analyzer build/hlolli-wg-analyzer \
  --profile model-profile.json \
  --bind reference_body_fit=/private/path/fit.wav \
  --bind reference_body_check=/private/path/check.wav \
  --output fit-result.json
```

The score uses measured gaps, body-shape error, and passive-decay error named
by the fit manifest. A version 1 manifest may set `check_weight` to zero so
validation remains an eligibility gate without affecting rank or tie-breaks.
Corpus objectives may also name a `source_group`; optional source-mean caps
then prevent a heavily sampled player or cello from dominating selection.
File hashes only bind the inputs and outputs to the receipt. They do not score
the sound. `write-profile` writes a new file, asks
the adapter to validate it, and writes a receipt. It never replaces the source
profile. A render-only adapter has empty `profile_paths`; the writer rejects
it. A version 1 manifest may instead declare `selection.mode` as `fit-only`; that
mode accepts only fit objectives, exact fit bindings, and zero check weight and
limits. Its baseline remains an eligible answer, so a search is not forced to
change a sound parameter. The `checked-note-harmonic-decay` objective binds a
reference SHA-256, requires native `isolated-note-1` pitch validity, and scores
native `harmonic-decay-1` T60 evidence rather than an experiment RMS response.
Fit-manifest version 2 verifies one fixed candidate against fit, validation,
and audit goals. Audit data cannot tune that candidate. The writer accepts it
only after all saved and recomputed gates pass.

`check-recordings` runs the body-envelope estimator on bounded excerpts of
real recordings. Keep private recordings outside the repository and pass each
one with `--recording`.

Both body-envelope fit scores and recording checks use the shared evidence
checker in `tools/analyzer_evidence.py`. It binds each run to the analyzer and
audio hashes, checks the default report grid and support counts, and verifies
comparison scores against their points. Low-support reports remain readable;
each caller decides whether they meet its needs. The body-envelope curve still
includes effects from the room, microphone, strings, and playing.

## Supported input

The built-in reader accepts seekable, little-endian RIFF/WAVE and RF64 files.
It supports:

- PCM with 8, 16, 24, or 32-bit containers
- IEEE float with 32 or 64-bit samples
- Plain and WAVE extensible format chunks
- 1 to 1,024 channels
- Sample rates from 8,000 through 768,000 Hz

The reader checks chunk sizes, frame layout, RF64 metadata, channel masks,
finite float samples, and integer overflow before allocation. It does not
accept RIFX, compressed WAVE, FLAC, AIFF, or CAF.

`inspect` and `export` accept `-` as standard input. `compare` accepts at most
one standard-input source. The tool copies standard input to an unnamed,
size-limited temporary file because WAVE parsing needs seeks. Artifact commands
require named files.

Use `--channel N` to select one 1-based channel or `--mixdown` for an equal
mono mix. With neither option, analysis keeps all channels.

## Limits

The CLI sets bounds for input bytes, decoded frames, work memory, transform
count, track rows, and command-specific work. Common options include:

```text
--max-bytes N
--max-frames N
--max-work-bytes N
--max-transforms N
--max-track-points N
--max-spectrum-values N
```

Later commands have their own `--max-*` limits. See `--help` for the full set.
The code checks size products before allocation. A work-byte limit covers the
main tracked buffers, not all process memory or allocator overhead.

## Input and output safety

The tool opens named inputs read-only. It never edits, renames, or deletes
them. `inspect` and `compare` create no side files.

File outputs use exclusive creation by default and reject an existing path.
`--replace` permits replacement of a regular file after the tool writes and
closes a same-directory temporary file.

Experiment, excerpt, report, `analyze-events`, `infer-note-events`,
`separate-instruments`, and `infer-stem-note-events` outputs must be new
directories. They do not support `--replace`. Event-bundle writers create
the final directory and remove the paths they made after a handled write or
check failure. A stopped process or storage failure can leave a partial new
directory.

The CLI rejects outputs that alias protected inputs, including hard links.
POSIX builds also reject output symlinks. Commands that build saved artifacts
hash their explicit inputs and check them again while they write output.
These checks handle common path mistakes and input changes, but they do not
protect against an active local attacker racing file-system operations.

The program has no network client or plug-in loader. Only `experiment` starts
an external process, and only with explicit renderer authority.

## C API

The public header is `include/hlolli_wg_analyzer.h`. It supports C and C++
callers. This C program analyzes a named WAVE file:

```c
#include <stdio.h>

#include <hlolli_wg_analyzer.h>

int main(void)
{
    HWAAnalysis analysis;
    char error[HWA_ERROR_SIZE];

    if (hwa_analyze_wav("candidate.wav", &analysis,
                        error, sizeof(error)) != 0) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }

    printf("duration: %.3f seconds\n", analysis.format.duration_seconds);
    if (analysis.loudness.integrated_valid) {
        printf("loudness: %.2f LUFS\n", analysis.loudness.integrated_lufs);
    }

    hwa_analysis_free(&analysis);
    return 0;
}
```

After a local install, compile it on a Unix-like host with:

```sh
cc -std=c11 example.c \
  -Iinstall/include -Linstall/lib \
  -lhlolli_wg_analyzer_core -lm -o example
```

Use `hwa_analysis_options_default()` with
`hwa_analyze_wav_with_options()` to set channel, frame, feature, and work
limits. Hosts without native paths can provide a bounded, seekable
`HWAByteSource` to `hwa_analyze_wav_source()`.

A successful analysis owns its copied path and result arrays. Call
`hwa_analysis_free()` before reuse. The header provides matching option,
operation, and free functions for alignment, segmentation, measurement,
profile comparison, physical checks, production views, run analysis,
experiments, and reports.

## Extra checks

The main build has switches for strict warnings, benchmarks, malformed-input
regression tests, and libFuzzer targets:

```text
HLOLLI_WG_ANALYZER_STRICT
HLOLLI_WG_ANALYZER_BUILD_BENCHMARKS
HLOLLI_WG_ANALYZER_BUILD_REGRESSION_TESTS
HLOLLI_WG_ANALYZER_BUILD_FUZZERS
```

Installed-client tests, sanitizer runs, and the path-free WASI core add wider
development coverage. See `docs/testing.md` and the files under `bench/`,
`regression/`, `fuzz/`, and `portable/` for commands.

## Scope

The analyzer reports measured facts and explicit gaps. It does not prove a
physical cause, infer the meaning of caller labels, judge artistic quality,
create a dry source, or make a corrected master.

See `docs/development.md` for the source layout and compatibility rules.

## License

Copyright (c) 2026 Hlöðver Sigurðsson.

This project is licensed under the MIT License. See [LICENSE](LICENSE).
The Basic Pitch decoder port retains Spotify's Apache-2.0 license and notice
under [third_party/basic-pitch](third_party/basic-pitch).
The HTDemucs model code and its ONNX conversion retain their MIT notices under
[third_party/demucs](third_party/demucs) and
[third_party/demucs-onnx](third_party/demucs-onnx).
