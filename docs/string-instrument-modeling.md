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
