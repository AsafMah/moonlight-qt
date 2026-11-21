#include <QEvent>
#include <QThread>

#if UPDATE_DISABLE_AUTOMATIC_UPDATES
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir root;
    Path::portableRoot = root.path();
    QFile marker(root.filePath("portable.dat"));
    marker.open(QIODevice::WriteOnly);
    marker.close();
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            ++failures;
        }
    };
    const QString digest = "sha256:" + QString(64, 'a');
    const QString url = "https://github.com/AsafMah/moonlight-qt/releases/download/v6.5.3.182/"
                        "Moonlight-VPlus-Portable-x64-6.5.3.182.zip";
    auto release = [&](const QString& assetDigest) {
        return QJsonObject{
            { "tag_name", "v6.5.3.182" },
            { "prerelease", false },
            { "draft", false },
            { "html_url", "https://github.com/AsafMah/moonlight-qt/releases/tag/v6.5.3.182" },
            { "assets",
              QJsonArray{ QJsonObject{ { "name", "Moonlight-VPlus-Portable-x64-6.5.3.182.zip" },
                                       { "digest", assetDigest },
                                       { "browser_download_url", url } } } }
        };
    };
    auto receive = [&](AutoUpdateChecker& currentChecker, const QJsonObject& response) {
        currentChecker.handleUpdateCheckRequestFinished(new Reply(response));
    };
    AutoUpdateChecker checker;
    checker.configureAutomaticUpdates(true, true);
    checker.start();
    check(requestedUrl.isEmpty(), "Disabled automatic updates must not check on startup");
    check(checker.checkForUpdates(), "An explicit manual update check must remain available");
    receive(checker, release(digest));
    check(checker.m_PortableUpdateInstaller->installs == 0,
          "Disabled automatic updates must not install after a completed check");
    checker.installUpdate(url);
    check(checker.m_PortableUpdateInstaller->installs == 1,
          "Explicit manual update installation must remain available");
    if (failures)
        return 1;
    std::puts("Automatic updates disabled");
    return 0;
}
#else
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir root;
    Path::portableRoot = root.path();
    QFile marker(root.filePath("portable.dat"));
    marker.open(QIODevice::WriteOnly);
    marker.close();
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            ++failures;
        }
    };
    const auto deliverTimeout = [&](QTimer& timer) {
        check(QMetaObject::invokeMethod(&timer, "timeout", Qt::QueuedConnection),
              "Manual timeout must use the real timer signal");
        const int delay = qEnvironmentVariableIntValue("MOONLIGHT_TEST_EVENT_LOOP_DELAY_MS");
        if (delay > 0)
            QThread::msleep(static_cast<unsigned long>(delay));
        QCoreApplication::sendPostedEvents(&timer, QEvent::MetaCall);
    };
    const QString url = "https://github.com/AsafMah/moonlight-qt/releases/download/v6.5.3.182/"
                        "Moonlight-VPlus-Portable-x64-6.5.3.182.zip";
    auto release = [&](const QString& digest, const QString& download = QString()) {
        return QJsonObject{
            { "tag_name", "v6.5.3.182" },
            { "prerelease", false },
            { "draft", false },
            { "html_url", "https://github.com/AsafMah/moonlight-qt/releases/tag/v6.5.3.182" },
            { "assets", QJsonArray{ QJsonObject{
                            { "name", "Moonlight-VPlus-Portable-x64-6.5.3.182.zip" },
                            { "digest", digest },
                            { "browser_download_url", download.isEmpty() ? url : download } } } }
        };
    };
    auto receive = [&](AutoUpdateChecker& checker, const QJsonObject& response) {
        checker.handleUpdateCheckRequestFinished(new Reply(response));
    };
    const QString digest = "sha256:" + QString(64, 'a');
    {
        AutoUpdateChecker checker;
        checker.configureAutomaticUpdates(true, true);
        receive(checker, release(digest));
        check(checker.m_PortableUpdateInstaller->installs == 1,
              "Verified update must auto-install when idle");
    }
    {
        bool streaming = true;
        AutoUpdateChecker checker;
        checker.setStreamingActiveProvider([&] { return streaming; });
        checker.configureAutomaticUpdates(true, true);
        receive(checker, release(digest));
        check(checker.m_PortableUpdateInstaller->installs == 0,
              "Automatic installation must defer during streaming");
        check(checker.m_IdleTimer.isActive(), "Deferred installation must start the idle timer");
        checker.m_IdleTimer.stop();
        checker.m_PeriodicCheck.stop();
        streaming = false;
        deliverTimeout(checker.m_IdleTimer);
        check(checker.m_PortableUpdateInstaller->installs == 1,
              "Deferred automatic install must fire when the stream ends");
    }
    {
        AutoUpdateChecker checker;
        checker.configureAutomaticUpdates(true, false);
        receive(checker, release(digest));
        check(checker.m_PortableUpdateInstaller->installs == 0,
              "Preference off must leave installation manual");
    }
    for (const auto& response :
         { release(QString()),
           release(digest, "https://github.com/foreign/repo/releases/download/v1/package.zip") }) {
        AutoUpdateChecker checker;
        checker.configureAutomaticUpdates(true, true);
        receive(checker, response);
        check(checker.m_PortableUpdateInstaller->installs == 0,
              "Missing digest or foreign URL must never auto-install");
    }
    {
        AutoUpdateChecker checker;
        checker.configureAutomaticUpdates(true, true);
        receive(checker, release(digest));
        auto installer = checker.m_PortableUpdateInstaller;
        emit installer->onUpdateReady();
        check(checker.restartCountdown() == 15 && installer->launches == 0,
              "Verified update must offer a cancellable countdown before restart");
        check(!checker.checkForUpdates(),
              "A manual check must not overwrite verified identity during automatic install");
        for (int i = 0; i < 14; ++i)
            checker.automaticTick();
        check(installer->launches == 0 && checker.restartCountdown() == 1,
              "Installer must not start before the final countdown tick");
        checker.automaticTick();
        check(installer->launches == 1,
              "Countdown must launch the existing installer exactly once");
        checker.automaticTick();
        check(installer->launches == 1, "Completed countdown must not launch twice");
    }
    {
        bool streaming = false;
        AutoUpdateChecker checker;
        checker.setStreamingActiveProvider([&] { return streaming; });
        checker.configureAutomaticUpdates(true, true);
        receive(checker, release(digest));
        emit checker.m_PortableUpdateInstaller->onUpdateReady();
        streaming = true;
        for (int i = 0; i < 20; ++i)
            checker.automaticTick();
        check(checker.m_PortableUpdateInstaller->launches == 0 && checker.restartCountdown() == 15,
              "Starting a stream must stop the verified restart countdown");
        streaming = false;
        checker.cancelUpdate();
        checker.automaticTick();
        check(checker.m_PortableUpdateInstaller->launches == 0 &&
                  checker.m_PortableUpdateInstaller->installs == 1 &&
                  !checker.automaticUpdateInProgress(),
              "Cancel must suppress automatic restart and same-version retry");
    }
    {
        AutoUpdateChecker checker;
        checker.configureAutomaticUpdates(true, true);
        receive(checker, release(digest));
        emit checker.m_PortableUpdateInstaller->onPortableUpdateFailed("test failure");
        for (int i = 0; i < 20; ++i)
            checker.automaticTick();
        check(checker.m_PortableUpdateInstaller->installs == 1 &&
                  !checker.automaticUpdateInProgress(),
              "Automatic failure must not retry in a tight loop");
        checker.installUpdate(url);
        check(checker.m_PortableUpdateInstaller->installs == 2,
              "Automatic failure must retain the verified manual fallback");
    }
    {
        AutoUpdateChecker checker;
        checker.configureAutomaticUpdates(true, false);
        receive(checker, release(digest));
        checker.m_PortableUpdateInstaller->busy = true;
        checker.configureAutomaticUpdates(true, true);
        checker.automaticTick();
        check(checker.m_PortableUpdateInstaller->installs == 0 && !checker.checkForUpdates(),
              "Automatic policy must not steal a manual download or overwrite its identity");
    }
    {
        AutoUpdateChecker checker;
        int checks = 0;
        QObject::connect(&checker, &AutoUpdateChecker::onUpdateCheckStarted, &checker,
                         [&] { ++checks; });
        const auto finishCheck = [&] {
            auto response = release(digest);
            response["tag_name"] = "v6.5.3.181";
            receive(checker, response);
        };
        check(checker.m_PeriodicCheck.interval() == 6 * 60 * 60 * 1000,
              "Default automatic check interval must be six hours");
        checker.configureAutomaticUpdates(true, false);
        check(checker.m_PeriodicCheck.isActive() && checker.m_IdleTimer.isActive(),
              "Enabling update checks must start both timers");
        checker.m_PeriodicCheck.stop();
        checker.m_IdleTimer.stop();
        check(checks == 1, "Enabling update checks must produce one startup check");
        finishCheck();
        for (int expected = 2; expected <= 3; ++expected) {
            deliverTimeout(checker.m_PeriodicCheck);
            check(checks == expected, "Each delivered periodic timeout must produce one check");
            finishCheck();
        }
        checker.configureAutomaticUpdates(true, false);
        check(checker.m_PeriodicCheck.isActive() && checker.m_IdleTimer.isActive(),
              "Enabled checks must rearm both real timers");
        checker.configureAutomaticUpdates(false, false);
        check(!checker.m_PeriodicCheck.isActive() && !checker.m_IdleTimer.isActive(),
              "Disabling update checks must stop both timers");
    }
    if (failures)
        return 1;
    std::puts("Automatic update policy passed");
    return 0;
}
#endif
