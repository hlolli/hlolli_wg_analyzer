#!/usr/bin/env python3
"""Public CLI checks for the isolated-note analyzer."""

import argparse
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
from typing import Optional


EXPECTED_HZ = 41.20344461410875
PITCH = 1
PASSIVE_DECAY = 2
REJECT_SILENCE = 1
REJECT_NOISE = 2
REJECT_OCTAVE = 4
REJECT_BOUNDARY = 8
REJECT_LATE_PULSE = 128


def write_decay(path: Path) -> None:
    rate = 48_000
    frames = rate * 3
    lead = rate // 10
    samples = bytearray()
    for frame in range(frames):
        value = 0.0
        if frame >= lead:
            time = (frame - lead) / rate
            phase = math.tau * EXPECTED_HZ * time
            value = 0.82 * math.exp(-time / 0.55) * (
                0.76 * math.sin(phase) + 0.24 * math.sin(2 * phase + 0.2)
            )
        samples += struct.pack("<h", round(max(-1, min(1, value)) * 32767))
    fmt = struct.pack("<HHIIHH", 1, 1, rate, rate * 2, 2, 16)
    chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt
    chunks += b"data" + struct.pack("<I", len(samples)) + samples
    path.write_bytes(b"RIFF" + struct.pack("<I", 4 + len(chunks)) + b"WAVE" + chunks)


def sample_bytes(value: float, bits: int) -> bytes:
    value = max(-1, min(1, value))
    if bits == 16:
        return struct.pack("<h", round(value * 32767))
    encoded = round(value * 8388607) & 0xFFFFFF
    return encoded.to_bytes(3, "little")


def write_note(
    path: Path,
    *,
    rate: int,
    bits: int,
    channels: int,
    extensible: bool,
    frequency: float,
    duration: float = 1.6,
    late_pulse: Optional[float] = None,
) -> None:
    frames = round(rate * duration)
    lead = round(rate * 0.05)
    audio = bytearray()
    for frame in range(frames):
        value = 0.0
        if frame >= lead:
            time = (frame - lead) / rate
            value = 0.75 * math.exp(-time / 0.75) * (
                0.82 * math.sin(math.tau * frequency * time)
                + 0.18 * math.sin(2 * math.tau * frequency * time + 0.31)
            )
            if late_pulse is not None and time >= late_pulse:
                pulse_time = time - late_pulse
                value += 0.55 * math.exp(-pulse_time / 0.45) * math.sin(
                    math.tau * frequency * pulse_time
                )
        for channel in range(channels):
            audio += sample_bytes(value * (1 - 0.15 * channel), bits)
    width = bits // 8
    block_align = channels * width
    if extensible:
        pcm_guid = bytes.fromhex("0100000000001000800000aa00389b71")
        channel_mask = (1 << channels) - 1
        fmt = struct.pack(
            "<HHIIHHHHI",
            0xFFFE,
            channels,
            rate,
            rate * block_align,
            block_align,
            bits,
            22,
            bits,
            channel_mask,
        ) + pcm_guid
    else:
        fmt = struct.pack(
            "<HHIIHH", 1, channels, rate, rate * block_align, block_align, bits
        )
    chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt
    chunks += b"data" + struct.pack("<I", len(audio)) + audio
    path.write_bytes(b"RIFF" + struct.pack("<I", 4 + len(chunks)) + b"WAVE" + chunks)


def write_pcm16_mono(path: Path, rate: int, samples) -> None:
    audio = bytearray()
    for value in samples:
        audio += sample_bytes(value, 16)
    fmt = struct.pack("<HHIIHH", 1, 1, rate, rate * 2, 2, 16)
    chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt
    chunks += b"data" + struct.pack("<I", len(audio)) + audio
    path.write_bytes(b"RIFF" + struct.pack("<I", 4 + len(chunks)) + b"WAVE" + chunks)


def run_note(analyzer: Path, wave: Path, expected: float, metrics: str) -> dict:
    process = subprocess.run(
        [
            str(analyzer),
            "isolated-note",
            str(wave),
            "--expected-hz",
            repr(expected),
            "--metrics",
            metrics,
        ],
        check=True,
        capture_output=True,
    )
    assert process.stderr == b""
    return json.loads(process.stdout)


def test_json_contract(analyzer: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="hwa-isolated-note-cli-") as folder:
        wave = Path(folder) / "low-e.wav"
        write_decay(wave)
        command = [
            str(analyzer),
            "isolated-note",
            str(wave),
            "--expected-hz",
            repr(EXPECTED_HZ),
            "--metrics",
            "pitch,passive-decay",
        ]
        first = subprocess.run(command, check=True, capture_output=True)
        second = subprocess.run(command, check=True, capture_output=True)
        assert first.stderr == b""
        assert first.stdout == second.stdout
        report = json.loads(first.stdout)
        assert report["schema"] == "hwa-isolated-note"
        assert report["schema_version"] == 1
        assert report["command"] == "isolated-note"
        assert report["method"] == "isolated-note-1"
        assert report["expected_hz"] == EXPECTED_HZ
        assert report["requested_mask"] == 3
        assert report["valid_mask"] == 3
        assert report["requested_metrics"] == ["pitch", "passive-decay"]
        assert report["valid_metrics"] == ["pitch", "passive-decay"]
        assert report["rejections"] == []
        assert report["format"]["sample_rate_hz"] == 48_000
        assert report["format"]["channels"] == 1
        assert report["format"]["bits_per_sample"] == 16
        assert report["pitch"]["valid"] is True
        assert abs(report["pitch"]["cents"]) < 2
        assert report["decay"]["valid"] is True
        assert report["decay"]["start_sample"] < report["decay"]["end_sample"]
        assert report["work"]["peak_bytes"] > 0
        assert report["work"]["evaluations"] > 0


