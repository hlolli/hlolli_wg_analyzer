# Note phases

`note-phases` measures four windows around a caller-supplied note span. It
uses the same boundary search as score-linked segmentation and the same
measurement engine as `measure`. It needs no score or instrument adapter.

```sh
build/hlolli-wg-analyzer note-phases solo.wav \
  --note-start-sample 44100 --note-end-sample 88200 \
  --tail-limit 4 > phases.json
```

Both sample hints are required: approximate onset and the end of the note's
body. They use the source sample rate. The command refines them within the
boundary search window, which defaults to 0.15 seconds. For a plucked note,
the end hint selects a point in the decay; it is not a known player release.
The input must be a seekable WAVE file. Output is always JSON.

## Output

The report uses schema `hwa-note-phases`, version 1, method `note-phases-1`.
It includes the source path and SHA-256, sample rate, frame count, input hints,
both analysis grids, and the next detected onset, or `null`.

The four rows always appear in this order:

| Phase | Window |
| --- | --- |
| `attack` | Estimated attack start to body start |
| `sustain` | Body start to estimated release start; level need not stay flat |
| `release` | Release start to the estimated direct-sound end |
| `clean-tail` | Direct-sound end to the tail floor or search limit |

Each row gives integer `start_sample` and exclusive `end_sample`, duration in
seconds, boundary confidence, status, and these six measurements:

| Metric | Unit |
| --- | --- |
| `rms_dbfs` | `dBFS` |
| `peak_dbfs` | `dBFS` |
| `level_slope_db_per_second` | `dB/s` |
| `centroid_hz` | `Hz` |
| `centroid_slope_hz_per_second` | `Hz/s` |
| `duration_seconds` | `seconds` |

Each measurement has its own status, value, confidence, and native measurement
quality flags. Rejected values are `null`. A usable window can still lack a
particular measure. Check both statuses before using a value. Numeric output
uses 17 significant digits; integer bounds retain the source sample clock.

## Optional measurements

`--phase-envelope` adds `crest_db`, `level_modulation_spread_db`, and
`centroid_modulation_spread_hz` to each phase. The spreads are frame standard
deviations and include trends. The attack also gains `rise_10_seconds`,
`rise_50_seconds`, `rise_90_seconds`, `attack_slope_db_per_second`, and
`attack_overshoot_db`. Rise times use 16 RMS bins and start at the estimated
attack onset.

`--phase-frames` adds `frame_series`. Each row gives level, centroid, and
flatness, with sample bounds and flags for crossing a phase boundary or
padding at the end of the file. The center sample assigns each row to a valid
phase. The measurement grid starts at source sample zero and uses a symmetric
Hann window on the arithmetic mean of the channels. Set the window and hop
with `--measure-fft-size` and `--measure-hop-size`.

`--phase-spectra` includes those rows and adds `bin_powers`, from DC through
Nyquist, in full-scale squared. The report records the bin spacing and
normalization. Raw powers include bins below the spectral floor.

`--phase-partials` includes the frame rows and adds linked spectral peaks.
Each point gives the track ID, bin index, interpolated frequency in Hz, bin
power, and whether it continues a track from the previous frame. The tracker
links stronger peaks first and starts a new track after a gap. Crossings or
merged peaks can change which track follows a partial.

`--max-partials` sets the peak cap per frame (default 12, maximum 32).
`--spectral-floor-dbfs` sets the absolute peak floor (default −100 dBFS).
`--partial-relative-floor-db` sets the floor below the frame's strongest bin
(default −30 dB), and `--partial-link-cents` sets the maximum frequency step
(default 100 cents). Partial tracking uses the raw spectra internally. Add
`--phase-spectra` if you also need the bin arrays in the report.

## What the labels mean

Phase status is `valid`, `no-signal`, `too-short`, `interrupted`, or `truncated`.
`valid` means the window passed these signal checks, not that the analyzer
knows the player's action. Boundary confidence is a heuristic, not a calibrated
probability. The phase names describe estimates from the recording.

The boundary pass defaults to 2048-sample frames and a 512-sample hop. The
measurement pass defaults to a 4096-sample FFT and a 256-sample hop. These are
separate settings. Small hops do not give small FFT windows better frequency
resolution. Frame-based measures use the measurement engine's window support,
which can cross a phase boundary; its quality flags remain in the report.
RMS and peak use samples within the reported span. Channel handling follows
the existing engines; this is not a spatial or per-channel measurement.

The existing tail search uses the body peak and the silence threshold. It
looks for two frames 12 dB below the body peak for the direct-sound end, and
three frames 30 dB below it for the tail end. The silence threshold bounds
both floors. A fallback direct-sound end has lower confidence.

The added interruption check scans from the supplied end hint through the
tail window. An active frame with combined onset strength at least 0.35 and
either energy onset strength at least 0.12 or pitch-change strength at least
0.5 marks a new onset. It skips the first hop after the hint. Release or tail
windows that overlap that onset return no usable measures. A tail that ends
above the floor also returns no usable measures. `--tail-limit` defaults to
1.5 seconds; longer decays may need more time.

This check can miss weak, overlapping, or same-pitch entries. A late end hint
can hide an entry before the scan starts. Vibrato, handling noise, or a cough
can trigger the detector. `clean-tail` does **not** mean room-free,
noise-free, or source-separated. These measures cannot identify an instrument's
physical parts or separate its response from the room and microphone.

Analysis, segmentation, and measurement keep their existing work limits.
The analysis tracks are freed before measurement; the limits apply to each
pass, not to a single combined allocation budget. Hash checks reject a source
that changes during the run.

The public C API is `hwa_analyze_note_phases_wav`, with
`hwa_note_phase_options_default` and `hwa_note_phase_result_free`. Initialize
options, set both sample hints, call the analyzer, and free a successful result.
The result owns its path. Failed calls clear the result. The envelope, frame,
and spectra variants have their own result and free functions. Use
`hwa_track_note_phase_partials` to link peaks from a spectra result.

## Optional fit objective

Fit-manifest v1 accepts this objective alongside its existing objectives:

```json
{
  "id": "sustain-level",
  "kind": "note-phase",
  "phase": "sustain",
  "metric": "rms_dbfs",
  "reference_span": [3200, 19200],
  "model_span": [3200, 19200],
  "case": "c-fit",
  "reference_binding": "c_fit",
  "resource_id": "model.final",
  "split": "fit",
  "weight": 1,
  "scale": 6
}
```

Spans use each file's own sample rate and must fit every candidate. The loss
is the absolute metric difference divided by `scale`, in the metric's units.
The shared evidence checker binds the analyzer and both sources by hash and
checks bounds, grids, units, status, confidence, and finite values. It uses
the default grids and tail limit unless the objective supplies `phase_options`.
For example, `"phase_options": {"tail_limit_seconds": 4}` allows a longer
tail search. Selecting an envelope metric requests the envelope report.
An unusable phase or measure stops selection with an error.
