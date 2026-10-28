"""Exercise production keyboard-capture methods without grabbing desktop input."""

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}\n", start) + 3]


class SystemKeyCaptureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / "app/streaming/input/input.cpp").read_text(encoding="utf-8")
        keyboard = (ROOT / "app/streaming/input/keyboard.cpp").read_text(encoding="utf-8")
        preferences = (ROOT / "app/settings/streamingpreferences.h").read_text(encoding="utf-8")
        enum = re.search(r"enum CaptureSysKeysMode\s*\{[^}]+\}", preferences).group()
        enum += ";\n" + re.search(r"enum WindowMode\s*\{[^}]+\}", preferences).group()
        methods = "\n".join(function(source, signature) for signature in (
            "void SdlInputHandler::updateKeyboardGrabState(",
            "bool SdlInputHandler::isSystemKeyCaptureActive(",
            "bool SdlInputHandler::isCaptureActive(",
            "void SdlInputHandler::setCaptureActive(",
            "void SdlInputHandler::holdReleaseMouse(",
            "void SdlInputHandler::handleMouseReleaseHold(",
            "void SdlInputHandler::applyMouseReleaseGesture(",
            "bool SdlInputHandler::handleMouseReleaseEscape(",
        ))
        signature = "bool SdlInputHandler::isSystemKeyCaptureEnabledForWindow("
        if signature in source:
            methods += function(source, signature)
        start = keyboard.index("    case KeyComboToggleKeyboardGrab:")
        end = keyboard.index("\n        break;", start)
        toggle = keyboard[start:end].split(":", 1)[1]
        methods += "\nvoid SdlInputHandler::toggle() {" + toggle + "\n}\n"
        harness = Path(__file__).with_name("harness.cpp").read_text(encoding="utf-8")
        harness = harness.replace("// CAPTURE_ENUM", enum + ";")
        harness = harness.replace("// INPUT_METHODS", methods)
        gesture = (ROOT / "app/streaming/input/mousereleasegesture.h").read_text(encoding="utf-8")
        harness = harness.replace("// RELEASE_GESTURE", gesture[gesture.index("class MouseReleaseGesture"):])
        session = (ROOT / "app/streaming/session.cpp").read_text(encoding="utf-8")
        session_methods = function(session, "bool Session::isStreamingWindowFullscreen(")
        signature = "bool Session::isStreamingWindowBorderlessOnDisplay("
        session_methods += (function(session, signature) if signature in session else
                            "bool Session::isStreamingWindowBorderlessOnDisplay() const { return false; }")
        harness = harness.replace("// SESSION_METHODS", session_methods)
        compiler = shutil.which("cl" if os.name == "nt" else "c++")
        if compiler is None:
            raise AssertionError("Run from a C++ compiler environment")
        cls.directory = tempfile.TemporaryDirectory(prefix="moonlight-capture-")
        cls.addClassCleanup(cls.directory.cleanup)
        path = Path(cls.directory.name)
        unit = path / "capture.cpp"
        unit.write_text(harness, encoding="utf-8")
        cls.executable = path / ("capture.exe" if os.name == "nt" else "capture")
        command = ([compiler, "/nologo", "/std:c++17", "/EHsc", str(unit),
                    f"/Fe:{cls.executable}"] if os.name == "nt" else
                   [compiler, "-std=c++17", str(unit), "-o", str(cls.executable)])
        build = subprocess.run(command, cwd=path, capture_output=True, text=True, timeout=60)
        if build.returncode:
            raise AssertionError(build.stdout + build.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.executable), case], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_menu_release_does_not_turn_toggle_into_always_enable(self):
        self.run_case("toggle")

    def test_existing_capture_modes(self):
        self.run_case("legacy")

    def test_monitor_covering_borderless_geometry(self):
        self.run_case("geometry")

    def test_borderless_policy_and_right_alt_transitions(self):
        self.run_case("borderless")

    def test_window_events_refresh_capture_before_renderer_early_exits(self):
        source = (ROOT / "app/streaming/session.cpp").read_text(encoding="utf-8")
        start = source.index("case SDL_WINDOWEVENT:\n            // Early handling")
        early = source[start:source.index("\n            presence.runCallbacks();", start)]
        self.assertIn("m_InputHandler->updateKeyboardGrabState();", early)
        self.assertIn("SDL_WINDOWEVENT_DISPLAY_CHANGED", early)
        self.assertIn("SDL_WINDOWEVENT_MAXIMIZED", early)


if __name__ == "__main__":
    unittest.main()
