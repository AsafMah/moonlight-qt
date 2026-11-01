"""Exercise the production JSON transport with deterministic Qt replies, never a host."""

import os
import json
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    match = re.search(r"\b" + r"\s+".join(re.escape(part) for part in signature.split()), source)
    if match is None:
        raise ValueError(f"Production function not found: {signature}")
    start = match.start()
    return source[start:source.index("\n}\n", start) + 3]


class AbrClientTests(unittest.TestCase):
    def test_measured_feedback_telemetry(self):
        source = (ROOT / "app/streaming/session.cpp").read_text(encoding="utf-8")
        harness = Path(__file__).with_name("telemetry_harness.cpp").read_text(encoding="utf-8")
        harness = harness.replace("// TELEMETRY",
                                  function(source, "void Session::sendSunshineAbrFeedback("))
        self.build_and_run(harness, "telemetry-regressions",
                           ("streaming/stats/streamstatsmodel.cpp", "streaming/stats/statsmath.cpp"))

    def test_overlay_status(self):
        harness = Path(__file__).with_name("overlay_harness.cpp").read_text(encoding="utf-8")
        self.build_and_run(harness, "overlay-regressions", (
            "streaming/video/overlaystatswindow.cpp",
            "streaming/video/overlaystatsstrip.cpp",
            "streaming/video/overlaystatslayout.cpp",
            "streaming/video/overlaystatspreset.cpp",
        ), ("streaming/video/overlaystatswindow.h", "streaming/video/overlayrasterwindow.h"))

    def test_feedback_generation(self):
        source = (ROOT / "app/streaming/session.cpp").read_text(encoding="utf-8")
        task = source[source.index("QString abrHttpErrorDetail"):
                      source.index("QString fileMappingStateName")]
        harness = Path(__file__).with_name("feedback_harness.cpp").read_text(encoding="utf-8")
        header = (ROOT / "app/backend/nvhttp.h").read_text(encoding="utf-8")
        exceptions = header[header.index("class GfeHttpResponseException"):
                            header.index("class NvHTTP")]
        harness = harness.replace("// EXCEPTIONS", exceptions).replace("// FEEDBACK_TASK", task)
        self.build_and_run(harness, "feedback-regressions")

    def test_transient_feedback_recovery(self):
        source = (ROOT / "app/streaming/session.cpp").read_text(encoding="utf-8")
        task = source[source.index("QString abrHttpErrorDetail"):
                      source.index("QString fileMappingStateName")]
        harness = Path(__file__).with_name("feedback_harness.cpp").read_text(encoding="utf-8")
        exceptions = (ROOT / "app/backend/nvhttp.h").read_text(encoding="utf-8")
        exceptions = exceptions[exceptions.index("class GfeHttpResponseException"):
                                exceptions.index("class NvHTTP")]
        self.build_and_run(harness.replace("// EXCEPTIONS", exceptions)
                           .replace("// FEEDBACK_TASK", task),
                           "feedback-regressions", args=["transient-red"])

    def test_ambiguous_initial_configure_retry(self):
        source = (ROOT / "app/streaming/session.cpp").read_text(encoding="utf-8")
        task = source[source.index("QString abrHttpErrorDetail"):
                      source.index("QString fileMappingStateName")]
        harness = Path(__file__).with_name("feedback_harness.cpp").read_text(encoding="utf-8")
        exceptions = (ROOT / "app/backend/nvhttp.h").read_text(encoding="utf-8")
        exceptions = exceptions[exceptions.index("class GfeHttpResponseException"):
                                exceptions.index("class NvHTTP")]
        self.build_and_run(harness.replace("// EXCEPTIONS", exceptions)
                           .replace("// FEEDBACK_TASK", task),
                           "feedback-regressions", args=["ambiguous-red"])

    def test_json_transport(self):
        header = (ROOT / "app/backend/nvhttp.h").read_text(encoding="utf-8")
        exceptions = header[header.index("class GfeHttpResponseException"):
                            header.index("class NvHTTP")]
        source = (ROOT / "app/backend/nvhttp.cpp").read_text(encoding="utf-8")
        transport = function(source, "QNetworkReply*\nNvHTTP::openJsonConnection(")
        parser = function(source, "QJsonObject\nNvHTTP::openJsonConnectionToObject(")
        abr_methods = "\n".join(function(source, signature) for signature in (
            "bool\nNvHTTP::getAbrCapabilities(",
            "QJsonObject\nNvHTTP::configureAbr(",
            "QJsonObject\nNvHTTP::sendAbrFeedback(",
        ))
        harness = Path(__file__).with_name("http_harness.cpp").read_text(encoding="utf-8")
        harness = harness.replace("// EXCEPTIONS", exceptions).replace("// TRANSPORT",
                                                                       transport + "\n" + parser +
                                                                       "\n" + abr_methods)
        self.build_and_run(harness, "http-regressions")

    def build_and_run(self, source, name, sources=(), headers=(), args=()):
        qmake = shutil.which("qmake6") or shutil.which("qmake")
        self.assertIsNotNone(qmake, "Run with existing Qt qmake and compiler on PATH")
        with tempfile.TemporaryDirectory(prefix="moonlight-abr-") as directory:
            path = Path(directory)
            if os.environ.get("OVERLAY_HIDPI_REPRO"):
                (path / "screen.json").write_text(json.dumps({
                    "screens": [{"name": "4k", "width": 3840, "height": 2160,
                                 "logicalDpi": 96, "logicalBaseDpi": 96, "dpr": 1.0}]
                }), encoding="utf-8")
            (path / "main.cpp").write_text(source, encoding="utf-8")
            (path / "abr.pro").write_text(
                "QT = core network gui testlib\nCONFIG += console c++17 release\n"
                "CONFIG -= app_bundle debug debug_and_release\n"
                "TEMPLATE = app\nTARGET = abr\nSOURCES = main.cpp\n"
                f'INCLUDEPATH += "{ROOT.as_posix()}/app"\n' +
                "SOURCES += " + " ".join(f'"{ROOT.as_posix()}/app/{s}"' for s in sources) + "\n" +
                "HEADERS += " + " ".join(f'"{ROOT.as_posix()}/app/{h}"' for h in headers) + "\n",
                encoding="utf-8")
            commands = [[qmake, "abr.pro"],
                        ["nmake", "/nologo"] if os.name == "nt" else ["make", "-s"]]
            for command in commands:
                result = subprocess.run(command, cwd=path, capture_output=True,
                                        text=True, timeout=120)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            if name == "overlay-regressions":
                lrelease = Path(qmake).resolve().with_name(
                    "lrelease.exe" if os.name == "nt" else "lrelease")
                if not lrelease.is_file():
                    query = subprocess.run([qmake, "-query", "QT_HOST_BINS"],
                                           capture_output=True, text=True, timeout=10)
                    self.assertEqual(query.returncode, 0, query.stdout + query.stderr)
                    lrelease = Path(query.stdout.strip()) / "lrelease"
                self.assertTrue(lrelease.is_file(), "Existing Qt lrelease is required")
                for language in ("zh_CN", "zh_TW"):
                    result = subprocess.run([
                        str(lrelease), str(ROOT / f"app/languages/qml_{language}.ts"),
                        "-qm", str(path / f"qml_{language}.qm")
                    ], capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            if os.name == "nt":
                runner = ROOT / "scripts/run-built-test.ps1"
                executable = next(path.rglob("abr.exe"))
                command = ["pwsh", "-NoProfile", "-File", str(runner),
                           "-Exe", str(executable), "-Platform",
                           "offscreen:configfile=screen.json" if os.environ.get("OVERLAY_HIDPI_REPRO")
                           else "offscreen"]
                if args:
                    command.extend(["-TestArgs", *args])
            else:
                command = [str(path / "abr"), *args]
            result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn(name + " passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
