import importlib.util
import json
import os
import subprocess
import tempfile
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
    def test_git_tag_output_is_decoded_as_utf8(self) -> None:
        # Git emits UTF-8 tag names even on a Windows ANSI-code-page runner.
        output = "v6.5.4\nunrelated-\u201d-tag".encode("utf-8")

        def check_output(*args, **kwargs):
            return output.decode(kwargs.get("encoding", "cp1252"))

        with mock.patch.object(derive_version.subprocess, "check_output",
                               side_effect=check_output):
            self.assertEqual(derive_version.run_git(Path("."), "tag", "--merged", "HEAD"),
                             output.decode("utf-8"))

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
            if args == ("tag", "--merged", "HEAD"):
                return ""
            raise AssertionError(f"Unexpected git command: {args}")

        with (
            mock.patch.dict(os.environ, {"CI_VERSION": ""}),
            mock.patch.object(derive_version, "run_git", side_effect=run_git),
            mock.patch.object(derive_version, "upstream_release_tags", return_value=None),
            mock.patch.object(
                derive_version, "read_fallback_version", return_value="6.2.84"
            ),
        ):
            return derive_version.derive(Path("."))

    def test_reachable_upstream_base_overrides_nearer_fork_release(self) -> None:
        with tempfile.TemporaryDirectory(prefix="moonlight-version-") as directory:
            root = Path(directory)

            def git(*args: str) -> str:
                return subprocess.check_output(
                    ["git", "-c", "user.name=Version Test", "-c",
                     "user.email=version-test@example.invalid", *args],
                    cwd=root, text=True,
                ).strip()

            git("init", "--quiet")
            git("commit", "--quiet", "--allow-empty", "-m", "inherited numbering")
            git("tag", "v6.21.46")
            git("commit", "--quiet", "--allow-empty", "-m", "upstream")
            git("tag", "v6.5.4")
            anchor = git("rev-parse", "HEAD")
            (root / "scripts").mkdir()
            manifest = root / "scripts" / "upstream-release-tags.json"
            manifest.write_text(json.dumps({
                "repository": "qiin2333/moonlight-qt", "anchor": anchor,
                "tags": {"v6.5.4": anchor},
            }), encoding="utf-8")
            git("add", "scripts/upstream-release-tags.json")
            git("commit", "--quiet", "--allow-empty", "-m", "fork release")
            git("tag", "v6.5.3.229")
            with mock.patch.dict(os.environ, {"CI_VERSION": ""}):
                result = derive_version.derive(root)
            self.assertEqual(result["base"], "6.5.4")
            self.assertEqual(result["numeric"], "6.5.4.1")
            self.assertEqual(self.derive_from_ci(result["artifact"])["numeric"], "6.5.4.1")

            git("tag", "v6.5.4.12")
            git("commit", "--quiet", "--allow-empty", "-m", "next fork revision")
            git("tag", "v6.5.3.230")
            with mock.patch.dict(os.environ, {"CI_VERSION": ""}):
                result = derive_version.derive(root)
            self.assertEqual(result["numeric"], "6.5.4.13")
            self.assertEqual(self.derive_from_ci(result["artifact"])["numeric"], "6.5.4.13")

            # A future release tag not yet merged must not advance this build.
            head = git("rev-parse", "HEAD")
            git("checkout", "--quiet", "--detach")
            git("commit", "--quiet", "--allow-empty", "-m", "unmerged future upstream")
            git("tag", "v6.5.40")
            git("checkout", "--quiet", "--detach", head)
            with mock.patch.dict(os.environ, {"CI_VERSION": ""}):
                self.assertEqual(derive_version.derive(root)["numeric"], "6.5.4.13")
            git("tag", "v6.99.0")
            with mock.patch.dict(os.environ, {"CI_VERSION": ""}):
                self.assertEqual(derive_version.derive(root)["numeric"], "6.5.4.13")
            for tag, commit, message in (
                ("v6.5.5", anchor, "missing or has the wrong commit"),
                ("v6.21.46", git("rev-parse", "v6.21.46"), "outside the release lineage"),
            ):
                with self.subTest(tag=tag):
                    manifest.write_text(json.dumps({
                        "repository": "qiin2333/moonlight-qt", "anchor": anchor,
                        "tags": {"v6.5.4": anchor, tag: commit},
                    }), encoding="utf-8")
                    with mock.patch.dict(os.environ, {"CI_VERSION": ""}):
                        with self.assertRaisesRegex(RuntimeError, message):
                            derive_version.derive(root)
            git("tag", "v6.5.5")
            manifest.write_text(json.dumps({
                "repository": "qiin2333/moonlight-qt", "anchor": anchor,
                "tags": {"v6.5.4": anchor, "v6.5.5": head},
            }), encoding="utf-8")
            with mock.patch.dict(os.environ, {"CI_VERSION": ""}):
                self.assertEqual(derive_version.derive(root)["base"], "6.5.5")
            manifest.write_text(json.dumps({
                "repository": "qiin2333/moonlight-qt",
                "anchor": git("rev-parse", "v6.5.40"),
                "tags": {"v6.5.4": anchor},
            }), encoding="utf-8")
            with mock.patch.dict(os.environ, {"CI_VERSION": ""}):
                with self.assertRaisesRegex(RuntimeError, "anchor is not reachable"):
                    derive_version.derive(root)

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
