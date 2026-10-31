"""Compile the production inline marker with a lightweight Session shell."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ReceivedVideoMarkerTests(unittest.TestCase):
    def test_marker_sets_atomic_flag_and_tolerates_no_active_session(self):
        source = (ROOT / "app/streaming/session.h").read_text(encoding="utf-8")
        start = source.index("    static void markVideoReceived()")
        end = source.index("\n    }\n", start) + len("\n    }\n")
        marker = source[start:end]
        harness = r"""
#include <atomic>
#include <thread>
class Session {
public:
    static Session* s_ActiveSession;
    std::atomic<bool> m_HasReceivedVideo{false};
// MARKER
};
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
            command = ([compiler, "/nologo", "/std:c++17", "/EHsc", str(unit),
                        f"/Fe:{executable}"] if os.name == "nt" else
                       [compiler, "-std=c++17", "-pthread", str(unit), "-o", str(executable)])
            build = subprocess.run(command, cwd=path, capture_output=True, text=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
