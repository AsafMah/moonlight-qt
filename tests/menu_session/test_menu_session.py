"""Exercise production menu dispatch and animation-close input ownership with fake SDL."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}\n", start) + 3]


class MenuSessionTests(unittest.TestCase):
    def test_next_stream_codec_preference_click(self):
        source = (ROOT / "app/streaming/sessionoverlaymenu.cpp").read_text(encoding="utf-8")
        handler = function(source, "bool Session::handleOverlayMenuOptionPayload(")
        harness = Path(__file__).with_name("next_stream_codec_harness.cpp").read_text()
        qmake = shutil.which("qmake6") or shutil.which("qmake")
        self.assertIsNotNone(qmake)
        with tempfile.TemporaryDirectory(prefix="moonlight-next-codec-") as directory:
            path = Path(directory)
            (path / "main.cpp").write_text(harness.replace("// METHOD", handler), encoding="utf-8")
            sources = ("streaming/video/overlaymenupanel.cpp",
                       "streaming/video/overlaymenugeometry.cpp",
                       "streaming/input/shortcuts.cpp", "streaming/streamquality.cpp")
            headers = ("streaming/video/overlaymenupanel.h",
                       "streaming/video/overlayrasterwindow.h")
            (path / "next.pro").write_text(
                "QT = core gui qml svg testlib\nCONFIG += console c++17 release\n"
                "CONFIG -= app_bundle debug debug_and_release\nTARGET = next\n"
                "SOURCES = main.cpp\n" +
                f'INCLUDEPATH += "{ROOT.as_posix()}/app"\n' +
                "SOURCES += " + " ".join(f'"{ROOT.as_posix()}/app/{s}"' for s in sources) + "\n" +
                "HEADERS += " + " ".join(f'"{ROOT.as_posix()}/app/{s}"' for s in headers) + "\n",
                encoding="utf-8")
            for command in ([qmake, "next.pro"],
                            ["nmake", "/nologo"] if os.name == "nt" else ["make", "-s"]):
                result = subprocess.run(command, cwd=path, capture_output=True, text=True,
                                        timeout=120)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            command = (["pwsh", "-NoProfile", "-File", str(ROOT / "scripts/run-built-test.ps1"),
                        "-Exe", str(next(path.rglob("next.exe"))), "-Platform", "offscreen"]
                       if os.name == "nt" else [str(path / "next")])
            environment = os.environ.copy()
            environment["QT_QPA_PLATFORM"] = "offscreen"
            result = subprocess.run(command, cwd=path, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, timeout=30,
                                    env=environment)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("next-stream-codec passed", result.stdout)

    def test_launch_and_reconnect_share_video_format_priorities(self):
        source = (ROOT / "app/streaming/session.cpp").read_text(encoding="utf-8")
        for signature in ("bool Session::initialize(", "bool Session::prepareStreamConfiguration("):
            self.assertIn("prioritizeVideoFormats(", function(source, signature))
        prepare = function(source, "bool Session::prepareStreamConfiguration(")
        launch_validation = function(source, "bool Session::validateLaunch(")
        self.assertIn("applyVideoFormatServerRestrictions(", prepare)
        self.assertIn("applyVideoFormatServerRestrictions(", launch_validation)
        method = function(source, "void Session::prioritizeVideoFormats(")
        server_filter = function(source, "void Session::applyVideoFormatServerRestrictions(")
        harness = Path(__file__).with_name("video_format_priority_harness.cpp").read_text()
        harness = harness.replace("// PRIORITIZATION_METHOD", method)
        harness = harness.replace("// SERVER_FILTER_METHOD", server_filter)
        self.compile_run(harness,
                         definitions=["Q_OS_DARWIN"])
        harness = Path(__file__).with_name("video_format_priority_harness.cpp").read_text()
        harness = harness.replace("// PRIORITIZATION_METHOD", method)
        harness = harness.replace("// SERVER_FILTER_METHOD", server_filter)
        self.compile_run(harness)
        priority_call = prepare.index("prioritizeVideoFormats(")
        self.assertEqual(prepare.count("prioritizeVideoFormats("), 1)
        self.assertNotIn("formats.deprioritizeByMask", prepare[priority_call:])
        hdr_fallback = prepare.index("formats.removeByMask(~VIDEO_FORMAT_MASK_10BIT)")
        empty_fallback = prepare.index("if (formats.isEmpty())", hdr_fallback)
        self.assertLess(hdr_fallback, empty_fallback)
        self.assertIn("AC_714_SURROUND", prepare)
        self.assertIn("AUDIO_CONFIGURATION_714_SURROUND", prepare)
        self.assertIn("m_PyrowaveRequested", prepare)

    def test_runtime_bitrate_completion_generations(self):
        session = (ROOT / "app/streaming/session.cpp").read_text(encoding="utf-8")
        method = function(session, "void Session::finishRuntimeBitrateChange(")
        harness = Path(__file__).with_name("runtime_bitrate_harness.cpp").read_text()
        self.compile_run(harness.replace("// METHOD", method))

    def test_dropped_reconnect_apply_reports_rejection(self):
        session = (ROOT / "app/streaming/session.cpp").read_text(encoding="utf-8")
        event_handler = function(session, "void Session::handleSdlUserEvent(")
        config_case = event_handler.split(
            "case SDL_CODE_APPLY_STREAM_CONFIGURATION:", 1)[1].split("default:", 1)[0]
        self.assertIn("reportStreamConfigurationNotApplied", config_case)
        report = function(session, "void Session::reportStreamConfigurationNotApplied(")
        self.assertIn("Not applied", report)
        self.assertIn("updateStreamConfigurationMenuState();", report)

    def test_live_menu_ownership(self):
        session = (ROOT / "app/streaming/session.cpp").read_text(encoding="utf-8")
        input_source = (ROOT / "app/streaming/input/input.cpp").read_text(encoding="utf-8")
        harness = Path(__file__).with_name("harness.cpp").read_text()
        if "bool Session::handleLiveMenuAction(" not in session:
            keyboard = (ROOT / "app/streaming/input/keyboard.cpp").read_text(encoding="utf-8")
            release = keyboard.split("case KeyComboUngrabInput:", 1)[1].split("break;", 1)[0]
            release = release.replace("setCaptureActive(", "m_InputHandler->setCaptureActive(")
            release = release.replace("isCaptureActive()", "m_InputHandler->isCaptureActive()")
            release = release.replace("raiseAllKeys()", "m_InputHandler->raiseAllKeys()")
            callback = session[session.index("m_MenuPanel->setCloseCallback("):]
            close = callback.split("// Restore mouse capture after menu closes", 1)[1]
            close = close.split("syncQtOverlayWindowsWithSdlWindowState();", 1)[0]
            methods = ("bool Session::handleLiveMenuAction(OverlayMenuPanel::MenuAction) {"
                       + release + "return true; }\n"
                       + "void Session::finishOverlayMenuInput() {" + close + "}\n")
            harness = harness[:harness.index("int main()")] + """
