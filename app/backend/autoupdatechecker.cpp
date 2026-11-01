#include "autoupdatechecker.h"
#include "portableupdateinstaller.h"
#include "path.h"
#include "versionutils.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QSysInfo>
#include <QTextStream>
#include <QTimer>
#include <QRegularExpression>

#ifndef UPDATE_REPOSITORY
#define UPDATE_REPOSITORY "qiin2333/moonlight-qt"
#endif
#ifndef UPDATE_BUILD_IS_RELEASE
#define UPDATE_BUILD_IS_RELEASE 0
#endif

static bool isUpdateRepositoryUrl(const QString& value, const QString& pathPrefix)
{
    const QUrl url(value);
    return url.scheme() == QStringLiteral("https") && url.host() == QStringLiteral("github.com") &&
           url.userInfo().isEmpty() &&
           url.path().startsWith(QStringLiteral("/") + QStringLiteral(UPDATE_REPOSITORY) +
                                 pathPrefix);
}

static bool hasUpdateDigest(const QString& digest)
{
    static const QRegularExpression pattern(QStringLiteral("^sha256:[0-9a-fA-F]{64}$"));
    return pattern.match(digest).hasMatch();
}

AutoUpdateChecker::AutoUpdateChecker(QObject *parent) :
    QObject(parent)
{
    m_Nam = new QNetworkAccessManager(this);
    m_PortableUpdateInstaller = new PortableUpdateInstaller(this);

    // Never communicate over HTTP
    m_Nam->setStrictTransportSecurityEnabled(true);

    // Allow HTTP redirects
    m_Nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);

    connect(m_Nam, &QNetworkAccessManager::finished,
            this, &AutoUpdateChecker::handleUpdateCheckRequestFinished);

    QString currentVersion(VERSION_STR);
    qDebug() << "Current Moonlight version:" << currentVersion;
    m_CurrentVersion = VersionUtils::parseBuild(currentVersion);

    // Should at least have a 1.0-style version number
    Q_ASSERT(m_CurrentVersion.segmentCount() > 1);

    connect(m_PortableUpdateInstaller, &PortableUpdateInstaller::onPortableUpdateStatusChanged,
            this, [this](const QString& message) {
                if (m_AutomaticInProgress)
                    setAutomaticStatus(message);
                emit onPortableUpdateStatusChanged(message);
            });
    connect(m_PortableUpdateInstaller, &PortableUpdateInstaller::onPortableUpdateFailed, this,
            [this](const QString& message) {
                if (m_AutomaticInProgress)
                    setAutomaticStatus(
                        tr("Automatic update failed: %1. Use the update button to retry.")
                            .arg(message));
                emit onPortableUpdateFailed(message);
                m_AutomaticInProgress = false;
                m_UpdateReady = false;
                m_RestartCountdown = 0;
                emit automaticUpdateChanged();
            });
    connect(m_PortableUpdateInstaller, &PortableUpdateInstaller::onUpdateCancelled, this,
            &AutoUpdateChecker::onUpdateCancelled);
    connect(m_PortableUpdateInstaller, &PortableUpdateInstaller::onUpdateInstallStarted, this,
            &AutoUpdateChecker::onUpdateInstallStarted);
    connect(m_PortableUpdateInstaller, &PortableUpdateInstaller::onUpdateReady, this, [this]() {
        m_UpdateReady = true;
        m_RestartCountdown = 15;
        setAutomaticStatus(tr("Update verified. Moonlight will restart shortly."));
    });
    connect(m_PortableUpdateInstaller, &PortableUpdateInstaller::onUpdateDeferred, this, [this]() {
        m_AutomaticInProgress = false;
        m_UpdateReady = false;
        m_AttemptedUpdateUrl.clear();
        setAutomaticStatus(tr("Update deferred until streaming ends."));
    });
    connect(this, &AutoUpdateChecker::onUpdateCheckFinished, this, [this]() {
        m_CheckDue = false;
        tryAutomaticInstall();
    });
    m_PeriodicCheck.setInterval(6 * 60 * 60 * 1000);
    connect(&m_PeriodicCheck, &QTimer::timeout, this, [this]() {
        m_CheckDue = true;
        m_AttemptedUpdateUrl.clear();
        automaticTick();
    });
    m_IdleTimer.setInterval(1000);
    connect(&m_IdleTimer, &QTimer::timeout, this, &AutoUpdateChecker::automaticTick);
}

