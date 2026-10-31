#include "streaming/video/overlaybitmaplayers.h"

#include <QCoreApplication>
#include <QImage>
#include <QtGlobal>

#include <atomic>
#include <cmath>
#include <limits>
#include <thread>
#include <vector>

using Overlay::BitmapLayer;
using Overlay::BitmapLayerStore;
using Overlay::BitmapLayerSupport;

namespace {
void require(bool condition, const char* message)
{
    if (!condition) {
        qFatal("%s", message);
    }
}

QImage solid(const QSize& size, QRgb color, QImage::Format format = QImage::Format_ARGB32)
{
    QImage image(size, format);
    image.fill(color);
    return image;
}

void testPublishAndClear()
{
    BitmapLayerStore store;
    require(store.generation() == 0, "a new store starts at generation zero");
    for (int i = 0; i < Overlay::BitmapLayerCount; i++) {
        require(!store.snapshot(BitmapLayer(i)).content, "a new store has no content");
    }

    const QRectF bounds(0.25, 0.5, 0.125, 0.0625);
    require(store.publish(BitmapLayer::Toast, solid(QSize(8, 4), qRgba(10, 20, 30, 40)), bounds),
            "a valid image must publish");
    const auto toast = store.snapshot(BitmapLayer::Toast);
    require(toast.content && toast.content->bounds == bounds &&
                toast.content->image.size() == QSize(8, 4) &&
                toast.content->image.format() == QImage::Format_ARGB32 &&
                toast.content->image.pixel(3, 2) == qRgba(10, 20, 30, 40),
            "a snapshot must return the published image and bounds");
    require(toast.generation > 0 && store.generation() == toast.generation,
            "publishing must advance the layer and store generations");
    require(!store.snapshot(BitmapLayer::StatsPanel).content,
            "publishing one layer must not touch another");

    // Republishing identical content is still a change: a renderer may have
    // dropped its texture, and generations are the only change signal
    const quint64 before = store.generation();
    require(store.publish(BitmapLayer::Toast, solid(QSize(8, 4), qRgba(10, 20, 30, 40)), bounds),
            "republishing must succeed");
    require(store.snapshot(BitmapLayer::Toast).generation > before,
            "republishing must advance the generation");

    const quint64 published = store.generation();
    store.clear(BitmapLayer::Toast);
    const auto cleared = store.snapshot(BitmapLayer::Toast);
    require(!cleared.content && cleared.generation > published,
            "clearing must remove content and advance the generation");
    const quint64 afterClear = store.generation();
    store.clear(BitmapLayer::Toast);
    require(store.generation() == afterClear, "clearing an empty layer must not churn renderers");

    store.publish(BitmapLayer::StatsPanel, solid(QSize(2, 2), qRgba(1, 2, 3, 4)), bounds);
    store.publish(BitmapLayer::MenuButton, solid(QSize(2, 2), qRgba(1, 2, 3, 4)), bounds);
    store.clearAll();
    for (int i = 0; i < Overlay::BitmapLayerCount; i++) {
        require(!store.snapshot(BitmapLayer(i)).content, "clearAll must clear every layer");
    }
}

void testStraightAlphaContract()
{
    BitmapLayerStore store;
    // Half-transparent pure red, premultiplied: stored as (128, 0, 0, 128)
    QImage premultiplied = solid(QSize(4, 4), qRgba(255, 0, 0, 128), QImage::Format_ARGB32);
    premultiplied = premultiplied.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    premultiplied.setDevicePixelRatio(1.5);
    require(store.publish(BitmapLayer::StatsPanel, premultiplied, QRectF(0, 0, 0.5, 0.5)),
            "a premultiplied image must publish");
    const auto snapshot = store.snapshot(BitmapLayer::StatsPanel);
    const QRgb pixel = snapshot.content->image.pixel(1, 1);
    require(snapshot.content->image.format() == QImage::Format_ARGB32,
            "renderers receive straight-alpha ARGB32");
    require(qAlpha(pixel) == 128 && qRed(pixel) >= 254 && qGreen(pixel) == 0 &&
                qBlue(pixel) == 0,
            "premultiplied color must be un-premultiplied once");

    // The publisher keeps using its image; the published copy must not change
    QImage reused = solid(QSize(2, 2), qRgba(0, 255, 0, 255));
    store.publish(BitmapLayer::StatsTooltip, reused, QRectF(0, 0, 0.1, 0.1));
    reused.fill(qRgba(0, 0, 255, 255));
    require(store.snapshot(BitmapLayer::StatsTooltip).content->image.pixel(0, 0) ==
                qRgba(0, 255, 0, 255),
            "a published image must be immutable");
}

void testRejectsInvalidContent()
{
    BitmapLayerStore store;
    const QRectF bounds(0, 0, 0.5, 0.5);
    store.publish(BitmapLayer::Toast, solid(QSize(2, 2), qRgba(1, 2, 3, 4)), bounds);
    const auto valid = store.snapshot(BitmapLayer::Toast);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    require(!store.publish(BitmapLayer::Toast, QImage(), bounds), "a null image must be rejected");
    require(!store.publish(BitmapLayer::Toast, solid(QSize(2, 2), 0), QRectF(0, 0, 0, 0.5)),
            "an empty rect must be rejected");
    require(!store.publish(BitmapLayer::Toast, solid(QSize(2, 2), 0), QRectF(nan, 0, 0.5, 0.5)),
            "a NaN rect must be rejected");
    require(!store.publish(BitmapLayer::Toast, solid(QSize(2, 2), 0), QRectF(0, 0, inf, 0.5)),
            "an infinite rect must be rejected");
    require(!store.publish(BitmapLayer::Toast, solid(QSize(2, 2), 0), QRectF(0, 0, -0.5, 0.5)),
            "a negative rect must be rejected");
    const auto after = store.snapshot(BitmapLayer::Toast);
    require(after.content == valid.content && after.generation == valid.generation,
            "a rejected publish must leave the previous content in place");
    require(store.publish(BitmapLayer::Toast, solid(QSize(2, 2), 0), QRectF(-0.25, 0.9, 0.5, 0.5)),
            "content partly outside the stream is valid and clipped by the renderer");
}

void testSupportIsStickyAcrossRendererGaps()
{
    BitmapLayerStore store;
    std::vector<BitmapLayerSupport> changes;
    store.setSupportChangedCallback([&](BitmapLayerSupport support) {
        // Callbacks run outside the lock; reading back must not deadlock
        require(store.support() == support, "callback must observe the new support");
        changes.push_back(support);
    });
    require(store.support() == BitmapLayerSupport::Unknown,
            "support is unknown until a renderer registers");

    store.setRendererSupport(true, true);
    require(store.support() == BitmapLayerSupport::Supported && changes.size() == 1,
            "a capable renderer must report support once");
    store.setRendererSupport(true, true);
    require(changes.size() == 1, "re-registering the same capability must not notify");

    // Reconnects and renderer recreation briefly leave no renderer. Native
    // windows must not flash up during that gap.
    store.setRendererSupport(false, false);
    require(store.support() == BitmapLayerSupport::Supported && changes.size() == 1,
            "a renderer gap must keep the last capability");

    store.setRendererSupport(true, false);
    require(store.support() == BitmapLayerSupport::Unsupported && changes.size() == 2 &&
                changes.back() == BitmapLayerSupport::Unsupported,
            "an incapable renderer must withdraw support");
    store.setRendererSupport(false, false);
    require(store.support() == BitmapLayerSupport::Unsupported && changes.size() == 2,
            "a gap after an incapable renderer keeps native presentation");
    store.setRendererSupport(true, true);
    require(store.support() == BitmapLayerSupport::Supported && changes.size() == 3,
            "a capable replacement renderer must restore support");
}

void testLateRendererSeesCurrentContent()
{
    BitmapLayerStore store;
    store.publish(BitmapLayer::StatsPanel, solid(QSize(4, 2), qRgba(9, 9, 9, 9)),
                  QRectF(0.1, 0.1, 0.2, 0.1));
    store.publish(BitmapLayer::MenuButton, solid(QSize(2, 2), qRgba(7, 7, 7, 7)),
                  QRectF(0.9, 0.9, 0.05, 0.05));
    store.clear(BitmapLayer::MenuButton);

    // A renderer created after publication (reconnect, resize recreation)
    // pulls everything it has not seen, starting from generation zero
    quint64 seen = 0;
    int drawn = 0;
    if (store.generation() != seen) {
        seen = store.generation();
        for (int i = 0; i < Overlay::BitmapLayerCount; i++) {
            drawn += store.snapshot(BitmapLayer(i)).content ? 1 : 0;
        }
    }
    require(seen != 0 && drawn == 1, "a late renderer must see exactly the live layers");
    require(store.generation() == seen, "an idle store must not ask renderers to resync");
}

void testConcurrentSnapshotsAreConsistent()
{
    BitmapLayerStore store;
    std::atomic<bool> done { false };
    std::atomic<int> torn { 0 };
    std::atomic<int> regressions { 0 };

    // The publisher encodes the same counter in the pixel and the rect; a
    // reader must never pair one publication's pixels with another's rect
    std::thread publisher([&] {
        for (int i = 1; i <= 20000; i++) {
            const int value = i % 250 + 1;
            store.publish(BitmapLayer::Toast, solid(QSize(3, 3), qRgba(value, 0, 0, 255)),
                          QRectF(value / 1000.0, 0, 0.5, 0.5));
            if (i % 7 == 0) {
                store.clear(BitmapLayer::Toast);
            }
        }
        done = true;
    });
    std::thread reader([&] {
        quint64 last = 0;
        while (!done) {
            const auto snapshot = store.snapshot(BitmapLayer::Toast);
            if (snapshot.generation < last) {
                regressions++;
            }
            last = snapshot.generation;
            if (snapshot.content) {
                const int value = qRed(snapshot.content->image.pixel(1, 1));
                if (std::lround(snapshot.content->bounds.x() * 1000) != value) {
                    torn++;
                }
            }
        }
    });
    publisher.join();
    reader.join();
    require(torn == 0, "a snapshot must pair an image with its own bounds");
    require(regressions == 0, "generations seen by a renderer must never go backwards");
}
}

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    testPublishAndClear();
    testStraightAlphaContract();
    testRejectsInvalidContent();
    testSupportIsStickyAcrossRendererGaps();
    testLateRendererSeesCurrentContent();
    testConcurrentSnapshotsAreConsistent();
    qInfo("PASS");
    return 0;
}
