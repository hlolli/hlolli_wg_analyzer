#!/usr/bin/env python3
"""Checked PCM/measurement helpers for cello pizzicato-onset diagnostics.

There is deliberately no model-selection or production-writing operation here.
"""
from __future__ import annotations

import csv
import hashlib
import json
import math
from pathlib import Path
import struct
from typing import Iterable, Sequence
import wave

PCM24_SCALE = 1 << 23
PCM24_MIN = -(1 << 23)
PCM24_MAX = (1 << 23) - 1
ANALYZER_FLOOR_DB = -300.0
MAX_PITCH_PEAK_BYTES = 8 * 1024 * 1024
MAX_PITCH_EVALUATIONS = 10_000_000
MIN_PITCH_CONFIDENCE = 0.65
MIN_PITCH_COVERAGE = 0.35


class DiagnosticError(ValueError):
    """A checked diagnostic contract failed."""


def finite_number(value: object, name: str) -> float:
    try:
        result = float(value)
    except (TypeError, ValueError) as error:
        raise DiagnosticError(f"{name} is not numeric") from error
    if not math.isfinite(result):
        raise DiagnosticError(f"{name} is not finite")
    return result


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _decode_pcm24(value: bytes) -> int:
    integer = value[0] | value[1] << 8 | value[2] << 16
    return integer - (1 << 24) if integer & (1 << 23) else integer


def read_pcm24(path: Path, rate: int, channels: int,
               frames: int | None = None) -> list[tuple[int, ...]]:
    try:
        with wave.open(str(path), "rb") as source:
            facts = (source.getframerate(), source.getnchannels(),
                     source.getsampwidth())
            if (facts != (rate, channels, 3) or
                    source.getcomptype() != "NONE"):
                raise DiagnosticError(
                    f"unexpected PCM24 WAVE format: {path}: {facts}")
            count = source.getnframes()
            if frames is not None and count != frames:
                raise DiagnosticError(
                    f"unexpected PCM24 frame count: {path}: {count}")
            payload = source.readframes(count)
            if (len(payload) != count * channels * 3 or
                    source.readframes(1)):
                raise DiagnosticError(f"truncated PCM24 WAVE: {path}")
    except (OSError, EOFError, wave.Error) as error:
        raise DiagnosticError(
            f"cannot read PCM24 WAVE {path}: {error}") from error
    stride = channels * 3
    return [
        tuple(_decode_pcm24(payload[offset + 3 * channel:
                                    offset + 3 * channel + 3])
              for channel in range(channels))
        for offset in range(0, len(payload), stride)
    ]


def read_pcm24_stereo(path: Path, rate: int,
                      frames: int | None = None) -> list[tuple[int, int]]:
    return [(row[0], row[1]) for row in read_pcm24(path, rate, 2, frames)]


def extract_six_channel_dry(
    rows: Sequence[tuple[int, ...]],
) -> tuple[list[tuple[int, int]], dict[str, int | bool]]:
    if not rows or any(len(row) != 6 for row in rows):
        raise DiagnosticError("dry transport must contain six-channel frames")
    auxiliary_nonzero = sum(value != 0 for row in rows for value in row[2:])
    if auxiliary_nonzero:
        raise DiagnosticError(
            "body-off transport has nonzero auxiliary channels")
    return (
        [(row[0], row[1]) for row in rows],
        {
            "frames": len(rows),
            "auxiliary_samples": 4 * len(rows),
            "auxiliary_nonzero_samples": 0,
            "body_path_absent": True,
        },
    )


def read_pcm_to_stereo24(path: Path, rate: int) -> list[tuple[int, int]]:
    try:
        with wave.open(str(path), "rb") as source:
            channels = source.getnchannels()
            width = source.getsampwidth()
            if (source.getframerate() != rate or channels not in (1, 2) or
                    width not in (2, 3) or source.getcomptype() != "NONE"):
                raise DiagnosticError(
                    f"unsupported listening-reference WAVE: {path}")
            count = source.getnframes()
            payload = source.readframes(count)
            if (len(payload) != count * channels * width or
                    source.readframes(1)):
                raise DiagnosticError(
                    f"truncated listening-reference WAVE: {path}")
    except (OSError, EOFError, wave.Error) as error:
        raise DiagnosticError(
            f"cannot read listening-reference WAVE {path}: {error}") from error
    result = []
    stride = channels * width
    for offset in range(0, len(payload), stride):
        values = []
        for channel in range(channels):
            sample = payload[offset + channel * width:
                             offset + (channel + 1) * width]
            value = (int.from_bytes(sample, "little", signed=True) << 8
                     if width == 2 else _decode_pcm24(sample))
            values.append(value)
        result.append((values[0], values[-1]))
    return result


