import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("publisher", ROOT / "scripts/publish-integration-release.py")
publisher = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(publisher)


class PublisherTests(unittest.TestCase):
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
