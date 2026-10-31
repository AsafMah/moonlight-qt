import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(os.name == "nt", "Windows updater script")
class PortableRollbackTests(unittest.TestCase):
    def test_invalid_archive_preserves_installation(self):
        with tempfile.TemporaryDirectory(prefix="moonlight-rollback-fixture-") as directory:
            root = Path(directory)
            install = root / "install"
            install.mkdir()
            (install / "Moonlight.exe").write_bytes(b"not an executable; never launched")
            (install / "sentinel.txt").write_text("original installation", encoding="utf-8")
            workspace = install / "MoonlightPortableUpdate-1-1"
            workspace.mkdir()
            archive = workspace / "update.zip"
            archive.write_bytes(b"verified but not a ZIP")
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            result = subprocess.run(
                ["pwsh", "-NoProfile", "-File",
                 str(Path(__file__).with_name("portable_rollback_harness.ps1")),
                 "-ScriptPath", str(ROOT / "app/deploy/windows/install-portable-update.ps1"),
                 "-InstallDir", str(install), "-WorkspaceDir", str(workspace),
                 "-ZipPath", str(archive), "-Digest", digest],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
