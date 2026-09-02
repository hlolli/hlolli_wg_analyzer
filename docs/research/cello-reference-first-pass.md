# First-pass cello reference analysis

Checked on 2026-08-23. This analyzer report records the first local checks of
the open files listed in [the cello source catalog](../../../hlolli_wg_cello/docs/research/cello-recording-sources.md).
Raw and derived sound files remain outside Git. The work used FFmpeg 8.1.2
and `hlolli-wg-analyzer` 1.1.0 at analyzer commit
`ec5f751b9d0d5e087cb4eddf9b38e34276cc678b`.

No step changed gain, sample rate, channel count, or pitch. No step used a
gate, noise reduction, compression, limiting, or room removal. The fixed model
still has `violin-derived` sound data. These checks did not change a model
constant.

## Raw file receipt

The eight Iowa files are 24-bit, 44.1 kHz, two-channel AIFF. The two Bach files
are the original Ogg Vorbis downloads. The file names below are local receipt
names, not repository paths.

```text
56798a31bfb79a784baba213e9cffd9ad1565b8bc7f3ce08b557a82f432d6725  iowa/arco-C2.aif
609f2ad4c8981a11a4c58b27be834b43518be11e808ea85c3395e229c5dca784  iowa/arco-G2.aif
13c450bd0e3862f938313f9e7cdf0e1d244815d50eb132182e9f1a90a57e4259  iowa/arco-D3.aif
b697c7610dcd80c9958781909b5c7cca4c8850586e80a1c55e8b2892000c534c  iowa/arco-A3.aif
5424133b9a1ca4143cb84c833c00bd236498934eb94ed4e204b9f6f69738c51e  iowa/pizz-C2.aif
b2948217098d0019a0c632e48f61e0f76b6ddade9b85653cb1e33adcb1e91cb0  iowa/pizz-G2.aif
a85c926d0ffdae969d611b651103bf013ced050f84ab4d7cd03dee0fccbc0a4b  iowa/pizz-D3.aif
d50a238556ef57d0a7c166771a84bdbee766dd8e25a26dcfe9ccbbc2cfe94c05  iowa/pizz-A3.aif
1e50d9e513c37725e8d8f378787a87330b040505ec679f9e919b01f2f4eae25f  bach/chris-bwv1007-prelude.ogg
2f27d08dbe734de19160a1ee3109a811a668be5314b1253d281b2ee6861d7d25  bach/john-michel-bwv1007-prelude.ogg
```

| File | Bytes | Source span | Source format |
|---|---:|---:|---|
| `arco-C2.aif` | 1,960,720 | 7.407664 s | PCM s24be, 44.1 kHz, 2 ch |
| `arco-G2.aif` | 2,045,572 | 7.728345 s | PCM s24be, 44.1 kHz, 2 ch |
| `arco-D3.aif` | 1,765,564 | 6.670113 s | PCM s24be, 44.1 kHz, 2 ch |
| `arco-A3.aif` | 1,549,192 | 5.852381 s | PCM s24be, 44.1 kHz, 2 ch |
| `pizz-C2.aif` | 963,718 | 3.639705 s | PCM s24be, 44.1 kHz, 2 ch |
| `pizz-G2.aif` | 1,256,458 | 4.746054 s | PCM s24be, 44.1 kHz, 2 ch |
| `pizz-D3.aif` | 870,376 | 3.286939 s | PCM s24be, 44.1 kHz, 2 ch |
| `pizz-A3.aif` | 1,010,386 | 3.816077 s | PCM s24be, 44.1 kHz, 2 ch |
| `chris-bwv1007-prelude.ogg` | 8,025,732 | 154.712604 s | Vorbis, 48 kHz, 2 ch |
| `john-michel-bwv1007-prelude.ogg` | 2,672,071 | 130.272653 s | Vorbis, 44.1 kHz, 2 ch |

## Trim and conversion receipt

FFmpeg `silencedetect` used a -60 dBFS threshold and a 40 ms minimum span.
Each Iowa start keeps 100 ms before the first sound when the source has it.
Each end keeps 300 ms after the final detected sound when the source has it.
The four pizzicato files have less than 300 ms of final quiet, so their work
files keep the source through its end. The Chris file has no final quiet span,
so it also keeps the source through its end.

