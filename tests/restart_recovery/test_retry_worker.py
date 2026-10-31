"""Execute the actual retry pump against a fake clock and connection worker."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_http_deadline import ROOT, function


class RetryWorkerTests(unittest.TestCase):
    def test_production_retry_pump(self):
        source = (ROOT / "app/streaming/session.cpp").read_text(encoding="utf-8")
        body = function(source, "bool Session::tryReconnect(")
        body = body.replace("RecoveryBudget::nowMs()", "fakeNow()")
        harness = Path(__file__).with_name("retry_worker_harness.cpp").read_text(encoding="utf-8")
        harness = harness.replace("// RETRY_WORKER", body)
        status = function(source, "void Session::processRecoveryStatus(")
        harness = harness.replace("// RECOVERY_STATUS", status.replace(
            "RecoveryBudget::nowMs()", "fakeNow()"))
        control = function(source, "bool Session::prepareConnectionAttemptControl(")
        harness = harness.replace("// ATTEMPT_CONTROL", control.replace(
            "RecoveryBudget::nowMs()", "fakeNow()"))
        interrupt = function(source, "bool Session::interruptConnectionAttemptIfStopped(")
        harness = harness.replace("// ATTEMPT_INTERRUPT", interrupt.replace(
            "RecoveryBudget::nowMs()", "fakeNow()"))
        harness = harness.replace("// ELIGIBILITY_LOG", function(
            source, "void Session::logRestartRecoveryIneligibility("))
        setup = function(source, "void Session::exec()")
        setup = setup[:setup.index("    // If the connection failed, clean up")]
        harness = harness.replace("// INITIAL_RECOVERY", setup + "\n}\n")
        header = (ROOT / "app/streaming/session.h").read_text(encoding="utf-8")
        start = header.index("    static void markVideoReceived()")
        end = header.index("\n    }\n", start) + len("\n    }\n")
        harness = harness.replace("// HOST_FRAME_MARKER", header[start:end].replace(
            "RecoveryBudget::nowMs()", "fakeNow()"))
        qmake = ((shutil.which("qmake6") or shutil.which("qmake"))
                 if os.name != "nt" else shutil.which("qmake"))
        self.assertIsNotNone(qmake, "Run with Qt qmake and a C++ compiler on PATH")
        with tempfile.TemporaryDirectory(prefix="moonlight-restart-worker-") as directory:
            path = Path(directory)
            (path / "main.cpp").write_text(harness, encoding="utf-8")
            (path / "worker.pro").write_text(
                "QT = core network\nCONFIG += console c++17 release\n"
                "CONFIG -= app_bundle debug debug_and_release\n"
                "TEMPLATE = app\nTARGET = worker\nSOURCES = main.cpp\n"
                f'INCLUDEPATH += "{ROOT / "app/streaming"}"\n', encoding="utf-8")
            for command in ([qmake, "worker.pro"],
                            ["nmake", "/nologo"] if os.name == "nt" else ["make", "-s"]):
                result = subprocess.run(command, cwd=path, capture_output=True,
                                        text=True, timeout=120)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            if os.name == "nt":
                executable = next(path.rglob("worker.exe"))
                runner = Path(r"E:\Code\moonlight-tools\run-test-exe.ps1")
                if not runner.is_file():
                    runner = ROOT / "scripts/run-built-test.ps1"
                command = ["pwsh", "-NoProfile", "-File", str(runner),
                           "-Exe", str(executable), "-Platform", "offscreen"]
            else:
                command = [str(path / "worker")]
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("retry worker deadline passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
