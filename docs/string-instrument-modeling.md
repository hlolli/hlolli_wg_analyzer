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

Each analyzer-side instrument adapter owns:

- the Csound render command and model-specific controls;
- the map from fit parameters to its fixed model-data file;
- plug-in build and model validation;
- instrument-specific safety bounds.

The plug-in repository receives only the selected fixed data and a receipt
safe to publish. Raw recordings and local fit output stay outside Git.