def pcm24_facts(samples: Sequence[tuple[int, int]]) -> dict[str, object]:
    if not samples:
        raise DiagnosticError("PCM24 stream is empty")
    left_nonzero = any(left != 0 for left, _ in samples)
    right_nonzero = any(right != 0 for _, right in samples)
    rail_count = sum(
        left in (PCM24_MIN, PCM24_MAX) or
        right in (PCM24_MIN, PCM24_MAX)
        for left, right in samples
    )
    peak = max(max(abs(left), abs(right)) for left, right in samples)
    return {
        "frames": len(samples),
        "left_nonzero": left_nonzero,
        "right_nonzero": right_nonzero,
        "rail_frame_count": rail_count,
        "peak_integer": peak,
        "peak_dbfs": (20.0 * math.log10(peak / PCM24_SCALE)
                       if peak else None),
    }


def mono(samples: Sequence[tuple[float, float]]) -> list[float]:
    return [(left + right) / (2.0 * PCM24_SCALE)
            for left, right in samples]


def _frame_bounds(onset: int, rate: int,
                  milliseconds: Sequence[float]) -> tuple[int, int]:
    if (len(milliseconds) != 2 or milliseconds[0] < 0 or
            milliseconds[1] <= milliseconds[0]):
        raise DiagnosticError("invalid onset-relative window")
    return (
        onset + round(milliseconds[0] * rate / 1000.0),
        onset + round(milliseconds[1] * rate / 1000.0),
    )


def onset_ratios(
    samples: Sequence[tuple[float, float]], onset: int, rate: int,
    transient_ms: Sequence[float] = (0.0, 10.0),
    tail_ms: Sequence[float] = (20.0, 100.0),
) -> dict[str, float]:
    values = mono(samples)
    first, last = _frame_bounds(onset, rate, transient_ms)
    tail_first, tail_last = _frame_bounds(onset, rate, tail_ms)
    if first < 0 or tail_last > len(values):
        raise DiagnosticError("onset windows exceed PCM stream")
    transient = values[first:last]
    tail = values[tail_first:tail_last]
    transient_msq = sum(value * value for value in transient) / len(transient)
    tail_msq = sum(value * value for value in tail) / len(tail)
    if transient_msq <= 0.0 or tail_msq <= 0.0:
        raise DiagnosticError("silent transient or tail measurement window")
    edge = max(abs(values[index] - values[index - 1])
               for index in range(max(first, 1), last))
    if edge <= 0.0:
        raise DiagnosticError("transient first difference is exact zero")
    tail_rms_value = math.sqrt(tail_msq)
    return {
        "transient_mean_square": transient_msq,
        "tail_mean_square": tail_msq,
        "transient_to_tail_db": 10.0 * math.log10(
            transient_msq / tail_msq),
        "maximum_first_difference": edge,
        "tail_rms": tail_rms_value,
        "step_to_tail_rms": edge / tail_rms_value,
        "step_to_tail_rms_db": 20.0 * math.log10(edge / tail_rms_value),
    }


def ratio_invariance(
    samples: Sequence[tuple[int, int]], onset: int, rate: int, gain: float,
) -> dict[str, object]:
    if not math.isfinite(gain) or gain <= 0.0:
        raise DiagnosticError("normalization gain is not positive and finite")
    before = onset_ratios(samples, onset, rate)
    scaled = [(left * gain, right * gain) for left, right in samples]
    after = onset_ratios(scaled, onset, rate)
    fields = ("transient_to_tail_db", "step_to_tail_rms_db")
    differences = {field: abs(before[field] - after[field])
                   for field in fields}
    return {
        "before": before,
        "after_pre_quantization": after,
        "absolute_differences": differences,
        "maximum_absolute_difference": max(differences.values()),
    }


