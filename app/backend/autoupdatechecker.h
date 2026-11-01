#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QVersionNumber>
#include <QTimer>
#include <functional>

class QNetworkReply;
class PortableUpdateInstaller;

class AutoUpdateChecker : public QObject
{
    Q_OBJECT
    Q_PROPERTY(
        QString automaticUpdateStatus READ automaticUpdateStatus NOTIFY automaticUpdateChanged)
    Q_PROPERTY(
        bool automaticUpdateInProgress READ automaticUpdateInProgress NOTIFY automaticUpdateChanged)
    Q_PROPERTY(int restartCountdown READ restartCountdown NOTIFY automaticUpdateChanged)
public:
    explicit AutoUpdateChecker(QObject *parent = nullptr);

    Q_INVOKABLE void start();
    Q_INVOKABLE bool supportsUpdateCheck() const;
    // Returns true only when a new check was started.
    Q_INVOKABLE bool checkForUpdates();
    Q_INVOKABLE bool supportsInAppUpdate() const;
    Q_INVOKABLE void installUpdate(QString url);
    Q_INVOKABLE void cancelUpdate();
    void setStreamingActiveProvider(std::function<bool()> provider);
    Q_INVOKABLE void configureAutomaticUpdates(bool checkEnabled, bool installEnabled);
    QString automaticUpdateStatus() const { return m_AutomaticStatus; }
    bool automaticUpdateInProgress() const { return m_AutomaticInProgress; }
    int restartCountdown() const { return m_RestartCountdown; }

signals:
    void onUpdateCheckStarted();
    void onUpdateCheckFinished(bool updateAvailable);
    void onUpdateCheckFailed();
    void onUpdateAvailable(QString newVersion, QString url);
    void onPortableUpdateStatusChanged(QString message);
    void onPortableUpdateFailed(QString message);
    void onUpdateCancelled();
    void onUpdateInstallStarted();
    void automaticUpdateChanged();

private slots:
    void handleUpdateCheckRequestFinished(QNetworkReply* reply);

private:
    bool isPortableInstall() const;
    QString getExpectedAssetPrefix() const;
    QString getExpectedAssetSuffix() const;
    // 同一个后缀里再优先挑本机架构的资产（macOS DMG 和 Linux AppImage）。
    QString getPreferredAssetSuffix() const;
    QString getCurrentBuildArch() const;
    void automaticTick();
    void tryAutomaticInstall();
    void setAutomaticStatus(const QString& status);

    QVersionNumber m_CurrentVersion;
    QNetworkAccessManager* m_Nam;
    PortableUpdateInstaller* m_PortableUpdateInstaller;
    bool m_UpdateCheckInProgress = false;
    QString m_UpdateDownloadUrl;
    QString m_UpdateAssetDigest;
    QTimer m_PeriodicCheck;
    QTimer m_IdleTimer;
    std::function<bool()> m_StreamingActive = [] {
        return false;
    };
    bool m_AutomaticCheckEnabled = false;
    bool m_AutomaticInstallEnabled = false;
    bool m_AutomaticInProgress = false;
    bool m_UpdateReady = false;
    bool m_CheckDue = false;
    int m_RestartCountdown = 0;
    QString m_AutomaticStatus;
    QString m_AttemptedUpdateUrl;
    QString m_CancelledUpdateUrl;
};
