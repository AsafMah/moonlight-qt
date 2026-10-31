"""Replay Core respawn against the production connection-start HTTP block."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SESSION = ROOT / "app" / "streaming" / "session.cpp"


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}\n", start) + 3]


class HostRestartRecoveryTests(unittest.TestCase):
    def test_production_retry_requests(self):
        source = SESSION.read_text(encoding="utf-8")
        snapshot = source[source.index("struct HostConnectionInfoSnapshot"):
                          source.index("struct PreparedServerInformation")]
        update = function(source, "void updateHostConnectionInfoFromServerInfo(")
        body = function(source, "bool Session::startConnectionAsync(")
        prelude = body[body.index("HostConnectionInfoSnapshot hostConnectionInfo ="):
                       body.index("    QString rtspSessionUrl;")]
        start = body.index("    try {\n        NvHTTP http(m_Computer);")
        end = body.index("    PreparedServerInformation preparedHostInfo;")
        block = body[start:end]
        harness = Path(__file__).with_name("host_restart_harness.cpp").read_text(encoding="utf-8")
        harness = harness.replace("// HOST_SNAPSHOT", snapshot)
        harness = harness.replace("// HOST_UPDATE", update)
        harness = harness.replace("// ATTEMPT_PRELUDE", prelude)
        harness = harness.replace("// ATTEMPT_HTTP", block)
        http_source = (ROOT / "app" / "backend" / "nvhttp.cpp").read_text(encoding="utf-8")
        http_methods = "\n".join(function(http_source, signature) for signature in (
            "int\nNvHTTP::getCurrentGame(",
            "QString\nNvHTTP::getXmlString(",
            "QString NvHTTP::getAuthenticatedServerInfo(",
        ))
        harness = harness.replace("// AUTHENTICATED_METHODS", http_methods)

        qmake = ((shutil.which("qmake6") or shutil.which("qmake"))
                 if os.name != "nt" else shutil.which("qmake"))
        self.assertIsNotNone(qmake, "Run with Qt qmake and a C++ compiler on PATH")
        with tempfile.TemporaryDirectory(prefix="moonlight-host-restart-") as directory:
            path = Path(directory)
            (path / "main.cpp").write_text(harness, encoding="utf-8")
            (path / "recovery.pro").write_text(
                "QT = core network\nCONFIG += console c++17 release\n"
                "CONFIG -= app_bundle debug debug_and_release\n"
                "TEMPLATE = app\nTARGET = recovery\nSOURCES = main.cpp\n", encoding="utf-8")
            for command in ([qmake, "recovery.pro"],
                            ["nmake", "/nologo"] if os.name == "nt" else ["make", "-s"]):
                result = subprocess.run(command, cwd=path, capture_output=True,
                                        text=True, timeout=120)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            if os.name == "nt":
                executable = next(path.rglob("recovery.exe"))
                local_runner = Path(r"E:\Code\moonlight-tools\run-test-exe.ps1")
                runner = (local_runner if local_runner.is_file()
                          else ROOT / "scripts/run-built-test.ps1")
                command = ["pwsh", "-NoProfile", "-File", str(runner),
                           "-Exe", str(executable), "-Platform", "offscreen"]
            else:
                command = [str(path / "recovery")]
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("host restart recovery passed", result.stdout)

    def test_refresh_requires_authenticated_https(self):
        source = (ROOT / "app" / "backend" / "nvhttp.cpp").read_text(encoding="utf-8")
        body = function(source, "NvHTTP::getAuthenticatedServerInfo(")
        self.assertIn("m_ServerCert.isNull()", body)
        self.assertIn("httpsPort() == 0", body)
        self.assertIn("m_BaseUrlHttps", body)
        self.assertIn("verifyResponseStatus", body)
        self.assertNotRegex(body, r"\bm_BaseUrlHttp\b")
        self.assertNotIn("catch", body)
        # Generic HTTPS transport must keep enforcing the paired certificate.
        transport = function(source, "NvHTTP::openConnection(QUrl")
        self.assertIn("PairedCertificate::enforce", transport)
        self.assertIn("PairedCertificate::matches", transport)

    def test_retry_does_not_assert_against_stale_cached_app(self):
        body = function(SESSION.read_text(encoding="utf-8"), "bool Session::startConnectionAsync(")
        self.assertNotIn("Q_ASSERT(m_Computer->currentGameId", body)

    def test_explicit_graceful_termination_still_excludes_reconnect(self):
        body = function(SESSION.read_text(encoding="utf-8"),
                        "void Session::clConnectionTerminated(")
        start = body.index("case ML_ERROR_GRACEFUL_TERMINATION:")
        self.assertIn("recoverable = false;", body[start:body.index("default:", start)])


if __name__ == "__main__":
    unittest.main()
