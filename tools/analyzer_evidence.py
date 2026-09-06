#!/usr/bin/env python3
"""Run the analyzer and check reports used as fit evidence."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Any, Mapping, NoReturn, Optional


MAX_REPORT_BYTES = 1024 * 1024
MAX_ERROR_BYTES = 2000
RUN_TIMEOUT_SECONDS = 120
MIN_PITCH_CONFIDENCE = 0.65
MIN_PITCH_COVERAGE = 0.35
MAX_PITCH_ERROR_CENTS = 80.0
MIN_EXPECTED_HZ = 20.0
MAX_EXPECTED_HZ = 5000.0
NOTE_REJECTIONS = (
    (1 << 0, "silence"),
    (1 << 1, "noise"),
    (1 << 2, "octave"),
    (1 << 3, "boundary"),
    (1 << 4, "low-support"),
    (1 << 5, "low-dynamic-range"),
    (1 << 6, "high-residual"),
    (1 << 7, "late-pulse"),
)
PITCH_REJECTION_MASK = (1 << 5) - 1
PITCH_FATAL_REJECTION_MASK = (1 << 0) | (1 << 4)
HARMONIC_REJECTIONS = (
    (1 << 0, "no-onset"),
    (1 << 1, "late-pulse"),
    (1 << 2, "low-anchor-snr"),
    (1 << 3, "band-out-of-range"),
    (1 << 4, "low-support"),
    (1 << 5, "low-dynamic-range"),
    (1 << 6, "non-decay"),
    (1 << 7, "high-residual"),
    (1 << 8, "truncated-fit"),
    (1 << 9, "low-harmonic-coverage"),
)
HARMONIC_PROFILE_REJECTION_MASK = (
    (1 << 0) | (1 << 1) | (1 << 4) | (1 << 9))
HARMONIC_BAND_REJECTION_MASK = sum(1 << bit for bit in range(2, 9))
MAX_HARMONIC_BANDS = 24


class EvidenceError(ValueError):
    """A checked input, analyzer run, or report contract failed."""


@dataclass(frozen=True)
class PitchEvidence:
    valid: bool
    hz: float
    cents: float
    confidence: float
    coverage: float


@dataclass(frozen=True)
class IsolatedNoteEvidence:
    report: dict[str, Any]
    pitch: PitchEvidence


@dataclass(frozen=True)
class HarmonicDecayEvidence:
    report: dict[str, Any]
    reference_valid: bool
    reference_valid_band_count: int
    model_valid: Optional[bool]
    model_valid_band_count: Optional[int]
    comparison_valid: Optional[bool]
    shared_valid_band_count: Optional[int]
    shared_reference_coverage: Optional[float]
    band_t60_log_errors_db: tuple[float, ...]
    t60_log_rmse_db: Optional[float]
    median_t60_log_bias_db: Optional[float]


def _reject_constant(value: str) -> NoReturn:
    raise EvidenceError("analyzer returned a non-finite JSON number: " + value)


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise EvidenceError("analyzer returned duplicate JSON key: " + key)
        result[key] = value
    return result


def _json_float(source: str) -> float:
    value = float(source)
    if not math.isfinite(value):
        raise EvidenceError("analyzer returned a non-finite JSON number")
    return value


def _finite(value: Any, field: str) -> float:
    if type(value) not in (int, float):
        raise EvidenceError(field + " must be finite")
    try:
        result = float(value)
    except (OverflowError, ValueError) as error:
        raise EvidenceError(field + " must be finite") from error
    if not math.isfinite(result):
        raise EvidenceError(field + " must be finite")
    return result


def _count(value: Any, field: str) -> int:
    if type(value) is not int or value < 0:
        raise EvidenceError(field + " must be a nonnegative integer")
    return value


def _checked_rejections(
        value: dict[str, Any], names: tuple[tuple[int, str], ...],
        allowed_mask: int, field: str) -> int:
    mask = _count(value.get("rejection_mask"), field + " rejection_mask")
    wanted = [name for flag, name in names if mask & flag]
    if mask & ~allowed_mask or value.get("rejections") != wanted:
        raise EvidenceError(field + " returned an unknown contract")
    return mask


def _harmonic_profile(
        value: Any, path: Path, expected_hz: float,
        field: str) -> tuple[
            bool, int, tuple[bool, ...], tuple[Optional[float], ...]]:
    if (type(value) is not dict or value.get("path") != str(path) or
            type(value.get("valid")) is not bool or
            type(value.get("bands")) is not list):
        raise EvidenceError(field + " returned an unknown contract")
    bands = value["bands"]
    band_count = _count(value.get("band_count"), field + " band_count")
    valid_count = _count(
        value.get("valid_band_count"), field + " valid_band_count")
    found_valid_count = 0
    profile_rejection_mask = _checked_rejections(
        value, HARMONIC_REJECTIONS,
        HARMONIC_PROFILE_REJECTION_MASK, field)
    validity = []
    t60_values = []
    for index, band in enumerate(bands):
        if type(band) is not dict or type(band.get("valid")) is not bool:
            raise EvidenceError(
                "{} bands[{}] returned an unknown contract".format(
                    field, index))
        if (type(band.get("harmonic_number")) is not int or
                band["harmonic_number"] != index + 1):
            raise EvidenceError(
                "{} bands[{}] returned an unknown contract".format(
                    field, index))
        target_hz = _finite(
            band.get("target_hz"),
            "{} bands[{}].target_hz".format(field, index))
        if not math.isclose(
                target_hz, expected_hz * (index + 1),
                rel_tol=1e-12, abs_tol=1e-9):
            raise EvidenceError(
                "{} bands[{}] returned an unknown contract".format(
                    field, index))
        t60_value = band.get("t60_seconds")
        if t60_value is None:
            t60 = None
        else:
            t60 = _finite(
                t60_value,
                "{} bands[{}].t60_seconds".format(field, index))
        if band["valid"]:
            if (t60 is None or t60 <= 0.0 or
                    _checked_rejections(
                        band, HARMONIC_REJECTIONS,
                        HARMONIC_BAND_REJECTION_MASK,
                        "{} bands[{}]".format(field, index)) != 0):
                raise EvidenceError(
                    "{} bands[{}] returned an unknown contract".format(
                        field, index))
            found_valid_count += 1
        elif _checked_rejections(
                band, HARMONIC_REJECTIONS,
                HARMONIC_BAND_REJECTION_MASK,
                "{} bands[{}]".format(field, index)) == 0:
            raise EvidenceError(
                "{} bands[{}] returned an unknown contract".format(
                    field, index))
        validity.append(band["valid"])
        t60_values.append(t60)
    if (band_count != len(bands) or band_count > MAX_HARMONIC_BANDS or
            valid_count != found_valid_count or
            value["valid"] != (valid_count >= 4) or
            (value["valid"] and profile_rejection_mask != 0) or
            (not value["valid"] and profile_rejection_mask == 0)):
        raise EvidenceError(field + " returned an unknown contract")
    return (
        value["valid"], valid_count, tuple(validity), tuple(t60_values))


def _digest(value: Any, field: str) -> str:
    if (type(value) is not str or len(value) != 64 or
            any(character not in "0123456789abcdef" for character in value)):
        raise EvidenceError(field + " must be a lower-case SHA-256")
    return value


def _regular(path: Path, field: str) -> Path:
    path = path.absolute()
    if not path.is_file() or path.is_symlink():
        raise EvidenceError(field + " must be a regular file: " + str(path))
    return path


def _sha256(path: Path, field: str) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as error:
        raise EvidenceError("cannot hash {}: {}".format(field, error)) from error
    return digest.hexdigest()


def _tool_command(path: Path) -> list[str]:
    if os.name == "nt" and path.suffix.lower() == ".py":
        return [sys.executable, "-I", str(path)]
    return [str(path)]


def _clean_environment() -> dict[str, str]:
    environment = {"LC_ALL": "C", "LANG": "C", "TZ": "UTC"}
    if os.name == "nt":
        for name in ("SystemRoot", "WINDIR"):
            value = os.environ.get(name)
            if value is not None:
                environment[name] = value
    return environment


def _checked_environment(
        environment: Optional[Mapping[str, str]]) -> dict[str, str]:
    if environment is None:
        return _clean_environment()
    try:
        items = environment.items()
    except AttributeError as error:
        raise EvidenceError("environment must be a string mapping") from error
    result = {}
    for name, value in items:
        if (type(name) is not str or not name or "\0" in name or
                "=" in name or type(value) is not str or "\0" in value):
            raise EvidenceError("environment must be a string mapping")
        result[name] = value
    return result


class AnalyzerEvidence:
    """Check one analyzer and a fixed set of source recordings."""

    def __init__(
            self, analyzer: Path,
            sources: Mapping[str, tuple[Path, str]], *,
            analyzer_sha256: Optional[str] = None,
            cwd: Optional[Path] = None,
            environment: Optional[Mapping[str, str]] = None,
            scratch: Optional[Path] = None) -> None:
        self._analyzer = _regular(Path(analyzer), "analyzer")
        if os.name != "nt" and not os.access(self._analyzer, os.X_OK):
            raise EvidenceError("analyzer is not executable: " + str(self._analyzer))
        measured_analyzer_hash = _sha256(self._analyzer, "analyzer")
        if analyzer_sha256 is None:
            self._analyzer_sha256 = measured_analyzer_hash
        else:
            self._analyzer_sha256 = _digest(
                analyzer_sha256, "analyzer SHA-256")
            if measured_analyzer_hash != self._analyzer_sha256:
                raise EvidenceError("analyzer hash changed")
        self._sources: dict[str, tuple[Path, str]] = {}
        for source_id, source in sources.items():
            if type(source_id) is not str or not source_id:
                raise EvidenceError("source id must be a nonempty string")
            if source_id in self._sources:
                raise EvidenceError("duplicate source id: " + source_id)
            if type(source) is not tuple or len(source) != 2:
                raise EvidenceError(source_id + " source must be (path, SHA-256)")
            path = _regular(Path(source[0]), source_id)
            expected_hash = _digest(source[1], source_id + " SHA-256")
            if _sha256(path, source_id) != expected_hash:
                raise EvidenceError(source_id + " hash changed")
            self._sources[source_id] = (path, expected_hash)
        if cwd is None:
            self._cwd = None
        else:
            self._cwd = Path(cwd).absolute()
            if not self._cwd.is_dir() or self._cwd.is_symlink():
                raise EvidenceError("work directory must be a directory")
        self._environment = _checked_environment(environment)
        if scratch is None:
            self._scratch = None
        else:
            self._scratch = Path(scratch).absolute()
            if not self._scratch.is_dir() or self._scratch.is_symlink():
                raise EvidenceError("scratch directory must be a directory")

    @property
    def analyzer_sha256(self) -> str:
        return self._analyzer_sha256

    def _verify_inputs(self, when: str) -> None:
        if _sha256(self._analyzer, "analyzer") != self._analyzer_sha256:
            raise EvidenceError("analyzer changed " + when)
        for source_id, (path, expected_hash) in self._sources.items():
            if _sha256(path, source_id) != expected_hash:
                raise EvidenceError(source_id + " changed " + when)

    def _source(self, source_id: str) -> Path:
        try:
            return self._sources[source_id][0]
        except KeyError as error:
            raise EvidenceError("unknown source id: " + str(source_id)) from error

    def _run(self, arguments: list[str], label: str) -> dict[str, Any]:
        self._verify_inputs("before " + label)
        try:
            with tempfile.TemporaryFile(
                    dir=self._scratch) as stdout, tempfile.TemporaryFile(
                        dir=self._scratch) as stderr:
                try:
                    completed = subprocess.run(
                        [*_tool_command(self._analyzer), "--json", *arguments],
                        check=False, stdout=stdout, stderr=stderr,
                        env=self._environment,
                        cwd=self._cwd, timeout=RUN_TIMEOUT_SECONDS,
                    )
                except subprocess.TimeoutExpired as error:
                    raise EvidenceError(label + " timed out") from error
                except OSError as error:
                    raise EvidenceError(
                        "cannot run {}: {}".format(label, error)) from error
                stdout_size = os.fstat(stdout.fileno()).st_size
                stderr_size = os.fstat(stderr.fileno()).st_size
                if completed.returncode != 0:
                    stderr.seek(max(0, stderr_size - MAX_ERROR_BYTES))
                    detail = stderr.read(MAX_ERROR_BYTES).decode(
                        "utf-8", errors="replace").strip()
                    if not detail:
                        stdout.seek(max(0, stdout_size - MAX_ERROR_BYTES))
                        detail = stdout.read(MAX_ERROR_BYTES).decode(
                            "utf-8", errors="replace").strip()
                    raise EvidenceError(
                        label + " failed" + (": " + detail if detail else ""))
                if stdout_size > MAX_REPORT_BYTES:
                    raise EvidenceError(label + " output exceeds the byte limit")
                stdout.seek(0)
                source = stdout.read(MAX_REPORT_BYTES + 1)
                if len(source) > MAX_REPORT_BYTES:
                    raise EvidenceError(
                        label + " output exceeds the byte limit")
        except EvidenceError:
            raise
        except OSError as error:
            raise EvidenceError(
                "cannot buffer {} output: {}".format(label, error)) from error
        finally:
            self._verify_inputs("during " + label)
        try:
            value = json.loads(
                source.decode("utf-8"), object_pairs_hook=_unique_object,
                parse_constant=_reject_constant, parse_float=_json_float,
            )
        except EvidenceError:
            raise
        except (UnicodeError, json.JSONDecodeError, ValueError,
                RecursionError) as error:
            raise EvidenceError(label + " returned invalid JSON") from error
        if type(value) is not dict:
            raise EvidenceError(label + " returned a non-object")
        return value

    def isolated_note(
            self, source_id: str, expected_hz: float) -> IsolatedNoteEvidence:
        """Return checked isolated-note pitch evidence."""
        expected = _finite(expected_hz, "expected_hz")
        if expected < MIN_EXPECTED_HZ or expected > MAX_EXPECTED_HZ:
            raise EvidenceError("expected_hz must be between 20 and 5000")
        path = self._source(source_id)
        report = self._run([
            "isolated-note", str(path), "--expected-hz",
            format(expected, ".17g"), "--metrics", "pitch",
        ], source_id + " checked note")
        pitch = report.get("pitch")
        if (report.get("schema") != "hwa-isolated-note" or
                type(report.get("schema_version")) is not int or
                report.get("schema_version") != 1 or
                report.get("command") != "isolated-note" or
                report.get("method") != "isolated-note-1" or
                report.get("path") != str(path) or
                _finite(report.get("expected_hz"),
                        source_id + " reported expected_hz") != expected or
                type(report.get("requested_mask")) is not int or
                report.get("requested_mask") != 1 or
                report.get("requested_metrics") != ["pitch"] or
                type(pitch) is not dict or
                type(pitch.get("valid")) is not bool):
            raise EvidenceError(
                source_id + " checked note returned an unknown contract")
        wanted_valid_mask = 1 if pitch["valid"] else 0
        wanted_valid_metrics = ["pitch"] if pitch["valid"] else []
        rejection_mask = _checked_rejections(
            report, NOTE_REJECTIONS, PITCH_REJECTION_MASK,
            source_id + " checked note")
        if (type(report.get("valid_mask")) is not int or
                report.get("valid_mask") != wanted_valid_mask or
                report.get("valid_metrics") != wanted_valid_metrics or
                rejection_mask & ~PITCH_REJECTION_MASK):
            raise EvidenceError(
                source_id + " checked note returned an unknown contract")
        pitch_hz = _finite(pitch.get("hz"), source_id + " pitch hz")
        pitch_cents = _finite(
            pitch.get("cents"), source_id + " pitch cents")
        confidence = _finite(
            pitch.get("confidence"), source_id + " pitch confidence")
        coverage = _finite(
            pitch.get("coverage"), source_id + " pitch coverage")
        if (confidence < 0.0 or confidence > 1.0 or
                coverage < 0.0 or coverage > 1.0):
            raise EvidenceError(
                source_id + " checked note returned an unknown contract")
        wanted_valid = bool(
            pitch_hz > 0.0 and confidence >= MIN_PITCH_CONFIDENCE and
            coverage >= MIN_PITCH_COVERAGE and
            abs(pitch_cents) <= MAX_PITCH_ERROR_CENTS)
        if (pitch_hz < 0.0 or
                (pitch_hz == 0.0 and
                 (pitch_cents != 0.0 or confidence != 0.0)) or
                (pitch_hz > 0.0 and
                 (confidence < MIN_PITCH_CONFIDENCE or
                  abs(pitch_cents) > MAX_PITCH_ERROR_CENTS)) or
                pitch["valid"] != wanted_valid):
            raise EvidenceError(
                source_id + " checked note returned an unknown contract")
        if ((not pitch["valid"] and
             not rejection_mask & PITCH_FATAL_REJECTION_MASK) or
                (pitch["valid"] and
                 rejection_mask & PITCH_FATAL_REJECTION_MASK)):
            raise EvidenceError(
                source_id + " checked note returned an unknown contract")
        if pitch_hz > 0.0:
            wanted_cents = 1200.0 * (
                math.log2(pitch_hz) - math.log2(expected))
            if not math.isclose(
                    pitch_cents, wanted_cents,
                    rel_tol=1e-12, abs_tol=1e-9):
                raise EvidenceError(
                    source_id + " checked note returned an unknown contract")
        return IsolatedNoteEvidence(
            report=report,
            pitch=PitchEvidence(
                valid=pitch["valid"],
                hz=pitch_hz,
                cents=pitch_cents,
                confidence=confidence,
                coverage=coverage,
            ),
        )

    def harmonic_decay(
            self, reference_id: str, expected_hz: float,
            model_id: Optional[str] = None) -> HarmonicDecayEvidence:
        """Return checked harmonic-decay evidence."""
        expected = _finite(expected_hz, "expected_hz")
        if expected < MIN_EXPECTED_HZ or expected > MAX_EXPECTED_HZ:
            raise EvidenceError("expected_hz must be between 20 and 5000")
        reference = self._source(reference_id)
        arguments = ["harmonic-decay", str(reference)]
        model = None
        if model_id is not None:
            model = self._source(model_id)
            arguments.append(str(model))
        arguments.extend(["--expected-hz", format(expected, ".17g")])
        report = self._run(arguments, reference_id + " harmonic decay")
        reference_profile = report.get("reference")
        if (report.get("schema") != "hwa-harmonic-decay" or
                type(report.get("schema_version")) is not int or
                report.get("schema_version") != 1 or
                report.get("command") != "harmonic-decay" or
                report.get("method") != "harmonic-decay-1" or
                _finite(report.get("expected_hz"),
                        reference_id + " harmonic expected_hz") != expected or
                type(reference_profile) is not dict):
            raise EvidenceError(
                reference_id + " harmonic decay returned an unknown contract")
        (reference_valid, reference_count, reference_band_validity,
         reference_t60) = _harmonic_profile(
            reference_profile, reference, expected,
            reference_id + " harmonic reference")
        if model is None:
            if (report.get("model") is not None or
                    report.get("comparison") is not None):
                raise EvidenceError(
                    reference_id +
                    " harmonic decay returned an unknown contract")
            return HarmonicDecayEvidence(
                report=report,
                reference_valid=reference_valid,
                reference_valid_band_count=reference_count,
                model_valid=None,
                model_valid_band_count=None,
                comparison_valid=None,
                shared_valid_band_count=None,
                shared_reference_coverage=None,
                band_t60_log_errors_db=(),
                t60_log_rmse_db=None,
                median_t60_log_bias_db=None,
            )
        model_profile = report.get("model")
        comparison = report.get("comparison")
        (model_valid, model_count, model_band_validity,
         model_t60) = _harmonic_profile(
            model_profile, model, expected,
            str(model_id) + " harmonic model")
        if (type(comparison) is not dict or
                type(comparison.get("valid")) is not bool or
                type(comparison.get("bands")) is not list):
            raise EvidenceError(
                reference_id + " harmonic comparison returned an unknown contract")
        bands = comparison["bands"]
        band_count = _count(
            comparison.get("band_count"), "harmonic comparison band_count")
        shared_count = _count(
            comparison.get("shared_valid_band_count"),
            "harmonic comparison shared_valid_band_count")
        errors = []
        for index, band in enumerate(bands):
            if type(band) is not dict or type(band.get("valid")) is not bool:
                raise EvidenceError(
                    "harmonic comparison bands[{}] returned an unknown contract".
                    format(index))
            reference_band_valid = (
                index < len(reference_band_validity) and
                reference_band_validity[index])
            model_band_valid = (
                index < len(model_band_validity) and
                model_band_validity[index])
            if (type(band.get("harmonic_number")) is not int or
                    band["harmonic_number"] != index + 1 or
                    type(band.get("reference_valid")) is not bool or
                    band["reference_valid"] != reference_band_valid or
                    type(band.get("model_valid")) is not bool or
                    band["model_valid"] != model_band_valid or
                    band["valid"] !=
                    (reference_band_valid and model_band_valid)):
                raise EvidenceError(
                    "harmonic comparison bands[{}] returned an unknown contract".
                    format(index))
            error_value = band.get("t60_log_error_db")
            if error_value is None:
                error = None
            else:
                error = _finite(
                    error_value,
                    "harmonic comparison bands[{}].t60_log_error_db".format(
                        index))
            if band["valid"]:
                if error is None:
                    raise EvidenceError(
                        "harmonic comparison bands[{}] returned an unknown contract".
                        format(index))
                reference_value = reference_t60[index]
                model_value = model_t60[index]
                if reference_value is None or model_value is None:
                    raise EvidenceError(
                        "harmonic comparison bands[{}] returned an unknown contract".
                        format(index))
                expected_error = 20.0 * (
                    math.log10(model_value) - math.log10(reference_value))
                if not math.isclose(
                        error, expected_error,
                        rel_tol=1e-12, abs_tol=1e-9):
                    raise EvidenceError(
                        "harmonic comparison bands[{}] returned an unknown contract".
                        format(index))
                errors.append(error)
        coverage = _finite(
            comparison.get("shared_reference_coverage"),
            "harmonic comparison shared_reference_coverage")
        reported_rmse = _finite(
            comparison.get("t60_log_rmse_db"),
            "harmonic comparison t60_log_rmse_db")
        reported_bias = _finite(
            comparison.get("median_t60_log_bias_db"),
            "harmonic comparison median_t60_log_bias_db")
        expected_coverage = (
            0.0 if reference_count == 0 else shared_count / reference_count)
        if errors:
            expected_rmse = math.sqrt(
                sum(value * value for value in errors) / len(errors))
            ordered_errors = sorted(errors)
            middle = len(ordered_errors) // 2
            if len(ordered_errors) % 2:
                expected_bias = ordered_errors[middle]
            else:
                expected_bias = 0.5 * (
                    ordered_errors[middle - 1] + ordered_errors[middle])
        else:
            expected_rmse = 0.0
            expected_bias = 0.0
        if (band_count != len(bands) or
                band_count != max(
                    len(reference_band_validity), len(model_band_validity)) or
                shared_count != len(errors) or
                coverage < 0.0 or coverage > 1.0 or
                not math.isclose(
                    coverage, expected_coverage,
                    rel_tol=1e-12, abs_tol=1e-12) or
                not math.isclose(
                    reported_rmse, expected_rmse,
                    rel_tol=1e-12, abs_tol=1e-12) or
                not math.isclose(
                    reported_bias, expected_bias,
                    rel_tol=1e-12, abs_tol=1e-12) or
                comparison["valid"] !=
                (shared_count >= 4 and coverage >= 0.5)):
            raise EvidenceError(
                reference_id + " harmonic comparison returned an unknown contract")
        return HarmonicDecayEvidence(
            report=report,
            reference_valid=reference_valid,
            reference_valid_band_count=reference_count,
            model_valid=model_valid,
            model_valid_band_count=model_count,
            comparison_valid=comparison["valid"],
            shared_valid_band_count=shared_count,
            shared_reference_coverage=coverage,
            band_t60_log_errors_db=tuple(errors),
            t60_log_rmse_db=reported_rmse,
            median_t60_log_bias_db=reported_bias,
        )