int main() {
    Session s;
    s.handleLiveMenuAction(OverlayMenuPanel::MenuAction::UngrabInput);
    s.finishOverlayMenuInput();
    return require(!s.input.captured && !s.m_WasCapturedBeforeMenu,
                   "Release mouse must stay free after animation close") ? 0 : 1;
}
"""
            self.compile_run(harness.replace("// METHODS", methods))
            return
        methods = "\n".join(function(session, signature) for signature in (
            "void Session::markVideoReceived(",
            "bool Session::handleLiveMenuAction(",
            "void Session::restoreResolutionMatchOverride(",
            "void Session::finishOverlayMenuInput(",
            "void Session::updateAudioMuteState(",
            "bool Session::handleOverlayMenuShortcutEvent(",
        ))
        match_state = function(session, "void Session::updateLiveMenuState(")
        self.assertNotIn("!m_ResolutionMatch.explicitOverride", match_state)
        self.assertIn("m_ResolutionMatch.explicitOverride", match_state)
        methods += function(input_source, "void SdlInputHandler::setAbsoluteMouseMode(")
        dispatch = function(session, "void Session::dispatchQtMenuAction(")
        self.assertIn("handleLiveMenuAction(action)", dispatch)
        exec_body = function(session, "void Session::exec(")
        routing = exec_body.index("if (handleOverlayMenuShortcutEvent(event.key))")
        self.assertLess(routing, exec_body.index("event.key.keysym.sym == SDLK_ESCAPE", routing))
        callback = session[session.index("m_MenuPanel->setCloseCallback("):]
        self.assertIn("finishOverlayMenuInput();", callback[:3000])
        for event in ("SDL_WINDOWEVENT_FOCUS_LOST:", "SDL_WINDOWEVENT_FOCUS_GAINED:"):
            body = session[session.index("case " + event):]
            self.assertIn("updateAudioMuteState();", body[:650])
        self.compile_run(harness.replace("// METHODS", methods))

    def test_stream_configuration_apply_event_flow(self):
        session = (ROOT / "app/streaming/session.cpp").read_text(encoding="utf-8")
        harness = Path(__file__).with_name("configuration_harness.cpp").read_text()
        methods = "\n".join(function(session, signature) for signature in (
            "void Session::requestStreamConfigurationChange(",
            "void Session::reportStreamConfigurationNotApplied(",
            "bool Session::applyPendingStreamConfigurationChange(",
            "void Session::applyPreparedStreamConfiguration(",
        ))
        exec_body = function(session, "void Session::exec(")
        dispatch = exec_body.index(
            "if (event.user.code == SDL_CODE_APPLY_STREAM_CONFIGURATION)")
        apply = exec_body.index("applyPendingStreamConfigurationChange()", dispatch)
        self.assertLess(dispatch, apply)
        callback = session[session.index("m_MenuPanel->setStreamConfigurationCallback("):]
        self.assertIn("requestStreamConfigurationChange(request)", callback[:300])
        apply_body = function(session, "bool Session::applyPendingStreamConfigurationChange(")
        self.assertIn(
            "tryReconnect(ReconnectReason::StreamQualityChange, &teardownStarted)", apply_body)
        self.assertIn("if (!teardownStarted)", apply_body)
        reconnect_body = function(session, "bool Session::tryReconnect(")
        stop = reconnect_body.index("LiStopConnection();")
        prepare = reconnect_body.index("applyPreparedStreamConfiguration();", stop)
        retry = reconnect_body.index("AsyncConnectionStartThread thread(this);", prepare)
        self.assertLess(stop, prepare)
        self.assertLess(prepare, retry)
        prepared = function(session, "void Session::applyPreparedStreamConfiguration(")
        self.assertIn("m_StreamConfig = prepared.streamConfig", prepared)
        abr_start = function(session, "void Session::startSunshineAbr(")
        self.assertIn("m_StreamConfig.bitrate", abr_start)
        connection_start = function(session, "bool Session::startConnectionAsync(")
        self.assertLess(connection_start.index("LiStartConnection("),
                        connection_start.index("startSunshineAbr();"))
        self.compile_run(harness.replace("// METHODS", methods))

    def compile_run(self, harness, definitions=()):
        compiler = shutil.which("cl")
        if compiler:
            msvc = True
        else:
            compiler = shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")
            msvc = False
        self.assertIsNotNone(compiler, "Run in a compiler environment")
        with tempfile.TemporaryDirectory(prefix="moonlight-menu-session-") as directory:
            path = Path(directory)
            source = path / "main.cpp"
            source.write_text(harness, encoding="utf-8")
            exe = path / ("main.exe" if os.name == "nt" else "main")
            command = ([compiler, "/nologo", "/std:c++17", "/EHsc", str(source), f"/Fe:{exe}"]
                       if msvc else [compiler, "-std=c++17", str(source), "-o", str(exe)])
            if msvc:
                command[4:4] = [f"/D{definition}" for definition in definitions]
            else:
                command[1:1] = [f"-D{definition}" for definition in definitions]
            build = subprocess.run(command, cwd=path, capture_output=True, text=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            local_runner = Path(r"E:\Code\moonlight-tools\run-test-exe.ps1")
            runner = local_runner if local_runner.is_file() else ROOT / "scripts/run-built-test.ps1"
            command = (["pwsh", "-NoProfile", "-File", str(runner),
                        "-Exe", str(exe), "-Platform", "offscreen"]
                       if os.name == "nt" else [str(exe)])
            run = subprocess.run(command,
                                 capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
