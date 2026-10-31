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
            "bool Session::handleLiveMenuAction(",
            "void Session::finishOverlayMenuInput(",
            "void Session::updateAudioMuteState(",
            "bool Session::handleOverlayMenuShortcutEvent(",
        ))
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

    def compile_run(self, harness):
        compiler = shutil.which("cl" if os.name == "nt" else "c++")
        self.assertIsNotNone(compiler, "Run in a compiler environment")
        with tempfile.TemporaryDirectory(prefix="moonlight-menu-session-") as directory:
            path = Path(directory)
            source = path / "main.cpp"
            source.write_text(harness, encoding="utf-8")
            exe = path / ("main.exe" if os.name == "nt" else "main")
            command = ([compiler, "/nologo", "/std:c++17", "/EHsc", str(source), f"/Fe:{exe}"]
                       if os.name == "nt" else
                       [compiler, "-std=c++17", str(source), "-o", str(exe)])
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
