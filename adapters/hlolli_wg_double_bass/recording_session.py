#!/usr/bin/env python3
"""Validate private controlled-double-bass recording-session evidence."""

from __future__ import annotations

import argparse
from datetime import datetime
import hashlib
import hmac
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys
from typing import Any, BinaryIO, Iterable


MANIFEST_SCHEMA = "hwa-double-bass-recording-session"
SUMMARY_SCHEMA = "hwa-double-bass-recording-session-summary"
CAMPAIGN_SCHEMA = "hwa-double-bass-recording-campaign"
CAMPAIGN_SUMMARY_SCHEMA = "hwa-double-bass-recording-campaign-summary"
SCHEMA_VERSION = 1
MAX_MANIFEST_BYTES = 1024 * 1024
MAX_CHANNELS = 32
MAX_SOURCE_FILES = 64
MAX_TAKES = 4096
SAMPLE_RATE_HZ = 96000
BITS_PER_SAMPLE = 24
POSITION_RATIO_REL_TOLERANCE = 1e-9
POSITION_RATIO_ABS_TOLERANCE = 1e-12

ID_PATTERN = re.compile(r"[a-z0-9][a-z0-9._-]{0,99}")
PITCH_PATTERN = re.compile(r"[A-G](?:#|b)?[0-9]")
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}")
TIMESTAMP_PATTERN = re.compile(
    r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}"
    r"(?:Z|[+-][0-9]{2}:[0-9]{2})"
)
STRING_PITCHES = {
    "e": "E1",
    "a": "A1",
    "d": "D2",
    "g": "G2",
}
CHANNEL_ROLES = frozenset(("instrument", "measurement", "room", "sync"))
TAKE_KINDS = (
    "passive-pizzicato",
    "room-tone",
    "steady-arco",
    "sync",
)
COMMON_TAKE_FIELDS = frozenset((
    "file_id", "frame_count", "id", "kind", "start_frame",
))
CONTROL_CELLS = {
    "low-slow-tasto-down": ("low", "slow", "tasto", "down-bow"),
    "low-medium-middle-up": ("low", "medium", "middle", "up-bow"),
    "medium-slow-ponticello-up": (
        "medium", "slow", "ponticello", "up-bow",
    ),
    "medium-fast-tasto-down": ("medium", "fast", "tasto", "down-bow"),
    "high-medium-ponticello-down": (
        "high", "medium", "ponticello", "down-bow",
    ),
    "high-fast-middle-up": ("high", "fast", "middle", "up-bow"),
}


class ContractError(ValueError):
    """The manifest or one of its bound source files is invalid."""


