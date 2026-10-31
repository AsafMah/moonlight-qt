#include "settings/statsoverlaysettings.h"
#include "streaming/video/overlaystatswindow.h"
#include "streaming/video/overlaystatscontroller.h"
#include "streaming/video/overlaymanager.h"
#include "streaming/stats/streamstatsmodel.h"

#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QFontDatabase>
#include <QImage>
#include <QGuiApplication>
#include <QScreen>
#include <QMouseEvent>
#include <QPaintEngine>
#include <QPainter>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTranslator>
#include <QProcess>

#include <cmath>
#include <set>

using namespace StatsOverlay;

extern "C" uint64_t LiGetMicroseconds();

namespace {

bool require(bool condition, const QString& message, QTextStream& err)
{
    if (!condition) {
        err << "FAIL: " << message << '\n';
    }

    return condition;
}

double luminance(const QColor& color)
{
    auto linear = [](double value) {
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) +
           0.0722 * linear(color.blueF());
}

struct PaintedText
{
    QString text;
    qreal pixels;
    QRectF bounds;
    QColor color;
    QFont font;
    QPointF baseline;
    QTransform transform;
};

class TextRecorder : public QPaintEngine
{
public:
    TextRecorder() : QPaintEngine(AllFeatures) {}
    bool begin(QPaintDevice*) override { return true; }
    bool end() override { return true; }
    Type type() const override { return User; }
    void updateState(const QPaintEngineState&) override {}
    void drawPixmap(const QRectF&, const QPixmap&, const QRectF&) override {}
    void drawImage(const QRectF&, const QImage& image, const QRectF&,
                   Qt::ImageConversionFlags) override
    {
        shadows.append(image);
    }
    void drawRects(const QRect*, int) override {}
    void drawRects(const QRectF*, int) override {}
    void drawLines(const QLineF*, int) override {}
    void drawPolygon(const QPointF*, int, PolygonDrawMode) override {}
    void drawTextItem(const QPointF& point, const QTextItem& item) override
    {
        const QTransform transform = state->transform();
        const QRectF bounds = transform.mapRect(QRectF(
            point.x(), point.y() - item.ascent(), item.width(), item.ascent() + item.descent()));
        // Qt splits a single label into runs when CJK needs a fallback font.
        if (!text.isEmpty() && text.last().baseline.y() == point.y() &&
            text.last().color == state->pen().color() &&
            std::abs(text.last().bounds.right() - bounds.left()) < 0.1) {
            text.last().text += item.text();
            text.last().bounds = text.last().bounds.united(bounds);
            return;
        }
        text.append({ item.text(), item.font().pixelSize() * transform.m11(), bounds,
                      state->pen().color(), item.font(), point, transform });
    }

    QVector<PaintedText> text;
    QVector<QImage> shadows;
};

class TextDevice : public QPaintDevice
{
public:
    explicit TextDevice(const QSize& size, qreal dpr = 1) : m_Size(size * dpr), m_Dpr(dpr) {}
    QPaintEngine* paintEngine() const override { return &recorder; }
    mutable TextRecorder recorder;

protected:
    int metric(PaintDeviceMetric metric) const override
    {
        switch (metric) {
        case PdmWidth:
            return m_Size.width();
        case PdmHeight:
            return m_Size.height();
        case PdmDpiX:
        case PdmDpiY:
        case PdmPhysicalDpiX:
        case PdmPhysicalDpiY:
            return 96;
        case PdmDepth:
            return 32;
        case PdmDevicePixelRatio:
            return qRound(m_Dpr);
        case PdmDevicePixelRatioScaled:
            return qRound(m_Dpr * devicePixelRatioFScale());
        default:
            return 0;
        }
    }

private:
    QSize m_Size;
    qreal m_Dpr;
};

class RenderWindow : public OverlayStatsWindow
{
public:
    using OverlayStatsWindow::paintOverlay;

