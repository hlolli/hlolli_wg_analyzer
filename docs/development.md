# Development

The project builds one static C library and one CLI. The library holds the
audio readers, analysis code, saved-file readers and writers, and higher-level
comparison tools. The CLI parses arguments and prints reports around that
library.

## Source layout

- `include/hlolli_wg_analyzer.h` is the installed C and C++ interface.
- `src/` holds the native library and CLI code.
- `tests/` holds unit, persistence, report, and CLI tests.
- `examples/` holds small input examples.
- `bench/` holds the opt-in benchmark and its result comparator.
- `regression/` holds fixed malformed and edge-case WAVE inputs.
- `fuzz/` holds bounded libFuzzer targets and seed data.
- `portable/` builds the path-free analysis core for WASI.
- `cmake/` holds the WASI toolchain and import check.

Keep file parsing, validation, and ownership in the library. CLI code should
only choose an operation, pass explicit inputs, and format the result. A new
public operation needs a default-options function, clear ownership rules, a
free function for owned results, and tests through the installed header.

## Generic data model

The analyzer treats names such as voices, parts, physical elements,
controllers, probes, stems, and cases as caller data. Core code must not give
an instrument-specific meaning to those names. Tests and examples should use
neutral names unless they test a public music term or a fixed analysis method.

Score kinds such as note, rest, ornament, and cadenza form part of the accepted
score format. Measures such as pitch, chroma, vibrato, and harmonic energy are
audio features. They do not tie the analyzer to one instrument.

## Compatibility

Saved artifacts record a schema version, a tool version, and one or more
method versions. Strings such as `stage1-1` through `stage9-1` are serialized
method IDs. Keep them stable when code, test, target, or document names change.
Changing a method ID requires a matching reader and compatibility plan.

Keep enum values and public struct fields stable within the 1.x API. Add new
fields only with a clear initialization, validation, save, load, report, and
free path. A writer must emit one canonical form. A reader may accept an older
tool version only when the saved method and schema remain compatible.

## Change checklist

Before sending a change for review:

1. Run a strict build and the full native test suite.
2. Run the focused test set for the code you changed.
3. Run `git diff --check` and inspect generated or ignored files.
4. Update the public header and README when behavior changes.
5. Add a regression case for any fixed parser or saved-file fault.

Use `docs/testing.md` for the full commands.
