"""Compile the production qmake version wiring and inspect its actual PE resources."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(os.name == "nt", "Windows PE resource compilation requires MSVC")
class ReleaseDisplayResourceTests(unittest.TestCase):
    def test_compiled_display_and_resource_share_release_identity(self):
        qmake = shutil.which("qmake")
        if qmake is None or shutil.which("cl") is None:
            self.skipTest("Qt qmake and MSVC are required")
        source = (ROOT / "app/app.pro").read_text(encoding="utf-8")
        version = source[source.index("VERSION_SCRIPT ="):source.index("CI_UPDATE_BUILD =")]
        definitions = next(line for line in source.splitlines() if line.startswith("DEFINES += VERSION_STR="))
        comments = next(line.strip() for line in source.splitlines()
                        if line.strip().startswith("QMAKE_TARGET_COMMENTS ="))
        spec = importlib.util.spec_from_file_location(
            "resource_publisher", ROOT / "scripts/publish-integration-release.py")
        publisher = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(publisher)
        with tempfile.TemporaryDirectory(prefix="moonlight-display-") as directory:
            root = Path(directory)
            (root / "app").mkdir()
            (root / "scripts").mkdir()
            shutil.copyfile(ROOT / "scripts/derive-version.py", root / "scripts/derive-version.py")
            (root / "app/version.txt").write_text("6.5.4\n", encoding="utf-8")
            (root / "app/main.cpp").write_text(
                '#include <cstdio>\nint main() { std::puts("Moonlight " VERSION_STR); }\n',
                encoding="utf-8")
            (root / "app/probe.pro").write_text(
                "TEMPLATE = app\nTARGET = probe\nQT = core\nCONFIG += console release\n"
                "CONFIG -= debug debug_and_release\nSOURCES = main.cpp\nVERSION_PYTHON = python\n"
                + version + "\nVERSION = $$MOONLIGHT_NUMERIC_VERSION\n"
                + comments + "\n" + definitions + "\n", encoding="utf-8")
            with patch.dict(os.environ, {
                "CI_VERSION": "6.5.4.225-7.ga7b37240",
                "GITHUB_ACTIONS": "true", "GITHUB_REF": "refs/heads/integration",
            }):
                for command in ([qmake, "app/probe.pro"], ["nmake", "/nologo"]):
                    result = subprocess.run(command, cwd=root, capture_output=True,
                                            text=True, timeout=120)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            exe = next(root.rglob("probe.exe"))
            result = subprocess.run([
                "pwsh", "-NoProfile", "-File", str(ROOT / "scripts/run-built-test.ps1"),
                "-Exe", str(exe), "-Platform", "offscreen",
            ], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("Moonlight 6.5.4.232\n", result.stdout)
            self.assertEqual(publisher.pe_file_version(exe.read_bytes()), "6.5.4.232")
            self.assertEqual(publisher.pe_display_version(exe.read_bytes()), "6.5.4.232")

    def test_windows_ci_checks_real_application_before_packaging(self):
        source = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")
        step = source.split("    - name: Verify built application display version", 1)[1].split(
            "\n    - name:", 1)[0]
        self.assertIn("run-built-test.ps1 -Exe build\\build-x64-release\\app\\release\\Moonlight.exe", step)
        self.assertIn("-TestArgs '--version'", step)
        self.assertIn('-cnotcontains "Moonlight $expected"', step)
        self.assertLess(source.index("Verify built application display version"),
                        source.index("Upload portable artifact x64"))