    QImage render(qreal dpr = 1)
    {
        resize(preferredSize());
        QImage image(size() * dpr, QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        paintOverlay(painter);
        return image;
    }

    QVector<PaintedText> recordedText(QVector<QImage>* shadows = nullptr, qreal dpr = 1)
    {
        resize(preferredSize());
        TextDevice device(size(), dpr);
        QPainter painter(&device);
        paintOverlay(painter);
        painter.end();
        if (shadows != nullptr) {
            *shadows = device.recorder.shadows;
        }
        return device.recorder.text;
    }
};

StatsSnapshot sample()
{
    StatsSnapshot s;
    s.valid = true;
    s.width = 3840;
    s.height = 2160;
    s.targetFps = 60;
    s.totalFps = 60;
    s.receivedFps = 60;
    s.decodedFps = 60;
    s.renderedFps = 59.9;
    s.codec = QStringLiteral("HEVC");
    s.decoder = QStringLiteral("hevc (d3d11va)");
    s.renderer = QStringLiteral("D3D11");
    s.frameTimes.samples = 120;
    s.frameTimes.onePercentLowFps = 58.7;
    s.frameTimes.pointOnePercentLowFps = 54.4;
    s.bitrateMbps = 37.6;
    s.hasRtt = true;
    s.rttMs = 1;
    s.rttVarianceMs = 1;
    s.jitterMs = 1.34;
    s.latency.hasHost = true;
    s.latency.hasNetwork = true;
    s.latency.hasQueue = true;
    s.latency.hostMs = 5.2;
    s.latency.networkMs = 0.5;
    s.latency.decodeMs = 0.8;
    s.latency.queueMs = 0.4;
    s.latency.renderMs = 1.1;
    s.presentTiming = { true, 60, 0.4, 0.7 };
    s.frameTimeHistoryMs = { 16.7f, 16.6f, 16.8f, 17.1f, 16.5f };
    return s;
}

bool testDefaultsAndPersistence(QTextStream& err)
{
    QTemporaryDir directory;
    if (!require(directory.isValid(), QStringLiteral("temporary settings directory"), err)) {
        return false;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    QCoreApplication::setOrganizationName(QStringLiteral("MoonlightTests"));
    QCoreApplication::setApplicationName(QStringLiteral("PerfBarDefaults"));
    QSettings settings;
    settings.setFallbacksEnabled(false);
    settings.clear();
    StatsOverlaySettings overlay;
    bool ok = require(overlay.scalePercent() == 80,
                      QStringLiteral("fresh overlay must default to the old 80% footprint"), err);
    ok &= require(overlay.opacityPercent() == 72,
                  QStringLiteral("fresh overlay must default to the approved 72% backing"), err);
    ok &= require(settings.allKeys().isEmpty(),
                  QStringLiteral("reading defaults must not write or migrate settings"), err);

    settings.setValue(QStringLiteral("statsoverlay/scale"), 125);
    settings.setValue(QStringLiteral("statsoverlay/opacity"), 50);
    overlay.setDisplayAnchor(QStringLiteral("test"), Anchor::BottomRight);
    overlay.setDisplaySize(QStringLiteral("test"), Preset::Compact, QSize(910, 140));
    overlay.setDetachedPosition(QStringLiteral("test"), QPoint(-500, 300));
    const QStringList keys = settings.allKeys();
    overlay.reload();
    ok &= require(overlay.scalePercent() == 125 && overlay.opacityPercent() == 50 &&
                      overlay.displayState(QStringLiteral("test")).anchor == Anchor::BottomRight &&
                      overlay.displaySize(QStringLiteral("test"), Preset::Compact) ==
                          QSize(910, 140) &&
                      overlay.detachedPosition(QStringLiteral("test")) == QPoint(-500, 300) &&
                      settings.allKeys() == keys,
                  QStringLiteral("new defaults must preserve explicit custom settings"), err);
    settings.setValue(QStringLiteral("statsoverlay/scale"), 100);
    settings.setValue(QStringLiteral("statsoverlay/opacity"), 85);
    ok &=
        require(overlay.scalePercent() == 100 && overlay.opacityPercent() == 85,
                QStringLiteral("explicit old defaults must not be mistaken for unset values"), err);
    ok &= require(!overlay.attachedPosition(QStringLiteral("test")),
                  QStringLiteral("legacy anchors must not be migrated to free positions"), err);
    overlay.setAttachedPosition(QStringLiteral("test"), QPoint(237, 191));
    overlay.setDisplayPreset(QStringLiteral("test"), Preset::Detailed);
    overlay.setDisplaySize(QStringLiteral("test"), Preset::Detailed, QSize(500, 300));
    StatsOverlaySettings reopened;
    ok &=
        require(reopened.attachedPosition(QStringLiteral("test")) == QPoint(237, 191) &&
                    !reopened.attachedPosition(QStringLiteral("other")) &&
                    reopened.detachedPosition(QStringLiteral("test")) == QPoint(-500, 300),
                QStringLiteral("exact free placement must survive size/preset changes and reopen, "
                               "without affecting another display or detached placement"),
                err);
    overlay.setDisplayAnchor(QStringLiteral("test"), Anchor::TopRight);
    ok &= require(!overlay.attachedPosition(QStringLiteral("test")),
                  QStringLiteral("an explicit anchor choice must replace free placement"), err);
    overlay.setAttachedPosition(QStringLiteral("test"), QPoint(0, 0));
    ok &= require(reopened.attachedPosition(QStringLiteral("test")) == QPoint(0, 0),
                  QStringLiteral("zero-origin placement must not be confused with unset"), err);
    overlay.resetPlacements();
    ok &= require(!reopened.attachedPosition(QStringLiteral("test")) &&
                      !reopened.detachedPosition(QStringLiteral("test")),
                  QStringLiteral("reset placements must clear free and detached positions"), err);
    settings.clear();
    return ok;
}

bool testRenderedBar(QTextStream& err)
{
    bool ok = true;
    const QString output = qEnvironmentVariable("MOONLIGHT_OVERLAY_TEST_OUTPUT_DIR");
    if (!output.isEmpty()) {
        ok &= require(QDir().mkpath(output), QStringLiteral("cannot create render output"), err);
    }
    RenderWindow window;
    window.setStats(sample(), {});
    window.setPreset(Preset::Compact);
    for (int percent : { DefaultScalePercent, 75, 100, 150, 80 }) {
        window.setScalePercent(percent);
        const QImage image = window.render();
        if (percent == DefaultScalePercent) {
            ok &= require(image.height() <= 30,
                          QStringLiteral("default Compact must remain one row, not %1px tall")
                              .arg(image.height()),
                          err);
            ok &= require(std::abs(image.pixelColor(20, 2).alpha() - 184) <= 1,
                          QStringLiteral("fresh backing defaults to 72%"), err);
            const QColor background = image.pixelColor(image.width() - 2, image.height() - 2);
            ok &= require(background.red() == 0 && background.green() == 0 &&
                              background.blue() == 0 && std::abs(background.alpha() - 184) <= 1,
                          QStringLiteral("strip has continuous black backing without chip gaps"),
                          err);
        }
        bool labelFound = false;
        bool valueFound = false;
        int labelHaloPasses = 0;
        bool opaqueLabel = false;
        QStringList labels{ QStringLiteral("RES"),  QStringLiteral("FPS"),  QStringLiteral("↓"),
                            QStringLiteral("LAT"),  QStringLiteral("RATE"), QStringLiteral("NET"),
                            QStringLiteral("Jit"),  QStringLiteral("Loss"), QStringLiteral("h"),
                            QStringLiteral("d"),    QStringLiteral("q"),    QStringLiteral("r"),
                            QStringLiteral("Drops") };
        QVector<QImage> shadows;
        for (const PaintedText& text : window.recordedText(&shadows)) {
            for (int i = labels.size() - 1; i >= 0; --i) {
                if (labels[i].compare(text.text, Qt::CaseInsensitive) == 0) {
                    labels.removeAt(i);
                }
            }
            if (text.text.compare(QLatin1String("FPS"), Qt::CaseInsensitive) == 0) {
                labelFound = true;
                if (text.color.red() == 0 && text.color.green() == 0 && text.color.blue() == 0 &&
                    text.color.alpha() >= 200) {
                    labelHaloPasses++;
                } else {
                    opaqueLabel = text.color.alpha() == 255;
                }
                ok &= require(
                    text.pixels >= 13.5 && text.font.stretch() == QFont::Unstretched,
                    QStringLiteral("%1% label shrinks to %2 px").arg(percent).arg(text.pixels),
                    err);
            }
            if (text.text == QLatin1String("60/60/60")) {
                valueFound = true;
                ok &= require(
                    text.pixels >= 13.5 && text.font.weight() == QFont::DemiBold,
                    QStringLiteral("%1% value shrinks to %2 px").arg(percent).arg(text.pixels),
                    err);
                ok &= require(text.color == QColor("#e6e6e6"),
                              QStringLiteral("strip values use neutral light semibold type"), err);
            }
            ok &= require(
                QRectF(QPointF(), image.size()).adjusted(-1, -1, 1, 1).contains(text.bounds),
                QStringLiteral("%1% painted text '%2' overflows").arg(percent).arg(text.text), err);
        }
        ok &= require(
            labelFound && valueFound,
            QStringLiteral("%1% sample must paint its real FPS label and value").arg(percent), err);
        ok &= require(labelHaloPasses == 0 && opaqueLabel,
                      QStringLiteral("%1% text must be opaque without the rejected hard outline")
                          .arg(percent),
                      err);
        ok &= require(shadows.isEmpty(),
                      QStringLiteral("plain strip must not retain chip text shadows"), err);
        ok &= require(labels.isEmpty(),
                      QStringLiteral("%1% hides Compact labels: %2")
                          .arg(percent)
                          .arg(labels.join(QStringLiteral(", "))),
                      err);
        if (!output.isEmpty()) {
            ok &= require(
                image.save(QDir(output).filePath(QStringLiteral("compact-%1.png").arg(percent))),
                QStringLiteral("cannot save rendered bar"), err);
        }
    }
    window.setOpacityPercent(30);
    const QImage transparent = window.render();
    window.setOpacityPercent(85);
    const QImage opaque = window.render();
    ok &= require(
        std::abs(transparent.pixelColor(20, 2).alpha() - 77) <= 1 &&
            std::abs(opaque.pixelColor(20, 2).alpha() - 217) <= 1 &&
            std::abs(transparent.pixelColor(transparent.width() - 2, 2).alpha() - 77) <= 1 &&
            std::abs(opaque.pixelColor(opaque.width() - 2, 2).alpha() - 217) <= 1,
        QStringLiteral("opacity must control the whole backing, not foreground text"), err);
    for (const PaintedText& text : window.recordedText()) {
        ok &= require(text.color.alpha() == 255,
                      QStringLiteral("user opacity must not fade the foreground text"), err);
    }
    return ok;
}

bool testRenderedPresetsAndDpi(QTextStream& err)
{
    bool ok = true;
    const QString output = qEnvironmentVariable("MOONLIGHT_OVERLAY_TEST_OUTPUT_DIR");
    RenderWindow window;
    window.setStats(sample(), {});
    for (Preset preset : { Preset::Compact, Preset::Detailed, Preset::Custom }) {
        window.setPreset(preset);
        for (int percent : { 75, 80, 100 }) {
            window.setScalePercent(percent);
            const QSize logicalSize = window.preferredSize();
            const auto text = window.recordedText();
            ok &=
                require(text.size() > 10, QStringLiteral("panel must paint its real metrics"), err);
            for (const PaintedText& item : text) {
                ok &= require(
                    QRectF(QPointF(), logicalSize).adjusted(-1, -1, 1, 1).contains(item.bounds),
                    QStringLiteral("preset %1 at %2% overflows text '%3'")
                        .arg(int(preset))
                        .arg(percent)
                        .arg(item.text),
                    err);
            }
            for (qreal dpr : { 1.0, 1.5, 2.0 }) {
                const QImage image = window.render(dpr);
                ok &= require(image.size() == logicalSize * dpr &&
                                  image.pixelColor(qRound(20 * dpr), 0).alpha() > 0,
                              QStringLiteral("DPI rendering must retain size and flush-top body"),
                              err);
                if (!output.isEmpty()) {
                    ok &= require(image.save(QDir(output).filePath(
                                      QStringLiteral("preset-%1-scale-%2-dpr-%3.png")
                                          .arg(int(preset))
                                          .arg(percent)
                                          .arg(dpr))),
                                  QStringLiteral("cannot save rendered DPI panel"), err);
                }
            }
        }
    }
    return ok;
}

bool testSceneComposites(QTextStream& err)
{
    const QString output = qEnvironmentVariable("MOONLIGHT_OVERLAY_TEST_OUTPUT_DIR");
    QImage scenes(1284, 10 * 64, QImage::Format_RGB32);
    QPainter painter(&scenes);
    RenderWindow window;
    window.setStats(sample(), {});
    window.setPreset(Preset::Compact);
    int row = 0;
    for (int percent : { DefaultScalePercent, 75 }) {
        window.setScalePercent(percent);
        for (int scene = 0; scene < 5; scene++, row++) {
            const QRect stripe(0, row * 64, scenes.width(), 64);
            painter.fillRect(stripe, scene == 0 ? Qt::white : Qt::black);
            if (scene == 2) {
                for (int x = 0; x < scenes.width(); x += 8) {
                    for (int y = 0; y < 64; y += 8) {
                        painter.fillRect(x, stripe.y() + y, 8, 8,
                                         QColor::fromHsv((x * 7 + y * 11) % 360, 180,
                                                         ((x + y) / 8 % 2) ? 250 : 70));
                    }
                }
            }
            if (scene == 3) {
                painter.fillRect(stripe, QColor(128, 128, 128));
            } else if (scene == 4) {
                painter.fillRect(stripe, QColor(0x22, 0xCD, 0xBC));
            }
            painter.drawImage(stripe.topLeft(), window.render());
        }
    }
    painter.end();
    return output.isEmpty() ||
           require(scenes.save(QDir(output).filePath(QStringLiteral("scene-composites.png"))),
                   QStringLiteral("cannot save bright/dark/busy scene composites"), err);
}

QImage softTextShadow(const QVector<PaintedText>& text, const QSize& size)
{
    QImage mask(size, QImage::Format_ARGB32_Premultiplied);
    mask.fill(Qt::transparent);
    {
        QPainter painter(&mask);
        painter.setRenderHint(QPainter::TextAntialiasing);
        for (const PaintedText& item : text) {
            painter.setWorldTransform(item.transform);
            painter.setFont(item.font);
            painter.setPen(QColor(0, 0, 0, 153));
            painter.drawText(item.baseline, item.text);
        }
    }
    QImage blurred(size, QImage::Format_ARGB32_Premultiplied);
    blurred.fill(Qt::transparent);
    const int kernel[] = { 1, 2, 3, 4, 3, 2, 1 };
    for (int y = 0; y < size.height(); y++) {
        for (int x = 0; x < size.width(); x++) {
            int alpha = 0;
            for (int dy = -3; dy <= 3; dy++) {
                for (int dx = -3; dx <= 3; dx++) {
                    if (mask.rect().contains(x + dx, y + dy)) {
                        alpha +=
                            qAlpha(mask.pixel(x + dx, y + dy)) * kernel[dx + 3] * kernel[dy + 3];
                    }
                }
            }
            blurred.setPixel(x, y, qRgba(0, 0, 0, alpha / 256));
        }
    }
    return blurred;
}

[[maybe_unused]] bool testAppearanceCandidates(QTextStream& err)
{
    const QString output = qEnvironmentVariable("MOONLIGHT_OVERLAY_TEST_OUTPUT_DIR");
    if (output.isEmpty()) {
        return true;
    }
    RenderWindow window;
    window.setStats(sample(), {});
    window.setPreset(Preset::Compact);
    window.setScalePercent(80);
    const QImage original = window.render();
    const QVector<PaintedText> text = window.recordedText();
    const QImage shadow = softTextShadow(text, original.size());
    QImage variants(original.width() * 3 + 32, 5 * 64 + 56, QImage::Format_RGB32);
    variants.fill(QColor(24, 26, 30));
    QPainter painter(&variants);
    const QStringList names{ QStringLiteral("A: Soft shadow, 30% bar"),
                             QStringLiteral("B: Soft shadow, 50% bar, muted light-grey labels"),
                             QStringLiteral(
                                 "C: Separate 55% dark chips, transparent gaps, soft shadow") };
    for (int candidate = 0; candidate < 3; candidate++) {
        const int left = candidate * (original.width() + 16);
        painter.setPen(Qt::white);
        QFont heading;
        heading.setPixelSize(18);
        painter.setFont(heading);
        painter.drawText(left + 8, 28, names[candidate]);
        QImage overlay(original.size(), QImage::Format_ARGB32_Premultiplied);
        overlay.fill(Qt::transparent);
        {
            QPainter p(&overlay);
            if (candidate < 2) {
                p.fillRect(overlay.rect().adjusted(0, 0, -4, -4),
                           QColor(23, 26, 32, candidate == 0 ? 77 : 128));
            } else {
                p.setRenderHint(QPainter::Antialiasing);
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(12, 14, 18, 140));
                for (int i = 0; i + 1 < text.size(); i += 2) {
                    p.drawRoundedRect(
                        text[i].bounds.united(text[i + 1].bounds).adjusted(-3, -2, 3, 2), 3, 3);
                }
            }
            p.drawImage(QPoint(0, 1), shadow);
            p.setRenderHint(QPainter::TextAntialiasing);
            for (int i = 0; i < text.size(); i++) {
                const PaintedText& item = text[i];
                p.setWorldTransform(item.transform);
                p.setFont(item.font);
                p.setPen(candidate == 1 && i % 2 == 0 ? QColor(191, 191, 191) : item.color);
                p.drawText(item.baseline, item.text);
            }
        }
        for (int scene = 0; scene < 5; scene++) {
            const QRect stripe(left, 56 + scene * 64, original.width(), 48);
            const QColor colors[] = { Qt::white, Qt::black, Qt::black, QColor(128, 128, 128),
                                      QColor(0x22, 0xCD, 0xBC) };
            painter.fillRect(stripe, colors[scene]);
            if (scene == 2) {
                for (int x = 0; x < original.width(); x += 8) {
                    for (int y = 0; y < stripe.height(); y += 8) {
                        painter.fillRect(left + x, stripe.y() + y, 8, 8,
                                         QColor::fromHsv((x * 7 + y * 11) % 360, 180,
                                                         ((x + y) / 8 % 2) ? 250 : 70));
                    }
                }
            }
            painter.drawImage(stripe.topLeft(), overlay);
        }
    }
    painter.end();
    return require(
        variants.save(QDir(output).filePath(QStringLiteral("appearance-candidates.png"))),
        QStringLiteral("cannot save appearance candidates"), err);
}

bool testFlushTop(QTextStream& err)
{
    bool ok = true;
    const QSize size(1284, 36);
    for (const QRect& parent :
         { QRect(0, 0, 1920, 1080), QRect(-1920, -1080, 1920, 1080), QRect(300, 180, 1600, 900) }) {
        for (Anchor anchor : { Anchor::TopLeft, Anchor::Top, Anchor::TopRight }) {
            const QPoint position = anchoredPosition(anchor, parent, size, 16);
            ok &= require(
                position.y() == parent.y(),
                QStringLiteral("top anchor leaves a %1 px gap").arg(position.y() - parent.y()),
                err);
            RenderWindow window;
            window.setStats(sample(), {});
            window.setPosition(position);
            const QImage image = window.render();
            ok &= require(image.pixelColor(20, 0).alpha() > 0,
                          QStringLiteral("overlay has transparent padding above its painted body"),
                          err);
            window.setEditing(true);
            window.render();
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(50, 12),
                              QPointF(position + QPoint(50, 12)), Qt::LeftButton, Qt::LeftButton,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(&window, &press);
            ok &= require(window.isDragging(),
                          QStringLiteral("edge-flush bar is not draggable in edit mode"), err);
        }
    }
    ok &= require(anchoredPosition(Anchor::BottomRight, QRect(0, 0, 1920, 1080), size, 16) ==
                      QPoint(1920 - size.width() - 16, 1080 - size.height() - 16),
                  QStringLiteral("non-top anchors must retain their existing margin"), err);
    return ok;
}

bool testResizeGrabZones(QTextStream& err)
{
    bool ok = true;
    for (int zone = 0; zone < 3; zone++) {
        RenderWindow window;
        window.setStats(sample(), {});
        window.setPreset(Preset::Compact);
        window.render();
        const int bodyHeight = window.height() - 4;
        window.setEditing(true);
        window.render();
        bool resized = false;
        bool moved = false;
        window.setResizeFinishedCallback([&](const QPoint&, const QSize&) { resized = true; });
        window.setDragFinishedCallback([&](const QPoint&) { moved = true; });
        const QPoint point = zone == 0   ? QPoint(window.width() / 2, window.height() - 8)
                             : zone == 1 ? QPoint(10, bodyHeight / 2)
                                         : QPoint(window.width() / 2, bodyHeight / 2);
        const QPoint global = window.position() + point;
        QMouseEvent hover(QEvent::MouseMove, QPointF(point), QPointF(global), Qt::NoButton,
                          Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&window, &hover);
        const Qt::CursorShape expected = zone == 0   ? Qt::SizeVerCursor
                                         : zone == 1 ? Qt::SizeHorCursor
                                                     : Qt::SizeAllCursor;
        ok &= require(
            window.cursor().shape() == expected,
            QStringLiteral("resize zone %1 lacks the correct resize/move cursor").arg(zone), err);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(point), QPointF(global), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&window, &press);
        const QPoint delta(20, 20);
        QMouseEvent move(QEvent::MouseMove, QPointF(point + delta), QPointF(global + delta),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&window, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point + delta),
                            QPointF(global + delta), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&window, &release);
        ok &= require(zone == 2 ? moved && !resized : resized && !moved,
                      QStringLiteral("zone %1 must %2")
                          .arg(zone)
                          .arg(zone == 2 ? QStringLiteral("move from the center")
                                         : QStringLiteral("resize over edge/footer text")),
                      err);
    }
    return ok;
}

