// Regression test: host -> client clipboard text must reach the real system
// clipboard with the helper's threading model. Runs on the native platform
// (not offscreen: the offscreen clipboard is in-process and thread-agnostic,
// which is exactly how the Windows failure slipped through). The user's
// clipboard is saved before and restored after.
#include "clipboardhelperthreading.h"
#include "streaming/clipboardsync.h"

#include <QClipboard>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QMimeData>
#include <QTextStream>
#include <QThread>
#include <QUuid>
#include <QtEndian>

namespace {

QByteArray wire(int kind, const QByteArray& payload)
{
    QByteArray frame;
    frame.append(char(1));
    frame.append(char(kind));
    const quint32 token = 0, size = qToLittleEndian(quint32(payload.size()));
    frame.append(reinterpret_cast<const char*>(&token), 4);
    frame.append(reinterpret_cast<const char*>(&size), 4);
    return frame + payload;
}

QMimeData* copyMime(const QMimeData* source)
{
    auto* copy = new QMimeData;
    if (source != nullptr) {
        for (const QString& format : source->formats()) {
            copy->setData(format, source->data(format));
        }
    }
    return copy;
}

} // namespace

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    QTextStream out(stdout), err(stderr);
    if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
        err << "SKIP: needs the native platform plugin\n";
        return 0;
    }

    QClipboard* cb = QGuiApplication::clipboard();
    QMimeData* saved = copyMime(cb->mimeData());

    QThread worker;
    worker.start();
    auto* engine = new ClipboardSync();
    if (ClipboardHelperThreading::engineUsesWorkerThread()) {
        engine->moveToThread(&worker);
    }
    QMetaObject::invokeMethod(
        engine, [engine] { engine->start(); },
        engine->thread() == &worker ? Qt::BlockingQueuedConnection : Qt::DirectConnection);

    const QString text = QStringLiteral("moonlight-inbound-%1").arg(QUuid::createUuid().toString());
    const QByteArray frame = wire(ClipboardSync::KIND_TEXT, text.toUtf8());
    engine->handleIncomingFrame(frame.constData(), frame.size());

    QElapsedTimer timer;
    timer.start();
    while (cb->text() != text && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    const bool applied = cb->text() == text;

    QMetaObject::invokeMethod(
        engine,
        [engine] {
            engine->stop();
            delete engine;
        },
        engine->thread() == &worker ? Qt::BlockingQueuedConnection : Qt::DirectConnection);
    worker.quit();
    worker.wait();
    cb->setMimeData(saved);

    if (!applied) {
        err << "FAIL: inbound host text never reached the system clipboard (platform "
            << QGuiApplication::platformName() << ")\n";
        return 1;
    }
    out << "clipboard_engine_thread=passed\n";
    return 0;
}
