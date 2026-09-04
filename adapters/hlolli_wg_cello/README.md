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

The fit note is the 2012 University of Iowa `ff` open C2 pizzicato from the
[individual-pitch page](https://theremin.music.uiowa.edu/MIS-Pitches-2012/MISCello2012.html).
The validation note comes from the older Iowa 2001 `ff` C-string range on the
[cello page](https://theremin.music.uiowa.edu/MIScello.html). The second take
uses a different player, cello, mic, and session.

| File | Role | SHA-256 |
|---|---|---|
| 2012 `Cello.pizz.ff.sulC.C2.stereo.aif` | raw fit source | `5424133b9a1ca4143cb84c833c00bd236498934eb94ed4e204b9f6f69738c51e` |
| 2012 `pizz-C2.wav` | fit input | `94dbd2259de5162cc16a4d5a8d9ef54590cdf7d3a9b3a37276c4483292714daa` |
| 2001 `Cello.pizz.ff.sulC.C2A2.aiff` | raw validation source | `4df44ccb2754696d96fa422a13e414fcaffef1dfb50a2d60e7205de77117ae96` |
| 2001 `iowa-2001-pizz-C2-check.wav` | validation input | `f61a03f01cf32cb8c60d2e9b3a98011deaace77f2fcd8b122530fc5f92f73d2a` |

The validation trim keeps source samples `[209692, 465842)`: 256,150 mono frames
at 44.1 kHz, or 5.808390 seconds. Conversion changes signed 16-bit AIFF
big-endian samples to the same signed values in little-endian WAVE. It does
not change rate, channels, gain, or samples. Keep both source recordings and
all fit output outside Git. The
[G2, D3, and A3 source receipt](../../docs/research/cello-gda-passive-loss-sources.md)
records the other official files and exact trims.

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

Select a point with the raw model WAVE artifacts, not the Stage 8 diagnostic
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
not the Stage 3 merge gate. Keep the four results separate until one joint
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
limits gate eligibility. This is intentionally different from pretending that
one cello's extra takes are extra independent instruments. The
[generic-corpus report](../../docs/research/cello-generic-passive-corpus-v1.md)
records the current source split, intake corrections, grids, and results.

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
Eight fit and validation cases give 16 Release render jobs. Audit goals reuse
the validation renders, so audit audio cannot change the candidate or add a
hidden build.

Run the experiment with every row in `bindings.json`, then call `select` with
the 12 `reference_*` rows. The version 2 fit manifest checks total score,
split means, each goal, the saved scalar losses, an absolute 6 dB curve-error
cap, a model/reference T60 ratio from 0.5 to 2.0, and a support ratio of at
least 0.5. A failed check writes a result, exits with status 2, and omits the
chosen fields. `write-profile` recomputes those checks and accepts only a
passing result and the frozen renderer. It writes a new profile and receipt;
it never replaces the source profile.

The 2026 RWC Instruments release supplies three maker/player variations. The
[source receipt](../../docs/research/cello-alternative-passive-audit-sources.md)
records its rights, member hashes, exact splits, pitch, and raw-tail checks.
Variation 2 mezzo fits G2 and variation 3 mezzo validates it. Variation 1 was
already used in the earlier four-string audit, so it cannot check the new G2
model as untouched data. An authorized OrchideaSOL set later supplied the
independent audit; its archive, audio, and results remain outside Git and may
not be redistributed.

## Current passive-loss result

The 2026-09-04 rerun binds the production displacement initializer and
published cello-string impedances to the unchanged 28-take, four-source corpus,
weights, gates, and parameter grids. D3 passes 12 points. C2, G2, and A3 each
have zero eligible points because one admitted RWC objective retains only two
valid model/reference harmonic overlaps; the frozen minimum is three. All 960
render contracts and hashes, 66 focused analyzer tests, and 11 cello tests
pass. The provenance-repaired result SHA-256 is
`36fa24974d5913f91e78717ab4a5112a10265ae6565b14249131258875eed496`;
repair receipt SHA-256 is
`3700216c8e520539ed9dbab8322846ab03f6cf6f3adf60970b3cfbc435863fb8`.
No profile or candidate was written and the audit remains sealed. Any follow-up
that can select release time, pluck position, termination, or a wider loss
range needs owner review and a new predeclaration.

The 2026-09-03 pre-physical-excitation generic-corpus run admitted the same 28
open-string takes and balanced source identities rather than raw file count.
Its G2, D3, and A3 passes are historical and no longer authorize profiles under
the changed excitation spectrum. They selected:

- G2: 18 kHz bridge cutoff, 200 Hz-wide 293 Hz bridge-loss peak, 0.02 peak
  loss, and 3.0 s broadband loss time;
- D3: the existing 7086.471 Hz bridge cutoff, 8 kHz nut cutoff, and 0.75 s
  loss time; and
- A3: 12 kHz bridge and nut cutoffs with 1.0 s loss time.

C2 fails both existing-control grids and an authorized 96-point bridge
high-shelf follow-up. The shelf improves upper-partial decay and lowers the
best generic score from 3.648 to 3.394, but the decisive source still has only
two harmonic overlaps. All five source-conditioned model fundamentals are
about 46 to 66 dB below their reference peaks. That is a generic
excitation/spectral-balance blocker, not a reason to keep widening passive-loss
grids. No C2 profile, four-string candidate, or
audit run is therefore valid yet. The
[generic-corpus report](../../docs/research/cello-generic-passive-corpus-v1.md)
records intake, weighting, grids, results, and receipts.

The earlier per-recording C2/D3 run is retained as historical development
evidence in the [C2/D3 report](../../docs/research/cello-cd-frequency-loss-v1.md).
The earlier G2 result used 2.5 s loss and a 100 Hz peak; its
[G2 report](../../docs/research/cello-g2-bridge-termination-v1.md) gives the
per-partial results and receipts.

The fixed profile still contains 0.25 s passive loss, zero peak loss, and zero
high-shelf loss for every string. Its schema hash changed, but the four C2
source-conditioned neutral renders remain byte-identical. The independent
OrchideaSOL joint audit rejected the staged
seven-value candidate: C2, D3, and G2 exceeded the 2.0 curve-loss cap, and D3
also exceeded the 0.25 harm cap. Aggregate fit, validation, audit, and total
scores improved, but per-goal failures block a profile write. The
[audit receipt](../../docs/research/cello-third-passive-audit-source.md)
records the measurements and result hashes.

The [full sweep report](../../docs/research/cello-four-string-passive-loss-v3.md)
records the historical 28-point score table, later G2 grid, fixed absolute
limits, RWC source split, and failed final audit. The original v3 run used:

- adapter: `1c83bef757054fcf29ad5cf16d2125c6e123b279e53dccc89ba97509bc244cfa`;
- fit selector: `e4476bdfde0f44546cbae7b251f2784e257c72ab548e6057396b621e31a96c98`;
- generic probe: `ac17b30bdddecaf23b4883b924a1af9ed7f6b01f0df47984853ff171db19ad48`.

The selector used by the high-shelf run has SHA-256
`a49c7560a7f9494ec3a0a189c6392fbbb22401475e36dceb0c3fff267d1b0801`.
Its frozen adapter has SHA-256
`1b626121bca0b81b9104369d11ed87965fd30845ad69e047198f75a4146dbe82`.
The earlier loss-only G2 grid had 19 points through 3.0 seconds and selected
1.50 seconds. The later RWC variation-1 audit failed at D3 and G2. The rejected
joint receipt hashes are
`8858d1504351c3340b2112037c76e2a03587dd4619620316172a4525b5e3ca34`
for the bundle and
`f89292ea9270786813eca9e9b7e7facf05df1fb795d6d712f6152a77ceef6449`
for the failed result.

One time value per string did not pass across these players and dynamics. The
next fit added bridge and nut cutoffs on G2 and D3 and scored eight partial
decays. D3 passed with 0.75 s loss, 8 kHz nut cutoff, and the existing
7086.47 Hz bridge cutoff. That G2 grid failed. The
[shape-fit report](../../docs/research/cello-gd-passive-shape-v1.md) records
those grids, limits, results, and hashes. The later narrow G2 bridge peak is
the first G2 candidate to pass both fit and validation.

## Historical C2 v1 sweep

On 2026-08-25 the seven-level Release sweep ran 14 jobs through the real
external-renderer process. Every model file was non-silent, unclipped, stereo
PCM24 at 44.1 kHz, and 159,861 frames. Fit and check jobs at the same point had
the same model hash.

| Loss time (s) | Model T60 (s) | Fit loss | Check loss |
|---:|---:|---:|---:|
| 0.080 | 0.520 | 5.692 | 5.578 |
| 0.125 | 0.797 | 5.335 | 5.182 |
| 0.250 | 1.548 | 4.493 | 4.324 |
| 0.500 | 3.003 | 3.080 | 3.301 |
| 0.750 | 4.426 | 1.452 | 2.000 |
| 1.000 | 5.815 | 0.514 | 1.095 |
| 1.500 | 8.180 | 1.859 | 1.006 |

The fit note measured a 5.246-second T60. The check note measured 8.441
seconds. The combined first pass chose 1.0 second. This is a provisional C2
result, not a fixed-model update. The Stage 8 RMS and band gaps all reached
their cap and stayed flat; `instrument_fit.py` chose from the passive-decay
curves in the job WAVE files instead.

The source hashes for this run were:

- adapter: `45d0d5667dfd2fc31f5c8fb664d6e7026ab4f4ff342d88991fc340835d8be453`;
- fit manifest: `5e76750085717b7e72f022201fef59f0f86a5a3bc571309b9d84336bdde5d107`;
- probe score: `c384de4fd235c3bbb3217cca9527f1348dd7c8398a7fbcbc5f45ee75a13a9ce5`;
- fit tool: `936aa063869bd3b7e2b8dfeb0a56def6e22c3122111bd7814c252c23825b8a9f`.

The bundle receipt holds the host tool, library, header-tree, input, renderer,
manifest, and binding hashes. Apple system libraries and other transitive
Csound libraries remain host trust inputs; the receipt does not claim to hash
the whole operating system.

## Physical excitation law

The follow-up [physical-dynamics report](../../docs/research/cello-physical-dynamics-v1.md)
replaces the proposed empirical C2 excitation sweep. Published string equations
show that ordinary pizzicato must begin from nonzero displacement and zero
velocity; the imported finite junction pulse was the wrong initial condition.
A no-recording private probe restores 21.296 dB of C2 fundamental and verifies
exact `20 log10(force ratio)` scaling. The 2026-09-04 production engine now
uses a repeatable, band-limited displacement initial condition and the paper's
measured cello-string impedances. The sealed development rerun above falsifies
a complete four-string candidate without redesigning or ranking the physical
law from microphone dBFS levels.
