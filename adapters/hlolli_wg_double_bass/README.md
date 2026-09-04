# Double-bass per-string passive-loss workflow

This directory holds the analyzer-side Stage 3 adapter. The v2 workflow fits
the E, A, D, and G string loss values independently from one declared
University of Iowa 2012 `ff` reference per physical string. A, D, and G use
their open notes. The E-string reference is G1 because the full E1 and F1
recordings failed the unchanged one-event gate and G1 was the first ascending
left-channel note to pass both checked pitch and harmonic decay. It does not
change the fixed model or mark Stage 3 done.

The failed v1 bundle remains here as historical evidence. It varied four loss
values together at 32 random points and exposed a flat whole-file RMS response.
Do not use that response or the v1 selector result to choose model data.

## Frozen physical-dynamics falsification

`physical_dynamics.py` validates one external, hash-bound twelve-file
OrchideaSOL roster, renders the fixed runtime at the declared score-level bow
speeds, and emits a path-free result. It compares `pp` and `ff` changes against
`mf`; this keeps static body colour from determining the dynamic-spectrum
objective. Its contact probes require the private test module built from the
same canonical C source. It does not fit a model or write fixed data.

```sh
python3 -B adapters/hlolli_wg_double_bass/physical_dynamics.py \
  --declaration /private/physical-dynamics/declaration.json \
  --roster /private/physical-dynamics/source-roster.json \
  --source-root /private/orchideasol-release \
  --runtime-source /path/to/hlolli_wg_double_bass/src/hlolli_wg_double_bass.c \
  --model /path/to/hlolli_wg_double_bass/model/double_bass-v1.json \
  --csound /path/to/csound \
  --module /path/to/libhlolli_wg_double_bass_test.so \
  --analyzer /path/to/hlolli-wg-analyzer \
  --output /private/physical-dynamics/result.json
```

The first frozen run checked all twelve source hashes/WAVE facts and rejected
the baseline on six predeclared level or dynamic-spectrum objectives. Pitch,
clipping, source level ordering, and contact-state gates passed. A second,
separately frozen development run replaced only the four string impedances with
midpoints of published orchestra-string ranges. It remained finite but failed
13 objectives, demonstrating that the runtime's absolute contact/hair constants
must be identified jointly with impedance. Orchidea does not report physical
bow force, speed, or position, so neither result can identify those constants
or serve as an independent audit. The sibling double-bass repository keeps the
public-safe receipt.

## Optional controlled recording intake

The owner superseded the proposed 108-region campaign before recording or
fitting in favor of published physical laws plus Iowa and OrchideaSOL. Keep
`recording_session.py` as a read-only fallback if a future residual parameter
really needs new rights-clear E-A-D-G evidence. It writes and copies nothing.
Raw audio, consent evidence, session manifests, and the campaign manifest
remain outside repositories.
Run it with absolute paths:

```sh
python3 -B adapters/hlolli_wg_double_bass/recording_session.py \
  validate-session --manifest /private/campaign/fit/manifest.json
python3 -B adapters/hlolli_wg_double_bass/recording_session.py \
  validate-session --manifest /private/campaign/check/manifest.json
python3 -B adapters/hlolli_wg_double_bass/recording_session.py \
  validate-campaign --campaign /private/campaign/campaign.json
```

A strict version-1 session binds documented analysis/model-fitting permission,
one rights-evidence file, an RFC 3339 recording time, setup and physical-string
logging IDs, A4 measurement, exact E1/A1/D2/G2 string declarations, measured
microphone geometry, whole 96-kHz PCM24 WAVE files, and non-overlapping take
regions. It accepts no processing history. Every pizzicato region is an open
ordinary pizzicato with measured pluck distance. Every steady-arco region is
an open sustain with measured bow force, speed, bridge distance, direction,
and a derived position ratio.

Each string needs five fit and three check pizzicato repetitions. Six balanced
low/medium/high bow-control cells require two fit and one check repetitions.
Measured force and speed levels must be strictly ordered; ponticello, middle,
and tasto bridge distances must also be separate and ordered. Each session
also needs room-tone and sync regions. This is 70 regions in fit and 38 in
check.

