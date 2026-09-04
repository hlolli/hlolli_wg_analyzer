#!/usr/bin/env python3

import json
import hashlib
import importlib.util
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock
import wave


ROOT = Path(__file__).resolve().parents[1]
ADAPTER = ROOT / "adapters" / "hlolli_wg_double_bass"
BUILDER = ADAPTER / "build_manifest.py"
FIT = ADAPTER / "fit-passive-open-v1.json"
CONTRACT = ADAPTER / "reference-contract-v1.json"
V2_CONTRACT = ADAPTER / "fit-reference-contract-v2.json"
V2_FITS = {
    target: ADAPTER / "fit-passive-{}-v2.json".format(target[0])
    for target in ("e1", "a1", "d2", "g2")
}
D_FREQUENCY_FIT = ADAPTER / "fit-passive-d-frequency-v3.json"
PYTHON = Path(sys.executable).resolve()
MODULE_SPEC = importlib.util.spec_from_file_location(
    "double_bass_build_manifest", BUILDER)
assert MODULE_SPEC is not None and MODULE_SPEC.loader is not None
MODULE = importlib.util.module_from_spec(MODULE_SPEC)
MODULE_SPEC.loader.exec_module(MODULE)
FIT_TOOL = ROOT / "tools" / "instrument_fit.py"
FIT_MODULE_SPEC = importlib.util.spec_from_file_location(
    "double_bass_instrument_fit", FIT_TOOL)
assert FIT_MODULE_SPEC is not None and FIT_MODULE_SPEC.loader is not None
FIT_MODULE = importlib.util.module_from_spec(FIT_MODULE_SPEC)
FIT_MODULE_SPEC.loader.exec_module(FIT_MODULE)
ROWS = (
    ("a", "iowa2012-pizz-a-mf-open",
     "iowa2001-pizz-mf-open-a1-heldout",
     "iowa2001-pizz-mf-open-a1-heldout-48k-soxr"),
    ("d", "iowa2012-pizz-d-mf-open",
     "iowa2001-pizz-mf-open-d2-heldout",
     "iowa2001-pizz-mf-open-d2-heldout-48k-soxr"),
    ("e", "iowa2012-pizz-e-ff-open",
     "iowa2001-pizz-mf-open-e1-heldout",
     "iowa2001-pizz-mf-open-e1-heldout-48k-soxr"),
    ("g", "iowa2012-pizz-g-pp-open",
     "iowa2001-pizz-mf-open-g2-heldout",
     "iowa2001-pizz-mf-open-g2-heldout-48k-soxr"),
)


def strings(value):
    if isinstance(value, str):
        yield value
    elif isinstance(value, list):
        for item in value:
            yield from strings(item)
    elif isinstance(value, dict):
        for key, item in value.items():
            yield key
            yield from strings(item)


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def wave_file(path, rate, width, frames):
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(1)
        stream.setsampwidth(width)
        stream.setframerate(rate)
        stream.writeframes(b"".join(
            int((index % 201) - 100).to_bytes(width, "little", signed=True)
            for index in range(frames)))


def fake_ffmpeg(path, nonrepeatable=False):
    path.write_text(
        f"#!{PYTHON} -I\n"
        "import pathlib, sys, wave\n"
        "if sys.argv[1:] == ['-version']:\n"
        " print('ffmpeg version 8.1.2-test Copyright test'); raise SystemExit(0)\n"
        "a=sys.argv[1:]\n"
        "expected=['-nostdin','-hide_banner','-loglevel','error','-xerror','-i','{source}','-map_metadata','-1','-vn','-sn','-dn','-ac','1','-af','aresample=resampler=soxr:precision=33:dither_method=none','-ar','48000','-c:a','pcm_s24le','-fflags','+bitexact','-flags:a','+bitexact','-f','wav','{derived}']\n"
        "if len(a)!=len(expected) or any(w not in ('{source}','{derived}') and w!=v for w,v in zip(expected,a)):\n"
        " raise SystemExit(9)\n"
        "source=pathlib.Path(a[6]); output=pathlib.Path(a[-1])\n"
        "with wave.open(str(source),'rb') as r:\n"
        " if r.getnchannels()!=1 or r.getsampwidth()!=2 or r.getframerate()!=44100: raise SystemExit(8)\n"
        " n=r.getnframes()\n"
        "frames=(n*48000+22050)//44100\n"
        "offset=1 if " + repr(nonrepeatable) +
        " and '-second.wav' in output.name else 0\n"
        "with wave.open(str(output),'wb') as w:\n"
        " w.setnchannels(1); w.setsampwidth(3); w.setframerate(48000)\n"
        " w.writeframes(b''.join(int((i%201)-100+offset).to_bytes(3,'little',signed=True) for i in range(frames)))\n",
        encoding="utf-8")
    path.chmod(0o700)