| File | Lead quiet ends | Final quiet starts | Kept source span | Work span | Work bytes |
|---|---:|---:|---:|---:|---:|
| `arco-C2` | 0.361179 s | 6.917506 s | 0.261179-7.217506 s | 6.956327 s | 1,840,746 |
| `arco-G2` | 0.230635 s | 7.006327 s | 0.130635-7.306327 s | 7.175692 s | 1,898,790 |
| `arco-D3` | 0.305737 s | 5.233447 s | 0.205737-5.533447 s | 5.327710 s | 1,409,814 |
| `arco-A3` | 0.309637 s | 5.270295 s | 0.209637-5.570295 s | 5.360658 s | 1,418,532 |
| `pizz-C2` | 0.114739 s | 3.395918 s | 0.014739-source end | 3.624966 s | 959,268 |
| `pizz-G2` | 0.122018 s | 4.543696 s | 0.022018-source end | 4.724036 s | 1,250,082 |
| `pizz-D3` | 0.183129 s | 3.058141 s | 0.083129-source end | 3.203810 s | 847,830 |
| `pizz-A3` | 0.085102 s | 3.627800 s | source start-source end | 3.816077 s | 1,009,836 |
| `chris-bwv1007-prelude` | 0.260875 s | none | 0.160875-source end | 154.551729 s | 44,511,000 |
| `john-michel-bwv1007-prelude` | none | 129.789660 s | source start-130.089660 s | 130.089660 s | 34,421,826 |

The work files use 24-bit little-endian PCM WAVE at each source rate and keep
both channels. Sample checks found an exact match between every Iowa work file
and its chosen raw frames after the byte-order change. The decoded Bach work
files also match the selected 24-bit FFmpeg decode frames exactly. This check
rules out an added gain, resample, channel mix, or filter in the conversion.

```text
0ca1b7c096bf336525c599432352623f3d940e404d74b0802140f9e4c766ffb1  arco-C2.wav
14f0b278ac06b82302a817c08d6cbf57d9fdad94f348c984efd4f93fdd144d21  arco-G2.wav
66de75fd4021572c1915ea13695e6820a2ed8e717546181b6d1d15dd968b588c  arco-D3.wav
cdeff128b8df8b8328ce901c3cde41cd62727e0880b921b9a3cb1e32f205545b  arco-A3.wav
94dbd2259de5162cc16a4d5a8d9ef54590cdf7d3a9b3a37276c4483292714daa  pizz-C2.wav
d0ed42947553acff74b795da3316e80731375bde08f5023a8734cbe22c23a72e  pizz-G2.wav
d27fbe2e24317c694a9997c4045ce004e2aa37ed5e4ffd96904ea3a14a32c40e  pizz-D3.wav
c60aecbe28fbdc2c7c837ffdb542d10473fcf2483f45ce0c9ae54ab8f5a23e02  pizz-A3.wav
ff5b52ebb46bc5114d19e9471507270276314dd0d67bb622f1263ea2c8d3d407  chris-bwv1007-prelude.wav
e325a4177ca7d08a5fee24d70df3243dd8f773b40cb52c30d8760196c82e0142  john-michel-bwv1007-prelude.wav
```

## Iowa open-string pitch

Pitch came from the trimmed PCM work files. Exact frame checks bind each work
file to the chosen source AIFF frames, so the work-file rerun reproduces the
earlier raw-file values. The analyzer-side check uses the known open-string
note to bound a YIN squared-difference search to plus or minus 80 cents. This
stops a weak C2 fundamental from turning into an octave or harmonic result.
It uses 1.25-second arco frames, 0.80-second pizzicato frames, several held
frames, and both source channels. The reported pitch pools every per-frame,
per-channel estimate and takes their median. It also reports each channel's
median. The largest gap between channel medians is 0.023 Hz.

Arco checks start 0.80 seconds after onset, end 0.55 seconds before final
quiet, and use a 0.35-second hop. Pizzicato checks start 0.12 seconds after
onset, stop before the last 0.30 seconds of decay or after 2.15 seconds, and
use a 0.22-second hop. The full method, commands, tests, and result hashes are
in [the bounded-pitch rerun](iowa-cello-bounded-pitch.md).

The analyzer's general frame pitch track put both C2 files near 59.9 Hz. That
result is an alias and is not used below.

The A4 column is only the A4 value that would put that one measured pitch at
equal temperament. It does not state the session tuning.