The campaign hash-binds exactly one fit and one check session under
`source-group-separated-before-fitting`. It requires distinct session and
source-group IDs and disjoint source-audio hashes while keeping instrument,
bow, string declarations, and tuning identical. Success emits deterministic,
path-free summary JSON. The double-bass repository's
`docs/research/stage3-controlled-recording-protocol.md` is the operator
protocol and minimum six-cell matrix.

## v2 contract

Check the public, path-free files:

```sh
python3 -I adapters/hlolli_wg_double_bass/build_manifest.py validate-v2
```

The v2 contract has four separate one-parameter grids. Each fit manifest:

- binds exactly one checked Iowa 2012 `ff` WAVE by its declared SHA-256;
- has only a `fit` objective and uses explicit `fit-only` selection;
- requires the analyzer's `isolated-note-1` pitch gate;
- scores `harmonic-decay-1` T60 evidence from the rendered note; and
- gates mean absolute harmonic error at 0.75 octave and worst harmonic error
  at 1.5 octaves.

Passive decay is amplitude-independent in the linear loop, so separate
`pp`/`mf`/`ff` loss targets would not identify separate coefficients. The
four `ff` notes maximize decay signal-to-noise while retaining one observation
per physical string.

The base 19 loss times run from 0.01 through 30 seconds and include the
unchanged 0.25-second baseline. After the first valid run bracketed D's
opposing first- and fifth-harmonic errors between 0.75 and 1.0 seconds, its
grid gained five predeclared refinement values from 0.85 through 0.95 seconds.
E, A, and G therefore have 19 points; D has 24.

Stage 8 requires both fit and check cases, so each point renders the same bound
recording once under each split: 38 renders for E, A, and G and 48 for D. The
check render is a format diagnostic, not an audit objective, and `fit-only`
selection cannot use it or accept another binding. A baseline can remain
selected when it has the lowest eligible fit loss.

`diagnostic.rms` remains in each experiment only because the experiment format
requires a response. `tools/instrument_fit.py` does not rank or gate it. It
reads each checked model artifact and scores native note/harmonic-decay
results instead.

The renderer starts each synthesized note after a fixed 100 ms lead-in, or
one quarter of the file for shorter test fixtures, while preserving the exact
reference frame count. This supplies the silence-before-onset required by the
one-event gate; it does not trim, filter, normalize, or otherwise alter the
reference recording.

## Predeclared D frequency-loss follow-up

The D-only v3 follow-up reuses the runtime's existing first-order bridge
termination rather than adding another filter or changing the fixed model.
For `p = exp(-2 pi f_c / f_s)`, that termination is

```text
y[k] = p y[k-1] + (1-p) x[k]
H_b(z) = (1-p) / (1-p z^-1)
```

and the modeled magnitude of harmonic `n` after one round trip is

```text
G_n = exp(-1 / (f_0 tau)) |H_nut(exp(j omega_n)) H_b(exp(j omega_n))|
omega_n = 2 pi n f_0 / f_s
T60_n = -ln(1000) / (f_0 ln(G_n)).
```

For `0 < p < 1`, the denominator of `|H_b(exp(j omega))|^2`
exceeds its `(1-p)^2` numerator by `2 p (1-cos(omega))`, so the filter is
finite, stable, and has magnitude at most one. The declared 48 kHz grid keeps
`p` from 0.3954950962479312 through 0.8773057690983457 and keeps the broadband
gain strictly between zero and one. The delay solver includes the filter's
exact phase rather than treating it as free.

