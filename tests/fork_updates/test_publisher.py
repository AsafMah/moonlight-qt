import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("publisher", ROOT / "scripts/publish-integration-release.py")
publisher = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(publisher)


class PublisherTests(unittest.TestCase):
    def test_real_a7b37240_asset_names_are_rejected(self):
        names = (
            "Portable-x64-6.5.4.225-7.ga7b37240.zip",
            "Portable-arm64-6.5.4.225-7.ga7b37240.zip",
            "Setup-6.5.4.225-7.ga7b37240.exe",
            "6.5.4.225-7.ga7b37240-arm64.dmg",
            "6.5.4.225-7.ga7b37240-x86_64.AppImage",
            "6.5.4.225-7.ga7b37240-aarch64.AppImage.zsync",
            "SteamLink-6.5.4.225-7.ga7b37240.zip",
        )
        with tempfile.TemporaryDirectory() as directory:
            for name in names:
                path = Path(directory) / ("Moonlight-VPlus-" + name)
                if name.startswith("Portable-"):
                    with zipfile.ZipFile(path, "w") as archive:
                        archive.writestr("Moonlight.exe", b"fixture PE")
                else:
                    path.write_bytes(b"fixture")
                with self.subTest(name=name), \
                        patch.object(publisher, "pe_file_version", return_value="6.5.4.232"):
                    with self.assertRaisesRegex(RuntimeError, "asset.*version"):
                        publisher.verify_package_versions([path], "6.5.4.232", "v6.5.4.232")

    def test_display_version_must_match_even_when_file_version_matches(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "Moonlight-VPlus-Portable-x64-6.5.4.232.zip"
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr("Moonlight.exe", b"fixture PE")
            with patch.object(publisher, "pe_file_version", return_value="6.5.4.232"), \
                    patch.object(publisher, "pe_display_version",
                                 return_value="6.5.4.225-7.ga7b37240", create=True):
                with self.assertRaisesRegex(RuntimeError, "display version"):
                    publisher.verify_package_versions([path], "6.5.4.232", "v6.5.4.232")

    def test_returned_release_identity_cannot_drift(self):
        release = {"tag_name": "v6.5.4.225", "target_commitish": "a" * 40}
        publisher.verify_release_identity(release, "v6.5.4.225", "a" * 40)
        for field in ("tag_name", "target_commitish"):
            with self.subTest(field=field), self.assertRaisesRegex(RuntimeError, "tag or target"):
                publisher.verify_release_identity(dict(release, **{field: "wrong"}),
                                                  "v6.5.4.225", "a" * 40)

    def test_numeric_identity_rejects_mismatched_names_and_tag(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "Moonlight-VPlus-Portable-x64-6.5.4.225.zip"
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr("Moonlight.exe", b"fixture PE")
            with patch.object(publisher, "pe_file_version", return_value="6.5.4.225"), \
                    patch.object(publisher, "pe_display_version", return_value="6.5.4.225"):
                publisher.verify_package_versions([path], "6.5.4.225", "v6.5.4.225")
                with self.assertRaisesRegex(RuntimeError, "asset.*version"):
                    publisher.verify_package_versions([path], "6.5.4.226", "v6.5.4.226")
                with self.assertRaisesRegex(RuntimeError, "release tag"):
                    publisher.verify_package_versions([path], "6.5.4.225", "v6.5.3.229")

    def test_all_exact_release_asset_names_pass(self):
        names = (
            "Portable-x64-6.5.4.232.zip", "Portable-arm64-6.5.4.232.zip",
            "Setup-6.5.4.232.exe", "6.5.4.232-arm64.dmg",
            "6.5.4.232-x86_64.AppImage", "6.5.4.232-aarch64.AppImage.zsync",
            "SteamLink-6.5.4.232.zip",
        )
        with tempfile.TemporaryDirectory() as directory:
            paths = []
            for name in names:
                path = Path(directory) / ("Moonlight-VPlus-" + name)
                if name.startswith("Portable-"):
                    with zipfile.ZipFile(path, "w") as archive:
                        archive.writestr("Moonlight.exe", b"fixture PE")
                else:
                    path.write_bytes(b"fixture")
                paths.append(path)
            with patch.object(publisher, "pe_file_version", return_value="6.5.4.232"), \
                    patch.object(publisher, "pe_display_version", return_value="6.5.4.232") as display:
                publisher.verify_package_versions(paths, "6.5.4.232", "v6.5.4.232")
            self.assertEqual(display.call_count, 2)

    def test_actual_packaged_exe_version_must_match(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "Moonlight-VPlus-Portable-arm64-6.5.4.225.zip"
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr("Moonlight.exe", b"stale PE")
            with patch.object(publisher, "pe_file_version", return_value="6.5.3.229",
                              create=True):
                with self.assertRaisesRegex(RuntimeError, "FileVersion"):
                    publisher.verify_package_versions([path], "6.5.4.225", "v6.5.4.225")

    def test_duplicate_or_missing_packaged_exe_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "Moonlight-VPlus-Portable-x64-6.5.4.225.zip"
            for entries in ([], ["Moonlight.exe", "other/Moonlight.exe"]):
                with self.subTest(entries=entries):
                    with zipfile.ZipFile(path, "w") as archive:
                        for entry in entries:
                            archive.writestr(entry, b"PE")
                    with self.assertRaisesRegex(RuntimeError, "exactly one Moonlight.exe"):
                        publisher.verify_package_versions([path], "6.5.4.225", "v6.5.4.225")

    def test_out_of_order_green_build_must_not_replace_newer_latest(self):
        self.assertGreater(publisher.numeric_tag("v6.5.3.183"), publisher.numeric_tag("v6.5.3.181"))
        self.assertGreater(publisher.numeric_tag("v6.5.4"), publisher.numeric_tag("v6.5.3.183"))
        with self.assertRaises(ValueError):
            publisher.numeric_tag("r43")

    def test_requires_current_version_packages_and_skips_log_wrappers(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            version = "6.5.3.181-2.gfeedface"
            for prefix, suffix in (("Portable-x64-", ".zip"), ("Portable-arm64-", ".zip"),
                                   ("Setup-", ".exe")):
                (root / f"Moonlight-VPlus-{prefix}{version}{suffix}").write_bytes(b"package")
            (root / "windows-upgrade-logs.zip").write_bytes(b"not a package")
            (root / "MoonlightPortable-x64-old.zip").write_bytes(b"legacy duplicate")
            self.assertEqual(len(publisher.packages(root, version)), 3)
            with self.assertRaises(ValueError):
                publisher.packages(root, version + ".dirty")
            with self.assertRaises(ValueError):
                publisher.packages(root, "6.5.3.181-3.gfeedface")
