# Cello G2, D3, and A3 passive-loss source receipt

Checked on 2026-08-25 and rechecked with passive-decay-v4 on 2026-08-31. This
note pins the public sound files for extending the first C2 passive-loss fit
to the other three open cello strings. Raw AIFF, derived WAVE, and fit output
stay outside Git.

Use the 2012 University of Iowa single-note `ff` files as fit inputs. Use the
first open-string notes in the 2001 Iowa `ff` range files as validation
inputs. The sessions use different players, cellos, mics, and channel layouts.
Validation loss affected the scalar rank, so the 2001 files are not an
untouched final audit set.

The source clips are suitable, but the current broadband straight-line decay
gate does not accept every one. Keep the failed results in the receipt. Do not
raise a limit just to make the sweep run, and do not write a fixed model from
this source pass.

## Rights and session facts

The [Iowa collection overview](https://theremin.music.uiowa.edu/MIS.html)
permits download and use in any project without restriction. It says that the
pre-2012 sessions used one Neumann KM 84 cardioid mic at 16-bit, 44.1 kHz. It
also says later sessions used Earthworks QTC-40 mics and that non-piano
instruments were recorded in the University of Iowa anechoic chamber.

The [2012 cello pitch page](https://theremin.music.uiowa.edu/MIS-Pitches-2012/MISCello2012.html)
names Yoo-Jung Chang, a 1923 Charles Quenoil cello, the 11 May 2012 session,
the anechoic chamber, Earthworks QTC40 mics at five feet, and 24-bit, 44.1 kHz
stereo files. Shane Hoose, Brian Penkrot, and Zach Zubow are named as the
engineers. The individual-pitch files below come from that page.

The [2001 cello page](https://theremin.music.uiowa.edu/MIScello.html) names
Jean Montes, a cello labelled there as a 1736 Antonius Stradivarius, the 16
November 2001 session, and the anechoic chamber. It names Michael Cash and
Andrew Stuck-Marcell as technicians, a Neumann KM 84 cardioid mic at five
feet, a Mackie 1402-VLZ mixer, a Panasonic SV-3800 DAT recorder, and 16-bit,
44.1 kHz mono files.

Neither cello page states A4, pluck position, string make, or a measured pluck
force. The pitch names identify the played notes and strings; they do not prove
a tuning reference.

## 2012 fit inputs

The raw files are signed 24-bit big-endian PCM AIFF at 44.1 kHz with two
channels. FFmpeg 8.1.2 made signed 24-bit little-endian PCM WAVE work files.
It kept the source rate, channel count, gain, and sample values.

| String | Official source | Raw bytes and frames | Raw SHA-256 |
|---|---|---:|---|
| G2 | [`Cello.pizz.ff.sulG.G2.stereo.aif`](https://theremin.music.uiowa.edu/sound%20files/MIS%20Pitches%20-%202014/Strings/Cello/Cello.pizz.ff.sulG.G2.stereo.aif) | 1,256,458 bytes; 209,301 frames | `b2948217098d0019a0c632e48f61e0f76b6ddade9b85653cb1e33adcb1e91cb0` |
| D3 | [`Cello.pizz.ff.sulD.D3.stereo.aif`](https://theremin.music.uiowa.edu/sound%20files/MIS%20Pitches%20-%202014/Strings/Cello/Cello.pizz.ff.sulD.D3.stereo.aif) | 870,376 bytes; 144,954 frames | `a85c926d0ffdae969d611b651103bf013ced050f84ab4d7cd03dee0fccbc0a4b` |
| A3 | [`Cello.pizz.ff.sulA.A3.stereo.aif`](https://theremin.music.uiowa.edu/sound%20files/MIS%20Pitches%20-%202014/Strings/Cello/Cello.pizz.ff.sulA.A3.stereo.aif) | 1,010,386 bytes; 168,289 frames | `d50a238556ef57d0a7c166771a84bdbee766dd8e25a26dcfe9ccbbc2cfe94c05` |

The work trims follow the first-pass source receipt. Each span uses half-open
source-frame bounds.

| String | Source frames kept | Work frames and span | Work file | Work SHA-256 |
|---|---:|---:|---|---|
| G2 | `[971, 209301)` | 208,330; 4.724036 s | `pizz-G2.wav` | `d0ed42947553acff74b795da3316e80731375bde08f5023a8734cbe22c23a72e` |
| D3 | `[3666, 144954)` | 141,288; 3.203810 s | `pizz-D3.wav` | `d27fbe2e24317c694a9997c4045ce004e2aa37ed5e4ffd96904ea3a14a32c40e` |
| A3 | `[0, 168289)` | 168,289; 3.816077 s | `pizz-A3.wav` | `c60aecbe28fbdc2c7c837ffdb542d10473fcf2483f45ce0c9ae54ab8f5a23e02` |

The bounded pitch check measured G2 at 98.2598 Hz, D3 at 146.7681 Hz, and A3
at 220.1781 Hz. Their one-string A4 values are 441.17, 439.81, and 440.36 Hz.
Use each measured pitch as a recording check. Do not force the three files to
one unstated A4.

## 2001 held-out inputs

The held-out sources are signed 16-bit big-endian PCM AIFF at 44.1 kHz with
one channel. Each range starts with its named open string.

| String | Official source | Raw bytes, frames, and span | Raw SHA-256 |
|---|---|---:|---|
| G2 | [`Cello.pizz.ff.sulG.G2B2.aiff`](https://theremin.music.uiowa.edu/sound%20files/MIS/Strings/cello/Cello.pizz.ff.sulG.G2B2.aiff) | 5,022,286 bytes; 2,511,090 frames; 56.940816 s | `2af87cad937f7b65287d2ea4637be79ac33773c0cf34674c09254a8f373c7db0` |
| D3 | [`Cello.pizz.ff.sulD.D3B3.aiff`](https://theremin.music.uiowa.edu/sound%20files/MIS/Strings/cello/Cello.pizz.ff.sulD.D3B3.aiff) | 6,759,262 bytes; 3,379,578 frames; 76.634422 s | `bb7b5cf318805a48ee2c743ce527b6cb9e1ef5d69bb0dbaeddeb758c94320b9f` |
| A3 | [`Cello.pizz.ff.sulA.A3B3.aiff`](https://theremin.music.uiowa.edu/sound%20files/MIS/Strings/cello/Cello.pizz.ff.sulA.A3B3.aiff) | 1,856,730 bytes; 928,312 frames; 21.050159 s | `e5a6d8d8ce6fa2fc53e06e9fb834eb6f60643a512c5b721b64b5621e77847cc8` |

### Exact first-note trims

A 20 ms RMS scan found the first open-string attack relative to the peak of
the first note. Each start keeps exactly 4,410 source frames, or 100 ms,
before that onset. FFmpeg `silencedetect` at -60 dBFS with a 40 ms minimum
found the start of the first 1.5-second inter-note gap. Each end uses that
sample. The half-open spans therefore keep the full first release and no part
of the next attack.

| String | Onset frame | Source frames kept | Work frames and span | Work SHA-256 |
|---|---:|---:|---:|---|
| G2 | 332,349 | `[327939, 861715)` | 533,776; 12.103764 s | `d5eb0f46d53790bfe3a14b23befeb0202e9c41f757caa96bbc054f2ef3118f36` |
| D3 | 122,182 | `[117772, 427197)` | 309,425; 7.016440 s | `2be3c37bbe67fc911302b0a0edfc9cb42df540e7a2943243ae331781650d60df` |
| A3 | 78,756 | `[74346, 257473)` | 183,127; 4.152540 s | `2c36a4366ddf86a2c7da7ae1eb7ff03a00b65dd14e954b3e4cd77ccc8d404148` |

The work files are named `iowa-2001-pizz-G2-check.wav`,
`iowa-2001-pizz-D3-check.wav`, and `iowa-2001-pizz-A3-check.wav`. They are
signed 16-bit little-endian PCM WAVE at 44.1 kHz with one channel. Their byte
sizes are 1,067,630, 618,928, and 366,332.

The conversion used this form, with the bounds from the table:

```sh
ffmpeg -hide_banner -loglevel error -y \
  -i "$RAW_DIR/Cello.pizz.ff.sulG.G2B2.aiff" \
  -af 'atrim=start_sample=327939:end_sample=861715' \
  -c:a pcm_s16le -ar 44100 -ac 1 \
  "$WORK_DIR/iowa-2001-pizz-G2-check.wav"
```

Repeat it with the D3 and A3 names and bounds. A frame-by-frame check found
that every output sample equals the matching signed source sample. The command
does not resample, mix, change gain, filter, gate, or remove noise. The G2
source contains 16 full-scale samples at the attack. D3 and A3 contain none.
Keep that G2 limit in every result receipt.

## Passive-decay gate

The first check used a 2.5 dB straight-line residual limit. It rejected the
A3 fit and G2 and D3 validation tails even though each file held one clean
decay with no second onset. The result below records that finding and the
current `passive-decay-v4` result.

| Role | String | T60 | Dynamic range | Support | Line residual | Second onset | v4 result |
|---|---|---:|---:|---:|---:|---|---|
| 2012 fit | G2 | 5.2855 s | 35.045 dB | 2.6789 s | 2.150 dB | no | pass |
| 2012 fit | D3 | 3.7397 s | 35.093 dB | 1.6812 s | 2.378 dB | no | pass |
| 2012 fit | A3 | 7.8205 s | 35.134 dB | 2.9982 s | 3.543 dB | no | pass |
| 2001 check | G2 | 12.4263 s | 35.161 dB | 6.7197 s | 4.005 dB | no | pass; clipped attack limit |
| 2001 check | D3 | 6.6467 s | 35.010 dB | 2.9782 s | 2.944 dB | no | pass |
| 2001 check | A3 | 4.3863 s | 35.059 dB | 2.5193 s | 2.230 dB | no | pass |

The v3 gate requires at least 20 dB of decay and permits up to 5 dB of line
residual. A named clean two-rate-tail test proves why the old 2.5 dB limit was
too low. A second test rejects a wide-range tail above the new 5 dB limit.
The second-onset, negative-slope, time-support, and 35 dB stop rules stay in
place. V4 adds a waveform-change check so slow modal beating does not look
like a second pluck. The fit tool for this first v4 check has SHA-256
`f2177e793885bfab90d0fca57dfa83635d49cd70b9004a7b4d9ac3aece0142cd`.

The 2001 `mf` and `pp` open-string files were also checked. None gives a full
G2, D3, and A3 held-out set that passes the current rule. The `mf` G2 file has
raw SHA-256
`144010f899db0d098e40fb09dcf2fa8b65ad50867ef534d98fb4ca80580462ea`;
it avoids full-scale samples but still has a 3.065 dB line residual and only
27.592 dB of range before the inter-note gap. Keep it as a useful G2
diagnostic, not as a silent replacement for the pinned `ff` check.

## Completed scalar pass

1. Bind the three pinned 2012 WAVE files only to fit cases and the three pinned
   2001 WAVE files only to check cases.
2. Keep separate G, D, and A parameters mapped to cello strings 1, 2, and 3.
   Do not couple them to the provisional C-string value.
3. Render each open string at the pitch measured for its 2012 recording. Keep
   exact A4 constructor math as model geometry truth.
4. Test the non-straight A3 fit and G2 and D3 validation tails before changing
   the gate.
5. Run selection only after every fit and validation file passes its declared
   input rules. Keep all selections provisional and outside Git.
6. Do not change the fixed profile or its `violin-derived` evidence status from
   this first four-string pass.

All six files passed the v3 input rules, and the three scalar runs completed.
The [four-string sweep report](cello-four-string-passive-loss-v3.md) records
the full scores and the provisional C, G, D, and A values. No raw source,
WAVE work file, private recording, or fit output belongs in either repository.