void AutoUpdateChecker::cancelUpdate()
{
    if (m_AutomaticInProgress) {
        m_CancelledUpdateUrl = m_UpdateDownloadUrl;
        m_AutomaticInProgress = false;
        m_UpdateReady = false;
        m_RestartCountdown = 0;
        setAutomaticStatus(
            tr("Automatic update cancelled. Use the update button to install later."));
    }
    m_PortableUpdateInstaller->cancelUpdate();
}

void AutoUpdateChecker::configureAutomaticUpdates(bool checkEnabled, bool installEnabled)
{
    const bool startCheck = checkEnabled && !m_AutomaticCheckEnabled;
    m_AutomaticCheckEnabled = checkEnabled;
    m_AutomaticInstallEnabled = installEnabled;
    if ((!checkEnabled || !installEnabled) && m_AutomaticInProgress)
        cancelUpdate();
#if UPDATE_BUILD_IS_RELEASE
    if (!QStringLiteral(VERSION_STR).contains(QStringLiteral("dirty"))) {
        if (checkEnabled) {
            if (!m_PeriodicCheck.isActive())
                m_PeriodicCheck.start();
            m_IdleTimer.start();
            m_CheckDue = m_CheckDue || startCheck;
            automaticTick();
        } else {
            m_PeriodicCheck.stop();
            m_IdleTimer.stop();
            m_CheckDue = false;
        }
    }
#else
    Q_UNUSED(startCheck);
#endif
}

void AutoUpdateChecker::setAutomaticStatus(const QString& status)
{
    m_AutomaticStatus = status;
    emit automaticUpdateChanged();
}

void AutoUpdateChecker::tryAutomaticInstall()
{
    if (!m_AutomaticCheckEnabled || !m_AutomaticInstallEnabled || m_AutomaticInProgress ||
        m_PortableUpdateInstaller->isBusy() || !supportsInAppUpdate() ||
        m_UpdateDownloadUrl == m_AttemptedUpdateUrl || m_UpdateDownloadUrl == m_CancelledUpdateUrl)
        return;
    if (m_StreamingActive()) {
        setAutomaticStatus(tr("Update available; installation deferred until streaming ends."));
        return;
    }
    m_AttemptedUpdateUrl = m_UpdateDownloadUrl;
    m_AutomaticInProgress = true;
    m_PortableUpdateInstaller->setDeferInstall(true);
    setAutomaticStatus(tr("Downloading automatic update..."));
    m_PortableUpdateInstaller->installUpdate(m_UpdateDownloadUrl, m_UpdateAssetDigest);
}

void AutoUpdateChecker::automaticTick()
{
    if (m_StreamingActive()) {
        if (m_UpdateReady) {
            m_RestartCountdown = 15;
            setAutomaticStatus(tr("Verified update deferred until streaming ends."));
        }
        return;
    }
    if (m_UpdateReady && m_AutomaticInProgress) {
        if (--m_RestartCountdown <= 0) {
            m_UpdateReady = false;
            m_RestartCountdown = 0;
            m_PortableUpdateInstaller->beginPreparedUpdate();
        } else {
            setAutomaticStatus(
                tr("Restarting to install the update in %1 seconds.").arg(m_RestartCountdown));
        }
        return;
    }
    if (m_CheckDue && !m_AutomaticInProgress && checkForUpdates())
        m_CheckDue = false;
    tryAutomaticInstall();
}

void AutoUpdateChecker::setStreamingActiveProvider(std::function<bool()> provider)
{
    m_StreamingActive = std::move(provider);
    m_PortableUpdateInstaller->setStreamingActiveProvider(m_StreamingActive);
}

bool AutoUpdateChecker::supportsInAppUpdate() const
{
    return !m_UpdateDownloadUrl.isEmpty() && hasUpdateDigest(m_UpdateAssetDigest) &&
           isUpdateRepositoryUrl(m_UpdateDownloadUrl, QStringLiteral("/releases/download/")) &&
           m_PortableUpdateInstaller->supportsInAppUpdate();
}

bool AutoUpdateChecker::supportsUpdateCheck() const
{
#if defined(Q_OS_WIN32) || defined(Q_OS_DARWIN) || defined(STEAM_LINK) || defined(APP_IMAGE)
    return true;
#else
    return false;
#endif
}

