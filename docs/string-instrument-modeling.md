# Shared string-instrument modeling

`hlolli_wg_analyzer` owns the analysis and fitting work shared by the violin,
viola, cello, and double-bass plug-ins. Each plug-in owns its real-time model
and one fixed set of instrument data. This split keeps measurement rules equal
across instruments and keeps private recordings out of plug-in repositories.

## Interface

The shared fit path takes a versioned manifest, a renderer adapter, objectives,
and bounded search rules. It returns:

- the selected parameter values;
- fit and held-out scores by case;
- physical and safety check results;
- input, renderer, model, and result hashes;
- public-safe reports and named listening clips.

A hash identifies the exact bytes used by a run. It does not claim that two
recordings sound alike or that a private recording can be rebuilt from its
hash.

## Ownership

The analyzer owns:

- WAVE parsing, channel and rate checks, and alignment;
- event, attack, held, release, and clean-tail windows;
- pitch, vibrato, spectrum, harmonic, body-envelope, chord, room, and distance
  measures;
- bounded sweeps, fit/held-out splits, score combining, selection, and
  sensitivity checks;
- checked model writing, receipts, reports, and clip export.

Each instrument adapter owns:

- the Csound render command and model-specific controls;
- the map from fit parameters to its fixed model-data file;
- plug-in build and model validation;
- instrument-specific safety bounds.

Instrument adapters, fit manifests, model-specific rules, diagnostics, tests,
and instrument documentation belong in their plug-in repositories under
`tools/analyzer_adapter/`. All four instruments use explicit shared-tool paths;
checkouts need not be siblings. Keep recordings, generated audio, fit output,
scratch notes, verification dumps, and commit/hash logs outside source control.
Public documentation should describe supported use, not local execution history.

For version-2 candidate verification, an instrument declares
`candidate.profile_change_contract`: an ordered list of `parameter`, `path`,
and `source_group` rules. Shared validation requires an exact match to the
change rows, bounded numeric edits, distinct paths/parameters, and one distinct
fit-result hash per source group. The instrument owns the supported paths and
group layout. Model writing still requires the hash-bound profile adapter and
rechecks the selected result. Legacy double-bass manifests retain their
existing protocol; other joint manifests must declare this contract.

## Harmonic decay methods

Version-1 fit manifests can set `harmonic_method_version` on each
`harmonic-decay` objective. Omission keeps `harmonic-decay-v1`, including its
fixed -90 dBFS cutoff. All harmonic objectives in a fit must use the same
method. Selection and model-writing receipts bind the chosen method.

The opt-in `harmonic-decay-v2` method measures quieter PCM audio without
normalizing its samples. For each harmonic it estimates an end-of-recording
floor from the upper quartile of the last tenth of its measured levels, using
at least five windows. It also bounds PCM rounding error by one sample step
in amplitude. Fitting stops 12 dB above the higher of these two floors and
excludes levels below that limit. A band needs at least 20 dB of measured
decay as well as the existing time-support, slope, and residual checks.
Reports include both floors, the fit limit, and the measured decay range.

Uniform gain should preserve estimates while sufficient signal remains above
the PCM limit. Gain cannot restore lost precision. The tail estimate can
include a still-decaying signal, so short recordings may lose support. This
method does not separate overlapping modes or room response, and its floor
estimate is not a calibrated signal-to-noise measurement. The whole-note
checks and the limit on slopes of -3 dB/s or faster still apply.

A method change requires new reference checks and a new selection result.
Keep the old manifest and result. Passing a synthetic method test does not
validate an instrument model or turn development recordings into held-out
evidence.
