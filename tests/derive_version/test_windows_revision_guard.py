"""Exercise the real packaging guard before any EXE/MSI/installer access."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class WindowsRevisionGuardTests(unittest.TestCase):
    def test_concrete_four_component_ci_artifact(self):
        script = (ROOT / "scripts/verify-windows-package.ps1").read_text(encoding="utf-8")
        boundary = "$exe = (Get-Item -LiteralPath $ExePath).VersionInfo"
        self.assertEqual(script.count(boundary), 1)
        guard = script.split(boundary)[0] + "\n'Independent revision guard passed'\n"
        versions = ("6.5.3.181-2.ga79ae688", "6.5.3-181.ge8ef36a8",
                    "6.5.3.181-70000.gfeedface")
        for version in versions:
            with self.subTest(version=version), tempfile.TemporaryDirectory() as directory:
                path = Path(directory)
                (path / "derive-version.py").write_text(
                    (ROOT / "scripts/derive-version.py").read_text(encoding="utf-8"), encoding="utf-8")
                fixture = path / "verify.ps1"
                fixture.write_text(guard, encoding="utf-8")
                environment = dict(os.environ, CI_VERSION=version)
                result = subprocess.run(
                    ["pwsh", "-NoProfile", "-File", str(fixture),
                     "-ExePath", "never-read.exe", "-MsiPath", "never-opened.msi"],
                    capture_output=True, text=True, timeout=15, env=environment)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("Independent revision guard passed", result.stdout)