def _solve_three(matrix: list[list[float]], vector: list[float]) -> list[float]:
    augmented = [row[:] + [value] for row, value in zip(matrix, vector)]
    for column in range(3):
        pivot = max(range(column, 3),
                    key=lambda row: abs(augmented[row][column]))
        if abs(augmented[pivot][column]) < 1.0e-24:
            raise DiagnosticError(
                "singular sinusoidal least-squares system")
        augmented[column], augmented[pivot] = (
            augmented[pivot], augmented[column])
        divisor = augmented[column][column]
        augmented[column] = [item / divisor
                             for item in augmented[column]]
        for row in range(3):
            if row != column:
                factor = augmented[row][column]
                augmented[row] = [
                    item - factor * base
                    for item, base in zip(augmented[row],
                                          augmented[column])
                ]
    return [augmented[row][3] for row in range(3)]


def harmonic_levels(
    samples: Sequence[tuple[int, int]], onset: int, rate: int,
    fundamental_hz: float, harmonics: Iterable[int] = range(1, 12),
    window_ms: Sequence[float] = (20.0, 70.0),
) -> list[dict[str, float | int | None]]:
    values = mono(samples)
    first, last = _frame_bounds(onset, rate, window_ms)
    if first < 0 or last > len(values) or last - first < 3:
        raise DiagnosticError("harmonic window exceeds PCM stream")
    count = last - first
    result = []
    for harmonic in harmonics:
        frequency = harmonic * fundamental_hz
        if harmonic < 1 or frequency >= rate / 2.0:
            raise DiagnosticError("harmonic frequency is out of range")
        gram = [[0.0] * 3 for _ in range(3)]
        rhs = [0.0] * 3
        for local, sample in enumerate(values[first:last]):
            weight = 0.5 - 0.5 * math.cos(
                2.0 * math.pi * local / (count - 1))
            angle = 2.0 * math.pi * frequency * (first + local) / rate
            basis = (math.cos(angle), math.sin(angle), 1.0)
            for row in range(3):
                rhs[row] += weight * basis[row] * sample
                for column in range(3):
                    gram[row][column] += (
                        weight * basis[row] * basis[column])
        cosine, sine, _ = _solve_three(gram, rhs)
        amplitude = math.hypot(cosine, sine)
        result.append({
            "harmonic": harmonic,
            "frequency_hz": frequency,
            "amplitude": amplitude,
            "level_dbfs": (20.0 * math.log10(amplitude)
                            if amplitude > 0.0 else None),
        })
    return result


def h10_notch_db(rows: Sequence[dict[str, float | int | None]]) -> float:
    levels = {int(row["harmonic"]): row["level_dbfs"] for row in rows}
    if any(levels.get(number) is None for number in (9, 10, 11)):
        raise DiagnosticError("H9-H11 levels are needed for the H10 notch")
    return float(levels[10]) - 0.5 * (
        float(levels[9]) + float(levels[11]))


def _high_band_fraction(row: dict[str, str]) -> dict[str, object]:
    levels = [finite_number(row.get(f"band_{index}_db"),
                            f"band_{index}_db")
              for index in range(10)]
    if any(level < ANALYZER_FLOOR_DB for level in levels):
        raise DiagnosticError("analyzer band is below its -300 dB floor")
    if all(level == ANALYZER_FLOOR_DB for level in levels[7:10]):
        return {
            "status": "censored-at-analyzer-floor",
            "db_fraction": None,
            "high_band_censored": True,
            "analyzer_floor_db": ANALYZER_FLOOR_DB,
        }
    powers = [10.0 ** (level / 10.0) for level in levels]
    total = sum(powers)
    high = sum(powers[7:10])
    if total <= 0.0 or high <= 0.0:
        raise DiagnosticError("analyzer bands have invalid power")
    return {
        "status": ("floor-limited-estimate"
                   if any(level == ANALYZER_FLOOR_DB for level in levels)
                   else "reported-band-power-ratio"),
        "db_fraction": 10.0 * math.log10(high / total),
        "high_band_censored": False,
        "analyzer_floor_db": ANALYZER_FLOOR_DB,
    }


