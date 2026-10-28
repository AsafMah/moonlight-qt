#include "settings/statsoverlaysettings.h"
#include "streaming/video/overlaystatswindow.h"

#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QImage>
#include <QMouseEvent>
#include <QPaintEngine>
#include <QPainter>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>

#include <cmath>

using namespace StatsOverlay;

namespace {

bool require(bool condition, const QString& message, QTextStream& err)
{
    if (!condition) {
        err << "FAIL: " << message << '\n';
    }
    return condition;
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
        text.append({ item.text(), item.font().pixelSize() * transform.m11(),
                      transform.mapRect(QRectF(point.x(), point.y() - item.ascent(), item.width(),
                                               item.ascent() + item.descent())),
                      state->pen().color(), item.font(), point, transform });
    }

    QVector<PaintedText> text;
    QVector<QImage> shadows;
};

class TextDevice : public QPaintDevice
{
public:
    explicit TextDevice(const QSize& size) : m_Size(size) {}
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
            return 1;
        case PdmDevicePixelRatioScaled:
            return devicePixelRatioFScale();
        default:
            return 0;
        }
    }

private:
    QSize m_Size;
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

    QVector<PaintedText> recordedText(QVector<QImage>* shadows = nullptr)
    {
        resize(preferredSize());
        TextDevice device(size());
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
    ok &= require(overlay.opacityPercent() == 55,
                  QStringLiteral("fresh overlay must default to 55% chip opacity"), err);
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
            ok &= require(image.width() == 1284,
                          QStringLiteral("default Compact footprint is %1 px, not old 80% (1284)")
                              .arg(image.width()),
                          err);
            ok &= require(std::abs(image.pixelColor(20, 2).alpha() - 140) <= 1,
                          QStringLiteral("default chip must paint at 55%, without stacked shadow"),
                          err);
            ok &= require(image.pixelColor(image.width() - 10, image.height() / 2).alpha() == 0,
                          QStringLiteral("Compact must not paint a full-bar background"), err);
        }
        bool labelFound = false;
        bool valueFound = false;
        int labelHaloPasses = 0;
        bool opaqueLabel = false;
        QStringList labels{ QStringLiteral("Res"),    QStringLiteral("Rx/De/Rd"),
                            QStringLiteral("1% low"), QStringLiteral("Lat"),
                            QStringLiteral("Rate"),   QStringLiteral("Rtt"),
                            QStringLiteral("Jit"),    QStringLiteral("Loss"),
                            QStringLiteral("Host"),   QStringLiteral("Dec"),
                            QStringLiteral("Queue"),  QStringLiteral("Rend"),
                            QStringLiteral("Drops") };
        QVector<QImage> shadows;
        for (const PaintedText& text : window.recordedText(&shadows)) {
            for (int i = labels.size() - 1; i >= 0; --i) {
                if (labels[i].compare(text.text, Qt::CaseInsensitive) == 0) {
                    labels.removeAt(i);
                }
            }
            if (text.text.compare(QLatin1String("Rx/De/Rd"), Qt::CaseInsensitive) == 0) {
                labelFound = true;
                if (text.color.red() == 0 && text.color.green() == 0 && text.color.blue() == 0 &&
                    text.color.alpha() >= 200) {
                    labelHaloPasses++;
                } else {
                    opaqueLabel = text.color.alpha() == 255;
                }
                ok &= require(
                    text.pixels >= 11,
                    QStringLiteral("%1% label shrinks to %2 px").arg(percent).arg(text.pixels),
                    err);
            }
            if (text.text == QLatin1String("60/60/60")) {
                valueFound = true;
                ok &= require(
                    text.pixels >= 12,
                    QStringLiteral("%1% value shrinks to %2 px").arg(percent).arg(text.pixels),
                    err);
                ok &= require(text.color.green() >= 235 && text.color.red() >= 110,
                              QStringLiteral("mint values must stay bright over teal scenes"), err);
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
        ok &= require(shadows.size() >= 26,
                      QStringLiteral("every Compact label/value needs its soft shadow"), err);
        for (const QImage& shadow : shadows) {
            int faintPixels = 0;
            int darkPixels = 0;
            for (int y = 0; y < shadow.height(); ++y) {
                for (int x = 0; x < shadow.width(); ++x) {
                    const int alpha = qAlpha(shadow.pixel(x, y));
                    faintPixels += alpha > 0 && alpha < 30;
                    darkPixels += alpha > 153;
                }
            }
            ok &= require(faintPixels > 0 && darkPixels == 0,
                          QStringLiteral("shadow must have a soft low-alpha falloff, not a ring"),
                          err);
        }
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
    ok &= require(std::abs(transparent.pixelColor(20, 2).alpha() - 77) <= 1 &&
                      std::abs(opaque.pixelColor(20, 2).alpha() - 217) <= 1 &&
                      transparent.pixelColor(transparent.width() - 10, 2).alpha() == 0 &&
                      opaque.pixelColor(opaque.width() - 10, 2).alpha() == 0,
                  QStringLiteral("opacity must control chips while preserving transparent gaps"),
                  err);
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

bool testAppearanceCandidates(QTextStream& err)
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

} // namespace

bool runWindowRenderTests(QTextStream& err)
{
    bool ok = testDefaultsAndPersistence(err);
    ok &= testRenderedBar(err);
    ok &= testRenderedPresetsAndDpi(err);
    ok &= testSceneComposites(err);
    ok &= testAppearanceCandidates(err);
    ok &= testFlushTop(err);
    return ok;
}
