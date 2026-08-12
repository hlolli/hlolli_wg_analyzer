# Testing

## Native suite

Run the full suite with strict compiler checks:

```sh
cmake -S . -B build-strict \
  -DCMAKE_BUILD_TYPE=Release \
  -DHLOLLI_WG_ANALYZER_STRICT=ON \
  -DHLOLLI_WG_ANALYZER_BUILD_REGRESSION_TESTS=ON \
  -DBUILD_TESTING=ON
cmake --build build-strict --parallel
ctest --test-dir build-strict --output-on-failure
```

CTest names use these prefixes:

- `unit.` for library, saved-file, and report checks
- `cli.` for end-to-end command checks
- `regression.` for fixed malformed-input cases
- `benchmark.` for benchmark contract checks
- `fuzz.` for bounded seed-corpus runs
- `wasm.` for the WASI module and import check

Use `ctest --test-dir build-strict -N` to list the tests. Use `-R` to run a
matching group.

## Installed clients

The native suite compiles a client against the build tree. CI also installs
the project and compiles the same source as C11 and C++17 against the installed
header and static library.

On a Unix-like host:

```sh
cmake --install build-strict --prefix install-test

cc -std=c11 -Wall -Wextra -Wpedantic -Werror \
  -Iinstall-test/include tests/public_client_tests.c \
  install-test/lib/libhlolli_wg_analyzer_core.a -lm \
  -o install-test/client-c
install-test/client-c

c++ -x c++ -std=c++17 -Wall -Wextra -Wpedantic -Werror \
  -Iinstall-test/include -c tests/public_client_tests.c \
  -o install-test/client-cxx.o
c++ install-test/client-cxx.o \
  install-test/lib/libhlolli_wg_analyzer_core.a -lm \
  -o install-test/client-cxx
install-test/client-cxx
```

## Benchmarks

```sh
cmake -S . -B build-bench \
  -DCMAKE_BUILD_TYPE=Release \
  -DHLOLLI_WG_ANALYZER_BUILD_BENCHMARKS=ON
cmake --build build-bench --parallel --target hwa_benchmark
ctest --test-dir build-bench --output-on-failure -R '^benchmark\.'
```

See `bench/README.md` for same-machine baseline comparisons.

## WAVE regression cases

```sh
cmake -S . -B build-regression \
  -DHLOLLI_WG_ANALYZER_BUILD_REGRESSION_TESTS=ON
cmake --build build-regression --parallel --target hwa_wav_regression
ctest --test-dir build-regression --output-on-failure \
  -R '^regression\.'
```

The runner reads only the checked-in corpus. It does not create mutations.

## Fuzzers and sanitizers

The fuzz build needs a compiler with libFuzzer support. Clang is the usual
local choice.

```sh
CC=clang cmake -S . -B build-fuzz \
  -DCMAKE_BUILD_TYPE=Release \
  -DHLOLLI_WG_ANALYZER_STRICT=ON \
  -DHLOLLI_WG_ANALYZER_BUILD_FUZZERS=ON \
  -DBUILD_TESTING=ON
cmake --build build-fuzz --parallel
ctest --test-dir build-fuzz --output-on-failure -R '^fuzz\.'
```

CI runs the fuzz seed corpus on Linux, macOS, and Windows. It also runs the
malformed WAVE set with address and undefined-behavior sanitizers.

## WASI

The `portable/` CMake project builds the path-free analysis core. It needs a
WASI SDK, Wasmtime, and `wasm-tools`. The CI workflow shows the required CMake
variables and runs both `wasm.portability` and `wasm.imports`.
