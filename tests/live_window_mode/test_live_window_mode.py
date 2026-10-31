"""Compile the production mode/menu dispatch against a non-disruptive SDL shell."""

from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}\n", start) + 3]


def generate():
    source = (ROOT / "app/streaming/session.cpp").read_text(encoding="utf-8")
    methods = "\n".join(function(source, signature) for signature in (
        "Session::WindowMode Session::streamingWindowMode(",
        "bool Session::supportsStreamingWindowMode(",
        "bool Session::setStreamingWindowMode(",
        "void Session::toggleFullscreen(",
        "void Session::updateWindowModeMenuState(",
        "bool Session::handleWindowModeMenuAction(",
        "void Session::restoreCaptureAfterWindowModeChange(",
        "bool Session::isStreamingWindowFullscreen(",
        "bool Session::isStreamingWindowBorderlessOnDisplay(",
        "bool Session::handleLiveMenuAction(",
        "void Session::finishOverlayMenuInput(",
        "void Session::updateAudioMuteState(",
        "void Session::updateLiveMenuState(",
    ))
    dispatch = function(source, "void Session::dispatchQtMenuAction(")
    # Keep the actual runtime dispatch prefix; unrelated host/USB/mic actions
    # need real host services and are not part of this test's shell.
    methods += dispatch[:dispatch.index("    // Quick actions")] + "}\n"
    stats = (ROOT / "app/streaming/sessionstatsmenu.cpp").read_text(encoding="utf-8")
    methods += "\n" + "\n".join(function(stats, signature) for signature in (
        "QString formatDuration(",
        "std::vector<std::pair<QString, QString>> Session::sessionStatsMenuRows(",
        "void Session::setupSessionStatsMenu(",
        "void Session::updateConnectionMenuInfo(",
    ))
    (Path(__file__).parent / "production_methods.inc").write_text(methods, encoding="utf-8")


class LiveModeTests(unittest.TestCase):
    def test_production_dispatch_contract(self):
        generate()
        source = (ROOT / "app/streaming/session.cpp").read_text(encoding="utf-8")
        callback = source[source.index("m_MenuPanel->setActionCallback("):]
        self.assertIn("dispatchQtMenuAction(action);", callback[:250])
        mode = function(source, "bool Session::setStreamingWindowMode(")
        self.assertNotIn("startConnection", mode)
        self.assertNotIn("LiStopConnection", mode)
        self.assertNotIn("resolutionMatchWindow", mode)
        self.assertNotIn("m_Preferences->save", mode)
        keyboard = (ROOT / "app/streaming/input/keyboard.cpp").read_text(encoding="utf-8")
        self.assertIn("Session::s_ActiveSession->toggleFullscreen();", keyboard)


if __name__ == "__main__":
    import sys
    if "--generate" in sys.argv:
        generate()
    else:
        unittest.main()