def high_band_contrast(first: dict[str, object],
                       second: dict[str, object]) -> dict[str, object]:
    censored = bool(first.get("high_band_censored") or
                    second.get("high_band_censored"))
    if censored:
        return {"value": None, "conclusive": False,
                "high_band_censored": True}
    first_value = finite_number(first.get("db_fraction"),
                                "first high-band fraction")
    second_value = finite_number(second.get("db_fraction"),
                                 "second high-band fraction")
    return {"value": first_value - second_value, "conclusive": True,
            "high_band_censored": False}


def selected_analyzer_frame(path: Path,
                            onset_seconds: float = 0.1) -> dict[str, object]:
    try:
        with path.open(newline="", encoding="utf-8") as stream:
            rows = list(csv.DictReader(stream))
    except (OSError, UnicodeError, csv.Error) as error:
        raise DiagnosticError(f"cannot read analyzer CSV: {error}") from error
    if not rows:
        raise DiagnosticError("analyzer CSV is empty")
    required_columns = {
        "time_seconds", "onset_strength", "spectral_centroid_hz",
        "spectral_rolloff_85_hz", "spectral_flatness",
        *(f"band_{index}_db" for index in range(10)),
    }
    if not required_columns.issubset(rows[0]):
        raise DiagnosticError("analyzer CSV lacks required columns")
    candidates = []
    for row in rows:
        if None in row:
            raise DiagnosticError("analyzer CSV has a malformed row")
        time = finite_number(row.get("time_seconds"), "time_seconds")
        onset_strength = finite_number(
            row.get("onset_strength"), "onset_strength")
        if onset_seconds - 0.010 <= time <= onset_seconds + 0.020:
            candidates.append((onset_strength, -time, row))
    if not candidates:
        raise DiagnosticError("analyzer CSV has no row in onset window")
    row = max(candidates, key=lambda item: (item[0], item[1]))[2]
    for name in ("time_seconds", "onset_strength", "spectral_centroid_hz",
                 "spectral_rolloff_85_hz", "spectral_flatness"):
        if row.get(name, "") == "":
            raise DiagnosticError(
                "selected analyzer row lacks required fields")
    return {
        "time_seconds": finite_number(row["time_seconds"], "time_seconds"),
        "onset_strength": finite_number(row["onset_strength"],
                                         "onset_strength"),
        "centroid_hz": finite_number(row["spectral_centroid_hz"],
                                     "spectral_centroid_hz"),
        "rolloff_85_hz": finite_number(row["spectral_rolloff_85_hz"],
                                       "spectral_rolloff_85_hz"),
        "flatness": finite_number(row["spectral_flatness"],
                                  "spectral_flatness"),
        "four_khz_plus": _high_band_fraction(row),
    }


def tail_rms(samples: Sequence[tuple[int, int]], onset: int, rate: int,
             window_ms: Sequence[float] = (20.0, 300.0)) -> float:
    first, last = _frame_bounds(onset, rate, window_ms)
    if first < 0 or last > len(samples):
        raise DiagnosticError("tail RMS window exceeds PCM stream")
    energy = sum(left * left + right * right
                 for left, right in samples[first:last])
    if energy <= 0:
        raise DiagnosticError("tail RMS window is silent")
    return math.sqrt(energy / (2.0 * (last - first))) / PCM24_SCALE


def encode_pcm24(samples: Sequence[tuple[int, int]]) -> bytes:
    output = bytearray()
    for pair in samples:
        for value in pair:
            if value < PCM24_MIN or value > PCM24_MAX:
                raise DiagnosticError("PCM24 value is out of range")
            encoded = value & 0xFFFFFF
            output.extend((encoded & 0xFF, (encoded >> 8) & 0xFF,
                           (encoded >> 16) & 0xFF))
    return bytes(output)


def write_pcm24_new(path: Path, samples: Sequence[tuple[int, int]],
                    rate: int) -> None:
    with path.open("xb") as raw:
        with wave.open(raw, "wb") as output:
            output.setnchannels(2)
            output.setsampwidth(3)
            output.setframerate(rate)
            output.writeframes(encode_pcm24(samples))


def encode_pcm16(samples: Sequence[tuple[int, int]], gain: float) -> bytes:
    if not math.isfinite(gain) or gain <= 0.0:
        raise DiagnosticError("invalid listening gain")
    output = bytearray()
    for pair in samples:
        for value in pair:
            scaled = max(-32768, min(32767, round(value * gain / 256.0)))
            output.extend(struct.pack("<h", scaled))
    return bytes(output)