def fake_analyzer(path):
    path.write_text(
        f"#!{PYTHON} -I\n"
        "import json, pathlib, sys, wave\n"
        "if sys.argv[1:] == ['--version']:\n"
        " print('hlolli-wg-analyzer 1.1.0-test'); raise SystemExit(0)\n"
        "a=sys.argv[1:]\n"
        "if a[:4] == ['--json','--max-bytes','67108864','inspect'] and len(a)==5:\n"
        " source=pathlib.Path(a[4])\n"
        " with wave.open(str(source),'rb') as r:\n"
        "  f={'container':'riff','encoding':'pcm','channels':r.getnchannels(),'sample_rate_hz':r.getframerate(),'bits_per_sample':r.getsampwidth()*8,'valid_bits_per_sample':r.getsampwidth()*8,'block_align':r.getnchannels()*r.getsampwidth(),'frames':r.getnframes()}\n"
        " print(json.dumps({'schema_version':2,'command':'inspect','file':{'path':str(source),'format':f}})); raise SystemExit(0)\n"
        "if len(a)==7 and a[:2]==['--json','isolated-note']:\n"
        " source=str(pathlib.Path(a[2]).absolute()); hz=float(a[4])\n"
        " print(json.dumps({'schema':'hwa-isolated-note','schema_version':1,'command':'isolated-note','method':'isolated-note-1','path':source,'expected_hz':hz,'requested_metrics':['pitch'],'pitch':{'valid':True,'cents':0.0}})); raise SystemExit(0)\n"
        "if len(a)==5 and a[:2]==['--json','harmonic-decay']:\n"
        " source=str(pathlib.Path(a[2]).absolute()); hz=float(a[4])\n"
        " profile={'path':source,'valid':True,'valid_band_count':4}\n"
        " print(json.dumps({'schema':'hwa-harmonic-decay','schema_version':1,'command':'harmonic-decay','method':'harmonic-decay-1','expected_hz':hz,'reference':profile,'model':None,'comparison':None})); raise SystemExit(0)\n"
        "raise SystemExit(9)\n",
        encoding="utf-8")
    path.chmod(0o700)


def fake_renderer_builder(path, marker):
    path.write_text(
        f"#!{PYTHON} -I\n"
        "import pathlib, sys\n"
        f"marker=pathlib.Path({str(marker)!r})\n"
        "a=sys.argv[1:]\n"
        "if len(a)!=5 or a[0]!='build' or a[1]!='--config' or a[3]!='--output':\n"
        " raise SystemExit(9)\n"
        "output=pathlib.Path(a[4])\n"
        "output.write_text('#!/usr/bin/python3 -I\\n',encoding='utf-8')\n"
        "output.chmod(0o700)\n"
        "marker.write_text('built\\n',encoding='utf-8')\n",
        encoding="utf-8",
    )
    path.chmod(0o700)


