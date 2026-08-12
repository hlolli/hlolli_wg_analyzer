# WAVE reader regression cases

The regression target reads the fixed hex files in `corpus/wav`. The set
covers valid PCM, odd chunk padding, RF64, IEEE float data, and a cut-off RIFF
header. It uses a 1 MiB input cap and a 64-frame read block.

Build and run it:

```sh
cmake -S . -B build-regression \
  -DHLOLLI_WG_ANALYZER_BUILD_REGRESSION_TESTS=ON
cmake --build build-regression --target hwa_wav_regression
ctest --test-dir build-regression -R regression.wav-malformed
```

The runner sends each file to the WAVE reader once. It does not make or run
new mutations.
