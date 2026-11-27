# Aligned speech spans

Use `compare-speech-spans` to find words and phones worth comparing by ear.
It reads two WAVs of the same text, an audio-to-audio alignment, and a CSV of
word and phone labels supplied by the caller.

```sh
analyzer=build/hlolli-wg-analyzer
"$analyzer" align reference.wav candidate.wav --output speech.hwa-align
mkdir excerpts
"$analyzer" compare-speech-spans reference.wav candidate.wav \
  --alignment speech.hwa-align --spans phones.csv \
  --output speech.json --excerpt-dir excerpts
```

## Label the reference WAV

Use unique IDs and zero-based sample bounds on the original reference WAV.
The end sample is exclusive. Each row needs `id`, `start_sample`, `end_sample`,
`word`, `phone`, and `kind` (`vowel` or `consonant`). Quote fields containing
commas. The optional `confidence` is between 0 and 1 and defaults to 1.

```csv
id,start_sample,end_sample,word,phone,kind,vowel_id,edge,confidence
c1,7320,8900,talk,t,consonant,v1,end,0.96
v1,8900,16760,talk,ɔ,vowel,,,0.98
c2,16760,21120,talk,k,consonant,v1,start,0.96
```

Each consonant links to a neighboring vowel by `vowel_id`. Set `edge` to
`end` for a consonant-to-vowel release or `start` for a vowel-to-consonant
onset. It defaults to `end`. The linked vowel must lie on the indicated side
of the boundary. Supply the labels and the alignment from whatever tools
you use to mark the recording.

The command checks the WAV hashes in the alignment. If you aligned mono or
downsampled copies, give them with `--alignment-reference` and
`--alignment-target`. The original WAVs stay in the positional arguments.
The copies must start at the same time and have matching durations within
sample rounding. The report maps bounds back to each original sample clock.

## Read the report

The `buzz` and `consonant_loss` arrays have independent ranks. Each row gives
its word, phone, both timestamps, and alignment confidence. Look at
`reference`, `candidate`, and `difference` for the evidence. Differences
are candidate minus reference. Findings start at rank 1 in each array.

Alignment confidence takes the minimum across the annotation and supporting
anchors. Consonant confidence also covers the transition and neighboring
vowel baseline. A row stays unranked if its confidence falls below
`--min-pair-confidence` (default 0.35) or crosses a saved unmatched or
uncertain interval. The report keeps mapped bounds wherever it has them.
Unavailable values are `null`.

`levels` gives the whole-file RMS in dBFS across samples and channels and the
level change in dB. Phone measurements use the mono average of each WAV.
The non-clipping PCM16 gain-only control measured a 7 dB change within
0.01 dB, with consonant differences within 0.2 dB and no ranked findings.
Very quiet or low-bit-depth WAVs can change shape when requantized.

### Buzz in vowels

The analyzer checks Hann-windowed spectra up to 48 ms long with 75% overlap.
It tracks pitch from 60 to 550 Hz and needs at least four reliable estimates,
70% coverage, and a rise of at least 6% between the 10th and 90th pitch
percentiles. With too little pitch motion or unreliable pitch estimates it
reports `indeterminate`.

Peaks need at least 6 dB of local prominence, a level of at least −40 dB
relative to the whole window spectrum, and persistence across at least
55% of pitched windows. A fixed-frequency peak must also pass a width check
and appear away from the harmonic grid in at least 35% of pitched windows.
The harmonic measurement skips crossings with detected fixed lines.

The selected peak fields give frequency, persistence (a share from 0 to 1),
strength above surrounding bins, level relative to the window spectrum, and
harmonic number where applicable. When a candidate peak warrants a comparison
but the reference has no detected track, the report probes the same fixed
frequency or harmonic number in both recordings. Probe measurements appear
in `fixed_probe` or `harmonic_probe` and supply the difference. A fixed probe
uses windows away from the harmonic grid. Among the tested peaks, the
strongest increase of at least 3 dB sets the row rank and classification.
Changes in pitch-following harmonics can reflect voicing as well as buzz.

### Consonant transitions

The command maps a window up to 20 ms on either side of the annotated edge.
It compares against the middle 60% of the neighboring vowel. It measures:

- `level_contrast_db` from the strongest 6 ms RMS window on the consonant side.
- `attack_contrast_db` from the largest local rise in short-window power.
- `high_band_contrast_db` from peak 2–8 kHz power through the transition,
  including windows across the boundary. The band ends at the lower Nyquist
  limit of the two WAVs.

The level and attack measures use the vowel's broadband RMS as the baseline.
The high-band measure uses the vowel's mean power in the same band. A floor
50 dB below vowel broadband power limits the effect of rounding noise.
The largest loss of at least 3 dB sets the rank. A negative difference means
the candidate has less contrast. Use the edge timestamps to listen to that
transition in each file.

### Listen to excerpts

With `--excerpt-dir`, each ranked finding can include a pair of PCM16 WAVs
with up to 150 ms of surrounding speech on each side. The command takes
both excerpts from the mapped sample bounds and keeps the original sample
rates and channel counts. It uses one gain per WAV to match RMS and keep
headroom. The report gives the paths, bounds, and gains. It shortens context
if the alignment cannot support it and leaves the excerpt unavailable if
either side has zero RMS. The directory must exist and the WAV paths must
be free.

The report goes to standard output by default. `--output` saves JSON to
a file. `--max-frames`, `--max-transforms`, and `--max-work-bytes` bound
the combined work, including the level scan and any requested excerpts.
Use the measurements to choose where to listen. They give evidence about
specific sounds in this pair of readings.
