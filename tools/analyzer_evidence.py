#!/usr/bin/env python3
"""Run the analyzer and check reports used as fit evidence."""

from __future__ import annotations

from dataclasses import dataclass
from collections import OrderedDict
import copy
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


@dataclass(frozen=True)
class BodyEnvelopeProfile:
    status: str
    confidence: float
    frames_seen: int
    frames_used: int
    valid_point_count: int


@dataclass(frozen=True)
class BodyEnvelopeEvidence:
    report: dict[str, Any]
    reference: BodyEnvelopeProfile
    model: Optional[BodyEnvelopeProfile]
    comparison_valid: Optional[bool]
    shape_rmse_db: Optional[float]
    shape_correlation: Optional[float]
    confidence: Optional[float]


NOTE_PHASE_UNITS = {
    "rms_dbfs": "dBFS", "peak_dbfs": "dBFS",
    "level_slope_db_per_second": "dB/s", "centroid_hz": "Hz",
    "centroid_slope_hz_per_second": "Hz/s", "duration_seconds": "seconds",
}

NOTE_PHASE_OPTIONS = {
    "boundary_frame_size": ("--frame-size", 2048, 256, 16384),
    "boundary_hop_size": ("--hop-size", 512, 1, 16384),
    "measurement_fft_size": ("--measure-fft-size", 4096, 256, 16384),
    "measurement_hop_size": ("--measure-hop-size", 256, 1, 16384),
    "boundary_search_seconds": ("--boundary-search", 0.15, 0, 10),
    "tail_limit_seconds": ("--tail-limit", 1.5, 0, 10),
    "min_phase_seconds": ("--min-phase", 0.02, 0, 1),
    "min_body_seconds": ("--min-body", 0.05, 0, 2),
    "silence_threshold_dbfs": ("--silence-threshold", -60.0, -200, 0),
}


def note_phase_options(options: Optional[dict[str, Any]] = None) -> dict[str, Any]:
    """Resolve phase settings without changing the caller's manifest."""
    if options is None:
        options = {}
    if type(options) is not dict or set(options) - set(NOTE_PHASE_OPTIONS):
        raise EvidenceError("note-phase options must contain only known settings")
    result = {}
    for name, (_, default, minimum, maximum) in NOTE_PHASE_OPTIONS.items():
        raw = options.get(name, default)
        value = _count(raw, name) if type(default) is int else _finite(raw, name)
        if not minimum <= value <= maximum:
            raise EvidenceError("note-phase option is out of range: " + name)
        result[name] = value
    for window, hop in (("boundary_frame_size", "boundary_hop_size"),
                        ("measurement_fft_size", "measurement_hop_size")):
        size = result[window]
        if size & (size - 1) or result[hop] > size:
            raise EvidenceError("note-phase window must be a power of two and contain its hop")
    return result


@dataclass(frozen=True)
class NotePhaseEvidence:
    report: dict[str, Any]
    phases: dict[str, dict[str, Any]]


class NotePhaseCache:
    """Reuse checked reports within one run; never persist them."""

    def __init__(self, max_entries: int = 64) -> None:
        if type(max_entries) is not int or not 1 <= max_entries <= 64:
            raise EvidenceError("note-phase cache capacity must be from 1 through 64")
        self._max_entries = max_entries
        self._reports: OrderedDict[tuple[Any, ...], NotePhaseEvidence] = OrderedDict()

    def note_phases(
            self, checks: AnalyzerEvidence, source_id: str,
            start_sample: int, end_sample: int, *,
            options: Optional[dict[str, Any]] = None) -> NotePhaseEvidence:
        start = _count(start_sample, "note start sample")
        end = _count(end_sample, "note end sample")
        if start >= end or end > 2**64 - 1:
            raise EvidenceError("note span must have increasing sample bounds")
        settings = note_phase_options(options)
        source = checks._source(source_id)
        key = (checks._analyzer, checks.analyzer_sha256,
               source, checks._sources[source_id][1], start, end,
               tuple(settings.items()), checks._cwd or Path.cwd(),
               tuple(sorted(checks._environment.items())))
        cached = self._reports.get(key)
        if cached is not None:
            checks._verify_inputs("before cached note phases")
            result = copy.deepcopy(cached)
            checks._verify_inputs("during cached note phases")
            self._reports.move_to_end(key)
            return result
        result = checks.note_phases(source_id, start, end, options=settings)
        self._reports[key] = copy.deepcopy(result)
        if len(self._reports) > self._max_entries:
            self._reports.popitem(last=False)
        return result


