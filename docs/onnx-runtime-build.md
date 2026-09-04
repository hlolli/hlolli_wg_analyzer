# ONNX Runtime build option

The default build does not need ONNX Runtime. It keeps the public static core,
the portable build, and existing clients free of a model-runtime dependency.
The opt-in native path uses the
[ONNX Runtime C API](https://onnxruntime.ai/docs/get-started/with-c.html).

To link ONNX Runtime into the command-line program, configure with an install
root that contains `onnxruntime_c_api.h` and the ONNX Runtime library:

```sh
cmake -S . -B build-onnx \
  -DHLOLLI_WG_ANALYZER_ENABLE_ONNX=ON \
  -DONNXRuntime_ROOT=/path/to/onnxruntime
cmake --build build-onnx
```

Configuration fails when the option is on and either the header or library is
missing. `ONNXRuntime_ROOT` must be an absolute path. On Windows, the root must
also contain `onnxruntime.dll`; the build copies that DLL beside the command-
line program. The build does not download or install a runtime. An installed
program still needs the ONNX Runtime shared library. On macOS and Linux, the
install keeps the absolute library search path found at configure time, so the
runtime must stay there. On Windows, its DLL must stay on the system library
search path.

To add the Basic Pitch real-model test, pass its pinned model path:

```sh
cmake -S . -B build-onnx \
  -DBUILD_TESTING=ON \
  -DHLOLLI_WG_ANALYZER_ENABLE_ONNX=ON \
  -DONNXRuntime_ROOT=/path/to/onnxruntime \
  -DHLOLLI_WG_ANALYZER_BASIC_PITCH_TEST_MODEL=/path/to/nmp.onnx
cmake --build build-onnx
ctest --test-dir build-onnx --output-on-failure \
  -R '^integration\.basic-pitch-onnx$'
```

The test option stays empty by default. Configuration checks the model's
pinned SHA-256. The test makes its own fixed WAVE input, runs the command
twice, checks the saved musical facts, and checks that both bundles have the
same bytes. It never downloads or copies the model into the source tree.

The HTDemucs test has a separate opt-in model path:

```sh
cmake -S . -B build-onnx \
  -DBUILD_TESTING=ON \
  -DHLOLLI_WG_ANALYZER_ENABLE_ONNX=ON \
  -DONNXRuntime_ROOT=/path/to/onnxruntime \
  -DHLOLLI_WG_ANALYZER_HTDEMUCS_TEST_MODEL=/path/to/htdemucs_6s_fp16weights.onnx
cmake --build build-onnx
ctest --test-dir build-onnx --output-on-failure \
  -R '^integration\.htdemucs-onnx$'
```

This option also stays empty by default. Configuration accepts only the
pinned six-stem file named below. The test makes a short fixed WAVE file,
runs real CPU inference, checks all six saved stems, and validates the saved
bundle.

The ONNX option builds the Basic Pitch and HTDemucs native runners into the
command-line program. Both use `CPUExecutionProvider`; version 1 has no
execution-provider flag. Supply an official-contract Basic Pitch ONNX model
and a mono 22050 Hz WAVE file:

```sh
build-onnx/hlolli-wg-analyzer infer-note-events INPUT.wav \
  --model MODEL.onnx --output NEW.hwa-events
```

For a pinned reference model, Spotify's `nmp.onnx` at commit
[`fa5997af`](https://github.com/spotify/basic-pitch/blob/fa5997af0a8210982619003269994a1be25eddf3/basic_pitch/saved_models/icassp_2022/nmp.onnx)
is 230444 bytes and has SHA-256
`2c3c1d144bfa61ad236e92e169c13535c880469a12a047d4e73451f2c059a0ec`.
Spotify publishes the project under
[Apache-2.0](https://github.com/spotify/basic-pitch/blob/fa5997af0a8210982619003269994a1be25eddf3/LICENSE).
This repository retains that license and notice under
`third_party/basic-pitch/` for its C decoder port. The model remains a
caller-supplied file and is not part of this repository.

For instrument stems, version 1 accepts only StemSplit's converted
`htdemucs_6s` file at revision
[`49df9b69`](https://huggingface.co/StemSplitio/htdemucs-6s-onnx/resolve/49df9b6989cf2150840ea65b0bef77a2e471b678/htdemucs_6s_fp16weights.onnx).
It is 136428532 bytes and has SHA-256
`7ce55792e2231c93fbf92de95f5fd5b3a5e6c89f7db690dfd693e8f1dce56869`.
Download it outside the source tree, then run:

```sh
build-onnx/hlolli-wg-analyzer separate-instruments INPUT.wav \
  --model /path/to/htdemucs_6s_fp16weights.onnx \
  --output NEW.hwa-events
```

The input must be mono or stereo 44100 Hz WAVE, and each decoded sample must
be finite and in `[-1, 1]`. The command writes stereo float32 stems for
`drums`, `bass`, `other`, `vocals`, `guitar`, and `piano`. It always checks
the built-in model hash, even when
`--expect-model-sha256` is absent. The model file stays caller-supplied and
is not part of this repository. Demucs and the converter use MIT licenses;
their notices live under `third_party/demucs/` and
`third_party/demucs-onnx/`.

Run note inference over those saved stems with the Basic Pitch model:

```sh
build-onnx/hlolli-wg-analyzer infer-stem-note-events NEW.hwa-events \
  --model /path/to/nmp.onnx \
  --output NEW-WITH-NOTES.hwa-events
```

This command reuses the checked stem files. It does not run HTDemucs again.
It opens the Basic Pitch model once and uses that session for each stem.

The program accepts one self-contained model file. It does not load ONNX
external-data weight files because those extra bytes would fall outside the
single-file hash. It caps the in-memory model at `INT_MAX` bytes even when
`--max-model-bytes` names a larger cap.

`--max-work-bytes` covers analyzer-owned audio buffers, decoded notes, and
saved result rows. HTDemucs stems stream through temporary files rather than
keeping the full result in memory. The limit does not cap memory that ONNX
Runtime uses to open or run a caller-supplied graph. The HTDemucs publisher
reports about 1.1 GB of run-time memory on an Apple M4 Pro. Run untrusted
models inside a process or system memory limit.

The program checks the model tensor names, float types, and shapes before it
runs. It also hashes the exact model bytes. Use
`--expect-model-sha256 HEX` when a workflow needs to pin those bytes. See
[Polyphonic note events version 1](polyphonic-note-events-v1.md) for the fixed
Basic Pitch schedule and decoder, and [Instrument stems version
1](instrument-stems-v1.md) for the HTDemucs schedule, output, and limits.

Use this command to inspect the current binary:

```sh
build-onnx/hlolli-wg-analyzer --json inference-capabilities
```

The capability result reports the linked runtime version and whether the
Basic Pitch and HTDemucs providers are available. The implemented tasks are
`org.hlolli.polyphonic-note-events-v1` and
`org.hlolli.instrument-stems-v1`. A default build still reports both tasks as
implemented but unavailable because it has no linked runtime. This reports
compiled support only; it does not find, open, or check a model file.

WebNN remains future validation work. The converted HTDemucs graph has a
browser WASM demo, but that does not prove WebNN support. ONNX Runtime Web
lists WebNN as an
[experimental execution provider](https://onnxruntime.ai/docs/tutorials/web/ep-webnn.html).
This native build does not promise WebNN support, browser support, or equal
results on another backend.
