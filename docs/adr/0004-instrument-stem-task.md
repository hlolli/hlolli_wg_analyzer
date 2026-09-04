# Define instrument stems before choosing a separator

Status: accepted. The portable provider implements the contract. The later
HTDemucs six-stem adapter and `separate-instruments` command use it without
changing the contract.

The next inference task is `org.hlolli.instrument-stems-v1`. We define its
source clock, bundled WAVE payloads, stem IDs, instrument labels, provenance,
limits, and fixed output order before we choose a model or runtime; see
[Instrument stems version 1](../instrument-stems-v1.md). This lets a fake
runner test the payload seam and lets later native and web runners return
the same meaning without exposing model tensors.

A successful run must return at least one stem. Each stem gets one full-span
`instrument-region` event that links its stable stem ID to one selected
instrument label and an optional label score. Note inference, event-bundle
merging, and score output remain later tasks.
