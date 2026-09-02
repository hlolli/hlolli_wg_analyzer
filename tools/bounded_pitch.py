#!/usr/bin/env python3
"""Measure a known nearby pitch in an uncompressed PCM WAVE file.

This tool is for checks where the expected note is known before analysis.  It
uses the squared-difference part of YIN inside a cents bound, so an overtone or
weak fundamental cannot move the search to another octave.
"""

from __future__ import annotations

import argparse
from array import array
import hashlib
import json
import math
from pathlib import Path
import statistics
import struct
import sys
from typing import Any, Sequence


class PitchError(ValueError):
    pass


def positive(text: str) -> float:
    try:
        value = float(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be a number") from error
    if not math.isfinite(value) or value <= 0.0:
        raise argparse.ArgumentTypeError("must be a finite positive number")
    return value


def nonnegative(text: str) -> float:
    try:
        value = float(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be a number") from error
    if not math.isfinite(value) or value < 0.0:
        raise argparse.ArgumentTypeError("must be a finite nonnegative number")
    return value


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            while True:
                block = stream.read(1024 * 1024)
                if not block:
                    break
                digest.update(block)
    except OSError as error:
        raise PitchError(f"cannot hash input: {path}: {error}") from error
    return digest.hexdigest()


def decode_sample(data: bytes, offset: int, width: int) -> float:
    if width == 1:
        return (data[offset] - 128) / 128.0
    if width == 2:
        return struct.unpack_from("<h", data, offset)[0] / 32768.0
    if width == 3:
        value = (data[offset] | (data[offset + 1] << 8) |
                 (data[offset + 2] << 16))
        if value & 0x800000:
            value -= 1 << 24
        return value / 8388608.0
    if width == 4:
        return struct.unpack_from("<i", data, offset)[0] / 2147483648.0
    raise PitchError(f"unsupported PCM sample width: {width} bytes")


PCM_GUID = bytes.fromhex("0100000000001000800000aa00389b71")


def read_pcm_wave(path: Path) -> tuple[list[array], int, int, int]:
    try:
        file_data = path.read_bytes()
    except OSError as error:
        raise PitchError(f"cannot read PCM WAVE: {path}: {error}") from error
    if len(file_data) < 12 or file_data[:4] != b"RIFF" or file_data[8:12] != b"WAVE":
        raise PitchError("input must be a little-endian RIFF/WAVE file")
    riff_size = struct.unpack_from("<I", file_data, 4)[0]
    if riff_size < 4 or riff_size + 8 > len(file_data):
        raise PitchError("input has a truncated RIFF container")
    limit = riff_size + 8
    format_data: bytes | None = None
    data: bytes | None = None
    offset = 12
    while offset < limit:
        if limit - offset < 8:
            raise PitchError("input has a truncated WAVE chunk header")
        chunk_id = file_data[offset:offset + 4]
        chunk_size = struct.unpack_from("<I", file_data, offset + 4)[0]
        chunk_start = offset + 8
        chunk_end = chunk_start + chunk_size
        if chunk_end > limit:
            raise PitchError("input has a truncated WAVE chunk")
        if chunk_id == b"fmt ":
            if format_data is not None:
                raise PitchError("input has more than one format chunk")
            format_data = file_data[chunk_start:chunk_end]
        elif chunk_id == b"data":
            if data is not None:
                raise PitchError("input has more than one data chunk")
            data = file_data[chunk_start:chunk_end]
        offset = chunk_end + (chunk_size & 1)
    if offset != limit or format_data is None or data is None:
        raise PitchError("input has an invalid WAVE chunk layout")
    if len(format_data) < 16:
        raise PitchError("input has a short WAVE format chunk")
    (format_tag, channels, sample_rate, byte_rate, frame_width,
     bits_per_sample) = struct.unpack_from("<HHIIHH", format_data)
    if format_tag == 0xFFFE:
        if len(format_data) < 40:
            raise PitchError("input has a short extensible format chunk")
        extension_size, valid_bits = struct.unpack_from("<HH", format_data, 16)
        if extension_size < 22 or format_data[24:40] != PCM_GUID:
            raise PitchError("input WAVE extensible data must use integer PCM")
        if valid_bits != bits_per_sample:
            raise PitchError("input must use all PCM container bits")
    elif format_tag != 1:
        raise PitchError("input must use integer PCM")
    sample_width = bits_per_sample // 8
    if (channels < 1 or sample_rate < 1 or bits_per_sample % 8 != 0 or
            sample_width not in (1, 2, 3, 4)):
        raise PitchError("input has invalid PCM WAVE metadata")
    if (frame_width != channels * sample_width or
            byte_rate != sample_rate * frame_width or len(data) % frame_width):
        raise PitchError("input has inconsistent PCM WAVE metadata")
    frame_count = len(data) // frame_width
    result = [array("d") for _ in range(channels)]
    for frame_offset in range(0, len(data), frame_width):
        for channel in range(channels):
            offset = frame_offset + channel * sample_width
            result[channel].append(decode_sample(data, offset, sample_width))
    return result, sample_rate, sample_width, frame_count


def dot(left: Sequence[float], right: Sequence[float]) -> float:
    if len(left) != len(right):
        raise PitchError("internal dot-product size mismatch")
    sumprod = getattr(math, "sumprod", None)
    if sumprod is not None:
        return float(sumprod(left, right))
    return math.fsum(a * b for a, b in zip(left, right))


def parabolic_minimum(values: Sequence[float], index: int) -> float:
    if index <= 0 or index >= len(values) - 1:
        return float(index)
    left, center, right = values[index - 1:index + 2]
    denominator = left - 2.0 * center + right
    if abs(denominator) < 1.0e-30:
        return float(index)
    return float(index) + 0.5 * (left - right) / denominator


def bounded_yin(frame: Sequence[float], sample_rate: int,
                expected_hz: float, cents_bound: float,
                lag_padding: int = 2) -> tuple[float, int, int]:
    """Return a parabolic YIN difference estimate near ``expected_hz``.

    The frame mean is removed and a symmetric Hann window is applied.  For
    each allowed lag, the score is the mean squared sample difference.  The
    lowest score wins, then three scores refine its lag with a parabola.
    """
    length = len(frame)
    if length < 4:
        raise PitchError("pitch window is too short")
    mean = math.fsum(frame) / length
    scale = 2.0 * math.pi / (length - 1)
    windowed = array("d", (
        (sample - mean) * (0.5 - 0.5 * math.cos(scale * index))
        for index, sample in enumerate(frame)
    ))
    low_hz = expected_hz * 2.0 ** (-cents_bound / 1200.0)
    high_hz = expected_hz * 2.0 ** (cents_bound / 1200.0)
    first_lag = max(2, math.floor(sample_rate / high_hz) - lag_padding)
    last_lag = math.ceil(sample_rate / low_hz) + lag_padding
    if last_lag >= length:
        raise PitchError("pitch window is too short for the requested bound")

    view = memoryview(windowed)
    square_prefix = array("d", [0.0])
    total = 0.0
    for sample in windowed:
        total += sample * sample
        square_prefix.append(total)
    differences: list[float] = []
    for lag in range(first_lag, last_lag + 1):
        pair_count = length - lag
        left_energy = square_prefix[pair_count]
        right_energy = square_prefix[length] - square_prefix[lag]
        cross = dot(view[:pair_count], view[lag:])
        difference = (left_energy + right_energy - 2.0 * cross) / pair_count
        # Roundoff can make an exact zero a tiny negative number.
        differences.append(max(0.0, difference))
    best = min(range(len(differences)), key=differences.__getitem__)
    refined_lag = first_lag + parabolic_minimum(differences, best)
    return sample_rate / refined_lag, first_lag, last_lag


def analyze(path: Path, expected_hz: float, start_seconds: float,
            end_seconds: float, window_seconds: float, hop_seconds: float,
            cents_bound: float = 80.0,
            selected_channels: Sequence[int] | None = None) -> dict[str, Any]:
    path = path.absolute()
    if not path.is_file() or path.is_symlink():
        raise PitchError(f"input must be a regular file: {path}")
    channels, sample_rate, sample_width, frame_count = read_pcm_wave(path)
    if selected_channels is None:
        channel_indexes = list(range(len(channels)))
    else:
        channel_indexes = []
        for channel in selected_channels:
            if channel < 1 or channel > len(channels):
                raise PitchError(f"channel is out of range: {channel}")
            index = channel - 1
            if index in channel_indexes:
                raise PitchError(f"duplicate channel: {channel}")
            channel_indexes.append(index)
        if not channel_indexes:
            raise PitchError("at least one channel is required")
    duration = frame_count / sample_rate
    if end_seconds <= start_seconds or end_seconds > duration + 0.5 / sample_rate:
        raise PitchError("analysis bounds must be ordered and inside the input")
    window_frames = round(window_seconds * sample_rate)
    hop_frames = round(hop_seconds * sample_rate)
    start_frame = round(start_seconds * sample_rate)
    end_frame = min(frame_count, round(end_seconds * sample_rate))
    if window_frames < 4 or hop_frames < 1:
        raise PitchError("window or hop is too short")
    stop = max(start_frame + 1, end_frame - window_frames + 1)
    starts = list(range(start_frame, stop, hop_frames))
    estimates: list[dict[str, Any]] = []
    by_channel: dict[int, list[float]] = {
        index: [] for index in channel_indexes
    }
    searched_lags: tuple[int, int] | None = None
    for frame_start in starts:
        if frame_start + window_frames > frame_count:
            continue
        for channel_index in channel_indexes:
            frame = channels[channel_index][frame_start:frame_start + window_frames]
            pitch_hz, first_lag, last_lag = bounded_yin(
                frame, sample_rate, expected_hz, cents_bound)
            if searched_lags is None:
                searched_lags = (first_lag, last_lag)
            by_channel[channel_index].append(pitch_hz)
            estimates.append({
                "channel": channel_index + 1,
                "start_frame": frame_start,
                "start_seconds": frame_start / sample_rate,
                "pitch_hz": pitch_hz,
            })
    pitches = [row["pitch_hz"] for row in estimates]
    if not pitches or searched_lags is None:
        raise PitchError("analysis bounds contain no full pitch window")
    pitch_hz = statistics.median(pitches)
    mad_hz = statistics.median(abs(value - pitch_hz) for value in pitches)
    return {
        "schema": "hwa-bounded-pitch",
        "schema_version": 1,
        "source": {
            "name": path.name,
            "sha256": sha256(path),
            "bytes": path.stat().st_size,
        },
        "audio": {
            "sample_rate": sample_rate,
            "channels": len(channels),
            "sample_width_bits": sample_width * 8,
            "frame_count": frame_count,
            "duration_seconds": duration,
        },
        "request": {
            "expected_hz": expected_hz,
            "start_seconds": start_seconds,
            "end_seconds": end_seconds,
            "window_seconds": window_seconds,
            "hop_seconds": hop_seconds,
            "cents_bound": cents_bound,
            "channels": [index + 1 for index in channel_indexes],
        },
        "method": {
            "id": "bounded-yin-difference",
            "mean_removed": True,
            "window": "symmetric-hann",
            "difference": "mean-squared-sample-difference",
            "lag_interpolation": "three-point-parabolic",
            "lag_padding": 2,
            "searched_lags": list(searched_lags),
        },
        "summary": {
            "frame_count_per_channel": len(pitches) // len(channel_indexes),
            "estimate_count": len(pitches),
            "pitch_hz": pitch_hz,
            "mad_hz": mad_hz,
            "minimum_hz": min(pitches),
            "maximum_hz": max(pitches),
            "channel_medians_hz": [
                statistics.median(by_channel[index]) for index in channel_indexes
            ],
            "cents_from_expected": 1200.0 * math.log2(pitch_hz / expected_hz),
            "a4_equivalent_hz": 440.0 * pitch_hz / expected_hz,
        },
        "estimates": estimates,
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("input", type=Path, help="uncompressed PCM WAVE input")
    result.add_argument("--expected-hz", required=True, type=positive)
    result.add_argument("--start-seconds", required=True, type=nonnegative)
    result.add_argument("--end-seconds", required=True, type=positive)
    result.add_argument("--window-seconds", required=True, type=positive)
    result.add_argument("--hop-seconds", required=True, type=positive)
    result.add_argument("--cents-bound", type=positive, default=80.0)
    result.add_argument("--channel", type=int, action="append",
                        help="one-based channel; repeat as needed; default: all")
    result.add_argument("--output", type=Path,
                        help="write JSON to this new file instead of stdout")
    return result


def run(arguments: argparse.Namespace) -> dict[str, Any]:
    return analyze(
        arguments.input, arguments.expected_hz, arguments.start_seconds,
        arguments.end_seconds, arguments.window_seconds, arguments.hop_seconds,
        arguments.cents_bound, arguments.channel)


def main(argv: Sequence[str] | None = None) -> int:
    arguments = parser().parse_args(argv)
    try:
        result = run(arguments)
        text = json.dumps(result, indent=2, sort_keys=True, allow_nan=False) + "\n"
        if arguments.output is None:
            sys.stdout.write(text)
        else:
            output = arguments.output.absolute()
            if output.exists():
                raise PitchError(f"output already exists: {output}")
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(text, encoding="utf-8")
    except (OSError, PitchError) as error:
        print(f"bounded_pitch: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