bool testConditionalClickThrough(QTextStream& err)
{
    RenderWindow window;
    window.setStats(sample(), {});
    window.render();
    bool ok = require(window.flags().testFlag(Qt::WindowTransparentForInput),
                      QStringLiteral("normal streaming stats must be click-through"), err);
    window.setBeginEditingCallback([&] { window.setEditing(true); });
    window.setEditOnClick(true);
    ok &= require(!window.flags().testFlag(Qt::WindowTransparentForInput) && !window.isEditing(),
                  QStringLiteral("persistent release must allow selecting, not auto-start editing"),
                  err);
    const QPoint point(window.width() / 2, 14);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(point), QPointF(point), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point), QPointF(point), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &release);
    ok &= require(window.isEditing(),
                  QStringLiteral("selecting stats must start existing edit mode"), err);
    window.setEditing(false);
    ok &= require(!window.flags().testFlag(Qt::WindowTransparentForInput),
                  QStringLiteral("finishing one edit must not end persistent selection mode"), err);
    window.setEditOnClick(false);
    ok &= require(window.flags().testFlag(Qt::WindowTransparentForInput),
                  QStringLiteral("exiting persistent release must restore click-through"), err);
    return ok;
}

bool testLightSnapping(QTextStream& err)
{
    bool ok = true;
    const QSize size(600, 60);
    for (const QRect& bounds : { QRect(0, 0, 1920, 1080), QRect(-1920, -500, 1920, 1080) }) {
        const QPoint free = bounds.topLeft() + QPoint(237, 191);
        ok &= require(lightlySnappedPosition(free, size, bounds) == free,
                      QStringLiteral("an arbitrary drop must not jump to a nine-grid anchor"), err);
        const QPoint near = bounds.topLeft() + QPoint(3, 191);
        ok &=
            require(lightlySnappedPosition(near, size, bounds) == bounds.topLeft() + QPoint(0, 191),
                    QStringLiteral("edge magnet must only affect the nearby axis"), err);
        const QPoint pulledAway = bounds.topLeft() + QPoint(5, 191);
        ok &= require(lightlySnappedPosition(pulledAway, size, bounds) == pulledAway,
                      QStringLiteral("edge magnet must release beyond four pixels"), err);
    }
    return ok;
}