void AutoUpdateChecker::installUpdate(QString url)
{
    if (url != m_UpdateDownloadUrl || !supportsInAppUpdate()) {
        emit onPortableUpdateFailed(
            tr("No verified update package is available for this installation."));
        return;
    }
    if (m_AutomaticInProgress || m_PortableUpdateInstaller->isBusy()) {
        emit onPortableUpdateStatusChanged(tr("An update is already in progress."));
        return;
    }
    m_PortableUpdateInstaller->setDeferInstall(false);
    m_PortableUpdateInstaller->installUpdate(url, m_UpdateAssetDigest);
}

void AutoUpdateChecker::start()
{
#if UPDATE_BUILD_IS_RELEASE
    if (!QStringLiteral(VERSION_STR).contains(QStringLiteral("dirty"))) {
        checkForUpdates();
    }
#endif
}

bool AutoUpdateChecker::checkForUpdates()
{
    if (!supportsUpdateCheck()) {
        return false;
    }

    if (m_UpdateCheckInProgress || m_AutomaticInProgress || m_PortableUpdateInstaller->isBusy()) {
        // The automatic startup check may still be running. Do not let a
        // newly opened settings page wait for a signal it cannot observe.
        return false;
    }

    m_UpdateCheckInProgress = true;
    emit onUpdateCheckStarted();

#if defined(Q_OS_WIN32) || defined(Q_OS_DARWIN) || defined(STEAM_LINK) || defined(APP_IMAGE)
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0) && QT_VERSION < QT_VERSION_CHECK(5, 15, 1) && !defined(QT_NO_BEARERMANAGEMENT)
    // HACK: Set network accessibility to work around QTBUG-80947 (introduced in Qt 5.14.0 and fixed in Qt 5.15.1)
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    m_Nam->setNetworkAccessible(QNetworkAccessManager::Accessible);
    QT_WARNING_POP
#endif

    // Query GitHub Releases API for the latest release
    QUrl url(QStringLiteral("https://api.github.com/repos/") + QStringLiteral(UPDATE_REPOSITORY) +
             QStringLiteral("/releases/latest"));
    QNetworkRequest request(url);

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    // Keep a manual check from leaving its button busy forever when the
    // network is unavailable but the connection never closes.
    request.setTransferTimeout(15000);
#endif

    // GitHub API requires a User-Agent header
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QString("Moonlight/%1").arg(VERSION_STR));
    // Request JSON response
    request.setRawHeader("Accept", "application/vnd.github+json");

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
#else
    request.setAttribute(QNetworkRequest::HTTP2AllowedAttribute, true);
#endif
    QNetworkReply* reply = m_Nam->get(request);
    // Some macOS network paths can remain in DNS/connect state without
    // honoring QNetworkRequest::setTransferTimeout(). Abort explicitly so
    // the UI always receives a terminal result. Parent the timer to the
    // reply so it cannot outlive the request or touch a deleted reply.
    QTimer* timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(reply, &QNetworkReply::finished, timeout, &QTimer::stop);
    connect(timeout, &QTimer::timeout, this, [this, reply]() {
        if (!reply->isRunning()) {
            return;
        }

        qWarning() << "Update check timed out";
        reply->abort();
        if (m_UpdateCheckInProgress) {
            m_UpdateCheckInProgress = false;
            emit onUpdateCheckFailed();
        }
    });
    timeout->start(15000);
#endif

    return true;
}

QString AutoUpdateChecker::getPreferredAssetSuffix() const
{
#if defined(Q_OS_DARWIN)
    // CI 出的 DMG 现在带架构后缀（Moonlight-<版本>-arm64.dmg）。
    // QSysInfo::buildCpuArchitecture() 给的是 arm64 / x86_64，和 generate-dmg.sh
    // 里的 MOONLIGHT_ARCH 用词一致。
    //
    // 只是「优先」而不是「必须」：这个后缀是从某个版本才开始有的，旧 release 里是
    // Moonlight-<版本>.dmg。匹配不到就退回任意 .dmg，否则老版本的用户会看到
    // 「找不到更新包」。
    return QStringLiteral("-") + QSysInfo::buildCpuArchitecture() + QStringLiteral(".dmg");
#elif defined(APP_IMAGE)
    QString architecture = QSysInfo::buildCpuArchitecture().toLower();
    if (architecture == QStringLiteral("arm64")) {
        architecture = QStringLiteral("aarch64");
    }
    return QStringLiteral("-") + architecture + QStringLiteral(".AppImage");
#else
    return QString();
#endif
}

