"""Replay the actual update checker with scripted replies, without networking."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ForkUpdateTests(unittest.TestCase):
    def test_production_fork_selection(self):
        qmake = shutil.which("qmake") or shutil.which("qmake6")
        self.assertIsNotNone(qmake, "Qt and a C++ compiler must be on PATH")
        source = (ROOT / "app/backend/autoupdatechecker.cpp").read_text(encoding="utf-8")
        source = source.replace('#include "path.h"', "")
        source = source.replace("new QNetworkAccessManager(this)",
                                "new ScriptedNetworkAccessManager(this)")
        source = source.replace("QSysInfo::buildCpuArchitecture()", "testArchitecture")
        harness = Path(__file__).with_name("selection_harness.cpp").read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory(prefix="moonlight-fork-selection-") as directory:
            path = Path(directory)
            header = (ROOT / "app/backend/autoupdatechecker.h").read_text(encoding="utf-8")
            # Expose state only in the disposable test copy, never in production.
            (path / "autoupdatechecker.h").write_text(
                header.replace("private slots:", "public slots:").replace("private:", "public:"),
                encoding="utf-8")
            (path / "portableupdateinstaller.h").write_text(
                Path(__file__).with_name("installer_stub.h").read_text(encoding="utf-8"),
                encoding="utf-8")
            (path / "checker.inc").write_text(source, encoding="utf-8")
            (path / "main.cpp").write_text(harness, encoding="utf-8")
            (path / "selection.pro").write_text(
                "QT = core network\nCONFIG += console c++17 release\n"
                "CONFIG -= app_bundle debug debug_and_release\n"
                "TEMPLATE = app\nTARGET = selection\nSOURCES = main.cpp\n"
                "HEADERS = autoupdatechecker.h portableupdateinstaller.h\n"
                f'INCLUDEPATH += "{(ROOT / "app").as_posix()}"\n'
                'DEFINES += VERSION_STR=\\\\\\"6.5.3+181.ge8ef36a8\\\\\\"\n'
                'DEFINES += UPDATE_REPOSITORY=\\\\\\"AsafMah/moonlight-qt\\\\\\"\n'
                "DEFINES += UPDATE_BUILD_IS_RELEASE=1\n", encoding="utf-8")
            for command in ([qmake, "selection.pro"],
                            ["nmake", "/nologo"] if os.name == "nt" else ["make", "-s"]):
                result = subprocess.run(command, cwd=path, capture_output=True, text=True, timeout=120)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            if os.name == "nt":
                executable = next(path.rglob("selection.exe"))
                runner = ROOT / "scripts/run-built-test.ps1"
                command = ["pwsh", "-NoProfile", "-File", str(runner),
                           "-Exe", str(executable), "-Platform", "offscreen"]
            else:
                command = [str(path / "selection")]
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("Fork release selection passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