def decode_pcm16_stereo(payload: bytes) -> list[tuple[int, int]]:
    if len(payload) % 4:
        raise DiagnosticError("unaligned stereo PCM16 payload")
    return [
        (int.from_bytes(payload[offset:offset + 2], "little", signed=True) << 8,
         int.from_bytes(payload[offset + 2:offset + 4], "little",
                        signed=True) << 8)
        for offset in range(0, len(payload), 4)
    ]


def assemble_abba(
    forward_first: bytes, forward_second: bytes,
    reverse_first: bytes, reverse_second: bytes, silence: bytes,
) -> bytes:
    if not all(len(value) % 4 == 0 for value in (
            forward_first, forward_second, reverse_first, reverse_second,
            silence)):
        raise DiagnosticError("ABBA payload is not aligned stereo PCM16")
    return (forward_first + forward_second + silence +
            reverse_first + reverse_second)


def listening_layout_facts(payload: bytes, segment_frames: int,
                           silence_frames: int) -> dict[str, object]:
    if segment_frames <= 0 or silence_frames < 0 or len(payload) % 4:
        raise DiagnosticError("invalid listening layout dimensions")
    expected_frames = 4 * segment_frames + silence_frames
    actual_frames = len(payload) // 4
    if actual_frames != expected_frames:
        raise DiagnosticError("listening block has an unexpected frame count")
    return {
        "order": ["forward_item_1", "forward_item_2", "silence",
                  "reverse_item_1", "reverse_item_2"],
        "segment_frames": segment_frames,
        "silence_frames": silence_frames,
        "frame_count": actual_frames,
        "gap_count": 1,
    }


def expand_exact_command(
    template: Sequence[str], expected_template: Sequence[str],
    context: dict[str, object],
) -> list[str]:
    if list(template) != list(expected_template):
        raise DiagnosticError("command template does not match the frozen argv")
    result = []
    for token in template:
        try:
            result.append(token.format_map(context))
        except (KeyError, ValueError) as error:
            raise DiagnosticError(f"cannot expand command token: {token}") from error
    if any("{" in token or "}" in token for token in result):
        raise DiagnosticError("command argv contains an unresolved placeholder")
    return result