| File | Median pitch | Estimate span | From A4=440 | One-string A4 value |
|---|---:|---:|---:|---:|
| `arco-C2` | 64.9566 Hz | 64.9220-66.2639 Hz | -11.95 cents | 436.97 Hz |
| `arco-G2` | 98.0020 Hz | 97.7888-98.8868 Hz | +0.06 cents | 440.01 Hz |
| `arco-D3` | 147.1141 Hz | 146.5207-147.4436 Hz | +3.32 cents | 440.84 Hz |
| `arco-A3` | 220.5622 Hz | 220.3769-220.7153 Hz | +4.42 cents | 441.12 Hz |
| `pizz-C2` | 64.9184 Hz | 64.8971-65.1455 Hz | -12.96 cents | 436.72 Hz |
| `pizz-G2` | 98.2598 Hz | 98.1956-98.5852 Hz | +4.60 cents | 441.17 Hz |
| `pizz-D3` | 146.7681 Hz | 146.6891-146.9985 Hz | -0.76 cents | 439.81 Hz |
| `pizz-A3` | 220.1781 Hz | 220.1145-220.6494 Hz | +1.40 cents | 440.36 Hz |

The eight one-string values span 436.72-441.17 Hz. They do not support one
common A4 claim. The C string also changes during the note: the bowed period
falls from about 66.6 Hz near the start to a stable 64.95 Hz late in the note;
the pluck falls from about 65.33 Hz to about 64.85 Hz. Both channels show the
same time track. Stage 2 should keep the exact A4 constructor math as geometry
truth and use these tracks as recording checks, not copy one measured value
into an open-string constant.

## Bach whole-piece checks

`yt-dlp` 2026.07.04 read the public metadata for the official Yo-Yo Ma video,
but its audio requests returned HTTP 403. The current
[PO token guide](https://github.com/yt-dlp/yt-dlp/wiki/PO-Token-Guide/current)
notes that some YouTube clients now need a token. No open recording license
was found, so this work did not read browser cookies, add a token provider, or
download that audio. The two open Commons files below did not need YouTube.

The analyzer ran on the trimmed, two-channel PCM files. It also ran on each
channel alone. The values serve as repeatable input checks, not cello fitting
targets, since both files lack a stated cello, mic, room, and tuning.
The run used 2,048-sample frames, a 512-sample hop, four-times true-peak
sampling, and a -60 dBFS activity threshold.

| Measure | Chris, CC0 | John Michel, CC BY-SA 3.0 |
|---|---:|---:|
| Work span | 154.551729 s | 130.089660 s |
| Integrated loudness, stereo | -15.78 LUFS | -24.40 LUFS |
| Loudness range | 9.9 LU | 8.7 LU |
| Spectrum centre | 570.96 Hz | 405.57 Hz |
| 85% rolloff | 923.08 Hz | 592.66 Hz |
| Stereo correlation | +0.8919 | -0.8946 |
| Side/mid RMS ratio | 0.2511 | 4.1676 |

Chris channel 1 has 0.9578 true peak and no clipped samples. Channel 2 has
1.0014 true peak and 120 full-scale samples. Channel 2 is 1.34 dB louder by
RMS. Keep this file as stereo for listening, but include the channel and peak
policy in any numeric compare.

The John Michel channels are close in level but have opposite polarity for
most of the piece. Channel 1 has 0.04724 RMS and -27.06 LUFS. Channel 2 has
0.04326 RMS and -27.80 LUFS. An equal mono mix falls to 0.01057 RMS and
-39.63 LUFS, so it cancels much of the cello and shifts the spectrum. Use
channel 1 for a mono test. Do not use the equal mix as a model target.

John Michel takes 15.83% less time than Chris for the full Prelude. A replica
test needs score alignment or time-aware measures; matching wall-clock frames
would compare different notes.

## Roadmap use and limits

- Stage 2 can use the Iowa pitch tracks as held checks for all four open
  strings. The spread blocks an unstated common-A4 claim.
- Stage 3 can use Iowa bowed held spans and plucked decay, but these files do
  not state bow force, speed, or position. They cannot finish Stage 3.
- Stage 4 can use the clean Iowa ends for release checks. They do not isolate
  bridge input from body and mic response, so they cannot set the body alone.
- Stage 6 can repeat these measures through an analyzer manifest after the
  cello adapter exists. The expected-note low-C pitch check belongs in the
  analyzer before then. Fit and held-out sources still need separate splits.
- Stage 8 can use the Chris recording as the main open full-piece check and
  John Michel as a second pace and production check. The public-domain
  Mutopia Prelude score stays outside Git until the earlier plug-in tests pass.

The local score receipt is:

```text
57fccefc88f315d90e479ce400a6647fc86fbc2b5cbda64f5f29e6c58ea143ce  bwv1007-mids.zip
59449437fd1455537d3fd9d8963eac4a9f2d413f0d446b39539edbc7090fe619  bwv1007-1.mid
b3497b85f8351fb8812577c7faf49c157ef37876eadc71d76214f92ef10ccf6e  bwv1007-lys.zip
```

No YouTube audio, private recording, raw sound, work WAVE, frame export, or
fit output belongs in this repository.
