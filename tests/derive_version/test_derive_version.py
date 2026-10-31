import importlib.util
import os
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest import mock


SCRIPT_PATH = Path(__file__).resolve().parents[2] / "scripts" / "derive-version.py"
SPEC = importlib.util.spec_from_file_location("derive_version", SCRIPT_PATH)
assert SPEC is not None and SPEC.loader is not None
derive_version = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(derive_version)


class DeriveVersionTests(unittest.TestCase):
    def derive_from_ci(self, version: str) -> dict[str, str]:
        with (
            mock.patch.dict(os.environ, {"CI_VERSION": version}),
            mock.patch.object(derive_version, "run_git", return_value=None),
            mock.patch.object(
                derive_version, "read_fallback_version", return_value="6.2.84"
            ),
        ):
            return derive_version.derive(Path("."))

    def derive_from_git(self, describe: str) -> dict[str, str]:
        def run_git(_source_root: Path, *args: str) -> str:
            if args and args[0] == "describe":
                return describe
            if args == ("rev-parse", "--short=8", "HEAD"):
                return "feedface"
            raise AssertionError(f"Unexpected git command: {args}")

        with (
            mock.patch.dict(os.environ, {"CI_VERSION": ""}),
            mock.patch.object(derive_version, "run_git", side_effect=run_git),
            mock.patch.object(
                derive_version, "read_fallback_version", return_value="6.2.84"
            ),
        ):
            return derive_version.derive(Path("."))

    def test_exact_stable_tag(self) -> None:
        result = self.derive_from_git("v6.3.13-0-g12345678")

        self.assertEqual(result["display"], "6.3.13")
        self.assertEqual(result["artifact"], "6.3.13")
        self.assertEqual(result["numeric"], "6.3.13.0")

    def test_fork_release_revision_survives_exact_tag_and_distance(self) -> None:
        self.assertEqual(self.derive_from_git("v6.5.3.181-0-ge8ef36a8")["numeric"],
                         "6.5.3.181")
        local = self.derive_from_git("v6.5.3.181-2-gfeedface")
        self.assertEqual(local["numeric"], "6.5.3.183")
        self.assertEqual(self.derive_from_ci(local["artifact"])["numeric"], "6.5.3.183")

    def test_exact_legacy_dot_prerelease_tag(self) -> None:
        result = self.derive_from_git("v6.3.10.beta-0-g4af38f30")

        self.assertEqual(result["display"], "6.3.10.beta")
        self.assertEqual(result["artifact"], "6.3.10.beta")
        self.assertEqual(result["numeric"], "6.3.10.0")

    def test_exact_semver_prerelease_tag(self) -> None:
        result = self.derive_from_git("v6.3.14-beta.1-0-gabcdef12")

        self.assertEqual(result["display"], "6.3.14-beta.1")
        self.assertEqual(result["artifact"], "6.3.14-beta.1")
        self.assertEqual(result["numeric"], "6.3.14.0")

    def test_commits_after_prerelease_tag_keep_git_metadata(self) -> None:
        result = self.derive_from_git(
            "v6.3.14-beta.1-3-gabcdef123456-dirty"
        )

        self.assertEqual(
            result["display"], "6.3.14-beta.1+3.gabcdef12.dirty"
        )
        self.assertEqual(
            result["artifact"], "6.3.14-beta.1-3.gabcdef12.dirty"
        )
        self.assertEqual(result["numeric"], "6.3.14.3")

    def test_explicit_version_strips_tag_prefix(self) -> None:
        with (
            mock.patch.dict(os.environ, {"CI_VERSION": "v6.3.15-rc.1"}),
            mock.patch.object(derive_version, "run_git", return_value="feedface"),
            mock.patch.object(
                derive_version, "read_fallback_version", return_value="6.2.84"
            ),
        ):
            result = derive_version.derive(Path("."))

        self.assertEqual(result["display"], "6.3.15-rc.1")
        self.assertEqual(result["artifact"], "6.3.15-rc.1")
        self.assertEqual(result["numeric"], "6.3.15.0")

    def test_ci_freeze_preserves_field_report_build_revisions(self) -> None:
        # CI freezes the artifact version before setup/build dirties the checkout.
        for count, sha in ((143, "ef57952a"), (162, "2e8989e6"), (163, "feedface")):
            with self.subTest(count=count):
                local = self.derive_from_git(f"v6.5.3-{count}-g{sha}")
                frozen = self.derive_from_ci(local["artifact"])
                self.assertEqual(frozen["artifact"], f"6.5.3-{count}.g{sha}")
                self.assertEqual(frozen["numeric"], f"6.5.3.{count}")
                self.assertEqual(frozen["numeric"], local["numeric"])

    def test_explicit_git_versions_preserve_revision_without_git_metadata(self) -> None:
        for version in (
            "v6.5.3+162.g2e8989e6",
            "6.5.3-162.g2e8989e6",
            "6.5.3+162.g2e8989e6.dirty",
            "6.5.3-beta.1-162.g2e8989e6",
            "6.5.3.beta+162.g2e8989e6",
        ):
            with self.subTest(version=version):
                self.assertEqual(self.derive_from_ci(version)["numeric"], "6.5.3.162")

    def test_explicit_four_part_version_preserves_revision(self) -> None:
        self.assertEqual(self.derive_from_ci("v6.5.3.163")["numeric"], "6.5.3.163")

    def test_explicit_stable_and_prerelease_tags_do_not_invent_revisions(self) -> None:
        for version in ("v6.5.3", "6.5.3-beta.162", "6.5.3-162"):
            with self.subTest(version=version):
                self.assertEqual(self.derive_from_ci(version)["numeric"], "6.5.3.0")

    def test_ci_revision_uses_same_windows_limit_as_git_revision(self) -> None:
        self.assertEqual(
            self.derive_from_ci("6.5.3-65536.g12345678")["numeric"], "6.5.3.65535"
        )

    def test_installer_detects_fourth_field_upgrades_and_preserves_rollback(self) -> None:
        root = SCRIPT_PATH.parents[1]
        ns = {"w": "http://wixtoolset.org/schemas/v4/wxs"}
        package = ET.parse(root / "wix" / "Moonlight" / "Product.wxs").find("w:Package", ns)
        self.assertIsNotNone(package)
        self.assertEqual(package.get("Version"), "!(bind.fileVersion.MoonlightExe)")
        upgrade = package.find("w:MajorUpgrade", ns)
        self.assertIsNotNone(upgrade)
        self.assertEqual(upgrade.get("AllowSameVersionUpgrades"), "yes")
        self.assertEqual(upgrade.get("Schedule"), "afterInstallInitialize")
        bundle = ET.parse(root / "wix" / "MoonlightSetup" / "Bundle.wxs").find("w:Bundle", ns)
        self.assertIsNotNone(bundle)
        self.assertEqual(bundle.get("Version"), "!(bind.PackageVersion.Moonlight_x64)")


if __name__ == "__main__":
    unittest.main()
