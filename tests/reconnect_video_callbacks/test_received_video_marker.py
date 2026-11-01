"""Compile the production received-video marker with a lightweight Session shell."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ReceivedVideoMarkerTests(unittest.TestCase):
    def test_marker_sets_atomic_flag_and_tolerates_no_active_session(self):
        source = (ROOT / "app/streaming/session.cpp").read_text(encoding="utf-8")
        start = source.index("void Session::markVideoReceived()\n{")
        end = source.index("\n}\n", start) + len("\n}\n")
        marker = source[start:end]
        harness = r"""
#include <atomic>
#include <cstdint>
#include <thread>
#include "recoverybudget.h"
constexpr int SDL_USEREVENT = 1;
constexpr int SDL_CODE_LIVE_RESOLUTION_FIRST_FRAME = 113;
struct SDL_Event {
    int type;
    struct { int code; void* data1; } user;
};
static SDL_Event lastEvent{};
static int eventCount = 0;
void pushEventOrWarn(const SDL_Event& event) {
    lastEvent = event;
    ++eventCount;
}
class Session {
public:
    static Session* s_ActiveSession;
    static void markVideoReceived();
    std::atomic<bool> m_HasReceivedVideo{false};
    std::atomic<bool> m_ConnectionCancel{false};
    std::atomic<bool> m_RecoveryFrameReceived{false};
    std::atomic<bool> m_LiveResolutionAwaitingFirstFrame{false};
    std::atomic<unsigned> m_ConnectionGeneration{7};
    RecoveryBudget m_RecoveryBudget;
};
// MARKER
Session* Session::s_ActiveSession = nullptr;
int main() {
    Session::markVideoReceived();
    Session session;
    Session::s_ActiveSession = &session;
    if (session.m_HasReceivedVideo) return 1;
    std::thread decoder([] { Session::markVideoReceived(); });
    decoder.join();
    if (!session.m_HasReceivedVideo) return 2;
    session.m_HasReceivedVideo = false;
    if (session.m_HasReceivedVideo) return 3;
    Session::markVideoReceived();
    if (!session.m_HasReceivedVideo) return 4;
    session.m_RecoveryBudget.begin(RecoveryBudget::nowMs());
    session.m_RecoveryBudget.awaitRecoveredFrame();
    session.m_ConnectionCancel = true;
    Session::markVideoReceived();
    if (session.m_RecoveryFrameReceived || !session.m_RecoveryBudget.active()) return 5;
    session.m_ConnectionCancel = false;
    session.m_LiveResolutionAwaitingFirstFrame = true;
    Session::markVideoReceived();
    if (!session.m_RecoveryFrameReceived || session.m_RecoveryBudget.active()) return 6;
    if (session.m_LiveResolutionAwaitingFirstFrame || eventCount != 1) return 7;
    if (lastEvent.type != SDL_USEREVENT ||
        lastEvent.user.code != SDL_CODE_LIVE_RESOLUTION_FIRST_FRAME ||
        (uintptr_t)lastEvent.user.data1 != session.m_ConnectionGeneration.load()) return 8;
    Session::markVideoReceived();
    if (eventCount != 1) return 9;
    Session::s_ActiveSession = nullptr;
    Session::markVideoReceived();
    return 0;
}
""".replace("// MARKER", marker)
        compiler = shutil.which("cl" if os.name == "nt" else "c++")
        self.assertIsNotNone(compiler, "Run from a C++ compiler environment")
        with tempfile.TemporaryDirectory(prefix="moonlight-received-video-") as directory:
            path = Path(directory)
            unit = path / "marker.cpp"
            executable = path / ("marker.exe" if os.name == "nt" else "marker")
            unit.write_text(harness, encoding="utf-8")
            include = ROOT / "app/streaming"
            command = ([compiler, "/nologo", "/std:c++17", "/EHsc", f"/I{include}", str(unit),
                        f"/Fe:{executable}"] if os.name == "nt" else
                       [compiler, "-std=c++17", "-pthread", f"-I{include}", str(unit),
                        "-o", str(executable)])
            build = subprocess.run(command, cwd=path, capture_output=True, text=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            if os.name == "nt":
                command = ["pwsh", "-NoProfile", "-File",
                           r"E:\Code\moonlight-tools\run-test-exe.ps1",
                           "-Exe", str(executable), "-Platform", "offscreen"]
            else:
                command = [str(executable)]
            run = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
