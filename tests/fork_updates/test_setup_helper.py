"""Run the real helper with fake process/UAC/dialog APIs, never a real installer."""
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SetupHelperTests(unittest.TestCase):
    def test_success_cancel_digest_and_failed_install(self):
        source = (ROOT / "app/deploy/windows/install-setup-update.ps1").read_text(encoding="utf-8")
        source, replacements = re.subn(
            r"    Add-Type -AssemblyName System.Windows.Forms\n"
            r"    \[System.Windows.Forms.MessageBox\]::Show\([\s\S]+?\) \| Out-Null",
            "    'error-dialog' | Add-Content -LiteralPath $TracePath", source)
        self.assertEqual(replacements, 1, "Harness must intercept the error dialog only")
        for mode in ("success", "cancel", "bad-digest", "installer-failure"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                workspace = root / "MoonlightPortableUpdate-123-456"
                workspace.mkdir()
                install = root / "installed"
                install.mkdir()
                executable = install / "Moonlight.exe"
                executable.write_bytes(b"never executed")
                package = workspace / "update.exe"
                package.write_bytes(b"never executed")
                digest = hashlib.sha256(package.read_bytes()).hexdigest()
                if mode == "bad-digest":
                    digest = "0" * 64
                script = workspace / "helper.ps1"
                script.write_text(source, encoding="utf-8")
                trace = root / "trace.txt"
                result = subprocess.run(
                    ["pwsh", "-NoProfile", "-File", str(Path(__file__).with_name("setup_harness.ps1")),
                     "-ScriptPath", str(script), "-WorkspaceDir", str(workspace),
                     "-InstallerPath", str(package), "-ExePath", str(executable),
                     "-Digest", digest, "-Mode", mode, "-TracePath", str(trace)],
                    text=True, capture_output=True, timeout=15)
                self.assertEqual(result.returncode, 1 if mode in ("cancel", "bad-digest") else 0,
                                 result.stdout + result.stderr)
                lines = trace.read_text().splitlines() if trace.exists() else []
                launches = [line for line in lines if line.startswith("RunAs|")]
                relaunches = [line for line in lines if str(executable) in line]
                self.assertEqual(len(launches), 0 if mode == "bad-digest" else 1)
                self.assertEqual(len(relaunches), 1 if mode in ("success", "installer-failure") else 0)
                self.assertEqual("error-dialog" in lines, mode == "installer-failure")
                self.assertEqual(workspace.exists(), mode != "success")
