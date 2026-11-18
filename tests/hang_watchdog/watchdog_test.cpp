#include "hangwatchdog.h"
#include "windowslogappend.h"
#include <io.h>
#include <windows.h>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QWidget>
#include <QMenu>
#include <QProcess>
#include <QProcessEnvironment>
#include <SDL.h>
#include <cstdio>
#include <atomic>
#include <thread>

#ifdef MOONLIGHT_HANG_TEST_THRESHOLD_MS
constexpr int ThresholdMs = MOONLIGHT_HANG_TEST_THRESHOLD_MS;
#else
constexpr int ThresholdMs = 12000;
#endif

static int checks = 0;
static void require(bool ok, const char* message)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
    ++checks;
}

int main(int argc, char** argv)
{
    int child = HangWatchdog::childMain(argc, argv);
    if (child >= 0) return child;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    QApplication app(argc, argv);
    if (app.arguments().contains("--failed-start-parent")) {
        QString directory = app.arguments().last();
        require(!HangWatchdog::start(reinterpret_cast<const wchar_t*>(directory.utf16()), L""),
                "process creation failure reported without preventing target execution");
        return 0;
    }
    if (app.arguments().contains("--lifetime-parent") ||
            app.arguments().contains("--blocked-writer-parent")) {
        const bool blockWriter = app.arguments().contains("--blocked-writer-parent");
        QString path = app.arguments().last();
        QString directory = QFileInfo(path).absolutePath();
        require(HangWatchdog::start(reinterpret_cast<const wchar_t*>(directory.utf16()), L""),
                "start lifetime helper");
        QFile identity(path);
        require(identity.open(QIODevice::WriteOnly), "write lifetime identity");
        identity.write(QByteArray::number(HangWatchdog::helperProcessId()));
        identity.close();
        if (blockWriter) {
            HANDLE helper = OpenProcess(SYNCHRONIZE, FALSE, HangWatchdog::helperProcessId());
            HangWatchdog::qtHeartbeat();
            Sleep(ThresholdMs + 2500);
            require(WaitForSingleObject(helper, 1000) == WAIT_OBJECT_0,
                    "bounded helper exit for blocked writer while target remains alive");
            CloseHandle(helper);
            HangWatchdog::qtHeartbeat();
        }
        else Sleep(1000);
        return 0; // Deliberately omit stop(): kernel parent-handle lifetime must suffice.
    }
    QDir dir(QCoreApplication::applicationDirPath() + "/receipts");
    require(dir.mkpath("."), "create receipts directory");
    for (const QString& name : dir.entryList(QDir::Files)) dir.remove(name);
    QString log = dir.filePath("application.log");
    QFile appLog(log);
    require(appLog.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text),
            "open same QFile append mode as application logger");
    HANDLE appLogHandle = reinterpret_cast<HANDLE>(_get_osfhandle(appLog.handle()));
    require(appendWindowsLog(appLogHandle, "before snapshot\n", 16), "native app logger append before dump");
    require(HangWatchdog::start(reinterpret_cast<const wchar_t*>(dir.absolutePath().utf16()),
                               reinterpret_cast<const wchar_t*>(log.utf16())), "start watchdog");
    HANDLE helper = OpenProcess(SYNCHRONIZE, FALSE, HangWatchdog::helperProcessId());
    require(helper != nullptr, "helper is a real separate live process");
    if (ThresholdMs < 12000) {
        Sleep(ThresholdMs + 500);
        require(dir.entryList({"*.dmp"}, QDir::Files).isEmpty(), "unarmed startup never fires");
    }
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, &app, HangWatchdog::qtHeartbeat);
    heartbeat.start(50);
    QWidget window;
    window.show();
    QTimer::singleShot(100, &app, [&]() {
        // A real blocked main thread, with nested and concurrent breadcrumbs.
        std::atomic<bool> gapReady{false};
        std::atomic<bool> releaseGap{false};
        std::thread gap([&]() {
            HangWatchdog::Stage temporary("temporary other thread");
            gapReady = true;
            while (!releaseGap) Sleep(1);
        });
        while (!gapReady) Sleep(1);
        HangWatchdog::Stage outer("reset");
        releaseGap = true;
        gap.join();
        HangWatchdog::Stage inner("ResizeBuffers");
        std::atomic<DWORD> renderThread{0};
        std::atomic<bool> releaseRender{false};
        std::thread renderer([&]() {
            HangWatchdog::Stage stage("Present");
            renderThread = GetCurrentThreadId();
            while (!releaseRender) Sleep(10);
        });
        Sleep(ThresholdMs + 2500);
        require(dir.entryList({"Moonlight-hang-v1-*.dmp"}, QDir::Files).size() == 1,
                "one real dump during 12-second main-thread stall");
        auto crumbs = dir.entryList({"Moonlight-hang-v1-*.txt"}, QDir::Files);
        require(crumbs.size() == 1, "one breadcrumb per stall");
        QFile breadcrumb(dir.filePath(crumbs.first()));
        require(breadcrumb.open(QIODevice::ReadOnly), "open breadcrumb");
        QByteArray text = breadcrumb.readAll();
        require(text.contains("ResizeBuffers") && text.contains(QByteArray::number(GetCurrentThreadId())),
                "current nested stage and owner thread");
        require(text.count("thread=" + QByteArray::number(GetCurrentThreadId()) + " stage=") == 1,
                "one coherent current stage per thread even when an earlier slot becomes free");
        require(text.contains("Present") && text.contains(QByteArray::number(renderThread.load())),
                "concurrent renderer stage and distinct owner thread");
        releaseRender = true;
        renderer.join();
        QFile dump(dir.filePath(dir.entryList({"*.dmp"}, QDir::Files).first()));
        require(dump.open(QIODevice::ReadOnly) && dump.read(4) == "MDMP", "real Windows minidump magic");
        Sleep(1500);
        require(dir.entryList({"*.dmp"}, QDir::Files).size() == 1, "unchanged heartbeat never repeats");
        QFile logfile(log);
        require(logfile.open(QIODevice::ReadOnly) && logfile.readAll().contains(".dmp"),
                "dump path logged while main thread is blocked");
        require(appendWindowsLog(appLogHandle, "after snapshot\n", 15), "native app logger append after child");
        logfile.seek(0);
        QByteArray logged = logfile.readAll();
        require(logged.contains("before snapshot") && logged.contains("after snapshot") &&
                logged.count("Hang watchdog: snapshot ") == 1,
                "native parent append never overwrites child path line");
        // Qt is intentionally NOT pumped here, just like the idle streaming UI.
        {
            HangWatchdog::SdlScope streaming;
            SDL_Init(SDL_INIT_EVENTS | SDL_INIT_TIMER);
            QWidget overlay;
            QMenu menu;
            menu.addAction("diagnostic menu");
            const int iterations = ThresholdMs < 12000 ? 30 : 145;
            for (int i = 0; i < iterations; ++i) {
                HangWatchdog::sdlHeartbeat();
                SDL_PumpEvents();
                if (i == 5) { overlay.show(); menu.popup(QPoint(0, 0)); }
                if (i == 15) { menu.hide(); overlay.hide(); }
                if (i == 20) window.showMinimized();
                if (i == 25) window.showNormal();
                if (i % 20 == 0) QApplication::processEvents();
                SDL_Event event;
                SDL_WaitEventTimeout(&event, 100);
            }
            SDL_Quit();
        }
        require(dir.entryList({"*.dmp"}, QDir::Files).size() == 1, "SDL progress with gated Qt never fires");
        QTimer::singleShot(ThresholdMs + 500, &app, [&]() {
            require(dir.entryList({"*.dmp"}, QDir::Files).size() == 1, "healthy Qt dispatch never fires");
            if (ThresholdMs < 12000) {
                QFile foreign(dir.filePath("Moonlight-unrelated.dmp"));
                require(foreign.open(QIODevice::WriteOnly), "create unrelated dump fixture");
                foreign.write("preserve");
                foreign.close();
                for (int i = 0; i < 4; ++i) {
                    HangWatchdog::qtHeartbeat();
                    HangWatchdog::Stage stage("recovered reset");
                    { HangWatchdog::Stage nested("temporary nested stage"); }
                    Sleep(ThresholdMs + 1500);
                }
                require(dir.entryList({"Moonlight-hang-v1-*.dmp"}, QDir::Files).size() == 3 &&
                        dir.entryList({"Moonlight-hang-v1-*.txt"}, QDir::Files).size() == 3,
                        "progress rearms; retain exactly last three successful pairs");
                require(foreign.exists(), "scoped retention never deletes unrelated crash dumps");
                for (const QString& name : dir.entryList({"Moonlight-hang-v1-*.txt"}, QDir::Files)) {
                    QFile breadcrumb(dir.filePath(name));
                    require(breadcrumb.open(QIODevice::ReadOnly), "retained breadcrumb readable");
                    QByteArray text = breadcrumb.readAll();
                    require(text.contains("recovered reset") && !text.contains("temporary nested stage"),
                            "nested RAII scope restores outer current stage");
                    QString dumpName = name;
                    dumpName.chop(4);
                    require(dir.exists(dumpName + ".dmp"), "retained breadcrumb paired with successful dump");
                }
                require(dir.entryList({"*.partial"}, QDir::Files).isEmpty(), "no incomplete published artifacts");
            }
            HangWatchdog::stop();
            require(WaitForSingleObject(helper, 3000) == WAIT_OBJECT_0, "nonblocking stop exits helper");
            CloseHandle(helper);
            QString identity = dir.filePath("lifetime.pid");
            QProcess parent;
            parent.start(QCoreApplication::applicationFilePath(), {"--lifetime-parent", identity});
            require(parent.waitForStarted(), "launch lifetime parent with inherited safe runtime");
            for (int i = 0; i < 100 && !QFile::exists(identity); ++i) Sleep(20);
            QFile pidfile(identity);
            require(pidfile.open(QIODevice::ReadOnly), "read lifetime helper identity");
            HANDLE orphan = OpenProcess(SYNCHRONIZE, FALSE, pidfile.readAll().toULong());
            require(orphan != nullptr, "lifetime helper live while parent alive");
            require(parent.waitForFinished(5000) && parent.exitCode() == 0, "parent exits normally");
            require(WaitForSingleObject(orphan, 3000) == WAIT_OBJECT_0, "helper exits when parent exits without stop");
            CloseHandle(orphan);
            if (ThresholdMs < 12000) {
                QProcess blockedWriter;
                auto environment = QProcessEnvironment::systemEnvironment();
                environment.insert("MOONLIGHT_HANG_TEST_BLOCK_WRITER", "1");
                blockedWriter.setProcessEnvironment(environment);
                blockedWriter.start(QCoreApplication::applicationFilePath(),
                                    {"--blocked-writer-parent", dir.filePath("blocked.pid")});
                require(blockedWriter.waitForStarted() && blockedWriter.waitForFinished(10000) &&
                        blockedWriter.exitCode() == 0,
                        "wedged writer exits helper only, parent keeps executing");
                QFile fallback(dir.filePath("Moonlight-hang-watchdog.log"));
                require(fallback.open(QIODevice::ReadOnly), "blocked-writer diagnostic logged");
                const QByteArray text = fallback.readAll();
                require(text.contains("test-only blocked dump writer") && text.contains("target was not interrupted"),
                        "writer timeout diagnosed on recovery without killing the target");
                QProcess failedStart;
                environment.remove("MOONLIGHT_HANG_TEST_BLOCK_WRITER");
                environment.insert("MOONLIGHT_HANG_TEST_FAIL_START", "1");
                failedStart.setProcessEnvironment(environment);
                failedStart.start(QCoreApplication::applicationFilePath(),
                                  {"--failed-start-parent", dir.absolutePath()});
                require(failedStart.waitForStarted() && failedStart.waitForFinished(5000) &&
                        failedStart.exitCode() == 0, "missing helper fails open");
                fallback.seek(0);
                require(fallback.readAll().contains("start hidden helper failed, Win32 error 2"),
                        "missing helper process start error is logged");
            }
            std::printf("PASS: %d parent checks, threshold=%dms; real stall, nested/concurrent stages, MDMP, one-shot, log, SDL/Qt UI, shutdown/lifetime%s\n",
                        checks, ThresholdMs, ThresholdMs < 12000 ? ", retention, nested restore, stuck writer, start failure" : "");
            app.quit();
        });
    });
    return app.exec();
}
