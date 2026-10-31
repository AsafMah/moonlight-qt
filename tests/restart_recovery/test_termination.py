"""Replay the actual Session termination callback, without a stream or host."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_http_deadline import ROOT, function


class TerminationTests(unittest.TestCase):
    def test_actual_termination_callback(self):
        source = (ROOT / "app/streaming/session.cpp").read_text(encoding="utf-8")
        harness = Path(__file__).with_name("termination_harness.cpp").read_text(encoding="utf-8")
        harness = harness.replace("// TERMINATION_CALLBACK", function(
            source, "void Session::clConnectionTerminated("))
        compiler = shutil.which("cl" if os.name == "nt" else "c++")
        self.assertIsNotNone(compiler, "Run with a C++ compiler on PATH")
        with tempfile.TemporaryDirectory(prefix="moonlight-restart-termination-") as directory:
            path = Path(directory)
            unit = path / "main.cpp"
            executable = path / ("termination.exe" if os.name == "nt" else "termination")
            unit.write_text(harness, encoding="utf-8")
            include = ROOT / "app/streaming"
            command = ([compiler, "/nologo", "/std:c++17", "/EHsc", f"/I{include}", str(unit),
                        f"/Fe:{executable}"] if os.name == "nt" else
                       [compiler, "-std=c++17", "-pthread", f"-I{include}", str(unit),
                        "-o", str(executable)])
            result = subprocess.run(command, cwd=path, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            if os.name == "nt":
                runner = Path(r"E:\Code\moonlight-tools\run-test-exe.ps1")
                if not runner.is_file():
                    runner = ROOT / "scripts/run-built-test.ps1"
                command = ["pwsh", "-NoProfile", "-File",
                           str(runner),
                           "-Exe", str(executable), "-Platform", "offscreen"]
            else:
                command = [str(executable)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("termination recovery passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
