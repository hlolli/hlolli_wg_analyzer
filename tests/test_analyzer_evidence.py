#!/usr/bin/env python3
"""Tests for checked analyzer evidence shared by fit tools."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import math
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import wave


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "analyzer_evidence.py"
ANALYZER = None


def load_module():
    specification = importlib.util.spec_from_file_location(
        "hwa_analyzer_evidence_test", MODULE_PATH)
    if specification is None or specification.loader is None:
        raise RuntimeError("cannot load analyzer evidence module")
    module = importlib.util.module_from_spec(specification)
    sys.modules[specification.name] = module
    specification.loader.exec_module(module)
    return module


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_real_harmonic_decay(path: Path) -> None:
    rate = 16000
    lead = 0.10
    duration = 4.0
    fundamental = 110.0
    taus = (0.8, 0.6, 0.4, 0.3)
    maximum = 32767
    raw = bytearray()
    for index in range(round((lead + duration) * rate)):
        time = index / rate
        value = 0.0
        if time >= lead:
            age = time - lead
            for harmonic, tau in enumerate(taus, 1):
                value += (
                    0.18 / harmonic * math.exp(-age / tau) *
                    math.sin(
                        2.0 * math.pi * fundamental * harmonic * age)
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


def write_pitch_analyzer(path: Path, mutation: str = "") -> None:
    path.write_text(
        "#!{} -I\n".format(Path(sys.executable).resolve()) +
        "import json, pathlib, sys\n"
        "a=sys.argv[1:]\n"
        "if len(a)!=7 or a[:2]!=['--json','isolated-note'] or a[3]!='--expected-hz' or a[5:]!=['--metrics','pitch']: raise SystemExit(9)\n"
        "p=str(pathlib.Path(a[2]).absolute()); hz=float(a[4])\n"
        "report={'schema':'hwa-isolated-note','schema_version':1,'command':'isolated-note','method':'isolated-note-1','path':p,'expected_hz':hz,'requested_mask':1,'valid_mask':1,'rejection_mask':0,'requested_metrics':['pitch'],'valid_metrics':['pitch'],'rejections':[],'pitch':{'valid':True,'hz':hz,'cents':0.0,'confidence':0.75,'coverage':0.875}}\n" +
        mutation + "\n"
        "print(json.dumps(report))\n",
        encoding="utf-8",
    )
    path.chmod(0o700)


def write_harmonic_analyzer(
        path: Path, valid_band_count: int = 3,
        profile_mutation: str = "") -> None:
    path.write_text(
        "#!{} -I\n".format(Path(sys.executable).resolve()) +
        "import json, pathlib, sys\n"
        "a=sys.argv[1:]\n"
        "if len(a)!=5 or a[:2]!=['--json','harmonic-decay'] or a[3]!='--expected-hz': raise SystemExit(9)\n"
        "p=str(pathlib.Path(a[2]).absolute()); hz=float(a[4]); count={}\n".format(
            valid_band_count) +
        "bands=[{'harmonic_number':i+1,'target_hz':hz*(i+1),'valid':i<count,'t60_seconds':1.0 if i<count else None,'rejection_mask':0 if i<count else 4,'rejections':[] if i<count else ['low-anchor-snr']} for i in range(6)]\n"
        "profile={'path':p,'valid':count>=4,'band_count':len(bands),'valid_band_count':count,'rejection_mask':0 if count>=4 else 512,'rejections':[] if count>=4 else ['low-harmonic-coverage'],'bands':bands}\n" +
        profile_mutation + "\n"
        "print(json.dumps({'schema':'hwa-harmonic-decay','schema_version':1,'command':'harmonic-decay','method':'harmonic-decay-1','expected_hz':hz,'reference':profile,'model':None,'comparison':None}))\n",
        encoding="utf-8",
    )
    path.chmod(0o700)


def write_harmonic_comparison_analyzer(
        path: Path, reported_rmse: float = 1.5811388300841898,
        forge_profile: bool = False) -> None:
    path.write_text(
        "#!{} -I\n".format(Path(sys.executable).resolve()) +
        "import json, pathlib, sys\n"
        "a=sys.argv[1:]\n"
        "if len(a)!=6 or a[:2]!=['--json','harmonic-decay'] or a[4]!='--expected-hz': raise SystemExit(9)\n"
        "r=str(pathlib.Path(a[2]).absolute()); m=str(pathlib.Path(a[3]).absolute()); hz=float(a[5])\n"
        "errors=(-2.0,-1.0,1.0,2.0)\n"
        "reference_t60=[1.0 for _ in errors]\n"
        "model_t60=[1.0 if %r else 10.0**(v/20.0) for v in errors]\n" % forge_profile +
        "profile=lambda p,values:{'path':p,'valid':True,'band_count':4,'valid_band_count':4,'rejection_mask':0,'rejections':[],'bands':[{'harmonic_number':i+1,'target_hz':hz*(i+1),'valid':True,'t60_seconds':v,'rejection_mask':0,'rejections':[]} for i,v in enumerate(values)]}\n"
        "bands=[{'harmonic_number':i+1,'reference_valid':True,'model_valid':True,'valid':True,'t60_log_error_db':v} for i,v in enumerate(errors)]\n"
        "comparison={'valid':True,'band_count':4,'shared_valid_band_count':4,'shared_reference_coverage':1.0,'t60_log_rmse_db':%r,'median_t60_log_bias_db':0.0,'bands':bands}\n" % reported_rmse +
        "print(json.dumps({'schema':'hwa-harmonic-decay','schema_version':1,'command':'harmonic-decay','method':'harmonic-decay-1','expected_hz':hz,'reference':profile(r,reference_t60),'model':profile(m,model_t60),'comparison':comparison}))\n",
        encoding="utf-8",
    )
    path.chmod(0o700)


class AnalyzerEvidenceTests(unittest.TestCase):
    def test_checked_run_uses_supplied_environment_and_scratch(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(
                prefix="hwa analyzer environment ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            source = root / "note.wav"
            write_pitch_analyzer(
                analyzer,
                "if __import__('os').environ.get('HWA_SENTINEL') != "
                "'fixed': raise SystemExit(8)",
            )
            source.write_bytes(b"source recording")
            directories = []
            temporary_file = module.tempfile.TemporaryFile

            def record_temporary_file(*args, **kwargs):
                directories.append(kwargs.get("dir"))
                return temporary_file(*args, **kwargs)

            checks = module.AnalyzerEvidence(
                analyzer, {"note": (source, sha256(source))},
                environment={"HWA_SENTINEL": "fixed"}, scratch=root)
            with mock.patch.object(
                    module.tempfile, "TemporaryFile",
                    side_effect=record_temporary_file):
                evidence = checks.isolated_note("note", 440.0)

            self.assertTrue(evidence.pitch.valid)
            self.assertEqual(directories, [root, root])

    def test_real_analyzer_harmonic_profile_matches_the_shared_contract(
            self) -> None:
        if ANALYZER is None:
            self.skipTest("analyzer executable was not supplied")
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa real evidence ") as text:
            root = Path(text)
            source = root / "harmonic.wav"
            write_real_harmonic_decay(source)
            digest = sha256(source)
            checks = module.AnalyzerEvidence(ANALYZER, {
                "reference": (source, digest),
                "model": (source, digest),
            })

            evidence = checks.harmonic_decay("reference", 110.0)
            comparison = checks.harmonic_decay(
                "reference", 110.0, model_id="model")

            self.assertTrue(evidence.reference_valid)
            self.assertGreaterEqual(evidence.reference_valid_band_count, 4)
            self.assertTrue(comparison.comparison_valid)
            self.assertGreaterEqual(comparison.shared_valid_band_count, 4)

    def test_checked_pitch_returns_raw_and_normalized_evidence(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa analyzer evidence ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            source = root / "note.wav"
            write_pitch_analyzer(analyzer)
            source.write_bytes(b"source recording")

            checks = module.AnalyzerEvidence(
                analyzer, {"note": (source, sha256(source))})
            evidence = checks.isolated_note("note", 440.0)

            self.assertEqual(checks.analyzer_sha256, sha256(analyzer))
            with self.assertRaises(AttributeError):
                checks.analyzer_sha256 = "0" * 64
            self.assertEqual(evidence.report["path"], str(source.absolute()))
            self.assertTrue(evidence.pitch.valid)
            self.assertEqual(evidence.pitch.hz, 440.0)
            self.assertEqual(evidence.pitch.cents, 0.0)
            self.assertEqual(evidence.pitch.confidence, 0.75)
            self.assertEqual(evidence.pitch.coverage, 0.875)

    def test_checked_pitch_rejects_contract_changes(self) -> None:
        module = load_module()
        changes = {
            "missing mask": "del report['requested_mask']",
            "missing rejection mask": "del report['rejection_mask']",
            "wrong mask": "report['valid_mask']=0",
            "decay-only rejection": (
                "report['rejection_mask']=128; "
                "report['rejections']=['late-pulse']"
            ),
            "wrong metric": "report['requested_metrics']=['decay']",
            "wrong method": "report['method']='isolated-note-2'",
            "wrong path": "report['path']=p+'.changed'",
            "non-finite pitch": "report['pitch']['hz']=float('nan')",
            "nonpositive pitch": "report['pitch']['hz']=0.0",
            "inconsistent cents": "report['pitch']['cents']=1.0",
            "bad confidence": "report['pitch']['confidence']=1.25",
            "bad coverage": "report['pitch']['coverage']=-0.25",
            "false valid flag": (
                "report['pitch']['valid']=False; report['valid_mask']=0; "
                "report['valid_metrics']=[]"
            ),
        }
        for name, mutation in changes.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory(
                    prefix="hwa bad pitch contract ") as text:
                root = Path(text)
                analyzer = root / "analyzer.py"
                source = root / "note.wav"
                write_pitch_analyzer(analyzer, mutation)
                source.write_bytes(b"source recording")
                checks = module.AnalyzerEvidence(
                    analyzer, {"note": (source, sha256(source))})
                with self.assertRaises(module.EvidenceError):
                    checks.isolated_note("note", 440.0)

    def test_checked_pitch_returns_a_coherent_invalid_measurement(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa invalid pitch ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            source = root / "note.wav"
            write_pitch_analyzer(
                analyzer,
                "report['pitch']={'valid':False,'hz':0.0,'cents':0.0,"
                "'confidence':0.0,'coverage':0.0}; "
                "report['valid_mask']=0; report['valid_metrics']=[]; "
                "report['rejection_mask']=16; "
                "report['rejections']=['low-support']",
            )
            source.write_bytes(b"source recording")
            checks = module.AnalyzerEvidence(
                analyzer, {"note": (source, sha256(source))})

            evidence = checks.isolated_note("note", 440.0)

            self.assertFalse(evidence.pitch.valid)
            self.assertEqual(evidence.pitch.hz, 0.0)

    def test_checked_pitch_rejects_duplicate_json_keys(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa duplicate JSON ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            source = root / "note.wav"
            write_pitch_analyzer(
                analyzer,
                "print('{\"pitch\":{\"valid\":true,\"valid\":false}}'); "
                "raise SystemExit(0)",
            )
            source.write_bytes(b"source recording")
            checks = module.AnalyzerEvidence(
                analyzer, {"note": (source, sha256(source))})
            with self.assertRaisesRegex(
                    module.EvidenceError, "duplicate JSON key: valid"):
                checks.isolated_note("note", 440.0)

    def test_checked_pitch_rejects_overflow_in_an_extra_json_field(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa JSON overflow ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            source = root / "note.wav"
            write_pitch_analyzer(
                analyzer,
                "print(json.dumps(report)[:-1]+',\"extra\":1e9999}'); "
                "raise SystemExit(0)",
            )
            source.write_bytes(b"source recording")
            checks = module.AnalyzerEvidence(
                analyzer, {"note": (source, sha256(source))})
            with self.assertRaisesRegex(
                    module.EvidenceError, "non-finite JSON number"):
                checks.isolated_note("note", 440.0)

    def test_analyzer_failure_uses_a_bounded_error_tail(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa analyzer failure ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            source = root / "note.wav"
            write_pitch_analyzer(
                analyzer,
                "print('named analyzer failure', file=sys.stderr); "
                "raise SystemExit(7)",
            )
            source.write_bytes(b"source recording")
            checks = module.AnalyzerEvidence(
                analyzer, {"note": (source, sha256(source))})
            with self.assertRaisesRegex(
                    module.EvidenceError, "failed: named analyzer failure"):
                checks.isolated_note("note", 440.0)

    def test_analyzer_report_has_a_byte_limit(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa analyzer output ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            source = root / "note.wav"
            write_pitch_analyzer(
                analyzer,
                "print('x'*(1024*1024+1)); raise SystemExit(0)",
            )
            source.write_bytes(b"source recording")
            checks = module.AnalyzerEvidence(
                analyzer, {"note": (source, sha256(source))})
            with self.assertRaisesRegex(
                    module.EvidenceError, "output exceeds the byte limit"):
                checks.isolated_note("note", 440.0)

    def test_analyzer_run_has_a_deadline(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa analyzer timeout ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            source = root / "note.wav"
            write_pitch_analyzer(
                analyzer, "import time; time.sleep(1.0)")
            source.write_bytes(b"source recording")
            checks = module.AnalyzerEvidence(
                analyzer, {"note": (source, sha256(source))})
            old_timeout = module.RUN_TIMEOUT_SECONDS
            module.RUN_TIMEOUT_SECONDS = 0.05
            try:
                with self.assertRaisesRegex(module.EvidenceError, "timed out"):
                    checks.isolated_note("note", 440.0)
            finally:
                module.RUN_TIMEOUT_SECONDS = old_timeout

    def test_constructor_rejects_a_changed_source_hash(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa changed source ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            source = root / "note.wav"
            write_pitch_analyzer(analyzer)
            source.write_bytes(b"source recording")
            with self.assertRaisesRegex(module.EvidenceError, "hash changed"):
                module.AnalyzerEvidence(
                    analyzer, {"note": (source, "0" * 64)})

    def test_run_rejects_a_source_changed_by_the_analyzer(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa source mutation ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            source = root / "note.wav"
            write_pitch_analyzer(
                analyzer, "pathlib.Path(p).write_bytes(b'changed')")
            source.write_bytes(b"source recording")
            checks = module.AnalyzerEvidence(
                analyzer, {"note": (source, sha256(source))})

            with self.assertRaisesRegex(
                    module.EvidenceError, "changed during"):
                checks.isolated_note("note", 440.0)

    def test_three_band_harmonic_profile_is_checked_but_not_valid(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa harmonic evidence ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            source = root / "note.wav"
            write_harmonic_analyzer(analyzer, valid_band_count=3)
            source.write_bytes(b"source recording")

            checks = module.AnalyzerEvidence(
                analyzer, {"reference": (source, sha256(source))})
            evidence = checks.harmonic_decay("reference", 440.0)

            self.assertFalse(evidence.reference_valid)
            self.assertEqual(evidence.reference_valid_band_count, 3)
            self.assertIsNone(evidence.model_valid)
            self.assertIsNone(evidence.comparison_valid)
            self.assertEqual(evidence.report["method"], "harmonic-decay-1")

    def test_harmonic_profile_rejects_missing_rejection_data(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa harmonic rejection ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            source = root / "note.wav"
            write_harmonic_analyzer(
                analyzer, profile_mutation="del profile['rejection_mask']")
            source.write_bytes(b"source recording")
            checks = module.AnalyzerEvidence(
                analyzer, {"reference": (source, sha256(source))})

            with self.assertRaises(module.EvidenceError):
                checks.harmonic_decay("reference", 440.0)

    def test_harmonic_comparison_returns_checked_values(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa harmonic comparison ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            reference = root / "reference.wav"
            model = root / "model.wav"
            write_harmonic_comparison_analyzer(analyzer)
            reference.write_bytes(b"reference recording")
            model.write_bytes(b"model recording")

            checks = module.AnalyzerEvidence(analyzer, {
                "reference": (reference, sha256(reference)),
                "model": (model, sha256(model)),
            })
            evidence = checks.harmonic_decay(
                "reference", 440.0, model_id="model")

            self.assertTrue(evidence.reference_valid)
            self.assertTrue(evidence.model_valid)
            self.assertTrue(evidence.comparison_valid)
            self.assertEqual(evidence.shared_valid_band_count, 4)
            self.assertEqual(evidence.shared_reference_coverage, 1.0)
            self.assertEqual(
                evidence.band_t60_log_errors_db, (-2.0, -1.0, 1.0, 2.0))
            self.assertEqual(
                evidence.t60_log_rmse_db, 1.5811388300841898)
            self.assertEqual(evidence.median_t60_log_bias_db, 0.0)

    def test_harmonic_comparison_rejects_a_false_summary(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa false harmonic sum ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            reference = root / "reference.wav"
            model = root / "model.wav"
            write_harmonic_comparison_analyzer(analyzer, reported_rmse=1.5)
            reference.write_bytes(b"reference recording")
            model.write_bytes(b"model recording")
            checks = module.AnalyzerEvidence(analyzer, {
                "reference": (reference, sha256(reference)),
                "model": (model, sha256(model)),
            })
            with self.assertRaisesRegex(
                    module.EvidenceError, "unknown contract"):
                checks.harmonic_decay(
                    "reference", 440.0, model_id="model")

    def test_harmonic_comparison_rejects_errors_unbound_from_profiles(
            self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa forged harmonic ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            reference = root / "reference.wav"
            model = root / "model.wav"
            write_harmonic_comparison_analyzer(analyzer, forge_profile=True)
            reference.write_bytes(b"reference recording")
            model.write_bytes(b"model recording")
            checks = module.AnalyzerEvidence(analyzer, {
                "reference": (reference, sha256(reference)),
                "model": (model, sha256(model)),
            })
            with self.assertRaisesRegex(
                    module.EvidenceError, "unknown contract"):
                checks.harmonic_decay(
                    "reference", 440.0, model_id="model")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--analyzer", type=Path)
    arguments, unittest_arguments = parser.parse_known_args()
    ANALYZER = arguments.analyzer
    unittest.main(argv=[sys.argv[0], *unittest_arguments])
