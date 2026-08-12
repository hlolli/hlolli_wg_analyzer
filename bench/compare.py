#!/usr/bin/env python3
"""Compare benchmark results against a baseline from the same machine."""

from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
import tempfile
from pathlib import Path
from typing import Any


SCHEMA = "hwa-benchmark"
RELEASE_CASES = frozenset(
    {
        "decode.pcm16_mono",
        "fft.complex_4096",
        "e2e.summary_mono_10s",
        "features.pitch_track",
        "features.stereo_delay_2048",
        "alignment.dtw_1200x1260",
        "json.analysis_report",
        "e2e.summary_mono_10min",
    }
)


class InputError(ValueError):
    """The input cannot support a sound comparison."""


def load_result(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise InputError(f"could not read {path}: {error}") from error
    if document.get("schema") != SCHEMA or document.get("schema_version") != 1:
        raise InputError(f"{path} is not a supported benchmark result")
    cases = document.get("cases")
    if not isinstance(cases, list) or not cases:
        raise InputError(f"{path} has no benchmark cases")
    return document


def machine_key(document: dict[str, Any]) -> tuple[Any, ...]:
    machine = document.get("machine", {})
    return (
        machine.get("hostname"),
        machine.get("os"),
        machine.get("arch"),
        machine.get("pointer_bits"),
        machine.get("compiler_family"),
        machine.get("compiler_version"),
        machine.get("build_mode"),
    )


def case_map(
    document: dict[str, Any], minimum_samples: int
) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for case in document["cases"]:
        name = case.get("name") if isinstance(case, dict) else None
        values = case.get("seconds_per_iteration") if isinstance(case, dict) else None
        if not isinstance(name, str) or name in result:
            raise InputError("case names must be unique strings")
        unit = case.get("unit")
        work = case.get("work_per_iteration")
        tracked = case.get("tracked_work_bytes")
        checksum = case.get("checksum")
        if not isinstance(unit, str) or not unit:
            raise InputError(f"case {name} has no work unit")
        if not isinstance(work, int) or isinstance(work, bool) or work <= 0:
            raise InputError(f"case {name} has an invalid work amount")
        if (not isinstance(tracked, int) or isinstance(tracked, bool)
                or tracked < 0):
            raise InputError(f"case {name} has an invalid tracked heap")
        if (not isinstance(checksum, str) or len(checksum) != 16
                or any(character not in "0123456789abcdef" for character in checksum)):
            raise InputError(f"case {name} has an invalid checksum")
        if not isinstance(values, list) or len(values) < minimum_samples:
            raise InputError(
                f"case {name} has fewer than {minimum_samples} samples"
            )
        if any(not isinstance(value, (int, float)) or not math.isfinite(value)
               or value <= 0.0 for value in values):
            raise InputError(f"case {name} has an invalid sample")
        result[name] = case
    return result


def median_and_mad(values: list[float]) -> tuple[float, float]:
    median = statistics.median(values)
    mad = statistics.median(abs(value - median) for value in values)
    return median, mad


def compare(
    baseline: dict[str, Any],
    candidate: dict[str, Any],
    *,
    warn_ratio: float,
    fail_ratio: float,
    suite_fail_ratio: float,
    min_delta_ms: float,
    noise_mad: float,
    allow_machine_mismatch: bool,
    minimum_samples: int,
    require_full_suite: bool,
) -> dict[str, Any]:
    if machine_key(baseline) != machine_key(candidate) and not allow_machine_mismatch:
        raise InputError(
            "machine or compiler metadata differs; use --allow-machine-mismatch "
            "only for an exploratory comparison"
        )

    base_cases = case_map(baseline, minimum_samples)
    new_cases = case_map(candidate, minimum_samples)
    if set(base_cases) != set(new_cases):
        missing = sorted(set(base_cases) - set(new_cases))
        added = sorted(set(new_cases) - set(base_cases))
        raise InputError(f"case sets differ (missing={missing}, added={added})")
    if require_full_suite and (
        baseline.get("long_cases") is not True
        or candidate.get("long_cases") is not True
        or set(base_cases) != RELEASE_CASES
    ):
        raise InputError(
            "release comparison requires --long and the fixed eight-case suite"
        )

    rows: list[dict[str, Any]] = []
    ratios: list[float] = []
    failed = False
    warned = False
    for name in sorted(base_cases):
        base = base_cases[name]
        new = new_cases[name]
        if base.get("unit") != new.get("unit") or (
            base.get("work_per_iteration") != new.get("work_per_iteration")
        ) or (
            base.get("tracked_work_bytes") != new.get("tracked_work_bytes")
        ) or (
            base.get("checksum") != new.get("checksum")
        ):
            raise InputError(
                f"case {name} changed its work, tracked heap, or checksum"
            )

        base_values = [float(value) for value in base["seconds_per_iteration"]]
        new_values = [float(value) for value in new["seconds_per_iteration"]]
        base_median, base_mad = median_and_mad(base_values)
        new_median, new_mad = median_and_mad(new_values)
        ratio = new_median / base_median
        delta_seconds = new_median - base_median
        noise_floor = max(min_delta_ms / 1000.0, noise_mad * base_mad)
        status = "pass"
        if ratio > fail_ratio and delta_seconds > noise_floor:
            status = "fail"
            failed = True
        elif ratio > warn_ratio and delta_seconds > noise_floor:
            status = "warn"
            warned = True
        ratios.append(ratio)
        rows.append(
            {
                "name": name,
                "baseline_median_seconds": base_median,
                "baseline_mad_seconds": base_mad,
                "candidate_median_seconds": new_median,
                "candidate_mad_seconds": new_mad,
                "ratio": ratio,
                "delta_seconds": delta_seconds,
                "noise_floor_seconds": noise_floor,
                "status": status,
            }
        )

    suite_ratio = math.exp(sum(math.log(ratio) for ratio in ratios) / len(ratios))
    if suite_ratio > suite_fail_ratio:
        failed = True
    return {
        "schema": "hwa-benchmark-comparison",
        "schema_version": 1,
        "status": "fail" if failed else ("warn" if warned else "pass"),
        "suite_geomean_ratio": suite_ratio,
        "suite_fail_ratio": suite_fail_ratio,
        "cases": rows,
    }


def print_report(report: dict[str, Any]) -> None:
    print("case                              base ms      new ms    ratio  status")
    for row in report["cases"]:
        print(
            f"{row['name']:<32}"
            f" {row['baseline_median_seconds'] * 1000.0:10.3f}"
            f" {row['candidate_median_seconds'] * 1000.0:10.3f}"
            f" {row['ratio']:8.3f}  {row['status']}"
        )
    print(
        f"suite geometric mean: {report['suite_geomean_ratio']:.3f} "
        f"({report['status']})"
    )


def self_test() -> int:
    machine = {
        "os": "test",
        "arch": "test",
        "pointer_bits": 64,
        "compiler_family": "test",
        "compiler_version": "1",
        "build_mode": "release",
    }

    def result(samples: list[float]) -> dict[str, Any]:
        return {
            "schema": SCHEMA,
            "schema_version": 1,
            "machine": machine,
            "cases": [
                {
                    "name": "test.case",
                    "unit": "items",
                    "work_per_iteration": 10,
                    "tracked_work_bytes": 20,
                    "checksum": "000000000000001e",
                    "seconds_per_iteration": samples,
                }
            ],
        }

    settings = dict(
        warn_ratio=1.10,
        fail_ratio=1.20,
        suite_fail_ratio=1.20,
        min_delta_ms=0.0,
        noise_mad=5.0,
        allow_machine_mismatch=False,
        minimum_samples=3,
        require_full_suite=False,
    )
    assert compare(result([1.0, 1.0, 1.0]), result([1.05, 1.05, 1.05]),
                   **settings)["status"] == "pass"
    assert compare(result([1.0, 1.0, 1.0]), result([1.15, 1.15, 1.15]),
                   **settings)["status"] == "warn"
    assert compare(result([1.0, 1.0, 1.0]), result([1.25, 1.25, 1.25]),
                   **settings)["status"] == "fail"
    noisy_settings = {**settings, "suite_fail_ratio": 2.0}
    assert compare(result([0.9, 1.0, 1.1]), result([1.25, 1.25, 1.25]),
                   **noisy_settings)["status"] == "pass"

    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "result.json"
        path.write_text(json.dumps(result([1.0])), encoding="utf-8")
        assert load_result(path)["schema"] == SCHEMA
    changed = result([1.0, 1.0, 1.0])
    changed["cases"][0]["checksum"] = "000000000000001f"
    try:
        compare(result([1.0, 1.0, 1.0]), changed, **settings)
    except InputError:
        pass
    else:
        raise AssertionError("changed checksum was accepted")
    print("PASS: comparator self-test")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("baseline", nargs="?", type=Path)
    parser.add_argument("candidate", nargs="?", type=Path)
    parser.add_argument("--warn-ratio", type=float, default=1.10)
    parser.add_argument("--fail-ratio", type=float, default=1.20)
    parser.add_argument("--suite-fail-ratio", type=float, default=1.10)
    parser.add_argument("--min-delta-ms", type=float, default=5.0)
    parser.add_argument("--noise-mad", type=float, default=5.0)
    parser.add_argument("--allow-machine-mismatch", action="store_true")
    parser.add_argument("--minimum-samples", type=int, default=7)
    parser.add_argument("--allow-partial", action="store_true")
    parser.add_argument("--json-output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if args.baseline is None or args.candidate is None:
        parser.error("baseline and candidate are required")
    if not (1.0 <= args.warn_ratio <= args.fail_ratio):
        parser.error("ratios must satisfy 1 <= warn <= fail")
    if args.suite_fail_ratio < 1.0 or args.min_delta_ms < 0.0 or args.noise_mad < 0.0:
        parser.error("suite ratio and noise limits must be nonnegative")
    if args.minimum_samples < 1:
        parser.error("minimum samples must be positive")

    try:
        report = compare(
            load_result(args.baseline),
            load_result(args.candidate),
            warn_ratio=args.warn_ratio,
            fail_ratio=args.fail_ratio,
            suite_fail_ratio=args.suite_fail_ratio,
            min_delta_ms=args.min_delta_ms,
            noise_mad=args.noise_mad,
            allow_machine_mismatch=args.allow_machine_mismatch,
            minimum_samples=args.minimum_samples,
            require_full_suite=not args.allow_partial,
        )
    except InputError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    print_report(report)
    if args.json_output is not None:
        args.json_output.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    return 1 if report["status"] == "fail" else 0


if __name__ == "__main__":
    raise SystemExit(main())