QString AutoUpdateChecker::getExpectedAssetSuffix() const
{
#if defined(Q_OS_WIN32)
    return isPortableInstall() ? QStringLiteral(".zip") : QStringLiteral(".exe");
#elif defined(Q_OS_DARWIN)
    return QStringLiteral(".dmg");
#elif defined(APP_IMAGE)
    return QStringLiteral(".AppImage");
#else
    return QString();
#endif
}

bool AutoUpdateChecker::isPortableInstall() const
{
#if defined(Q_OS_WIN32)
    return QFile::exists(QDir(Path::getPortableRootDir()).filePath("portable.dat"));
#else
    return false;
#endif
}

QString AutoUpdateChecker::getExpectedAssetPrefix() const
{
#if defined(Q_OS_WIN32)
    if (isPortableInstall()) {
        return QStringLiteral("Moonlight-VPlus-Portable-%1-").arg(getCurrentBuildArch());
    }

    return QStringLiteral("Moonlight-VPlus-Setup-");
#else
    return QString();
#endif
}

QString AutoUpdateChecker::getCurrentBuildArch() const
{
    QString buildArch = QSysInfo::buildCpuArchitecture();

    if (buildArch == "x86_64") {
        return QStringLiteral("x64");
    }
    else if (buildArch == "i386") {
        return QStringLiteral("x86");
    }

    return buildArch.toLower();
}

