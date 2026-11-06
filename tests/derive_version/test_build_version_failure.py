"""Version derivation failures must stop configuration, not select a fallback."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class BuildVersionFailureTests(unittest.TestCase):
    def test_qmake_propagates_derivation_failure(self):
        qmake = shutil.which("qmake6") or shutil.which("qmake")
        if qmake is None or (os.name == "nt" and shutil.which("cl") is None):
            self.skipTest("Qt qmake and its compiler are required")
        source = (ROOT / "app/app.pro").read_text(encoding="utf-8")
        block = source[source.index("VERSION_SCRIPT ="):source.index("CI_UPDATE_BUILD =")]
        with tempfile.TemporaryDirectory(prefix="moonlight-version-failure-") as directory:
            root = Path(directory)
            (root / "app").mkdir()
            (root / "scripts").mkdir()
            # Stand in for an unreachable-anchor error from derive-version.py.
            (root / "scripts/derive-version.py").write_text(
                "raise RuntimeError('Upstream release-lineage anchor is not reachable from HEAD')\n",
                encoding="utf-8")
            (root / "app/version.txt").write_text("6.2.84\n", encoding="utf-8")
            project = root / "app/probe.pro"
            project.write_text(
                "TEMPLATE = aux\nQT =\nVERSION_PYTHON = "
                + ("python" if os.name == "nt" else "python3") + "\n" + block,
                encoding="utf-8")
            result = subprocess.run(
                [qmake, str(project)], cwd=root, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("anchor is not reachable", result.stdout + result.stderr)
            self.assertIn("Cannot derive Moonlight", result.stdout + result.stderr)

    def test_windows_ci_checks_derivation_exit_status(self):
        workflow = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")
        version_step = workflow.split("    - name: Get version", 1)[1].split(
            "\n    - name:", 1)[0]
        self.assertIn("python scripts\\derive-version.py --field artifact >nul\n"
                      "        if errorlevel 1 exit /b %ERRORLEVEL%", version_step)
