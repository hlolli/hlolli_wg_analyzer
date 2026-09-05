#!/usr/bin/env python3

import importlib.util
import json
import math
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import types
import unittest
from unittest import mock
import wave


TOOL = Path(__file__).resolve().parents[1] / "tools" / "instrument_fit.py"
SPEC = importlib.util.spec_from_file_location("instrument_fit", TOOL)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def write_decay(path, tau, *, width=2, lead=0.10, duration=2.5,
                gain=0.7, polarity=1.0, second_onset=None, noise=0.0,
                extensible=False, secondary_tau=None, secondary_mix=0.45,
                modulation_db=0.0, modulation_period=1.5,
                second_frequency=220.0, second_gain=0.7,
                second_attack=0.0, pre_noise=0.0):
    rate = 16000
    frames = round((lead + duration) * rate)
    maximum = (1 << (width * 8 - 1)) - 1
    state = 1
    raw = bytearray()
    for index in range(frames):
        time = index / rate
        value = 0.0
        if time >= lead:
            age = time - lead
            envelope = math.exp(-age / tau)
            if secondary_tau is not None:
                envelope = ((1.0 - secondary_mix) * envelope +
                            secondary_mix * math.exp(-age / secondary_tau))
            if modulation_db:
                envelope *= 10.0 ** (
                    modulation_db * math.sin(
                        2.0 * math.pi * age / modulation_period
                    ) / 20.0
                )
            value = gain * envelope * math.sin(2.0 * math.pi * 220.0 * age)
        if second_onset is not None and time >= lead + second_onset:
            age = time - lead - second_onset
            attack = (1.0 if second_attack <= 0.0 else
                      min(1.0, 0.5 - 0.5 * math.cos(
                          math.pi * age / second_attack)))
            value += second_gain * attack * math.exp(-age / tau) * math.sin(
                2.0 * math.pi * second_frequency * age
            )
        noise_gain = pre_noise if time < lead else noise
        if noise_gain:
            state = (1103515245 * state + 12345) & 0x7fffffff
            value += noise_gain * (2.0 * state / 0x7fffffff - 1.0)
        sample = max(-maximum, min(maximum, round(polarity * value * maximum)))
        encoded = int(sample).to_bytes(width, "little", signed=True)
        raw.extend(encoded)
        raw.extend(encoded)
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(2)
        stream.setsampwidth(width)
        stream.setframerate(rate)
        stream.writeframes(raw)
    if extensible:
        if width != 3:
            raise ValueError("extensible test data must be 24-bit")
        format_chunk = struct.pack(
            "<HHIIHHH", 0xfffe, 2, rate, rate * 6, 6, 24, 22
        ) + struct.pack("<HI", 24, 3) + bytes.fromhex(
            "0100000000001000800000aa00389b71"
        )
        chunks = (b"fmt " + struct.pack("<I", len(format_chunk)) + format_chunk +
                  b"data" + struct.pack("<I", len(raw)) + raw)
        path.write_bytes(b"RIFF" + struct.pack("<I", len(chunks) + 4) +
                         b"WAVE" + chunks)


def write_spectral_decay(path, low_tau, high_tau, *, lead=0.10,
                         duration=3.0):
    rate = 16000
    maximum = 32767
    frames = round((lead + duration) * rate)
    raw = bytearray()
    for index in range(frames):
        time = index / rate
        value = 0.0
        if time >= lead:
            age = time - lead
            value = (
                0.50 * math.exp(-age / low_tau) *
                math.sin(2.0 * math.pi * 220.0 * age) +
                0.22 * math.exp(-age / high_tau) *
                math.sin(2.0 * math.pi * 1760.0 * age + 0.31)
            )
        sample = max(-maximum, min(maximum, round(value * maximum)))
        encoded = int(sample).to_bytes(2, "little", signed=True)
        raw.extend(encoded)
        raw.extend(encoded)
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(2)
        stream.setsampwidth(2)
        stream.setframerate(rate)
        stream.writeframes(raw)


def write_harmonic_decay(path, fundamental, taus, *, lead=0.10,
                         duration=4.0):
    rate = 16000
    maximum = 32767
    frames = round((lead + duration) * rate)
    raw = bytearray()
    for index in range(frames):
        time = index / rate
        value = 0.0
        if time >= lead:
            age = time - lead
            for harmonic, tau in enumerate(taus, 1):
                value += (
                    0.18 / harmonic * math.exp(-age / tau) *
                    math.sin(2.0 * math.pi * fundamental * harmonic * age)
                )
        sample = max(-maximum, min(maximum, round(value * maximum)))
        encoded = int(sample).to_bytes(2, "little", signed=True)
        raw.extend(encoded)
        raw.extend(encoded)
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(2)
        stream.setsampwidth(2)
        stream.setframerate(rate)
        stream.writeframes(raw)


def make_v2_fixture(root, *, candidate_tau=0.40,
                    expected_loss=0.05):
    adapter_path = root / "profile-adapter.py"
    adapter_path.write_text(
        "#!/usr/bin/env python3\n"
        "import json, sys\n"
        "assert sys.argv[1] == '--validate-profile'\n"
        "json.load(open(sys.argv[2], encoding='utf-8'))\n",
        encoding="utf-8",
    )
    adapter_path.chmod(0o755)
    references = {}
    for split, lead in (("fit", 0.07), ("check", 0.19), ("audit", 0.31)):
        path = root / (split + "-reference.wav")
        write_decay(path, 0.40, lead=lead)
        references[split] = path
    jobs = []
    artifacts = []
    job_id = 0
    for point_id, tau in ((1, 0.80), (2, candidate_tau)):
        for case_id, case_name in (
                (1, "audit"), (2, "check"), (3, "fit")):
            job_id += 1
            audio = root / ("model-%d-%s.wav" % (point_id, case_name))
            write_decay(audio, tau, lead=0.11 + case_id * 0.03)
            jobs.append({
                "id": job_id, "key": (str(job_id) * 64)[:64],
                "point_id": point_id, "case_id": case_id,
            })
            artifacts.append({
                "id": job_id, "job_id": job_id,
                "resource_id": "model.final",
                "artifact": {"path": audio.name,
                             "sha256": MODULE.sha256(audio)},
                "file_bytes": audio.stat().st_size, "kind": "stem",
            })
    experiment = {
        "command": "experiment", "schema_version": 10,
        "parameters": [{
            "id": 1, "name": "joint_candidate", "unit": "choice",
            "minimum": 0, "maximum": 1, "baseline": 0,
        }],
        "cases": [
            {"id": 1, "name": "c-audit", "split": "audit", "weight": 1},
            {"id": 2, "name": "c-check", "split": "check", "weight": 1},
            {"id": 3, "name": "c-fit", "split": "fit", "weight": 1},
        ],
        "responses": [],
        "points": [
            {"id": 1, "key": "a" * 64, "baseline": True},
            {"id": 2, "key": "b" * 64, "baseline": False},
        ],
        "values": [{"id": 1, "point_id": 2, "parameter_id": 1,
                    "value": 1}],
        "jobs": jobs, "artifacts": artifacts, "candidates": [],
    }
    objectives = [
        {"id": "c-fit", "kind": "passive-decay", "case": "c-fit",
         "reference_binding": "c_fit", "resource_id": "model.final",
         "split": "fit", "weight": 1, "scale": 1},
        {"id": "c-check", "kind": "passive-decay", "case": "c-check",
         "reference_binding": "c_check", "resource_id": "model.final",
         "split": "check", "weight": 1, "scale": 1},
        {"id": "c-audit", "kind": "passive-decay", "case": "c-audit",
         "reference_binding": "c_audit", "resource_id": "model.final",
         "split": "audit", "weight": 1, "scale": 1},
    ]
    changes = [{
        "parameter": "loss_time_constant_%s_seconds" % letter,
        "path": ["strings", index, "loss_time_constant_seconds"],
        "before": 0.25, "after": value, "minimum": 0.08,
        "maximum": 1.5, "unit": "seconds",
        "source_fit_result_sha256": str(index + 1) * 64,
    } for index, (letter, value) in enumerate(
        (("c", 1.0), ("g", 1.0), ("d", 0.75), ("a", 1.0))
    )]
    manifest = {
        "schema": "hwa-instrument-fit", "schema_version": 2,
        "adapter_id": "joint-decay",
        "parameters": [{
            "id": "joint_candidate", "unit": "choice", "minimum": 0,
            "maximum": 1, "baseline": 0, "profile_paths": [],
        }],
        "objectives": objectives,
        "selection": {
            "mode": "verify-candidate",
            "score_weights": {"fit": 1, "check": 1, "audit": 1},
            "max_score_increase": 0,
            "max_candidate_worst_harm": 0.25,
            "max_expected_loss_increase": 0.25,
            "minimum_candidate_t60_ratio": 0.5,
            "maximum_candidate_t60_ratio": 2.0,
            "minimum_candidate_support_ratio": 0.5,
            "limits": [
                {"split": "fit", "max_mean_loss_increase": 0,
                 "max_objective_loss_increase": 0.25,
                 "max_candidate_loss": 2.0},
                {"split": "check", "max_mean_loss_increase": 0.15,
                 "max_objective_loss_increase": 0.25,
                 "max_candidate_loss": 2.0},
                {"split": "audit", "max_mean_loss_increase": 0.15,
                 "max_objective_loss_increase": 0.25,
                 "max_candidate_loss": 2.0},
            ],
        },
        "candidate": {
            "parameters": {"joint_candidate": 1},
            "expected_objective_losses": {
                "c-fit": expected_loss, "c-check": expected_loss,
            },
            "profile_changes": changes,
            "profile_adapter_sha256": MODULE.sha256(adapter_path),
        },
    }
    manifest_path = root / "fit-v2.json"
    experiment_path = root / "experiment-v2.json"
    profile_path = root / "profile.json"
    analyzer_path = root / "analyzer"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    experiment_path.write_text(json.dumps(experiment), encoding="utf-8")
    profile_path.write_text(
        json.dumps({"strings": [
            {"loss_time_constant_seconds": 0.25} for unused in range(4)
        ]}),
        encoding="utf-8",
    )
    analyzer_path.write_text("not used", encoding="utf-8")
    return {
        "manifest": manifest_path, "experiment": experiment_path,
        "profile": profile_path, "analyzer": analyzer_path,
        "references": references, "adapter": adapter_path,
    }


