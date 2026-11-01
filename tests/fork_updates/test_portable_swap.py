import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(os.name == "nt", "Windows portable replacement")
class PortableSwapTests(unittest.TestCase):
    def test_package_names_only_and_locked_backup_cleanup(self):
        for mode in ("normal", "locked-backup", "partial-backup"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                install = root / "install"
                install.mkdir()
                (install / "Moonlight.exe").write_text("old build")
                (install / "platforms").mkdir()
                (install / "platforms" / "old-platform.dll").write_text("old dependency")
                for name in ("sentinel.txt", "logs/old.log", "dumps/old.dmp", "cache/old.cache"):
                    path = install / name
                    path.parent.mkdir(exist_ok=True)
                    path.write_text("unrelated")
                workspace = root / "MoonlightPortableUpdate-2-2"
                workspace.mkdir()
                archive = workspace / "update.zip"
                with zipfile.ZipFile(archive, "w") as stream:
                    stream.writestr("Moonlight.exe", "new build")
                    stream.writestr("platforms/qwindows.dll", "new dependency")
                (root / "expected-settings-before-update.txt").write_text("unchanged", encoding="utf-8")
                result = subprocess.run(
                    ["pwsh", "-NoProfile", "-File", str(Path(__file__).with_name("portable_swap_harness.ps1")),
                     "-ScriptPath", str(ROOT / "app/deploy/windows/install-portable-update.ps1"),
                     "-WorkspaceDir", str(workspace), "-InstallDir", str(install),
                     "-ZipPath", str(archive), "-Digest", hashlib.sha256(archive.read_bytes()).hexdigest(),
                     "-Mode", mode, "-TracePath", str(root / "trace.txt")],
                    capture_output=True, text=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