bool testLiveFreeDrag(QTextStream& err)
{
    RenderWindow window;
    window.setStats(sample(), {});
    window.setPreset(Preset::Compact);
    window.setEditing(true);
    window.render();
    const QRect bounds(-1920, -500, 1920, 1080);
    window.setMovementBounds(bounds);
    const QPoint start = bounds.topLeft() + QPoint(100, 50);
    const QPoint local(window.width() / 2, 14);
    bool ok = true;
    for (const QPoint& offset : { QPoint(237, 191), QPoint(3, 191), QPoint(5, 191) }) {
        window.setPosition(start);
        const QPoint global = start + local;
        const QPoint delta = bounds.topLeft() + offset - start;
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(local), QPointF(global), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&window, &press);
        QMouseEvent move(QEvent::MouseMove, QPointF(local + delta), QPointF(global + delta),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&window, &move);
        const QPoint expected =
            bounds.topLeft() + QPoint(offset.x() == 3 ? 0 : offset.x(), offset.y());
        ok &= require(window.position() == expected,
                      QStringLiteral("live drag must only magnetize within four pixels"), err);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(local + delta),
                            QPointF(global + delta), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&window, &release);
        ok &= require(window.position() == expected,
                      QStringLiteral("dropping a free drag must not snap back to an anchor"), err);
    }
    return ok;
}

bool testMoveControlsVisible(QTextStream& err)
{
    bool ok = true;
    for (const int scale : { 80, 100, 150 }) {
        RenderWindow window;
        window.setStats(sample(), {});
        window.setPreset(Preset::Compact);
        window.setScalePercent(scale);
        window.setBodySize(QSize(320, 80));
        window.setEditing(true);
        const auto text = window.recordedText();
        const auto tr = [](const char* text) {
            return QCoreApplication::translate("OverlayStatsWindow", text);
        };
        QVector<QRectF> bounds;
        for (const QString& label : { tr("Detach"), tr("Details: %1").arg(tr("Compact")),
                                      tr("Custom: Off"), tr("Opacity -"), tr("Opacity +"),
                                      tr("Size -"), tr("Size +"), tr("Reset"), tr("Done") }) {
            const auto found = std::find_if(text.begin(), text.end(), [&](const PaintedText& item) {
                return item.text == label;
            });
            ok &= require(found != text.end(),
                          QStringLiteral("move mode is missing visible control '%1'").arg(label),
                          err);
            if (found != text.end()) {
                ok &= require(QRectF(QPointF(), window.size()).contains(found->bounds),
                              QStringLiteral("move-mode control '%1' is clipped at %2%")
                                  .arg(label)
                                  .arg(scale),
                              err);
                for (const auto& other : bounds) {
                    ok &= require(!other.intersects(found->bounds),
                                  QStringLiteral("move-mode control labels overlap"), err);
                }
                bounds.append(found->bounds);
            }
        }
        const QString output = qEnvironmentVariable("MOONLIGHT_OVERLAY_TEST_OUTPUT_DIR");
        if (!output.isEmpty()) {
            const QString locale =
                tr("Detach") == QLatin1String("Detach") ? QStringLiteral("en") : tr("Compact");
            ok &= require(window.render().save(QDir(output).filePath(
                              QStringLiteral("edit-controls-%1-%2.png").arg(locale).arg(scale))),
                          QStringLiteral("save edit-controls image"), err);
        }
    }
    return ok;
}

bool clickControl(RenderWindow& window, const QString& label, QTextStream& err,
                  QEvent::Type pressType = QEvent::MouseButtonPress)
{
    const auto text = window.recordedText();
    const auto found = std::find_if(text.begin(), text.end(),
                                    [&](const PaintedText& item) { return item.text == label; });
    if (!require(found != text.end(), QStringLiteral("missing control '%1'").arg(label), err)) {
        return false;
    }
    const QPointF local = found->bounds.center();
    const QPointF global = window.position() + local;
    QMouseEvent press(pressType, local, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &press);
    bool ok = require(press.isAccepted() && !window.isDragging(),
                      QStringLiteral("control '%1' started a drag or resize").arg(label), err);
    QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &release);
    ok &= require(release.isAccepted(), QStringLiteral("control release escaped to stream"), err);
    return ok;
}

// A physical double-click arrives as press, release, double-click, release
bool doubleClickControl(RenderWindow& window, const QString& label, QTextStream& err)
{
    const auto text = window.recordedText();
    const auto found = std::find_if(text.begin(), text.end(),
                                    [&](const PaintedText& item) { return item.text == label; });
    if (!require(found != text.end(), QStringLiteral("missing control '%1'").arg(label), err)) {
        return false;
    }
    const QPointF local = found->bounds.center();
    bool ok = true;
    for (const QEvent::Type type : { QEvent::MouseButtonPress, QEvent::MouseButtonRelease,
                                     QEvent::MouseButtonDblClick, QEvent::MouseButtonRelease }) {
        const bool release = type == QEvent::MouseButtonRelease;
        QMouseEvent event(type, local, window.position() + local, Qt::LeftButton,
                          release ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&window, &event);
        ok &= require(event.isAccepted() && !window.isDragging(),
                      QStringLiteral("double-click on '%1' escaped or dragged").arg(label), err);
    }
    return ok;
}

