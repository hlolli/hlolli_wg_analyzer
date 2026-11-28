# Aligned speech spans

Use `compare-speech-spans` to find words and phones worth comparing by ear.
Supply two WAVs and a CSV of labels. Use shared timing for renders on the
same sample clock and a saved alignment for independent readings.

## Compare on a shared sample clock

```sh
mkdir excerpts
build/hlolli-wg-analyzer compare-speech-spans baseline.wav edited.wav \
  --shared-timing --spans joins.csv --output temporal.json --excerpt-dir excerpts
```

`--shared-timing` declares that the two renders use the same starting sample
and timing throughout. The command checks for equal sample rates and equal,
nonempty lengths in sample frames. Use either `--shared-timing` or
`--alignment` and reserve the alignment copy options for a saved alignment.

Supply unique IDs, words, and zero-based sample bounds on the shared clock.
The end sample is exclusive. A word row needs only `id`, `start_sample`,
`end_sample`, and `word`. Add a `phone` label for a phone. Use
`kind=boundary` with a `phone` or `boundary` label to name a window around a
join. The command also accepts the vowel and consonant kinds from aligned
comparisons. `confidence` gives the optional annotation confidence.

```csv
id,start_sample,end_sample,word,phone,kind
word-1,8000,12400,fann,,word
join-1,10600,11000,fann,a→n,boundary
```

The `temporal` object reports the actual window and hop in samples and
milliseconds. By default the window is a 15 ms Hann window and the hop is
5 ms. The analysis uses the channel average of each WAV. The `level` and
`spectral` arrays rank regions separately by the largest absolute change.
Each finding includes its label, range on both WAVs, duration, peak frame
time, two measurements, and candidate minus reference.

- `level_db` measures hop RMS in dB relative to the recording's full RMS, with
  the typical gain difference removed. A negative difference means the
  candidate is locally quieter. The `levels` object still reports the
  whole-file level difference.
- `log_spectral_change_db` measures the RMS first difference of log spectral
  power shares across frequency bands. `log_spectral_acceleration_db` uses
  the second difference of the same band values. Negative differences mean
  less motion or acceleration in the candidate. A strong attack present in
  both renders can have a high measured value and zero difference.

Regions join adjacent hops with waveform differences above a gain and
rounding tolerance and evidence of local level or spectral change. Each
hop needs at least 0.25 dB of support and a region needs a peak difference
of at least 0.5 dB to rank. The spectral support can use the instantaneous
difference in spectral shape. A region's rank uses the first or second
difference measurement. The frame center must fall inside its label.
`temporal.spans` includes the paired measurements at each annotation's
largest absolute difference, including unchanged controls and values that
fall below the ranking threshold.

Excerpts use the same sample range on both WAVs. A containing word label
lets the excerpt include the whole word, with up to 150 ms of context on
either side. The signed measures describe what changed in the two renders.
Listen to the clips to judge the change.

## Compare independent readings

Use an audio-to-audio alignment with phone labels for the buzz and consonant
contrast measures below. This mode retains the alignment confidence and
abstention rules. Its temporal report has status `requires-shared-timing`.

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

Alignment confidence takes the minimum across the annotation, supporting
anchors, and overlapping saved match or uncertain intervals. Consonant
confidence also covers the transition and neighboring vowel baseline. A row
stays unranked if its confidence falls below `--min-pair-confidence`
(default 0.35) or crosses a saved unmatched or uncertain interval. The report keeps mapped bounds wherever it has them.
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
percentiles. It checks for interleaved harmonics before accepting a longer
pitch period. With too little pitch motion or an unresolved octave jump
it reports `indeterminate`.

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
uses windows away from the harmonic grid. The comparison checks all retained
candidate peaks before choosing the strongest measured increase. A rise
of at least 3 dB sets the row rank and classification.
Changes in pitch-following harmonics can reflect voicing as well as buzz.

### Consonant transitions

The command maps a window up to 20 ms on either side of the annotated edge.
The window stays within the consonant and neighboring vowel bounds.
It can include a short gap between the labels. A gap with another labeled
phone makes that transition indeterminate. It compares against the middle
60% of the linked vowel. The report gives three measures.

- `level_contrast_db` from the strongest 6 ms RMS window through the transition.
- `attack_contrast_db` from the largest local rise in short-window power.
- `high_band_contrast_db` from peak 2–8 kHz power through the transition,
  including windows across the boundary. The band ends at the lower Nyquist
  limit of the two WAVs.

The level and attack measures use the vowel's broadband RMS as the baseline.
The high-band measure uses the vowel's mean power in the same band. A floor
50 dB below vowel broadband power limits the effect of rounding noise.
The largest loss of at least 3 dB sets the rank. A negative difference means
the candidate has less contrast. A nearby burst outside the mapped transition
can cast doubt on an unlocked edge. In that case, the row stays unranked with
`transition-timing-disagreement`. The report gives the nearby candidate cue
time alongside the saved mapped bounds.

### Listen to excerpts

With `--excerpt-dir`, each ranked finding can include a pair of PCM16 WAVs
with up to 150 ms of surrounding speech on each side. The command takes
both excerpts from the mapped sample bounds and keeps the original sample
rates and channel counts. It matches both excerpts at about −18 dBFS RMS
and lowers that target when needed to keep peak headroom. The report gives
the paths, bounds, and gains. The command shortens context when the alignment
provides less and skips playback for a pair if either side has zero RMS. The directory must exist and the WAV paths must
be free.

The report goes to standard output by default. `--output` saves JSON to
a file. `--max-frames`, `--max-transforms`, and `--max-work-bytes` bound
the combined work, including the level scan and any requested excerpts.
In shared timing mode, `--max-track-points` also limits the total number of
analysis hops checked across all annotations.
Use the measurements to choose where to listen. They give evidence about
specific sounds in this pair of readings.
