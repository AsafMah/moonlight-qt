#!/usr/bin/env python3
"""Replay candidate GitHub metadata through the production Windows updater."""
import argparse
import base64
import importlib.util
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def verify_update(candidate: dict, previous: str) -> None:
    if not re.fullmatch(r"v?\d+\.\d+\.\d+(?:\.\d+)?", previous):
        raise RuntimeError(f"Cannot replay updater from previous release {previous}")
    if candidate.get("prerelease", True):
        raise RuntimeError("Cannot publish a prerelease through the stable updater")
    # Replay public visibility without making the draft public first. Preserve
    # the exact tag, URLs, names and digests returned by GitHub after upload.
    release = dict(candidate, draft=False)
    encoded = base64.b64encode(json.dumps(release).encode("utf-8")).decode("ascii")
    # Real GitHub responses can exceed MSVC's single string-literal limit.
    payload = " +\n        ".join(f'QByteArray("{encoded[offset:offset + 4096]}")'
                                 for offset in range(0, len(encoded), 4096))
    spec = importlib.util.spec_from_file_location(
        "release_selection", ROOT / "tests/fork_updates/test_release_selection.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    base = (ROOT / "tests/fork_updates/selection_harness.cpp").read_text(encoding="utf-8")
    main = r'''
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir root;
    Path::portableRoot = root.path();
    const auto release = QJsonDocument::fromJson(
        QByteArray::fromBase64(CANDIDATE_PAYLOAD)).object();
    for (const auto& architecture : { QStringLiteral("x86_64"), QStringLiteral("arm64") }) {
        testArchitecture = architecture;
        for (bool portable : { true, false }) {
            QFile marker(root.filePath("portable.dat"));
            if (portable) {
                if (!marker.open(QIODevice::WriteOnly))
                    return 1;
                marker.close();
            } else if (!marker.remove()) {
                return 1;
            }
            AutoUpdateChecker checker;
            checker.m_CurrentVersion = VersionUtils::parseRelease("PREVIOUS_VERSION");
            const auto next = VersionUtils::parseRelease(release["tag_name"].toString());
            if (VersionUtils::compare(checker.m_CurrentVersion, next) >= 0) {
                std::fprintf(stderr, "FAIL: Candidate is not newer than previous published version\n");
                return 2;
            }
            int notifications = 0;
            QObject::connect(&checker, &AutoUpdateChecker::onUpdateAvailable,
                             [&](const QString&, const QString&) { ++notifications; });
            checker.handleUpdateCheckRequestFinished(new Reply(release));
            const QString expected = portable
                ? QString("Portable-%1-").arg(checker.getCurrentBuildArch()) : QString("Setup-");
            if (notifications != 1 || !checker.supportsInAppUpdate() ||
                !checker.m_UpdateDownloadUrl.contains(expected)) {
                std::fprintf(stderr, "FAIL: Production updater rejected candidate or selected wrong package\n");
                return 3;
            }
            bool matched = false;
            for (const auto& entry : release["assets"].toArray()) {
                const auto asset = entry.toObject();
                if (asset["browser_download_url"].toString() == checker.m_UpdateDownloadUrl &&
                    asset["digest"].toString() == checker.m_UpdateAssetDigest)
                    matched = true;
            }
            if (!matched) {
                std::fprintf(stderr, "FAIL: Selected URL/digest do not match uploaded metadata\n");
                return 4;
            }
        }
    }
    std::puts("Release-time production updater replay passed");
    return 0;
}
'''.replace("CANDIDATE_PAYLOAD", payload).replace("PREVIOUS_VERSION", previous)
    module.ForkUpdateTests().run_checker_harness(
        "selection_harness.cpp", "Release-time production updater replay passed",
        harness=base[:base.index("int main(")] + main, windows_selection=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--metadata", required=True, type=Path)
    parser.add_argument("--previous", required=True)
    args = parser.parse_args()
    verify_update(json.loads(args.metadata.read_text(encoding="utf-8-sig")), args.previous)


if __name__ == "__main__":
    main()
