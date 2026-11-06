"""Fake the GitHub REST visibility rules; no network or release publication."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("flow_publisher", ROOT / "scripts/publish-integration-release.py")
publisher = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(publisher)
HEAD = "a" * 40
TAG = "v6.5.3.184"
VERSION = "6.5.3.181-3.gaaaaaaaa"


class FakeGh:
    def __init__(self, assets, orphan=False, latest="v6.5.3.181", workflows_match=True):
        self.assets = assets
        self.releases = [{"id": 42, "tag_name": TAG, "draft": True,
                          "target_commitish": HEAD, "assets": [], "html_url": "https://example/release"}] if orphan else []
        self.latest = latest
        self.calls = []
        self.workflows_match = workflows_match

    def run(self, command, **kwargs):
        args = command[1:]
        self.calls.append(args)
        output = ""
        error = ""
        code = 0
        if args[:2] == ["release", "create"]:
            self.releases.append({"id": 42, "tag_name": TAG, "draft": True,
                                  "target_commitish": HEAD, "assets": [],
                                  "html_url": "https://example/release"})
            # Legacy gh create-with-assets uploads immediately.
            self.releases[-1]["assets"] = self.assets if any(str(a).endswith(".zip") for a in args) else []
        elif args[:2] == ["release", "upload"]:
            self.releases[0]["assets"] = self.assets
        elif args[0] == "api":
            endpoint = args[1]
            if endpoint == "repos/AsafMah/moonlight-qt":
                output = json.dumps({"default_branch": "integration"})
            elif "/commits/" in endpoint:
                ref = endpoint.rsplit("/", 1)[1]
                output = json.dumps({"commit": {"tree": {"sha": ref}}})
            elif "/git/trees/" in endpoint:
                tree = endpoint.rsplit("/", 1)[1]
                if tree in ("integration", HEAD):
                    output = json.dumps({"tree": [{"path": ".github", "type": "tree", "sha": tree + "-github"}]})
                else:
                    sha = "matching" if self.workflows_match else tree
                    output = json.dumps({"tree": [{"path": "workflows", "type": "tree", "sha": sha}]})
            elif endpoint.endswith("/latest"):
                output = json.dumps({"tag_name": self.latest})
            elif endpoint.endswith("/releases?per_page=100&page=1"):
                output = json.dumps(self.releases)
            elif endpoint.endswith("/releases"):
                self.releases.append({"id": 42, "tag_name": TAG, "draft": True,
                                      "target_commitish": HEAD, "assets": [],
                                      "html_url": "https://example/release"})
                output = json.dumps(self.releases[-1])
            elif "/releases/42/assets?name=" in endpoint:
                name = endpoint.split("name=")[1]
                self.releases[0]["assets"].append(next(a for a in self.assets if a["name"] == name))
                output = "{}"
            elif "/releases/tags/" in endpoint:
                published = [r for r in self.releases if not r["draft"]]
                if published:
                    output = json.dumps(published[0])
                else:
                    code, error = 1, "gh: Not Found (HTTP 404)"
            elif endpoint.endswith("/releases/42"):
                if "PATCH" in args:
                    self.releases[0]["draft"] = False
                output = json.dumps(self.releases[0])
            else:
                raise AssertionError(f"Unexpected fake gh endpoint {args}")
        else:
            raise AssertionError(f"Unexpected fake gh invocation {args}")
        return subprocess.CompletedProcess(command, code, output, error)


class PublicationFlowTests(unittest.TestCase):
    def test_draft_creation_resume_and_unknown_latest(self):
        for orphan, latest, override, workflows_match in (
                (False, "v6.5.3.181", True, True),
                (True, "v6.5.3.181", True, True),
                (False, "r43", True, True),
                (False, "v6.5.3.181", False, True),
                (False, "v6.5.3.181", False, False)):
            with self.subTest(orphan=orphan, latest=latest), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                for name in ("Portable-x64", "Portable-arm64", "Setup"):
                    suffix = ".exe" if name == "Setup" else ".zip"
                    package = root / f"Moonlight-VPlus-{name}-{VERSION}{suffix}"
                    if suffix == ".zip":
                        with zipfile.ZipFile(package, "w") as archive:
                            archive.writestr("Moonlight.exe", b"verified PE")
                    else:
                        package.write_bytes(b"verified PE")
                assets = [{"name": p.name, "digest": "sha256:" + publisher.sha256(p)} for p in sorted(root.iterdir())]
                fake = FakeGh(assets, orphan, latest, workflows_match)
                environment = dict(GITHUB_ACTIONS="true", GITHUB_REF="refs/heads/integration",
                                   GITHUB_EVENT_NAME="push", GITHUB_REPOSITORY="AsafMah/moonlight-qt",
                                   GITHUB_SHA=HEAD, GITHUB_RUN_NUMBER="47", GITHUB_RUN_ID="123",
                                   GITHUB_SERVER_URL="https://github.com",
                                   GH_RELEASE_TOKEN="test-token" if override else "")

                def check_output(command, **kwargs):
                    return "" if command[0] == "git" else VERSION if command[-1] == "artifact" else "6.5.3.184"

                with patch.dict(os.environ, environment), patch.object(sys, "argv", ["publisher", "--artifacts", str(root)]), \
                        patch.object(publisher.subprocess, "check_output", side_effect=check_output), \
                        patch.object(publisher, "pe_file_version", return_value="6.5.3.184"), \
                        patch.object(publisher, "verify_previous_update") as replay, \
                        patch.object(publisher.subprocess, "run", side_effect=fake.run):
                    if not override and not workflows_match:
                        with self.assertRaisesRegex(RuntimeError, "default branch integration"):
                            publisher.main()
                            replay.assert_called_once()
                        self.assertEqual(fake.releases, [], "Preflight must fail before creating any draft")
                        continue
                    publisher.main()
                self.assertEqual(len(fake.releases), 1, "An orphan draft must be resumed, not duplicated")
                self.assertFalse(fake.releases[0]["draft"])
                self.assertEqual(sorted(fake.releases[0]["assets"], key=lambda a: a["name"]), assets)
                self.assertEqual(sum(c[0] == "api" and c[1].endswith("/releases") and "POST" in c for c in fake.calls), 0 if orphan else 1)
                self.assertTrue(any("/releases/42" in c[1] and "PATCH" in c for c in fake.calls))
                if latest == "r43":
                    self.assertTrue(any("make_latest=false" in c for c in fake.calls))

    def test_dirty_checkout_with_untracked_file_fails_before_release_api(self):
        environment = dict(GITHUB_ACTIONS="true", GITHUB_REF="refs/heads/integration",
                           GITHUB_EVENT_NAME="push", GITHUB_REPOSITORY="AsafMah/moonlight-qt",
                           GITHUB_SHA=HEAD, GITHUB_RUN_NUMBER="47", GITHUB_RUN_ID="123",
                           GITHUB_SERVER_URL="https://github.com", GH_RELEASE_TOKEN="test-token")
        fake = FakeGh([])
        with patch.dict(os.environ, environment), \
                patch.object(sys, "argv", ["publisher", "--artifacts", "."]), \
                patch.object(publisher.subprocess, "check_output", return_value="?? local-scratch.txt\n"), \
                patch.object(publisher.subprocess, "run", side_effect=fake.run):
            with self.assertRaisesRegex(RuntimeError, "dirty source checkout"):
                publisher.main()
        self.assertEqual(fake.calls, [], "Dirty local contents must not reach release APIs")
