"""Persisted Q_ENUM integers are a compatibility contract, not declaration order."""

from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[2]
BASELINE = {
    "AudioConfig": (0, "AC_STEREO AC_51_SURROUND AC_71_SURROUND AC_714_SURROUND"),
    "VideoCodecConfig": (0, "VCC_AUTO VCC_FORCE_H264 VCC_FORCE_HEVC "
                           "VCC_FORCE_HEVC_HDR_DEPRECATED VCC_FORCE_AV1 VCC_FORCE_PYROWAVE"),
    "VideoDecoderSelection": (0, "VDS_AUTO VDS_FORCE_HARDWARE VDS_FORCE_SOFTWARE"),
    "RendererSelection": (-1, "RS_PROBE_ONLY RS_AUTO RS_VULKAN RS_METAL RS_AVSBDL"),
    "WindowMode": (0, "WM_FULLSCREEN WM_FULLSCREEN_DESKTOP WM_WINDOWED"),
    "UIDisplayMode": (0, "UI_WINDOWED UI_MAXIMIZED UI_FULLSCREEN"),
    "BackgroundSource": (0, "BGS_PHOTOGRAPHY BGS_ANIME BGS_API BGS_LOCAL BGS_NONE"),
    "Language": (0, "LANG_AUTO LANG_EN LANG_FR LANG_ZH_CN LANG_DE LANG_NB_NO LANG_RU "
                    "LANG_ES LANG_JA LANG_VI LANG_TH LANG_KO LANG_HU LANG_NL LANG_SV "
                    "LANG_TR LANG_UK LANG_ZH_TW LANG_PT LANG_PT_BR LANG_EL LANG_IT "
                    "LANG_HI LANG_PL LANG_CS LANG_HE LANG_CKB LANG_LT LANG_ET LANG_BG "
                    "LANG_EO LANG_TA"),
    "CaptureSysKeysMode": (0, "CSK_OFF CSK_FULLSCREEN CSK_ALWAYS"),
    "OverlayMenuPosition": (0, "OMP_DISABLED OMP_BUTTON OMP_TOP_EDGE OMP_RIGHT_EDGE OMP_LEFT_EDGE"),
    "HdrMode": (1, "HDR_PQ HDR_HLG"),
    "HdrBrightnessMode": (0, "HBM_HOST_DEFAULT HBM_AUTO HBM_MANUAL"),
    "DualSenseHapticsMode": (0, "DSHM_PHYSICAL DSHM_EMULATED"),
    "GamepadQuitCombo": (0, "GQC_DEFAULT GQC_SELECT_L1_R1_X GQC_SELECT_L1_R1_Y "
                           "GQC_START_L1_R1_A GQC_START_L1_R1_B GQC_L1_R1_X_Y GQC_L1_R1_A_B"),
    "ScreenCombinationMode": (-1, "SCM_FOLLOW_HOST SCM_NO_OPERATION SCM_ENSURE_ACTIVE "
                                 "SCM_ENSURE_PRIMARY SCM_ENSURE_ONLY_DISPLAY SCM_ENSURE_SECONDARY"),
}


def enum_values(source):
    source = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)
    result = {}
    for name, body in re.findall(r"enum\s+(\w+)\s*\{([^}]+)\}", source):
        value = 0
        members = {}
        for item in body.split(","):
            item = item.strip()
            if not item:
                continue
            parts = item.split("=", 1)
            if len(parts) == 2:
                value = int(parts[1].strip(), 0)
            members[parts[0].strip()] = value
            value += 1
        result[name] = members
    return result


class SettingsEnumTests(unittest.TestCase):
    def test_existing_persisted_values(self):
        source = (ROOT / "app/settings/streamingpreferences.h").read_text(encoding="utf-8")
        values = enum_values(source)
        self.assertEqual(set(re.findall(r"Q_ENUM\((\w+)\)", source)), set(BASELINE))
        for name, (start, members) in BASELINE.items():
            for value, member in enumerate(members.split(), start):
                with self.subTest(enum=name, member=member):
                    self.assertEqual(values[name].get(member), value)

    def test_borderless_mode_is_appended_and_exposed(self):
        source = (ROOT / "app/settings/streamingpreferences.h").read_text(encoding="utf-8")
        self.assertEqual(enum_values(source)["CaptureSysKeysMode"].get(
            "CSK_FULLSCREEN_OR_BORDERLESS"), 3)
        qml = (ROOT / "app/gui/settings/LegacySettingsPage.qml").read_text(encoding="utf-8")
        cli = (ROOT / "app/cli/commandlineparser.cpp").read_text(encoding="utf-8")
        self.assertIn('qsTr("in fullscreen or borderless window")', qml)
        self.assertIn("StreamingPreferences.CSK_FULLSCREEN_OR_BORDERLESS", qml)
        self.assertRegex(cli, r'"fullscreen-or-borderless"\s*,\s*'
                              r'StreamingPreferences::CSK_FULLSCREEN_OR_BORDERLESS')
        fixture = (ROOT / "tests/settings_search/tst_capture_mode.qml").read_text(encoding="utf-8")
        choices = dict(re.findall(
            r'text:\s*qsTr\("([^"]+)"\);\s*val:\s*StreamingPreferences\.(CSK_\w+)', qml))
        for label, value in re.findall(r'text:\s*"([^"]+)";\s*val:\s*(\d+)', fixture):
            with self.subTest(choice=label):
                self.assertEqual(enum_values(source)["CaptureSysKeysMode"][choices[label]], int(value))

    def test_gate_detects_structural_merge_reordering(self):
        source = (ROOT / "app/settings/streamingpreferences.h").read_text(encoding="utf-8")
        mutated = source.replace("CSK_OFF,", "CSK_SWAPPED,", 1)
        mutated = mutated.replace("CSK_ALWAYS,", "CSK_OFF,", 1)
        mutated = mutated.replace("CSK_SWAPPED,", "CSK_ALWAYS,", 1)
        self.assertNotEqual(enum_values(mutated)["CaptureSysKeysMode"]["CSK_OFF"], 0)

    def test_capture_strings_are_translated_in_both_chinese_locales(self):
        for locale in ("zh_CN", "zh_TW"):
            root = ET.parse(ROOT / f"app/languages/qml_{locale}.ts").getroot()
            for context, sources in {
                "LegacySettingsPage": ("in fullscreen or borderless window",
                                       "Borderless windows must cover their monitor."),
                "OverlayMenuPanel": ("Toggle System Key Capture", "Cycle Preset"),
            }.items():
                messages = {m.findtext("source"): m.find("translation")
                            for c in root.findall("context") if c.findtext("name") == context
                            for m in c.findall("message")}
                for source in sources:
                    with self.subTest(locale=locale, source=source):
                        translation = messages.get(source)
                        self.assertIsNotNone(translation)
                        self.assertNotEqual(translation.get("type"), "unfinished")
                        self.assertTrue(translation.text)


if __name__ == "__main__":
    unittest.main()
