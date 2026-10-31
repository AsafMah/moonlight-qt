"""Exercise real installer preflight and download cancellation without execution."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class InstallerControlTests(unittest.TestCase):
    def test_verified_download_preflight(self):
        qmake = shutil.which("qmake") or shutil.which("qmake6")
        self.assertIsNotNone(qmake)
        source = (ROOT / "app/backend/portableupdateinstaller.cpp").read_text(encoding="utf-8")
        source = source.replace('#include "path.h"', "")
        source = source.replace("new QNetworkAccessManager(this)", "new ScriptedNetworkAccessManager(this)")
        header = (ROOT / "app/backend/portableupdateinstaller.h").read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory(prefix="moonlight-installer-control-") as directory:
            path = Path(directory)
            (path / "portableupdateinstaller.h").write_text(
                header.replace("private slots:", "public slots:").replace("private:", "public:"),
                encoding="utf-8")
            (path / "installer.inc").write_text(source, encoding="utf-8")
            (path / "main.cpp").write_text(
                Path(__file__).with_name("installer_harness.cpp").read_text(encoding="utf-8"),
                encoding="utf-8")
            (path / "control.pro").write_text(
                "QT = core network\nCONFIG += console c++17 release\n"
                "CONFIG -= app_bundle debug debug_and_release\n"
                "TEMPLATE = app\nTARGET = control\nSOURCES = main.cpp\n"
                "HEADERS = portableupdateinstaller.h\n"
                'DEFINES += VERSION_STR=\\\\\\"6.5.3.181\\\\\\"\n', encoding="utf-8")
            for command in ([qmake, "control.pro"],
                            ["nmake", "/nologo"] if os.name == "nt" else ["make", "-s"]):
                result = subprocess.run(command, cwd=path, capture_output=True, text=True, timeout=120)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            if os.name == "nt":
                runner = ROOT / "scripts/run-built-test.ps1"
                command = ["pwsh", "-NoProfile", "-File", str(runner),
                           "-Exe", str(next(path.rglob("control.exe"))), "-Platform", "offscreen"]
            else:
                command = [str(path / "control")]
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("Installer preflight passed", result.stdout)
