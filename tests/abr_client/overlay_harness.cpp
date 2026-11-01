#include "streaming/video/overlaystatswindow.h"

#include <QGuiApplication>
#include <QImage>
#include <QFontDatabase>
#include <QPainter>
#include <QDebug>
#include <QTranslator>
#include <QMetaProperty>
#include <QSignalSpy>
#include <QScreen>
#include <stdexcept>

class PaintedOverlay : public OverlayStatsWindow
{
public:
    void render()
    {
        resize(preferredSize());
        QImage image(size() * devicePixelRatio(), QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(devicePixelRatio());
        image.fill(Qt::transparent);
        QPainter painter(&image);
        paintOverlay(painter);
        painter.end();
        const QString artifacts = qEnvironmentVariable("OVERLAY_UI_ARTIFACTS");
        if (!artifacts.isEmpty()) {
            image.save(artifacts + "/stats-compact-dpr2.png");
            QImage composite(QSize(3840, 2160), QImage::Format_ARGB32_Premultiplied);
            composite.setDevicePixelRatio(2);
            composite.fill(QColor("#303841"));
            QPainter canvas(&composite);
            canvas.drawImage(QPoint(20, 20), image);
            canvas.end();
            composite.save(artifacts + "/stats-4k-200percent.png");
        }
    }
};

class OpacityOverlay : public OverlayRasterWindow
{
public:
    explicit OpacityOverlay(OverlayWindowMode mode) : OverlayRasterWindow(nullptr, mode) {}

protected:
    void paintOverlay(QPainter&) override {}
};

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    if (qEnvironmentVariableIsSet("OVERLAY_HIDPI_REPRO")) {
        if (app.primaryScreen()->geometry().size() != QSize(1920, 1080) ||
            app.primaryScreen()->devicePixelRatio() != 2)
            return 2;
    }
#ifdef Q_OS_WIN
    QFontDatabase::addApplicationFont(QStringLiteral("C:\\Windows\\Fonts\\segoeui.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral("C:\\Windows\\Fonts\\msyh.ttc"));
#endif
    PaintedOverlay window;
    using State = SunshineAbrRuntime::State;
    auto require = [](bool condition, const char* message) {
        if (!condition)
            throw std::runtime_error(message);
    };
    try {
        for (auto mode : { OverlayWindowMode::Detached, OverlayWindowMode::WaylandSubsurface }) {
            OpacityOverlay opacityWindow(mode);
            opacityWindow.create();
            const auto property = opacityWindow.metaObject()->property(
                opacityWindow.metaObject()->indexOfProperty("opacity"));
            require(property.hasNotifySignal(),
                    "Opacity property must retain change notifications");
            QSignalSpy propertyChanges(&opacityWindow, property.notifySignal());
            QSignalSpy windowChanges(&opacityWindow, &QWindow::opacityChanged);
            opacityWindow.setOpacity(0.5);
            require(opacityWindow.opacity() == 0.5, "Opacity setter must retain its value");
            require(propertyChanges.count() == 1 && windowChanges.count() == 1,
                    "Opacity must notify property and QWindow subscribers exactly once");
            require(propertyChanges.at(0).at(0).toReal() == 0.5,
                    "Opacity property notification must carry the current value");
            opacityWindow.setOpacity(0.5);
            require(propertyChanges.count() == 1 && windowChanges.count() == 1,
                    "Unchanged opacity must not notify again");
        }
        for (const QString& language :
             { QString(), QStringLiteral("zh_CN"), QStringLiteral("zh_TW") }) {
            QTranslator translator;
            if (!language.isEmpty()) {
                require(translator.load(QCoreApplication::applicationDirPath() +
                                        QStringLiteral("/qml_%1.qm").arg(language)),
                        "ABR translations failed to load");
                app.installTranslator(&translator);
            }
            for (auto preset : { StatsOverlay::Preset::Detailed, StatsOverlay::Preset::Custom }) {
                window.setPreset(preset);
                window.setMetrics({ StatsOverlay::Metric::Abr });
                for (auto entry : { std::pair<State, QString>{ State::Disabled, "Off" },
                                    { State::Enabling, "Enabling" },
                                    { State::Active, "Active" },
                                    { State::Unsupported, "Unsupported" },
                                    { State::HostRejected, "Rejected" },
                                    { State::AuthenticationFailed, "Auth failed" },
                                    { State::Degraded, "Degraded" },
                                    { State::Stopped, "Stopped" } }) {
                    OverlayStatsWindow::Extras extras;
                    extras.abr.state = entry.first;
                    extras.abr.detail = entry.first == State::Degraded && language.isEmpty()
                                            ? QString(2048, QLatin1Char('x'))
                                            : QStringLiteral("HTTP 400: no_active_stream");
                    window.setStats({}, extras);
                    window.render();
                    std::optional<OverlayStatsWindow::TooltipInfo> hit;
                    for (int y = 0; y < window.height() && !hit; ++y) {
                        for (int x = 0; x < window.width(); ++x) {
                            auto candidate = window.tooltipAt({ x, y });
                            if (candidate && candidate->metric == StatsOverlay::Metric::Abr) {
                                hit = candidate;
                                break;
                            }
                        }
                    }
                    require(hit.has_value(), "Rendered ABR cell not reachable by pointer");
                    const QString translated =
                        QCoreApplication::translate("OverlayStatsWindow", entry.second.toUtf8());
                    if (language == QStringLiteral("zh_CN")) {
                        const QHash<QString, QString> expected{
                            { "Enabling", "启用中" },      { "Active", "已启用" },
                            { "Unsupported", "不支持" },   { "Rejected", "已拒绝" },
                            { "Auth failed", "认证失败" }, { "Degraded", "通信异常" },
                            { "Stopped", "已停止" }
                        };
                        if (entry.first != State::Disabled) {
                            require(translated == expected.value(entry.second),
                                    "Rendered ABR state has incorrect Simplified Chinese text");
                        }
                    }
                    if (language == QStringLiteral("zh_TW")) {
                        const QHash<QString, QString> expected{
                            { "Enabling", "啟用中" },      { "Active", "已啟用" },
                            { "Unsupported", "不支援" },   { "Rejected", "已拒絕" },
                            { "Auth failed", "驗證失敗" }, { "Degraded", "通訊異常" },
                            { "Stopped", "已停止" }
                        };
                        if (entry.first != State::Disabled) {
                            require(translated == expected.value(entry.second),
                                    "Rendered ABR state has incorrect Traditional Chinese text");
                        }
                    }
                    require(hit->value.startsWith(translated),
                            "Rendered status still boolean/stale");
                    if (extras.abr.detail.size() > 256) {
                        require(hit->value.size() < 600 && hit->value.contains(QChar(0x2026)),
                                "Long ABR details were not bounded and elided");
                    } else {
                        require(hit->value.contains(extras.abr.detail),
                                "Host reason absent from hover");
                    }
                    const bool paused = entry.first == State::Degraded ||
                                        entry.first == State::HostRejected ||
                                        entry.first == State::AuthenticationFailed;
                    const QString recovery = QCoreApplication::translate(
                        "OverlayStatsWindow", "ABR paused. Reconnect to try again.");
                    require(hit->value.contains(recovery) == paused, "Wrong recovery affordance");
                    require(window.width() > 0 && window.width() < 1800,
                            "Short ABR state inflated overlay width");
                }
            }
            app.removeTranslator(&translator);
        }
        window.setPreset(StatsOverlay::Preset::Custom);
        window.setMetrics({ StatsOverlay::Metric::Bitrate });
        StatsSnapshot measured;
        measured.valid = true;
        measured.bitrateMbps = 70;
        window.setScalePercent(100);
        const QSize fullScale = window.preferredSize();
        window.setScalePercent(40);
        require(window.preferredSize().width() <= fullScale.width() * 0.45 &&
                    window.preferredSize().height() <= fullScale.height() * 0.45,
                "Stats compact scale must shrink the actual rendered geometry");
        for (auto entry : { std::pair<State, QString>{ State::Disabled, "Off" },
                            { State::Enabling, "Enabling" },
                            { State::Active, "Active" },
                            { State::Degraded, "Degraded" } }) {
            OverlayStatsWindow::Extras extras;
            extras.abr.state = entry.first;
            extras.targetBitrateKbps = 400000;
            window.setStats(measured, extras);
            window.render();
            std::optional<OverlayStatsWindow::TooltipInfo> bitrate;
            for (int y = 0; y < window.height() && !bitrate; ++y)
                for (int x = 0; x < window.width(); ++x) {
                    auto hit = window.tooltipAt({ x, y });
                    if (hit && hit->metric == StatsOverlay::Metric::Bitrate) {
                        bitrate = hit;
                        break;
                    }
                }
            require(bitrate.has_value(), "Bitrate tooltip must remain reachable");
            require(bitrate->value.contains("ABR: " + entry.second),
                    "Bitrate must expose actual ABR state even without the ABR row");
            require(bitrate->value.contains("Received") && bitrate->value.contains("70.0") &&
                        bitrate->value.contains("400"),
                    "Bitrate must distinguish received rate from target budget");
        }
    } catch (const std::exception& error) {
        qCritical("FAIL: %s", error.what());
        return 1;
    }
    qInfo("overlay-regressions passed");
    return 0;
}