def test_all_open_strings_and_checked_formats(analyzer: Path) -> None:
    cases = [
        ("e", 41.20344461410875, 44_100, 16, 1, False),
        ("a", 55.0, 44_100, 24, 2, True),
        ("d", 73.41619197935188, 48_000, 24, 1, False),
        ("g", 97.99885899543733, 48_000, 16, 2, True),
    ]
    with tempfile.TemporaryDirectory(prefix="hwa-isolated-note-formats-") as folder:
        for name, frequency, rate, bits, channels, extensible in cases:
            wave = Path(folder) / f"{name}.wav"
            write_note(
                wave,
                rate=rate,
                bits=bits,
                channels=channels,
                extensible=extensible,
                frequency=frequency,
            )
            report = run_note(analyzer, wave, frequency, "pitch")
            assert report["requested_mask"] == PITCH
            assert report["valid_mask"] == PITCH
            assert abs(report["pitch"]["cents"]) < 2
            assert report["format"]["sample_rate_hz"] == rate
            assert report["format"]["bits_per_sample"] == bits
            assert report["format"]["channels"] == channels


def test_rejects_silence_noise_octave_and_boundary(analyzer: Path) -> None:
    rate = 48_000
    with tempfile.TemporaryDirectory(prefix="hwa-isolated-note-reject-") as folder:
        root = Path(folder)
        silence = root / "silence.wav"
        write_pcm16_mono(silence, rate, (0.0 for _ in range(rate)))
        report = run_note(analyzer, silence, 55.0, "pitch")
        assert report["valid_mask"] == 0
        assert report["rejection_mask"] & REJECT_SILENCE
        assert "silence" in report["rejections"]

        state = 1

        def noise_values():
            nonlocal state
            for _ in range(rate * 2):
                state = (1103515245 * state + 12345) & 0x7FFFFFFF
                yield 0.4 * (state / 0x3FFFFFFF - 1.0)

        noise = root / "noise.wav"
        write_pcm16_mono(noise, rate, noise_values())
        report = run_note(analyzer, noise, 55.0, "pitch")
        assert report["valid_mask"] == 0
        assert report["rejection_mask"] & REJECT_NOISE
        assert "noise" in report["rejections"]

        octave = root / "octave.wav"
        write_note(
            octave,
            rate=rate,
            bits=16,
            channels=1,
            extensible=False,
            frequency=110.0,
        )
        report = run_note(analyzer, octave, 55.0, "pitch")
        assert report["valid_mask"] == 0
        assert report["rejection_mask"] & REJECT_OCTAVE
        assert "octave" in report["rejections"]

        boundary = root / "boundary.wav"
        write_note(
            boundary,
            rate=rate,
            bits=16,
            channels=1,
            extensible=False,
            frequency=55.0 * math.pow(2, 112 / 1200),
        )
        report = run_note(analyzer, boundary, 55.0, "pitch")
        assert report["valid_mask"] == 0
        assert report["rejection_mask"] & REJECT_BOUNDARY
        assert "boundary" in report["rejections"]


def test_decay_stops_before_late_pulse(analyzer: Path) -> None:
    rate = 48_000
    late_pulse = 1.25
    with tempfile.TemporaryDirectory(prefix="hwa-isolated-note-tail-") as folder:
        wave = Path(folder) / "late-pulse.wav"
        write_note(
            wave,
            rate=rate,
            bits=24,
            channels=2,
            extensible=True,
            frequency=55.0,
            duration=2.5,
            late_pulse=late_pulse,
        )
        report = run_note(analyzer, wave, 55.0, "passive-decay")
        pulse_sample = round(rate * (0.05 + late_pulse))
        assert report["requested_mask"] == PASSIVE_DECAY
        assert report["valid_mask"] == PASSIVE_DECAY
        assert report["rejection_mask"] & REJECT_LATE_PULSE
        assert "late-pulse" in report["rejections"]
        assert report["decay"]["end_sample"] <= pulse_sample


def test_hard_caps_fail_before_unbounded_work(analyzer: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="hwa-isolated-note-caps-") as folder:
        wave = Path(folder) / "low-e.wav"
        write_decay(wave)
        base = [
            str(analyzer),
            "isolated-note",
            str(wave),
            "--expected-hz",
            repr(EXPECTED_HZ),
            "--metrics",
            "pitch,passive-decay",
        ]
        cases = [
            (["--max-work-bytes", "1024"], b"work limit exceeded"),
            (["--max-frames", "100"], b"frame limit exceeded"),
            (["--max-note-evaluations", "1000"], b"evaluation limit exceeded"),
        ]
        for options, message in cases:
            process = subprocess.run(base + options, check=False, capture_output=True)
            assert process.returncode == 1
            assert process.stdout == b""
            assert message in process.stderr


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--analyzer", type=Path, required=True)
    arguments = parser.parse_args()
    test_json_contract(arguments.analyzer)
    test_all_open_strings_and_checked_formats(arguments.analyzer)
    test_rejects_silence_noise_octave_and_boundary(arguments.analyzer)
    test_decay_stops_before_late_pulse(arguments.analyzer)
    test_hard_caps_fail_before_unbounded_work(arguments.analyzer)


if __name__ == "__main__":
    main()
