import unittest
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
REQUIRED = {
    "OverlayMenuPanel": {
        "%1 · %2 · %3", "5.1", "7.1", "7.1.4", "AV1",
        "Another stream change is in progress", "Apply pending stream settings",
        "Apply stream settings", "Applying %1", "Applying stream settings",
        "Audio channels", "Automatic", "Busy", "Busy · another stream change is in progress",
        "Codec, HDR & audio", "Current: %1", "H.264", "HDR", "HEVC", "HLG",
        "Live is unavailable; choose Reconnect", "Live resolution request",
        "Live · brief reset; host may refuse", "Live · brief video reset; host may refuse",
        "Negotiated", "PQ", "Pending: %1 · %2 · %3", "Pending: %1 · current: %2",
        "PyroWave", "PyroWave remains selected; changing codecs requires a new stream",
        "Reconnect", "Reconnect to %1", "Reconnect · stops and resumes this stream",
        "Reconnects this stream; does not save defaults", "Requested",
        "Retry live resolution", "Stereo", "Unknown", "Video codec",
    },
    "Session": {
        "%1 channels", "Another resolution or stream change is in progress",
        "Another stream change is already in progress",
        "Audio fell back to stereo; check the negotiated channel count",
        "Changing to or from PyroWave requires a new stream",
        "Connection changed before the live resolution result was confirmed",
        "Couldn't create a decoder test window",
        "HDR is unavailable and SDR fallback failed: %1",
        "HDR was unavailable; the stream continued in SDR", "HLG",
        "Host applied %1 instead of requested %2; retry Live or choose Reconnect",
        "Host did not apply %1; retry Live or choose Reconnect",
        "Host rejected the live resolution request (%1); choose Retry or Reconnect",
        "Live is no longer eligible; choose Reconnect explicitly",
        "Live resolution applied: %1",
        "Live resolution is no longer available; choose Reconnect explicitly",
        "No compatible video format and decoder are available", "Not applied: %1",
        "Off", "PQ", "Reconnect complete; negotiated values are shown above",
        "Reconnect failed; the session ended",
        "Reconnecting with requested codec, HDR and audio settings",
        "Requesting %1; brief video reset, host may refuse",
        "The host and this device have no compatible video format",
        "The requested audio channels are unavailable on this device",
        "The requested audio configuration is invalid", "The requested bitrate change failed",
        "The requested stream settings are invalid",
        "The selected renderer cannot decode the requested video format",
    },
}


class TranslationCoverageTests(unittest.TestCase):
    def test_phase2a_strings_are_translated(self):
        for catalog_name in ("qml_zh_CN.ts", "qml_zh_TW.ts"):
            catalog = ROOT / "app" / "languages" / catalog_name
            root = ET.parse(catalog).getroot()
            contexts = {
                context.findtext("name"): context
                for context in root.findall("context")
            }
            for context_name, sources in REQUIRED.items():
                with self.subTest(catalog=catalog_name, context=context_name):
                    context = contexts[context_name]
                    messages = {
                        message.findtext("source"): message
                        for message in context.findall("message")
                    }
                    for source in sorted(sources):
                        message = messages.get(source)
                        self.assertIsNotNone(message, f"missing source: {source}")
                        translation = message.find("translation")
                        self.assertIsNotNone(translation, f"missing translation: {source}")
                        self.assertNotEqual(translation.get("type"), "unfinished", source)
                        self.assertTrue((translation.text or "").strip(), source)


if __name__ == "__main__":
    unittest.main()
