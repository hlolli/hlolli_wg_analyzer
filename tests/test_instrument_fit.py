#!/usr/bin/env python3

import importlib.util
import json
from pathlib import Path
import tempfile
import types
import unittest


TOOL = Path(__file__).resolve().parents[1] / "tools" / "instrument_fit.py"
SPEC = importlib.util.spec_from_file_location("instrument_fit", TOOL)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class InstrumentFitTests(unittest.TestCase):
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
