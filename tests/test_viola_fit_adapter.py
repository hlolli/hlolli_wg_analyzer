#!/usr/bin/env python3

import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock
import wave


ROOT = Path(__file__).resolve().parents[1]
ADAPTER = ROOT / "adapters" / "hlolli_wg_viola" / "adapter.py"
FIT_MANIFEST = (ROOT / "adapters" / "hlolli_wg_viola" /
                "fit-passive-c-v1.json")
FIT_MANIFESTS = {
    target: ROOT / "adapters" / "hlolli_wg_viola" /
    "fit-passive-{}-v1.json".format(target[0])
    for target in ("c3", "g3", "d4", "a4")
}
FIT_TOOL = ROOT / "tools" / "instrument_fit.py"
PYTHON = Path(sys.executable).resolve()


def load_adapter():
    spec = importlib.util.spec_from_file_location("viola_fit_adapter", ADAPTER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build_bundle(module, arguments) -> None:
    with mock.patch.object(module, "verify_csound_library"):
        module.build(arguments)


def pcm24_wave(path: Path, frames: int = 256, channels: int = 2) -> None:
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(channels)
        stream.setsampwidth(3)
        stream.setframerate(44100)
        stream.writeframes(b"\0\0\0" * frames * channels)


def pcm16_wave(path: Path, frames: int = 256, channels: int = 2) -> None:
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(channels)
        stream.setsampwidth(2)
        stream.setframerate(44100)
        stream.writeframes(b"\0\0" * frames * channels)


def extensible_pcm24_wave(path: Path, frames: int = 256,
                          channels: int = 1) -> None:
    block_align = channels * 3
    audio = b"\0\0\0" * frames * channels
    pcm_guid = bytes.fromhex("0100000000001000800000aa00389b71")
    channel_mask = 4 if channels == 1 else 3
    format_chunk = struct.pack(
        "<HHIIHHHHI16s", 0xfffe, channels, 44100,
        44100 * block_align, block_align, 24, 22, 24, channel_mask, pcm_guid,
    )
    body = (b"WAVE" + b"fmt " + struct.pack("<I", len(format_chunk)) +
            format_chunk + b"data" + struct.pack("<I", len(audio)) + audio)
    path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)


def extensible_pcm16_wave(path: Path, frames: int = 256,
                          channels: int = 1) -> None:
    block_align = channels * 2
    audio = b"\0\0" * frames * channels
    pcm_guid = bytes.fromhex("0100000000001000800000aa00389b71")
    channel_mask = 4 if channels == 1 else 3
    format_chunk = struct.pack(
        "<HHIIHHHHI16s", 0xfffe, channels, 44100,
        44100 * block_align, block_align, 16, 22, 16, channel_mask, pcm_guid,
    )
    body = (b"WAVE" + b"fmt " + struct.pack("<I", len(format_chunk)) +
            format_chunk + b"data" + struct.pack("<I", len(audio)) + audio)
    path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)


