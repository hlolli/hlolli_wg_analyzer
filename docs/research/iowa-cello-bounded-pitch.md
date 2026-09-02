# Iowa cello open-string pitch rerun

Checked on 2026-08-23. This note records a repeatable expected-note pitch check
for the open Iowa files listed in the cello source catalog. Raw sound, work
WAVE files, and result JSON remain outside Git.

## Project boundary

The reusable check lives in `tools/bounded_pitch.py`; its focused test lives in
`tests/test_bounded_pitch.py`. Local result JSON must stay outside Git. The
cello repository keeps only the public source, rights, download, and trim
receipt. It does not copy the pitch code or frame measures.

The analyzer base was version 1.1.0 at commit
`ec5f751b9d0d5e087cb4eddf9b38e34276cc678b`. The tool has SHA-256
`6ca7aadf25e3f4b4787a0e89c156ab5f5125b3e4e04e6fe36769e7826a6d622d`.
The test has SHA-256
`9fbd5e9f806b49de4deb012d92018a78f5c6a20131637d0f62e0d0993e4109db`.

## Public source facts

The [University of Iowa collection overview](https://theremin.music.uiowa.edu/MIS.html)
allows the files to be downloaded and used in any project without restriction.
The [2012 cello page](https://theremin.music.uiowa.edu/MIScello2012.html) names
Yoo-Jung Chang, a 1923 Charles Quenoil cello, an anechoic chamber, Earthworks
QTC40 mics at five feet, and the 11 May 2012 session. The
[individual-pitch page](https://theremin.music.uiowa.edu/MIS-Pitches-2012/MISCello2012.html)
supplies the 24-bit, 44.1 kHz stereo `ff` files used here. The source does not
state A4, bow force, bow speed, or bow position.

| File | Direct source | Raw bytes | Raw SHA-256 | Local receipt time |
|---|---|---:|---|---|
| `arco-C2.aif` | [AIFF](https://theremin.music.uiowa.edu/sound%20files/MIS%20Pitches%20-%202014/Strings/Cello/Cello.arco.ff.sulC.C2.stereo.aif) | 1,960,720 | `56798a31bfb79a784baba213e9cffd9ad1565b8bc7f3ce08b557a82f432d6725` | 19:04:35 +03:00 |
| `arco-G2.aif` | [AIFF](https://theremin.music.uiowa.edu/sound%20files/MIS%20Pitches%20-%202014/Strings/Cello/Cello.arco.ff.sulG.G2.stereo.aif) | 2,045,572 | `609f2ad4c8981a11a4c58b27be834b43518be11e808ea85c3395e229c5dca784` | 19:04:49 +03:00 |
| `arco-D3.aif` | [AIFF](https://theremin.music.uiowa.edu/sound%20files/MIS%20Pitches%20-%202014/Strings/Cello/Cello.arco.ff.sulD.D3.stereo.aif) | 1,765,564 | `13c450bd0e3862f938313f9e7cdf0e1d244815d50eb132182e9f1a90a57e4259` | 19:04:48 +03:00 |
| `arco-A3.aif` | [AIFF](https://theremin.music.uiowa.edu/sound%20files/MIS%20Pitches%20-%202014/Strings/Cello/Cello.arco.ff.sulA.A3.stereo.aif) | 1,549,192 | `b697c7610dcd80c9958781909b5c7cca4c8850586e80a1c55e8b2892000c534c` | 19:04:47 +03:00 |
| `pizz-C2.aif` | [AIFF](https://theremin.music.uiowa.edu/sound%20files/MIS%20Pitches%20-%202014/Strings/Cello/Cello.pizz.ff.sulC.C2.stereo.aif) | 963,718 | `5424133b9a1ca4143cb84c833c00bd236498934eb94ed4e204b9f6f69738c51e` | 19:04:47 +03:00 |
| `pizz-G2.aif` | [AIFF](https://theremin.music.uiowa.edu/sound%20files/MIS%20Pitches%20-%202014/Strings/Cello/Cello.pizz.ff.sulG.G2.stereo.aif) | 1,256,458 | `b2948217098d0019a0c632e48f61e0f76b6ddade9b85653cb1e33adcb1e91cb0` | 19:04:54 +03:00 |
| `pizz-D3.aif` | [AIFF](https://theremin.music.uiowa.edu/sound%20files/MIS%20Pitches%20-%202014/Strings/Cello/Cello.pizz.ff.sulD.D3.stereo.aif) | 870,376 | `a85c926d0ffdae969d611b651103bf013ced050f84ab4d7cd03dee0fccbc0a4b` | 19:04:49 +03:00 |
| `pizz-A3.aif` | [AIFF](https://theremin.music.uiowa.edu/sound%20files/MIS%20Pitches%20-%202014/Strings/Cello/Cello.pizz.ff.sulA.A3.stereo.aif) | 1,010,386 | `d50a238556ef57d0a7c166771a84bdbee766dd8e25a26dcfe9ccbbc2cfe94c05` | 19:04:47 +03:00 |

All receipt times use 2026-08-23. These are local download times, not source
publication times.

## Work inputs and bounds

FFmpeg 8.1.2 made the work files as 24-bit little-endian PCM WAVE. It kept the
source sample rate and both channels. Sample checks in the first-pass receipt
show that each work file contains the chosen raw frames exactly after byte
order changes. No step changed gain or pitch, mixed channels, resampled,
denoised, gated, compressed, limited, or removed room sound.

Expected pitches use exact twelve-tone equal temperament from A4=440 Hz. The
four values are C2 `65.40639132514966`, G2 `97.99885899543733`, D3
`146.8323839587038`, and A3 `220.0` Hz. They only bound the search. The result
does not assume that the recording used A4=440.

| File | Work SHA-256 | Work bytes | Start-end | Window | Hop |
|---|---|---:|---:|---:|---:|
| `arco-C2.wav` | `0ca1b7c096bf336525c599432352623f3d940e404d74b0802140f9e4c766ffb1` | 1,840,746 | 0.900000-6.106327 s | 1.25 s | 0.35 s |
| `arco-G2.wav` | `14f0b278ac06b82302a817c08d6cbf57d9fdad94f348c984efd4f93fdd144d21` | 1,898,790 | 0.900000-6.325692 s | 1.25 s | 0.35 s |
| `arco-D3.wav` | `66de75fd4021572c1915ea13695e6820a2ed8e717546181b6d1d15dd968b588c` | 1,409,814 | 0.900000-4.477710 s | 1.25 s | 0.35 s |
| `arco-A3.wav` | `cdeff128b8df8b8328ce901c3cde41cd62727e0880b921b9a3cb1e32f205545b` | 1,418,532 | 0.900000-4.510658 s | 1.25 s | 0.35 s |
| `pizz-C2.wav` | `94dbd2259de5162cc16a4d5a8d9ef54590cdf7d3a9b3a37276c4483292714daa` | 959,268 | 0.220000-2.250000 s | 0.80 s | 0.22 s |
| `pizz-G2.wav` | `d0ed42947553acff74b795da3316e80731375bde08f5023a8734cbe22c23a72e` | 1,250,082 | 0.220000-2.250000 s | 0.80 s | 0.22 s |
| `pizz-D3.wav` | `d27fbe2e24317c694a9997c4045ce004e2aa37ed5e4ffd96904ea3a14a32c40e` | 847,830 | 0.220000-2.250000 s | 0.80 s | 0.22 s |
| `pizz-A3.wav` | `c60aecbe28fbdc2c7c837ffdb542d10473fcf2483f45ce0c9ae54ab8f5a23e02` | 1,009,836 | 0.205102-2.235102 s | 0.80 s | 0.22 s |

The work-file bounds map to the raw-file rules in the first receipt. Arco
starts 0.80 seconds after detected onset and ends 0.55 seconds before final
quiet. Pizzicato starts 0.12 seconds after onset and ends before the last 0.30
seconds of decay or 2.15 seconds after onset. Each start which leaves a full
window before the end bound is used. Both channels are checked without a mix.

## Method

The tool accepts plain integer PCM and integer PCM in WAVE-extensible files.
It checks the RIFF chunks, sample width, channel count, byte rate, block size,
and data length. It reads 8, 16, 24, and 32-bit PCM.

For each frame and each channel, it:

1. removes the frame mean and applies a symmetric Hann window;
2. converts the expected pitch plus or minus 80 cents to a lag range;
3. adds two lags at each edge so interpolation remains well formed;
4. finds the smallest mean squared sample difference in that range; and
5. refines that lag with a three-point parabola.

This is the difference step of YIN with a known-note bound. The bound rejects
octaves and upper partials. It does not fold an arbitrary pitch track after
analysis. The reported pitch is the median of every per-frame, per-channel
estimate pooled together. Channel medians remain separate output fields. The
old phrase “median of both channel tracks” was unclear and should be replaced
with this exact rule. The largest channel-median gap is 0.02252557 Hz in
`pizz-D3`.

The earlier general analyzer pitch track put both C2 files near 59.9 Hz. That
alias remains excluded. The bounded check does not replace the general track;
it serves a known-note recording check.

## Exact commands

The commands below show the exact settings. Run them from the analyzer root.
`WORK_DIR` names the decoded WAVE folder outside Git and `RESULT_DIR` names a
new output folder outside Git.

```sh
python3 -B tools/bounded_pitch.py "$WORK_DIR/arco-C2.wav" --expected-hz 65.40639132514966 --start-seconds 0.9 --end-seconds 6.106327 --window-seconds 1.25 --hop-seconds 0.35 --output "$RESULT_DIR/arco-C2.json"
python3 -B tools/bounded_pitch.py "$WORK_DIR/arco-G2.wav" --expected-hz 97.99885899543733 --start-seconds 0.9 --end-seconds 6.325692 --window-seconds 1.25 --hop-seconds 0.35 --output "$RESULT_DIR/arco-G2.json"
python3 -B tools/bounded_pitch.py "$WORK_DIR/arco-D3.wav" --expected-hz 146.8323839587038 --start-seconds 0.9 --end-seconds 4.477710 --window-seconds 1.25 --hop-seconds 0.35 --output "$RESULT_DIR/arco-D3.json"
python3 -B tools/bounded_pitch.py "$WORK_DIR/arco-A3.wav" --expected-hz 220.0 --start-seconds 0.9 --end-seconds 4.510658 --window-seconds 1.25 --hop-seconds 0.35 --output "$RESULT_DIR/arco-A3.json"
python3 -B tools/bounded_pitch.py "$WORK_DIR/pizz-C2.wav" --expected-hz 65.40639132514966 --start-seconds 0.22 --end-seconds 2.25 --window-seconds 0.80 --hop-seconds 0.22 --output "$RESULT_DIR/pizz-C2.json"
python3 -B tools/bounded_pitch.py "$WORK_DIR/pizz-G2.wav" --expected-hz 97.99885899543733 --start-seconds 0.22 --end-seconds 2.25 --window-seconds 0.80 --hop-seconds 0.22 --output "$RESULT_DIR/pizz-G2.json"
python3 -B tools/bounded_pitch.py "$WORK_DIR/pizz-D3.wav" --expected-hz 146.8323839587038 --start-seconds 0.22 --end-seconds 2.25 --window-seconds 0.80 --hop-seconds 0.22 --output "$RESULT_DIR/pizz-D3.json"
python3 -B tools/bounded_pitch.py "$WORK_DIR/pizz-A3.wav" --expected-hz 220.0 --start-seconds 0.205102 --end-seconds 2.235102 --window-seconds 0.80 --hop-seconds 0.22 --output "$RESULT_DIR/pizz-A3.json"
```

## Results

| File | Frames per channel | Pooled median | Estimate span | From A4=440 | One-string A4 value |
|---|---:|---:|---:|---:|---:|
| `arco-C2` | 12 | 64.9566 Hz | 64.9220-66.2639 Hz | -11.95 cents | 436.97 Hz |
| `arco-G2` | 12 | 98.0020 Hz | 97.7888-98.8868 Hz | +0.06 cents | 440.01 Hz |
| `arco-D3` | 7 | 147.1141 Hz | 146.5207-147.4436 Hz | +3.32 cents | 440.84 Hz |
| `arco-A3` | 7 | 220.5622 Hz | 220.3769-220.7153 Hz | +4.42 cents | 441.12 Hz |
| `pizz-C2` | 6 | 64.9184 Hz | 64.8971-65.1455 Hz | -12.96 cents | 436.72 Hz |
| `pizz-G2` | 6 | 98.2598 Hz | 98.1956-98.5852 Hz | +4.60 cents | 441.17 Hz |
| `pizz-D3` | 6 | 146.7681 Hz | 146.6891-146.9985 Hz | -0.76 cents | 439.81 Hz |
| `pizz-A3` | 6 | 220.1781 Hz | 220.1145-220.6494 Hz | +1.40 cents | 440.36 Hz |

The eight medians and spans reproduce the current first-pass table at every
shown digit. They also match the earlier raw-AIFF bounded YIN values at full
stored precision. The A4 and cents fields now use the exact open frequencies
rather than six-decimal display values. This only changes digits beyond those
shown in the table.

The eight one-string A4 values still span 436.72-441.17 Hz. They do not support
one common A4 claim. Keep exact constructor math as geometry truth and use
these values only as recording checks.

## Result hashes and checks

```text
33bd705bd616f44c79796d733e8aef41008ffabc12608e4cceb98b4a21615132  arco-A3.json
0550bf0b1ee4287d39c8ae7a8fd8e2f02a0ac65d78d9bd44196012a784c81a4e  arco-C2.json
6ccb04e51d455ba00a9cd4caf6e878fa09a36a33bd1b3bd866b2a6e9ac3655a3  arco-D3.json
fb0b39a7cacaad00a9dbac3896f27d8ee704e87b0e0c7f515cefbfb6289a3299  arco-G2.json
5a084e5956f1d37eb454051e98d16ede8577c61753f4e56ef4eaa0c03988e59a  pizz-A3.json
25475fc491bcb75bda201b8edf4dde7a321bce920c6d7f7dda47d056eaeb2ecf  pizz-C2.json
77ef30f0f37b59201c338a9b38b8cc42c3a847cb3edddd4216d94353371827ed  pizz-D3.json
bf0853e6cbaaef34b32226d2b25707a9460a0f529dbd947f0bc2bcc4130a8cf4  pizz-G2.json
```

The full eight-file run was repeated with the same tool and settings. Every
JSON file was byte-for-byte equal. The low-C result was also byte-for-byte
equal under Apple Python 3.9.6 and Homebrew Python 3.14.7. Both test runs pass:

```sh
python3 -B tests/test_bounded_pitch.py
```

The three focused tests cover a two-channel sine measurement, signed 24-bit
decoding, and 24-bit PCM in a WAVE-extensible header. The Iowa run covers all
eight real 24-bit stereo files.