def _body_number(value: Any, expected: float, field: str) -> float:
    actual = _finite(value, field)
    if not math.isclose(actual, expected, rel_tol=1e-10, abs_tol=1e-9):
        raise EvidenceError(field + " disagrees with its supporting points")
    return actual


def _body_profile(value: Any, path: Path) -> BodyEnvelopeProfile:
    field = "body-envelope profile"
    if (type(value) is not dict or value.get("path") != str(path) or
            type(value.get("points")) is not list or
            len(value["points"]) != 160):
        raise EvidenceError(field + " returned an unknown contract")
    seen = _count(value.get("frames_seen"), field + " frames_seen")
    used = _count(value.get("frames_used"), field + " frames_used")
    rejected = _count(
        value.get("frames_rejected_pitch"), field + " frames_rejected_pitch")
    observations = _count(
        value.get("observation_count"), field + " observation_count")
    pitch_min = _finite(value.get("pitch_min_hz"), field + " pitch_min_hz")
    pitch_max = _finite(value.get("pitch_max_hz"), field + " pitch_max_hz")
    if (seen > 500000 or observations > 2000000 or
            used + rejected > seen or not used <= observations <= used * 32 or
            (used == 0 and (pitch_min != 0.0 or pitch_max != 0.0)) or
            (used > 0 and not 0.0 < pitch_min <= pitch_max)):
        raise EvidenceError(field + " has inconsistent support")
    valid_confidences = []
    total_observations = 0
    for index, point in enumerate(value["points"]):
        if type(point) is not dict or type(point.get("valid")) is not bool:
            raise EvidenceError(field + " has an invalid point")
        _body_number(point.get("frequency_hz"),
                     120.0 * 2.0 ** (index / 24.0), field + " frequency_hz")
        _finite(point.get("relative_db"), field + " relative_db")
        spread = _finite(
            point.get("residual_spread_db"), field + " residual_spread_db")
        count = _count(point.get("observation_count"), field + " point count")
        cells = _count(point.get("pitch_cell_count"), field + " pitch cells")
        harmonics = _count(point.get("harmonic_count"), field + " harmonics")
        if (spread < 0.0 or count > observations or
                cells > min(count, used, 256) or
                harmonics > min(count, 32) or
                (count > 0 and (cells == 0 or harmonics == 0))):
            raise EvidenceError(field + " has inconsistent point support")
        flags = ((1 if count < 6 else 0) | (2 if cells < 2 else 0) |
                 (4 if harmonics < 2 else 0) | (8 if spread > 12.0 else 0))
        if (_count(point.get("quality_flags"), field + " quality_flags") !=
                flags or point["valid"] !=
                (count >= 6 and cells >= 2 and harmonics >= 2)):
            raise EvidenceError(field + " has inconsistent point validity")
        expected_confidence = (
            min(count / 20.0, 1.0) * min(cells / 4.0, 1.0) *
            min(harmonics / 4.0, 1.0) * math.exp(-spread / 12.0))
        confidence = _body_number(
            point.get("confidence"), expected_confidence,
            field + " point confidence")
        if not 0.0 <= confidence <= 1.0:
            raise EvidenceError(field + " confidence is outside 0..1")
        if point["valid"]:
            valid_confidences.append(confidence)
        total_observations += count
    valid_count = len(valid_confidences)
    status = ("valid" if valid_count >= 8 else
              "low-support" if observations else "no-support")
    if (total_observations != observations or value.get("status") != status):
        raise EvidenceError(field + " has inconsistent status or support")
    confidence = _body_number(
        value.get("confidence"),
        math.fsum(valid_confidences) / valid_count if valid_count else 0.0,
        field + " confidence")
    if not 0.0 <= confidence <= 1.0:
        raise EvidenceError(field + " confidence is outside 0..1")
    return BodyEnvelopeProfile(status, confidence, seen, used, valid_count)


