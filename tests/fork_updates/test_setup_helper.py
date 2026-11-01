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
        self.assertGreaterEqual(replacements, 1, "Harness must intercept helper dialogs")
        for mode in ("success", "cancel", "bad-digest", "installer-failure", "reboot", "parent-timeout"):
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
                self.assertEqual(result.returncode, 1 if mode == "bad-digest" else 0,
                                 result.stdout + result.stderr +
                                 ((workspace / "setup-error.txt").read_text()
                                  if (workspace / "setup-error.txt").exists() else ""))
                lines = trace.read_text().splitlines() if trace.exists() else []
                self.assertNotIn("Installer must remain read-locked at the execution boundary.", lines,
                                 "\n".join(lines))
                launches = [line for line in lines if line.startswith("RunAs|")]
                relaunches = [line for line in lines if str(executable) in line]
                self.assertEqual(len(launches), 0 if mode in ("bad-digest", "parent-timeout") else 1)
                self.assertEqual(len(relaunches), 0 if mode in ("bad-digest", "reboot", "parent-timeout") else 1)
                self.assertEqual("error-dialog" in lines, mode in ("installer-failure", "cancel", "reboot", "parent-timeout"))
                self.assertEqual(workspace.exists(), mode == "bad-digest")
