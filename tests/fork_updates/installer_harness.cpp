#include <QCoreApplication>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTemporaryDir>
#include <cstdio>

class Path
{
public:
    static QString root;
    static QString getPortableRootDir() { return root; }
};
QString Path::root;
int requests = 0;
class ScriptedNetworkAccessManager : public QNetworkAccessManager
{
public:
    using QNetworkAccessManager::QNetworkAccessManager;

protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest&, QIODevice*) override
    {
        ++requests;
        class PendingReply : public QNetworkReply
        {
        public:
            explicit PendingReply(QObject* parent) : QNetworkReply(parent) { open(ReadOnly); }
            void abort() override
            {
                setError(OperationCanceledError, "cancelled");
                setFinished(true);
                emit finished();
            }

        protected:
            qint64 readData(char*, qint64) override { return -1; }
        };
        return new PendingReply(this);
    }
};
#include "installer.inc"

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir root;
    Path::root = root.path();
    QFile marker(root.filePath("portable.dat"));
    marker.open(QIODevice::WriteOnly);
    marker.close();
    int failures = 0;
    auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            ++failures;
        }
    };
    const QString url = "https://github.com/AsafMah/moonlight-qt/releases/download/v6.5.3.182/"
                        "Moonlight-VPlus-Portable-x64-6.5.3.182.zip";
    {
        PortableUpdateInstaller installer;
        installer.setStreamingActiveProvider([] { return true; });
        installer.installUpdate(url, "sha256:" + QString(64, 'a'));
        check(requests == 0, "active streaming must refuse any update download");
    }
    {
        PortableUpdateInstaller installer;
        installer.installUpdate(url, "");
        check(requests == 0, "missing SHA256 must refuse download");
        installer.resetPortableUpdateState(true);
    }
    {
        PortableUpdateInstaller installer;
        installer.installUpdate(url, "sha256:" + QString(64, 'a'));
        check(requests == 1, "verified package must start exactly one download");
        check(QMetaObject::invokeMethod(&installer, "cancelUpdate"),
              "download cancellation must have a real invokable path");
        check(installer.m_UpdateReply == nullptr, "cancel must discard pending download");
        installer.resetPortableUpdateState(true);
    }
    marker.remove();
    {
        PortableUpdateInstaller installer;
        check(installer.supportsInAppUpdate(),
              "Setup installations must support the verified in-app update path");
    }
    if (failures)
        return 1;
    std::puts("Installer preflight passed");
    return 0;
}
