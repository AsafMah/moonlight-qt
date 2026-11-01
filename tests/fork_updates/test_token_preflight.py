import importlib.util
import os
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("token_publisher", ROOT / "scripts/publish-integration-release.py")
publisher = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(publisher)


class TokenPreflightTests(unittest.TestCase):
    def test_builtin_token_fails_before_creation_when_workflows_differ(self):
        with patch.dict(os.environ, {"GH_RELEASE_TOKEN": ""}), \
                patch.object(publisher, "gh", return_value='{"default_branch":"master"}'), \
                patch.object(publisher, "workflow_tree", side_effect=["old-tree", "new-tree"]):
            with self.assertRaisesRegex(RuntimeError, "GITHUB_TOKEN cannot create"):
                publisher.preflight_token("AsafMah/moonlight-qt", "a" * 40)

    def test_explicit_token_or_matching_workflows_can_proceed(self):
        with patch.dict(os.environ, {"GH_RELEASE_TOKEN": "test"}), patch.object(publisher, "gh") as api:
            publisher.preflight_token("AsafMah/moonlight-qt", "a" * 40)
            api.assert_not_called()
        with patch.dict(os.environ, {"GH_RELEASE_TOKEN": ""}), \
                patch.object(publisher, "gh", return_value='{"default_branch":"integration"}'), \
                patch.object(publisher, "workflow_tree", return_value="matching-tree"):
            publisher.preflight_token("AsafMah/moonlight-qt", "a" * 40)
