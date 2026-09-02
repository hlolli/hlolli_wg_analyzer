# Double-bass open-string passive-loss manifest

This directory holds the first four-string Stage 3 manifest. It fits one loss
value for each open string from 2012 Iowa pizzicato notes and checks the joint
candidate against 2001 Iowa notes. It does not change the fixed model or mark
Stage 3 done.

The first real run completed all 264 render jobs, but it did not produce a fit.
The isolated-note gate rejected the E1 and G2 fit tails and the G2 held-out
tail, while the older selector stopped on a second onset. Keep those failures;
do not raise a limit or write a model from this first pass.

The two Iowa sets keep fixed roles:

- 2012 E1 `ff`, A1 `mf`, D2 `mf`, and G2 `pp` are fit inputs;
- 2001 E1, A1, D2, and G2 `mf` are held-out inputs.

The dynamic marks are ordered labels. They do not give pluck force, speed, or
contact position. The manifest fits passive loss only. It does not fit bow,
body, release, or gesture data.

## Check the public files

Run:

```sh
/usr/bin/python3 -I adapters/hlolli_wg_double_bass/build_manifest.py validate
```

The checked fit and reference files contain IDs, WAVE facts, and hashes. They
contain no local recording paths.

## Build a private bundle

Keep all recordings and output outside Git. Pass each file with its checked
ID. The order of the repeated options does not matter.

```sh
/usr/bin/python3 -I adapters/hlolli_wg_double_bass/build_manifest.py build \
  --fit-reference iowa2012-pizz-e-ff-open=/path/to/fit-e.wav \
  --fit-reference iowa2012-pizz-a-mf-open=/path/to/fit-a.wav \
  --fit-reference iowa2012-pizz-d-mf-open=/path/to/fit-d.wav \
  --fit-reference iowa2012-pizz-g-pp-open=/path/to/fit-g.wav \
  --heldout-source iowa2001-pizz-mf-open-e1-heldout=/path/to/heldout-e.wav \
  --heldout-source iowa2001-pizz-mf-open-a1-heldout=/path/to/heldout-a.wav \
  --heldout-source iowa2001-pizz-mf-open-d2-heldout=/path/to/heldout-d.wav \
  --heldout-source iowa2001-pizz-mf-open-g2-heldout=/path/to/heldout-g.wav \
  --analyzer /path/to/hlolli-wg-analyzer \
  --ffmpeg /path/to/ffmpeg \
  --output-dir /path/to/new-private-bundle
```

The builder asks the checked analyzer to read each WAVE file. It requires the
native held-out files as mono PCM16 at 44.1 kHz. It runs the checked FFmpeg and
libsoxr command twice for each file, then requires byte-identical results. It
also checks the output rate, channel count, PCM24 width, nearest-duration frame
count, and hash. The new bindings end in `-48k-soxr`; the native IDs stay in
each receipt row as `source_id`. This is an explicit derived binding. It does
not claim that the derived PCM equals the source PCM.

The output directory contains:

- `experiment.json`, with four fit and four check cases at 48 kHz;
- `fit.json`, with eight passive-decay objectives;
- four derived held-out WAVE files;
- `bindings.local.json`, with local paths for the analyzer; and
- `receipt.json`, with source and output facts plus the resolved analyzer and
  FFmpeg paths, hashes, versions, and fixed command.

The experiment has one baseline and 32 seeded random points. With eight cases,
that makes 264 render jobs. This is a bounded first pass, not a final model fit.
