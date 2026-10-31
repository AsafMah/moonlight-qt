#include "streaming/video/overlaystatswindow.h"

#include <QGuiApplication>
#include <QImage>
#include <QFontDatabase>
#include <QPainter>
#include <QDebug>
#include <QTranslator>
#include <stdexcept>

class PaintedOverlay : public OverlayStatsWindow
{
public:
    void render()
    {
        resize(preferredSize());
        QImage image(size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        paintOverlay(painter);
    }
};

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
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
                    extras.abr.detail =
                        QStringLiteral("HTTP 400: No active streaming session for this client");
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
                    require(language.isEmpty() || entry.first == State::Disabled ||
                                translated != entry.second,
                            "ABR state untranslated");
                    require(hit->value.startsWith(translated),
                            "Rendered status still boolean/stale");
                    require(hit->value.contains(extras.abr.detail),
                            "Host reason absent from hover");
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
    } catch (const std::exception& error) {
        qCritical("FAIL: %s", error.what());
        return 1;
    }
    qInfo("overlay-regressions passed");
    return 0;
}