def file_hash(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def render_request(reference: Path, output: Path, value: float = 0.4,
                   case_id: str = None, split: str = "fit",
                   reference_channels: int = 1,
                   target: str = "c3", binding_id: str = None) -> dict:
    if case_id is None:
        case_id = target + "-passive-" + split
    binding = (binding_id if binding_id is not None else
               "reference_{}_{}".format(target, split))
    return {
        "schema": "hwa-render-job",
        "schema_version": 1,
        "method_version": "stage8-1",
        "case_id": case_id,
        "job_id": 1,
        "job_key": "1" * 64,
        "inputs": [{
            "binding_id": binding,
            "channels": reference_channels,
            "gain_db": 0,
            "kind": "stem",
            "path": str(reference),
            "probe_format": None,
            "probe_name": None,
            "rate_denominator": 0,
            "rate_hz": 44100,
            "rate_numerator": 0,
            "resource_id": "reference.final",
            "role": "final",
            "sha256": file_hash(reference),
            "side": "reference",
            "start_sample": 0,
            "unit": None,
            "value_count": 0,
        }],
        "outputs": [{
            "id": "model.final",
            "kind": "stem",
            "path": str(output),
            "side": "model",
            "role": "final",
            "probe_format": None,
            "probe_name": None,
            "unit": None,
            "start_sample": 0,
            "gain_db": 0,
            "rate_hz": 44100,
            "channels": 2,
            "rate_numerator": 0,
            "rate_denominator": 0,
            "value_count": 0,
        }],
        "parameters": [{
            "id": "loss_time_constant_{}_seconds".format(target[0]),
            "unit": "seconds",
            "value": value,
        }],
        "replicate": 0,
        "seed": 29042016,
        "split": split,
    }


def joint_render_request(reference: Path, output: Path, tau: float,
                         cutoff_scale: float, case_id: str, split: str,
                         target: str, reference_channels: int = 1) -> dict:
    request = render_request(
        reference, output, value=tau, case_id=case_id, split=split,
        reference_channels=reference_channels, target=target,
        binding_id=case_id,
    )
    request["parameters"] = [
        {
            "id": "loss_time_constant_{}_seconds".format(target[0]),
            "unit": "seconds",
            "value": tau,
        },
        {
            "id": "nut_bridge_cutoff_scale",
            "unit": "ratio",
            "value": cutoff_scale,
        },
    ]
    return request


def write_executable(path: Path, text: str) -> None:
    shebang = "#!/usr/bin/python3"
    if text.startswith(shebang):
        text = "#!" + str(PYTHON) + text[len(shebang):]
    path.write_text(text, encoding="utf-8")
    path.chmod(0o700)


def fake_viola_tree(root: Path) -> Path:
    viola = root / "viola"
    (viola / "src").mkdir(parents=True)
    (viola / "model" / "schema").mkdir(parents=True)
    (viola / "tools").mkdir(parents=True)
    (viola / "tests").mkdir(parents=True)
    (viola / "src" / "hlolli_wg_viola.c").write_text(
        "hlolli_wg_viola_test_string_impulse\n"
        "hlolli_wg_viola_test_passive_joint\n",
        encoding="ascii",
    )
    model = {
        "strings": [
            {"loss_time_constant_seconds": 0.25,
             "nut_cutoff_hz": 20000.0,
             "bridge_cutoff_hz": 5386.995271806526,
             "nut_loss_fraction": 0.25},
            {"loss_time_constant_seconds": 0.25,
             "nut_cutoff_hz": 4200.0,
             "bridge_cutoff_hz": 4964.244281108786,
             "nut_loss_fraction": 0.25},
            {"loss_time_constant_seconds": 0.25,
             "nut_cutoff_hz": 12000.0,
             "bridge_cutoff_hz": 7086.471045764144,
             "nut_loss_fraction": 0.25},
            {"loss_time_constant_seconds": 0.25,
             "nut_cutoff_hz": 12000.0,
             "bridge_cutoff_hz": 6024.580442039031,
             "nut_loss_fraction": 0.25},
        ]
    }
    (viola / "model" / "viola-v1.json").write_text(
        json.dumps(model), encoding="utf-8"
    )
    (viola / "model" / "schema" / "viola-model-v1.schema.json").write_text(
        "{}", encoding="ascii"
    )
    (viola / "tests" / "fit_passive.csd").write_text(
        "<CsoundSynthesizer>\n"
        "<CsInstruments>\n"
        "#ifndef FIT_A4\n"
        "#define FIT_A4 #440#\n"
        "#endif\n"
        "giViola hlolli_wg_viola_create $FIT_A4\n"
        "</CsInstruments>\n"
        "</CsoundSynthesizer>\n",
        encoding="ascii",
    )
    (viola / "tests" / "fit_passive_joint.csd").write_text(
        "<CsoundSynthesizer>\n"
        "<CsInstruments>\n"
        "#ifndef FIT_A4\n"
        "#define FIT_A4 #440#\n"
        "#endif\n"
        "giViola hlolli_wg_viola_create $FIT_A4\n"
        "iOk hlolli_wg_viola_test_passive_joint giViola, "
        "$FIT_STRING, $FIT_PASSIVE_TAU, $FIT_CUTOFF_SCALE\n"
        "</CsInstruments>\n"
        "</CsoundSynthesizer>\n",
        encoding="ascii",
    )
    write_executable(
        viola / "tools" / "generate_model.py",
        """#!/usr/bin/python3
import argparse
import json
from pathlib import Path

def load_model(model_path, schema_path):
    value = json.loads(Path(model_path).read_text())
    if not isinstance(value, dict) or not Path(schema_path).is_file():
        raise ValueError("invalid model")
    return {"data": value}, {}

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--schema", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    arguments = parser.parse_args()
    if arguments.self_test:
        return 0
    value = json.loads(arguments.model.read_text())
    losses = [row["loss_time_constant_seconds"] for row in value["strings"]]
    line = "candidate_losses={}\\n".format(json.dumps(losses, separators=(",", ":")))
    if arguments.check:
        if line not in arguments.source.read_text(encoding="ascii"):
            raise SystemExit(9)
    else:
        with arguments.source.open("a", encoding="ascii") as stream:
            stream.write(line)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
""",
    )
    return viola


def fake_tools(root: Path) -> tuple[Path, Path, Path]:
    compiler = root / "cc"
    csound = root / "csound"
    csound_library = root / "libcsound64.dylib"
    csound_library.write_bytes(b"fixed fake Csound library\n")
    write_executable(
        compiler,
        """#!/usr/bin/python3
from pathlib import Path
import os
import sys

source = next(Path(row) for row in sys.argv[1:] if row.endswith(".c"))
if not os.environ.get("TMPDIR") or not Path(os.environ["TMPDIR"]).is_dir():
    print("compiler has no private temporary directory", file=sys.stderr)
    raise SystemExit(6)
count_path = Path(__file__).with_suffix(".count")
count = int(count_path.read_text()) + 1 if count_path.exists() else 1
count_path.write_text(str(count))
line = next((row for row in source.read_text().splitlines()
             if row.startswith("candidate_losses=")), None)
output = Path(sys.argv[sys.argv.index("-o") + 1])
if line is None:
    if Path(os.environ["TMPDIR"]).resolve() not in source.resolve().parents:
        print("joint compile did not use a staged source", file=sys.stderr)
        raise SystemExit(8)
    text = source.read_text(encoding="ascii")
    if ("hlolli_wg_viola_test_passive_joint" not in text or
            "-DHLOLLI_WG_VIOLA_TEST_API=1" not in sys.argv):
        print("candidate value did not reach generated source", file=sys.stderr)
        raise SystemExit(7)
    output.write_text("joint_test_module\\n", encoding="ascii")
else:
    output.write_text(line, encoding="ascii")
""",
    )
    write_executable(
        csound,
        """#!/usr/bin/python3
from pathlib import Path
import sys
import wave
import json
import math

if "--version" in sys.argv:
    print("--Csound version 7.0", file=sys.stderr)
    raise SystemExit(0)

module = next(row.split("=", 1)[1] for row in sys.argv
              if row.startswith("--opcode-lib="))
if not Path(module).is_file():
    raise SystemExit(8)
module_text = Path(module).read_text().strip()
joint = module_text == "joint_test_module"
losses = None if joint else json.loads(module_text.split("=", 1)[1])
string = int(next(row.split("=", 1)[1] for row in sys.argv
                  if row.startswith("--omacro:FIT_STRING=")))
frequency = float(next(row.split("=", 1)[1] for row in sys.argv
                       if row.startswith("--omacro:FIT_FREQUENCY=")))
a4 = float(next(row.split("=", 1)[1] for row in sys.argv
                if row.startswith("--omacro:FIT_A4=")))
csd_text = Path(sys.argv[-1]).read_text()
if "giViola hlolli_wg_viola_create $FIT_A4" not in csd_text:
    print("fit render did not use the numeric-A4 constructor", file=sys.stderr)
    raise SystemExit(10)
nominal = [130.8127826502993, 195.99771799087463,
           293.6647679174076, 440.0][string - 1]
if abs(frequency - nominal * a4 / 440.0) > 1e-10:
    print("fit frequency and numeric A4 disagree", file=sys.stderr)
    raise SystemExit(11)
if joint:
    tau = float(next(row.split("=", 1)[1] for row in sys.argv
                     if row.startswith("--omacro:FIT_PASSIVE_TAU=")))
    cutoff_scale = float(next(row.split("=", 1)[1] for row in sys.argv
                              if row.startswith("--omacro:FIT_CUTOFF_SCALE=")))
    total = int(next(row.split("=", 1)[1] for row in sys.argv
                     if row.startswith("--omacro:FIT_TOTAL_SECONDS=")))
    if (total != 16 or
            "hlolli_wg_viola_test_passive_joint" not in csd_text):
        print("joint render did not use its fixed probe", file=sys.stderr)
        raise SystemExit(12)
    nut_cutoffs = [20000.0, 4200.0, 12000.0, 12000.0]
    bridge_cutoffs = [5386.995271806526, 4964.244281108786,
                      7086.471045764144, 6024.580442039031]
    cap = 0.45 * 44100.0
    nut_cutoff = min(nut_cutoffs[string - 1] * cutoff_scale, cap)
    bridge_cutoff = min(bridge_cutoffs[string - 1] * cutoff_scale, cap)
    loop_gain = math.exp(-1.0 / (frequency * tau))
    nut_pole = math.exp(-2.0 * math.pi * nut_cutoff / 44100.0)
    bridge_pole = math.exp(-2.0 * math.pi * bridge_cutoff / 44100.0)
    nut_gain = min(max(math.pow(loop_gain, 0.25), 0.0), 0.99995)
    bridge_gain = min(max(math.pow(loop_gain, 0.75), 0.0), 0.99995)
    coefficients = [nut_pole, nut_gain, bridge_pole, bridge_gain]
    corrupt = {
        0.5: 0,
        1.0: 1,
        2.0: 2,
        4.0: 3,
    }
    if tau == 4.0:
        coefficients[corrupt[cutoff_scale]] = 0.123
    suffix = " extra" if tau == 5.0 and cutoff_scale == 4.0 else ""
    print("WG_PASSIVE_JOINT_FACTS {:.17g} {:.17g} {:.17g} {:.17g} "
          "{:.17g} {:.17g} {:.17g} {:.17g} 1{}".format(
              string, frequency, tau, cutoff_scale, *coefficients, suffix))
    loss = tau
else:
    cutoff_scale = 1.0
    loss = float(losses[string - 1])
sample = max(1, min(8388607, round(loss * 100000))).to_bytes(
    3, "little", signed=True)
output = Path(sys.argv[sys.argv.index("-o") + 1])
frames = 705600 if joint else (176415 if loss == 0.03 else 176416)
if loss == 0.02:
    sample = b"\\0\\0\\0"
with wave.open(str(output), "wb") as stream:
    stream.setnchannels(2)
    stream.setsampwidth(3)
    stream.setframerate(44100)
    if loss == 5.0:
        sample = (8388607).to_bytes(3, "little", signed=True)
    if joint:
        scale_sample = round(cutoff_scale * 100000).to_bytes(
            3, "little", signed=True)
        frequency_sample = round(frequency * 10000).to_bytes(
            3, "little", signed=True)
        a4_sample = round(a4 * 10000).to_bytes(
            3, "little", signed=True)
        stream.writeframes(
            sample * 2 + scale_sample * 2 + frequency_sample * 2 +
            a4_sample * 2 + sample * (frames - 4) * 2)
    elif loss not in (0.02, 5.0) and frames >= 3:
        frequency_sample = round(frequency * 10000).to_bytes(
            3, "little", signed=True)
        a4_sample = round(a4 * 10000).to_bytes(3, "little", signed=True)
        stream.writeframes(
            sample * 2 + frequency_sample * 2 + a4_sample * 2 +
            sample * (frames - 3) * 2)
    else:
        stream.writeframes(sample * frames * 2)
if loss == 0.04:
    with output.open("r+b") as stream:
        stream.truncate(output.stat().st_size - 1)
if loss == 4.9:
    raise SystemExit(9)
""",
    )
    return compiler, csound, csound_library


def fake_includes(root: Path) -> Path:
    includes = root / "include"
    includes.mkdir()
    (includes / "csdl.h").write_text("csdl.h\n", encoding="ascii")
    (includes / "float-version.h").write_text(
        "float-version.h\n", encoding="ascii"
    )
    (includes / "version.h").write_text(
        "#define CS_VERSION (7)\n", encoding="ascii"
    )
    return includes


def write_roster(path: Path, target: str, rows: list[tuple]) -> dict:
    cases = []
    for case_id, source_family, split, recording, frequency_hz in rows:
        cases.append({
            "id": case_id,
            "source_family": source_family,
            "split": split,
            "path": str(recording),
            "sha256": file_hash(recording),
            "frequency_hz": frequency_hz,
        })
    value = {
        "schema": "hwa-viola-passive-tail-roster",
        "schema_version": 1,
        "target": target,
        "cases": cases,
    }
    path.write_text(json.dumps(value), encoding="utf-8")
    return value


class ViolaFitAdapterTests(unittest.TestCase):
    def test_joint_passive_diagnostic_option_requires_a_roster(self) -> None:
        help_result = subprocess.run(
            [sys.executable, str(ADAPTER), "build", "--help"],
            check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, env={}, cwd=ROOT,
        )
        self.assertEqual(help_result.returncode, 0, help_result.stderr)
        self.assertIn("--joint-passive-diagnostic", help_result.stdout)
        self.assertIn("--published-output-dir", help_result.stdout)

        module = load_adapter()
        with tempfile.TemporaryDirectory(
                prefix="hwa viola joint option ") as text:
            root = Path(text)
            viola = root / "viola"
            viola.mkdir()
            with self.assertRaisesRegex(
                    module.AdapterError,
                    "--joint-passive-diagnostic requires --roster"):
                module.build(types.SimpleNamespace(
                    output_dir=root / "bundle",
                    viola_root=viola,
                    roster=None,
                    joint_passive_diagnostic=True,
                ))
            with self.assertRaisesRegex(
                    module.AdapterError,
                    "--published-output-dir requires"):
                module.build(types.SimpleNamespace(
                    output_dir=root / "bundle",
                    published_output_dir=(root / "bundle").resolve(),
                    viola_root=viola,
                    roster=None,
                    joint_passive_diagnostic=False,
                ))
            invalid_published = (
                (Path("bundle"), "absolute path"),
                ((root / "other-name").resolve(), "build output name"),
                ((ROOT / "private-joint-test-bundle").resolve(),
                 "outside the repositories"),
            )
            for published, message in invalid_published:
                with self.subTest(published=published):
                    output_name = (published.name
                                   if "repositories" in message else
                                   "bundle")
                    with self.assertRaisesRegex(module.AdapterError, message):
                        module.build(types.SimpleNamespace(
                            output_dir=root / output_name,
                            published_output_dir=published,
                            viola_root=viola,
                            roster=root / "missing-roster.json",
                            joint_passive_diagnostic=True,
                        ))
            stage = root / "stage"
            stage.mkdir()
            with self.assertRaisesRegex(module.AdapterError,
                                        "cannot be nested"):
                module.build(types.SimpleNamespace(
                    output_dir=stage / "g3",
                    published_output_dir=(
                        stage / "g3" / "sub" / "g3"
                    ).resolve(),
                    viola_root=viola,
                    roster=root / "missing-roster.json",
                    joint_passive_diagnostic=True,
                ))

    def test_joint_passive_diagnostic_rejects_values_outside_fixed_grid(
            self) -> None:
        module = load_adapter()
        with tempfile.TemporaryDirectory(
                prefix="hwa viola joint grid ") as text:
            root = Path(text)
            viola = fake_viola_tree(root)
            compiler, csound, csound_library = fake_tools(root)
            includes = fake_includes(root)
            recordings = []
            for index in range(3):
                recording = root / "tail-{}.wav".format(index)
                pcm24_wave(recording, frames=256 + index, channels=1)
                recordings.append(recording)
            roster = root / "roster.json"
            write_roster(roster, "g3", [
                ("iowa2012-g3-pizz", "iowa-2012", "fit",
                 recordings[0], 195.5),
                ("rwc-v1-g3-pizz", "rwc-variation-1", "fit",
                 recordings[1], 195.6),
                ("best-music-tools-g3-a442",
                 "best-music-tools-a442", "check",
                 recordings[2], 196.9),
            ])
            bundle = root / "bundle"
            build_bundle(module, types.SimpleNamespace(
                output_dir=bundle, viola_root=viola, csound=csound,
                csound_library=csound_library, c_compiler=compiler,
                python=PYTHON,
                csound_include_dir=[includes], roster=roster,
                joint_passive_diagnostic=True, target=None,
                reference_fit=None, reference_check=None,
                reference_c3_fit=None, reference_c3_check=None,
                fit_frequency_hz=None, check_frequency_hz=None,
                sample_count=None,
            ))
            for index, (tau, scale) in enumerate(((0.20, 1.0),
                                                   (0.25, 0.75))):
                with self.subTest(tau=tau, scale=scale):
                    job = root / "job-{}".format(index)
                    job.mkdir()
                    output = job / "model.wav"
                    request = joint_render_request(
                        recordings[0], output, tau, scale,
                        "iowa2012-g3-pizz", "fit", "g3",
                    )
                    request_path = job / "request.json"
                    request_path.write_text(
                        json.dumps(request), encoding="utf-8"
                    )
                    completed = subprocess.run(
                        [str(bundle / "renderer"), "--hwa-experiment-job",
                         str(request_path), "--output-dir", str(job)],
                        check=False, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, text=True, env={}, cwd=job,
                    )
                    self.assertNotEqual(completed.returncode, 0)
                    self.assertIn("fixed diagnostic grid", completed.stderr)
                    self.assertFalse(output.exists())

            variants = (
                "missing_cutoff", "duplicate_tau", "extra_parameter",
                "wrong_cutoff_unit", "boolean_tau", "nan_cutoff",
            )
            for index, variant in enumerate(variants):
                with self.subTest(variant=variant):
                    job = root / "malformed-job-{}".format(index)
                    job.mkdir()
                    output = job / "model.wav"
                    request = joint_render_request(
                        recordings[0], output, 0.25, 1.0,
                        "iowa2012-g3-pizz", "fit", "g3",
                    )
                    if variant == "missing_cutoff":
                        request["parameters"].pop()
                    elif variant == "duplicate_tau":
                        request["parameters"][1] = dict(
                            request["parameters"][0]
                        )
                    elif variant == "extra_parameter":
                        request["parameters"].append({
                            "id": "other", "unit": "ratio", "value": 1.0,
                        })
                    elif variant == "wrong_cutoff_unit":
                        request["parameters"][1]["unit"] = "seconds"
                    elif variant == "boolean_tau":
                        request["parameters"][0]["value"] = True
                    else:
                        request["parameters"][1]["value"] = float("nan")
                    request_path = job / "request.json"
                    request_path.write_text(
                        json.dumps(request), encoding="utf-8"
                    )
                    completed = subprocess.run(
                        [str(bundle / "renderer"), "--hwa-experiment-job",
                         str(request_path), "--output-dir", str(job)],
                        check=False, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, text=True, env={}, cwd=job,
                    )
                    self.assertNotEqual(completed.returncode, 0)
                    self.assertFalse(output.exists())

    def test_joint_passive_diagnostic_renders_two_values_without_rebuild(
            self) -> None:
        module = load_adapter()
        with tempfile.TemporaryDirectory(
                prefix="hwa viola joint jobs ") as text:
            root = Path(text)
            viola = fake_viola_tree(root)
            compiler, csound, csound_library = fake_tools(root)
            includes = fake_includes(root)
            recordings = []
            for index in range(3):
                recording = root / "tail-{}.wav".format(index)
                pcm24_wave(recording, frames=256 + index, channels=1)
                recordings.append(recording)
            roster = root / "roster.json"
            write_roster(roster, "g3", [
                ("iowa2012-g3-pizz", "iowa-2012", "fit",
                 recordings[0], 195.5),
                ("rwc-v1-g3-pizz", "rwc-variation-1", "fit",
                 recordings[1], 195.6),
                ("best-music-tools-g3-a442",
                 "best-music-tools-a442", "check",
                 recordings[2], 196.9),
            ])
            bundle = root / "bundle"
            build_bundle(module, types.SimpleNamespace(
                output_dir=bundle, viola_root=viola, csound=csound,
                csound_library=csound_library, c_compiler=compiler,
                python=PYTHON,
                csound_include_dir=[includes], roster=roster,
                joint_passive_diagnostic=True, target=None,
                reference_fit=None, reference_check=None,
                reference_c3_fit=None, reference_c3_check=None,
                fit_frequency_hz=None, check_frequency_hz=None,
                sample_count=None,
            ))
            count_path = compiler.with_suffix(".count")
            self.assertEqual(count_path.read_text(encoding="ascii"), "1")
            relocated = root / "analyzer-renderer-copy"
            relocated.mkdir()
            relocated_renderer = relocated / "renderer"
            relocated_renderer.write_bytes(
                (bundle / "renderer").read_bytes()
            )
            relocated_renderer.chmod(0o700)

            rendered = []
            for index, (tau, scale) in enumerate(((0.25, 1.0), (0.70, 2.0))):
                job = root / "job-{}".format(index)
                job.mkdir()
                output = job / "model.wav"
                request = joint_render_request(
                    recordings[0], output, tau, scale,
                    "iowa2012-g3-pizz", "fit", "g3",
                )
                request_path = job / "request.json"
                request_path.write_text(json.dumps(request), encoding="utf-8")
                completed = subprocess.run(
                    [str(relocated_renderer), "--hwa-experiment-job",
                     str(request_path), "--output-dir", str(job)],
                    check=False, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE, text=True, env={}, cwd=job,
                )
                self.assertEqual(completed.returncode, 0, completed.stderr)
                with wave.open(str(output), "rb") as stream:
                    self.assertEqual(
                        (stream.getframerate(), stream.getnchannels(),
                         stream.getsampwidth(), stream.getnframes()),
                        (44100, 2, 3, 705600),
                    )
                    first = stream.readframes(4)
                values = tuple(
                    int.from_bytes(first[offset:offset + 3], "little",
                                   signed=True)
                    for offset in range(0, 24, 6)
                )
                self.assertEqual(values, (
                    round(tau * 100000), round(scale * 100000),
                    round(195.5 * 10000),
                    round((440.0 * 195.5 / 195.99771799087463) * 10000),
                ))
                rendered.append(output.read_bytes())
            self.assertNotEqual(rendered[0], rendered[1])
            self.assertEqual(count_path.read_text(encoding="ascii"), "1")

            bad_facts = (
                (4.0, 0.5, "nut-pole"),
                (4.0, 1.0, "nut-gain"),
                (4.0, 2.0, "bridge-pole"),
                (4.0, 4.0, "bridge-gain"),
                (5.0, 4.0, "extra-field"),
            )
            for tau, scale, name in bad_facts:
                with self.subTest(bad_joint_fact=name):
                    wrong_job = root / ("wrong-facts-" + name)
                    wrong_job.mkdir()
                    wrong_output = wrong_job / "model.wav"
                    wrong_request = joint_render_request(
                        recordings[0], wrong_output, tau, scale,
                        "iowa2012-g3-pizz", "fit", "g3",
                    )
                    wrong_request_path = wrong_job / "request.json"
                    wrong_request_path.write_text(
                        json.dumps(wrong_request), encoding="utf-8"
                    )
                    wrong = subprocess.run(
                        [str(relocated_renderer),
                         "--hwa-experiment-job", str(wrong_request_path),
                         "--output-dir", str(wrong_job)],
                        check=False, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, text=True, env={},
                        cwd=wrong_job,
                    )
                    self.assertNotEqual(wrong.returncode, 0)
                    self.assertIn(
                        "joint diagnostic facts do not match", wrong.stderr
                    )
                    self.assertFalse(wrong_output.exists())

            copied_reference = root / "copied-tail.wav"
            copied_reference.write_bytes(recordings[0].read_bytes())
            copied_job = root / "copied-reference"
            copied_job.mkdir()
            copied_output = copied_job / "model.wav"
            copied_request = joint_render_request(
                copied_reference, copied_output, 0.25, 1.0,
                "iowa2012-g3-pizz", "fit", "g3",
            )
            copied_request_path = copied_job / "request.json"
            copied_request_path.write_text(
                json.dumps(copied_request), encoding="utf-8"
            )
            copied = subprocess.run(
                [str(relocated_renderer), "--hwa-experiment-job",
                 str(copied_request_path), "--output-dir", str(copied_job)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={}, cwd=copied_job,
            )
            self.assertNotEqual(copied.returncode, 0)
            self.assertIn("reference input has wrong path", copied.stderr)
            self.assertFalse(copied_output.exists())

    def test_joint_passive_diagnostic_builds_one_fixed_render_only_grid(
            self) -> None:
        module = load_adapter()
        with tempfile.TemporaryDirectory(
                prefix="hwa viola joint diagnostic ") as text:
            root = Path(text)
            viola = fake_viola_tree(root)
            compiler, csound, csound_library = fake_tools(root)
            includes = fake_includes(root)
            recordings = []
            for index in range(3):
                recording = root / "tail-{}.wav".format(index)
                pcm24_wave(recording, frames=256 + index, channels=1)
                recordings.append(recording)
            roster = root / "roster.json"
            write_roster(roster, "g3", [
                ("iowa2012-g3-pizz", "iowa-2012", "fit",
                 recordings[0], 195.5),
                ("rwc-v1-g3-pizz", "rwc-variation-1", "fit",
                 recordings[1], 195.6),
                ("best-music-tools-g3-a442",
                 "best-music-tools-a442", "check",
                 recordings[2], 196.9),
            ])
            bundle = root / "bundle"
            build_bundle(module, types.SimpleNamespace(
                output_dir=bundle,
                viola_root=viola,
                csound=csound,
                csound_library=csound_library,
                c_compiler=compiler,
                python=PYTHON,
                csound_include_dir=[includes],
                roster=roster,
                joint_passive_diagnostic=True,
                target=None,
                reference_fit=None,
                reference_check=None,
                reference_c3_fit=None,
                reference_c3_check=None,
                fit_frequency_hz=None,
                check_frequency_hz=None,
                sample_count=None,
            ))

            suffix = ".dylib" if sys.platform == "darwin" else ".so"
            self.assertEqual(
                sorted(path.name for path in bundle.iterdir()),
                ["bindings.json", "experiment.json",
                 "hlolli_wg_viola_joint_test" + suffix,
                 "receipt.json", "renderer", "roster.json"],
            )
            experiment = json.loads(
                (bundle / "experiment.json").read_text(encoding="utf-8")
            )
            self.assertEqual(experiment["parameters"], [
                {
                    "id": "loss_time_constant_g_seconds",
                    "unit": "seconds",
                    "minimum": 0.02,
                    "maximum": 5.0,
                    "baseline": 0.25,
                    "levels": [
                        0.10, 0.15, 0.25, 0.35, 0.45, 0.55,
                        0.70, 0.85, 1.00, 1.15, 1.30, 1.45, 1.60,
                        1.75, 1.90, 2.05, 2.20, 2.50, 3.00, 4.00,
                        5.00,
                    ],
                },
                {
                    "id": "nut_bridge_cutoff_scale",
                    "unit": "ratio",
                    "minimum": 0.5,
                    "maximum": 4.0,
                    "baseline": 1.0,
                    "levels": [0.5, 1.0, 2.0, 4.0],
                },
            ])
            self.assertEqual(experiment["plan"], {
                "kind": "grid", "seed": 29042016,
                "sample_count": 0, "replicates": 1,
            })
            receipt = json.loads(
                (bundle / "receipt.json").read_text(encoding="utf-8")
            )
            module_path = bundle / ("hlolli_wg_viola_joint_test" + suffix)
            expected_module_path = str(module_path.resolve())
            self.assertEqual(
                receipt["schema"],
                "hwa-viola-passive-joint-diagnostic-bundle",
            )
            self.assertEqual(
                (receipt["mode"], receipt["point_count"],
                 receipt["job_count"], receipt["render_frames"]),
                ("joint-passive-diagnostic-roster", 84, 252, 705600),
            )
            self.assertEqual(receipt["build_output_dir"],
                             str(bundle.resolve()))
            self.assertEqual(receipt["published_output_dir"],
                             str(bundle.resolve()))
            self.assertEqual(receipt["diagnostic_module_name"],
                             module_path.name)
            self.assertEqual(receipt["diagnostic_module_path"],
                             expected_module_path)
            receipt_module = next(
                row for row in receipt["resources"]
                if row["id"] == "diagnostic_module"
            )
            self.assertEqual(receipt_module["path"], expected_module_path)
            described = subprocess.run(
                [str(bundle / "renderer"), "--describe"],
                check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, env={}, cwd=root,
            )
            self.assertEqual(described.returncode, 0, described.stderr)
            description = json.loads(described.stdout)
            self.assertEqual(
                (description["mode"], description["adapter_id"],
                 description["permissions"]),
                (
                    "joint-passive-diagnostic-roster",
                    "hlolli-wg-viola-passive-g3-joint-diagnostic-v1",
                    {"render": True, "validate_profile": False,
                     "write_profile": False},
                ),
            )
            described_module = next(
                row for row in description["resources"]
                if row["id"] == "diagnostic_module"
            )
            self.assertEqual(described_module["path"], expected_module_path)
            profile_check = subprocess.run(
                [str(bundle / "renderer"), "--validate-profile",
                 str(viola / "model" / "viola-v1.json")],
                check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, env={}, cwd=root,
            )
            self.assertNotEqual(profile_check.returncode, 0)
            self.assertIn("no profile interface", profile_check.stderr)

            module_bytes = module_path.read_bytes()
            module_path.write_bytes(b"changed diagnostic module\n")
            changed = subprocess.run(
                [str(bundle / "renderer"), "--describe"],
                check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, env={}, cwd=root,
            )
            self.assertNotEqual(changed.returncode, 0)
            self.assertIn("configured resource changed: diagnostic_module",
                          changed.stderr)
            module_path.write_bytes(module_bytes)

            csd = viola / "tests" / "fit_passive_joint.csd"
            csd_bytes = csd.read_bytes()
            csd.write_bytes(b"changed joint probe\n")
            changed = subprocess.run(
                [str(bundle / "renderer"), "--describe"],
                check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, env={}, cwd=root,
            )
            self.assertNotEqual(changed.returncode, 0)
            self.assertIn("configured resource changed: csd", changed.stderr)
            csd.write_bytes(csd_bytes)

    def test_joint_passive_diagnostic_survives_outer_atomic_publish(
            self) -> None:
        module = load_adapter()
        with tempfile.TemporaryDirectory(
                prefix="hwa viola joint outer publish ") as text:
            root = Path(text)
            viola = fake_viola_tree(root)
            compiler, csound, csound_library = fake_tools(root)
            includes = fake_includes(root)
            recordings = []
            for index in range(3):
                recording = root / "tail-{}.wav".format(index)
                pcm24_wave(recording, frames=256 + index, channels=1)
                recordings.append(recording)
            roster = root / "roster.json"
            write_roster(roster, "g3", [
                ("iowa2012-g3-pizz", "iowa-2012", "fit",
                 recordings[0], 195.5),
                ("rwc-v1-g3-pizz", "rwc-variation-1", "fit",
                 recordings[1], 195.6),
                ("best-music-tools-g3-a442",
                 "best-music-tools-a442", "check",
                 recordings[2], 196.9),
            ])
            staging_root = root / ".r2.prepare"
            (staging_root / "bundles").mkdir(parents=True)
            published_root = root / "r2"
            staging_bundle = staging_root / "bundles" / "g3"
            published_bundle = published_root / "bundles" / "g3"
            published_bundle_contract = published_bundle.resolve()
            build_bundle(module, types.SimpleNamespace(
                output_dir=staging_bundle,
                published_output_dir=published_bundle_contract,
                viola_root=viola, csound=csound,
                csound_library=csound_library, c_compiler=compiler,
                python=PYTHON,
                csound_include_dir=[includes], roster=roster,
                joint_passive_diagnostic=True, target=None,
                reference_fit=None, reference_check=None,
                reference_c3_fit=None, reference_c3_check=None,
                fit_frequency_hz=None, check_frequency_hz=None,
                sample_count=None,
            ))
            receipt = json.loads(
                (staging_bundle / "receipt.json").read_text(encoding="utf-8")
            )
            suffix = ".dylib" if sys.platform == "darwin" else ".so"
            expected_module = published_bundle_contract / (
                "hlolli_wg_viola_joint_test" + suffix
            )
            staged_module = staging_bundle / expected_module.name
            self.assertEqual(receipt["build_output_dir"],
                             str(staging_bundle.resolve()))
            self.assertEqual(receipt["published_output_dir"],
                             str(published_bundle_contract))
            self.assertEqual(receipt["diagnostic_module_name"],
                             expected_module.name)
            self.assertEqual(receipt["diagnostic_module_path"],
                             str(expected_module))
            module_resource = next(
                row for row in receipt["resources"]
                if row["id"] == "diagnostic_module"
            )
            self.assertEqual(module_resource, {
                "id": "diagnostic_module",
                "path": str(expected_module),
                "sha256": file_hash(staged_module),
            })

            staging_root.rename(published_root)
            self.assertFalse(staging_root.exists())
            self.assertTrue(expected_module.is_file())
            relocated = root / "analyzer-copy"
            relocated.mkdir()
            renderer = relocated / "renderer"
            renderer.write_bytes(
                (published_bundle / "renderer").read_bytes()
            )
            renderer.chmod(0o700)
            described = subprocess.run(
                [str(renderer), "--describe"], check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, env={}, cwd=root,
            )
            self.assertEqual(described.returncode, 0, described.stderr)

            job = root / "job"
            job.mkdir()
            output = job / "model.wav"
            request = joint_render_request(
                recordings[0], output, 0.25, 1.0,
                "iowa2012-g3-pizz", "fit", "g3",
            )
            request_path = job / "request.json"
            request_path.write_text(json.dumps(request), encoding="utf-8")
            completed = subprocess.run(
                [str(renderer), "--hwa-experiment-job", str(request_path),
                 "--output-dir", str(job)], check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, env={}, cwd=job,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            with wave.open(str(output), "rb") as stream:
                self.assertEqual(stream.getnframes(), 705600)

            race_root = root / ".race.prepare"
            (race_root / "bundles").mkdir(parents=True)
            race_bundle = race_root / "bundles" / "g3"
            race_published = (root / "race-r2" / "bundles" / "g3").resolve()
            original_build_renderer = module.build_renderer
            race_identity = None

            def inject_build_output(config, renderer_path) -> None:
                nonlocal race_identity
                original_build_renderer(config, renderer_path)
                race_bundle.mkdir()
                facts = race_bundle.stat()
                race_identity = (facts.st_dev, facts.st_ino)

            with mock.patch.object(
                    module, "build_renderer",
                    side_effect=inject_build_output):
                with self.assertRaisesRegex(
                        module.AdapterError,
                        "build output directory changed while building"):
                    build_bundle(module, types.SimpleNamespace(
                        output_dir=race_bundle,
                        published_output_dir=race_published,
                        viola_root=viola, csound=csound,
                        csound_library=csound_library,
                        c_compiler=compiler,
                        python=PYTHON,
                        csound_include_dir=[includes], roster=roster,
                        joint_passive_diagnostic=True, target=None,
                        reference_fit=None, reference_check=None,
                        reference_c3_fit=None, reference_c3_check=None,
                        fit_frequency_hz=None, check_frequency_hz=None,
                        sample_count=None,
                    ))
            self.assertIsNotNone(race_identity)
            facts = race_bundle.stat()
            self.assertEqual((facts.st_dev, facts.st_ino), race_identity)
            self.assertEqual(list(race_bundle.iterdir()), [])

    def test_g3_selection_uses_one_best_music_tools_check_tail(self) -> None:
        module = load_adapter()
        with tempfile.TemporaryDirectory(prefix="hwa viola g fallback ") as text:
            root = Path(text)
            recordings = []
            for index in range(7):
                recording = root / "tail-{}.wav".format(index)
                pcm24_wave(recording, frames=256 + index, channels=1)
                recordings.append(recording)
            fit_rows = [
                ("iowa2012-g3-pizz", "iowa-2012", "fit",
                 recordings[0], 195.5),
                ("rwc-v1-g3-pizz", "rwc-variation-1", "fit",
                 recordings[1], 195.6),
            ]
            valid = root / "valid.json"
            write_roster(valid, "g3", fit_rows + [(
                "best-music-tools-g3-a442", "best-music-tools-a442",
                "check", recordings[2], 196.9,
            )])
            loaded = module.load_selection_roster(valid)
            self.assertEqual(
                [(row["source_family"], row["split"], row["weight"])
                 for row in loaded["cases"]],
                [("best-music-tools-a442", "check", 1.0),
                 ("iowa-2012", "fit", 0.5),
                 ("rwc-variation-1", "fit", 0.5)],
            )
            experiment = module.build_roster_experiment(loaded)
            self.assertEqual(experiment["plan"], {
                "kind": "grid", "seed": 29042016,
                "sample_count": 0, "replicates": 1,
            })
            self.assertEqual(len(experiment["parameters"][0]["levels"]), 21)

            rwc2 = root / "rwc2.json"
            write_roster(rwc2, "g3", fit_rows + [(
                "rwc-v2-g3-pizz", "rwc-variation-2", "check",
                recordings[3], 195.7,
            )])
            with self.assertRaisesRegex(module.AdapterError,
                                        "check source family"):
                module.load_selection_roster(rwc2)

            unknown = root / "unknown.json"
            write_roster(unknown, "g3", fit_rows + [(
                "unknown-g3-pizz", "unknown", "check",
                recordings[4], 195.8,
            )])
            with self.assertRaisesRegex(module.AdapterError,
                                        "unknown source family"):
                module.load_selection_roster(unknown)

            multiple = root / "multiple.json"
            write_roster(multiple, "g3", fit_rows + [
                ("best-music-tools-g3-a", "best-music-tools-a442", "check",
                 recordings[2], 196.9),
                ("best-music-tools-g3-b", "best-music-tools-a442", "check",
                 recordings[3], 197.0),
            ])
            with self.assertRaisesRegex(module.AdapterError, "exactly one"):
                module.load_selection_roster(multiple)

            other_targets = {
                "c3": (3, 130.5),
                "d4": (2, 293.5),
                "a4": (2, 440.0),
            }
            for target, (rwc1_count, frequency_hz) in other_targets.items():
                rows = [(
                    "iowa2012-{}-pizz".format(target),
                    "iowa-2012", "fit", recordings[0], frequency_hz,
                )]
                for index in range(rwc1_count):
                    rows.append((
                        "rwc-v1-{}-pizz-{}".format(target, index),
                        "rwc-variation-1", "fit", recordings[index + 1],
                        frequency_hz + (index + 1) / 100.0,
                    ))
                rows.append((
                    "best-music-tools-{}".format(target),
                    "best-music-tools-a442", "check",
                    recordings[rwc1_count + 1], frequency_hz + 0.04,
                ))
                roster = root / "wrong-check-{}.json".format(target)
                write_roster(roster, target, rows)
                with self.subTest(target=target):
                    with self.assertRaisesRegex(module.AdapterError,
                                                "check source family"):
                        module.load_selection_roster(roster)

    def test_control_json_hash_names_the_exact_parsed_bytes(self) -> None:
        module = load_adapter()
        with tempfile.TemporaryDirectory(prefix="hwa viola json bytes ") as text:
            path = Path(text) / "control.json"
            path.write_text('{"state":"disk"}', encoding="utf-8")
            parsed_bytes = b'{"state":"parsed"}'
            with mock.patch.object(
                    module, "read_bounded_json", return_value=parsed_bytes):
                value, source_hash = module.load_json_evidence(path)
            self.assertEqual(value, {"state": "parsed"})
            self.assertEqual(
                source_hash, hashlib.sha256(parsed_bytes).hexdigest()
            )

    def test_selection_roster_renderer_handles_every_frozen_case(self) -> None:
        module = load_adapter()
        with tempfile.TemporaryDirectory(
                prefix="hwa viola roster renders ") as text:
            root = Path(text)
            viola = fake_viola_tree(root)
            compiler, csound, csound_library = fake_tools(root)
            includes = fake_includes(root)
            recordings = []
            for index in range(6):
                recording = root / "tail-{}.wav".format(index)
                (pcm16_wave if index % 2 == 0 else pcm24_wave)(
                    recording, frames=256 + index, channels=1 + index % 2
                )
                recordings.append(recording)
            roster = root / "roster.json"
            roster_value = write_roster(roster, "d4", [
                ("iowa2012-d4-pizz", "iowa-2012", "fit",
                 recordings[0], 292.70),
                ("rwc-v1-d4-pizz-a", "rwc-variation-1", "fit",
                 recordings[1], 292.90),
                ("rwc-v1-d4-pizz-b", "rwc-variation-1", "fit",
                 recordings[2], 293.10),
                ("rwc-v2-d4-pizz-a", "rwc-variation-2", "check",
                 recordings[3], 293.30),
                ("rwc-v2-d4-pizz-b", "rwc-variation-2", "check",
                 recordings[4], 293.50),
                ("rwc-v2-d4-pizz-c", "rwc-variation-2", "check",
                 recordings[5], 293.70),
            ])
            bundle = root / "bundle"
            build_bundle(module, types.SimpleNamespace(
                output_dir=bundle,
                viola_root=viola,
                csound=csound,
                csound_library=csound_library,
                c_compiler=compiler,
                python=PYTHON,
                csound_include_dir=[includes],
                roster=roster,
                target=None,
                reference_fit=None,
                reference_check=None,
                reference_c3_fit=None,
                reference_c3_check=None,
                fit_frequency_hz=None,
                check_frequency_hz=None,
                sample_count=None,
            ))
            bindings = {
                row["id"]: row for row in json.loads(
                    (bundle / "bindings.json").read_text(encoding="utf-8")
                )["bindings"]
            }
            receipt = json.loads(
                (bundle / "receipt.json").read_text(encoding="utf-8")
            )
            rows = {row["id"]: row for row in roster_value["cases"]}
            for index, (case_id, row) in enumerate(sorted(rows.items())):
                with self.subTest(case_id=case_id):
                    job = root / "job-{}".format(index)
                    job.mkdir()
                    output = job / "model.wav"
                    value = 0.4 + index / 10.0
                    request = render_request(
                        Path(row["path"]), output, value=value,
                        case_id=case_id, split=row["split"],
                        reference_channels=1 + recordings.index(
                            Path(row["path"])) % 2,
                        target="d4", binding_id=case_id,
                    )
                    request["inputs"][0]["sha256"] = bindings[case_id]["sha256"]
                    request_path = job / "request.json"
                    request_path.write_text(json.dumps(request), encoding="utf-8")
                    completed = subprocess.run(
                        [str(bundle / "renderer"), "--hwa-experiment-job",
                         str(request_path), "--output-dir", str(job)],
                        check=False, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, text=True, env={}, cwd=job,
                    )
                    self.assertEqual(completed.returncode, 0, completed.stderr)
                    with wave.open(str(output), "rb") as stream:
                        self.assertEqual(
                            (stream.getframerate(), stream.getnchannels(),
                             stream.getsampwidth(), stream.getnframes()),
                            (44100, 2, 3, 176416),
                        )
                        rendered = stream.readframes(3)
                    case_render = receipt["render"][case_id]
                    self.assertEqual(
                        int.from_bytes(rendered[:3], "little", signed=True),
                        round(value * 100000),
                    )
                    self.assertEqual(
                        int.from_bytes(rendered[6:9], "little", signed=True),
                        round(case_render["frequency_hz"] * 10000),
                    )
                    self.assertEqual(
                        int.from_bytes(rendered[12:15], "little", signed=True),
                        round(case_render["a4_hz"] * 10000),
                    )

            case_ids = sorted(rows)
            bad_job = root / "wrong-case-input"
            bad_job.mkdir()
            output = bad_job / "model.wav"
            first = rows[case_ids[0]]
            second = rows[case_ids[1]]
            request = render_request(
                Path(second["path"]), output, case_id=case_ids[0],
                split=first["split"], target="d4",
                reference_channels=2, binding_id=case_ids[1],
            )
            request_path = bad_job / "request.json"
            request_path.write_text(json.dumps(request), encoding="utf-8")
            rejected = subprocess.run(
                [str(bundle / "renderer"), "--hwa-experiment-job",
                 str(request_path), "--output-dir", str(bad_job)],
                check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, env={}, cwd=bad_job,
            )
            self.assertNotEqual(rejected.returncode, 0)
            self.assertFalse(output.exists())

            copied_job = root / "copied-case-input"
            copied_job.mkdir()
            output = copied_job / "model.wav"
            copied_reference = root / "same-bytes-other-path.wav"
            copied_reference.write_bytes(Path(first["path"]).read_bytes())
            request = render_request(
                copied_reference, output, case_id=case_ids[0],
                split=first["split"], target="d4",
                reference_channels=1, binding_id=case_ids[0],
            )
            request_path = copied_job / "request.json"
            request_path.write_text(json.dumps(request), encoding="utf-8")
            rejected = subprocess.run(
                [str(bundle / "renderer"), "--hwa-experiment-job",
                 str(request_path), "--output-dir", str(copied_job)],
                check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, env={}, cwd=copied_job,
            )
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("wrong path", rejected.stderr)
            self.assertFalse(output.exists())

    def test_selection_roster_rejects_malformed_rows_and_json(self) -> None:
        module = load_adapter()
        with tempfile.TemporaryDirectory(prefix="hwa viola malformed ") as text:
            root = Path(text)
            recordings = []
            for index in range(4):
                recording = root / "tail-{}.wav".format(index)
                pcm24_wave(recording, frames=256 + index, channels=1)
                recordings.append(recording)
            base_path = root / "base.json"
            base = write_roster(base_path, "g3", [
                ("iowa2012-g3-pizz", "iowa-2012", "fit",
                 recordings[0], 195.5),
                ("rwc-v1-g3-pizz", "rwc-variation-1", "fit",
                 recordings[1], 195.6),
                ("best-music-tools-g3-a442", "best-music-tools-a442",
                 "check", recordings[2], 196.9),
            ])
            variants = []

            def changed() -> dict:
                return json.loads(json.dumps(base))

            value = changed()
            value["other"] = 1
            variants.append(value)
            value = changed()
            value["cases"][0]["other"] = 1
            variants.append(value)
            value = changed()
            value["cases"][0].pop("frequency_hz")
            variants.append(value)
            value = changed()
            value["cases"][0]["source_family"] = "other"
            variants.append(value)
            value = changed()
            value["cases"][2]["split"] = "fit"
            variants.append(value)
            value = changed()
            value["cases"][1]["id"] = value["cases"][0]["id"]
            variants.append(value)
            value = changed()
            value["cases"][2]["path"] = value["cases"][0]["path"]
            value["cases"][2]["sha256"] = value["cases"][0]["sha256"]
            variants.append(value)
            value = changed()
            value["cases"][0]["path"] = "relative.wav"
            variants.append(value)
            value = changed()
            value["cases"][0]["sha256"] = "0" * 64
            variants.append(value)
            value = changed()
            value["cases"][0]["frequency_hz"] = True
            variants.append(value)
            value = changed()
            value["cases"][0]["id"] = "a" * 101
            variants.append(value)
            for index, value in enumerate(variants):
                with self.subTest(variant=index):
                    path = root / "bad-{}.json".format(index)
                    path.write_text(json.dumps(value), encoding="utf-8")
                    with self.assertRaises(module.AdapterError):
                        module.load_selection_roster(path)

            duplicate_key = root / "duplicate-key.json"
            source = json.dumps(base)
            source = source.replace(
                '"target": "g3"',
                '"target": "g3", "target": "g3"',
                1,
            )
            duplicate_key.write_text(source, encoding="utf-8")
            with self.assertRaisesRegex(module.AdapterError,
                                        "duplicate JSON key"):
                module.load_selection_roster(duplicate_key)

            oversized = root / "oversized.json"
            oversized.write_text(" " * (module.MAX_JSON_BYTES + 1),
                                  encoding="ascii")
            with self.assertRaisesRegex(module.AdapterError, "byte limit"):
                module.load_selection_roster(oversized)
            with self.assertRaisesRegex(module.AdapterError, "must be absolute"):
                module.load_selection_roster(Path("relative-roster.json"))

            swapped = json.loads(json.dumps(base))
            swapped["cases"][0]["frequency_hz"] = 195.55
            swapped_bytes = json.dumps(swapped).encode("utf-8")
            with mock.patch.object(
                    module, "read_bounded_json", return_value=swapped_bytes):
                with self.assertRaisesRegex(
                        module.AdapterError, "changed while reading"):
                    module.load_selection_roster(base_path)

    def test_selection_roster_cannot_mix_single_case_options(self) -> None:
        module = load_adapter()
        with tempfile.TemporaryDirectory(prefix="hwa viola modes ") as text:
            root = Path(text)
            viola = fake_viola_tree(root)
            base = {
                "output_dir": root / "bundle",
                "viola_root": viola,
                "roster": root / "roster.json",
                "target": None,
                "reference_fit": None,
                "reference_check": None,
                "reference_c3_fit": None,
                "reference_c3_check": None,
                "fit_frequency_hz": None,
                "check_frequency_hz": None,
                "sample_count": None,
            }
            variants = {
                "target": "g3",
                "reference_fit": root / "fit.wav",
                "reference_check": root / "check.wav",
                "reference_c3_fit": root / "legacy-fit.wav",
                "reference_c3_check": root / "legacy-check.wav",
                "fit_frequency_hz": 195.5,
                "check_frequency_hz": 195.6,
            }
            for name, value in variants.items():
                with self.subTest(option=name):
                    arguments = dict(base)
                    arguments[name] = value
                    with self.assertRaisesRegex(
                            module.AdapterError, "cannot be mixed"):
                        module.build(types.SimpleNamespace(**arguments))
            arguments = dict(base)
            arguments["sample_count"] = 1
            with self.assertRaisesRegex(
                    module.AdapterError,
                    "--sample-count cannot be used with --roster"):
                module.build(types.SimpleNamespace(**arguments))

    def test_selection_roster_and_audio_are_rechecked_before_publish(self) -> None:
        for mutation in ("roster", "audio"):
            with self.subTest(mutation=mutation):
                module = load_adapter()
                with tempfile.TemporaryDirectory(
                        prefix="hwa viola roster race ") as text:
                    root = Path(text)
                    viola = fake_viola_tree(root)
                    compiler, csound, csound_library = fake_tools(root)
                    includes = fake_includes(root)
                    recordings = []
                    for index in range(3):
                        recording = root / "tail-{}.wav".format(index)
                        pcm24_wave(recording, frames=256 + index, channels=1)
                        recordings.append(recording)
                    roster = root / "roster.json"
                    write_roster(roster, "g3", [
                        ("iowa2012-g3-pizz", "iowa-2012", "fit",
                         recordings[0], 195.5),
                        ("rwc-v1-g3-pizz", "rwc-variation-1", "fit",
                         recordings[1], 195.6),
                        ("best-music-tools-g3-a442",
                         "best-music-tools-a442", "check",
                         recordings[2], 196.9),
                    ])
                    output = root / "bundle"
                    original_build_renderer = module.build_renderer

                    def mutate_after_freeze(config, renderer) -> None:
                        original_build_renderer(config, renderer)
                        if mutation == "roster":
                            roster.write_text(
                                roster.read_text(encoding="utf-8") + " ",
                                encoding="utf-8",
                            )
                        else:
                            with recordings[0].open("ab") as stream:
                                stream.write(b"x")

                    message = ("roster changed before publish"
                               if mutation == "roster" else
                               "reference changed while building")
                    with mock.patch.object(
                            module, "build_renderer",
                            side_effect=mutate_after_freeze):
                        with self.assertRaisesRegex(
                                module.AdapterError, message):
                            build_bundle(module, types.SimpleNamespace(
                                output_dir=output,
                                viola_root=viola,
                                csound=csound,
                                csound_library=csound_library,
                                c_compiler=compiler,
                                python=PYTHON,
                                csound_include_dir=[includes],
                                roster=roster,
                                target=None,
                                reference_fit=None,
                                reference_check=None,
                                reference_c3_fit=None,
                                reference_c3_check=None,
                                fit_frequency_hz=None,
                                check_frequency_hz=None,
                                sample_count=None,
                            ))
                    self.assertFalse(output.exists())

    def test_selection_roster_enforces_the_accepted_tail_counts(self) -> None:
        module = load_adapter()
        with tempfile.TemporaryDirectory(prefix="hwa viola counts ") as text:
            root = Path(text)
            recordings = []
            for index in range(8):
                recording = root / "tail-{}.wav".format(index)
                pcm24_wave(recording, frames=256 + index, channels=1)
                recordings.append(recording)
            valid_g3 = [
                ("iowa2012-g3-pizz", "iowa-2012", "fit",
                 recordings[0], 195.5),
                ("rwc-v1-g3-pizz", "rwc-variation-1", "fit",
                 recordings[1], 195.6),
                ("best-music-tools-g3-a442", "best-music-tools-a442",
                 "check", recordings[2], 196.9),
            ]
            variants = []
            extra_iowa = list(valid_g3)
            extra_iowa.insert(1, (
                "iowa2012-g3-pizz-extra", "iowa-2012", "fit",
                recordings[3], 195.55,
            ))
            variants.append(("fit-tail count", "g3", extra_iowa))
            short_c3 = [
                ("iowa2012-c3-pizz", "iowa-2012", "fit",
                 recordings[0], 130.5),
                ("rwc-v1-c3-pizz-a", "rwc-variation-1", "fit",
                 recordings[1], 130.6),
                ("rwc-v1-c3-pizz-b", "rwc-variation-1", "fit",
                 recordings[2], 130.7),
                ("rwc-v2-c3-pizz", "rwc-variation-2", "check",
                 recordings[3], 130.8),
            ]
            variants.append(("fit-tail count", "c3", short_c3))
            too_many_checks = [
                ("iowa2012-d4-pizz", "iowa-2012", "fit",
                 recordings[0], 293.5),
                ("rwc-v1-d4-pizz-a", "rwc-variation-1", "fit",
                 recordings[1], 293.6),
                ("rwc-v1-d4-pizz-b", "rwc-variation-1", "fit",
                 recordings[2], 293.7),
            ]
            for index in range(3, 7):
                too_many_checks.append((
                    "rwc-v2-d4-pizz-{}".format(index),
                    "rwc-variation-2", "check", recordings[index],
                    293.7 + index / 100.0,
                ))
            variants.append(("1..3", "d4", too_many_checks))
            for index, (message, target, rows) in enumerate(variants):
                with self.subTest(target=target, message=message):
                    roster = root / "roster-{}.json".format(index)
                    write_roster(roster, target, rows)
                    with self.assertRaisesRegex(module.AdapterError, message):
                        module.load_selection_roster(roster)
            nominal = {
                "c3": 130.8, "g3": 196.0, "d4": 293.7, "a4": 440.0,
            }
            rwc1_counts = {"c3": 3, "g3": 1, "d4": 2, "a4": 2}
            for target, rwc1_count in rwc1_counts.items():
                with self.subTest(target=target, state="accepted"):
                    rows = [
                        ("iowa2012-{}-pizz".format(target),
                         "iowa-2012", "fit", recordings[0], nominal[target]),
                    ]
                    for index in range(rwc1_count):
                        rows.append((
                            "rwc-v1-{}-pizz-{}".format(target, index),
                            "rwc-variation-1", "fit", recordings[index + 1],
                            nominal[target] + (index + 1) / 100.0,
                        ))
                    check_family = ("best-music-tools-a442"
                                    if target == "g3" else
                                    "rwc-variation-2")
                    check_id = ("best-music-tools-g3-a442"
                                if target == "g3" else
                                "rwc-v2-{}-pizz".format(target))
                    rows.append((
                        check_id, check_family, "check",
                        recordings[rwc1_count + 1],
                        nominal[target] + 0.04,
                    ))
                    roster = root / "accepted-{}.json".format(target)
                    write_roster(roster, target, rows)
                    loaded = module.load_selection_roster(roster)
                    self.assertEqual(len(loaded["cases"]), rwc1_count + 2)

    def test_selection_roster_and_audio_must_stay_outside_repositories(self) -> None:
        module = load_adapter()
        with tempfile.TemporaryDirectory(prefix="hwa viola boundary ") as text:
            root = Path(text)
            analyzer = root / "analyzer"
            private = root / "private"
            analyzer.mkdir()
            private.mkdir()
            viola = fake_viola_tree(root)
            private_recordings = []
            for index in range(3):
                recording = private / "tail-{}.wav".format(index)
                pcm24_wave(recording, frames=256 + index, channels=1)
                private_recordings.append(recording)
            analyzer_recording = analyzer / "tail.wav"
            pcm24_wave(analyzer_recording, frames=300, channels=1)
            viola_recording = viola / "tail.wav"
            pcm24_wave(viola_recording, frames=301, channels=1)
            rows = [
                ("iowa2012-c3-pizz", "iowa-2012", "fit",
                 private_recordings[0], 130.5),
                ("rwc-v1-c3-pizz", "rwc-variation-1", "fit",
                 private_recordings[1], 130.6),
                ("rwc-v2-c3-pizz", "rwc-variation-2", "check",
                 private_recordings[2], 130.7),
            ]
            variants = []
            inside_roster = analyzer / "roster.json"
            write_roster(inside_roster, "c3", rows)
            variants.append(("roster", inside_roster))
            analyzer_audio = private / "analyzer-audio.json"
            changed = list(rows)
            changed[0] = (*changed[0][:3], analyzer_recording, changed[0][4])
            write_roster(analyzer_audio, "c3", changed)
            variants.append(("recording", analyzer_audio))
            viola_audio = private / "viola-audio.json"
            changed = list(rows)
            changed[1] = (*changed[1][:3], viola_recording, changed[1][4])
            write_roster(viola_audio, "c3", changed)
            variants.append(("recording", viola_audio))
            valid_roster = private / "valid-g3.json"
            write_roster(valid_roster, "g3", [
                ("iowa2012-g3-pizz", "iowa-2012", "fit",
                 private_recordings[0], 195.5),
                ("rwc-v1-g3-pizz", "rwc-variation-1", "fit",
                 private_recordings[1], 195.6),
                ("best-music-tools-g3-a442", "best-music-tools-a442",
                 "check", private_recordings[2], 196.9),
            ])

            with mock.patch.object(module, "ROOT", analyzer):
                for index, (name, roster) in enumerate(variants):
                    with self.subTest(name=name, index=index):
                        with self.assertRaisesRegex(
                                module.AdapterError, "outside.*repositories"):
                            module.build(types.SimpleNamespace(
                                output_dir=root / "bundle-{}".format(index),
                                viola_root=viola,
                                roster=roster,
                                target=None,
                                reference_fit=None,
                                reference_check=None,
                                reference_c3_fit=None,
                                reference_c3_check=None,
                                fit_frequency_hz=None,
                                check_frequency_hz=None,
                                sample_count=None,
                            ))
                for index, output in enumerate((
                        analyzer / "bundle", viola / "bundle"), start=3):
                    with self.subTest(name="output", index=index):
                        with self.assertRaisesRegex(
                                module.AdapterError, "outside.*repositories"):
                            module.build(types.SimpleNamespace(
                                output_dir=output,
                                viola_root=viola,
                                roster=valid_roster,
                                target=None,
                                reference_fit=None,
                                reference_check=None,
                                reference_c3_fit=None,
                                reference_c3_check=None,
                                fit_frequency_hz=None,
                                check_frequency_hz=None,
                                sample_count=None,
                            ))

    def test_selection_roster_weights_source_families_before_tails(self) -> None:
        module = load_adapter()
        fit_spec = importlib.util.spec_from_file_location(
            "instrument_fit", FIT_TOOL
        )
        fit_tool = importlib.util.module_from_spec(fit_spec)
        fit_spec.loader.exec_module(fit_tool)
        with tempfile.TemporaryDirectory(prefix="hwa viola roster ") as text:
            root = Path(text)
            viola = fake_viola_tree(root)
            compiler, csound, csound_library = fake_tools(root)
            includes = fake_includes(root)
            recordings = []
            for index in range(6):
                recording = root / "tail-{}.wav".format(index)
                (pcm16_wave if index % 2 == 0 else pcm24_wave)(
                    recording, frames=256 + index, channels=1 + index % 2
                )
                recordings.append(recording)
            roster = root / "roster.json"
            roster_value = write_roster(roster, "c3", [
                ("iowa2012-c3-pizz", "iowa-2012", "fit",
                 recordings[0], 130.50),
                ("rwc-v1-c3-pizz-a", "rwc-variation-1", "fit",
                 recordings[1], 130.60),
                ("rwc-v1-c3-pizz-b", "rwc-variation-1", "fit",
                 recordings[2], 130.70),
                ("rwc-v1-c3-pizz-c", "rwc-variation-1", "fit",
                 recordings[3], 130.80),
                ("rwc-v2-c3-pizz-a", "rwc-variation-2", "check",
                 recordings[4], 130.90),
                ("rwc-v2-c3-pizz-b", "rwc-variation-2", "check",
                 recordings[5], 131.00),
            ])
            bundle = root / "bundle"
            build_bundle(module, types.SimpleNamespace(
                output_dir=bundle,
                viola_root=viola,
                csound=csound,
                csound_library=csound_library,
                c_compiler=compiler,
                python=PYTHON,
                csound_include_dir=[includes],
                roster=roster,
                target=None,
                reference_fit=None,
                reference_check=None,
                reference_c3_fit=None,
                reference_c3_check=None,
                fit_frequency_hz=None,
                check_frequency_hz=None,
                sample_count=None,
            ))

            self.assertEqual(
                sorted(path.name for path in bundle.iterdir()),
                ["bindings.json", "experiment.json", "fit.json",
                 "receipt.json", "renderer", "roster.json"],
            )
            experiment = json.loads(
                (bundle / "experiment.json").read_text(encoding="utf-8")
            )
            self.assertEqual(experiment["plan"], {
                "kind": "grid",
                "seed": 29042016,
                "sample_count": 0,
                "replicates": 1,
            })
            self.assertEqual(experiment["parameters"], [{
                "id": "loss_time_constant_c_seconds",
                "unit": "seconds",
                "minimum": 0.02,
                "maximum": 5.0,
                "baseline": 0.25,
                "levels": [
                    0.10, 0.15, 0.25, 0.35, 0.45, 0.55,
                    0.70, 0.85, 1.00, 1.15, 1.30, 1.45, 1.60, 1.75,
                    1.90, 2.05, 2.20, 2.50, 3.00, 4.00, 5.00,
                ],
            }])
            manifest = fit_tool.fit_manifest(bundle / "fit.json")
            bindings = json.loads(
                (bundle / "bindings.json").read_text(encoding="utf-8")
            )["bindings"]
            receipt = json.loads(
                (bundle / "receipt.json").read_text(encoding="utf-8")
            )
            frozen_roster = json.loads(
                (bundle / "roster.json").read_text(encoding="utf-8")
            )
            expected_weights = {
                "iowa2012-c3-pizz": 0.5,
                "rwc-v1-c3-pizz-a": 0.5 / 3.0,
                "rwc-v1-c3-pizz-b": 0.5 / 3.0,
                "rwc-v1-c3-pizz-c": 0.5 / 3.0,
                "rwc-v2-c3-pizz-a": 0.5,
                "rwc-v2-c3-pizz-b": 0.5,
            }
            self.assertEqual(experiment["inputs"], [
                {"id": row["id"], "sha256": row["sha256"]}
                for row in sorted(roster_value["cases"], key=lambda row: row["id"])
            ])
            self.assertEqual(
                {row["id"]: row["weight"] for row in experiment["cases"]},
                expected_weights,
            )
            self.assertEqual(
                {row["case"]: row["weight"]
                 for row in manifest["objectives"]},
                expected_weights,
            )
            self.assertEqual(
                {row["case"]: row["reference_binding"]
                 for row in manifest["objectives"]},
                {case_id: case_id for case_id in expected_weights},
            )
            self.assertEqual(
                [row["id"] for row in manifest["objectives"]],
                [case_id + "-passive-decay"
                 for case_id in sorted(expected_weights)],
            )
            self.assertEqual(manifest["selection"], {
                "check_weight": 0.5,
                "max_candidate_loss": 2.0,
                "max_candidate_worst_harm": 3.0,
                "max_check_loss_increase": 0.0,
                "maximum_candidate_t60_ratio": 2.0,
                "minimum_candidate_support_ratio": 0.5,
                "minimum_candidate_t60_ratio": 0.5,
            })
            self.assertEqual(
                {row["id"]: row["path"] for row in bindings},
                {row["id"]: str(Path(row["path"]).resolve())
                 for row in roster_value["cases"]},
            )
            expected_roster_cases = []
            for row in roster_value["cases"]:
                expected_row = dict(row)
                expected_row["path"] = str(Path(row["path"]).resolve())
                expected_roster_cases.append(expected_row)
            self.assertEqual(frozen_roster, {
                **roster_value,
                "cases": sorted(expected_roster_cases,
                                key=lambda row: row["id"]),
            })
            self.assertEqual(receipt["mode"], "selection-roster")
            self.assertEqual(receipt["target"], "c3")
            self.assertEqual(receipt["roster_source_sha256"], file_hash(roster))
            self.assertEqual(
                receipt["roster_sha256"], file_hash(bundle / "roster.json")
            )
            self.assertEqual(receipt["source_family_weights"], [
                {"source_family": "iowa-2012", "split": "fit",
                 "total_weight": 0.5, "case_count": 1},
                {"source_family": "rwc-variation-1", "split": "fit",
                 "total_weight": 0.5, "case_count": 3},
                {"source_family": "rwc-variation-2", "split": "check",
                 "total_weight": 1.0, "case_count": 2},
            ])
            self.assertEqual(
                {row["id"]: row["weight"] for row in receipt["references"]},
                expected_weights,
            )
            described = subprocess.run(
                [str(bundle / "renderer"), "--describe"],
                check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, env={}, cwd=root,
            )
            self.assertEqual(described.returncode, 0, described.stderr)
            description = json.loads(described.stdout)
            self.assertEqual(description["mode"], "selection-roster")
            self.assertEqual(set(description["render"]), set(expected_weights))

    def test_each_open_string_has_one_exact_passive_fit_contract(self) -> None:
        module = load_adapter()
        spec = importlib.util.spec_from_file_location("instrument_fit", FIT_TOOL)
        tool = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(tool)
        expected = {
            "c3": (0, 1, 130.8127826502993, 130.58241487372985,
                   "loss_time_constant_c_seconds"),
            "g3": (1, 2, 195.99771799087463, 195.58538558422308,
                   "loss_time_constant_g_seconds"),
            "d4": (2, 3, 293.6647679174076, 292.96845778349154,
                   "loss_time_constant_d_seconds"),
            "a4": (3, 4, 440.0, 441.08823856808476,
                   "loss_time_constant_a_seconds"),
        }

        self.assertEqual(set(module.STRING_TARGETS), set(expected))
        for target, facts in expected.items():
            with self.subTest(target=target):
                profile_index, render_string, nominal_hz, measured_hz, parameter_id = facts
                target_spec = module.STRING_TARGETS[target]
                manifest = tool.fit_manifest(FIT_MANIFESTS[target])
                module.validate_fit_manifest(manifest, target)
                self.assertEqual(target_spec["profile_index"], profile_index)
                self.assertEqual(target_spec["render_string"], render_string)
                self.assertEqual(target_spec["nominal_open_hz"], nominal_hz)
                self.assertEqual(target_spec["measured_reference_hz"], measured_hz)
                self.assertEqual(target_spec["render_a4"],
                                 440.0 * measured_hz / nominal_hz)
                self.assertEqual(manifest["parameters"][0]["id"], parameter_id)
                self.assertEqual(
                    manifest["parameters"][0]["profile_paths"],
                    [["strings", profile_index, "loss_time_constant_seconds"]],
                )
                self.assertEqual(
                    [(row["case"], row["reference_binding"], row["split"])
                     for row in manifest["objectives"]],
                    [(target + "-passive-fit", "reference_{}_fit".format(target),
                      "fit"),
                     (target + "-passive-check",
                      "reference_{}_check".format(target), "check")],
                )

    def test_each_target_accepts_pcm16_or_pcm24_mono_or_stereo(self) -> None:
        module = load_adapter()
        layouts = {
            "c3": ((2, 1), (3, 2)),
            "g3": ((2, 2), (3, 1)),
            "d4": ((3, 1), (2, 2)),
            "a4": ((3, 2), (2, 1)),
        }
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            for target, ((fit_width, fit_channels),
                         (check_width, check_channels)) in layouts.items():
                fit = root / (target + "-fit.wav")
                check = root / (target + "-check.wav")
                (pcm16_wave if fit_width == 2 else pcm24_wave)(
                    fit, channels=fit_channels
                )
                (pcm16_wave if check_width == 2 else pcm24_wave)(
                    check, frames=257, channels=check_channels
                )
                references = {
                    "reference_{}_fit".format(target): fit,
                    "reference_{}_check".format(target): check,
                }
                experiment = module.build_experiment(
                    references, sample_count=3, target=target
                )
                cases = {row["id"]: row for row in experiment["cases"]}
                self.assertEqual(
                    [row["id"] for row in experiment["parameters"]],
                    [module.STRING_TARGETS[target]["parameter_id"]],
                )
                self.assertEqual(
                    cases[target + "-passive-fit"]["stems"][1]["channels"],
                    fit_channels,
                )
                self.assertEqual(
                    cases[target + "-passive-check"]["stems"][1]["channels"],
                    check_channels,
                )

    def test_each_target_builds_and_renders_its_own_tuned_string(self) -> None:
        module = load_adapter()
        expected = {
            "c3": ("hlolli-wg-viola-passive-c-v1", 1,
                   130.58241487372985, 439.22513824997105,
                   130.8127826502993),
            "g3": ("hlolli-wg-viola-passive-g-v1", 2,
                   195.58538558422308, 439.0743450444911,
                   195.99771799087463),
            "d4": ("hlolli-wg-viola-passive-d-v1", 3,
                   292.96845778349154, 438.95671359865264,
                   293.6647679174076),
            "a4": ("hlolli-wg-viola-passive-a-v1", 4,
                   441.08823856808476, 441.08823856808476,
                   440.0),
        }
        with tempfile.TemporaryDirectory(prefix="hwa viola targets ") as text:
            root = Path(text)
            viola = fake_viola_tree(root)
            compiler, csound, csound_library = fake_tools(root)
            includes = fake_includes(root)
            for index, (target, target_expected) in enumerate(expected.items()):
                with self.subTest(target=target):
                    (adapter_id, render_string, fit_frequency_hz,
                     fit_a4, check_frequency_hz) = target_expected
                    fit = root / (target + "-fit.wav")
                    check = root / (target + "-check.wav")
                    pcm16_wave(fit, frames=256 + index, channels=1)
                    pcm24_wave(check, frames=300 + index, channels=2)
                    bundle = root / (target + "-bundle")
                    build_bundle(module, types.SimpleNamespace(
                        output_dir=bundle,
                        viola_root=viola,
                        csound=csound,
                        csound_library=csound_library,
                        c_compiler=compiler,
                        python=PYTHON,
                        csound_include_dir=[includes],
                        target=target,
                        reference_fit=fit,
                        reference_check=check,
                        reference_c3_fit=None,
                        reference_c3_check=None,
                        fit_frequency_hz=fit_frequency_hz,
                        check_frequency_hz=check_frequency_hz,
                        sample_count=1,
                    ))
                    receipt = json.loads(
                        (bundle / "receipt.json").read_text(encoding="utf-8")
                    )
                    self.assertEqual(receipt["adapter_id"], adapter_id)
                    self.assertEqual(receipt["target"], target)
                    self.assertEqual(receipt["render"], {
                        "fit": {
                            "a4_hz": fit_a4,
                            "frequency_hz": fit_frequency_hz,
                            "string": render_string,
                        },
                        "check": {
                            "a4_hz": 440.0,
                            "frequency_hz": check_frequency_hz,
                            "string": render_string,
                        },
                    })
                    described = subprocess.run(
                        [str(bundle / "renderer"), "--describe"],
                        check=False, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, text=True, env={}, cwd=root,
                    )
                    self.assertEqual(described.returncode, 0, described.stderr)
                    description = json.loads(described.stdout)
                    self.assertEqual(description["target"], target)
                    self.assertEqual(description["render"], receipt["render"])
                    bindings = json.loads(
                        (bundle / "bindings.json").read_text(encoding="utf-8")
                    )["bindings"]
                    for split, reference, channels in (
                            ("fit", fit, 1), ("check", check, 2)):
                        binding = next(
                            row for row in bindings
                            if row["id"] ==
                            "reference_{}_{}".format(target, split)
                        )
                        job = root / (target + "-" + split + "-job")
                        job.mkdir()
                        output = job / "model.wav"
                        value = 0.4 + index * 0.1
                        request = render_request(
                            reference, output, value=value, split=split,
                            reference_channels=channels, target=target,
                        )
                        request["inputs"][0]["sha256"] = binding["sha256"]
                        request_path = job / "request.json"
                        request_path.write_text(
                            json.dumps(request), encoding="utf-8"
                        )
                        completed = subprocess.run(
                            [str(bundle / "renderer"), "--hwa-experiment-job",
                             str(request_path), "--output-dir", str(job)],
                            check=False, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, env={}, cwd=job,
                        )
                        self.assertEqual(
                            completed.returncode, 0, completed.stderr
                        )
                        with wave.open(str(output), "rb") as stream:
                            self.assertEqual(
                                (stream.getframerate(), stream.getnchannels(),
                                 stream.getsampwidth(), stream.getnframes()),
                                (44100, 2, 3, 176416),
                            )
                            rendered = stream.readframes(3)
                            first = int.from_bytes(
                                rendered[:3], "little", signed=True
                            )
                        self.assertEqual(first, round(value * 100000))
                        case_render = receipt["render"][split]
                        self.assertEqual(
                            int.from_bytes(rendered[6:9], "little", signed=True),
                            round(case_render["frequency_hz"] * 10000),
                        )
                        self.assertEqual(
                            int.from_bytes(rendered[12:15], "little", signed=True),
                            round(case_render["a4_hz"] * 10000),
                        )

    def test_generic_build_requires_bounded_fit_and_check_tuning(self) -> None:
        module = load_adapter()
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            viola = root / "viola"
            viola.mkdir()
            fit = root / "fit.wav"
            check = root / "check.wav"
            pcm16_wave(fit)
            pcm24_wave(check)
            base = {
                "output_dir": root / "bundle",
                "viola_root": viola,
                "target": "g3",
                "reference_fit": fit,
                "reference_check": check,
                "reference_c3_fit": None,
                "reference_c3_check": None,
                "fit_frequency_hz": 195.58538558422308,
                "check_frequency_hz": 195.99771799087463,
            }
            missing = dict(base)
            missing["check_frequency_hz"] = None
            with self.assertRaisesRegex(
                    module.AdapterError, "fit-frequency-hz"):
                module.build(types.SimpleNamespace(**missing))
            low = dict(base)
            low["fit_frequency_hz"] = 160.0
            with self.assertRaisesRegex(
                    module.AdapterError, "numeric-A4 range"):
                module.build(types.SimpleNamespace(**low))
            nonfinite = dict(base)
            nonfinite["check_frequency_hz"] = float("nan")
            with self.assertRaisesRegex(module.AdapterError, "must be finite"):
                module.build(types.SimpleNamespace(**nonfinite))


    def test_fit_manifest_passes_the_shared_selector_contract(self) -> None:
        adapter = load_adapter()
        spec = importlib.util.spec_from_file_location("instrument_fit", FIT_TOOL)
        tool = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(tool)
        manifest = tool.fit_manifest(FIT_MANIFEST)
        self.assertEqual(manifest["adapter_id"],
                         "hlolli-wg-viola-passive-c-v1")
        adapter.validate_fit_manifest(manifest)
        changed = json.loads(json.dumps(manifest))
        changed["objectives"][0]["resource_id"] = "model.other"
        with self.assertRaises(adapter.AdapterError):
            adapter.validate_fit_manifest(changed)
        changed = json.loads(json.dumps(manifest))
        changed["schema_version"] = True
        with self.assertRaises(adapter.AdapterError):
            adapter.validate_fit_manifest(changed)

    def test_csound_core_library_proof_uses_the_loaded_path(self) -> None:
        module = load_adapter()
        expected = Path("/private/tmp/libcsound64.dylib")
        completed = types.SimpleNamespace(
            stdout="", stderr="dyld[1]: {}\n".format(expected)
        )
        with mock.patch.object(module.sys, "platform", "darwin"):
            with mock.patch.object(module, "run_tool", return_value=completed):
                module.verify_csound_library(
                    Path("/private/tmp/csound"), expected,
                    Path("/private/tmp"), {"PATH": "/usr/bin:/bin"},
                )
                with self.assertRaisesRegex(module.AdapterError,
                                            "did not load"):
                    module.verify_csound_library(
                        Path("/private/tmp/csound"),
                        Path("/private/tmp/other.dylib"),
                        Path("/private/tmp"), {"PATH": "/usr/bin:/bin"},
                    )

    def test_passive_c_contract_uses_fixed_model_path(self) -> None:
        module = load_adapter()
        with tempfile.TemporaryDirectory(prefix="hwa viola adapter ") as text:
            viola = fake_viola_tree(Path(text))
            module.VIOLA_ROOT = viola
            model_path = viola / "model" / "viola-v1.json"
            module.validate_profile(model_path)
            model = json.loads(model_path.read_text(encoding="utf-8"))
        manifest = json.loads(FIT_MANIFEST.read_text(encoding="utf-8"))

        self.assertEqual(manifest["schema"], "hwa-instrument-fit")
        self.assertEqual(manifest["schema_version"], 1)
        self.assertEqual(manifest["adapter_id"], module.ADAPTER_ID)
        self.assertEqual(len(manifest["parameters"]), 1)
        parameter = manifest["parameters"][0]
        self.assertEqual(parameter["id"], "loss_time_constant_c_seconds")
        self.assertEqual(
            parameter["profile_paths"],
            [["strings", 0, "loss_time_constant_seconds"]],
        )
        self.assertEqual(parameter["baseline"],
                         model["strings"][0]["loss_time_constant_seconds"])
        self.assertEqual({row["split"] for row in manifest["objectives"]},
                         {"fit", "check"})

    def test_experiment_binds_distinct_fit_and_check_audio(self) -> None:
        module = load_adapter()
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            fit = root / "fit.wav"
            check = root / "check.wav"
            extensible_pcm24_wave(fit, channels=1)
            pcm24_wave(check, frames=257)
            experiment = module.build_experiment(
                {"reference_c3_fit": fit, "reference_c3_check": check},
                sample_count=3,
            )

        self.assertEqual(experiment["schema"], "hwa-experiment")
        self.assertEqual(experiment["schema_version"], 1)
        self.assertEqual(experiment["method_version"], "stage8-1")
        self.assertEqual(experiment["clock_rate_hz"], 44100)
        self.assertEqual(experiment["plan"]["sample_count"], 3)
        self.assertEqual(
            [row["id"] for row in experiment["parameters"]],
            ["loss_time_constant_c_seconds"],
        )
        self.assertEqual(
            [(row["id"], row["split"]) for row in experiment["cases"]],
            [("c3-passive-check", "check"),
             ("c3-passive-fit", "fit")],
        )
        self.assertEqual(
            [row["id"] for row in experiment["inputs"]],
            ["reference_c3_check", "reference_c3_fit"],
        )
        self.assertEqual(len({row["sha256"] for row in experiment["inputs"]}),
                         2)
        self.assertTrue(all(len(row["sha256"]) == 64
                            for row in experiment["inputs"]))

    def test_experiment_accepts_extensible_pcm24_reference(self) -> None:
        module = load_adapter()
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            fit = root / "fit.wav"
            check = root / "check.wav"
            extensible_pcm24_wave(fit, channels=1)
            pcm24_wave(check, frames=257, channels=2)
            experiment = module.build_experiment(
                {"reference_c3_fit": fit, "reference_c3_check": check},
                sample_count=1,
            )
        cases = {row["id"]: row for row in experiment["cases"]}
        fit_reference = cases["c3-passive-fit"]["stems"][1]
        self.assertEqual(fit_reference["channels"], 1)

    def test_experiment_accepts_extensible_pcm16_reference(self) -> None:
        module = load_adapter()
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            fit = root / "fit.wav"
            check = root / "check.wav"
            extensible_pcm16_wave(fit, channels=2)
            pcm24_wave(check, frames=257, channels=1)
            experiment = module.build_experiment(
                {"reference_g3_fit": fit, "reference_g3_check": check},
                sample_count=1, target="g3",
            )
        cases = {row["id"]: row for row in experiment["cases"]}
        self.assertEqual(
            cases["g3-passive-fit"]["stems"][1]["channels"], 2
        )

    def test_build_publishes_one_checked_frozen_bundle(self) -> None:
        module = load_adapter()
        with tempfile.TemporaryDirectory(prefix="hwa viola bundle ") as text:
            root = Path(text)
            viola = fake_viola_tree(root)
            fit = root / "fit.wav"
            check = root / "check.wav"
            extensible_pcm24_wave(fit, channels=1)
            pcm24_wave(check, frames=257)
            compiler, csound, csound_library = fake_tools(root)
            includes = fake_includes(root)
            python = root / "python with spaces"
            write_executable(
                python, "#!/bin/sh\nexec \"{}\" \"$@\"\n".format(PYTHON)
            )
            outputs = [root / "bundle-one", root / "bundle-two"]
            for output in outputs:
                build_bundle(module, types.SimpleNamespace(
                    output_dir=output,
                    viola_root=viola,
                    csound=csound,
                    csound_library=csound_library,
                    c_compiler=compiler,
                    python=python,
                    csound_include_dir=[includes],
                    reference_c3_fit=fit,
                    reference_c3_check=check,
                    sample_count=3,
                ))

            self.assertEqual(
                sorted(path.name for path in outputs[0].iterdir()),
                ["bindings.json", "experiment.json", "fit.json",
                 "receipt.json", "renderer"],
            )
            self.assertEqual((outputs[0] / "renderer").read_bytes(),
                             (outputs[1] / "renderer").read_bytes())
            described = subprocess.run(
                [str(outputs[0] / "renderer"), "--describe"],
                check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, env={}, cwd=root,
            )
            self.assertEqual(described.returncode, 0, described.stderr)
            description = json.loads(described.stdout)
            self.assertEqual(description["schema"], "hwa-viola-renderer")
            self.assertEqual(description["adapter_id"], module.ADAPTER_ID)
            self.assertEqual(description["permissions"], {
                "render": True,
                "validate_profile": True,
                "write_profile": False,
            })
            validated = subprocess.run(
                [str(outputs[0] / "renderer"), "--validate-profile",
                 str(viola / "model" / "viola-v1.json")],
                check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, env={}, cwd=root,
            )
            self.assertEqual(validated.returncode, 0, validated.stderr)

            compiler_bytes = compiler.read_bytes()
            compiler.write_bytes(b"changed compiler\n")
            changed = subprocess.run(
                [str(outputs[0] / "renderer"), "--describe"],
                check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, env={}, cwd=root,
            )
            self.assertNotEqual(changed.returncode, 0)
            self.assertIn("configured resource changed: c_compiler",
                          changed.stderr)
            compiler.write_bytes(compiler_bytes)

            csound_library.write_bytes(b"changed core library\n")
            changed = subprocess.run(
                [str(outputs[0] / "renderer"), "--describe"],
                check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, env={}, cwd=root,
            )
            self.assertNotEqual(changed.returncode, 0)
            self.assertIn("configured resource changed: csound_library",
                          changed.stderr)

    def test_render_job_builds_the_candidate_and_only_declared_wave(self) -> None:
        module = load_adapter()
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            viola = fake_viola_tree(root)
            compiler, csound, csound_library = fake_tools(root)
            includes = fake_includes(root)
            fit = root / "fit.wav"
            check = root / "check.wav"
            extensible_pcm24_wave(fit, channels=1)
            pcm24_wave(check, frames=257)
            bundle = root / "bundle"
            build_bundle(module, types.SimpleNamespace(
                output_dir=bundle, viola_root=viola, csound=csound,
                csound_library=csound_library,
                c_compiler=compiler, python=PYTHON,
                csound_include_dir=[includes],
                reference_c3_fit=fit, reference_c3_check=check,
                sample_count=1,
            ))
            job = root / "job"
            job.mkdir()
            output = job / "model.wav"
            request = render_request(fit, output)
            request_path = job / "request.json"
            request_path.write_text(json.dumps(request), encoding="utf-8")
            completed = subprocess.run(
                [str(bundle / "renderer"), "--hwa-experiment-job",
                 str(request_path), "--output-dir", str(job)],
                check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, env={}, cwd=job,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            with wave.open(str(output), "rb") as stream:
                self.assertEqual(
                    (stream.getframerate(), stream.getnchannels(),
                     stream.getsampwidth(), stream.getnframes()),
                    (44100, 2, 3, 176416),
                )
            self.assertEqual(sorted(path.name for path in job.iterdir()),
                             ["model.wav", "request.json"])

            second_job = root / "second-job"
            second_job.mkdir()
            second_output = second_job / "model.wav"
            second_request = json.loads(json.dumps(request))
            second_request["outputs"][0]["path"] = str(second_output)
            second_request["parameters"][0]["value"] = 0.8
            second_request_path = second_job / "request.json"
            second_request_path.write_text(
                json.dumps(second_request), encoding="utf-8"
            )
            second = subprocess.run(
                [str(bundle / "renderer"), "--hwa-experiment-job",
                 str(second_request_path), "--output-dir", str(second_job)],
                check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, env={}, cwd=second_job,
            )
            self.assertEqual(second.returncode, 0, second.stderr)
            self.assertNotEqual(output.read_bytes(), second_output.read_bytes())

    def test_render_job_rejects_wrong_request_fields(self) -> None:
        module = load_adapter()
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            viola = fake_viola_tree(root)
            compiler, csound, csound_library = fake_tools(root)
            includes = fake_includes(root)
            fit = root / "fit.wav"
            check = root / "check.wav"
            pcm24_wave(fit, channels=1)
            pcm24_wave(check, frames=257)
            bundle = root / "bundle"
            build_bundle(module, types.SimpleNamespace(
                output_dir=bundle, viola_root=viola, csound=csound,
                csound_library=csound_library,
                c_compiler=compiler, python=PYTHON,
                csound_include_dir=[includes],
                reference_c3_fit=fit, reference_c3_check=check,
                sample_count=1,
            ))
            variants = (
                "missing_job_id", "unknown_root_field", "boolean_job_id",
                "boolean_schema_version", "boolean_input_gain",
                "boolean_output_start", "bad_job_key", "wrong_split",
                "wrong_binding",
                "wrong_reference_hash", "extra_input_field",
                "wrong_output_name", "extra_output_field", "wrong_unit",
                "extra_parameter_field", "nan_value", "oversized_request",
            )
            for index, variant in enumerate(variants):
                with self.subTest(variant=variant):
                    job = root / "job-{}".format(index)
                    job.mkdir()
                    output = job / "model.wav"
                    request = render_request(fit, output)
                    if variant == "missing_job_id":
                        request.pop("job_id")
                    elif variant == "unknown_root_field":
                        request["other"] = 1
                    elif variant == "boolean_job_id":
                        request["job_id"] = True
                    elif variant == "boolean_schema_version":
                        request["schema_version"] = True
                    elif variant == "boolean_input_gain":
                        request["inputs"][0]["gain_db"] = False
                    elif variant == "boolean_output_start":
                        request["outputs"][0]["start_sample"] = False
                    elif variant == "bad_job_key":
                        request["job_key"] = "A" * 64
                    elif variant == "wrong_split":
                        request["split"] = "check"
                    elif variant == "wrong_binding":
                        request["inputs"][0]["binding_id"] = "other"
                    elif variant == "wrong_reference_hash":
                        request["inputs"][0]["sha256"] = "0" * 64
                    elif variant == "extra_input_field":
                        request["inputs"][0]["other"] = 1
                    elif variant == "wrong_output_name":
                        request["outputs"][0]["path"] = str(job / "other.wav")
                    elif variant == "extra_output_field":
                        request["outputs"][0]["other"] = 1
                    elif variant == "wrong_unit":
                        request["parameters"][0]["unit"] = "ratio"
                    elif variant == "extra_parameter_field":
                        request["parameters"][0]["other"] = 1
                    else:
                        request["parameters"][0]["value"] = float("nan")
                    request_path = job / "request.json"
                    if variant == "oversized_request":
                        request_path.write_text(
                            " " * (module.MAX_JSON_BYTES + 1), encoding="utf-8"
                        )
                    else:
                        request_path.write_text(
                            json.dumps(request), encoding="utf-8"
                        )
                    completed = subprocess.run(
                        [str(bundle / "renderer"), "--hwa-experiment-job",
                         str(request_path), "--output-dir", str(job)],
                        check=False, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, text=True, env={}, cwd=job,
                    )
                    self.assertNotEqual(completed.returncode, 0)
                    self.assertFalse(output.exists())

    def test_failed_or_clipped_render_leaves_no_output(self) -> None:
        module = load_adapter()
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            viola = fake_viola_tree(root)
            compiler, csound, csound_library = fake_tools(root)
            includes = fake_includes(root)
            fit = root / "fit.wav"
            check = root / "check.wav"
            pcm24_wave(fit, channels=1)
            pcm24_wave(check, frames=257)
            bundle = root / "bundle"
            build_bundle(module, types.SimpleNamespace(
                output_dir=bundle, viola_root=viola, csound=csound,
                csound_library=csound_library,
                c_compiler=compiler, python=PYTHON,
                csound_include_dir=[includes],
                reference_c3_fit=fit, reference_c3_check=check,
                sample_count=1,
            ))
            for index, value in enumerate((0.02, 0.03, 0.04, 4.9, 5.0)):
                with self.subTest(value=value):
                    job = root / "job-{}".format(index)
                    job.mkdir()
                    output = job / "model.wav"
                    request_path = job / "request.json"
                    request_path.write_text(
                        json.dumps(render_request(fit, output, value)),
                        encoding="utf-8",
                    )
                    completed = subprocess.run(
                        [str(bundle / "renderer"), "--hwa-experiment-job",
                         str(request_path), "--output-dir", str(job)],
                        check=False, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, text=True, env={}, cwd=job,
                    )
                    self.assertNotEqual(completed.returncode, 0)
                    self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