def _unique_json_object(rows: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in rows:
        if key in result:
            raise DiagnosticError(f"duplicate JSON key in model: {key}")
        result[key] = value
    return result


def _json_paths(left: object, right: object, prefix: str = "") -> set[str]:
    if type(left) is not type(right):
        return {prefix}
    if isinstance(left, dict):
        result: set[str] = set()
        for key in set(left) | set(right):
            child = f"{prefix}.{key}" if prefix else key
            if key not in left or key not in right:
                result.add(child)
            else:
                result |= _json_paths(left[key], right[key], child)
        return result
    if isinstance(left, list):
        if len(left) != len(right):
            return {prefix}
        result = set()
        for index, (first, second) in enumerate(zip(left, right)):
            result |= _json_paths(first, second, f"{prefix}[{index}]")
        return result
    return set() if left == right else {prefix}


def patch_pizzicato_right_model(
    source: bytes, release_min: str, release_range: str, noise_gain: str,
) -> tuple[bytes, list[dict[str, object]]]:
    try:
        text = source.decode("utf-8")
        before = json.loads(
            text, object_pairs_hook=_unique_json_object,
            parse_constant=lambda value: (_ for _ in ()).throw(
                DiagnosticError(f"non-finite JSON constant: {value}")))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise DiagnosticError("model is not valid UTF-8 JSON") from error
    marker = '    "pizzicato_right": {'
    start = text.find(marker)
    if start < 0 or text.find(marker, start + 1) >= 0:
        raise DiagnosticError("model needs one pizzicato_right object")
    end = text.find("\n    }", start)
    if end < 0:
        raise DiagnosticError("unterminated pizzicato_right object")
    block = text[start:end]
    replacements = (
        ('      "noise_gain": 0.0025,',
         f'      "noise_gain": {noise_gain},', "noise_gain"),
        ('      "release_min_seconds": 0.00018,',
         f'      "release_min_seconds": {release_min},',
         "release_min_seconds"),
        ('      "release_range_seconds": 0.00072',
         f'      "release_range_seconds": {release_range}',
         "release_range_seconds"),
    )
    facts = []
    for old, new, field in replacements:
        if block.count(old) != 1:
            raise DiagnosticError(
                f"pizzicato_right token is not unique: {field}")
        block = block.replace(old, new, 1)
        facts.append({"field": field, "old_token": old, "new_token": new})
    patched = text[:start] + block + text[end:]
    try:
        after = json.loads(
            patched, object_pairs_hook=_unique_json_object,
            parse_constant=lambda value: (_ for _ in ()).throw(
                DiagnosticError(f"non-finite JSON constant: {value}")))
    except json.JSONDecodeError as error:
        raise DiagnosticError("patched model is invalid JSON") from error
    allowed = {
        "exciters.pizzicato_right.noise_gain",
        "exciters.pizzicato_right.release_min_seconds",
        "exciters.pizzicato_right.release_range_seconds",
    }
    differences = _json_paths(before, after)
    if not differences.issubset(allowed):
        raise DiagnosticError("patched model violates the semantic allowlist")
    for fact in facts:
        fact["changed"] = fact["old_token"] != fact["new_token"]
    return patched.encode("utf-8"), facts


def validate_pitch_report(
    report: object, expected_path: Path, expected_hz: float,
    sample_rate: int, frames: int,
) -> float:
    if type(report) is not dict:
        raise DiagnosticError("pitch report root is not an object")
    required = {
        "schema", "schema_version", "command", "method", "path",
        "expected_hz", "requested_mask", "valid_mask", "rejection_mask",
        "requested_metrics", "valid_metrics", "rejections", "format",
        "pitch", "decay", "work",
    }
    if set(report) != required:
        raise DiagnosticError("pitch report has an unexpected schema")
    if (report["schema"] != "hwa-isolated-note" or
            type(report["schema_version"]) is not int or
            report["schema_version"] != 1 or
            report["command"] != "isolated-note" or
            report["method"] != "isolated-note-1" or
            type(report["path"]) is not str or
            report["path"] != str(expected_path) or
            type(report["expected_hz"]) not in (int, float) or
            finite_number(report["expected_hz"],
                          "pitch expected_hz") != expected_hz or
            any(type(report[name]) is not int
                for name in ("requested_mask", "valid_mask",
                             "rejection_mask")) or
            report["requested_mask"] != 1 or report["valid_mask"] != 1 or
            report["rejection_mask"] != 0 or
            report["requested_metrics"] != ["pitch"] or
            report["valid_metrics"] != ["pitch"] or
            report["rejections"] != []):
        raise DiagnosticError("pitch report contract changed")
    format_row = report["format"]
    expected_format = {
        "container": "riff", "encoding": "pcm",
        "sample_rate_hz": sample_rate, "channels": 2,
        "bits_per_sample": 24, "valid_bits_per_sample": 24,
        "frames": frames,
    }
    if (type(format_row) is not dict or format_row != expected_format or
            any(type(format_row[name]) is not int
                for name in ("sample_rate_hz", "channels", "bits_per_sample",
                             "valid_bits_per_sample", "frames"))):
        raise DiagnosticError("pitch report format changed")
    pitch = report["pitch"]
    if type(pitch) is not dict or set(pitch) != {
            "valid", "hz", "cents", "confidence", "coverage",
            "window_count", "accepted_window_count", "start_sample",
            "end_sample"} or pitch.get("valid") is not True:
        raise DiagnosticError("pitch report is not valid")
    if type(pitch.get("hz")) not in (int, float):
        raise DiagnosticError("pitch hz has the wrong type")
    hz = finite_number(pitch.get("hz"), "pitch hz")
    if hz <= 0.0:
        raise DiagnosticError("pitch hz is not positive")
    if any(type(pitch[name]) not in (int, float)
           for name in ("cents", "confidence", "coverage")):
        raise DiagnosticError("pitch numeric field has the wrong type")
    cents = finite_number(pitch["cents"], "pitch cents")
    expected_cents = 1200.0 * math.log2(hz / expected_hz)
    if abs(cents - expected_cents) > 1.0e-9:
        raise DiagnosticError("pitch cents and hz disagree")
    if abs(cents) > 80.0:
        raise DiagnosticError("valid pitch lies outside the method boundary")
    confidence = finite_number(pitch["confidence"], "pitch confidence")
    coverage = finite_number(pitch["coverage"], "pitch coverage")
    if not 0.0 <= confidence <= 1.0 or not 0.0 <= coverage <= 1.0:
        raise DiagnosticError("pitch confidence or coverage is out of range")
    if confidence < MIN_PITCH_CONFIDENCE:
        raise DiagnosticError("valid pitch confidence is below the method floor")
    if coverage < MIN_PITCH_COVERAGE:
        raise DiagnosticError("valid pitch coverage is below the method floor")
    for name in ("window_count", "accepted_window_count", "start_sample",
                 "end_sample"):
        if type(pitch[name]) is not int or pitch[name] < 0:
            raise DiagnosticError(f"pitch {name} is invalid")
    window_count = pitch["window_count"]
    accepted_count = pitch["accepted_window_count"]
    if (window_count <= 0 or accepted_count < 2 or
            accepted_count > window_count or pitch["start_sample"] >=
            pitch["end_sample"] or pitch["end_sample"] > frames):
        raise DiagnosticError("pitch support bounds are invalid")
    method_window = max(2048, math.ceil(6.0 * sample_rate / expected_hz))
    if method_window > frames:
        raise DiagnosticError("valid pitch has less than one method window")
    method_hop = method_window // 2
    method_window_count = 1 + (frames - method_window) // method_hop
    start_sample = pitch["start_sample"]
    end_sample = pitch["end_sample"]
    if (window_count > method_window_count or
            start_sample % method_hop != 0 or
            end_sample < method_window or
            (end_sample - method_window) % method_hop != 0):
        raise DiagnosticError("pitch support is inconsistent with the method grid")
    first_window = start_sample // method_hop
    last_window = (end_sample - method_window) // method_hop
    support_window_count = last_window - first_window + 1
    if (first_window > last_window or last_window >= method_window_count or
            accepted_count > support_window_count):
        raise DiagnosticError(
            "pitch support span is inconsistent with accepted windows")
    expected_coverage = accepted_count / window_count
    if coverage != expected_coverage:
        raise DiagnosticError("pitch coverage and window counts disagree")
    decay = report["decay"]
    if type(decay) is not dict or set(decay) != {
            "valid", "slope_db_per_second", "t60_seconds",
            "support_seconds", "dynamic_range_db", "residual_db",
            "floor_dbfs", "point_count", "start_sample", "end_sample"}:
        raise DiagnosticError("pitch report decay schema changed")
    if decay["valid"] is not False:
        raise DiagnosticError("pitch-only report cannot contain a valid decay")
    for name in ("slope_db_per_second", "t60_seconds", "support_seconds",
                 "dynamic_range_db", "residual_db", "floor_dbfs"):
        if type(decay[name]) not in (int, float):
            raise DiagnosticError(f"decay {name} has the wrong type")
        finite_number(decay[name], f"decay {name}")
    for name in ("point_count", "start_sample", "end_sample"):
        if type(decay[name]) is not int or decay[name] < 0:
            raise DiagnosticError(f"decay {name} is invalid")
    decay_numbers = tuple(
        finite_number(decay[name], f"decay {name}")
        for name in ("slope_db_per_second", "t60_seconds",
                     "support_seconds", "dynamic_range_db", "residual_db",
                     "floor_dbfs")
    )
    if (any(value != 0.0 for value in decay_numbers) or
            any(decay[name] != 0
                for name in ("point_count", "start_sample", "end_sample"))):
        raise DiagnosticError("pitch-only decay fields must all be zero")
    work = report["work"]
    if (type(work) is not dict or set(work) != {"peak_bytes", "evaluations"}
            or type(work.get("peak_bytes")) is not int
            or type(work.get("evaluations")) is not int
            or not 0 < work["peak_bytes"] <= MAX_PITCH_PEAK_BYTES
            or not 0 < work["evaluations"] <= MAX_PITCH_EVALUATIONS):
        raise DiagnosticError("pitch report work schema changed")
    return hz


def write_pcm16_new(path: Path, payload: bytes, rate: int) -> None:
    with path.open("xb") as raw:
        with wave.open(raw, "wb") as output:
            output.setnchannels(2)
            output.setsampwidth(2)
            output.setframerate(rate)
            output.writeframes(payload)


def write_json_new(path: Path, value: object) -> None:
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
