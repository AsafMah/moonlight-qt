import os
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "resolve-update-repository.py"


class AppImageUpdateMetadataTests(unittest.TestCase):
    def run_metadata(self, arch, build_environment):
        environment = os.environ.copy()
        environment.pop("GITHUB_ACTIONS", None)
        environment.pop("GITHUB_REPOSITORY", None)
        environment.update(build_environment)
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--appimage-arch", arch, "--appimage-tag", "latest"],
            capture_output=True,
            text=True,
            timeout=10,
            env=environment,
        )

    def test_ci_fork_repository_is_embedded_for_both_architectures(self):
        for arch in ("x86_64", "aarch64"):
            with self.subTest(arch=arch):
                result = self.run_metadata(
                    arch,
                    {"GITHUB_ACTIONS": "true", "GITHUB_REPOSITORY": "AsafMah/moonlight-qt"},
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(
                    result.stdout.strip(),
                    f"gh-releases-zsync|AsafMah|moonlight-qt|latest|Moonlight-VPlus-*-{arch}.AppImage.zsync",
                )

    def test_non_ci_build_keeps_upstream_default(self):
        result = self.run_metadata(
            "x86_64",
            {"GITHUB_REPOSITORY": "AsafMah/moonlight-qt"},
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(
            result.stdout.strip(),
            "gh-releases-zsync|qiin2333|moonlight-qt|latest|Moonlight-VPlus-*-x86_64.AppImage.zsync",
        )

    def test_ci_rejects_malformed_repository(self):
        result = self.run_metadata(
            "aarch64",
            {"GITHUB_ACTIONS": "true", "GITHUB_REPOSITORY": "not-a-repository"},
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("GITHUB_REPOSITORY", result.stderr)

    def test_application_and_appimage_share_repository_resolver(self):
        app_pro = (ROOT / "app" / "app.pro").read_text(encoding="utf-8")
        appimage_script = (ROOT / "scripts" / "build-appimage.sh").read_text(encoding="utf-8")
        resolver = "resolve-update-repository.py"
        self.assertIn(resolver, app_pro)
        self.assertIn(resolver, appimage_script)

    def test_appimage_stages_and_verifies_the_desktop_icon(self):
        appimage_script = (ROOT / "scripts" / "build-appimage.sh").read_text(encoding="utf-8")
        desktop_entry = (
            ROOT / "app" / "deploy" / "linux" / "com.moonlight_stream.Moonlight.desktop"
        ).read_text(encoding="utf-8")
        self.assertIn("Icon=moonlight", desktop_entry)
        self.assertIn(
            'cp "$APP_ICON_SOURCE" "$DEPLOY_FOLDER/moonlight.png"',
            appimage_script,
        )
        self.assertIn('[ -s "$EXTRACTED_APPDIR/.DirIcon" ]', appimage_script)
        self.assertIn('[ -s "$EXTRACTED_APPDIR/moonlight.png" ]', appimage_script)


if __name__ == "__main__":
    unittest.main()
