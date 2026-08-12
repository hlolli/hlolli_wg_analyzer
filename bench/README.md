# Benchmarks

The benchmark runs fixed decode, FFT, summary feature, pitch track, stereo
delay, alignment, and JSON report cases. Each case warms once, then gathers
separate timed samples. The JSON file records raw times, work counts, and build
details.

The default source-based summary is 10 seconds long. `--long` also runs a
10-minute source-based summary. The generated source does not hold its samples
in memory. The benchmark finds the smallest accepted `max_work_bytes` value
for both runs, records it as `tracked_work_bytes`, and fails if duration makes
that processor budget grow. This measures the analyzer's tracked heap budget,
not process RSS.

Build and take a baseline:

```sh
cmake -S . -B build-bench \
  -DCMAKE_BUILD_TYPE=Release \
  -DHLOLLI_WG_ANALYZER_BUILD_BENCHMARKS=ON
cmake --build build-bench --target hwa_benchmark
build-bench/bench/hwa_benchmark --samples 7 --long --output baseline.json
```

Run the same command after a change and compare it on the same idle machine:

```sh
build-bench/bench/hwa_benchmark --samples 7 --long --output candidate.json
python3 bench/compare.py baseline.json candidate.json
```

The default gate fails when one case is over 20% and 5 ms slower, after a
five-MAD noise allowance. It also fails when the geometric mean is over 10%
slower. A ratio over 10% prints a warning after the same noise allowance.
These are machine-relative checks. The tool rejects a different OS, CPU kind,
compiler, pointer size, or build
mode unless `--allow-machine-mismatch` is set.

Use at least seven samples for a release gate. `--samples 1 --minimum-ms 1`
exists only for a quick build check.
