"""Exercise the production HTTP event loop with a scripted, non-network reply."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}\n", start) + 3]


class HttpDeadlineTests(unittest.TestCase):
    def test_production_transport_deadline_and_cancel(self):
        source = (ROOT / "app/backend/nvhttp.cpp").read_text(encoding="utf-8")
        harness = Path(__file__).with_name("http_deadline_harness.cpp").read_text(
            encoding="utf-8")
        harness = harness.replace(
            "// HTTP_TRANSPORT", function(source, "QNetworkReply*\nNvHTTP::openConnection("))
        qmake = ((shutil.which("qmake6") or shutil.which("qmake"))
                 if os.name != "nt" else shutil.which("qmake"))
        self.assertIsNotNone(qmake, "Run with Qt qmake and a C++ compiler on PATH")
        with tempfile.TemporaryDirectory(prefix="moonlight-restart-http-") as directory:
            path = Path(directory)
            (path / "main.cpp").write_text(harness, encoding="utf-8")
            (path / "deadline.pro").write_text(
                "QT = core network\nCONFIG += console c++17 release\n"
                "CONFIG -= app_bundle debug debug_and_release\n"
                "TEMPLATE = app\nTARGET = deadline\nSOURCES = main.cpp\n", encoding="utf-8")
            for command in ([qmake, "deadline.pro"],
                            ["nmake", "/nologo"] if os.name == "nt" else ["make", "-s"]):
                result = subprocess.run(command, cwd=path, capture_output=True,
                                        text=True, timeout=120)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            if os.name == "nt":
                executable = next(path.rglob("deadline.exe"))
                local_runner = Path(r"E:\Code\moonlight-tools\run-test-exe.ps1")
                runner = (local_runner if local_runner.is_file()
                          else ROOT / "scripts/run-built-test.ps1")
                command = ["pwsh", "-NoProfile", "-File", str(runner),
                           "-Exe", str(executable), "-Platform", "offscreen"]
            else:
                command = [str(path / "deadline")]
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("HTTP deadline and cancellation passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