bool testMoveControlLifecycle(QTextStream& err)
{
    QTemporaryDir directory;
    if (!require(directory.isValid(), QStringLiteral("temporary edit settings"), err)) {
        return false;
    }
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    QCoreApplication::setApplicationName(QStringLiteral("OverlayMoveControls"));
    Overlay::OverlayManager manager;
    StreamStatsModel model;
    auto window = std::make_unique<RenderWindow>();
    auto* view = window.get();
    OverlayStatsController controller(manager, model, nullptr, OverlayWindowMode::Detached, true,
                                      std::move(window));
    const QRect stream(0, 0, 1280, 720);
    const QString key = displayKey(QGuiApplication::primaryScreen()->name());
    StatsOverlaySettings settings;
    settings.setAttachedPosition(key, QPoint(91, 73));
    settings.setDetachedPosition(key, QPoint(350, 220));
    settings.setDisplaySize(key, Preset::Compact, QSize(410, 90));
    controller.setParentGeometryProvider([&] { return stream; });
    controller.setPreset(Preset::Compact);
    int editStarts = 0;
    int editEnds = 0;
    bool captured = true;
    controller.setEditModeCallback([&](bool editing) {
        editing ? ++editStarts : ++editEnds;
        captured = !editing;
    });
    controller.beginEditing();
    QWindow* focusBefore = QGuiApplication::focusWindow();
    bool ok = require(controller.isEditing() && !captured,
                      QStringLiteral("entering edit mode must release pointer"), err);

    ok &= clickControl(*view, QStringLiteral("Detach"), err);
    ok &=
        require(controller.isDetached() && controller.isEditing() && !captured && editStarts == 1 &&
                    editEnds == 0 && QGuiApplication::focusWindow() == focusBefore &&
                    view->flags().testFlag(Qt::WindowDoesNotAcceptFocus),
                QStringLiteral("detach must preserve editing, focus and capture ownership"), err);
    ok &= clickControl(*view, QStringLiteral("Re-attach"), err);
    ok &= require(!controller.isDetached() && controller.isEditing() && !captured &&
                      settings.attachedPosition(key) == QPoint(91, 73) &&
                      settings.detachedPosition(key) == QPoint(350, 220) &&
                      settings.displaySize(key, Preset::Compact) == QSize(410, 90),
                  QStringLiteral("re-attach must preserve both placement stores"), err);
    {
        const auto text = view->recordedText();
        const auto detach = std::find_if(text.begin(), text.end(), [](const PaintedText& item) {
            return item.text == QLatin1String("Detach");
        });
        if (!require(detach != text.end(), QStringLiteral("detach control after round trip"),
                     err)) {
            return false;
        }
        const QPointF local = detach->bounds.center();
        QMouseEvent press(QEvent::MouseButtonPress, local, view->position() + local, Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(view, &press);
        ok &= require(!controller.isDetached() && !view->isDragging(),
                      QStringLiteral("detach must wait until control release"), err);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(), QPointF(view->position()),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(view, &release);
        ok &= require(!controller.isDetached() && release.isAccepted(),
                      QStringLiteral("release outside control must cancel without leaking input"),
                      err);
    }

    const std::array<QString, 5> names = { QStringLiteral("Compact"), QStringLiteral("Detailed"),
                                           QStringLiteral("Custom"), QStringLiteral("Off"),
                                           QStringLiteral("Minimal") };
    for (const auto& name : names) {
        const Preset expected = nextPreset(controller.preset());
        ok &= clickControl(*view, QStringLiteral("Details: ") + name, err);
        ok &= require(
            controller.preset() == expected && controller.isEditing() && view->isVisible() &&
                !captured && editEnds == 0,
            QStringLiteral("cycling %1 must update preview without closing editor").arg(name), err);
        const auto preview = view->recordedText();
        const bool offPreview =
            std::any_of(preview.begin(), preview.end(), [](const PaintedText& item) {
                return item.text == QLatin1String("Overlay off");
            });
        ok &= require(offPreview == (expected == Preset::Off),
                      QStringLiteral("Off must replace the stats with an explicit live preview"),
                      err);
        if (expected != Preset::Off) {
            ok &= require(settings.displayState(key).preset == expected,
                          QStringLiteral("edit-mode preset did not persist"), err);
        }
    }
    // Each click of a cycle control counts, so rapid clicking keeps cycling.
    // A toggle swallows the second click; it would silently undo the first.
    ok &= doubleClickControl(*view, QStringLiteral("Details: Compact"), err);
    ok &= require(controller.preset() == Preset::Custom,
                  QStringLiteral("double-clicking Details must advance once per click"), err);
    ok &= doubleClickControl(*view, QStringLiteral("Detach"), err);
    ok &= require(controller.isDetached() && controller.isEditing(),
                  QStringLiteral("double-clicking Detach must toggle exactly once"), err);
    ok &= clickControl(*view, QStringLiteral("Re-attach"), err);
    controller.setPreset(Preset::Detailed);
    const auto customMetrics = settings.metricList();
    ok &= clickControl(*view, QStringLiteral("Custom: Off"), err);
    ok &= require(controller.preset() == Preset::Custom && controller.isEditing(),
                  QStringLiteral("Custom must update the live editor"), err);
    ok &= doubleClickControl(*view, QStringLiteral("Custom: On"), err);
    ok &= require(
        controller.preset() == Preset::Detailed && settings.metricList() == customMetrics,
        QStringLiteral("Custom must return to the previous level without changing metrics"), err);
    const int scale = settings.scalePercent();
    const int opacity = settings.opacityPercent();
    ok &= clickControl(*view, QStringLiteral("Size +"), err);
    ok &= clickControl(*view, QStringLiteral("Opacity +"), err);
    ok &=
        require(settings.scalePercent() == scale + 5 && settings.opacityPercent() == opacity + 5 &&
                    controller.isEditing(),
                QStringLiteral("size and opacity controls must update persisted live style"), err);
    ok &= clickControl(*view, QStringLiteral("Size -"), err);
    ok &= clickControl(*view, QStringLiteral("Opacity -"), err);
    settings.setAttachedPosition(QStringLiteral("other"), QPoint(123, 456));
    settings.setDisplaySize(QStringLiteral("other"), Preset::Detailed, QSize(700, 500));
    settings.setDisplaySize(key, Preset::Detailed, QSize(520, 200));
    ok &= clickControl(*view, QStringLiteral("Reset"), err);
    ok &= require(
        controller.preset() == Preset::Detailed && controller.isEditing() &&
            !controller.isDetached() && !settings.attachedPosition(key) &&
            !settings.displaySize(key, Preset::Detailed).isValid() &&
            settings.detachedPosition(key) == QPoint(350, 220) &&
            settings.attachedPosition(QStringLiteral("other")) == QPoint(123, 456) &&
            settings.displaySize(QStringLiteral("other"), Preset::Detailed) == QSize(700, 500) &&
            settings.scalePercent() == DefaultScalePercent &&
            settings.opacityPercent() == DefaultOpacityPercent &&
            settings.metricList() == customMetrics,
        QStringLiteral("Reset must preserve other placements, preset, attachment and metrics"),
        err);
    controller.setPreset(Preset::Compact);
    controller.setPreset(Preset::Off);
    ok &= clickControl(*view, QStringLiteral("Done"), err);
    ok &= require(!controller.isEditing() && !view->isVisible() && captured && editEnds == 1 &&
                      controller.preset() == Preset::Off,
                  QStringLiteral("Done must hide Off preview and end capture ownership once"), err);

    controller.setPointerEditMode(true);
    controller.setPreset(Preset::Compact);
    view->render();
    const QPointF local(50, 12);
    QMouseEvent press(QEvent::MouseButtonPress, local, view->position() + local, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(view, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, local, view->position() + local, Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(view, &release);
    ok &= require(
        view->isVisible() && controller.isEditing() && press.isAccepted() && release.isAccepted() &&
            !view->isDragging(),
        QStringLiteral("held Right Alt click must enter edit without activating a control"), err);
    ok &= clickControl(*view, QStringLiteral("Details: Compact"), err);
    ok &= require(controller.preset() == Preset::Detailed,
                  QStringLiteral("reopening Off editor must preview the retained preset"), err);
    controller.setPreset(Preset::Off);
    controller.setPointerEditMode(false);
    ok &= require(
        controller.isEditing() && view->isVisible() && editEnds == 1,
        QStringLiteral("Right Alt release must not close an editor that owns the pointer"), err);
    controller.endEditing();
    controller.setPointerEditMode(true);
    controller.beginEditing();
    controller.syncWithStreamWindow(false);
    ok &= require(!controller.isEditing() && !view->isVisible(),
                  QStringLiteral("minimizing must hide the editor"), err);
    return ok;
}

bool testMoveControlsFitStream(QTextStream& err)
{
    QTemporaryDir directory;
    if (!require(directory.isValid(), QStringLiteral("temporary fit settings"), err)) {
        return false;
    }
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    QCoreApplication::setApplicationName(QStringLiteral("OverlayMoveControlsFit"));
    QRect stream(0, 0, 1280, 720);
    const QString key = displayKey(QGuiApplication::primaryScreen()->name());
    StatsOverlaySettings settings;
    settings.setScalePercent(80);
    // Narrowest and tallest saved panel: the controls wrap into several rows
    settings.setDisplaySize(key, Preset::Detailed, QSize(1, 100000));
    Overlay::OverlayManager manager;
    StreamStatsModel model;
    auto window = std::make_unique<RenderWindow>();
    auto* view = window.get();
    OverlayStatsController controller(manager, model, nullptr, OverlayWindowMode::Detached, true,
                                      std::move(window));
    controller.setParentGeometryProvider([&] { return stream; });
    controller.setPreset(Preset::Detailed);
    controller.beginEditing();
    bool ok = require(stream.contains(view->geometry()) &&
                          stream.contains(QRect(view->position(), view->preferredSize())),
                      QStringLiteral("move-mode window leaves the stream: %1x%2 at %3,%4")
                          .arg(view->preferredSize().width())
                          .arg(view->preferredSize().height())
                          .arg(view->x())
                          .arg(view->y()),
                      err);
    const auto text = view->recordedText();
    const QRectF client(QPointF(), view->size());
    QString painted;
    for (const PaintedText& item : text) {
        ok &= require(client.contains(item.bounds),
                      QStringLiteral("'%1' is painted outside the %2x%3 move-mode window")
                          .arg(item.text)
                          .arg(view->width())
                          .arg(view->height()),
                      err);
        painted += item.text;
    }
    // Narrow controls wrap their labels across painted lines
    painted.remove(QLatin1Char(' '));
    for (const QString& label :
         { QStringLiteral("Detach"), QStringLiteral("Details:Detailed"), QStringLiteral("Done") }) {
        ok &= require(painted.contains(label),
                      QStringLiteral("move-mode control '%1' is not painted").arg(label), err);
    }
    stream = QRect(20, 30, 640, 360);
    controller.syncWithStreamWindow(true);
    ok &= require(stream.contains(view->geometry()) &&
                      stream.contains(QRect(view->position(), view->preferredSize())),
                  QStringLiteral("shrinking stream: window %1x%2 at %3,%4; preferred %5x%6")
                      .arg(view->width())
                      .arg(view->height())
                      .arg(view->x())
                      .arg(view->y())
                      .arg(view->preferredSize().width())
                      .arg(view->preferredSize().height()),
                  err);
    ok &= clickControl(*view, QStringLiteral("Done"), err);
    controller.endEditing();
    return ok;
}

bool testLocalizedMoveControls(QTextStream& err)
{
    bool ok = true;
#ifdef Q_OS_WIN32
    // The offscreen font database does not discover Windows' system fallback fonts.
    ok &= require(
        QFontDatabase::addApplicationFont(
            QDir(qEnvironmentVariable("WINDIR")).filePath(QStringLiteral("Fonts/msyh.ttc"))) >= 0,
        QStringLiteral("load production Microsoft YaHei fallback for offscreen rendering"), err);
#endif
    for (const QString& locale : { QStringLiteral("zh_CN"), QStringLiteral("zh_TW") }) {
        QTranslator translator;
        if (!require(translator.load(QDir(QStringLiteral(TEST_TRANSLATIONS_DIR))
                                         .filePath(QStringLiteral("qml_%1.qm").arg(locale))),
                     QStringLiteral("load compiled move-control translations for %1").arg(locale),
                     err)) {
            return false;
        }
        QCoreApplication::installTranslator(&translator);
        ok &= testMoveControlsVisible(err);
        QCoreApplication::removeTranslator(&translator);
    }
    return ok;
}

bool testCompactSavedHeightReflow(QTextStream& err)
{
    bool ok = true;
    const QStringList labels{ "Res",  "FPS",  "1% low", "Lat",   "Rate", "RTT",  "Jit",
                              "Loss", "Host", "Dec",    "Queue", "Rend", "Drops" };
    const QStringList values{ "3840x2160@60 HEVC",
                              "60/60/60",
                              "59",
                              "~8",
                              "37.6",
                              "1 ± 1",
                              "1.34",
                              "0.00%",
                              "5.2",
                              "0.8",
                              "0.4",
                              "1.1",
                              "0.0%" };
    for (int width : { 1600, 1450, 1300, 1150, 1000, 900, 750, 600, 500, 400, 300, 250 }) {
        RenderWindow window;
        window.setStats(sample(), {});
        window.setPreset(Preset::Compact);
        window.setScalePercent(80);
        window.setBodySize(QSize(width, 25));
        const auto text = window.recordedText();
        auto hasText = [&](const QString& wanted) {
            return std::any_of(text.begin(), text.end(), [&](const PaintedText& item) {
                return wanted.compare(item.text, Qt::CaseInsensitive) == 0;
            });
        };
        for (const QString& label : width <= 400 ? labels : QStringList()) {
            ok &= require(
                hasText(label),
                QStringLiteral("saved-height Compact width%1 drops label %2").arg(width).arg(label),
                err);
        }
        for (const QString& value : values) {
            ok &= require(
                hasText(value),
                QStringLiteral("saved-height Compact width%1 drops value %2").arg(width).arg(value),
                err);
        }
        ok &= require(
            (hasText(QStringLiteral("Frame time")) || hasText(QStringLiteral("FT"))) &&
                !hasText(QStringLiteral("Estimated latency")),
            QStringLiteral("Compact has the approved inline trace, not Custom's latency panel"),
            err);
        for (const PaintedText& item : text) {
            ok &= require(
                QRectF(QPointF(), window.size()).adjusted(-1, -1, 1, 1).contains(item.bounds),
                QStringLiteral("reflow text leaves body: %1").arg(item.text), err);
        }
        for (qreal dpr : { 1.0, 1.5, 2.0 }) {
            const QImage image = window.render(dpr);
            const auto nativeText = window.recordedText(nullptr, dpr);
            for (const QString& wanted : (width <= 400 ? labels : QStringList()) + values) {
                ok &= require(
                    std::any_of(nativeText.begin(), nativeText.end(),
                                [&](const PaintedText& item) {
                                    return wanted.compare(item.text, Qt::CaseInsensitive) == 0 &&
                                           QRectF(image.rect())
                                               .adjusted(-1, -1, 1, 1)
                                               .contains(item.bounds);
                                }),
                    QStringLiteral("Compact w%1 DPR%2 clips %3").arg(width).arg(dpr).arg(wanted),
                    err);
            }
            ok &= require(image.devicePixelRatio() == dpr,
                          QStringLiteral("reflow raster has wrong DPR"), err);
            const QString output = qEnvironmentVariable("MOONLIGHT_OVERLAY_TEST_OUTPUT_DIR");
            if (!output.isEmpty()) {
                ok &=
                    require(image.save(QDir(output).filePath(
                                QStringLiteral("compact-reflow-w%1-d%2.png").arg(width).arg(dpr))),
                            QStringLiteral("save reflow proof"), err);
            }
        }
    }
    return ok;
}

bool testStripCoverageAndTelemetry(QTextStream& err)
{
    bool ok = true;
    RenderWindow window;
    const QString output = qEnvironmentVariable("MOONLIGHT_OVERLAY_TEST_OUTPUT_DIR");
    for (int scale : { 75, 80, 100, 150 }) {
        window.setScalePercent(scale);
        window.setStats(sample(), {});
        window.setPreset(Preset::Compact);
        for (int width : { 1600, 1450, 1300, 1150, 1000, 900, 750, 600, 500, 400, 300, 250 }) {
            window.setBodySize(QSize(width, 25));
            for (qreal dpr : { 1.0, 1.5, 2.0 }) {
                const auto text = window.recordedText(nullptr, dpr);
                std::set<Metric> metrics;
                for (const auto& item : text) {
                    if (item.text != QStringLiteral("|")) {
                        // Fresh72% black backing over white is the worst-case
                        // scene. This is nominal contrast, not glyph-size proof.
                        ok &= require(
                            (luminance(item.color) + 0.05) /
                                    (luminance(QColor(72, 72, 72)) + 0.05) >=
                                4.5,
                            QStringLiteral("strip text lacks nominal contrast: %1").arg(item.text),
                            err);
                    }
                    const auto hit = window.tooltipAt((item.bounds.center() / dpr).toPoint());
                    if (hit) {
                        metrics.insert(hit->metric);
                        if (hit->metric == Metric::Rtt) {
                            ok &= require(hit->value == QStringLiteral("1 ± 1 ms"),
                                          QStringLiteral("RTT tooltip loses variance/units"), err);
                        }
                    }
                    ok &= require(QRectF(QPointF(), window.size() * dpr)
                                      .adjusted(-1, -1, 1, 1)
                                      .contains(item.bounds),
                                  QStringLiteral("strip width%1 scale%2 dpr%3 clips '%4'")
                                      .arg(width)
                                      .arg(scale)
                                      .arg(dpr)
                                      .arg(item.text),
                                  err);
                }
                for (Metric metric : compactMetrics()) {
                    ok &= require(metrics.count(metric) == 1,
                                  QStringLiteral("strip width%1 scale%2 dpr%3 loses tooltip %4")
                                      .arg(width)
                                      .arg(scale)
                                      .arg(dpr)
                                      .arg(metricId(metric)),
                                  err);
                }
                if (!output.isEmpty()) {
                    ok &= require(window.render(dpr).save(
                                      QDir(output).filePath(QStringLiteral("strip-w%1-s%2-d%3.png")
                                                                .arg(width)
                                                                .arg(scale)
                                                                .arg(dpr))),
                                  QStringLiteral("save strip matrix"), err);
                }
            }
        }
    }
    window.setBodySize(QSize());
    window.setPreset(Preset::Minimal);
    auto text = window.recordedText();
    for (const QString& value :
         { QStringLiteral("60"), QStringLiteral("59"), QStringLiteral("~8") }) {
        ok &= require(std::any_of(text.begin(), text.end(),
                                  [&](const PaintedText& item) { return item.text == value; }),
                      QStringLiteral("Minimal must preserve FPS, 1% low and latency"), err);
    }
    window.setPreset(Preset::Detailed);
    StatsSnapshot unmeasured = sample();
    unmeasured.presentTiming.measured = false;
    window.setStats(unmeasured, {});
    text = window.recordedText();
    auto contains = [&](const QString& label) {
        return std::any_of(text.begin(), text.end(),
                           [&](const PaintedText& item) { return item.text == label; });
    };
    ok &= require(contains(QStringLiteral("0.1% low")) && !contains(QStringLiteral("Phases")) &&
                      !contains(QStringLiteral("FEC")),
                  QStringLiteral("Detailed must retain lows and gate unmeasured telemetry"), err);
    StatsSnapshot measured = sample();
    measured.presentTiming.measured = true;
    measured.videoPackets = 200;
    window.setStats(measured, {});
    text = window.recordedText();
    ok &= require(contains(QStringLiteral("Phases")) && contains(QStringLiteral("FEC")),
                  QStringLiteral("Detailed must show measured Present and FEC"), err);
    window.setPreset(Preset::Custom);
    window.setMetrics({ Metric::Fps, Metric::Latency });
    text = window.recordedText();
    ok &= require(contains(QStringLiteral("FPS")) && contains(QStringLiteral("Latency")) &&
                      !contains(QStringLiteral("RTT")),
                  QStringLiteral("Custom must retain the user's metric selection"), err);
    StatsSnapshot unavailable = sample();
    unavailable.valid = false;
    window.setPreset(Preset::Minimal);
    window.setStats(unavailable, {});
    text = window.recordedText();
    ok &= require(!contains(QStringLiteral("59")) && !contains(QStringLiteral("60")),
                  QStringLiteral("unavailable telemetry must not show cached sample values"), err);
    return ok;
}
bool testLiveStripWrapStability(QTextStream& err)
{
    bool ok = true;
    StatsSnapshot low = sample();
    low.rttMs = 8;
    low.rttVarianceMs = 1;
    StatsSnapshot high = low;
    high.rttMs = 12;
    high.rttVarianceMs = 3;
    RenderWindow probe;
    probe.setPreset(Preset::Compact);
    probe.setScalePercent(100);
    probe.setStats(low, {});
    probe.setBodySize(QSize(3000, 25));
    const int rowHeight = probe.preferredSize().height();
    // Discover the real-font boundary, rather than pinning platform glyph widths.
    int left = 250, right = 3000;
    while (left < right) {
        const int width = (left + right) / 2;
        probe.setBodySize(QSize(width, 25));
        if (probe.preferredSize().height() == rowHeight) {
            right = width;
        } else {
            left = width + 1;
        }
    }
    const int boundary = left;
    err << "Live NET fixture: one-row boundary=" << boundary << " at 100%\n";
    const QString output = qEnvironmentVariable("MOONLIGHT_OVERLAY_TEST_OUTPUT_DIR");
    QVector<QImage> sequence;
    for (int scale : { 75, 80, 100, 150 }) {
        for (int width : { boundary - 2, boundary, boundary + 2, 1452, 400, 250 }) {
            RenderWindow window;
            window.setPreset(Preset::Compact);
            window.setScalePercent(scale);
            window.setBodySize(QSize(width, 25));
            window.setStats(low, {});
            const QSize initial = window.preferredSize();
            for (int update = 0; update < 12; ++update) {
                const StatsSnapshot& snapshot = update % 2 ? high : low;
                window.setStats(snapshot, {});
                const auto text = window.recordedText();
                ok &= require(window.preferredSize() == initial,
                              QStringLiteral("live NET w%1 s%2 update%3 changed %4x%5 to %6x%7")
                                  .arg(width)
                                  .arg(scale)
                                  .arg(update)
                                  .arg(initial.width())
                                  .arg(initial.height())
                                  .arg(window.preferredSize().width())
                                  .arg(window.preferredSize().height()),
                              err);
                std::set<Metric> metrics;
                for (const auto& item : text) {
                    const auto hit = window.tooltipAt(item.bounds.center().toPoint());
                    if (hit) {
                        metrics.insert(hit->metric);
                        if (hit->metric == Metric::Rtt) {
                            ok &=
                                require(hit->value == (update % 2 ? QStringLiteral("12 ± 3 ms")
                                                                  : QStringLiteral("8 ± 1 ms")),
                                        QStringLiteral("live NET must retain the full value"), err);
                        }
                    }
                    ok &= require(QRectF(QPointF(), window.size())
                                      .adjusted(-1, -1, 1, 1)
                                      .contains(item.bounds),
                                  QStringLiteral("live NET clips %1").arg(item.text), err);
                }
                for (Metric metric : compactMetrics()) {
                    ok &= require(metrics.count(metric) == 1,
                                  QStringLiteral("live NET loses metric %1").arg(metricId(metric)),
                                  err);
                }
                if (scale == 80 && width == 1452 && update < 3) {
                    err << "NET sequence update" << update << ": " << window.preferredSize().width()
                        << 'x' << window.preferredSize().height() << '\n';
                    sequence.append(window.render());
                }
            }
            window.setBodySize(QSize(250, 25));
            const QSize narrow = window.preferredSize();
            window.setBodySize(QSize(3000, 25));
            ok &= require(window.preferredSize().height() < narrow.height(),
                          QStringLiteral("user resizing must still reflow live stats"), err);
        }
    }
    if (!output.isEmpty()) {
        QImage sheet(1400, 300, QImage::Format_RGB32);
        sheet.fill(QColor("#24282d"));
        QPainter painter(&sheet);
        QFont caption;
        caption.setPixelSize(14);
        painter.setFont(caption);
        for (int i = 0; i < sequence.size(); ++i) {
            painter.setPen(Qt::white);
            painter.drawText(
                8, i * 95 + 18,
                QStringLiteral("Update %1: NET %2 — same width/scale")
                    .arg(i + 1)
                    .arg(i == 1 ? QStringLiteral("12 ± 3 ms") : QStringLiteral("8 ± 1 ms")));
            painter.drawImage(8, i * 95 + 28, sequence[i]);
        }
        painter.end();
        ok &= require(sheet.save(QDir(output).filePath("live-net-sequence.png")),
                      QStringLiteral("save repeated live NET sequence"), err);
    }
    return ok;
}

bool testLiveStripSlotsAndController(QTextStream& err)
{
    bool ok = true;
    StatsSnapshot low = sample(), high = low, missing = low;
    low.targetFps = high.targetFps = missing.targetFps = 120;
    low.receivedFps = low.decodedFps = low.renderedFps = low.totalFps = 99;
    high.receivedFps = high.decodedFps = high.renderedFps = high.totalFps = 100;
    low.rttMs = low.rttVarianceMs = 9;
    high.rttMs = high.rttVarianceMs = 99;
    low.jitterMs = 9.99;
    high.jitterMs = 10.0;
    low.bitrateMbps = 99.9;
    high.bitrateMbps = 100.0;
    low.networkLossPercent = low.pacerDropPercent = 9.99;
    high.networkLossPercent = high.pacerDropPercent = 100;
    low.latency.hostMs = low.latency.decodeMs = low.latency.queueMs = low.latency.renderMs = 9.9;
    high.latency.hostMs = high.latency.decodeMs = high.latency.queueMs = high.latency.renderMs =
        10.0;
    high.frameTimes.onePercentLowFps = 100;
    missing.valid = false;
    missing.hasRtt = missing.latency.hasHost = missing.latency.hasQueue = false;
    for (Preset preset : { Preset::Minimal, Preset::Compact, Preset::Detailed }) {
        for (int width : { 250, 400, 1452, 2000 }) {
            RenderWindow window;
            window.setPreset(preset);
            window.setBodySize(QSize(width, 25));
            window.setStats(low, {});
            const QSize initial = window.preferredSize();
            for (int update = 0; update < 9; ++update) {
                window.setStats(update % 3 == 0 ? low : update % 3 == 1 ? high : missing, {});
                for (qreal dpr : { 1.0, 1.5, 2.0 }) {
                    const auto text = window.recordedText(nullptr, dpr);
                    ok &= require(window.preferredSize() == initial,
                                  QStringLiteral("live slots preset%1 w%2 update%3 reflow")
                                      .arg(static_cast<int>(preset))
                                      .arg(width)
                                      .arg(update),
                                  err);
                    for (const auto& item : text) {
                        ok &= require(QRectF(QPointF(), window.size() * dpr)
                                          .adjusted(-1, -1, 1, 1)
                                          .contains(item.bounds),
                                      QStringLiteral("live slots clip '%1'").arg(item.text), err);
                    }
                }
            }
        }
    }
    {
        RenderWindow window;
        window.setPreset(Preset::Compact);
        window.setBodySize(QSize(250, 25));
        window.setStats(low, {});
        const QSize normal = window.preferredSize();
        StatsSnapshot extreme = low;
        extreme.rttMs = extreme.rttVarianceMs = 123456;
        window.setStats(extreme, {});
        const QSize expanded = window.preferredSize();
        window.setStats(low, {});
        ok &= require(window.preferredSize() == expanded,
                      QStringLiteral("exceptional numeric widths must not shrink on refresh"), err);
        // Repeated controller-style setters are not deliberate layout changes.
        window.setPreset(Preset::Compact);
        window.setScalePercent(80);
        window.setBodySize(QSize(250, 25));
        ok &= require(window.preferredSize() == expanded,
                      QStringLiteral("idempotent style setters must retain numeric budgets"), err);
        window.setBodySize(QSize(251, 25));
        window.setBodySize(QSize(250, 25));
        ok &= require(window.preferredSize() == normal,
                      QStringLiteral("explicit resize must reset exceptional width budgets"), err);
        window.setScalePercent(100);
        ok &= require(window.preferredSize() != normal,
                      QStringLiteral("explicit scale must still change geometry"), err);
        window.setPreset(Preset::Minimal);
        ok &= require(window.preferredSize().height() < expanded.height(),
                      QStringLiteral("preset changes must still reflow"), err);
    }
    QTemporaryDir directory;
    if (!require(directory.isValid(), QStringLiteral("temporary live stats settings"), err)) {
        return false;
    }
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    QCoreApplication::setApplicationName(QStringLiteral("OverlayLiveWrap"));
    Overlay::OverlayManager manager;
    StreamStatsModel model;
    auto window = std::make_unique<RenderWindow>();
    auto* view = window.get();
    OverlayStatsController controller(manager, model, nullptr, OverlayWindowMode::Detached, true,
                                      std::move(window));
    StatsOverlaySettings settings;
    const QString key = displayKey(QGuiApplication::primaryScreen()->name());
    settings.setDisplaySize(key, Preset::Compact, QSize(1452, 25));
    controller.setParentGeometryProvider([] { return QRect(0, 0, 1920, 1080); });
    controller.setPreset(Preset::Compact);
    StatsWindow stats;
    stats.width = 3840;
    stats.height = 2160;
    stats.codec = QStringLiteral("HEVC");
    stats.targetFps = 60;
    stats.totalFps = stats.receivedFps = stats.decodedFps = stats.renderedFps = 60;
    QSize preferred, outer;
    for (int update = 0; update < 12; ++update) {
        stats.rttMs = update % 2 ? 12 : 8;
        stats.rttVarianceMs = update % 2 ? 3 : 1;
        model.publishWindow(stats, {}, LiGetMicroseconds());
        controller.syncWithStreamWindow(true);
        if (update == 0) {
            preferred = view->preferredSize();
            outer = view->size();
        }
        ok &= require(view->preferredSize() == preferred && view->size() == outer,
                      QStringLiteral("controller live refresh must not feed back into row layout"),
                      err);
    }
    return ok;
}

bool testDetachedColdRestore(QTextStream& err)
{
    QTemporaryDir directory;
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    QCoreApplication::setApplicationName(QStringLiteral("OverlayDetachedRestart"));
    Overlay::OverlayManager manager;
    StreamStatsModel model;
    const QRect stream(0, 0, 1280, 720);
    {
        auto window = std::make_unique<RenderWindow>();
        auto* view = window.get();
        OverlayStatsController first(manager, model, nullptr, OverlayWindowMode::Detached, true,
                                     std::move(window));
        first.setParentGeometryProvider([&] { return stream; });
        first.setDetached(true);
        first.beginEditing();
        view->setPosition(QPoint(222, 111));
        QCoreApplication::processEvents();
        first.resetLayout();
    }
    QSettings().sync();
    const auto coldProcess = [&](bool detached) {
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(),
                    { QStringLiteral("--check-detached"), directory.path(),
                      detached ? QStringLiteral("true") : QStringLiteral("false") });
        return require(child.waitForFinished(30000) && child.exitStatus() == QProcess::NormalExit &&
                           child.exitCode() == 0,
                       QStringLiteral("cold process detachment restore failed: %1")
                           .arg(QString::fromUtf8(child.readAllStandardError())),
                       err);
    };
    const QString key = displayKey(QGuiApplication::primaryScreen()->name());
    bool ok = require(StatsOverlaySettings().detachedPosition(key) != QPoint(222, 111),
                      QStringLiteral("Reset must cancel pending saves before destruction"), err);
    ok &= coldProcess(true);
    {
        OverlayStatsController unsupported(manager, model, nullptr, OverlayWindowMode::Detached,
                                           true);
        ok &= require(!unsupported.usesCompositorWindow() && StatsOverlaySettings().detached(),
                      QStringLiteral("compositor fallback must preserve detached preference"), err);
    }
    auto window = std::make_unique<RenderWindow>();
    auto* view = window.get();
    OverlayStatsController reopened(manager, model, nullptr, OverlayWindowMode::Detached, true,
                                    std::move(window));
    reopened.setParentGeometryProvider([&] { return stream; });
    ok &=
        require(reopened.isDetached() && view->isStandalone(),
                QStringLiteral("detachment must survive a cold controller/settings restart"), err);
    reopened.setDetached(false);
    QSettings().sync();
    ok &= coldProcess(false);
    {
        OverlayStatsController attached(manager, model, nullptr, OverlayWindowMode::Detached, true,
                                        std::make_unique<RenderWindow>());
        ok &= require(!attached.isDetached(), QStringLiteral("explicit re-attach must persist too"),
                      err);
    }
    return ok;
}

} // namespace

bool checkDetachedRestart(const QString& settingsPath, bool detached, QTextStream& err)
{
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsPath);
    QCoreApplication::setOrganizationName(QStringLiteral("MoonlightTests"));
    QCoreApplication::setApplicationName(QStringLiteral("OverlayDetachedRestart"));
    Overlay::OverlayManager manager;
    StreamStatsModel model;
    auto window = std::make_unique<RenderWindow>();
    auto* view = window.get();
    OverlayStatsController controller(manager, model, nullptr, OverlayWindowMode::Detached, true,
                                      std::move(window));
    return require(controller.isDetached() == detached && view->isStandalone() == detached &&
                       !view->isVisible(),
                   QStringLiteral("restore the attachment configuration before first show"), err);
}

