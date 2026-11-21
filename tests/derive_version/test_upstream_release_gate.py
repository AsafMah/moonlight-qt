"""Discover merged qiin releases independently of the maintained manifest."""
import importlib.util
import json
import os
import subprocess
import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("gate_version", ROOT / "scripts/derive-version.py")
version = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(version)


class UpstreamReleaseGateTests(unittest.TestCase):
    def setUp(self):
        manifest = patch.object(version, "upstream_release_tags", return_value={"v6.5.4": "a" * 40})
        self.addCleanup(manifest.stop)
        manifest.start()
        origin = patch.object(version, "verify_manifest_tags_on_origin")
        self.addCleanup(origin.stop)
        origin.start()

    def test_v6_5_6_is_registered_at_the_verified_qiin_release_commit(self):
        release_tags = json.loads(
            (ROOT / "scripts/upstream-release-tags.json").read_text(encoding="utf-8")
        )["tags"]
        self.assertEqual(
            release_tags.get("v6.5.6"),
            "490923396a0bc53350dfadf88d5cf39bc9a39362",
        )

    def test_unreachable_anchor_fails_even_with_explicit_ci_version(self):
        with patch.dict(os.environ, {"CI_VERSION": "6.5.4.225"}), \
                patch.object(version, "upstream_release_tags",
                             side_effect=RuntimeError("anchor is not reachable")):
            with self.assertRaisesRegex(RuntimeError, "anchor is not reachable"):
                version.verify_upstream_release_base(ROOT)

    def test_unregistered_reachable_upstream_release_fails(self):
        remote = ("a" * 40 + "\trefs/tags/v6.5.4\n"
                  + "b" * 40 + "\trefs/tags/v6.5.5\n")
        with patch.object(version.subprocess, "check_output", return_value=remote), \
                patch.object(version, "run_git", return_value=""), \
                patch.object(version, "derive", return_value={"base": "6.5.4"}):
            with self.assertRaisesRegex(RuntimeError, "6.5.5.*derived base 6.5.4"):
                version.verify_upstream_release_base(ROOT)

    def test_unmerged_and_inherited_tags_do_not_advance_the_gate(self):
        remote = ("a" * 40 + "\trefs/tags/v6.5.4\n"
                  + "b" * 40 + "\trefs/tags/v6.5.5\n"
                  + "c" * 40 + "\trefs/tags/v6.5.6-rc.1\n")

        def ancestry(_root, *args):
            self.assertEqual(args[:2], ("merge-base", "--is-ancestor"))
            return "" if args[2] == "a" * 40 else None

        with patch.object(version.subprocess, "check_output", return_value=remote), \
                patch.object(version, "run_git", side_effect=ancestry), \
                patch.object(version, "derive", return_value={"base": "6.5.4"}):
            version.verify_upstream_release_base(ROOT)

    def test_annotated_upstream_tag_uses_peeled_commit(self):
        remote = ("d" * 40 + "\trefs/tags/v6.5.5\n"
                  + "b" * 40 + "\trefs/tags/v6.5.5^{}\n")
        with patch.object(version.subprocess, "check_output", return_value=remote), \
                patch.object(version, "run_git", return_value="") as git, \
                patch.object(version, "derive", return_value={"base": "6.5.4"}):
            with self.assertRaisesRegex(RuntimeError, "6.5.5"):
                version.verify_upstream_release_base(ROOT)
            anchor = json.loads((ROOT / "scripts/upstream-release-tags.json")
                                .read_text(encoding="utf-8"))["anchor"]
            self.assertEqual(git.call_args_list[0].args,
                             (ROOT, "merge-base", "--is-ancestor", "b" * 40, "HEAD"))
            self.assertEqual(git.call_args_list[1].args,
                             (ROOT, "merge-base", "--is-ancestor", anchor, "b" * 40))

    def test_old_inherited_qiin_tag_outside_lineage_is_not_a_release(self):
        remote = "c" * 40 + "\trefs/tags/v6.21.46\n"
        with patch.object(version.subprocess, "check_output", return_value=remote), \
                patch.object(version, "run_git", side_effect=["", None]), \
                patch.object(version, "derive", return_value={"base": "6.5.4"}):
            version.verify_upstream_release_base(ROOT)

    def test_ci_checks_actual_checkout_not_only_synthetic_fixtures(self):
        workflow = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")
        job = workflow.split("\n  version-tests:\n", 1)[1].split("\n  cpp-format:", 1)[0]
        self.assertIn("fetch-depth: 0", job)
        self.assertIn("scripts/derive-version.py --verify-upstream", job)

    def test_version_job_initializes_protocol_submodule_before_reconnect_tests(self):
        workflow = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")
        job = workflow.split("\n  version-tests:\n", 1)[1].split("\n  cpp-format:", 1)[0]
        checkout = "git submodule update --init --depth 1 -- moonlight-common-c/moonlight-common-c"
        self.assertIn(checkout, job)
        self.assertLess(job.index(checkout), job.index("tests/reconnect_video_callbacks"))


