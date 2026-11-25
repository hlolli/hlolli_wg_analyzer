# Paired note spectra

Save an audio-to-audio alignment with `align REFERENCE.wav TARGET.wav`. Pass
the same WAVs in the same order to `compare-note-spectra`. The command checks
their SHA-256 hashes against the alignment.

If you aligned downsampled or mono copies, pass those with
`--alignment-reference` and `--alignment-target` and keep the full-resolution
WAVs as the two positional inputs. The command checks the copies against the
alignment and checks that their durations match the originals within sample
rounding. Each copy must start at the same time as its original.

Give the command one of these sources for note spans.

- `--notes notes.csv` reads spans on the reference WAV's sample clock. Use
  `--notes-clock target` for spans on the target WAV. Use `id`, `start_sample`,
  and `end_sample` on the original WAV's clock. Samples start at zero and
  the end is exclusive. For pitch verification, fill in either `expected_hz` or
  `midi_note` on each row. The optional `confidence` column takes a value
  between 0 and 1 and defaults to 1. Other columns are labels.
- `--score-alignment score.hwa-align` reads note matches from
  `align --score SCORE.csv AUDIO.wav`. The score alignment must refer to one
  of the two WAVs. The command takes the expected pitch from the MIDI note
  and supports match labels such as `voice` and `score_position` for grouping.

```csv
id,start_sample,end_sample,midi_note,register,string
n1,48000,84600,55,low,G
n2,89000,128000,62,middle,D
```

The command takes the central 60% of the supplied span by default and maps
those bounds onto the other recording using the saved anchors.
`--held-fraction` changes that share. The pair confidence is the lower of the
note's supplied confidence and the lowest anchor confidence used by the held
span. Pairs below `--min-pair-confidence` (default 0.35) are skipped.
When a note supplies an expected pitch, the dominant pitch on each side must
match within 50 cents on a majority of its audible held windows.

Repeat `--band LOW:HIGH` to choose bands in Hz. The defaults are 2–3 kHz and
3–4 kHz. Bands must fit within the lower of the two Nyquist frequencies.
The command averages Hann-windowed power across the channels and accepted
windows. Power is in squared full-scale units. The absolute difference is
`10 log10(target band power / reference band power)`. The normalized
difference compares each band's share of its recording's total power over
the shared frequency range. Set `--normalize-band LOW:HIGH` to use a narrower
range for that total, for example `180:8000`. Positive values mean more
power, or a larger share of power, on the target side. A dB difference is
`null` when the pair was skipped or a power in its ratio is zero.

The `hwa-paired-note-spectra` JSON contains one row per note, with full and
held bounds on each WAV's original clock, the measured pitches and
confidence, window counts, band powers, and dB differences. Unavailable bounds
and confidence values are `null`. The summary counts
usable and skipped pairs, tallies skip reasons, and gives band means with each
usable note weighted equally. Repeat `--group-by LABEL` to summarize by
each label. Notes with an empty value contribute to the overall summary.
Use `--output FILE.json` to save the report. The command writes to standard
output by default.

Each side reports its FFT size and hop, the number of complete windows
checked, and the number used to average power. The overall window counts sum
the used windows on each side of usable pairs. Those counts can differ when
the recordings hold the same note for different lengths of time.

The command decodes only the audio needed for complete FFT windows in the
held spans. `--max-frames` and `--max-transforms` limit that work across both
WAVs. The report includes actual counts. The hash check streams the source
files without decoding them.
