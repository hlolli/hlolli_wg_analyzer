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

## Work order

The worktree already contains draft body-envelope code and an instrument-fit
driver. Audit their tests and keep passing parts before adding new versions.

### 1. Settle the renderer contract

- [ ] Audit the current `instrument_fit.py` and its tests against this contract.
- [ ] Version the render job, case, parameter, input, output, and failure
  fields.
- [ ] Keep one process adapter for all instruments.
- [ ] Make render-only, score-only, and apply-model rights explicit.
- [ ] Test paths with spaces, failures, limits, resume, and exact reruns.

Done when a mock adapter and one real adapter pass the same contract tests.

### 2. Settle objective adapters

- [ ] Audit the current body-envelope code and tests before extending it.
- [ ] Move the violin attack and held-path checks behind shared objectives.
- [ ] Move open-string and passive-decay checks behind shared objectives.
- [ ] Finish body-envelope storage and real-recording tests.
- [ ] Add clean-tail checks which reject windows containing the next note.
- [ ] Add chord response plus room and distance accounting.

Done when each measure has a small fixture, a real-recording check, units,
limits, and a stable report form.

### 3. Combine scores and select

- [ ] Keep fit and held-out cases separate throughout search.
- [ ] Combine sound scores with pitch, finite-state, output, and cost gates.
- [ ] Report worst harm as well as mean gain.
- [ ] Keep parameter sensitivity and interaction results.
- [ ] Select only candidates that pass every hard gate.

Done when a chosen candidate can be explained from one result file without
rerunning the search.

### 4. Write fixed model data safely

- [ ] Validate the source model and candidate against the plug-in schema.
- [ ] Write a new file and ask the plug-in adapter to validate it.
- [ ] Never replace the source model during search.
- [ ] Bind the result to the adapter, manifest, inputs, and selected values.

Done when an interrupted or failed write cannot alter the source model.

### 5. Prove reuse

- [x] Finish and test the violin adapter and manifest.
- [ ] Add viola, cello, and double-bass adapters as their renderers become
  available.
- [ ] Keep one shared objective set where the measured fact is the same.
- [ ] Add a piano adapter and retain its instrument-specific objectives.

Done when violin and piano pass the same renderer, search, selection, writer,
and receipt tests, then one lower-string plug-in passes without copied analyzer
code.