def unique_object(pairs: Iterable[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ContractError("duplicate JSON key: " + key)
        result[key] = value
    return result


def exact_object(value: Any, fields: Iterable[str], name: str) -> dict[str, Any]:
    expected = set(fields)
    if type(value) is not dict or set(value) != expected:
        raise ContractError(name + " has invalid fields")
    return value


def bounded_list(value: Any, name: str, maximum: int) -> list[Any]:
    if type(value) is not list or not value or len(value) > maximum:
        raise ContractError(name + " must be a nonempty bounded array")
    return value


def stable_id(value: Any, name: str) -> str:
    if type(value) is not str or ID_PATTERN.fullmatch(value) is None:
        raise ContractError(name + " must be a stable lower-case id")
    return value


def pitch(value: Any, name: str) -> str:
    if type(value) is not str or PITCH_PATTERN.fullmatch(value) is None:
        raise ContractError(name + " must be a scientific pitch name")
    return value


def finite_number(value: Any, name: str, minimum: float,
                  maximum: float, *, minimum_inclusive: bool = True,
                  maximum_inclusive: bool = True) -> float:
    if type(value) not in (int, float):
        raise ContractError(name + " must be a finite number")
    try:
        result = float(value)
    except (OverflowError, ValueError) as error:
        raise ContractError(name + " must be a finite number") from error
    lower_ok = result >= minimum if minimum_inclusive else result > minimum
    upper_ok = result <= maximum if maximum_inclusive else result < maximum
    if not math.isfinite(result) or not lower_ok or not upper_ok:
        raise ContractError(name + " is outside its valid bounds")
    return result


def bounded_integer(value: Any, name: str, minimum: int,
                    maximum: int = 0xffffffffffffffff) -> int:
    if type(value) is not int or value < minimum or value > maximum:
        raise ContractError(name + " is outside its valid bounds")
    return value


def normalized_relative_path(value: Any, name: str) -> str:
    if type(value) is not str or not value or "\\" in value or "\0" in value:
        raise ContractError(name + " must be a normalized relative POSIX path")
    parts = value.split("/")
    candidate = PurePosixPath(value)
    if (candidate.is_absolute() or any(part in ("", ".", "..") for part in parts)
            or candidate.as_posix() != value):
        raise ContractError(name + " must be a normalized relative POSIX path")
    return value


def _open_regular(path: Path, name: str) -> tuple[BinaryIO, os.stat_result]:
    try:
        before = os.lstat(path)
        if not stat.S_ISREG(before.st_mode):
            raise OSError
        flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
        flags |= getattr(os, "O_NOFOLLOW", 0)
        descriptor = os.open(path, flags)
    except OSError as error:
        raise ContractError(
            name + " must be a readable regular non-symlink file"
        ) from error
    try:
        opened = os.fstat(descriptor)
        after = os.lstat(path)
        identities = (
            (before.st_dev, before.st_ino),
            (opened.st_dev, opened.st_ino),
            (after.st_dev, after.st_ino),
        )
        if (not stat.S_ISREG(opened.st_mode)
                or not stat.S_ISREG(after.st_mode)
                or len(set(identities)) != 1):
            raise OSError
        return os.fdopen(descriptor, "rb"), opened
    except (OSError, ValueError) as error:
        os.close(descriptor)
        raise ContractError(
            name + " must be a readable regular non-symlink file"
        ) from error


def _same_open_file(path: Path, opened: os.stat_result,
                    current: os.stat_result) -> bool:
    try:
        named = os.lstat(path)
    except OSError:
        return False
    return (
        stat.S_ISREG(named.st_mode)
        and (opened.st_dev, opened.st_ino) == (named.st_dev, named.st_ino)
        and (opened.st_dev, opened.st_ino) == (current.st_dev, current.st_ino)
        and opened.st_size == current.st_size
        and current.st_size == named.st_size
        and opened.st_mtime_ns == current.st_mtime_ns
        and current.st_mtime_ns == named.st_mtime_ns
        and opened.st_ctime_ns == current.st_ctime_ns
        and current.st_ctime_ns == named.st_ctime_ns
    )


def read_manifest(path: Path) -> bytes:
    stream, opened = _open_regular(path, "manifest")
    try:
        if opened.st_size > MAX_MANIFEST_BYTES:
            raise ContractError("manifest exceeds the byte limit")
        source = stream.read(MAX_MANIFEST_BYTES + 1)
        current = os.fstat(stream.fileno())
    except OSError as error:
        raise ContractError("cannot read manifest") from error
    finally:
        stream.close()
    if len(source) > MAX_MANIFEST_BYTES:
        raise ContractError("manifest exceeds the byte limit")
    if not _same_open_file(path, opened, current):
        raise ContractError("manifest changed while it was read")
    return source


def parse_manifest(source: bytes) -> dict[str, Any]:
    def invalid_constant(value: str) -> None:
        raise ContractError("invalid JSON number: " + value)

    try:
        value = json.loads(
            source.decode("utf-8"),
            object_pairs_hook=unique_object,
            parse_constant=invalid_constant,
        )
    except ContractError:
        raise
    except (UnicodeError, json.JSONDecodeError, RecursionError,
            ValueError) as error:
        raise ContractError("manifest is not valid UTF-8 JSON") from error
    if type(value) is not dict:
        raise ContractError("manifest root must be an object")
    return value


def hash_regular_file(path: Path, name: str) -> str:
    stream, opened = _open_regular(path, name)
    digest = hashlib.sha256()
    try:
        while True:
            block = stream.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
        current = os.fstat(stream.fileno())
    except OSError as error:
        raise ContractError("cannot read " + name) from error
    finally:
        stream.close()
    if not _same_open_file(path, opened, current):
        raise ContractError(name + " changed while it was read")
    return digest.hexdigest()


def checked_timestamp(value: Any, name: str) -> str:
    if type(value) is not str or TIMESTAMP_PATTERN.fullmatch(value) is None:
        raise ContractError(name + " must be an RFC 3339 timestamp with offset")
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as error:
        raise ContractError(
            name + " must be an RFC 3339 timestamp with offset"
        ) from error
    if parsed.tzinfo is None:
        raise ContractError(name + " must be an RFC 3339 timestamp with offset")
    return value


def _read_chunk(stream: BinaryIO, size: int, digest: Any,
                name: str) -> bytes:
    value = stream.read(size)
    digest.update(value)
    if len(value) != size:
        raise ContractError("truncated PCM24 WAVE: " + name)
    return value


def _consume_chunk(stream: BinaryIO, size: int, digest: Any,
                   name: str) -> None:
    remaining = size
    while remaining:
        block = stream.read(min(remaining, 1024 * 1024))
        if not block:
            raise ContractError("truncated PCM24 WAVE: " + name)
        digest.update(block)
        remaining -= len(block)


def inspect_wave(path: Path, name: str) -> dict[str, Any]:
    """Hash and inspect one regular WAVE file in one streamed pass."""
    stream, opened = _open_regular(path, name)
    digest = hashlib.sha256()
    try:
        header = _read_chunk(stream, 12, digest, name)
        if header[:4] != b"RIFF" or header[8:12] != b"WAVE":
            raise ContractError("invalid PCM24 WAVE header: " + name)
        declared_size = int.from_bytes(header[4:8], "little") + 8
        if declared_size != opened.st_size or declared_size < 12:
            raise ContractError("invalid PCM24 WAVE size: " + name)

        format_chunk = None
        data_size = None
        while stream.tell() < declared_size:
            if declared_size - stream.tell() < 8:
                raise ContractError("truncated PCM24 WAVE chunk: " + name)
            chunk_header = _read_chunk(stream, 8, digest, name)
            chunk_name = chunk_header[:4]
            chunk_size = int.from_bytes(chunk_header[4:8], "little")
            padded_size = chunk_size + (chunk_size & 1)
            if stream.tell() + padded_size > declared_size:
                raise ContractError("invalid PCM24 WAVE chunk size: " + name)
            if chunk_name == b"fmt ":
                if format_chunk is not None or chunk_size > 4096:
                    raise ContractError("invalid PCM24 WAVE format: " + name)
                format_chunk = _read_chunk(stream, chunk_size, digest, name)
            else:
                if chunk_name == b"data":
                    if data_size is not None:
                        raise ContractError("duplicate PCM24 WAVE data: " + name)
                    data_size = chunk_size
                _consume_chunk(stream, chunk_size, digest, name)
            if chunk_size & 1:
                _read_chunk(stream, 1, digest, name)
        current = os.fstat(stream.fileno())
    except OSError as error:
        raise ContractError("cannot read PCM24 WAVE: " + name) from error
    finally:
        stream.close()

    if not _same_open_file(path, opened, current):
        raise ContractError("source file changed while it was read: " + name)
    if format_chunk is None or data_size is None or len(format_chunk) < 16:
        raise ContractError("PCM24 WAVE lacks format or data: " + name)

    format_tag = int.from_bytes(format_chunk[0:2], "little")
    channels = int.from_bytes(format_chunk[2:4], "little")
    sample_rate = int.from_bytes(format_chunk[4:8], "little")
    byte_rate = int.from_bytes(format_chunk[8:12], "little")
    block_align = int.from_bytes(format_chunk[12:14], "little")
    bits_per_sample = int.from_bytes(format_chunk[14:16], "little")
    if format_tag == 0xfffe:
        pcm_guid = bytes.fromhex("0100000000001000800000aa00389b71")
        if (len(format_chunk) < 40
                or int.from_bytes(format_chunk[16:18], "little") < 22
                or int.from_bytes(format_chunk[18:20], "little") != 24
                or format_chunk[24:40] != pcm_guid):
            raise ContractError("WAVE extensible source is not PCM24: " + name)
    elif format_tag != 1:
        raise ContractError("WAVE source is not integer PCM: " + name)

    if (sample_rate != SAMPLE_RATE_HZ
            or channels < 1 or channels > MAX_CHANNELS
            or bits_per_sample != BITS_PER_SAMPLE
            or block_align != channels * 3
            or byte_rate != SAMPLE_RATE_HZ * block_align
            or data_size == 0 or data_size % block_align != 0):
        raise ContractError(
            name + " must be nonempty 96 kHz PCM24 WAVE with a valid layout"
        )
    return {
        "bits_per_sample": bits_per_sample,
        "channels": channels,
        "frame_count": data_size // block_align,
        "sample_rate_hz": sample_rate,
        "sha256": digest.hexdigest(),
    }


def bound_source_path(manifest_directory: Path, relative_path: str,
                      name: str) -> Path:
    """Bind a normalized path without allowing a symlinked subdirectory."""
    parts = relative_path.split("/")
    current = manifest_directory
    for part in parts[:-1]:
        current /= part
        try:
            facts = os.lstat(current)
        except OSError as error:
            raise ContractError(name + " has an invalid parent directory") from error
        if not stat.S_ISDIR(facts.st_mode):
            raise ContractError(name + " has an invalid parent directory")
    return current / parts[-1]


def validate_recording_setup(value: Any) -> dict[str, str]:
    row = exact_object(value, (
        "audio_interface_id", "bow_id", "clock_source", "instrument_id",
        "performer_id", "physical_string_logging_method", "recorder_id",
        "room_id",
    ), "recording_setup")
    for field in row:
        stable_id(row[field], "recording_setup " + field)
    return row


def validate_rights(value: Any, manifest_directory: Path) -> None:
    row = exact_object(value, (
        "analysis_permission", "audio_redistribution_permission",
        "evidence_path", "evidence_sha256", "model_fitting_permission",
        "performer_consent", "rights_holder_id",
    ), "rights")
    if row["analysis_permission"] is not True:
        raise ContractError("rights must permit private analysis")
    if row["model_fitting_permission"] is not True:
        raise ContractError("rights must permit model fitting")
    if type(row["audio_redistribution_permission"]) is not bool:
        raise ContractError(
            "rights audio_redistribution_permission must be boolean"
        )
    if row["performer_consent"] != "documented":
        raise ContractError("performer consent must be documented")
    stable_id(row["rights_holder_id"], "rights rights_holder_id")
    relative_path = normalized_relative_path(
        row["evidence_path"], "rights evidence_path",
    )
    expected_hash = row["evidence_sha256"]
    if (type(expected_hash) is not str
            or SHA256_PATTERN.fullmatch(expected_hash) is None):
        raise ContractError("rights evidence_sha256 is invalid")
    evidence_path = bound_source_path(
        manifest_directory, relative_path, "rights evidence",
    )
    actual_hash = hash_regular_file(evidence_path, "rights evidence")
    if not hmac.compare_digest(actual_hash, expected_hash):
        raise ContractError("rights evidence SHA-256 does not match")


def validate_tuning(value: Any) -> None:
    row = exact_object(value, ("a4_hz", "reference"), "tuning")
    a4_hz = finite_number(row["a4_hz"], "tuning a4_hz", 380.0, 480.0)
    reference = exact_object(row["reference"], (
        "frequency_hz", "measurement_method", "pitch",
    ), "tuning reference")
    if pitch(reference["pitch"], "tuning reference pitch") != "A4":
        raise ContractError("tuning reference pitch must be A4")
    reference_hz = finite_number(
        reference["frequency_hz"], "tuning reference frequency_hz",
        380.0, 480.0,
    )
    stable_id(reference["measurement_method"],
              "tuning reference measurement_method")
    if not math.isclose(a4_hz, reference_hz, rel_tol=0.0, abs_tol=1e-12):
        raise ContractError("A4 and reference tuning do not agree")


def validate_strings(value: Any) -> dict[str, float]:
    rows = bounded_list(value, "strings", len(STRING_PITCHES))
    lengths: dict[str, float] = {}
    for index, value_row in enumerate(rows):
        name = "string {}".format(index)
        row = exact_object(value_row, (
            "id", "length_m", "maker_model_id", "measurement_method",
            "open_pitch",
        ), name)
        string_id = stable_id(row["id"], name + " id")
        if string_id in lengths:
            raise ContractError("duplicate string id")
        expected_pitch = STRING_PITCHES.get(string_id)
        if expected_pitch is None or row["open_pitch"] != expected_pitch:
            raise ContractError(name + " has an invalid double-bass open pitch")
        lengths[string_id] = finite_number(
            row["length_m"], name + " length_m", 0.5, 1.5,
            minimum_inclusive=False,
        )
        stable_id(row["measurement_method"], name + " measurement_method")
        stable_id(row["maker_model_id"], name + " maker_model_id")
    if set(lengths) != set(STRING_PITCHES):
        raise ContractError("strings must measure e, a, d, and g")
    return lengths


def validate_channels(value: Any) -> dict[str, str]:
    rows = bounded_list(value, "channels", MAX_CHANNELS)
    channels: dict[str, str] = {}
    for index, value_row in enumerate(rows):
        name = "channel {}".format(index)
        row = exact_object(value_row, (
            "azimuth_degrees", "distance_m", "height_m", "id", "input_id",
            "measurement_method", "role", "transducer_id",
        ), name)
        channel_id = stable_id(row["id"], name + " id")
        if channel_id in channels:
            raise ContractError("duplicate channel id")
        role = row["role"]
        if type(role) is not str or role not in CHANNEL_ROLES:
            raise ContractError(name + " has an invalid role")
        stable_id(row["input_id"], name + " input_id")
        stable_id(row["transducer_id"], name + " transducer_id")
        stable_id(row["measurement_method"], name + " measurement_method")
        finite_number(row["distance_m"], name + " distance_m", 0.01, 50.0)
        finite_number(row["height_m"], name + " height_m", 0.0, 10.0)
        finite_number(
            row["azimuth_degrees"], name + " azimuth_degrees", -180.0, 180.0,
        )
        channels[channel_id] = role
    return channels


def validate_source_files(value: Any, manifest_directory: Path,
                          channel_ids: set[str]) -> tuple[dict[str, dict[str, Any]],
                                                         set[str]]:
    rows = bounded_list(value, "source_files", MAX_SOURCE_FILES)
    files: dict[str, dict[str, Any]] = {}
    used_channels: set[str] = set()
    inspection_cache: dict[str, dict[str, Any]] = {}
    for index, value_row in enumerate(rows):
        name = "source file {}".format(index)
        row = exact_object(value_row, ("id", "layout", "path", "sha256"), name)
        file_id = stable_id(row["id"], name + " id")
        if file_id in files:
            raise ContractError("duplicate source file id")
        relative_path = normalized_relative_path(row["path"], name + " path")
        expected_hash = row["sha256"]
        if type(expected_hash) is not str or SHA256_PATTERN.fullmatch(
                expected_hash) is None:
            raise ContractError(name + " has an invalid SHA-256")

        layout = exact_object(row["layout"], (
            "bits_per_sample", "channel_ids", "frame_count", "sample_rate_hz",
        ), name + " layout")
        if (type(layout["sample_rate_hz"]) is not int
                or layout["sample_rate_hz"] != SAMPLE_RATE_HZ
                or type(layout["bits_per_sample"]) is not int
                or layout["bits_per_sample"] != BITS_PER_SAMPLE):
            raise ContractError(name + " must declare 96 kHz PCM24")
        frame_count = bounded_integer(
            layout["frame_count"], name + " layout frame_count", 1,
        )
        declared_channels = bounded_list(
            layout["channel_ids"], name + " layout channel_ids", MAX_CHANNELS,
        )
        checked_channels: list[str] = []
        for channel_index, channel_value in enumerate(declared_channels):
            channel_id = stable_id(
                channel_value,
                "{} layout channel {}".format(name, channel_index),
            )
            if channel_id not in channel_ids:
                raise ContractError(name + " layout names an unknown channel")
            if channel_id in checked_channels:
                raise ContractError(name + " layout has a duplicate channel")
            checked_channels.append(channel_id)
        used_channels.update(checked_channels)

        facts = inspection_cache.get(relative_path)
        if facts is None:
            source_path = bound_source_path(manifest_directory, relative_path, name)
            facts = inspect_wave(source_path, name)
            inspection_cache[relative_path] = facts
        if not hmac.compare_digest(facts["sha256"], expected_hash):
            raise ContractError(name + " SHA-256 does not match")
        if (facts["sample_rate_hz"] != layout["sample_rate_hz"]
                or facts["bits_per_sample"] != layout["bits_per_sample"]
                or facts["channels"] != len(checked_channels)
                or facts["frame_count"] != frame_count):
            raise ContractError(name + " declared layout does not match WAVE")
        files[file_id] = {
            "frame_count": frame_count,
            "channel_count": len(checked_channels),
            "path": relative_path,
            "sha256": expected_hash,
        }
    return files, used_channels


def validate_bow(value: Any, string_length_m: float,
                 expected_direction: str, name: str) -> dict[str, float]:
    row = exact_object(value, (
        "bridge_distance_m", "direction", "force_n", "measurement_methods",
        "position_ratio", "speed_m_per_s",
    ), name)
    force_n = finite_number(
        row["force_n"], name + " force_n", 0.0, 100.0,
        minimum_inclusive=False,
    )
    speed = finite_number(
        row["speed_m_per_s"], name + " speed_m_per_s", 0.0, 20.0,
        minimum_inclusive=False,
    )
    bridge_distance = finite_number(
        row["bridge_distance_m"], name + " bridge_distance_m",
        0.0, string_length_m, minimum_inclusive=False,
        maximum_inclusive=False,
    )
    position_ratio = finite_number(
        row["position_ratio"], name + " position_ratio",
        0.0, 1.0, minimum_inclusive=False, maximum_inclusive=False,
    )
    if row["direction"] != expected_direction:
        raise ContractError(
            name + " direction does not match its declared control cell"
        )
    methods = exact_object(row["measurement_methods"], (
        "bridge_distance", "force", "speed",
    ), name + " measurement_methods")
    for measurement, method in methods.items():
        stable_id(method, name + " " + measurement + " measurement method")
    expected_ratio = bridge_distance / string_length_m
    if not math.isclose(
            position_ratio, expected_ratio,
            rel_tol=POSITION_RATIO_REL_TOLERANCE,
            abs_tol=POSITION_RATIO_ABS_TOLERANCE):
        raise ContractError(
            name + " position_ratio does not equal bridge_distance_m / length_m"
        )
    return {
        "bridge_distance_m": bridge_distance,
        "force_n": force_n,
        "speed_m_per_s": speed,
    }


def validate_pluck(value: Any, string_length_m: float,
                   name: str) -> None:
    row = exact_object(value, (
        "bridge_distance_m", "measurement_method", "position_ratio",
    ), name)
    bridge_distance = finite_number(
        row["bridge_distance_m"], name + " bridge_distance_m",
        0.0, string_length_m, minimum_inclusive=False,
        maximum_inclusive=False,
    )
    position_ratio = finite_number(
        row["position_ratio"], name + " position_ratio",
        0.0, 1.0, minimum_inclusive=False, maximum_inclusive=False,
    )
    stable_id(row["measurement_method"], name + " measurement_method")
    if not math.isclose(
            position_ratio, bridge_distance / string_length_m,
            rel_tol=POSITION_RATIO_REL_TOLERANCE,
            abs_tol=POSITION_RATIO_ABS_TOLERANCE):
        raise ContractError(
            name + " position_ratio does not equal bridge_distance_m / length_m"
        )


def ordered_levels(values: dict[str, list[float]], order: tuple[str, ...],
                   name: str) -> None:
    for lower, upper in zip(order, order[1:]):
        if max(values[lower]) >= min(values[upper]):
            raise ContractError(name + " levels are not strictly separated")


def validate_takes(value: Any, files: dict[str, dict[str, Any]],
                   string_lengths: dict[str, float],
                   split: str) -> dict[str, Any]:
    rows = bounded_list(value, "takes", MAX_TAKES)
    ids: set[str] = set()
    counts = {kind: 0 for kind in TAKE_KINDS}
    per_string = {
        string_id: {"passive-pizzicato": 0, "steady-arco": 0}
        for string_id in STRING_PITCHES
    }
    cells = {
        string_id: {cell: 0 for cell in CONTROL_CELLS}
        for string_id in STRING_PITCHES
    }
    measurements = {
        string_id: {
            "force": {level: [] for level in ("low", "medium", "high")},
            "speed": {level: [] for level in ("slow", "medium", "fast")},
            "position": {
                level: [] for level in ("ponticello", "middle", "tasto")
            },
        }
        for string_id in STRING_PITCHES
    }
    intervals: dict[str, list[tuple[int, int]]] = {
        file_id: [] for file_id in files
    }

    for index, value_row in enumerate(rows):
        name = "take {}".format(index)
        if type(value_row) is not dict:
            raise ContractError(name + " must be an object")
        kind = value_row.get("kind")
        if type(kind) is not str or kind not in counts:
            raise ContractError(name + " has an invalid kind")
        fields = set(COMMON_TAKE_FIELDS)
        if kind == "passive-pizzicato":
            fields.update(("articulation", "pitch", "pluck", "string_id"))
        elif kind == "steady-arco":
            fields.update((
                "articulation", "bow", "control_cell", "pitch", "string_id",
            ))
        row = exact_object(value_row, fields, name)

        take_id = stable_id(row["id"], name + " id")
        if take_id in ids:
            raise ContractError("duplicate take id")
        ids.add(take_id)
        file_id = stable_id(row["file_id"], name + " file_id")
        source = files.get(file_id)
        if source is None:
            raise ContractError(name + " names an unknown source file")
        start_frame = bounded_integer(row["start_frame"], name + " start_frame", 0)
        frame_count = bounded_integer(row["frame_count"], name + " frame_count", 1)
        if (start_frame > source["frame_count"]
                or frame_count > source["frame_count"] - start_frame):
            raise ContractError(name + " is outside its source file bounds")
        intervals[file_id].append((start_frame, start_frame + frame_count))

        if kind in ("passive-pizzicato", "steady-arco"):
            string_id = stable_id(row["string_id"], name + " string_id")
            if string_id not in string_lengths:
                raise ContractError(name + " names an unknown string")
            take_pitch = pitch(row["pitch"], name + " pitch")
            if take_pitch != STRING_PITCHES[string_id]:
                raise ContractError(name + " pitch does not match its open string")
            per_string[string_id][kind] += 1
            if kind == "passive-pizzicato":
                if row["articulation"] != "ordinary-pizzicato":
                    raise ContractError(
                        name + " passive articulation must be ordinary-pizzicato"
                    )
                validate_pluck(
                    row["pluck"], string_lengths[string_id], name + " pluck",
                )
            else:
                if row["articulation"] != "sustain":
                    raise ContractError(
                        name + " steady-arco articulation must be sustain"
                    )
                cell = row["control_cell"]
                if type(cell) is not str or cell not in CONTROL_CELLS:
                    raise ContractError(name + " has an invalid control cell")
                force_level, speed_level, position_level, direction = (
                    CONTROL_CELLS[cell]
                )
                bow = validate_bow(
                    row["bow"], string_lengths[string_id], direction,
                    name + " bow",
                )
                cells[string_id][cell] += 1
                measurements[string_id]["force"][force_level].append(
                    bow["force_n"]
                )
                measurements[string_id]["speed"][speed_level].append(
                    bow["speed_m_per_s"]
                )
                measurements[string_id]["position"][position_level].append(
                    bow["bridge_distance_m"]
                )
        counts[kind] += 1

    for file_id, file_intervals in intervals.items():
        previous_end = 0
        for start, end in sorted(file_intervals):
            if start < previous_end:
                raise ContractError(
                    "takes overlap within source file " + file_id
                )
            previous_end = end

    passive_minimum = 5 if split == "fit" else 3
    cell_minimum = 2 if split == "fit" else 1
    for string_id in STRING_PITCHES:
        if per_string[string_id]["passive-pizzicato"] < passive_minimum:
            raise ContractError(
                split + " split lacks passive repetitions for string " + string_id
            )
        for cell, count in cells[string_id].items():
            if count < cell_minimum:
                raise ContractError(
                    split + " split lacks control cell " + cell
                    + " for string " + string_id
                )
        ordered_levels(
            measurements[string_id]["force"], ("low", "medium", "high"),
            string_id + " bow-force",
        )
        ordered_levels(
            measurements[string_id]["speed"], ("slow", "medium", "fast"),
            string_id + " bow-speed",
        )
        ordered_levels(
            measurements[string_id]["position"],
            ("ponticello", "middle", "tasto"),
            string_id + " contact-position",
        )
    if counts["room-tone"] < 1 or counts["sync"] < 1:
        raise ContractError("each session needs room-tone and sync takes")
    return {"counts": counts, "per_string": per_string}

def validate_manifest(path: Path) -> dict[str, Any]:
    supplied = Path(path)
    if not supplied.is_absolute():
        raise ContractError("manifest path must be absolute")
    manifest_path = supplied.absolute()
    source = read_manifest(manifest_path)
    manifest_hash = hashlib.sha256(source).hexdigest()
    value = parse_manifest(source)
    root = exact_object(value, (
        "channels", "processing", "recorded_at", "recording_setup", "rights",
        "schema", "schema_version", "session_id", "source_files",
        "source_group_id", "split", "strings", "takes", "tuning",
    ), "manifest")
    if (type(root["schema"]) is not str or root["schema"] != MANIFEST_SCHEMA
            or type(root["schema_version"]) is not int
            or root["schema_version"] != SCHEMA_VERSION):
        raise ContractError("unsupported manifest schema or version")
    session_id = stable_id(root["session_id"], "session_id")
    source_group_id = stable_id(root["source_group_id"], "source_group_id")
    checked_timestamp(root["recorded_at"], "recorded_at")
    split = root["split"]
    if split not in ("check", "fit"):
        raise ContractError("split must be fit or check")
    if type(root["processing"]) is not list or root["processing"]:
        raise ContractError("processing must be an empty array")

    setup = validate_recording_setup(root["recording_setup"])
    validate_rights(root["rights"], manifest_path.parent)
    validate_tuning(root["tuning"])
    string_lengths = validate_strings(root["strings"])
    channels = validate_channels(root["channels"])
    if "instrument" not in set(channels.values()):
        raise ContractError("session needs at least one instrument channel")
    files, used_channels = validate_source_files(
        root["source_files"], manifest_path.parent, set(channels),
    )
    if used_channels != set(channels):
        raise ContractError("every declared channel must occur in a source layout")
    take_facts = validate_takes(root["takes"], files, string_lengths, split)
    channel_counts = sorted({row["channel_count"] for row in files.values()})
    return {
        "counts": {
            "channels": len(channels),
            "source_files": len(files),
            "takes": sum(take_facts["counts"].values()),
            "takes_by_kind": take_facts["counts"],
            "takes_by_string": take_facts["per_string"],
        },
        "layout": {
            "bits_per_sample": BITS_PER_SAMPLE,
            "channel_counts": channel_counts,
            "sample_rate_hz": SAMPLE_RATE_HZ,
        },
        "manifest_sha256": manifest_hash,
        "schema": SUMMARY_SCHEMA,
        "schema_version": SCHEMA_VERSION,
        "session": {
            "a4_hz": float(root["tuning"]["a4_hz"]),
            "bow_id": setup["bow_id"],
            "instrument_id": setup["instrument_id"],
            "session_id": session_id,
            "source_file_sha256": sorted(
                row["sha256"] for row in files.values()
            ),
            "source_group_id": source_group_id,
            "split": split,
            "strings_sha256": hashlib.sha256(json.dumps(
                root["strings"], allow_nan=False,
                separators=(",", ":"), sort_keys=True,
            ).encode("utf-8")).hexdigest(),
        },
    }


def validate_campaign(path: Path) -> dict[str, Any]:
    supplied = Path(path)
    if not supplied.is_absolute():
        raise ContractError("campaign path must be absolute")
    campaign_path = supplied.absolute()
    source = read_manifest(campaign_path)
    campaign_hash = hashlib.sha256(source).hexdigest()
    value = parse_manifest(source)
    root = exact_object(value, (
        "campaign_id", "processing", "schema", "schema_version", "sessions",
        "split_policy",
    ), "campaign")
    if (root["schema"] != CAMPAIGN_SCHEMA
            or type(root["schema_version"]) is not int
            or root["schema_version"] != SCHEMA_VERSION):
        raise ContractError("unsupported campaign schema or version")
    stable_id(root["campaign_id"], "campaign_id")
    if root["split_policy"] != "source-group-separated-before-fitting":
        raise ContractError("campaign split policy is invalid")
    if type(root["processing"]) is not list or root["processing"]:
        raise ContractError("campaign processing must be an empty array")
    rows = bounded_list(root["sessions"], "campaign sessions", 2)
    if len(rows) != 2:
        raise ContractError("campaign must contain exactly fit and check sessions")

    summaries: dict[str, dict[str, Any]] = {}
    paths: set[str] = set()
    hashes: set[str] = set()
    for index, value_row in enumerate(rows):
        name = "campaign session {}".format(index)
        row = exact_object(value_row, ("manifest_path", "sha256", "split"), name)
        split = row["split"]
        if split not in ("fit", "check") or split in summaries:
            raise ContractError("campaign needs one fit and one check session")
        relative_path = normalized_relative_path(
            row["manifest_path"], name + " manifest_path",
        )
        expected_hash = row["sha256"]
        if (type(expected_hash) is not str
                or SHA256_PATTERN.fullmatch(expected_hash) is None):
            raise ContractError(name + " has an invalid SHA-256")
        if relative_path in paths or expected_hash in hashes:
            raise ContractError("campaign sessions must bind distinct manifests")
        paths.add(relative_path)
        hashes.add(expected_hash)
        manifest_path = bound_source_path(
            campaign_path.parent, relative_path, name,
        )
        summary = validate_manifest(manifest_path.absolute())
        if not hmac.compare_digest(summary["manifest_sha256"], expected_hash):
            raise ContractError(name + " manifest SHA-256 does not match")
        if summary["session"]["split"] != split:
            raise ContractError(name + " split does not match its manifest")
        summaries[split] = summary

    fit = summaries["fit"]["session"]
    check = summaries["check"]["session"]
    if fit["session_id"] == check["session_id"]:
        raise ContractError("fit and check need distinct recording sessions")
    if fit["source_group_id"] == check["source_group_id"]:
        raise ContractError("fit and check need distinct source groups")
    if fit["instrument_id"] != check["instrument_id"]:
        raise ContractError("fit and check must record the same instrument")
    if fit["bow_id"] != check["bow_id"]:
        raise ContractError("fit and check must use the same bow")
    if fit["strings_sha256"] != check["strings_sha256"]:
        raise ContractError("fit and check string declarations differ")
    if not math.isclose(fit["a4_hz"], check["a4_hz"], rel_tol=0.0,
                        abs_tol=1e-12):
        raise ContractError("fit and check tuning references differ")
    if set(fit["source_file_sha256"]) & set(check["source_file_sha256"]):
        raise ContractError("fit and check reuse source audio")

    return {
        "campaign_sha256": campaign_hash,
        "counts": {
            "sessions": 2,
            "source_files": sum(
                summary["counts"]["source_files"]
                for summary in summaries.values()
            ),
            "takes": sum(
                summary["counts"]["takes"] for summary in summaries.values()
            ),
        },
        "layout": {
            "bits_per_sample": BITS_PER_SAMPLE,
            "sample_rate_hz": SAMPLE_RATE_HZ,
        },
        "schema": CAMPAIGN_SUMMARY_SCHEMA,
        "schema_version": SCHEMA_VERSION,
        "sessions": {
            split: {
                "manifest_sha256": summary["manifest_sha256"],
                "source_files": summary["counts"]["source_files"],
                "takes": summary["counts"]["takes"],
            }
            for split, summary in sorted(summaries.items())
        },
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Validate private controlled-double-bass recording evidence."
        ),
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    session = subparsers.add_parser(
        "validate-session", help="validate one session without writing",
    )
    session.add_argument(
        "--manifest", required=True, type=Path,
        help="absolute path to the private session JSON manifest",
    )
    campaign = subparsers.add_parser(
        "validate-campaign", help="validate the frozen fit/check campaign",
    )
    campaign.add_argument(
        "--campaign", required=True, type=Path,
        help="absolute path to the private campaign JSON manifest",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    arguments = build_parser().parse_args(argv)
    try:
        if arguments.command == "validate-session":
            summary = validate_manifest(arguments.manifest)
        elif arguments.command == "validate-campaign":
            summary = validate_campaign(arguments.campaign)
        else:
            raise ContractError("unsupported command")
    except ContractError as error:
        print("error: " + str(error), file=sys.stderr)
        return 2
    print(json.dumps(summary, allow_nan=False, separators=(",", ":"),
                     sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
