from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ReleaseWorkflowTests(unittest.TestCase):
    def test_no_release_published_rebuild(self):
        workflow = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")
        self.assertFalse("types: [ published ]" in workflow,
                         "Publishing a release must not rebuild or replace its verified bytes")

    def test_green_integration_release_and_token_fallback(self):
        workflow = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")
        self.assertIn("branches: [ master, main, integration ]", workflow)
        release = workflow[workflow.index("\n  release:\n"):]
        self.assertIn("github.ref == 'refs/heads/integration'", release)
        self.assertIn("github.token", release)
        self.assertIn("secrets.GH_RELEASE_TOKEN", release)
        self.assertLess(release.index("--preflight"), release.index("actions/download-artifact"))
        self.assertNotIn("secrets.GH_BOT_TOKEN", release)
        global_permissions = workflow.split("concurrency:")[0]
        self.assertNotIn("contents: write", global_permissions)
        self.assertIn("contents: write", release)
        self.assertIn("needs: [qml-lint, version-tests, cpp-format, build-windows, build-macos, build-linux, build-steamlink]",
                      release)
        self.assertNotIn("softprops/action-gh-release", release,
                         "Verified artifact publication must not silently overwrite existing assets")

    def test_side_branch_cannot_publish(self):
        workflow = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")
        release = workflow[workflow.index("\n  release:\n"):]
        self.assertIn("github.ref == 'refs/heads/integration'", release)
        self.assertNotIn("fix/updater-release-batch", workflow)