bool runWindowRenderTests(QTextStream& err)
{
    bool ok = testDefaultsAndPersistence(err);
    {
        RenderWindow strip;
        strip.setPreset(Preset::Compact);
        strip.setStats(sample(), {});
        strip.setScalePercent(100);
        strip.setOpacityPercent(95);
        const QImage image = strip.render();
        const QColor background = image.pixelColor(image.width() / 2, image.height() - 2);
        ok &= require(background.red() == 0 && background.green() == 0 && background.blue() == 0 &&
                          std::abs(background.alpha() - 242) <= 1,
                      QStringLiteral("approved strip needs a continuous black backing"), err);
        bool fpsValue = false;
        for (const PaintedText& text : strip.recordedText()) {
            if (text.text == QStringLiteral("60/60/60")) {
                fpsValue = true;
                ok &= require(
                    text.font.stretch() == QFont::Unstretched &&
                        text.font.weight() == QFont::DemiBold && text.font.pixelSize() == 18,
                    QStringLiteral("strip values need normal-width 18px semibold type"), err);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
                ok &= require(text.font.featureValue("tnum") == 1,
                              QStringLiteral("strip figures must request tabular spacing"), err);
#endif
            }
        }
        ok &= require(fpsValue, QStringLiteral("strip preserves the FPS triplet"), err);
    }
    ok &= testRenderedBar(err);
    ok &= testRenderedPresetsAndDpi(err);
    ok &= testSceneComposites(err);
    ok &= testFlushTop(err);
    ok &= testResizeGrabZones(err);
    ok &= testLightSnapping(err);
    ok &= testConditionalClickThrough(err);
    ok &= testLiveFreeDrag(err);
    ok &= testMoveControlsVisible(err);
    ok &= testMoveControlLifecycle(err);
    ok &= testMoveControlsFitStream(err);
    ok &= testLocalizedMoveControls(err);
    ok &= testCompactSavedHeightReflow(err);
    ok &= testStripCoverageAndTelemetry(err);
    ok &= testLiveStripWrapStability(err);
    ok &= testLiveStripSlotsAndController(err);
    ok &= testDetachedColdRestore(err);
    return ok;
}
