#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTemporaryDir>
#include <cstdio>
#include <cstring>

class Path
{
public:
    static QString portableRoot;
    static QString getPortableRootDir() { return portableRoot; }
};
QString Path::portableRoot;

QUrl requestedUrl;
QString testArchitecture = "x86_64";
class ScriptedNetworkAccessManager : public QNetworkAccessManager
{
public:
    using QNetworkAccessManager::QNetworkAccessManager;

protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request, QIODevice*) override
    {
        requestedUrl = request.url();
        class PendingReply : public QNetworkReply
        {
        public:
            explicit PendingReply(QObject* parent) : QNetworkReply(parent) { open(ReadOnly); }
            void abort() override {}

        protected:
            qint64 readData(char*, qint64) override { return -1; }
        };
        return new PendingReply(this);
    }
};

#include "checker.inc"

class Reply : public QNetworkReply
{
public:
    explicit Reply(const QJsonObject& release) : bytes(QJsonDocument(release).toJson())
    {
        open(ReadOnly);
        setFinished(true);
    }
    void abort() override {}
    qint64 bytesAvailable() const override { return bytes.size() - offset; }

protected:
    qint64 readData(char* output, qint64 length) override
    {
        const auto count = qMin(length, bytes.size() - offset);
        if (count <= 0)
            return -1;
        memcpy(output, bytes.constData() + offset, static_cast<size_t>(count));
        offset += count;
        return count;
    }

private:
    QByteArray bytes;
    qint64 offset = 0;
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir root;
    Path::portableRoot = root.path();
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            ++failures;
        }
    };
    AutoUpdateChecker checker;
    int notifications = 0;
    QObject::connect(&checker, &AutoUpdateChecker::onUpdateAvailable,
                     [&](const QString&, const QString&) { ++notifications; });
    check(checker.m_CurrentVersion == QVersionNumber({ 6, 5, 3, 181 }),
          "Git revision must participate in fork version comparison");
    checker.start();
    check(requestedUrl == QUrl("https://api.github.com/repos/AsafMah/moonlight-qt/releases/latest"),
          "fork builds must never query upstream releases");
    const QString digest = "sha256:" + QString(64, 'a');
    const auto response = [&](const QString& filename, const QString& assetDigest,
                              const QString& repo = QStringLiteral("AsafMah/moonlight-qt")) {
        return QJsonObject{
            { "tag_name", "v6.5.3.182" },
            { "prerelease", false },
            { "draft", false },
            { "html_url", "https://github.com/" + repo + "/releases/tag/v6.5.3.182" },
            { "assets",
              QJsonArray{ QJsonObject{
                  { "name", filename },
                  { "browser_download_url",
                    "https://github.com/" + repo + "/releases/download/v6.5.3.182/" + filename },
                  { "digest", assetDigest } } } }
        };
    };
    const auto replay = [&](QJsonObject release) {
        notifications = 0;
        checker.handleUpdateCheckRequestFinished(new Reply(release));
    };
    QFile marker(root.filePath("portable.dat"));
    marker.open(QIODevice::WriteOnly);
    marker.close();
    const auto portable =
        QString("Moonlight-VPlus-Portable-%1-6.5.3.182.zip").arg(checker.getCurrentBuildArch());
    replay(response(portable, digest));
    check(notifications == 1 && checker.m_UpdateAssetDigest == digest,
          "portable architecture asset must be selected with required SHA256");
    for (const auto& architecture : { QStringLiteral("x86_64"), QStringLiteral("arm64") }) {
        testArchitecture = architecture;
        const auto name =
            QString("Moonlight-VPlus-Portable-%1-6.5.3.182.zip").arg(checker.getCurrentBuildArch());
        replay(response(name, digest));
        check(notifications == 1 && checker.m_UpdateDownloadUrl.endsWith(name),
              "both x64 and arm64 portable selectors must choose their own asset");
    }
    testArchitecture = "x86_64";
    replay(response(portable, ""));
    check(checker.m_UpdateDownloadUrl.isEmpty() || checker.m_UpdateAssetDigest == digest,
          "asset with missing digest must not become executable update candidate");
    replay(response(portable, "sha256:invalid"));
    check(checker.m_UpdateDownloadUrl.isEmpty(),
          "malformed integrity must not become an update candidate");
    replay(response(portable, digest, "qiin2333/moonlight-qt"));
    check(notifications == 0, "foreign release must not be offered to a fork build");
    auto prerelease = response(portable, digest);
    prerelease["prerelease"] = true;
    replay(prerelease);
    check(notifications == 0, "prerelease must not be offered");
    auto current = response(portable, digest);
    current["tag_name"] = "v6.5.3.181";
    replay(current);
    check(notifications == 0, "current build must not be offered to itself");
    marker.remove();
    replay(response("Moonlight-VPlus-Setup-6.5.3.182.exe", digest));
    check(notifications == 1 && checker.m_UpdateDownloadUrl.endsWith(".exe"),
          "setup installation must select the verified Setup executable");
    check(VersionUtils::parseBuild("6.5.3.181+2.gabcdef01") == QVersionNumber({ 6, 5, 3, 183 }),
          "version-shaped release tags must retain revision across future commits");
    if (failures)
        return 1;
    std::puts("Fork release selection passed");
    return 0;
}