void AutoUpdateChecker::handleUpdateCheckRequestFinished(QNetworkReply* reply)
{
    Q_ASSERT(reply->isFinished());

    auto finish = [this](bool updateAvailable) {
        m_UpdateCheckInProgress = false;
        emit onUpdateCheckFinished(updateAvailable);
    };
    auto fail = [this]() {
        m_UpdateCheckInProgress = false;
        emit onUpdateCheckFailed();
    };

    if (reply->error() == QNetworkReply::NoError) {
        QTextStream stream(reply);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        stream.setEncoding(QStringConverter::Utf8);
#else
        stream.setCodec("UTF-8");
#endif

        // Read all data and queue the reply for deletion
        QString jsonString = stream.readAll();
        reply->deleteLater();

        QJsonParseError error;
        QJsonDocument jsonDoc = QJsonDocument::fromJson(jsonString.toUtf8(), &error);
        if (jsonDoc.isNull()) {
            qWarning() << "GitHub release response malformed:" << error.errorString();
            fail();
            return;
        }

        if (!jsonDoc.isObject()) {
            qWarning() << "GitHub release response is not a JSON object";
            fail();
            return;
        }

        QJsonObject releaseObj = jsonDoc.object();
        m_UpdateDownloadUrl.clear();
        m_UpdateAssetDigest.clear();
        if (!isUpdateRepositoryUrl(releaseObj["html_url"].toString(),
                                   QStringLiteral("/releases/tag/"))) {
            qWarning() << "Release does not belong to the configured update repository";
            fail();
            return;
        }

        // GitHub Releases API response format:
        // {
        //   "tag_name": "v6.3.0",
        //   "name": "Release 6.3.0",
        //   "html_url": "https://github.com/owner/repo/releases/tag/v6.3.0",
        //   "prerelease": false,
        //   "draft": false,
        //   "assets": [
        //     {
        //       "name": "MoonlightSetup-x64-6.3.0.exe",
        //       "browser_download_url": "https://github.com/..."
        //     }
        //   ]
        // }

        // Skip pre-releases and drafts
        if (releaseObj["prerelease"].toBool(false) || releaseObj["draft"].toBool(false)) {
            qDebug() << "Latest GitHub release is a pre-release or draft, skipping";
            finish(false);
            return;
        }

        if (!releaseObj.contains("tag_name") || !releaseObj["tag_name"].isString()) {
            qWarning() << "GitHub release missing tag_name";
            fail();
            return;
        }

        QString tagName = releaseObj["tag_name"].toString();
        qDebug() << "Latest GitHub release tag:" << tagName;

        // Parse version from tag (strip 'v' prefix if present)
        const QVersionNumber latestVersion = VersionUtils::parseRelease(tagName);
        if (m_CurrentVersion.segmentCount() < 2 || latestVersion.segmentCount() < 2) {
            qWarning() << "Invalid release version:" << QStringLiteral(VERSION_STR) << tagName;
            fail();
            return;
        }

        int res = VersionUtils::compare(m_CurrentVersion, latestVersion);
        if (res < 0) {
            // Current version is older than latest release
            qDebug() << "Update available:" << tagName;

            // Try to find a platform-specific download URL from assets
            QString downloadUrl;
            QString assetDigest;
            QString expectedPrefix = getExpectedAssetPrefix();
            QString expectedSuffix = getExpectedAssetSuffix();

            QString preferredSuffix = getPreferredAssetSuffix();

            if (!expectedSuffix.isEmpty() && releaseObj.contains("assets") && releaseObj["assets"].isArray()) {
                QJsonArray assets = releaseObj["assets"].toArray();

                // 后备候选：后缀对得上但不带本机架构后缀的那个（旧 release 的命名）
                QString fallbackUrl;
                QString fallbackName;
                QString fallbackDigest;

                for (const auto& asset : std::as_const(assets)) {
                    if (asset.isObject()) {
                        QJsonObject assetObj = asset.toObject();
                        if (!hasUpdateDigest(assetObj["digest"].toString()) ||
                            !isUpdateRepositoryUrl(assetObj["browser_download_url"].toString(),
                                                   QStringLiteral("/releases/download/"))) {
                            qWarning()
                                << "Skipping update asset with missing integrity or foreign URL";
                            continue;
                        }
                        QString assetName = assetObj["name"].toString();
                        bool prefixMatches = expectedPrefix.isEmpty() ||
                                             assetName.startsWith(expectedPrefix, Qt::CaseInsensitive);
#if defined(Q_OS_WIN32)
                        // Accept pre-rebrand assets while users transition from Moonlight PC.
                        if (!prefixMatches) {
                            const QString legacyPrefix = isPortableInstall()
                                    ? QStringLiteral("MoonlightPortable-%1-").arg(getCurrentBuildArch())
                                    : QStringLiteral("MoonlightSetup-");
                            prefixMatches = assetName.startsWith(legacyPrefix, Qt::CaseInsensitive);
                        }
#endif
                        bool suffixMatches = assetName.endsWith(expectedSuffix, Qt::CaseInsensitive);

                        if (!prefixMatches || !suffixMatches) {
                            continue;
                        }

                        if (!preferredSuffix.isEmpty() &&
                                assetName.endsWith(preferredSuffix, Qt::CaseInsensitive)) {
                            downloadUrl = assetObj["browser_download_url"].toString();
                            assetDigest = assetObj["digest"].toString();
                            qDebug() << "Found matching asset for this architecture:" << assetName;
                            break;
                        }

                        // 后备只认「没带架构后缀」的旧命名。带了别的架构后缀的资产
                        // 绝对不能当后备 —— 只发了 arm64 包的 release 会把 arm64 的
                        // DMG 喂给 Intel 客户端。这种情况下宁可让 downloadUrl 留空，
                        // 退回打开 release 页面让用户自己看。
                        bool isOtherArchAsset =
                                assetName.endsWith(QStringLiteral("-arm64.dmg"), Qt::CaseInsensitive) ||
                                assetName.endsWith(QStringLiteral("-x86_64.dmg"), Qt::CaseInsensitive) ||
                                (!preferredSuffix.isEmpty() &&
                                 (assetName.endsWith(QStringLiteral("-aarch64.AppImage"), Qt::CaseInsensitive) ||
                                  assetName.endsWith(QStringLiteral("-x86_64.AppImage"), Qt::CaseInsensitive)));

                        if (fallbackUrl.isEmpty() && !isOtherArchAsset) {
                            fallbackUrl = assetObj["browser_download_url"].toString();
                            fallbackName = assetName;
                            fallbackDigest = assetObj["digest"].toString();
                        }
                    }
                }

                if (downloadUrl.isEmpty() && !fallbackUrl.isEmpty()) {
                    downloadUrl = fallbackUrl;
                    assetDigest = fallbackDigest;
                    qDebug() << "Found matching asset:" << fallbackName;
                }
            }

            m_UpdateDownloadUrl = downloadUrl;
            m_UpdateAssetDigest = assetDigest;

            emit onUpdateAvailable(
                tagName, downloadUrl.isEmpty() ? releaseObj["html_url"].toString() : downloadUrl);
            finish(true);
        }
        else if (res > 0) {
            qDebug() << "Current version is newer than latest release";
            finish(false);
        }
        else {
            qDebug() << "Current version matches latest release";
            finish(false);
        }
    }
    else {
        qWarning() << "Update checking failed:" << reply->error() << reply->errorString();
        reply->deleteLater();
        fail();
    }
}
