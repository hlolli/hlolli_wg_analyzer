# Cello open-string passive-loss fit

This adapter fits one open-string loss value at a time, then checks one fixed
four-string candidate. It can also search termination shape and loss together.
The current C2 and D3 searches vary nut cutoff, bridge cutoff, and loss time.
The G2 search adds a narrow bridge-loss peak. One implementation freezes a
separate bundle for C2, G2, D3, or A3:

| Target | Manifest | Temporary model path | Render string |
|---|---|---|---:|
| `c2` | `fit-c2-frequency-loss.json` | three fields under `strings[0]` | 1 |
| `g2` | `fit-g2-bridge-peak.json` | four fields under `strings[1]` | 2 |
| `d3` | `fit-d3-frequency-loss.json` | three fields under `strings[2]` | 3 |
| `a3` | `fit-a3.json` | `strings[3].loss_time_constant_seconds` | 4 |

It does not fit bow, body, release, or gesture data. It never changes the fixed
model in place.

The adapter has three checked parts:

- `adapter.py` builds one frozen renderer and runs analyzer render jobs;
- the scalar and shape `fit*.json` files score passive decay on separate fit
  and validation recordings; and
- `hlolli_wg_cello/examples/passive_open_string_fit.csd` makes the frozen
  target's pizzicato while keeping the gate high. The high gate avoids the
  normal note-off damping, which would hide passive string loss.

The renderer copies the cello source, model, schema, and build files into a
new temporary tree for each job. It changes the temporary profile, regenerates
the temporary C source, makes a Release plug-in, and renders one PCM24 WAVE.
It never writes to either source repository. Before and after each job it
checks all copied inputs, the Csound header trees, Csound, its direct library,
libsndfile, CMake, Ninja, Python, and the C compiler. Bundle creation also
checks that the host loads the named Csound core and libsndfile paths. Output
publication uses an exclusive hard link and never follows an existing path.

## Recordings

