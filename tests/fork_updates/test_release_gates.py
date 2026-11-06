"""Verify publication gates run before public visibility changes."""
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from test_publication_flow import FakeGh, HEAD, VERSION, publisher

ROOT = Path(__file__).resolve().parents[2]


class ReleaseGatesTests(unittest.TestCase):
    def test_missing_or_unparseable_previous_release_fails_closed(self):
        with self.assertRaisesRegex(RuntimeError, "Previous published release is missing"):
            publisher.verify_previous_update({}, None)
        with self.assertRaisesRegex(RuntimeError, "previous release r43"):
            publisher.verify_previous_update({}, {"tag_name": "r43"})

    def test_updater_failure_cannot_publish(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("Portable-x64", "Portable-arm64", "Setup"):
                suffix = ".exe" if name == "Setup" else ".zip"
                package = root / f"Moonlight-VPlus-{name}-{VERSION}{suffix}"
                if name == "Setup":
                    package.write_bytes(b"PE")
                else:
                    with zipfile.ZipFile(package, "w") as archive:
                        archive.writestr("Moonlight.exe", b"PE")
            assets = [{"name": p.name, "digest": "sha256:" + publisher.sha256(p)}
                      for p in sorted(root.iterdir())]
            fake = FakeGh(assets)
            environment = dict(GITHUB_ACTIONS="true", GITHUB_REF="refs/heads/integration",
                               GITHUB_EVENT_NAME="push", GITHUB_REPOSITORY="AsafMah/moonlight-qt",
                               GITHUB_SHA=HEAD, GITHUB_RUN_NUMBER="47", GITHUB_RUN_ID="123",
                               GITHUB_SERVER_URL="https://github.com", GH_RELEASE_TOKEN="test")

            def output(command, **kwargs):
                return "" if command[0] == "git" else (
                    VERSION if command[-1] == "artifact" else "6.5.3.184")

            with patch.dict(os.environ, environment), \
                    patch.object(sys, "argv", ["publisher", "--artifacts", str(root)]), \
                    patch.object(publisher.subprocess, "check_output", side_effect=output), \
                    patch.object(publisher.subprocess, "run", side_effect=fake.run), \
                    patch.object(publisher, "pe_file_version", return_value="6.5.3.184"), \
                    patch.object(publisher, "verify_previous_update", create=True,
                                 side_effect=RuntimeError("Updater rejected candidate")) as replay:
                with self.assertRaisesRegex(RuntimeError, "Updater rejected candidate"):
                    publisher.main()
            replay.assert_called_once()
            self.assertTrue(fake.releases[0]["draft"])
            self.assertFalse(any("PATCH" in call for call in fake.calls))

    def test_ci_installs_gate_dependencies_before_publication(self):
        workflow = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")
        release = workflow.split("\n  release:\n")[1]
        self.assertIn("release-gates-requirements.txt", release)
        self.assertIn("qt6-base-dev", release)
        self.assertLess(release.index("release-gates-requirements.txt"),
                        release.index("--artifacts"))

    def test_production_updater_replay_accepts_upgrade_and_rejects_non_upgrade(self):
        path = ROOT / "scripts/verify-release-update.py"
        self.assertTrue(path.is_file(), "The release-time production updater replay is missing")
        spec = importlib.util.spec_from_file_location("update_gate", path)
        gate = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(gate)
        for tag in ("v6.5.4.1", "v6.5.3.229"):
            with self.subTest(tag=tag):
                candidate = {
                    "tag_name": tag, "draft": True, "prerelease": False,
                    "body": "Release notes " * 8000,
                    "html_url": f"https://github.com/AsafMah/moonlight-qt/releases/tag/{tag}",
                    "assets": [],
                }
                for arch in ("x64", "arm64"):
                    filename = f"Moonlight-VPlus-Portable-{arch}-{tag[1:]}.zip"
                    candidate["assets"].append({
                        "name": filename,
                        "digest": "sha256:" + "a" * 64,
                        "browser_download_url": "https://github.com/AsafMah/moonlight-qt/"
                                                f"releases/download/{tag}/{filename}",
                    })
                candidate["assets"].append({
                    "name": f"Moonlight-VPlus-Setup-{tag[1:]}.exe", "digest": "sha256:" + "a" * 64,
                    "browser_download_url": f"https://github.com/AsafMah/moonlight-qt/releases/download/{tag}/"
                                            f"Moonlight-VPlus-Setup-{tag[1:]}.exe",
                })
                if tag == "v6.5.4.1":
                    gate.verify_update(candidate, "v6.5.3.229")
                else:
                    with self.assertRaisesRegex(AssertionError, "Candidate is not newer"):
                        gate.verify_update(candidate, "v6.5.3.229")