def joint_v2_fixture(root):
    repositories = [root / "analyzer repository", root / "plugin repository"]
    for repository in repositories:
        repository.mkdir()
    fixed = repositories[1] / "double_bass-v1.json"
    fixed_value = {
        "strings": [
            {
                "bridge_cutoff_hz": 7086.471045764144,
                "loss_time_constant_seconds": 0.25,
            }
            for unused_index in range(4)
        ],
    }
    fixed.write_text(json.dumps(fixed_value), encoding="utf-8")
    candidate_value = json.loads(json.dumps(fixed_value))
    for row in MODULE.joint_candidate_changes():
        parent = candidate_value
        for part in row["path"][:-1]:
            parent = parent[part]
        parent[row["path"][-1]] = row["after"]
    candidate = root / "candidate.json"
    candidate.write_text(json.dumps(candidate_value), encoding="utf-8")
    renderer_config = root / "renderer-base.json"
    renderer_config.write_text(json.dumps({
        "permissions": {
            "render": True,
            "validate_profile": True,
            "write_profile": False,
        },
    }), encoding="utf-8")

    analyzer = root / "analyzer"
    fake_analyzer(analyzer)
    renderer_marker = root / "renderer-built"
    renderer_builder = root / "renderer-builder.py"
    fake_renderer_builder(renderer_builder, renderer_marker)
    fit_selector = root / "instrument-fit.py"
    fit_selector.write_text("# fixture\n", encoding="utf-8")

    fit_references = []
    fit_reference_rows = []
    fit_selection_rows = []
    roster = []
    archive = {
        "bytes": 2168063959,
        "md5": "a" * 32,
        "name": "good-sounds.zip",
        "url": "https://example.invalid/good-sounds.zip",
    }
    license_row = {
        "archive_description_spdx": "CC-BY-NC-4.0",
        "archive_record_url": "https://example.invalid/good-sounds",
        "archive_structured_metadata_spdx": "CC-BY-4.0",
        "member_page_spdx": "CC-BY-3.0",
        "member_page_url": "https://example.invalid/good-sounds/member",
        "status": "conflicting-source-metadata-private-analysis-only",
    }
    source_group_id = "good-sounds-double-bass-player-1"
    for index, (target, target_spec) in enumerate(
            MODULE.JOINT_OPEN_TARGETS.items()):
        fit_id = "fixture-fit-" + target
        fit_path = root / (fit_id + ".wav")
        wave_file(fit_path, 48000, 3, 12000 + index)
        fit_hash = sha256(fit_path)
        fit_references.append({
            "bits_per_sample": 24,
            "channels": 1,
            "expected_hz": target_spec["expected_hz"],
            "frames": 12000 + index,
            "id": fit_id,
            "sample_rate_hz": 48000,
            "sha256": fit_hash,
            "target": target,
        })
        fit_reference_rows.append({"path": str(fit_path), "target": target})
        selection = root / ("selection-" + target + ".json")
        selection.write_text(json.dumps({
            "chosen_parameters": MODULE.JOINT_SELECTION_PARAMETERS[target],
            "chosen_point_id": "selected",
            "points": [{
                "eligible": True,
                "evidence": [{
                    "checked_harmonic_decay_valid": True,
                    "checked_note_valid": True,
                    "maximum_absolute_t60_error_octaves": 0.4,
                    "mean_absolute_t60_error_octaves": 0.2,
                    "rms_t60_error_octaves": 0.3,
                    "valid_harmonic_count": 4,
                }],
                "point_id": "selected",
            }],
            "reference_bindings": [{"id": fit_id, "sha256": fit_hash}],
            "schema": "hwa-instrument-fit-result",
            "schema_version": 1,
            "selection_mode": "fit-only",
            "status": "pass",
        }), encoding="utf-8")
        fit_selection_rows.append({
            "path": str(selection),
            "sha256": sha256(selection),
            "target": target,
        })

        source_path = root / ("whole-good-sounds-" + target + ".wav")
        wave_file(source_path, 48000, 3, 13000 + index)
        source_hash = sha256(source_path)
        source_bytes = source_path.stat().st_size
        roster.append({
            "expected_hz": target_spec["expected_hz"],
            "note": target_spec["note"],
            "source": {
                "archive": archive,
                "bits_per_sample": 24,
                "bytes": source_bytes,
                "channels": 1,
                "frames": 13000 + index,
                "id": "good-sounds-audit-" + target,
                "license": license_row,
                "member": {
                    "bytes": source_bytes,
                    "path": "sound_files/double_bass/" +
                            f"take-{index:04d}.wav",
                    "sha256": source_hash,
                },
                "path": str(source_path),
                "sample_rate_hz": 48000,
                "sha256": source_hash,
                "source_group_id": source_group_id,
                "string_assignment_evidence":
                    MODULE.JOINT_V2_STRING_ASSIGNMENT_EVIDENCE,
                "whole_file": True,
            },
            "target": target,
        })

    tools = {
        "analyzer": analyzer,
        "fit_selector": fit_selector,
        "manifest_builder": BUILDER,
        "renderer_builder": renderer_builder,
    }
    declaration = {
        "audit": {
            "dataset_id": MODULE.JOINT_V2_DATASET_ID,
            "independent_of_fit_source_group": True,
            "source_group_id": source_group_id,
        },
        "candidate": {"path": str(candidate), "sha256": sha256(candidate)},
        "commands": ["build-joint-validation-v2"],
        "created_utc": "2026-09-04T09:00:00Z",
        "fit_references": fit_reference_rows,
        "fit_selections": fit_selection_rows,
        "fixed_model": {"path": str(fixed), "sha256": sha256(fixed)},
        "gates": {
            "harmonic_decay_method": "harmonic-decay-1",
            "maximum_absolute_t60_error_octaves": 1.5,
            "maximum_mean_absolute_t60_error_octaves": 0.75,
            "minimum_valid_harmonics": 4,
            "pitch_method": "isolated-note-1",
        },
        "policy": {
            "candidate_results_outside_repositories": True,
            "candidate_tuning_after_validation": False,
            "fixed_model_write": False,
            "raw_and_derived_audio_outside_repositories": True,
            "validation_runs": 1,
        },
        "renderer_base_config": {
            "path": str(renderer_config), "sha256": sha256(renderer_config),
        },
        "repositories": [str(repository) for repository in repositories],
        "schema": "hwa-double-bass-joint-validation-declaration",
        "schema_version": 2,
        "tools": {
            name: {"path": str(tool), "sha256": sha256(tool)}
            for name, tool in tools.items()
        },
        "validation_roster": roster,
    }
    declaration_path = root / "joint-v2-declaration.json"
    declaration_path.write_text(json.dumps(declaration), encoding="utf-8")
    contract = {"fit_references": fit_references}
    return declaration_path, declaration, contract, renderer_marker


