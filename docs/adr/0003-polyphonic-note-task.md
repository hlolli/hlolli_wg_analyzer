# Keep the first model task to polyphonic note events

Status: accepted and implemented by the opt-in Basic Pitch ONNX adapter.

The first trained-model task accepts one WAVE source clock and returns only
overlapping note spans with a selected pitch and score. We leave source
separation, score structure, rhythm, and ornaments to later tasks because
joining them here would hide which model made each claim. The task pins the
provider and model, saves the full run settings, and maps model frames to
source samples by one integer rule; see
[Polyphonic note events version 1](../polyphonic-note-events-v1.md).

Version 1 accepts mono 22050 Hz WAVE input and a supplied official-contract
Basic Pitch ONNX model. The native runner needs an opt-in ONNX Runtime build
and uses the CPU execution provider. The command is:

```sh
hlolli-wg-analyzer infer-note-events INPUT.wav \
  --model MODEL.onnx --output NEW.hwa-events
```

The adapter uses a fixed 256-sample model-frame grid. It adds 3840 left-pad
samples, sends 43844 samples to each model call, crops 15 frames from each end
of the 172-frame output, keeps 142 frames, and advances 36352 samples. We use
36352 instead of the 36164-sample step in Spotify's stock inference code so
the kept frames stay on one exact source grid.

This decision covers note events, model identity, exact source mapping, and
saved run facts. It does not cover pitch bends, source separation, rhythm, or
score output. WebNN remains a future compatibility test, not a version 1
promise.
