# Recording dereverberation

Dereverberation can complement `isolate-instrument`. Room reflections carry
the same notes and can share the harmonics isolation tries to keep. In a
single recording, the room tail can overlap the instrument's natural decay.
Yasuraoka et al. heard struck strings turn staccato when dereverberation removed
part of their sustain. [Music dereverberation study](https://www.kecl.ntt.co.jp/people/kameoka.hirokazu/publications/Yasuraoka2011ICASSP05.pdf)

## Methods to compare

- **Measured room response.** With a matching room impulse response, estimate
  the original by regularized deconvolution. The regularization limits the
  boost at frequencies the measurement barely captures. Measure a sweep at
  the recording's source and microphone positions when possible. Changes in
  position, source radiation, or the room itself can reduce the fit.
  [Sweep measurement](https://aes.org/publications/elibrary/elibrary-page/?id=10211) ·
  [Regularized inversion](https://aes.org/publications/elibrary-page/?id=12098) ·
  [Sensitivity to changes in the response](https://doi.org/10.17743/jaes.2016.0047)
- **Unknown room response.** Weighted prediction error (WPE) estimates late
  reflections from earlier audio frames and subtracts them. NARA-WPE provides
  an MIT-licensed Python reference for mono and multiple microphones. Two
  distinct microphone signals give the predictor more information. A duplicated
  mono track gives it the same information twice. Use WPE as one comparison
  and delayed spectral subtraction tested on music as another. Both need
  checks on sustained tones and releases before we use them for measurements.
  [NARA-WPE paper](https://groups.uni-paderborn.de/nt/pubs/2018/ITG_2018_Drude_Paper.pdf) ·
  [Delayed spectral subtraction study](https://www.dafx.de/paper-archive/details/dIzKbja_aR1Rfu6wSQnQ5A)
- **Music-trained model.** RemFX publishes PyTorch weights for reverb removal
  with training examples drawn from guitars, bass, drums, and vocals. Its tests
  use added plug-in reverb, so room recordings of bowed strings and winds need
  their own tests. Try the reverb model by itself and compare it with the DSP
  baseline. If it preserves more of the instrument, check the weights' terms,
  CPU cost, and output agreement after ONNX export before adding a provider.
  [RemFX paper](https://arxiv.org/abs/2308.16177) ·
  [Code](https://github.com/mhrice/RemFx) ·
  [Published weights](https://zenodo.org/records/8218621) ·
  [Analyzer ONNX build](../onnx-runtime-build.md)

## Small comparison on the isolation fixtures

A NumPy prototype added two simulated half-second room responses to each of
the violin and viola clips. Each response had a direct tap, two early echoes,
and a seeded, decaying noise tail. The unprocessed clips served as references
and as controls without the added reverb.

| Method | Reduction in energy after the clip ended |
| --- | --- |
| Mono WPE with half of the predicted signal subtracted | 1.7–5.0 dB |
| Regularized deconvolution with the exact simulated response | 15.2–26.9 dB |

The blind pass also lowered the level of the untreated sustained viola clip by
4.8 dB. Post-clip energy alone would miss that loss. The inverse used the exact
filter that generated the test input. This test used two short clips and
simulated responses. It measured signal changes without a listening test or
an AI model comparison. [Violin clip](../../tests/fixtures/violin-isolation/README.md) ·
[Viola clip](../../tests/fixtures/viola-isolation/README.md)

## Recording-only mode

The `isolate-instrument --deconv` pass uses the isolated recording alone.
It fits the falling energy after distinct note releases. It checks the
phase motion in each candidate tail to protect smooth pitched decays, then
requires at least two releases with similar slopes. It uses the shared decay
rate to reduce late spectral energy within those spans. It keeps the stereo
channels together and preserves the source sample clock. The bundle saves
the estimated time for a 60 dB decay and the number of supporting releases.
`removed.wav` holds the difference between the source and the final stem.
[Isolation command](../../README.md) · [Blind decay estimation](https://labrosa.ee.columbia.edu/~dpwe/papers/Ratnam03-reverb.pdf)

The detector can leave audible reverb in continuous passages or in recordings
with too few clear releases. A natural decay with enough phase variation can
also resemble a room tail. Compare the stem and removed audio with the source
before using the result to measure an instrument's own decay.

The CLI test checks synthetic note releases with and without added room
responses, along with a recorded viola control. A wider evaluation can use
dry takes convolved with measured responses from several rooms and compare
close and room microphones from the same performance. Check attacks, pitch,
harmonic decay, quiet notes, and overlaps in addition to the energy after
known releases.
[Aachen room responses](https://www.iks.rwth-aachen.de/en/research/tools-downloads/databases/aachen-impulse-response-database/)