Use separate fit and validation recordings. The legacy C2 examples use the
University of Iowa [individual-pitch recordings](https://theremin.music.uiowa.edu/MIS-Pitches-2012/MISCello2012.html)
and [cello range recordings](https://theremin.music.uiowa.edu/MIScello.html).
Supply lossless WAVE inputs with the format and sample bounds required by the
chosen manifest. Keep source audio, source receipts, and fit output outside Git.
Use recordings only under their source license or permission.

## Build a frozen bundle

Pass one resolved regular Python 3 executable. The frozen renderer records and
reuses that exact interpreter, so it also works on hosts such as NixOS that do
not provide `/usr/bin/python3`. Pass resolved libraries that the named Csound
build loads on the host.

```sh
python3 -B adapters/hlolli_wg_cello/adapter.py build \
  --cello-root /path/to/hlolli_wg_cello \
  --csound /path/to/csound \
  --csound-library /path/to/CsoundLib64 \
  --sndfile-library /path/to/libsndfile \
  --cmake /path/to/cmake \
  --ninja /path/to/ninja \
  --python /absolute/path/to/python3 \
  --cc /path/to/cc \
  --csound-build-dir /path/to/csound-build \
  --csound-source-dir /path/to/csound-source \
  --target g2 \
  --reference-fit /private/path/pizz-G2.wav \
  --reference-check /private/path/iowa-2001-pizz-G2-check.wav \
  --output-dir /private/new/cello-g2-fit-bundle
```

The new directory contains `renderer`, `experiment.json`, `fit.json`,
`bindings.json`, and `receipt.json`. Pass every `id=path` row from
`bindings.json` to the analyzer as a `--bind` option. The analyzer command has
this form:

```sh
hlolli-wg-analyzer \
  --renderer /private/new/cello-g2-fit-bundle/renderer \
  --allow-run \
  --bind reference_g2_fit=/private/path/pizz-G2.wav \
  --bind reference_g2_check=/private/path/iowa-2001-pizz-G2-check.wav \
  --bind base_profile=/path/to/hlolli_wg_cello/model/cello-v1.json \
  --bind cello_cmake=/path/to/hlolli_wg_cello/CMakeLists.txt \
  --bind cello_source=/path/to/hlolli_wg_cello/src/hlolli_wg_cello.c \
  --bind model_generator=/path/to/hlolli_wg_cello/tools/generate_model.py \
  --bind model_manifest=/path/to/hlolli_wg_cello/model/manifest.json \
  --bind model_schema=/path/to/hlolli_wg_cello/model/schema/cello-v1.schema.json \
  --bind probe_csd=/path/to/hlolli_wg_cello/examples/passive_open_string_fit.csd \
  --bind wasm_preparer=/path/to/hlolli_wg_cello/tools/prepare_wasm_source.py \
  --output /private/new/cello-g2-experiment \
  experiment /private/new/cello-g2-fit-bundle/experiment.json
```

Select a point with the raw model WAVE artifacts, not the experiment's diagnostic
gaps. Scalar selection applies the same absolute checks used by the joint
gate: each fit and validation goal needs loss at most 2.0, T60 ratio from 0.5
through 2.0, and support ratio at least 0.5.

```sh
python3 -B tools/instrument_fit.py select \
  --manifest /private/new/cello-g2-fit-bundle/fit.json \
  --experiment /private/new/cello-g2-experiment/result.hwa-experiment \
  --analyzer /path/to/hlolli-wg-analyzer \
  --profile /path/to/hlolli_wg_cello/model/cello-v1.json \
  --bind reference_g2_fit=/private/path/pizz-G2.wav \
  --bind reference_g2_check=/private/path/iowa-2001-pizz-G2-check.wav \
  --output /private/new/cello-g2-selection.json
```

Repeat the command with `c2`, `d3`, or `a3` and the matching files. The old
hidden C2 reference flags remain only so the first frozen receipt can be
reproduced.

For C2, G2, or D3, replace `build` with `build-shape`. The current C2 bundle
varies loss time, nut cutoff, and bridge cutoff across 48 fixed points. G2
varies bridge cutoff, peak bandwidth, peak loss, and passive loss time across
96 points. D3 varies loss time, nut cutoff, and bridge cutoff across 24 points.
The shape manifests combine whole-tail error with eight per-partial T60 checks.
C2 and D3 rank fit loss only after validation acts as a binary gate. Pass the
bundle's `fit.json` to the same `select` command. Shape selection writes only a
new result; it does not edit the fixed model.

`write-profile` can write one per-string candidate to a new path, but that is
not a four-string validation gate. Keep the four results separate until one joint
candidate renders all four strings and passes a third recording set.

## Build a source-balanced corpus bundle

`build-corpus` accepts a checked `hwa-cello-passive-corpus-plan` instead of one
fit and one validation file. Each reference names its source identity,
performance, dynamic, split, path, and measured fundamental. A source identity
must stay in one split, each split needs at least two independent sources, and
duplicate audio is rejected. The adapter renders once per source and grid
point, at that source's median admitted pitch, while all admitted dynamics
share the source-conditioned model artifact.

```sh
python3 -B adapters/hlolli_wg_cello/adapter.py build-corpus \
  --target c2 \
  --corpus-plan /private/corpus-plan-c2.json \
  --cello-root /path/to/hlolli_wg_cello \
  --csound /path/to/csound \
  --csound-library /path/to/CsoundLib64 \
  --sndfile-library /path/to/libsndfile \
  --cmake /path/to/cmake \
  --ninja /path/to/ninja \
  --python /absolute/path/to/python3 \
  --cc /path/to/cc \
  --csound-build-dir /path/to/csound-build \
  --csound-source-dir /path/to/csound-source \
  --output-dir /private/c2-corpus-bundle
```

Within each objective kind, admitted dynamics divide one source's weight.
Both source-separated development folds rank the generic model; the score then
uses worst-source loss and point ID as tie-breaks. Source-mean and physical
limits gate eligibility. Extra takes from one source do not count as extra
independent instruments.

## Check one four-string candidate

`build-joint` takes the four legacy frozen bundles, their current selection
results, and four new audit recordings. Generic-corpus results remain separate
until all four strings pass and a corpus-aware candidate assembler is frozen;
the current C2 failure intentionally blocks that step. Each audit row also
names its source set and performance. The command rejects a
reused recording, a stale result, a changed source or tool, audio stored in
either source repository, or a bundle path under either repository.

```sh
python3 -B adapters/hlolli_wg_cello/adapter.py build-joint \
  --scalar c2 /private/c2-bundle /private/c2-selection.json \
  --scalar g2 /private/g2-bundle /private/g2-selection.json \
  --scalar d3 /private/d3-bundle /private/d3-selection.json \
  --scalar a3 /private/a3-bundle /private/a3-selection.json \
  --audit c2 /private/audit-C2.wav source-set performance-c2 \
  --audit g2 /private/audit-G2.wav source-set performance-g2 \
  --audit d3 /private/audit-D3.wav source-set performance-d3 \
  --audit a3 /private/audit-A3.wav source-set performance-a3 \
  --output-dir /private/new/cello-joint-bundle
```

The new directory has `renderer`, `experiment.json`, `fit.json`,
`bindings.json`, `receipt.json`, and `candidate-profile.json`. Its experiment
has two points: the unchanged four-string baseline and one frozen candidate.
Twelve fit, validation, and audit cases give 24 Release render jobs. Each audit
goal uses its own frozen case, recording, and model render. Audit audio cannot
change the one fixed candidate.

Run the experiment with every row in `bindings.json`, then call `select` with
the 12 `reference_*` rows. The version 2 fit manifest checks total score,
split means, each goal, the saved scalar losses, an absolute 6 dB curve-error
cap, a model/reference T60 ratio from 0.5 to 2.0, and a support ratio of at
least 0.5. A failed check writes a result, exits with status 2, and omits the
chosen fields. `write-profile` recomputes those checks and accepts only a
passing result and the frozen renderer. It writes a new profile and receipt;
it never replaces the source profile.

Keep source rights, recording identities, exact spans, and hashes in the
external source receipt. Audio used to choose a model cannot also serve as
an untouched audit. Do not redistribute recordings without permission.

## Physical excitation law

A passive pizzicato probe must use the model's displacement initial condition
and string impedances. Do not infer physical bow or pluck forces from
microphone dBFS levels. Fit results describe the supplied recordings and
model; they do not establish a complete physical model of the instrument.