class RealHistoryGateTests(unittest.TestCase):
    def test_forgotten_registration_is_blocked_then_registration_passes(self):
        with tempfile.TemporaryDirectory(prefix="moonlight-upstream-gate-") as directory:
            root = Path(directory)

            def git(*args):
                return subprocess.check_output(
                    ["git", "-c", "user.name=Gate Test", "-c",
                     "user.email=gate@example.invalid", *args],
                    cwd=root, text=True, encoding="utf-8").strip()

            git("init", "--quiet")
            git("commit", "--quiet", "--allow-empty", "-m", "upstream anchor")
            git("tag", "v6.5.4")
            anchor = git("rev-parse", "HEAD")
            (root / "scripts").mkdir()
            manifest = root / "scripts/upstream-release-tags.json"
            data = {"repository": "qiin2333/moonlight-qt", "anchor": anchor,
                    "tags": {"v6.5.4": anchor}}
            manifest.write_text(json.dumps(data), encoding="utf-8")
            git("add", "scripts/upstream-release-tags.json")
            git("commit", "--quiet", "-m", "forgot to register newly merged upstream release")
            git("tag", "v6.5.5")
            git("tag", "v6.5.4.225")
            upstream = git("rev-parse", "HEAD")
            remote = f"{anchor}\trefs/tags/v6.5.4\n{upstream}\trefs/tags/v6.5.5\n"
            check_output = subprocess.check_output

            def output(command, **kwargs):
                return remote if command[:2] == ["git", "ls-remote"] else check_output(command, **kwargs)

            with patch.dict(os.environ, {"CI_VERSION": "", "GITHUB_REF": ""}), \
                    patch.object(version.subprocess, "check_output", side_effect=output):
                self.assertEqual(version.derive(root)["base"], "6.5.4")
                with self.assertRaisesRegex(RuntimeError, "6.5.5.*derived base 6.5.4"):
                    version.verify_upstream_release_base(root)
                data["tags"]["v6.5.5"] = upstream
                manifest.write_text(json.dumps(data), encoding="utf-8")
                version.verify_upstream_release_base(root)


class OriginManifestGateTests(unittest.TestCase):
    def verify(self, origin_output):
        expected = {"v6.5.4": "a" * 40, "v6.5.6": "b" * 40}

        def output(command, **kwargs):
            if command[:4] == ["git", "ls-remote", "--tags", "origin"]:
                self.assertEqual(kwargs["cwd"], ROOT)
                self.assertEqual(kwargs["env"]["GIT_TERMINAL_PROMPT"], "0")
                if isinstance(origin_output, Exception):
                    raise origin_output
                return origin_output
            return "a" * 40 + "\trefs/tags/v6.5.4\n"

        with patch.object(version, "upstream_release_tags", return_value=expected), \
                patch.object(version.subprocess, "check_output", side_effect=output), \
                patch.object(version, "derive", return_value={"base": "6.5.6"}):
            version.verify_upstream_release_base(ROOT)

    def test_local_tags_cannot_hide_missing_origin_tag(self):
        with self.assertRaisesRegex(RuntimeError, "v6.5.6.*origin"):
            self.verify("a" * 40 + "\trefs/tags/v6.5.4\n")

    def test_wrong_origin_commit_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "v6.5.6.*origin"):
            self.verify("a" * 40 + "\trefs/tags/v6.5.4\n"
                            + "c" * 40 + "\trefs/tags/v6.5.6\n")

    def test_lightweight_and_annotated_tags_match_commit(self):
        for lines in (
                    ["d" * 40 + "\trefs/tags/v6.5.6", "b" * 40 + "\trefs/tags/v6.5.6^{}"],
                    ["b" * 40 + "\trefs/tags/v6.5.6^{}", "d" * 40 + "\trefs/tags/v6.5.6"],
                    ["b" * 40 + "\trefs/tags/v6.5.6"]):
            with self.subTest(lines=lines):
                self.verify("a" * 40 + "\trefs/tags/v6.5.4\n" + "\n".join(lines) + "\n")

    def test_tag_object_sha_does_not_override_wrong_peeled_commit(self):
        with self.assertRaisesRegex(RuntimeError, "v6.5.6.*origin"):
            self.verify("a" * 40 + "\trefs/tags/v6.5.4\n"
                            + "b" * 40 + "\trefs/tags/v6.5.6\n"
                            + "c" * 40 + "\trefs/tags/v6.5.6^{}\n")

    def test_origin_network_failure_is_not_success(self):
        for error in (subprocess.CalledProcessError(1, ["git", "ls-remote"]),
                          subprocess.TimeoutExpired(["git", "ls-remote"], 60), OSError("offline")):
            with self.subTest(error=error), self.assertRaises(type(error)):
                self.verify(error)
