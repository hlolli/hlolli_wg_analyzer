# Violin fit adapter

This adapter brings the violin's existing five-control fit onto the analyzer's
settled renderer, selection, profile-writer, and receipt path. It varies body
wet gain and one bridge cutoff for each string. It does not change the violin
model, claim a new scientific fit, or authorize the historical preview result.

The fit cases are open G3 and A4 plus one body excerpt. The separate check
cases are open D4 and E5 plus a second body excerpt. Shared analyzer objectives
score three spectral bands and the pitch-conditioned body envelope. The
version 1 manifest keeps the existing parameter bounds, weights, and selection
limits.

## Build a private bundle

The builder supports macOS and Linux. It stops on Windows because it cannot
yet check which Csound library the executable loaded.

Keep recordings and the generated bundle outside both repositories. The four
open-string recordings and body sources must be nonempty mono or stereo PCM16
WAVE at 44.1 kHz. The builder takes a centered excerpt from each body source.
It does not resample measurement input. Before it writes the bundle, it runs
the named analyzer's `isolated-note` pitch check on G3, D4, A4, and E5. Each
check must report valid pitch at the note named by that input. This check does
not add attack or held-note goals; those need their own test data and stage.
All six source hashes and all six bound hashes must be distinct. The generated
fit manifest pins both body excerpts by SHA-256. The Python executable path
must contain no whitespace because it forms the frozen renderer's shebang.

```sh
python3 -I adapters/hlolli_wg_violin/adapter.py build \
  --violin-root /path/to/hlolli_wg_violin \
  --analyzer /path/to/hlolli-wg-analyzer \
  --csound /path/to/csound \
  --csound-library /path/to/CsoundLib64 \
  --module /path/to/libhlolli_wg_violin_test \
  --python /absolute/path/to/python3 \
  --reference-g3 /private/path/g3.wav \
  --reference-d4 /private/path/d4.wav \
  --reference-a4 /private/path/a4.wav \
  --reference-e5 /private/path/e5.wav \
  --reference-body-fit /private/path/body-fit.wav \
  --reference-body-check /private/path/body-check.wav \
  --output-dir /private/new/violin-fit-bundle
```

The output contains `renderer`, `experiment.json`, `fit.json`,
`bindings.json`, `receipt.json`, and the two private body excerpts. The
renderer pins the exact Csound executable and library, Python interpreter,
analyzer executable, test module, diagnostic score, source profile, schema,
generator, and fit manifest. It rechecks those resources before and after each
action.

Run the experiment with every `id=path` row from `bindings.json`:

```sh
hlolli-wg-analyzer \
  --renderer /private/new/violin-fit-bundle/renderer \
  --allow-run \
  --bind reference_g3=/private/path/g3.wav \
  --bind reference_d4=/private/path/d4.wav \
  --bind reference_a4=/private/path/a4.wav \
  --bind reference_e5=/private/path/e5.wav \
  --bind reference_body_fit=/private/new/violin-fit-bundle/reference_body_fit.wav \
  --bind reference_body_check=/private/new/violin-fit-bundle/reference_body_check.wav \
  --output /private/new/violin-experiment \
  experiment /private/new/violin-fit-bundle/experiment.json
```

Then select a candidate through the shared fitter. A result that passes those
checks is evidence for the declared recordings only.

```sh
python3 -I tools/instrument_fit.py select \
  --manifest /private/new/violin-fit-bundle/fit.json \
  --experiment /private/new/violin-experiment/result.hwa-experiment \
  --analyzer /path/to/hlolli-wg-analyzer \
  --profile /path/to/hlolli_wg_violin/profiles/generic_violin.json \
  --bind reference_body_fit=/private/new/violin-fit-bundle/reference_body_fit.wav \
  --bind reference_body_check=/private/new/violin-fit-bundle/reference_body_check.wav \
  --output /private/new/violin-selection.json
```

`write-profile` accepts the frozen renderer as the profile adapter. It writes
a new profile and receipt and never replaces `generic_violin.json`.

```sh
python3 -I tools/instrument_fit.py write-profile \
  --manifest /private/new/violin-fit-bundle/fit.json \
  --fit /private/new/violin-selection.json \
  --source /path/to/hlolli_wg_violin/profiles/generic_violin.json \
  --adapter /private/new/violin-fit-bundle/renderer \
  --output /private/new/candidate-violin.json \
  --receipt /private/new/candidate-violin-receipt.json
```

The frozen renderer has three actions: `--describe`, `--validate-profile
PATH`, and `--hwa-experiment-job JOB --output-dir DIR`. Render requests must
name one frozen case, its exact split and reference, all five bounded
parameters, and one new `model.wav` inside the supplied output directory.
Renders always use Csound's sample-accurate single-thread mode and publish one
non-silent, unclipped stereo PCM16 WAVE at 44.1 kHz.