Varying `tau` moves broadband decay while varying the one bridge corner
changes one monotonic frequency slope. This is the smallest extension of the
failed scalar fit: it adds one fitted parameter, no DSP state, and no model
field. A shelf would add both a corner and depth; a second-order peak is not
justified by the current broad high-harmonic residual. This follows Smith's
[*Frequency-Dependent Damping*](https://ccrma.stanford.edu/~jos/pasp/Frequency_Dependent_Damping.html)
loop-filter constraints; retaining partial-decay error as the fit objective is
consistent with Bank and Välimäki's
[robust loss-filter design](https://ieeexplore.ieee.org/document/1172822/).

The grid was frozen before a new render or selection. It has 30 points:

- loss time `tau`: 0.25, 1.0, 1.5, 2.0, 2.5, and 3.0 seconds;
- bridge cutoff `f_c`: 1000, 1500, 2000, 3000, and the unchanged
  7086.471045764144 Hz baseline; and
- exactly the existing D2 reference, pitch gate, harmonic-decay method,
  0.75-octave mean limit, 1.5-octave worst limit, and fit-only ranking.

The previous D residual sets these bounds. Its scalar render needed a longer
fundamental decay while harmonics 5 and 6 were already too long. Evaluating the
one-pole equation against the already-consumed six-band reference brackets the
needed monotonic corner between 1 and 3 kHz and the broadband time between 1
and 3 seconds. This calculation only sets a bounded experiment; measured
radiated gains are not copied into the string.

Validate or build only this frozen D bundle with:

```sh
python3 -I adapters/hlolli_wg_double_bass/build_manifest.py \
  validate-d-frequency-v3

python3 -I adapters/hlolli_wg_double_bass/build_manifest.py \
  build-d-frequency-v3 \
  --fit-reference \
  iowa2012-pizz-d-ff-open-left-48k-soxr=/private/path/fit-d.wav \
  --analyzer /path/to/hlolli-wg-analyzer \
  --output-dir /private/new/double-bass-d-frequency-v3
```

The generated experiment contains one selecting fit case and the required
same-binding diagnostic check case. Both carry exactly the two D parameters;
other strings and corpora cannot enter this fit.

The real frozen run passed. Point 13 selected a 1500 Hz bridge cutoff and
3.0-second broadband loss time. Eleven of 30 points passed the unchanged fit
rules. The selected six-harmonic RMS, mean absolute, and worst T60 errors are
0.529, 0.427, and 0.980 octaves; rendered pitch is +0.228 cents. The selected
loss time lies on the declared upper boundary, so the result is fit-side
evidence only, not permission to widen the grid or write a candidate. The
public receipt in the sibling double-bass repository records the smoke, run
hashes, verification, and unchanged-model proof.

## Build a private v2 bundle

The four declared Iowa inputs must match the hashes and WAVE facts in
`fit-reference-contract-v2.json`:

```sh
python3 -I adapters/hlolli_wg_double_bass/build_manifest.py build-v2 \
  --fit-reference iowa2012-pizz-e-string-g1-ff-left-48k-soxr=/private/path/fit-e.wav \
  --fit-reference iowa2012-pizz-a-ff-open-left-48k-soxr=/private/path/fit-a.wav \
  --fit-reference iowa2012-pizz-d-ff-open-left-48k-soxr=/private/path/fit-d.wav \
  --fit-reference iowa2012-pizz-g-ff-open-left-48k-soxr=/private/path/fit-g.wav \
  --analyzer /path/to/hlolli-wg-analyzer \
  --output-dir /private/new/double-bass-passive-v2
```

Before publishing the bundle, the builder checks each WAVE with the named
analyzer, requires valid pitch and a valid reference harmonic profile, and
rehashes every input. The output has one `e1`, `a1`, `d2`, and `g2` directory.
Each contains `experiment.json`, `fit.json`, `bindings.local.json`, and
`reference-evidence.json`. The top-level `receipt.json` binds those files,
the analyzer, the builder, and the four references.

No audio is copied, linked, converted, or written into the bundle. The local
bindings and result bundle therefore stay outside Git.

Build the frozen renderer with its checked local Python, Csound, compiler,
headers, source, generator, and fixed-model resources. Its generated shebang
uses the declared Python executable:

```sh
python3 -I adapters/hlolli_wg_double_bass/build_renderer.py build \
  --config /private/path/renderer-config.json \
  --output /private/path/double-bass-renderer
```

For each target, run the experiment with only that target's binding, then run
fit selection. For the E string the commands have this shape:

```sh
/path/to/hlolli-wg-analyzer \
  --renderer /private/path/double-bass-renderer --allow-run \
  --bind iowa2012-pizz-e-string-g1-ff-left-48k-soxr=/private/path/fit-e.wav \
  --output /private/path/e1-results \
  experiment /private/new/double-bass-passive-v2/e1/experiment.json

python3 -B -I tools/instrument_fit.py select \
  --manifest /private/new/double-bass-passive-v2/e1/fit.json \
  --experiment /private/path/e1-results/result.hwa-experiment \
  --analyzer /path/to/hlolli-wg-analyzer \
  --profile /path/to/hlolli_wg_double_bass/model/double_bass-v1.json \
  --bind iowa2012-pizz-e-string-g1-ff-left-48k-soxr=/private/path/fit-e.wav \
  --output /private/path/e1-selection.json
```

Repeat with A1, D2, and G2. A passing per-string result is only a fit-side
candidate. Do not write or publish a four-string model until a separately
frozen joint candidate passes untouched validation and audit evidence.

## Evidence separation

The Iowa 2001 session remains held out and cannot determine a v2 per-string
value. OrchideaSOL Contrabass recordings are separate development evidence
only, not a fit or final-audit set. They are absent from the v2 contract, fit
manifests, renderer cases, and build arguments; their bytes must never be
copied into this or the plug-in repository. If either source guides a later
parameter choice, a new untouched audit set is required.

## Independent-source joint validation v2

`build-joint-validation-v2` is the independent-source successor. It uses the
same frozen five candidate changes and the same four fit selections as v1, but
its audit roster consists of four already-existing whole WAVE archive members
from one MTG Good-sounds source group. It neither accepts an FFmpeg tool nor
has a conversion or repeat-output field. Source facts remain in one external,
read-only declaration; no source declaration or audio belongs in this
repository.

```sh
python3 -B -I adapters/hlolli_wg_double_bass/build_manifest.py \
  build-joint-validation-v2 \
  --declaration /private/read-only/good-sounds-joint-declaration-v2.json \
  --output-dir /private/new/good-sounds-joint-validation-v2
```

The declaration uses schema
`hwa-double-bass-joint-validation-declaration`, version 2. Its top-level fields
match v1 except that `conversion` is forbidden. `tools` must contain only the
hash-bound `analyzer`, `fit_selector`, `manifest_builder`, and
`renderer_builder`. The unchanged policy requires one validation run, no
post-validation tuning, no fixed-model write, and external audio, candidate,
results, and renderer configuration. The renderer configuration must set
`permissions.write_profile` to `false`.

`audit` has exactly these fields:

- `dataset_id: "mtg-good-sounds-v1"`;
- one declared `source_group_id`; and
- `independent_of_fit_source_group: true`.

`validation_roster` has exactly one row for each target below. Frequencies are
the exact A4=442 open targets declared by this Good-sounds source group, not
post-analysis measured-frequency substitutions.

| Target | Note | `expected_hz` |
|---|---|---:|
| `e1` | E1 | 41.390732998718335 |
| `a1` | A1 | 55.25 |
| `d2` | D2 | 73.74990194289438 |
| `g2` | G2 | 98.4443083545075 |

Each row has only `target`, `note`, `expected_hz`, and `source`. Each `source`
requires:

- a unique binding `id`, external regular-file `path`, SHA-256, byte size,
  frame count, channels, sample rate, bit depth, `whole_file: true`, and the
  same `source_group_id` as the other three rows;
- `archive` provenance with HTTPS URL, name, the publisher-reported MD5, and
  byte size;
- `member` provenance with its relative archive path, SHA-256, and byte size;
  the member hash and size must equal the local WAVE hash and size;
- `license` evidence that keeps the source conflict explicit: Zenodo's
  structured metadata says `CC-BY-4.0`, its description says
  `CC-BY-NC-4.0`, and the four Freesound member pages say `CC-BY-3.0`;
  these inputs are therefore private-analysis-only; and
- this exact `string_assignment_evidence` object:

```json
{
  "good_sounds_metadata_string": null,
  "kind": "open-pitch-transfer-proxy",
  "source_proven_physical_string": false
}
```

All four members must come from the same declared archive and license. The
local inputs must be distinct, nonempty mono PCM16 or PCM24 RIFF WAVE files at
48 kHz. Their names are unrestricted: the checker does not recognize Iowa
filenames. It rejects symlinks, changed hashes/sizes/WAVE facts, duplicate
members, extra fields, another source group, any derived/repeat path, or a
physical-string claim. `string=null` means pitch labels alone assign these
notes to renderer targets. This can test open-pitch transfer, but it cannot
prove that the recorded E1, A1, D2, or G2 came from that physical string.

Before it builds the renderer, the builder checks all four fit inputs and
selections, re-inspects and rehashes all four Good-sounds WAVE files, and runs
`isolated-note-1` pitch plus the `harmonic-decay-1` reference gate. Every
reference needs at least four valid harmonics. Any failure stops before bundle
publication or candidate rendering. The resulting version-2 bundle retains
one baseline and one frozen candidate, fit/check/audit objectives, mean
harmonic error at most 0.75 octave, worst harmonic error at most 1.5 octaves,
checked Python shebang and resources, output confinement, repeated input
checks, and `write_profile: false`. Its receipt copies the path-free local
WAVE facts, archive/member provenance, license, source-group ID, and explicit
open-pitch proxy evidence. It copies no audio.

The real frozen run used exact A4=442 E1/A1/D2/G2 members from one player,
session, microphone, and hard-pizzicato group. All four references passed
pitch and harmonic preflight. After Stage 8 rejected an initial manifest before
job 1 because `joint_candidate.levels` omitted its baseline, the adapter was
corrected to declare `[0.0, 1.0]` and a superseding declaration retained the
same candidate, roster, and gates. The resulting 24-job run failed: G passed
at 0.628-octave mean and 1.240-octave worst error, A failed at 0.893/2.039, D
failed at 1.238/2.412, and E had no valid shared decay bands. No profile was
written and this roster must not tune a successor.

Source review also found conflicting rights metadata: Zenodo's structured
field, its description, and the Freesound member pages name three different
licenses. The checker now requires that conflict and private-analysis-only
status explicitly. The sibling double-bass repository has the public-safe
receipt.

## Frozen joint validation

`build-joint-validation-v1` consumes one external, read-only pre-analysis
declaration. The declaration binds the four fit selections, unchanged source
model, external candidate profile, one untouched converted validation note per
physical string, conversion and tool hashes, fixed gates, and exact commands.
It must stay outside both repositories.

```sh
python3 -B -I adapters/hlolli_wg_double_bass/build_manifest.py \
  build-joint-validation-v1 \
  --declaration /private/read-only/pre-analysis-declaration.json \
  --output-dir /private/new/joint-validation-bundle
```

The builder rechecks all source and tool hashes, requires byte-identical repeat
conversions, runs the native reference pitch and harmonic preflight, and only
then emits a frozen renderer plus a version-2 candidate-verification manifest.
The experiment has exactly two points: the unchanged fixed model and the
hash-bound candidate. The renderer accepts only `joint_candidate = 0` or `1`
and changes only the declared E/A/D/G losses and D bridge cutoff in a temporary
model. It retains regular-file and symlink rejection, checked Python shebangs,
resource rehashing, output confinement, and `write_profile: false`.

The first frozen roster failed before bundle publication: its physical
E-string A-flat1 reference passed `isolated-note-1` pitch but
`harmonic-decay-1` rejected it with `late-pulse`. The candidate was not
rendered, the remaining validation references were not analyzed, and the
roster was not changed after the result. The public receipt in the sibling
plug-in repository records hashes and the next-source decision.

## Historical v1 bundle

`fit-passive-open-v1.json`, `reference-contract-v1.json`, and the old
`validate`/`build` commands retain the first 264-job trial contract for audit.
A new run has a new builder hash; the original receipt remains the authority
for the historical run. The v1 files are not the active fit path, and no v1
point authorizes a model update.