class DoubleBassManifestTests(unittest.TestCase):
    def test_checked_fit_and_reference_contract_validate_without_local_paths(self):
        completed = subprocess.run(
            [str(PYTHON), "-I", str(BUILDER), "validate",
             "--fit-manifest", str(FIT),
             "--reference-contract", str(CONTRACT)],
            check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, env={})
        self.assertEqual(completed.returncode, 0, completed.stderr)
        summary = json.loads(completed.stdout)
        self.assertEqual(summary, {
            "adapter_id": "hlolli_wg_double_bass-passive-open-v1",
            "check_cases": [row[3] for row in ROWS],
            "fit_cases": [row[1] for row in ROWS],
            "heldout_sources": [row[2] for row in ROWS],
            "schema": "hwa-double-bass-manifest-contract",
            "schema_version": 1,
        })

        for path in (FIT, CONTRACT):
            value = json.loads(path.read_text(encoding="utf-8"))
            self.assertFalse(any(text.startswith("/") for text in strings(value)))
            self.assertNotIn("force", json.dumps(value).lower())

        changed = json.loads(CONTRACT.read_text(encoding="utf-8"))
        changed["heldout_sources"][0]["dynamic"] = "ff"
        with tempfile.TemporaryDirectory() as text:
            path = Path(text) / "changed.json"
            path.write_text(json.dumps(changed), encoding="utf-8")
            rejected = subprocess.run(
                [str(PYTHON), "-I", str(BUILDER), "validate",
                 "--fit-manifest", str(FIT),
                 "--reference-contract", str(path)],
                check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, env={})
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("reference contract changed", rejected.stderr)

    def test_v2_contract_is_fit_only_and_has_no_local_or_audit_corpus(self):
        completed = subprocess.run(
            [str(PYTHON), "-I", str(BUILDER), "validate-v2"],
            check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, env={})
        self.assertEqual(completed.returncode, 0, completed.stderr)
        summary = json.loads(completed.stdout)
        self.assertEqual(summary["selection"], "fit-only")
        self.assertFalse(summary["audio_copied"])
        self.assertEqual(summary["fit_cases"], [
            "iowa2012-pizz-e-string-g1-ff-left-48k-soxr",
            "iowa2012-pizz-a-ff-open-left-48k-soxr",
            "iowa2012-pizz-d-ff-open-left-48k-soxr",
            "iowa2012-pizz-g-ff-open-left-48k-soxr",
        ])
        contract = json.loads(V2_CONTRACT.read_text(encoding="utf-8"))
        self.assertEqual(contract["policy"], {
            "audio_copied": False,
            "candidate_source": "fit-references-only",
            "dynamic_model": "passive-loss-amplitude-independent",
            "other_corpora": "separate-development-or-audit-only",
            "source_selection": "one-ff-note-per-physical-string",
        })
        checked_paths = [V2_CONTRACT, *V2_FITS.values()]
        for path in checked_paths:
            source = path.read_text(encoding="utf-8")
            self.assertNotIn("Orchidea", source)
            self.assertNotIn("/home/", source)
        for target, path in V2_FITS.items():
            fit = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(fit["selection"]["mode"], "fit-only")
            self.assertEqual(
                {row["split"] for row in fit["objectives"]}, {"fit"})
            self.assertEqual(
                {row["kind"] for row in fit["objectives"]},
                {"checked-note-harmonic-decay"},
            )
            self.assertEqual(len(fit["parameters"]), 1, target)

        frequency = subprocess.run(
            [str(PYTHON), "-I", str(BUILDER),
             "validate-d-frequency-v3"],
            check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, env={})
        self.assertEqual(frequency.returncode, 0, frequency.stderr)
        frequency_summary = json.loads(frequency.stdout)
        self.assertEqual(frequency_summary["grid_points"], 30)
        self.assertEqual(frequency_summary["selection"], "fit-only")
        self.assertEqual(frequency_summary["parameters"], [
            "string_d_bridge_cutoff_hz", "string_d_loss_seconds",
        ])
        frequency_fit = json.loads(
            D_FREQUENCY_FIT.read_text(encoding="utf-8"))
        self.assertEqual(len(frequency_fit["parameters"]), 2)
        self.assertEqual(
            frequency_fit["parameters"][0]["profile_paths"],
            [["strings", 2, "bridge_cutoff_hz"]],
        )
        self.assertNotIn("/home/", D_FREQUENCY_FIT.read_text(
            encoding="utf-8"))

    def test_v2_build_checks_fit_references_without_copying_audio(self):
        with tempfile.TemporaryDirectory(prefix="hwa bass v2 manifest ") as text:
            root = Path(text)
            analyzer = root / "analyzer"
            fake_analyzer(analyzer)
            contract = MODULE.expected_v2_contract()
            references = {}
            for index, row in enumerate(contract["fit_references"]):
                path = root / (row["id"] + ".wav")
                wave_file(path, 48000, 3, 12000 + index)
                row["frames"] = 12000 + index
                row["sha256"] = sha256(path)
                references[row["id"]] = path
            contract_path = root / "fit-reference-contract-v2.json"
            contract_path.write_text(json.dumps(contract), encoding="utf-8")
            fit_paths = {}
            for target in ("e1", "a1", "d2", "g2"):
                path = root / ("fit-" + target + ".json")
                path.write_text(json.dumps(
                    MODULE.expected_v2_fit(target, contract)
                ), encoding="utf-8")
                fit_paths[target] = path
            output = root / "v2-bundle"
            arguments = types.SimpleNamespace(
                reference_contract=contract_path,
                fit_manifest=[
                    target + "=" + str(path)
                    for target, path in fit_paths.items()
                ],
                fit_reference=[
                    identifier + "=" + str(path)
                    for identifier, path in references.items()
                ],
                analyzer=analyzer,
                output_dir=output,
            )
            with mock.patch.object(
                    MODULE, "expected_v2_contract", return_value=contract):
                summary = MODULE.build_v2_bundle(arguments)
            self.assertEqual(summary, {
                "audio_copied": False,
                "output": str(output.absolute()),
                "targets": ["e1", "a1", "d2", "g2"],
            })
            self.assertFalse(any(path.suffix == ".wav"
                                 for path in output.rglob("*")))
            self.assertEqual(
                sorted(path.name for path in output.iterdir()),
                ["a1", "d2", "e1", "g2", "receipt.json"],
            )
            receipt = json.loads(
                (output / "receipt.json").read_text(encoding="utf-8"))
            self.assertFalse(receipt["audio_copied"])
            self.assertEqual(receipt["candidate_source"],
                             "fit-references-only")
            for target in ("e1", "a1", "d2", "g2"):
                experiment = json.loads(
                    (output / target / "experiment.json").read_text(
                        encoding="utf-8"))
                fit = json.loads(
                    (output / target / "fit.json").read_text(
                        encoding="utf-8"))
                self.assertEqual(experiment["plan"]["kind"], "grid")
                self.assertEqual(len(experiment["parameters"]), 1)
                self.assertEqual(
                    len(experiment["parameters"][0]["levels"]),
                    24 if target == "d2" else 19,
                )
                self.assertEqual(len(experiment["cases"]), 2)
                self.assertEqual(
                    {row["split"] for row in experiment["cases"]},
                    {"fit", "check"},
                )
                self.assertEqual(len(experiment["inputs"]), 1)
                self.assertEqual(
                    experiment["responses"][0]["id"], "diagnostic.rms")
                self.assertEqual(fit["selection"]["mode"], "fit-only")
                self.assertEqual(
                    fit["objectives"][0]["kind"],
                    "checked-note-harmonic-decay",
                )
                evidence = json.loads(
                    (output / target / "reference-evidence.json").read_text(
                        encoding="utf-8"))
                self.assertEqual(
                    evidence["harmonic_decay"]["method"],
                    "harmonic-decay-1",
                )

    def test_d_frequency_build_is_d_only_and_copies_no_audio(self):
        with tempfile.TemporaryDirectory(
                prefix="hwa bass d frequency manifest ") as text:
            root = Path(text)
            analyzer = root / "analyzer"
            fake_analyzer(analyzer)
            contract = MODULE.expected_v2_contract()
            reference = next(
                row for row in contract["fit_references"]
                if row["target"] == "d2"
            )
            reference_path = root / (reference["id"] + ".wav")
            wave_file(reference_path, 48000, 3, 12000)
            reference["frames"] = 12000
            reference["sha256"] = sha256(reference_path)
            contract_path = root / "fit-reference-contract-v2.json"
            contract_path.write_text(json.dumps(contract), encoding="utf-8")
            fit_path = root / "fit-d-frequency.json"
            fit_path.write_text(json.dumps(
                MODULE.expected_d_frequency_fit(contract)
            ), encoding="utf-8")
            output = root / "d-frequency-bundle"
            arguments = types.SimpleNamespace(
                reference_contract=contract_path,
                fit_manifest=fit_path,
                fit_reference=[reference["id"] + "=" + str(reference_path)],
                analyzer=analyzer,
                output_dir=output,
            )
            with mock.patch.object(
                    MODULE, "expected_v2_contract", return_value=contract):
                summary = MODULE.build_d_frequency_bundle(arguments)
            self.assertEqual(summary, {
                "audio_copied": False,
                "grid_points": 30,
                "output": str(output.absolute()),
                "target": "d2",
            })
            self.assertFalse(any(path.suffix == ".wav"
                                 for path in output.rglob("*")))
            self.assertEqual(
                sorted(path.name for path in output.iterdir()),
                ["bindings.local.json", "experiment.json", "fit.json",
                 "receipt.json", "reference-evidence.json"],
            )
            experiment = json.loads(
                (output / "experiment.json").read_text(encoding="utf-8"))
            self.assertEqual(len(experiment["cases"]), 2)
            self.assertEqual(
                {row["split"] for row in experiment["cases"]},
                {"fit", "check"},
            )
            self.assertEqual(
                [row["levels"] for row in experiment["parameters"]],
                [list(MODULE.D_FREQUENCY_BRIDGE_CUTOFF_LEVELS),
                 list(MODULE.D_FREQUENCY_LOSS_LEVELS)],
            )
            self.assertEqual(
                len(experiment["parameters"][0]["levels"]) *
                len(experiment["parameters"][1]["levels"]),
                30,
            )
            receipt = json.loads(
                (output / "receipt.json").read_text(encoding="utf-8"))
            self.assertFalse(receipt["audio_copied"])
            self.assertFalse(receipt["candidate_model_written"])
            self.assertEqual(receipt["selection"], "fit-only")
            self.assertEqual(receipt["grid"]["point_count"], 30)

    def test_joint_manifest_has_two_points_and_checked_four_string_gates(self):
        references = {}
        for index, target in enumerate(MODULE.JOINT_TARGETS):
            references[target] = {
                "fit": {
                    "expected_hz": 50.0 + index, "id": "fit-" + target,
                    "path": "/private/fit-" + target + ".wav",
                    "sha256": str(index + 1) * 64,
                },
                "audit": {
                    "expected_hz": 60.0 + index, "id": "audit-" + target,
                    "path": "/private/audit-" + target + ".wav",
                    "sha256": str(index + 5) * 64,
                },
            }
        experiment = MODULE.joint_experiment(references)
        self.assertEqual(
            hashlib.sha256(MODULE.json_bytes(experiment)).hexdigest(),
            "1109daca5b0904d37aa1d5e85f44f29fed908968584a4769bb5d7876e8210b92",
        )
        self.assertEqual(
            experiment["parameters"][0]["levels"], [0.0, 1.0]
        )
        self.assertEqual(experiment["plan"], {
            "kind": "one-at-a-time", "replicates": 1,
            "sample_count": 0, "seed": 1701,
        })
        self.assertEqual(experiment["parameters"], [MODULE.JOINT_PARAMETER])
        self.assertEqual(len(experiment["cases"]), 12)
        manifest = MODULE.joint_fit_manifest(
            references,
            {target: 0.2 for target in MODULE.JOINT_TARGETS},
            MODULE.joint_candidate_changes(), "f" * 64,
        )
        self.assertEqual(
            hashlib.sha256(MODULE.json_bytes(manifest)).hexdigest(),
            "fecc8cad2246a7708850f42e7c2d78fc29004f6508d8bfc5d6683c4b9c38b96e",
        )
        self.assertEqual(manifest["schema_version"], 2)
        self.assertEqual(manifest["selection"][
            "minimum_candidate_harmonic_count"], 4)
        self.assertEqual(manifest["selection"][
            "max_candidate_harmonic_mean_error_octaves"], 0.75)
        self.assertEqual(manifest["selection"][
            "max_candidate_harmonic_maximum_error_octaves"], 1.5)
        self.assertEqual(len(manifest["candidate"]["profile_changes"]), 5)
        self.assertEqual(
            {row["split"] for row in manifest["objectives"]},
            {"fit", "check", "audit"},
        )
        with tempfile.TemporaryDirectory() as text:
            path = Path(text) / "joint-fit.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            parsed = FIT_MODULE.fit_manifest(path)
        self.assertEqual(parsed["adapter_id"], MODULE.JOINT_ADAPTER_ID)

    def test_joint_v2_checker_pins_whole_good_sounds_proxy_sources(self):
        self.assertEqual(MODULE.JOINT_OPEN_TARGETS, {
            "e1": {"expected_hz": 41.390732998718335, "note": "E1"},
            "a1": {"expected_hz": 55.25, "note": "A1"},
            "d2": {"expected_hz": 73.74990194289438, "note": "D2"},
            "g2": {"expected_hz": 98.4443083545075, "note": "G2"},
        })
        with tempfile.TemporaryDirectory(prefix="hwa bass joint v2 ") as text:
            root = Path(text)
            declaration_path, declaration, unused_contract, unused_marker = (
                joint_v2_fixture(root)
            )
            checked = MODULE.checked_joint_declaration_v2(declaration_path)
            self.assertEqual(checked, declaration)
            self.assertNotIn("conversion", checked)
            for row in checked["validation_roster"]:
                self.assertNotIn("derived", row)
                self.assertEqual(
                    row["source"]["string_assignment_evidence"],
                    {
                        "good_sounds_metadata_string": None,
                        "kind": "open-pitch-transfer-proxy",
                        "source_proven_physical_string": False,
                    },
                )
                self.assertTrue(row["source"]["whole_file"])
                self.assertNotIn("iowa", row["source"]["id"])

            def rejects(name, change, message):
                changed = json.loads(json.dumps(declaration))
                change(changed)
                changed_path = root / (name + ".json")
                changed_path.write_text(json.dumps(changed), encoding="utf-8")
                with self.assertRaisesRegex(MODULE.ManifestError, message):
                    MODULE.checked_joint_declaration_v2(changed_path)

            rejects(
                "wrong-open-frequency",
                lambda value: value["validation_roster"][0].update(
                    expected_hz=42.0),
                "exact open target",
            )
            rejects(
                "physical-string-claim",
                lambda value: value["validation_roster"][0]["source"][
                    "string_assignment_evidence"
                ].update(source_proven_physical_string=True),
                "open-pitch transfer proxy",
            )
            rejects(
                "second-source-group",
                lambda value: value["validation_roster"][0]["source"].update(
                    source_group_id="another-good-sounds-player"),
                "one source group",
            )
            rejects(
                "conversion-field",
                lambda value: value.update(conversion={}),
                "fields differ",
            )
            rejects(
                "license-conflict-erased",
                lambda value: value["validation_roster"][0]["source"][
                    "license"
                ].update(status="clear"),
                "license conflict",
            )
            source_path = Path(
                declaration["validation_roster"][0]["source"]["path"]
            )
            linked = root / "linked-source.wav"
            linked.symlink_to(source_path)
            rejects(
                "source-symlink",
                lambda value: value["validation_roster"][0]["source"].update(
                    path=str(linked)),
                "must be a regular file",
            )

    def test_joint_v2_build_preflights_before_emitting_frozen_bundle(self):
        with tempfile.TemporaryDirectory(prefix="hwa bass joint v2 build ") as text:
            root = Path(text)
            declaration_path, declaration, contract, renderer_marker = (
                joint_v2_fixture(root)
            )
            output = root / "joint-v2-bundle"
            arguments = types.SimpleNamespace(
                declaration=declaration_path,
                output_dir=output,
            )
            fixed_path = Path(declaration["fixed_model"]["path"])
            candidate_path = Path(declaration["candidate"]["path"])
            fixed_before = fixed_path.read_bytes()
            candidate_before = candidate_path.read_bytes()
            with mock.patch.object(
                    MODULE, "expected_v2_contract", return_value=contract):
                summary = MODULE.build_joint_validation_v2_bundle(arguments)
            self.assertEqual(summary["audit_scope"],
                             "open-pitch-transfer-proxy")
            self.assertEqual(summary["source_group_id"],
                             declaration["audit"]["source_group_id"])
            self.assertTrue(renderer_marker.is_file())
            self.assertTrue((output / "renderer").is_file())
            self.assertFalse(any(
                path.suffix.lower() in (".wav", ".aif", ".aiff")
                for path in output.rglob("*")
            ))
            receipt = json.loads(
                (output / "receipt.json").read_text(encoding="utf-8")
            )
            self.assertEqual(receipt["adapter_id"], MODULE.JOINT_V2_ADAPTER_ID)
            self.assertEqual(receipt["schema_version"], 2)
            self.assertFalse(receipt["audio_copied"])
            self.assertEqual(receipt["audit"], declaration["audit"])
            for row in receipt["validation_references"]:
                self.assertNotIn("path", row["source"])
                self.assertEqual(
                    row["source"]["string_assignment_evidence"],
                    MODULE.JOINT_V2_STRING_ASSIGNMENT_EVIDENCE,
                )
                self.assertEqual(
                    row["source"]["sha256"], row["source"]["member"]["sha256"]
                )
            fit = json.loads(
                (output / "fit.json").read_text(encoding="utf-8")
            )
            self.assertEqual(fit["adapter_id"], MODULE.JOINT_V2_ADAPTER_ID)
            parsed_fit = FIT_MODULE.fit_manifest(output / "fit.json")
            self.assertEqual(parsed_fit["adapter_id"], MODULE.JOINT_V2_ADAPTER_ID)
            self.assertEqual(
                {row["split"] for row in fit["objectives"]},
                {"fit", "check", "audit"},
            )
            self.assertEqual(fit["selection"][
                "minimum_candidate_harmonic_count"], 4)
            self.assertEqual(fit["selection"][
                "max_candidate_harmonic_mean_error_octaves"], 0.75)
            self.assertEqual(fit["selection"][
                "max_candidate_harmonic_maximum_error_octaves"], 1.5)
            renderer_config = json.loads(
                (output / "renderer-config.json").read_text(encoding="utf-8")
            )
            self.assertFalse(renderer_config["permissions"]["write_profile"])
            self.assertEqual(
                renderer_config["joint_candidate"]["adapter_id"],
                MODULE.JOINT_V2_ADAPTER_ID,
            )
            self.assertEqual(fixed_path.read_bytes(), fixed_before)
            self.assertEqual(candidate_path.read_bytes(), candidate_before)

        with tempfile.TemporaryDirectory(
                prefix="hwa bass joint v2 rejected ") as text:
            root = Path(text)
            declaration_path, unused_declaration, contract, renderer_marker = (
                joint_v2_fixture(root)
            )
            output = root / "rejected-bundle"
            arguments = types.SimpleNamespace(
                declaration=declaration_path,
                output_dir=output,
            )
            with mock.patch.object(
                    MODULE, "expected_v2_contract", return_value=contract), \
                    mock.patch.object(
                        MODULE, "v2_reference_evidence",
                        side_effect=MODULE.ManifestError(
                            "fixture reference preflight failed")):
                with self.assertRaisesRegex(
                        MODULE.ManifestError, "reference preflight failed"):
                    MODULE.build_joint_validation_v2_bundle(arguments)
            self.assertFalse(renderer_marker.exists())
            self.assertFalse(output.exists())

    def test_build_makes_a_repeatable_explicit_derived_binding_bundle(self):
        with tempfile.TemporaryDirectory(prefix="hwa bass manifest ") as text:
            root = Path(text)
            fit_references = {}
            heldout_sources = {}
            for index, (_, fit_id, source_id, _) in enumerate(ROWS):
                fit_references[fit_id] = root / (fit_id + ".wav")
                heldout_sources[source_id] = root / (source_id + ".wav")
                wave_file(fit_references[fit_id], 48000, 3,
                          12000 + index * 2)
                wave_file(heldout_sources[source_id], 44100, 2,
                          11025 + index * 147)
            ffmpeg = root / "ffmpeg tool"
            fake_ffmpeg(ffmpeg)
            analyzer = root / "analyzer tool"
            fake_analyzer(analyzer)
            contract = json.loads(CONTRACT.read_text(encoding="utf-8"))
            for row in contract["fit_references"]:
                path = fit_references[row["id"]]
                with wave.open(str(path), "rb") as stream:
                    row["frames"] = stream.getnframes()
                row["sha256"] = sha256(path)
            for row in contract["heldout_sources"]:
                path = heldout_sources[row["id"]]
                with wave.open(str(path), "rb") as stream:
                    row["frames"] = stream.getnframes()
                row["sha256"] = sha256(path)
            contract_path = root / "contract.json"
            contract_path.write_text(json.dumps(contract), encoding="utf-8")

            def command_for(bundle):
                command = [
                    str(PYTHON), "-I", str(BUILDER), "build",
                    "--fit-manifest", str(FIT),
                    "--reference-contract", str(contract_path),
                    "--analyzer", str(analyzer),
                    "--ffmpeg", str(ffmpeg), "--output-dir", str(bundle),
                ]
                for _, fit_id, source_id, _ in ROWS:
                    command.extend(("--fit-reference",
                                    fit_id + "=" + str(fit_references[fit_id])))
                    command.extend(("--heldout-source",
                                    source_id + "=" +
                                    str(heldout_sources[source_id])))
                return command

            rejected_contract = subprocess.run(
                command_for(root / "unchecked bundle"), check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, env={})
            self.assertNotEqual(rejected_contract.returncode, 0)
            self.assertIn("reference contract changed",
                          rejected_contract.stderr)

            def arguments_for(bundle):
                return types.SimpleNamespace(
                    fit_manifest=FIT,
                    reference_contract=contract_path,
                    fit_reference=[
                        fit_id + "=" + str(fit_references[fit_id])
                        for _, fit_id, _, _ in ROWS
                    ],
                    heldout_source=[
                        source_id + "=" + str(heldout_sources[source_id])
                        for _, _, source_id, _ in ROWS
                    ],
                    analyzer=analyzer,
                    ffmpeg=ffmpeg,
                    output_dir=bundle,
                )

            bundles = [root / "bundle one", root / "bundle two"]
            for bundle in bundles:
                with mock.patch.object(
                        MODULE, "expected_contract", return_value=contract):
                    summary = MODULE.build_bundle(arguments_for(bundle))
                self.assertEqual(summary, {
                    "derived_bindings": [
                        {"id": derived_id,
                         "sha256": sha256(bundle / (derived_id + ".wav"))}
                        for _, _, _, derived_id in ROWS
                    ],
                    "output": str(bundle.absolute()),
                })
                self.assertEqual(stat.S_IMODE(bundle.stat().st_mode), 0o700)
                self.assertEqual(sorted(path.name for path in bundle.iterdir()), [
                    "bindings.local.json", "experiment.json", "fit.json",
                    *sorted(row[3] + ".wav" for row in ROWS),
                    "receipt.json",
                ])

            stable = ("experiment.json", "fit.json", "receipt.json",
                      *(row[3] + ".wav" for row in ROWS))
            for name in stable:
                self.assertEqual((bundles[0] / name).read_bytes(),
                                 (bundles[1] / name).read_bytes())

            experiment = json.loads(
                (bundles[0] / "experiment.json").read_text(encoding="utf-8"))
            self.assertEqual(experiment["clock_rate_hz"], 48000)
            self.assertEqual(
                [row["id"] for row in experiment["inputs"]],
                [row[3] for row in ROWS] + [row[1] for row in ROWS])
            self.assertEqual(
                [row["id"] for row in experiment["cases"]],
                [row[3] for row in ROWS] + [row[1] for row in ROWS])
            self.assertEqual(
                [row["id"] for row in experiment["parameters"]],
                ["string_a_loss_seconds", "string_d_loss_seconds",
                 "string_e_loss_seconds", "string_g_loss_seconds"])
            self.assertEqual(experiment["plan"], {
                "kind": "random", "replicates": 1,
                "sample_count": 32, "seed": 1701,
            })
            self.assertTrue(all(not row["levels"]
                                for row in experiment["parameters"]))
            fit_value = json.loads(
                (bundles[0] / "fit.json").read_text(encoding="utf-8"))
            expected_cases = [
                (row[3], "check") for row in ROWS
            ] + [
                (row[1], "fit") for row in ROWS
            ]
            self.assertEqual(
                [(row["case"], row["split"])
                 for row in fit_value["objectives"]],
                expected_cases)
            self.assertTrue(all(
                row["reference_binding"] == row["case"]
                for row in fit_value["objectives"]))

            for _, _, source_id, derived_id in ROWS:
                derived = bundles[0] / (derived_id + ".wav")
                with wave.open(str(heldout_sources[source_id]), "rb") as stream:
                    expected_frames = (
                        stream.getnframes() * 48000 + 22050) // 44100
                with wave.open(str(derived), "rb") as stream:
                    self.assertEqual(
                        (stream.getframerate(), stream.getnchannels(),
                         stream.getsampwidth(), stream.getnframes()),
                        (48000, 1, 3, expected_frames))
            receipt = json.loads(
                (bundles[0] / "receipt.json").read_text(encoding="utf-8"))
            self.assertEqual(receipt["builder_sha256"], sha256(BUILDER))
            self.assertEqual(receipt["analyzer"], {
                "path": str(analyzer.resolve()),
                "sha256": sha256(analyzer),
                "version": "hlolli-wg-analyzer 1.1.0-test",
            })
            self.assertEqual(receipt["transform"]["tool"], {
                "path": str(ffmpeg.resolve()),
                "sha256": sha256(ffmpeg),
                "version": "ffmpeg version 8.1.2-test",
            })
            self.assertTrue(all(row["sample_rate_hz"] == 44100
                                for row in receipt["heldout_sources"]))
            self.assertTrue(all(row["sample_rate_hz"] == 48000
                                for row in receipt["derived_bindings"]))
            self.assertEqual(
                [row["source_id"] for row in receipt["derived_bindings"]],
                [row[2] for row in ROWS])
            bindings = json.loads(
                (bundles[0] / "bindings.local.json").read_text(encoding="utf-8"))
            self.assertEqual(
                [row["id"] for row in bindings["bindings"]],
                [row[3] for row in ROWS] + [row[1] for row in ROWS])
            for index, (_, _, _, derived_id) in enumerate(ROWS):
                self.assertEqual(
                    Path(bindings["bindings"][index]["path"]),
                    bundles[0] / (derived_id + ".wav"))

            fake_ffmpeg(ffmpeg, nonrepeatable=True)
            rejected_output = root / "rejected bundle"
            with mock.patch.object(
                    MODULE, "expected_contract", return_value=contract):
                with self.assertRaisesRegex(
                        MODULE.ManifestError, "is not byte-repeatable"):
                    MODULE.build_bundle(arguments_for(rejected_output))
            self.assertFalse(rejected_output.exists())


if __name__ == "__main__":
    unittest.main()
