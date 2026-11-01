#pragma once
#include <QObject>
#include <functional>

class PortableUpdateInstaller : public QObject
{
    Q_OBJECT
public:
    explicit PortableUpdateInstaller(QObject* parent = nullptr) : QObject(parent) {}
    bool supportsInAppUpdate() const { return true; }
    bool busy = false;
    bool isBusy() const { return busy; }
    int installs = 0;
    QString lastUrl;
    void installUpdate(const QString& url, const QString&)
    {
        ++installs;
        lastUrl = url;
    }
    void cancelUpdate() {}
    void setDeferInstall(bool) {}
    int launches = 0;
    void beginPreparedUpdate() { ++launches; }
    void setStreamingActiveProvider(std::function<bool()>) {}
signals:
    void onPortableUpdateStatusChanged(QString);
    void onPortableUpdateFailed(QString);
    void onUpdateCancelled();
    void onUpdateInstallStarted();
    void onUpdateReady();
    void onUpdateDeferred();
};
