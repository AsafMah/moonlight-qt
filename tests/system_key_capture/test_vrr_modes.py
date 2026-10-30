"""Exercise production decoder-parameter initialization against the VRR mode matrix."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class VrrModeTests(unittest.TestCase):
    def test_defaults_and_fallback_stay_on_arrival(self):
        source = (ROOT / "app/settings/streamingpreferences.cpp").read_text(encoding="utf-8")
        for expression in ("settings.value(SER_VRRSMOOTH, false)",
                           "settings.value(SER_VRRIMPLEMENTATION, VRR_IMPL_FORK)",
                           "settings.value(SER_VRRLATENCYMODE, VRR_LATENCY_BALANCED)"):
            self.assertIn(expression, source)
        session = (ROOT / "app/streaming/session.cpp").read_text(encoding="utf-8")
        start = session.index("if (sync.nonaryVrr && !m_VideoDecoder->isNonaryVrrActive())")
        fallback = session[start:session.index("// SDL_CreateRenderer()", start)]
        self.assertRegex(fallback, r"chooseDecoder\([\s\S]*VRR_IMPL_FORK,\s*"
                                   r"m_Preferences->vrrLatencyMode,\s*displayHz,\s*false\)")
        self.assertNotIn("m_Preferences->vrrSmooth", fallback)
        header = (ROOT / "app/streaming/session.h").read_text(encoding="utf-8")
        self.assertRegex(header, r"vrrDisplayRefreshHz = 0,\s*bool smoothVrr = false")

    def test_production_decoder_parameters(self):
        source = (ROOT / "app/streaming/session.cpp").read_text(encoding="utf-8")
        start = source.index("    DECODER_PARAMETERS params;")
        body = source[start:source.index("    // Preserve the saved preference", start)]
        harness = r"""
#include <cassert>
#include <iostream>
#include "app/streaming/video/vrrpolicy.h"
#define SDL_assert assert
struct StreamingPreferences {
    enum VrrImplementation { VRR_IMPL_FORK = 0, VRR_IMPL_NONARY = 1 };
};
struct DECODER_PARAMETERS {
    int width, height, frameRate, videoFormat, vrrLatencyMode, vrrDisplayRefreshHz;
    void* window;
    bool enableVsync, enableVrr, enableFramePacing, smoothVrr;
    StreamingPreferences::VrrImplementation vrrImplementation;
};
DECODER_PARAMETERS initialize(bool enableVrr, bool testOnly,
                             StreamingPreferences::VrrImplementation vrrImplementation,
                             bool smoothVrr) {
    int width = 1920, height = 1080, frameRate = 120, videoFormat = 1;
    int vrrLatencyMode = 1, vrrDisplayRefreshHz = 144;
    void* window = nullptr;
    bool enableVsync = false, enableFramePacing = false;
    // PRODUCTION_INITIALIZATION
    return params;
}
int main() {
    for (bool enabled : {false, true})
    for (bool testOnly : {false, true})
    for (bool savedSmooth : {false, true})
    for (auto implementation : {StreamingPreferences::VRR_IMPL_FORK,
                                StreamingPreferences::VRR_IMPL_NONARY}) {
        const auto params = initialize(enabled, testOnly, implementation, savedSmooth);
        const bool expected = enabled && !testOnly && savedSmooth &&
                              implementation == StreamingPreferences::VRR_IMPL_FORK;
        if (params.smoothVrr != expected) {
            std::cerr << "Smooth eligibility mismatch: VRR=" << enabled
                      << " testOnly=" << testOnly << " savedSmooth=" << savedSmooth
                      << " implementation=" << implementation << '\n';
            return 1;
        }
    }
}
""".replace("// PRODUCTION_INITIALIZATION", body)
        compiler = shutil.which("cl" if os.name == "nt" else "c++")
        self.assertIsNotNone(compiler, "Run from a C++ compiler environment")
        with tempfile.TemporaryDirectory(prefix="moonlight-vrr-modes-") as directory:
            path = Path(directory)
            unit = path / "modes.cpp"
            unit.write_text(harness, encoding="utf-8")
            executable = path / ("modes.exe" if os.name == "nt" else "modes")
            if os.name == "nt":
                command = [compiler, "/nologo", "/std:c++17", "/EHsc", f"/I{ROOT}",
                           str(unit), f"/Fe:{executable}"]
            else:
                command = [compiler, "-std=c++17", f"-I{ROOT}", str(unit), "-o", str(executable)]
            build = subprocess.run(command, cwd=path, capture_output=True, text=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
