"""Compile the production Session methods against fake time, window and transport."""

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SESSION = ROOT / "app" / "streaming" / "session.cpp"


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}\n", start) + 3]


class ResolutionMatchTests(unittest.TestCase):
    def test_production_session_lifecycle(self):
        source = SESSION.read_text(encoding="utf-8")
        header = SESSION.with_suffix(".h").read_text(encoding="utf-8")
        state_start = header.index("    struct ResolutionMatchState")
        state_end = header.index("    } m_ResolutionMatch;", state_start)
        state = header[state_start:state_end + len("    } m_ResolutionMatch;")]
        quality_source = (ROOT / "app/streaming/streamquality.cpp").read_text(
            encoding="utf-8")
        quality_header = (ROOT / "app/streaming/streamquality.h").read_text(encoding="utf-8")
        settings_start = quality_header.index("struct Settings")
        settings_end = quality_header.index("\n};", settings_start) + 3
        live_state_start = quality_header.index("struct Change")
        transaction_start = quality_header.index("class LiveResolutionTransaction", live_state_start)
        transaction_end = quality_header.index("\n};", transaction_start) + 3
        live_types = quality_header[settings_start:settings_end] + "\n" + \
            quality_header[live_state_start:transaction_end]
        quality_methods = "\n".join(function(quality_source, signature) for signature in (
            "bool validResolutionTarget(",
            "bool hostStreamingDisplayIsOneToOne(",
            "bool canApplyLiveResolution(",
            "bool LiveResolutionTransaction::begin(",
            "bool LiveResolutionTransaction::observeHostReport(",
            "bool LiveResolutionTransaction::decoderResetCompleted(",
            "LiveResolutionResult LiveResolutionTransaction::firstFrame(",
            "bool LiveResolutionTransaction::expire(",
            "void LiveResolutionTransaction::cancel(",
        ))
        methods = ("namespace StreamQuality {\nconstexpr int kMaxDimension = 16384;\n" +
                   quality_methods + "\n}\n")
        methods += "\n".join(function(source, signature) for signature in (
            "void Session::scheduleWindowResolutionMatch(",
            "void Session::scheduleInitialWindowResolutionMatch(",
            "bool Session::requestWindowResolutionMatchNow(",
            "void Session::processWindowResolutionMatch(",
            "void Session::handleHostResolutionReport(",
        ))
        harness = Path(__file__).with_name("harness.cpp").read_text(encoding="utf-8")
        harness = harness.replace("// LIVE_TRANSACTION_TYPES", live_types)
        harness = harness.replace("// RESOLUTION_STATE", state)
        harness = harness.replace("// SESSION_METHODS", methods)
        compiler = shutil.which("cl" if os.name == "nt" else "c++")
        self.assertIsNotNone(compiler, "Run from a C++ compiler environment")
        with tempfile.TemporaryDirectory(prefix="moonlight-resolution-") as directory:
            path = Path(directory)
            unit = path / "resolution.cpp"
            unit.write_text(harness, encoding="utf-8")
            executable = path / ("resolution.exe" if os.name == "nt" else "resolution")
            if os.name == "nt":
                command = [compiler, "/nologo", "/std:c++17", "/EHsc", str(unit),
                           f"/Fe:{executable}"]
            else:
                command = [compiler, "-std=c++17", str(unit), "-o", str(executable)]
            build = subprocess.run(command, cwd=path, capture_output=True, text=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            local_runner = Path(r"E:\Code\moonlight-tools\run-test-exe.ps1")
            runner = local_runner if local_runner.is_file() else ROOT / "scripts/run-built-test.ps1"
            run_command = (["pwsh", "-NoProfile", "-File", str(runner),
                            "-Exe", str(executable), "-Platform", "offscreen"]
                           if os.name == "nt" else [str(executable)])
            run = subprocess.run(run_command, capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_user_event_codes_are_unique(self):
        entries = []
        for path in SESSION.parent.rglob("*"):
            if path.suffix not in {".cpp", ".h"}:
                continue
            entries.extend(
                (name, value, path)
                for name, value in re.findall(
                    r"^#define (SDL_CODE_\w+) (\d+)$",
                    path.read_text(encoding="utf-8"),
                    re.MULTILINE,
                )
            )
        values = {}
        for name, value, path in entries:
            self.assertNotIn(value, values, f"{path}: {name} collides with {values.get(value)}")
            values[value] = f"{path}: {name}"

    def test_windowed_start_matches_initial_created_window_size(self):
        source = SESSION.read_text(encoding="utf-8")
        exec_body = function(source, "void Session::exec(")
        create = exec_body.index("SDL_CreateWindow(")
        initial_mode = exec_body.index("// Enter full screen if requested")
        schedule = exec_body.index(
            "scheduleInitialWindowResolutionMatch(awaitingFullScreenEntry);")
        self.assertLess(create, initial_mode)
        self.assertLess(initial_mode, schedule)

        header = SESSION.with_suffix(".h").read_text(encoding="utf-8")
        self.assertIn("void scheduleInitialWindowResolutionMatch(bool awaitingFullscreenEntry);",
                      header)

    def test_input_dimension_setter_is_not_duplicated(self):
        source = (ROOT / "app/streaming/input/input.cpp").read_text(encoding="utf-8")
        self.assertEqual(source.count("void SdlInputHandler::setStreamDimensions("), 1)

    def test_preference_defaults_off(self):
        source = (ROOT / "app/settings/streamingpreferences.cpp").read_text(encoding="utf-8")
        self.assertRegex(source, r"resolutionMatchWindow\s*=\s*settings\.value\("
                                 r"SER_RESOLUTION_MATCH_WINDOW,\s*false\)\.toBool\(\)")

    def test_quality_menu_uses_active_dimensions(self):
        source = SESSION.read_text(encoding="utf-8")
        for signature in ("void Session::updateStreamQualityMenuState(",
                          "bool Session::applyPendingStreamQualityChange("):
            body = function(source, signature)
            self.assertIn("m_ActiveVideoWidth", body)
            self.assertIn("m_ActiveVideoHeight", body)

    def test_save_default_occurs_only_after_apply_gates_accept(self):
        body = function(SESSION.read_text(encoding="utf-8"),
                        "bool Session::applyPendingStreamQualityChange(")
        busy_gate = body.index(
            "if (m_LiveResolutionTransaction.isActive() || m_PendingStreamConfiguration")
        busy_path = body.index("if (path == StreamQuality::Path::Busy)")
        save = body.index("m_Preferences->save();")
        self.assertLess(busy_gate, save)
        self.assertLess(busy_path, save)

    def test_quality_and_configuration_apply_wait_for_runtime_bitrate(self):
        source = SESSION.read_text(encoding="utf-8")
        for signature in (
                "void Session::requestStreamQualityChange(",
                "bool Session::applyPendingStreamQualityChange(",
                "void Session::requestStreamConfigurationChange(",
                "bool Session::applyPendingStreamConfigurationChange("):
            self.assertIn("m_RuntimeBitrateInFlight", function(source, signature))

    def test_runtime_bitrate_completion_is_connection_generation_safe(self):
        source = SESSION.read_text(encoding="utf-8")
        worker = function(source, "void Session::startRuntimeBitrateWorker(")
        self.assertIn("m_ConnectionGeneration", worker)
        self.assertIn("finishRuntimeBitrateChange", worker)
        finish = function(source, "void Session::finishRuntimeBitrateChange(")
        self.assertLess(finish.index("m_ConnectionGeneration"),
                        finish.index("m_StreamConfig.bitrate"))
        self.assertIn("m_ConnectionCancel", finish)
        self.assertIn("m_ConnectionInterrupted", finish)
        self.assertIn("m_ReconnectInProgress", finish)
        self.assertIn("m_SuppressConnectionErrorDialog", finish)

    def test_explicit_quality_choice_does_not_schedule_a_window_override(self):
        body = function(SESSION.read_text(encoding="utf-8"), "bool Session::tryReconnect(")
        self.assertRegex(body, r"if\s*\(!qualityChange\)\s*\{\s*"
                              r"scheduleWindowResolutionMatch\(\);")

    def test_each_connection_resets_resolution_state_before_reading_capabilities(self):
        body = function(SESSION.read_text(encoding="utf-8"),
                        "bool Session::startConnectionAsync(")
        self.assertIn("m_ResolutionMatch = {}", body)
        self.assertLess(body.index("LiStartConnection("),
                        body.index("m_ResolutionMatch = {}"))
        self.assertLess(body.index("m_ResolutionMatch = {}"),
                        body.index("LiGetHostFeatureFlags()"))


if __name__ == "__main__":
    unittest.main()
