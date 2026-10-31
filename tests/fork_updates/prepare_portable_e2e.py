"""Prepare an explicitly requested, isolated full-app update fixture.

Only the fixture's QML automates confirmation. Production has no test endpoint,
automatic-consent hook or digest bypass. The real checker downloads r43 over HTTPS.
"""
import argparse
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[2]
PAYLOAD_SHA256 = "a760aabf7b5eedff1b79fc7a0c40885648b8da60116889ea641b03efa7c094cc"

DRIVER = """
    Timer {
        property int phase: 0
        interval: 100
        running: true
        repeat: true
        onTriggered: {
            if (phase === 0 && updateButton.visible) {
                console.log("UPDATE_E2E_OFFER", updateButton.releaseVersion)
                updateButton.clicked()
                phase = 1
            } else if (phase === 1 && updateConfirmationDialog.opened) {
                console.log("UPDATE_E2E_CONFIRMED", updateConfirmationDialog.downloadUrl)
                updateConfirmationDialog.footer.standardButton(Dialog.Ok).clicked()
                phase = 2
            }
        }
    }
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--payload", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    args = parser.parse_args()
    if hashlib.sha256(args.payload.read_bytes()).hexdigest() != PAYLOAD_SHA256:
        raise RuntimeError("The r43 payload does not match the published artifact")
    fixture = args.fixtures.resolve()
    if (fixture / "manifest.json").exists():
        raise RuntimeError("An existing validation fixture must not be overwritten")
    fixture.mkdir(exist_ok=True)
    portable = fixture / "portable"
    portable.mkdir(exist_ok=True)
    with zipfile.ZipFile(args.payload) as archive:
        archive.extractall(portable)
    executables = list(portable.rglob("Moonlight.exe"))
    if len(executables) != 1:
        raise RuntimeError("Expected one actual Moonlight executable in r43")
    executable = executables[0]
    target_digest = hashlib.sha256(executable.read_bytes()).hexdigest()
    (executable.parent / "portable.dat").touch()
    (executable.parent / "Moonlight.ini").write_text(
        "[General]\nautoUpdateCheck=true\nupdateFixtureSentinel=preserve-me\n", encoding="utf-8")
    source = (ROOT / "app/gui/main.qml").read_text(encoding="utf-8")
    if source.count("    id: window\n") != 1:
        raise RuntimeError("Cannot attach fixture confirmation driver to the real window")
    qml = fixture / "main.qml"
    qml.write_text(source.replace("    id: window\n", "    id: window\n" + DRIVER, 1), encoding="utf-8")
    resources = ET.parse(ROOT / "app/qml.qrc")
    for element in resources.findall(".//file"):
        alias = element.get("alias", element.text)
        element.set("alias", alias)
        element.text = str(qml if alias == "gui/main.qml" else (ROOT / "app" / element.text).resolve())
    resource = fixture / "fixture.qrc"
    resources.write(resource, encoding="utf-8")
    (fixture / "fork_update_e2e.prf").write_text(
        "RESOURCES -= qml.qrc $$PWD/qml.qrc $$OUT_PWD/qml_qmlcache.qrc\n"
        "QMAKE_EXTRA_COMPILERS -= qmlcache qmlcache_loader\n"
        "QMLCACHE_FILES =\nQMLCACHE_RESOURCE_FILES =\n"
        "DEFINES += LOG_TO_FILE\n"
        "RESOURCES += $$quote($$(E2E_QML_RESOURCE))\n", encoding="utf-8")
    manifest = fixture / "manifest.json"
    manifest.write_text(json.dumps({"executable": str(executable), "targetSha256": target_digest,
                                    "resource": str(resource), "fixture": str(fixture)}, indent=2),
                        encoding="utf-8")
    print(str(manifest))


if __name__ == "__main__":
    main()
