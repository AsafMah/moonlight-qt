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
    def test_returned_release_identity_cannot_drift(self):
        release = {"tag_name": "v6.5.4.225", "target_commitish": "a" * 40}
        publisher.verify_release_identity(release, "v6.5.4.225", "a" * 40)
        for field in ("tag_name", "target_commitish"):
            with self.subTest(field=field), self.assertRaisesRegex(RuntimeError, "tag or target"):
                publisher.verify_release_identity(dict(release, **{field: "wrong"}),
                                                  "v6.5.4.225", "a" * 40)

    def test_numeric_identity_rejects_mismatched_names_and_tag(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "Moonlight-VPlus-Portable-x64-6.5.4-225.gc4f8fd54.zip"
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr("Moonlight.exe", b"fixture PE")
            with patch.object(publisher, "pe_file_version", return_value="6.5.4.225",
                              create=True):
                publisher.verify_package_versions([path], "6.5.4.225", "v6.5.4.225")
                with self.assertRaisesRegex(RuntimeError, "asset.*version"):
                    publisher.verify_package_versions([path], "6.5.4.226", "v6.5.4.226")
                with self.assertRaisesRegex(RuntimeError, "release tag"):
                    publisher.verify_package_versions([path], "6.5.4.225", "v6.5.3.229")

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