def select_v2_command(fixture, output):
    references = fixture["references"]
    return [
        sys.executable, str(TOOL), "select",
        "--manifest", str(fixture["manifest"]),
        "--experiment", str(fixture["experiment"]),
        "--analyzer", str(fixture["analyzer"]),
        "--profile", str(fixture["profile"]),
        "--bind", "c_fit=" + str(references["fit"]),
        "--bind", "c_check=" + str(references["check"]),
        "--bind", "c_audit=" + str(references["audit"]),
        "--output", str(output),
    ]


def make_v1_fixture(root, *, candidate_tau=0.40,
                    max_candidate_loss=None,
                    max_check_loss_increase=0.1):
    fixture = make_v2_fixture(root, candidate_tau=candidate_tau)
    manifest = json.loads(
        fixture["manifest"].read_text(encoding="utf-8")
    )
    manifest["schema_version"] = 1
    manifest["objectives"] = [
        row for row in manifest["objectives"] if row["split"] != "audit"
    ]
    manifest["selection"] = {
        "check_weight": 1,
        "max_check_loss_increase": max_check_loss_increase,
        "max_candidate_worst_harm": 1,
    }
    if max_candidate_loss is not None:
        manifest["selection"].update({
            "max_candidate_loss": max_candidate_loss,
            "minimum_candidate_t60_ratio": 0.5,
            "maximum_candidate_t60_ratio": 2.0,
            "minimum_candidate_support_ratio": 0.5,
        })
    del manifest["candidate"]
    fixture["manifest"].write_text(json.dumps(manifest), encoding="utf-8")
    return fixture


def select_v1_command(fixture, output):
    references = fixture["references"]
    return [
        sys.executable, str(TOOL), "select",
        "--manifest", str(fixture["manifest"]),
        "--experiment", str(fixture["experiment"]),
        "--analyzer", str(fixture["analyzer"]),
        "--profile", str(fixture["profile"]),
        "--bind", "c_fit=" + str(references["fit"]),
        "--bind", "c_check=" + str(references["check"]),
        "--output", str(output),
    ]


def write_fake_checked_analyzer(path):
    python = Path(sys.executable).resolve()
    path.write_text(
        "#!{} -I\n".format(python) +
        "import json, pathlib, sys\n"
        "a=sys.argv[1:]\n"
        "if len(a)==7 and a[:2]==['--json','isolated-note']:\n"
        " p=str(pathlib.Path(a[2]).absolute()); hz=float(a[4]); valid='invalid' not in pathlib.Path(p).name\n"
        " print(json.dumps({'schema':'hwa-isolated-note','schema_version':1,'command':'isolated-note','method':'isolated-note-1','path':p,'expected_hz':hz,'requested_metrics':['pitch'],'pitch':{'valid':valid,'cents':0.25}})); raise SystemExit(0)\n"
        "if len(a)==6 and a[:2]==['--json','harmonic-decay']:\n"
        " r=str(pathlib.Path(a[2]).absolute()); m=str(pathlib.Path(a[3]).absolute()); hz=float(a[5]); valid='invalid' not in pathlib.Path(m).name\n"
        " name=pathlib.Path(m).name; error=1.2 if 'candidate' in name or 'model-2-' in name else 6.0\n"
        " bands=[{'valid':valid,'t60_log_error_db':error if valid else None} for _ in range(4)]\n"
        " profile=lambda p,v:{'path':p,'valid':v}\n"
        " comparison={'valid':valid,'shared_valid_band_count':4 if valid else 0,'shared_reference_coverage':1.0,'t60_log_rmse_db':error if valid else None,'median_t60_log_bias_db':error if valid else None,'bands':bands}\n"
        " print(json.dumps({'schema':'hwa-harmonic-decay','schema_version':1,'command':'harmonic-decay','method':'harmonic-decay-1','expected_hz':hz,'reference':profile(r,True),'model':profile(m,valid),'comparison':comparison})); raise SystemExit(0)\n"
        "raise SystemExit(9)\n",
        encoding="utf-8",
    )
    path.chmod(0o700)


