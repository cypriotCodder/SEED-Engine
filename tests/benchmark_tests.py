"""Focused regressions for missing GPU samples and long benchmark workloads."""
import contextlib
import io
import json
from pathlib import Path
import runpy
import sys
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "benchmark.py"

class BenchmarkTests(unittest.TestCase):
    def test_missing_gpu_and_long_run_timeout(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "reports"
            timeouts = []
            def fake_run(command, *, check, timeout):
                self.assertTrue(check)
                timeouts.append(timeout)
                destination = Path(command[command.index("--benchmark") + 1])
                destination.write_text(json.dumps({"milliseconds": {
                    "frame_cpu": {"p95": 4.5}, "render_gpu": None}}))
            with patch.object(sys, "argv", [str(SCRIPT), "unused-demo", str(output),
                                            "--frames", "10000", "--runs", "1"]), \
                 patch("subprocess.run", side_effect=fake_run), \
                 contextlib.redirect_stdout(io.StringIO()):
                runpy.run_path(str(SCRIPT), run_name="__main__")
            result = json.loads((output / "summary.json").read_text())
            self.assertIsNone(result["static"]["gpu_p95_median_ms"])
            self.assertIsNone(result["stream"]["gpu_p95_median_ms"])
            self.assertEqual(result["static"]["frame_cpu_p95_median_ms"], 4.5)
            self.assertTrue(all(timeout > 10000 / 10 for timeout in timeouts))

if __name__ == "__main__":
    unittest.main()
