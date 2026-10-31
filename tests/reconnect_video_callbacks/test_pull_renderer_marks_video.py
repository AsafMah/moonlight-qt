"""Pull renderers bypass drSubmitDecodeUnit(), including FFmpeg and PyroWave.
Only successfully pulled streaming frames may enable auto-reconnect; decoder
self-test samples must not mark the session as having received host video.
"""

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
FFMPEG = ROOT / "app" / "streaming" / "video" / "ffmpeg.cpp"
PYROWAVE = ROOT / "app" / "streaming" / "video" / "pyrowave" / "pyrowavevideodecoder.cpp"
SESSION_H = ROOT / "app" / "streaming" / "session.h"
SESSION_CPP = ROOT / "app" / "streaming" / "session.cpp"


def function_body(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}\n", start)]


class PullRendererMarksVideoTests(unittest.TestCase):
    def test_ffmpeg_decoder_is_a_pull_renderer(self):
        self.assertIn("capabilities |= CAPABILITY_PULL_RENDERER;", FFMPEG.read_text(encoding="utf-8"))

    def test_every_ffmpeg_pulled_decode_unit_marks_video_received(self):
        body = function_body(FFMPEG.read_text(encoding="utf-8"),
                             "void FFmpegVideoDecoder::decoderThreadProc()")
        submits = [m.start() for m in re.finditer(r"submitDecodeUnit\(du\)", body)]
        self.assertEqual(len(submits), 2)
        self.assertRegex(body, r"continue;\s*}\s*Session::markVideoReceived\(\);\s*"
                              r"LiCompleteVideoFrame\(handle, submitDecodeUnit\(du\)\);")
        self.assertRegex(body, r"if \(LiPollNextVideoFrame\(&handle, &du\)\) \{\s*"
                              r"Session::markVideoReceived\(\);")
        for pos in submits:
            preceding = body[:pos]
            pulled = max(preceding.rfind("LiWaitForNextVideoFrame("),
                         preceding.rfind("LiPollNextVideoFrame("))
            self.assertGreaterEqual(pulled, 0)
            self.assertIn("Session::markVideoReceived();", preceding[pulled:],
                          "a pulled decode unit is submitted without marking received video")

    def test_pyrowave_marks_successfully_pulled_video(self):
        source = PYROWAVE.read_text(encoding="utf-8")
        self.assertIn("m_Contract.pullRenderer", source)
        body = function_body(source, "int PyrowaveVideoDecoder::decoderThreadProc(void* context)")
        self.assertRegex(body, r"continue;\s*}\s*Session::markVideoReceived\(\);\s*"
                              r"LiCompleteVideoFrame\(handle, me->processFrame\(du\)\);")

    def test_decoder_self_tests_do_not_mark_received_video(self):
        for path, signature in [
            (FFMPEG, "int FFmpegVideoDecoder::submitDecodeUnit("),
            (PYROWAVE, "int PyrowaveVideoDecoder::submitDecodeUnit("),
            (PYROWAVE, "int PyrowaveVideoDecoder::processFrame("),
        ]:
            self.assertNotIn("markVideoReceived", function_body(path.read_text(encoding="utf-8"),
                                                               signature))

    def test_flag_is_safe_across_threads(self):
        self.assertRegex(SESSION_H.read_text(encoding="utf-8"),
                         r"std::atomic<bool>\s+m_HasReceivedVideo;")

    def test_connection_retries_reset_received_video(self):
        body = function_body(SESSION_CPP.read_text(encoding="utf-8"),
                             "bool Session::tryReconnect(")
        self.assertLess(body.index("m_HasReceivedVideo = false;"),
                        body.index("AsyncConnectionStartThread thread(this);"))

    def test_connection_start_uses_tested_callback_preparation(self):
        body = function_body(SESSION_CPP.read_text(encoding="utf-8"),
                             "bool Session::startConnectionAsync(")
        self.assertLess(body.index("prepareVideoCallbacksForConnection(&m_VideoCallbacks);"),
                        body.index("LiStartConnection("))


if __name__ == "__main__":
    unittest.main()