class InstrumentFitTests(unittest.TestCase):
    def test_v1_can_gate_on_check_without_using_it_to_rank(self):
        with tempfile.TemporaryDirectory() as text:
            fixture = make_v1_fixture(Path(text))
            manifest = json.loads(
                fixture["manifest"].read_text(encoding="utf-8")
            )
            manifest["selection"]["check_weight"] = 0
            fixture["manifest"].write_text(
                json.dumps(manifest), encoding="utf-8"
            )
            parsed = MODULE.fit_manifest(fixture["manifest"])

        self.assertEqual(parsed["selection"]["check_weight"], 0)
        rows = [
            {"score": 1.0, "check_loss": 2.0, "point_id": 2},
            {"score": 1.0, "check_loss": 1.0, "point_id": 3},
        ]
        self.assertEqual(
            min(rows, key=lambda row: MODULE.v1_rank_key(row, 0.0))[
                "point_id"
            ],
            2,
        )
        self.assertEqual(
            min(rows, key=lambda row: MODULE.v1_rank_key(row, 1.0))[
                "point_id"
            ],
            3,
        )

    def test_v1_can_cap_each_objective_loss_increase(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            fixture = make_v1_fixture(
                root, candidate_tau=0.10, max_check_loss_increase=100.0
            )
            manifest = json.loads(
                fixture["manifest"].read_text(encoding="utf-8")
            )
            manifest["selection"]["max_objective_loss_increase"] = 0.0
            fixture["manifest"].write_text(
                json.dumps(manifest), encoding="utf-8"
            )
            output = root / "objective-increase-result.json"
            completed = subprocess.run(
                select_v1_command(fixture, output), check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            self.assertEqual(completed.returncode, 2, completed.stderr)
            result = json.loads(output.read_text(encoding="utf-8"))
            candidate = next(
                row for row in result["points"] if not row["baseline"]
            )
            self.assertFalse(candidate["eligible"])
            self.assertTrue(any(
                row["loss_increase_from_baseline"] > 0.0
                for row in candidate["evidence"]
            ))

    def test_v1_source_group_limits_require_two_groups_per_split(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            fixture = make_v1_fixture(root)
            manifest = json.loads(
                fixture["manifest"].read_text(encoding="utf-8")
            )
            rows = []
            for objective in manifest["objectives"]:
                for suffix in ("a", "b"):
                    rows.append({
                        **objective,
                        "id": objective["id"] + "-" + suffix,
                        "source_group": "source-" + suffix,
                    })
            manifest["objectives"] = rows
            manifest["selection"].update({
                "max_candidate_source_mean_loss": 3.0,
                "max_source_mean_loss_increase": 0.25,
            })
            fixture["manifest"].write_text(
                json.dumps(manifest), encoding="utf-8"
            )
            parsed = MODULE.fit_manifest(fixture["manifest"])
            self.assertEqual(len(parsed["objectives"]), 4)
            manifest["objectives"][0].pop("source_group")
            fixture["manifest"].write_text(
                json.dumps(manifest), encoding="utf-8"
            )
            with self.assertRaisesRegex(
                    MODULE.FitError, "group on every objective"):
                MODULE.fit_manifest(fixture["manifest"])

    def test_v1_source_groups_are_weighted_and_rank_the_worst_tie(self):
        objectives = [
            {"id": "fit-a-1", "split": "fit", "source_group": "a",
             "weight": 0.5},
            {"id": "fit-a-2", "split": "fit", "source_group": "a",
             "weight": 0.5},
            {"id": "fit-b", "split": "fit", "source_group": "b",
             "weight": 1.0},
        ]
        evidence = [
            {"objective": "fit-a-1", "loss": 1.0},
            {"objective": "fit-a-2", "loss": 3.0},
            {"objective": "fit-b", "loss": 1.5},
        ]
        groups = MODULE.v1_source_group_rows(objectives, evidence)
        self.assertEqual(groups, [
            {"split": "fit", "source_group": "a", "loss": 2.0},
            {"split": "fit", "source_group": "b", "loss": 1.5},
        ])
        rows = [
            {"score": 1.0, "check_loss": 0.5, "point_id": 2,
             "source_groups": [{"loss": 2.0}]},
            {"score": 1.0, "check_loss": 0.1, "point_id": 3,
             "source_groups": [{"loss": 1.5}]},
        ]
        self.assertEqual(
            min(rows, key=lambda row: MODULE.v1_rank_key(row, 1.0))[
                "point_id"
            ],
            3,
        )

    def test_v1_gate_failure_writes_evidence_and_cannot_write_profile(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            fixture = make_v1_fixture(root, max_candidate_loss=0.0)
            failed_output = root / "failed-v1-result.json"
            completed = subprocess.run(
                select_v1_command(fixture, failed_output), check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            self.assertEqual(completed.returncode, 2, completed.stderr)
            failed = json.loads(failed_output.read_text(encoding="utf-8"))
            self.assertEqual(failed["schema_version"], 1)
            self.assertEqual(failed["status"], "fail")
            self.assertFalse(any(name.startswith("chosen_") for name in failed))
            self.assertEqual(len(failed["points"]), 2)
            self.assertTrue(all(not row["eligible"]
                                for row in failed["points"]))
            self.assertTrue(all(
                {item["objective"] for item in row["evidence"]} ==
                {"c-fit", "c-check"}
                for row in failed["points"]
            ))

            rejected_profile = root / "rejected-profile.json"
            rejected_receipt = root / "rejected-receipt.json"
            completed = subprocess.run([
                sys.executable, str(TOOL), "write-profile",
                "--manifest", str(fixture["manifest"]),
                "--fit", str(failed_output),
                "--source", str(fixture["profile"]),
                "--adapter", str(fixture["adapter"]),
                "--output", str(rejected_profile),
                "--receipt", str(rejected_receipt),
            ], check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
               text=True)
            self.assertEqual(completed.returncode, 1)
            self.assertIn("did not pass", completed.stderr)
            self.assertFalse(rejected_profile.exists())
            self.assertFalse(rejected_receipt.exists())

    def test_v1_baseline_is_only_the_held_out_comparator(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            fixture = make_v1_fixture(
                root, candidate_tau=0.10, max_check_loss_increase=0.0
            )
            output = root / "baseline-result.json"
            completed = subprocess.run(
                select_v1_command(fixture, output), check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            self.assertEqual(completed.returncode, 2, completed.stderr)
            result = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(result["status"], "fail")
            self.assertFalse(any(name.startswith("chosen_") for name in result))
            baseline = next(row for row in result["points"]
                            if row["baseline"])
            candidate = next(row for row in result["points"]
                             if not row["baseline"])
            self.assertFalse(baseline["eligible"])
            self.assertFalse(candidate["eligible"])

    def test_v2_manifest_validates_the_candidate_block(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            fixture = make_v2_fixture(root)
            manifest = json.loads(
                fixture["manifest"].read_text(encoding="utf-8")
            )
            del manifest["candidate"]["parameters"]
            fixture["manifest"].write_text(
                json.dumps(manifest), encoding="utf-8"
            )
            with self.assertRaisesRegex(MODULE.FitError, "candidate"):
                MODULE.fit_manifest(fixture["manifest"])

    def test_select_rejects_an_artifact_outside_the_experiment_directory(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            bundle = root / "bundle"
            bundle.mkdir()
            outside = root / "outside.wav"
            write_decay(outside, 0.40)
            fixture = make_v2_fixture(bundle)
            experiment = json.loads(
                fixture["experiment"].read_text(encoding="utf-8"))
            experiment["artifacts"][0]["artifact"] = {
                "path": "../outside.wav", "sha256": MODULE.sha256(outside),
            }
            fixture["experiment"].write_text(json.dumps(experiment),
                                               encoding="utf-8")
            output = bundle / "escaped-result.json"
            completed = subprocess.run(
                select_v2_command(fixture, output), check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            self.assertEqual(completed.returncode, 1)
            self.assertIn("artifact path escapes", completed.stderr)
            self.assertFalse(output.exists())

    def test_v2_select_rejects_extra_experiment_cases_and_jobs(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            fixture = make_v2_fixture(root)
            experiment = json.loads(
                fixture["experiment"].read_text(encoding="utf-8"))
            experiment["cases"].append({
                "id": 4, "name": "unused-check", "split": "check",
                "weight": 1,
            })
            fixture["experiment"].write_text(json.dumps(experiment),
                                               encoding="utf-8")
            output = root / "extra-case-result.json"
            completed = subprocess.run(
                select_v2_command(fixture, output), check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            self.assertEqual(completed.returncode, 1)
            self.assertIn("case set", completed.stderr)
            self.assertFalse(output.exists())

    def test_v2_audit_gate_cannot_choose_or_publish_the_candidate(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            fixture = make_v2_fixture(root)
            write_decay(fixture["references"]["audit"], 0.80, lead=0.31)
            output = root / "audit-failure.json"
            completed = subprocess.run(
                select_v2_command(fixture, output), check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            self.assertEqual(completed.returncode, 2, completed.stderr)
            result = json.loads(output.read_text(encoding="utf-8"))
            audit = next(row for row in result["gates"]["objectives"]
                         if row["split"] == "audit")
            self.assertFalse(audit["passed"])
            self.assertNotIn("chosen_point_id", result)

            experiment = json.loads(
                fixture["experiment"].read_text(encoding="utf-8")
            )
            next(row for row in experiment["cases"]
                 if row["name"] == "c-check")["split"] = "fit"
            fixture["experiment"].write_text(json.dumps(experiment),
                                               encoding="utf-8")
            invalid_output = root / "wrong-case-split.json"
            completed = subprocess.run(
                select_v2_command(fixture, invalid_output), check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            self.assertEqual(completed.returncode, 1)
            self.assertIn("wrong case split", completed.stderr)
            self.assertFalse(invalid_output.exists())

    def test_v2_absolute_gate_rejects_a_bad_audit_that_beats_baseline(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            fixture = make_v2_fixture(root)
            write_decay(fixture["references"]["audit"], 0.10, lead=0.31)
            output = root / "bad-but-improved-audit.json"
            completed = subprocess.run(
                select_v2_command(fixture, output), check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            self.assertEqual(completed.returncode, 2, completed.stderr)
            result = json.loads(output.read_text(encoding="utf-8"))
            audit = next(row for row in result["gates"]["objectives"]
                         if row["split"] == "audit")
            self.assertLess(audit["loss_increase"], 0.0)
            self.assertGreater(audit["candidate_loss"],
                               audit["maximum_candidate_loss"])
            self.assertFalse(audit["passed"])

    def test_v2_t60_gate_rejects_a_bad_ratio_even_with_a_loose_loss_cap(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            fixture = make_v2_fixture(root)
            write_decay(fixture["references"]["audit"], 0.10, lead=0.31)
            manifest = json.loads(
                fixture["manifest"].read_text(encoding="utf-8"))
            next(row for row in manifest["selection"]["limits"]
                 if row["split"] == "audit")["max_candidate_loss"] = 100.0
            fixture["manifest"].write_text(json.dumps(manifest),
                                            encoding="utf-8")
            output = root / "bad-ratio-audit.json"
            completed = subprocess.run(
                select_v2_command(fixture, output), check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            self.assertEqual(completed.returncode, 2, completed.stderr)
            result = json.loads(output.read_text(encoding="utf-8"))
            audit = next(row for row in result["gates"]["objectives"]
                         if row["split"] == "audit")
            self.assertGreater(audit["candidate_t60_ratio"],
                               audit["maximum_candidate_t60_ratio"])
            self.assertFalse(audit["passed"])

    def test_v2_select_writes_gate_failure_but_not_invalid_input(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            fixture = make_v2_fixture(root, candidate_tau=0.41,
                                      expected_loss=0.01)
            failed_output = root / "failed-result.json"
            completed = subprocess.run(
                select_v2_command(fixture, failed_output), check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            self.assertEqual(completed.returncode, 2, completed.stderr)
            failed = json.loads(failed_output.read_text(encoding="utf-8"))
            self.assertEqual(failed["schema_version"], 2)
            self.assertEqual(failed["status"], "fail")
            self.assertFalse(failed["gates"]["passed"])
            self.assertEqual(failed["selection_mode"], "verify-candidate")
            self.assertNotIn("chosen_point_id", failed)
            self.assertNotIn("chosen_parameters", failed)
            expected_gates = failed["gates"]["expected_objectives"]
            self.assertTrue(any(not row["passed"] for row in expected_gates))
            self.assertTrue(all(row["passed"]
                                for row in failed["gates"]["splits"]))
            self.assertTrue(all(row["passed"]
                                for row in failed["gates"]["objectives"]))
            self.assertEqual(failed["candidate_point_id"], 2)

            unused_adapter = root / "unused-adapter"
            unused_adapter.write_text("not reached", encoding="utf-8")
            rejected_profile = root / "rejected-profile.json"
            rejected_receipt = root / "rejected-receipt.json"
            completed = subprocess.run([
                sys.executable, str(TOOL), "write-profile",
                "--manifest", str(fixture["manifest"]),
                "--fit", str(failed_output),
                "--source", str(fixture["profile"]),
                "--adapter", str(unused_adapter),
                "--output", str(rejected_profile),
                "--receipt", str(rejected_receipt),
            ], check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
               text=True)
            self.assertEqual(completed.returncode, 1)
            self.assertIn("did not pass", completed.stderr)
            self.assertFalse(rejected_profile.exists())
            self.assertFalse(rejected_receipt.exists())

            manifest = json.loads(
                fixture["manifest"].read_text(encoding="utf-8")
            )
            manifest["candidate"]["parameters"]["joint_candidate"] = 0.5
            fixture["manifest"].write_text(json.dumps(manifest),
                                            encoding="utf-8")
            invalid_output = root / "invalid-result.json"
            completed = subprocess.run(
                select_v2_command(fixture, invalid_output), check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            self.assertEqual(completed.returncode, 1)
            self.assertIn("candidate point", completed.stderr)
            self.assertFalse(invalid_output.exists())

    def test_v2_checked_harmonic_candidate_keeps_pitch_and_decay_gates(self):
        with tempfile.TemporaryDirectory(prefix="hwa v2 checked harmonic ") as text:
            root = Path(text)
            fixture = make_v2_fixture(root, expected_loss=0.2)
            write_fake_checked_analyzer(fixture["analyzer"])
            manifest = json.loads(
                fixture["manifest"].read_text(encoding="utf-8")
            )
            for objective in manifest["objectives"]:
                reference = fixture["references"][objective["split"]]
                objective.update({
                    "expected_hz": 220.0,
                    "kind": "checked-note-harmonic-decay",
                    "reference_sha256": MODULE.sha256(reference),
                    "scale": 1.0,
                })
            manifest["selection"].update({
                "max_candidate_harmonic_mean_error_octaves": 0.75,
                "max_candidate_harmonic_maximum_error_octaves": 1.5,
                "minimum_candidate_harmonic_count": 4,
            })
            fixture["manifest"].write_text(
                json.dumps(manifest), encoding="utf-8"
            )
            output = root / "checked-harmonic-result.json"
            completed = subprocess.run(
                select_v2_command(fixture, output), check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            result = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["method_versions"], {
            "selection": MODULE.VERIFY_CANDIDATE_METHOD_VERSION,
            "isolated_note": MODULE.CHECKED_NOTE_METHOD_VERSION,
            "checked_harmonic_decay":
                MODULE.CHECKED_HARMONIC_DECAY_METHOD_VERSION,
        })
        self.assertTrue(result["gates"]["passed"])
        for row in result["gates"]["objectives"]:
            self.assertTrue(row["checked_note_valid"])
            self.assertTrue(row["checked_harmonic_decay_valid"])
            self.assertEqual(row["minimum_valid_harmonic_count"], 4)
            self.assertEqual(
                row["maximum_mean_absolute_t60_error_octaves"], 0.75
            )
            self.assertEqual(row["maximum_harmonic_error_octaves"], 1.5)

    def test_v2_write_profile_rechecks_gates_and_writes_v2_receipt(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            fixture = make_v2_fixture(root)
            fit_path = root / "passing-result.json"
            completed = subprocess.run(
                select_v2_command(fixture, fit_path), check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            fit = json.loads(fit_path.read_text(encoding="utf-8"))
            self.assertEqual(fit["status"], "pass")
            self.assertTrue(fit["gates"]["passed"])
            self.assertEqual(fit["chosen_point_id"], 2)
            self.assertEqual(fit["chosen_parameters"],
                             {"joint_candidate": 1})
            self.assertEqual(set(fit["losses"]["candidate"]),
                             {"fit", "check", "audit"})
            self.assertEqual(len(fit["profile_changes"]), 4)

            source_before = fixture["profile"].read_bytes()
            changing_adapter = root / "changing-adapter.py"
            changing_adapter.write_text(
                "#!/usr/bin/env python3\n"
                "import json, sys\n"
                "json.load(open(sys.argv[2], encoding='utf-8'))\n"
                "with open(__file__, 'a', encoding='utf-8') as out:\n"
                "    out.write('# changed\\n')\n",
                encoding="utf-8",
            )
            changing_adapter.chmod(0o755)
            changed_output = root / "changed-adapter-profile.json"
            changed_receipt = root / "changed-adapter-receipt.json"
            completed = subprocess.run([
                sys.executable, str(TOOL), "write-profile",
                "--manifest", str(fixture["manifest"]),
                "--fit", str(fit_path),
                "--source", str(fixture["profile"]),
                "--adapter", str(changing_adapter),
                "--output", str(changed_output),
                "--receipt", str(changed_receipt),
            ], check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
               text=True)
            self.assertEqual(completed.returncode, 1)
            self.assertIn("does not match", completed.stderr)
            self.assertFalse(changed_output.exists())
            self.assertFalse(changed_receipt.exists())

            adapter = fixture["adapter"]
            output = root / "updated-profile.json"
            receipt = root / "write-receipt.json"
            completed = subprocess.run([
                sys.executable, str(TOOL), "write-profile",
                "--manifest", str(fixture["manifest"]),
                "--fit", str(fit_path),
                "--source", str(fixture["profile"]),
                "--adapter", str(adapter),
                "--output", str(output), "--receipt", str(receipt),
            ], check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
               text=True)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            profile = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(
                [row["loss_time_constant_seconds"] for row in profile["strings"]],
                [1.0, 1.0, 0.75, 1.0],
            )
            written = json.loads(receipt.read_text(encoding="utf-8"))
            self.assertEqual(written["schema_version"], 2)
            self.assertEqual(written["changes"], fit["profile_changes"])
            self.assertEqual(written["selector_sha256"], MODULE.sha256(TOOL))
            self.assertEqual(written["joint_gates"], fit["gates"])
            self.assertEqual(len(written["source_fit_results"]), 4)
            self.assertEqual(fixture["profile"].read_bytes(), source_before)

            tampered = json.loads(fit_path.read_text(encoding="utf-8"))
            tampered["gates"]["objectives"][0]["passed"] = False
            tampered_path = root / "tampered-result.json"
            tampered_path.write_text(json.dumps(tampered), encoding="utf-8")
            bad_output = root / "bad-profile.json"
            bad_receipt = root / "bad-receipt.json"
            completed = subprocess.run([
                sys.executable, str(TOOL), "write-profile",
                "--manifest", str(fixture["manifest"]),
                "--fit", str(tampered_path),
                "--source", str(fixture["profile"]),
                "--adapter", str(adapter),
                "--output", str(bad_output), "--receipt", str(bad_receipt),
            ], check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
               text=True)
            self.assertEqual(completed.returncode, 1)
            self.assertIn("gate data", completed.stderr)
            self.assertFalse(bad_output.exists())
            self.assertFalse(bad_receipt.exists())

    def test_fit_only_checked_note_harmonic_selection_uses_one_fit_binding(
            self):
        with tempfile.TemporaryDirectory(prefix="hwa checked fit only ") as text:
            root = Path(text)
            reference = root / "fit-reference.wav"
            baseline_audio = root / "baseline-model.wav"
            candidate_audio = root / "candidate-model.wav"
            for path, tau in ((reference, 0.4), (baseline_audio, 0.8),
                              (candidate_audio, 0.4)):
                write_decay(path, tau)
            analyzer = root / "analyzer"
            write_fake_checked_analyzer(analyzer)
            manifest = {
                "schema": "hwa-instrument-fit", "schema_version": 1,
                "adapter_id": "fit-only-checked-harmonics",
                "parameters": [{
                    "id": "tau", "unit": "seconds", "minimum": 0.1,
                    "maximum": 1.0, "baseline": 0.8,
                    "profile_paths": [["tau"]],
                }],
                "objectives": [{
                    "id": "fit_harmonics",
                    "kind": "checked-note-harmonic-decay",
                    "case": "fit", "reference_binding": "fit_ref",
                    "reference_sha256": MODULE.sha256(reference),
                    "resource_id": "model.final", "expected_hz": 220.0,
                    "split": "fit", "weight": 1.0, "scale": 1.0,
                }],
                "selection": {
                    "mode": "fit-only", "check_weight": 0.0,
                    "max_check_loss_increase": 0.0,
                    "max_candidate_worst_harm": 0.0,
                    "max_candidate_harmonic_mean_error_octaves": 0.75,
                    "max_candidate_harmonic_maximum_error_octaves": 1.5,
                },
            }
            experiment = {
                "command": "experiment", "schema_version": 10,
                "parameters": [{
                    "id": 1, "name": "tau", "unit": "seconds",
                    "minimum": 0.1, "maximum": 1.0, "baseline": 0.8,
                }],
                "cases": [{
                    "id": 1, "name": "fit", "split": "fit", "weight": 1,
                }],
                "responses": [],
                "points": [
                    {"id": 1, "key": "a" * 64, "baseline": True},
                    {"id": 2, "key": "b" * 64, "baseline": False},
                ],
                "values": [{
                    "id": 1, "point_id": 2, "parameter_id": 1,
                    "value": 0.4,
                }],
                "jobs": [
                    {"id": 1, "key": "1" * 64,
                     "point_id": 1, "case_id": 1},
                    {"id": 2, "key": "2" * 64,
                     "point_id": 2, "case_id": 1},
                ],
                "artifacts": [
                    {"id": 1, "job_id": 1, "resource_id": "model.final",
                     "artifact": {"path": baseline_audio.name,
                                  "sha256": MODULE.sha256(baseline_audio)},
                     "file_bytes": baseline_audio.stat().st_size,
                     "kind": "stem"},
                    {"id": 2, "job_id": 2, "resource_id": "model.final",
                     "artifact": {"path": candidate_audio.name,
                                  "sha256": MODULE.sha256(candidate_audio)},
                     "file_bytes": candidate_audio.stat().st_size,
                     "kind": "stem"},
                ],
                "candidates": [],
            }
            manifest_path = root / "fit.json"
            experiment_path = root / "experiment.json"
            profile_path = root / "profile.json"
            output = root / "result.json"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            experiment_path.write_text(json.dumps(experiment), encoding="utf-8")
            profile_path.write_text('{"tau":0.8}', encoding="utf-8")
            passed = MODULE.select(types.SimpleNamespace(
                manifest=manifest_path, experiment=experiment_path,
                analyzer=analyzer, profile=profile_path,
                bind=["fit_ref=" + str(reference)], output=output,
            ))
            result = json.loads(output.read_text(encoding="utf-8"))
            self.assertTrue(passed)
            self.assertEqual(result["selection_mode"], "fit-only")
            self.assertEqual(result["chosen_point_id"], 2)
            self.assertEqual(result["method_versions"], {
                "selection": MODULE.FIT_ONLY_SELECTION_METHOD_VERSION,
                "isolated_note": MODULE.CHECKED_NOTE_METHOD_VERSION,
                "checked_harmonic_decay":
                    MODULE.CHECKED_HARMONIC_DECAY_METHOD_VERSION,
            })
            chosen = next(row for row in result["points"]
                          if row["point_id"] == 2)
            self.assertEqual(chosen["check_loss"], 0.0)
            self.assertTrue(
                chosen["evidence"][0]["checked_note_valid"])
            self.assertTrue(
                chosen["evidence"][0]["checked_harmonic_decay_valid"])
            self.assertLess(chosen["fit_loss"], 0.21)

            invalid_model = root / "invalid-model.wav"
            write_decay(invalid_model, 0.02)
            invalid_evidence = MODULE.checked_note_harmonic_decay(
                analyzer, reference, invalid_model, 220.0,
                MODULE.sha256(analyzer), MODULE.sha256(reference),
                MODULE.sha256(invalid_model),
            )
            self.assertFalse(invalid_evidence["checked_note_valid"])
            self.assertFalse(
                invalid_evidence["checked_harmonic_decay_valid"])
            self.assertEqual(
                invalid_evidence["rms_t60_error_octaves"],
                MODULE.INVALID_HARMONIC_LOSS_OCTAVES,
            )
            self.assertFalse(MODULE.v1_objective_passes_absolute_limits(
                {"loss": invalid_evidence["rms_t60_error_octaves"],
                 **invalid_evidence},
                manifest["selection"],
            ))
            with self.assertRaisesRegex(MODULE.FitError, "analyzer hash"):
                MODULE.checked_note_harmonic_decay(
                    analyzer, reference, candidate_audio, 220.0,
                    "0" * 64, MODULE.sha256(reference),
                    MODULE.sha256(candidate_audio),
                )

            manifest["objectives"][0]["reference_sha256"] = "0" * 64
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            rejected_output = root / "wrong-reference-result.json"
            with self.assertRaisesRegex(MODULE.FitError, "wrong hash"):
                MODULE.select(types.SimpleNamespace(
                    manifest=manifest_path, experiment=experiment_path,
                    analyzer=analyzer, profile=profile_path,
                    bind=["fit_ref=" + str(reference)],
                    output=rejected_output,
                ))
            self.assertFalse(rejected_output.exists())

            manifest["objectives"][0]["reference_sha256"] = MODULE.sha256(
                reference)
            manifest["objectives"][0]["split"] = "check"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(MODULE.FitError, "fit-only"):
                MODULE.fit_manifest(manifest_path)

    def test_passive_decay_reader_enforces_byte_and_frame_limits(self):
        with tempfile.TemporaryDirectory() as text:
            path = Path(text) / "decay.wav"
            write_decay(path, 0.40, duration=0.5)
            with mock.patch.object(
                    MODULE, "MAX_PASSIVE_WAVE_BYTES", path.stat().st_size - 1):
                with self.assertRaisesRegex(MODULE.FitError, "byte limit"):
                    MODULE.read_pcm_wave(path)
            with mock.patch.object(MODULE, "MAX_PASSIVE_FRAMES", 10):
                with self.assertRaisesRegex(MODULE.FitError, "frame limit"):
                    MODULE.read_pcm_wave(path)
            with self.assertRaisesRegex(MODULE.FitError, "hash changed"):
                MODULE.read_pcm_wave(path, "0" * 64)

    def test_new_json_publish_is_exclusive_under_a_race(self):
        with tempfile.TemporaryDirectory() as text:
            output = Path(text) / "result.json"

            def competing_publish(_source, destination):
                Path(destination).write_text("other\n", encoding="utf-8")
                raise FileExistsError(destination)

            with mock.patch.object(MODULE.os, "link",
                                   side_effect=competing_publish):
                with self.assertRaisesRegex(MODULE.FitError,
                                            "already exists"):
                    MODULE.write_new_json(output, {"wanted": True})
            self.assertEqual(output.read_text(encoding="utf-8"), "other\n")

    def test_passive_decay_matches_16_and_24_bit_independent_tails(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            reference = root / "reference.wav"
            exact = root / "exact.wav"
            close = root / "close.wav"
            far = root / "far.wav"
            write_decay(reference, 0.40, width=3, lead=0.08, extensible=True)
            write_decay(exact, 0.40, width=2, lead=0.31,
                        gain=0.24, polarity=-1.0)
            write_decay(close, 0.46, width=3, lead=0.18, gain=0.5)
            write_decay(far, 0.80, width=2, lead=0.04, gain=0.9)
            exact_result = MODULE.run_passive_decay(reference, exact)
            close_result = MODULE.run_passive_decay(reference, close)
            far_result = MODULE.run_passive_decay(reference, far)
        self.assertLess(exact_result["shape_rmse_db"], 0.15)
        self.assertLess(close_result["shape_rmse_db"],
                        far_result["shape_rmse_db"])
        self.assertAlmostEqual(exact_result["reference_slope_db_per_second"],
                               exact_result["model_slope_db_per_second"], delta=0.2)
        self.assertAlmostEqual(exact_result["reference_t60_seconds"],
                               60.0 / (20.0 / math.log(10.0) / 0.40), delta=0.03)

    def test_passive_decay_accepts_a_clean_curved_string_tail(self):
        with tempfile.TemporaryDirectory() as text:
            path = Path(text) / "curved.wav"
            write_decay(path, 0.10, duration=7.0, secondary_tau=2.0,
                        secondary_mix=0.05)
            result = MODULE.run_passive_decay(path, path)
        self.assertLess(result["shape_rmse_db"], 0.01)
        self.assertGreater(result["reference_dynamic_range_db"], 30.0)
        self.assertGreater(result["reference_line_residual_db"], 2.5)

    def test_passive_decay_accepts_smooth_modal_beating(self):
        with tempfile.TemporaryDirectory() as text:
            path = Path(text) / "beating.wav"
            write_decay(path, 0.40, duration=3.0, modulation_db=6.0,
                        modulation_period=0.25)
            result = MODULE.run_passive_decay(path, path)
        self.assertLess(result["shape_rmse_db"], 0.01)
        self.assertGreater(result["reference_dynamic_range_db"], 30.0)
        self.assertGreater(result["reference_line_residual_db"], 4.0)

    def test_passive_decay_rejects_a_slower_spectral_second_excitation(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            reference = root / "reference.wav"
            changed = root / "spectral-second.wav"
            write_decay(reference, 0.40)
            write_decay(changed, 0.40, second_onset=0.75,
                        second_frequency=1760.0, second_gain=0.30,
                        second_attack=0.040)
            with self.assertRaisesRegex(MODULE.FitError, "second onset"):
                MODULE.run_passive_decay(reference, changed)

    def test_passive_decay_finds_a_low_gain_attack_after_noisy_preroll(self):
        with tempfile.TemporaryDirectory() as text:
            path = Path(text) / "quiet-after-noise.wav"
            write_decay(path, 0.40, width=3, lead=0.30, duration=2.5,
                        gain=0.01, pre_noise=0.0006)
            result = MODULE.run_passive_decay(path, path)
        self.assertLess(result["shape_rmse_db"], 0.01)
        self.assertGreater(result["reference_dynamic_range_db"], 30.0)

    def test_passive_decay_uses_the_reference_span_for_short_models(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            reference = root / "reference.wav"
            fast = root / "fast.wav"
            closer = root / "closer.wav"
            write_decay(reference, 0.10, duration=7.0, secondary_tau=2.0,
                        secondary_mix=0.05)
            write_decay(fast, 0.125)
            write_decay(closer, 0.25)
            fast_result = MODULE.run_passive_decay(reference, fast)
            closer_result = MODULE.run_passive_decay(reference, closer)
        self.assertLess(closer_result["shape_rmse_db"],
                        fast_result["shape_rmse_db"])
        self.assertAlmostEqual(
            fast_result["comparison_support_seconds"],
            fast_result["reference_support_seconds"],
        )
        self.assertGreater(fast_result["model_support_shortfall_seconds"],
                           closer_result["model_support_shortfall_seconds"])

    def test_passive_decay_rejects_an_irregular_tail_with_wide_range(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            reference = root / "reference.wav"
            irregular = root / "irregular.wav"
            write_decay(reference, 0.40)
            write_decay(irregular, 0.80, duration=5.0, gain=0.5,
                        modulation_db=8.0, modulation_period=1.5)
            with self.assertRaisesRegex(MODULE.FitError, "too irregular"):
                MODULE.run_passive_decay(reference, irregular)

    def test_passive_decay_shape_detects_the_high_frequency_decay(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            reference = root / "reference.wav"
            matching = root / "matching.wav"
            wrong = root / "wrong.wav"
            write_spectral_decay(reference, 0.80, 0.18)
            write_spectral_decay(matching, 0.80, 0.18, lead=0.21)
            write_spectral_decay(wrong, 0.80, 0.80, lead=0.17)
            matched = MODULE.run_passive_decay(reference, matching)
            mismatched = MODULE.run_passive_decay(reference, wrong)
        self.assertLess(matched["spectral_change_rmse_db"], 0.05)
        self.assertGreater(mismatched["spectral_change_rmse_db"], 3.0)
        self.assertGreater(
            mismatched["combined_shape_rmse_db"],
            mismatched["shape_rmse_db"],
        )
        self.assertEqual(
            MODULE.passive_decay_loss("passive-decay", mismatched, 3.0),
            mismatched["shape_rmse_db"] / 3.0,
        )
        self.assertEqual(
            MODULE.passive_decay_loss(
                "passive-decay-shape", mismatched, 3.0
            ),
            mismatched["combined_shape_rmse_db"] / 3.0,
        )

    def test_harmonic_decay_compares_each_partial(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            reference = root / "reference.wav"
            matching = root / "matching.wav"
            wrong = root / "wrong.wav"
            write_harmonic_decay(reference, 110.0, [0.8, 0.6, 0.4, 0.3])
            write_harmonic_decay(matching, 110.0, [0.8, 0.6, 0.4, 0.3],
                                 lead=0.23)
            write_harmonic_decay(wrong, 110.0, [0.8, 0.6, 0.8, 0.8],
                                 lead=0.17)
            matched = MODULE.run_harmonic_decay(
                reference, matching, 110.0, 4
            )
            mismatched = MODULE.run_harmonic_decay(
                reference, wrong, 110.0, 4
            )
        self.assertEqual(matched["method_version"], "harmonic-decay-v1")
        self.assertEqual(matched["comparison"]["valid_harmonic_count"], 4)
        self.assertLess(
            matched["comparison"]["mean_absolute_t60_error_octaves"],
            0.01,
        )
        self.assertGreater(
            mismatched["comparison"]["mean_absolute_t60_error_octaves"],
            0.30,
        )

    def test_passive_decay_rejects_bad_tails(self):
        cases = [
            ("short", {"duration": 0.20}, "too short"),
            ("flat", {"tau": 10.0, "duration": 1.0}, "dynamic range"),
            ("noise", {"noise": 0.12}, "noisy"),
            ("second", {"second_onset": 0.75}, "second onset"),
        ]
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            reference = root / "reference.wav"
            write_decay(reference, 0.40)
            for name, options, message in cases:
                path = root / (name + ".wav")
                tau = options.pop("tau", 0.40)
                write_decay(path, tau, **options)
                with self.subTest(name=name):
                    with self.assertRaisesRegex(MODULE.FitError, message):
                        MODULE.run_passive_decay(reference, path)

    def test_manifest_accepts_passive_decay_objective(self):
        manifest = {
            "schema": "hwa-instrument-fit", "schema_version": 1,
            "adapter_id": "decay",
            "parameters": [{
                "id": "tau", "unit": "seconds", "minimum": 0.1,
                "maximum": 1.0, "baseline": 0.25,
                "profile_paths": [["tau"]],
            }],
            "objectives": [
                {"id": "fit", "kind": "passive-decay", "case": "fit",
                 "reference_binding": "fit_ref", "resource_id": "model.final",
                 "split": "fit", "weight": 1, "scale": 3},
                {"id": "check", "kind": "passive-decay", "case": "check",
                 "reference_binding": "check_ref", "resource_id": "model.final",
                 "split": "check", "weight": 1, "scale": 3},
            ],
            "selection": {"check_weight": 1, "max_check_loss_increase": 0.1,
                          "max_candidate_worst_harm": 1},
        }
        with tempfile.TemporaryDirectory() as text:
            path = Path(text) / "fit.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            result = MODULE.fit_manifest(path)
        self.assertEqual(result["objectives"][0]["kind"], "passive-decay")

        manifest["objectives"][0]["kind"] = "passive-decay-shape"
        manifest["selection"].update({
            "max_candidate_level_rmse_db": 6.0,
            "max_candidate_spectral_rmse_db": 6.0,
        })
        with tempfile.TemporaryDirectory() as text:
            path = Path(text) / "fit.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            result = MODULE.fit_manifest(path)
        self.assertEqual(
            result["objectives"][0]["kind"], "passive-decay-shape"
        )
        self.assertEqual(
            MODULE.passive_method_versions(result["objectives"]),
            {
                "passive_decay": MODULE.PASSIVE_DECAY_METHOD_VERSION,
                "passive_decay_shape":
                    MODULE.PASSIVE_DECAY_SHAPE_METHOD_VERSION,
            },
        )

        manifest["objectives"][0].update({
            "kind": "harmonic-decay",
            "fundamental_hz": 220.0,
            "harmonic_count": 4,
        })
        manifest["selection"].pop("max_candidate_level_rmse_db")
        manifest["selection"].pop("max_candidate_spectral_rmse_db")
        manifest["selection"].update({
            "max_candidate_harmonic_mean_error_octaves": 0.75,
            "max_candidate_harmonic_maximum_error_octaves": 1.5,
        })
        with tempfile.TemporaryDirectory() as text:
            path = Path(text) / "fit.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            result = MODULE.fit_manifest(path)
        self.assertEqual(result["objectives"][0]["kind"], "harmonic-decay")
        self.assertEqual(
            MODULE.passive_method_versions(result["objectives"])[
                "harmonic_decay"],
            MODULE.HARMONIC_DECAY_METHOD_VERSION,
        )

    def test_scalar_absolute_limits_are_complete_and_check_decay_facts(self):
        selection = {
            "max_candidate_loss": 2.0,
            "minimum_candidate_t60_ratio": 0.5,
            "maximum_candidate_t60_ratio": 2.0,
            "minimum_candidate_support_ratio": 0.5,
        }
        evidence = {
            "loss": 1.5,
            "reference_t60_seconds": 4.0,
            "model_t60_seconds": 6.0,
            "reference_support_seconds": 2.0,
            "model_support_seconds": 1.5,
        }
        self.assertTrue(MODULE.v1_objective_passes_absolute_limits(
            evidence, selection))

        harmonic_selection = {
            **selection,
            "max_candidate_harmonic_mean_error_octaves": 0.75,
            "max_candidate_harmonic_maximum_error_octaves": 1.5,
        }
        harmonic_evidence = {
            "loss": 1.0,
            "valid_harmonic_count": 4,
            "mean_absolute_t60_error_octaves": 0.70,
            "maximum_absolute_t60_error_octaves": 1.40,
        }
        self.assertTrue(MODULE.v1_objective_passes_absolute_limits(
            harmonic_evidence, harmonic_selection
        ))
        harmonic_evidence["maximum_absolute_t60_error_octaves"] = 1.51
        self.assertFalse(MODULE.v1_objective_passes_absolute_limits(
            harmonic_evidence, harmonic_selection
        ))
        failures = {
            "loss": {"loss": 2.01},
            "short t60": {"model_t60_seconds": 1.99},
            "long t60": {"model_t60_seconds": 8.01},
            "short support": {"model_support_seconds": 0.99},
        }
        for name, change in failures.items():
            changed = {**evidence, **change}
            with self.subTest(name=name):
                self.assertFalse(MODULE.v1_objective_passes_absolute_limits(
                    changed, selection))

        manifest = {
            "schema": "hwa-instrument-fit", "schema_version": 1,
            "adapter_id": "decay",
            "parameters": [{
                "id": "tau", "unit": "seconds", "minimum": 0.1,
                "maximum": 1.0, "baseline": 0.25,
                "profile_paths": [["tau"]],
            }],
            "objectives": [
                {"id": "fit", "kind": "passive-decay", "case": "fit",
                 "reference_binding": "fit_ref", "resource_id": "model.final",
                 "split": "fit", "weight": 1, "scale": 3},
                {"id": "check", "kind": "passive-decay", "case": "check",
                 "reference_binding": "check_ref", "resource_id": "model.final",
                 "split": "check", "weight": 1, "scale": 3},
            ],
            "selection": {
                "check_weight": 1, "max_check_loss_increase": 0.1,
                "max_candidate_worst_harm": 1,
                "max_candidate_loss": 2.0,
            },
        }
        with tempfile.TemporaryDirectory() as text:
            path = Path(text) / "fit.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(
                    MODULE.FitError, "every absolute candidate limit"):
                MODULE.fit_manifest(path)

    def test_shape_limits_keep_level_and_spectral_errors_separate(self):
        selection = {
            "max_candidate_loss": math.sqrt(8.0),
            "max_candidate_level_rmse_db": 6.0,
            "max_candidate_spectral_rmse_db": 6.0,
            "minimum_candidate_t60_ratio": 0.5,
            "maximum_candidate_t60_ratio": 2.0,
            "minimum_candidate_support_ratio": 0.5,
        }
        evidence = {
            "loss": math.hypot(1.9, 1.4),
            "shape_rmse_db": 5.7,
            "spectral_change_rmse_db": 4.2,
            "reference_t60_seconds": 4.0,
            "model_t60_seconds": 4.5,
            "reference_support_seconds": 2.0,
            "model_support_seconds": 1.5,
        }
        self.assertTrue(MODULE.v1_objective_passes_absolute_limits(
            evidence, selection
        ))
        for field in ("shape_rmse_db", "spectral_change_rmse_db"):
            changed = {**evidence, field: 6.01}
            with self.subTest(field=field):
                self.assertFalse(MODULE.v1_objective_passes_absolute_limits(
                    changed, selection
                ))

    def test_manifest_requires_parameter_unit_and_baseline(self):
        manifest = {
            "schema": "hwa-instrument-fit", "schema_version": 1,
            "adapter_id": "decay",
            "parameters": [{
                "id": "tau", "unit": "seconds", "minimum": 0.1,
                "maximum": 1.0, "baseline": 0.25,
                "profile_paths": [["tau"]],
            }],
            "objectives": [
                {"id": "fit", "kind": "experiment-gap", "response": "r",
                 "split": "fit", "weight": 1, "scale": 1},
                {"id": "check", "kind": "experiment-gap", "response": "r",
                 "split": "check", "weight": 1, "scale": 1},
            ],
            "selection": {"check_weight": 1,
                          "max_check_loss_increase": 0.1,
                          "max_candidate_worst_harm": 1},
        }
        with tempfile.TemporaryDirectory() as text:
            path = Path(text) / "fit.json"
            for field in ("unit", "baseline"):
                changed = json.loads(json.dumps(manifest))
                del changed["parameters"][0][field]
                path.write_text(json.dumps(changed), encoding="utf-8")
                with self.subTest(field=field):
                    with self.assertRaisesRegex(MODULE.FitError, field):
                        MODULE.fit_manifest(path)

    def test_select_uses_passive_decay_for_fit_and_check(self):
        manifest = {
            "schema": "hwa-instrument-fit", "schema_version": 1,
            "adapter_id": "decay",
            "parameters": [{
                "id": "tau", "unit": "seconds", "minimum": 0.1,
                "maximum": 1.0, "baseline": 0.8,
                "profile_paths": [["tau"]],
            }],
            "objectives": [
                {"id": "fit", "kind": "passive-decay", "case": "fit",
                 "reference_binding": "fit_ref", "resource_id": "model.final",
                 "split": "fit", "weight": 1, "scale": 3},
                {"id": "check", "kind": "passive-decay", "case": "check",
                 "reference_binding": "check_ref", "resource_id": "model.final",
                 "split": "check", "weight": 1, "scale": 3},
            ],
            "selection": {"check_weight": 1, "max_check_loss_increase": 0.1,
                          "max_candidate_worst_harm": 1},
        }
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            fit_reference = root / "fit-reference.wav"
            check_reference = root / "check-reference.wav"
            write_decay(fit_reference, 0.40, width=3, lead=0.07)
            write_decay(check_reference, 0.40, width=2, lead=0.23,
                        gain=0.35, polarity=-1.0)
            artifacts = []
            jobs = []
            job_id = 0
            for point_id, tau in ((1, 0.80), (2, 0.40)):
                for case_id in (1, 2):
                    job_id += 1
                    audio = root / ("model-%d-%d.wav" % (point_id, case_id))
                    write_decay(audio, tau, lead=0.13 + case_id * 0.02)
                    jobs.append({"id": job_id, "key": str(job_id) * 64,
                                 "point_id": point_id, "case_id": case_id})
                    artifacts.append({
                        "id": job_id, "job_id": job_id,
                        "resource_id": "model.final",
                        "artifact": {"path": audio.name,
                                     "sha256": MODULE.sha256(audio)},
                        "file_bytes": audio.stat().st_size, "kind": "stem",
                    })
            experiment = {
                "command": "experiment", "schema_version": 10,
                "parameters": [{"id": 1, "name": "tau", "unit": "seconds",
                                "minimum": 0.1, "maximum": 1.0,
                                "baseline": 0.8}],
                "cases": [{"id": 1, "name": "fit", "split": "fit", "weight": 1},
                          {"id": 2, "name": "check", "split": "check", "weight": 1}],
                "responses": [],
                "points": [{"id": 1, "key": "a" * 64, "baseline": True},
                           {"id": 2, "key": "b" * 64, "baseline": False}],
                "values": [{"id": 1, "point_id": 2, "parameter_id": 1,
                            "value": 0.4}],
                "jobs": jobs, "artifacts": artifacts, "candidates": [],
            }
            manifest_path = root / "fit.json"
            experiment_path = root / "experiment.json"
            profile_path = root / "profile.json"
            analyzer_path = root / "analyzer"
            output_path = root / "result.json"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            experiment_path.write_text(json.dumps(experiment), encoding="utf-8")
            profile_path.write_text('{"tau": 0.8}', encoding="utf-8")
            analyzer_path.write_text("not used", encoding="utf-8")
            MODULE.select(types.SimpleNamespace(
                manifest=manifest_path, experiment=experiment_path,
                analyzer=analyzer_path, profile=profile_path,
                bind=["fit_ref=" + str(fit_reference),
                      "check_ref=" + str(check_reference)],
                output=output_path,
            ))
            result = json.loads(output_path.read_text(encoding="utf-8"))
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["chosen_point_id"], 2)
        self.assertEqual(result["method_versions"], {
            "selection": MODULE.SELECTION_METHOD_VERSION,
            "passive_decay": MODULE.PASSIVE_DECAY_METHOD_VERSION,
        })
        self.assertEqual(result["selector_sha256"], MODULE.sha256(TOOL))
        chosen = next(row for row in result["points"] if row["point_id"] == 2)
        self.assertEqual({row["objective"] for row in chosen["evidence"]},
                         {"fit", "check"})
        self.assertIn("reference_slope_db_per_second", chosen["evidence"][0])

    def test_select_uses_harmonic_decay_and_caches_references(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            fixture = make_v1_fixture(root, max_candidate_loss=2.0)
            manifest = json.loads(
                fixture["manifest"].read_text(encoding="utf-8")
            )
            for objective in manifest["objectives"]:
                objective.update({
                    "kind": "harmonic-decay",
                    "fundamental_hz": 110.0,
                    "harmonic_count": 4,
                    "scale": 0.5,
                })
            manifest["selection"].update({
                "max_candidate_harmonic_mean_error_octaves": 0.75,
                "max_candidate_harmonic_maximum_error_octaves": 1.5,
            })
            fixture["manifest"].write_text(
                json.dumps(manifest), encoding="utf-8"
            )
            profile = [{
                "harmonic": index,
                "center_hz": 110.0 * index,
                "status": "valid",
                "t60_seconds": 0.8 - 0.1 * index,
            } for index in range(1, 5)]
            output = root / "harmonic-result.json"
            with mock.patch.object(
                    MODULE, "harmonic_decay_profile",
                    return_value=profile) as measured:
                passed = MODULE.select(types.SimpleNamespace(
                    manifest=fixture["manifest"],
                    experiment=fixture["experiment"],
                    analyzer=fixture["analyzer"],
                    profile=fixture["profile"],
                    bind=[
                        "c_fit=" + str(fixture["references"]["fit"]),
                        "c_check=" + str(fixture["references"]["check"]),
                    ],
                    output=output,
                ))
            result = json.loads(output.read_text(encoding="utf-8"))
        self.assertTrue(passed)
        self.assertEqual(result["chosen_point_id"], 2)
        self.assertEqual(result["method_versions"]["harmonic_decay"],
                         MODULE.HARMONIC_DECAY_METHOD_VERSION)
        self.assertEqual(measured.call_count, 6)
        chosen = next(row for row in result["points"]
                      if row["point_id"] == 2)
        self.assertTrue(all(
            row["valid_harmonic_count"] == 4
            for row in chosen["evidence"]
        ))

    def test_saved_experiment_reader(self):
        rows = [
            "HWA_EXPERIMENT,1",
            "META,parameter_count,1,parameters",
            "META,case_count,1,cases",
            "META,response_count,1,responses",
            "META,point_count,1,points",
            "META,value_count,1,values",
            "META,job_count,1,jobs",
            "META,artifact_count,1,artifacts",
            "META,candidate_count,1,candidates",
            "PARAMETER,1,body,ratio,0,1,0.5,0,0",
            "CASE,1,fit,fit,1",
            "RESPONSE,1,final.rms,final,rms_dbfs,0",
            "POINT,1," + "1" * 64 + ",1",
            "VALUE,1,1,1,0.5",
            "JOB,1," + "2" * 64 + ",1,1,0,1,00," + "3" * 64 + "," + "4" * 64 + ",1,1",
            "ARTIFACT,1,1,model.final," + b"jobs/a/model.wav".hex() + "," + "5" * 64 + ",1,stem",
            "CANDIDATE,1,1,1,fit,available,0.25,0,0,1,0,1",
        ]
        with tempfile.TemporaryDirectory() as text:
            path = Path(text) / "result.hwa-experiment"
            path.write_bytes(("\r\n".join(rows) + "\r\n").encode("ascii"))
            result = MODULE.load_saved_experiment(path)
        self.assertEqual(result["parameters"][0]["name"], "body")
        self.assertEqual(result["artifacts"][0]["artifact"]["path"],
                         "jobs/a/model.wav")
        self.assertEqual(result["candidates"][0]["mean_gap"], 0.25)

    def test_render_only_parameter_cannot_write_profile(self):
        manifest = {
            "schema": "hwa-instrument-fit", "schema_version": 1,
            "adapter_id": "render-only",
            "parameters": [{
                "id": "body", "unit": "ratio", "minimum": 0,
                "maximum": 1, "baseline": 0.5, "profile_paths": [],
            }],
            "objectives": [
                {"id": "fit", "kind": "experiment-gap",
                 "response": "r", "split": "fit", "weight": 1, "scale": 1},
                {"id": "check", "kind": "experiment-gap",
                 "response": "r", "split": "check", "weight": 1, "scale": 1},
            ],
            "selection": {"check_weight": 1, "max_check_loss_increase": 0,
                          "max_candidate_worst_harm": 1},
        }
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            manifest_path = root / "fit.json"
            profile_path = root / "profile.json"
            adapter_path = root / "adapter"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            profile_path.write_text("{}", encoding="utf-8")
            adapter_path.write_text("unused", encoding="utf-8")
            fit = {
                "schema": "hwa-instrument-fit-result", "schema_version": 1,
                "adapter_id": "render-only",
                "method_versions": {
                    "selection": MODULE.SELECTION_METHOD_VERSION,
                },
                "selector_sha256": "a" * 64,
                "fit_manifest_sha256": MODULE.sha256(manifest_path),
                "profile_sha256": MODULE.sha256(profile_path),
                "chosen_parameters": {"body": 0.6},
            }
            fit_path = root / "result.json"
            fit_path.write_text(json.dumps(fit), encoding="utf-8")
            arguments = types.SimpleNamespace(
                manifest=manifest_path, fit=fit_path, source=profile_path,
                adapter=adapter_path, output=root / "out.json",
                receipt=root / "receipt.json",
            )
            with self.assertRaisesRegex(MODULE.FitError, "does not map body"):
                MODULE.write_profile(arguments)
            self.assertFalse(arguments.output.exists())


if __name__ == "__main__":
    unittest.main()