def _body_comparison(
        value: Any, reference: list[dict[str, Any]],
        model: list[dict[str, Any]]) -> tuple[bool, float, float, float]:
    field = "body-envelope comparison"
    if (type(value) is not dict or type(value.get("valid")) is not bool or
            type(value.get("gaps")) is not list or
            len(value["gaps"]) != len(reference)):
        raise EvidenceError(field + " returned an unknown contract")
    common = [index for index, (r, m) in enumerate(zip(reference, model))
              if r["valid"] and m["valid"] and
              min(r["confidence"], m["confidence"]) > 0.0]
    valid = len(common) >= 3
    if value["valid"] != valid:
        raise EvidenceError(field + " has inconsistent validity")
    gaps = {}
    rmse = correlation = confidence = 0.0
    if valid:
        weights = [min(reference[i]["confidence"], model[i]["confidence"])
                   for i in common]
        deltas = [model[i]["relative_db"] - reference[i]["relative_db"]
                  for i in common]
        offset = math.fsum(d * w for d, w in zip(deltas, weights)) / math.fsum(
            weights)
        centered = [d - offset for d in deltas]
        r_mean = math.fsum(reference[i]["relative_db"] for i in common) / len(
            common)
        m_mean = math.fsum(model[i]["relative_db"] for i in common) / len(common)
        r_values = [reference[i]["relative_db"] - r_mean for i in common]
        m_values = [model[i]["relative_db"] - m_mean for i in common]
        r_norm = math.sqrt(math.fsum(v * v for v in r_values))
        m_norm = math.sqrt(math.fsum(v * v for v in m_values))
        if r_norm > 0.0 and m_norm > 0.0:
            correlation = math.fsum(
                (r / r_norm) * (m / m_norm)
                for r, m in zip(r_values, m_values))
        rmse = math.sqrt(math.fsum(d * d for d in centered) / len(common))
        confidence = math.fsum(weights) / len(common)
        gaps = {i: (delta, weight)
                for i, delta, weight in zip(common, centered, weights)}
    for index, gap in enumerate(value["gaps"]):
        if (type(gap) is not dict or type(gap.get("valid")) is not bool or
                gap["valid"] != (index in gaps)):
            raise EvidenceError(field + " has inconsistent gap validity")
        _body_number(gap.get("frequency_hz"), reference[index]["frequency_hz"],
                     field + " gap frequency_hz")
        delta, weight = gaps.get(index, (0.0, 0.0))
        _body_number(gap.get("model_minus_reference_db"), delta,
                     field + " gap value")
        _body_number(gap.get("confidence"), weight, field + " gap confidence")
    reported_rmse = _body_number(
        value.get("shape_rmse_db"), rmse, field + " shape_rmse_db")
    reported_correlation = _body_number(
        value.get("shape_correlation"), correlation, field + " shape_correlation")
    reported_confidence = _body_number(
        value.get("confidence"), confidence, field + " confidence")
    if (reported_rmse < 0.0 or abs(reported_correlation) > 1.0 + 1e-12 or
            not 0.0 <= reported_confidence <= 1.0):
        raise EvidenceError(field + " has an out-of-range score")
    return valid, reported_rmse, reported_correlation, reported_confidence


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

    def note_phases(
            self, source_id: str, start_sample: int,
            end_sample: int, *, options: Optional[dict[str, Any]] = None
            ) -> NotePhaseEvidence:
        """Measure the phases around a caller-supplied note span."""
        start = _count(start_sample, "note start sample")
        end = _count(end_sample, "note end sample")
        if start >= end or end > 2**64 - 1:
            raise EvidenceError("note span must have increasing sample bounds")
        settings = note_phase_options(options)
        path = self._source(source_id)
        arguments = [
            "note-phases", str(path), "--note-start-sample", str(start),
            "--note-end-sample", str(end)]
        for name, value in settings.items():
            arguments.extend([NOTE_PHASE_OPTIONS[name][0],
                              str(value) if type(value) is int else format(value, ".17g")])
        report = self._run(arguments, source_id + " note phases")
        if (report.get("schema") != "hwa-note-phases" or
                type(report.get("schema_version")) is not int or
                report["schema_version"] != 1 or
                report.get("command") != "note-phases" or
                report.get("method") != "note-phases-1" or
                report.get("path") != str(path) or
                report.get("audio_sha256") != self._sources[source_id][1] or
                _count(report.get("note_start_sample"), "start") != start or
                _count(report.get("note_end_sample"), "end") != end):
            raise EvidenceError("note-phases returned an unknown contract")
        rate = _count(report.get("sample_rate_hz"), "sample rate")
        frames = _count(report.get("frames"), "source frames")
        if rate == 0 or frames < end:
            raise EvidenceError("note-phases has an invalid source clock")
        for name, expected in settings.items():
            actual = (_count(report.get(name), name) if type(expected) is int
                      else _finite(report.get(name), name))
            if actual != expected:
                raise EvidenceError("note-phases analysis setting changed: " + name)
        next_onset = report.get("next_onset_sample")
        if next_onset is not None and _count(next_onset, "next onset") >= frames:
            raise EvidenceError("note-phases next onset exceeds the source")
        names = ("attack", "sustain", "release", "clean-tail")
        rows = report.get("phases")
        if type(rows) is not list or len(rows) != len(names):
            raise EvidenceError("note-phases has an invalid phase set")
        previous_end = None
        phases = {}
        for name, row in zip(names, rows):
            if (type(row) is not dict or row.get("phase") != name or
                    row.get("status") not in (
                        "valid", "no-signal", "too-short", "interrupted", "truncated")):
                raise EvidenceError("note-phases has an invalid phase")
            first = _count(row.get("start_sample"), "phase start")
            last = _count(row.get("end_sample"), "phase end")
            confidence = _finite(row.get("boundary_confidence"), "boundary confidence")
            if (first > last or last > frames or not 0.0 <= confidence <= 1.0 or
                    (previous_end is not None and first != previous_end) or
                    (row["status"] == "valid" and first == last) or
                    not math.isclose(_finite(row.get("duration_seconds"), "duration"),
                                     (last - first) / rate, rel_tol=1e-12, abs_tol=1e-12)):
                raise EvidenceError("note-phases has inconsistent phase bounds")
            if row["status"] == "interrupted" and (
                    name not in ("release", "clean-tail") or
                    next_onset is None or last <= next_onset):
                raise EvidenceError("note-phases has inconsistent interruption")
            metrics = row.get("metrics")
            if type(metrics) is not dict or set(metrics) != set(NOTE_PHASE_UNITS):
                raise EvidenceError("note-phases metric set changed")
            for metric_name, unit in NOTE_PHASE_UNITS.items():
                value = metrics[metric_name]
                if (type(value) is not dict or value.get("unit") != unit or
                        value.get("status") not in (
                            "valid", "no-data", "unsupported-item", "empty-span",
                            "too-short", "no-signal", "below-floor", "no-pitch",
                            "multi-pitch", "no-reference")):
                    raise EvidenceError("note-phases metric contract changed")
                metric_confidence = _finite(value.get("confidence"), "metric confidence")
                if not 0.0 <= metric_confidence <= 1.0:
                    raise EvidenceError("note-phases has invalid confidence")
                _count(value.get("quality_flags"), "metric quality")
                if value["status"] == "valid":
                    actual = _finite(value.get("value"), metric_name)
                    if row["status"] != "valid":
                        raise EvidenceError("rejected note phase contains valid measurements")
                    if metric_name == "duration_seconds" and not math.isclose(
                            actual, (last-first)/rate, rel_tol=1e-12, abs_tol=1e-12):
                        raise EvidenceError("phase duration disagrees with its bounds")
                    if metric_name == "centroid_hz" and not 0.0 <= actual <= rate/2:
                        raise EvidenceError("phase centroid exceeds the source clock")
                elif value.get("value") is not None:
                    raise EvidenceError("invalid phase measurement must be null")
            phases[name] = row
            previous_end = last
        return NotePhaseEvidence(report, phases)

    def body_envelope(
            self, reference_id: str,
            model_id: Optional[str] = None) -> BodyEnvelopeEvidence:
        """Check the default pitch-conditioned radiated envelope report."""
        reference_path = self._source(reference_id)
        arguments = ["body-envelope", str(reference_path)]
        model_path = None if model_id is None else self._source(model_id)
        if model_path is not None:
            arguments.append(str(model_path))
        report = self._run(arguments, reference_id + " body-envelope")
        if (type(report.get("schema_version")) is not int or
                report["schema_version"] != 1 or
                report.get("command") != "body-envelope" or
                report.get("method") != "crossed-harmonic-response-1" or
                report.get("shape_constraints") !=
                ["zero-mean", "zero-log-frequency-slope"]):
            raise EvidenceError("body-envelope returned an unknown contract")
        reference = _body_profile(report.get("reference"), reference_path)
        if model_path is None:
            if "model" in report or "comparison" in report:
                raise EvidenceError("body-envelope returned an unexpected model")
            return BodyEnvelopeEvidence(
                report, reference, None, None, None, None, None)
        model = _body_profile(report.get("model"), model_path)
        try:
            valid, rmse, correlation, confidence = _body_comparison(
                report.get("comparison"), report["reference"]["points"],
                report["model"]["points"])
        except EvidenceError:
            raise
        except (OverflowError, ValueError) as error:
            raise EvidenceError(
                "body-envelope comparison cannot be checked") from error
        return BodyEnvelopeEvidence(
            report, reference, model, valid, rmse, correlation, confidence)

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
