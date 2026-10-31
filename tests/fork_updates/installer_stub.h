#pragma once
#include <QObject>
#include <functional>

class PortableUpdateInstaller : public QObject
{
    Q_OBJECT
public:
    explicit PortableUpdateInstaller(QObject* parent = nullptr) : QObject(parent) {}
    bool supportsInAppUpdate() const { return true; }
    void installUpdate(const QString&, const QString&) {}
    void cancelUpdate() {}
    void setStreamingActiveProvider(std::function<bool()>) {}
signals:
    void onPortableUpdateStatusChanged(QString);
    void onPortableUpdateFailed(QString);
    void onUpdateCancelled();
    void onUpdateInstallStarted();
};
