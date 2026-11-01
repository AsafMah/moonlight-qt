#include "streaming/video/overlaymenupanel.h"
#include "settings/streamingpreferences.h"
#include <QGuiApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <stdexcept>

struct Preferences
{
    StreamingPreferences::VideoCodecConfig videoCodecConfig = StreamingPreferences::VCC_FORCE_AV1;
    int saves = 0;
    int changes = 0;
    void videoCodecConfigChanged() { ++changes; }
    void save()
    {
        ++saves;
        QSettings().setValue("codec", int(videoCodecConfig));
    }
};
struct PyrowaveRuntime
{
    static PyrowaveRuntime& get()
    {
        static PyrowaveRuntime runtime;
        return runtime;
    }
    bool isAvailable() const { return available; }
    bool available = true;
};
struct OverlayMenuSettings
{
    void setScalePercent(int) {}
};
struct FakeStatsOverlay
{
    void setOpacityPercent(int) {}
};
class Session
{
public:
    Preferences preferences;
    Preferences* m_Preferences = &preferences;
    FakeStatsOverlay* m_StatsOverlay = nullptr;
    OverlayMenuPanel panel;
    OverlayMenuPanel::StreamConfigurationState state;
    int liveCalls = 0;
    QString toast;
    static QString tr(const char* text) { return QString::fromUtf8(text); }
    void showStreamingToast(const QString& text) { toast = text; }
    void updateStreamConfigurationMenuState()
    {
        state.nextStreamCodec = preferences.videoCodecConfig;
        panel.updateStreamConfigurationState(state);
    }
    bool handleOverlayMenuOptionPayload(OverlayMenuPanel::MenuAction, const QString&);
};

// METHOD

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    QTemporaryDir directory;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    QCoreApplication::setOrganizationName("MenuPreferenceTest");
    QCoreApplication::setApplicationName("NextStream");
    auto require = [](bool ok, const char* message) {
        if (!ok)
            throw std::runtime_error(message);
    };
    try {
        Session session;
        auto& panel = session.panel;
        session.state.currentCodec = "AV1";
        session.state.requestedCodec = StreamingPreferences::VCC_FORCE_AV1;
        session.state.pyrowaveAvailable = true;
        session.updateStreamConfigurationMenuState();
        panel.setStreamConfigurationCallback([&](const auto&) { ++session.liveCalls; });
        panel.setPayloadActionCallback([&](auto action, const QString& payload) {
            require(session.handleOverlayMenuOptionPayload(action, payload),
                    "Native next-stream click did not reach production preference handler");
        });
        panel.showWithPlacement(OverlayMenu::Placement::TopLeft, 0, 0, 1920, 1080, std::nullopt,
                                std::nullopt);
        QTest::qWait(160);
        require(panel.showLevel(OverlayMenuPanel::MenuAction::OpenStreamConfiguration),
                "Codec menu did not open");
        const int row = panel.rowOfAction(OverlayMenuPanel::MenuAction::SelectNextStreamCodec, "5");
        require(row >= 0 && panel.rowEnabled(row), "Next-stream PyroWave must be selectable");
        panel.ensureRowVisible(row);
        QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, panel.rowGeometry(row).center());
        require(session.preferences.videoCodecConfig == StreamingPreferences::VCC_FORCE_PYROWAVE &&
                    session.preferences.saves == 1 && session.preferences.changes == 1 &&
                    QSettings().value("codec").toInt() == 5,
                "Click must save the PyroWave preference");
        require(session.state.requestedCodec == StreamingPreferences::VCC_FORCE_AV1 &&
                    session.state.currentCodec == "AV1" && session.liveCalls == 0,
                "Next-stream preference changed the current stream or reconfigured it");
        require(session.state.nextStreamCodec == 5 && !session.state.pending &&
                    session.toast.contains("current stream unchanged"),
                "Saved preference must be distinct from current/pending stream state");
        const int apply = panel.rowOfAction(OverlayMenuPanel::MenuAction::ApplyStreamConfiguration);
        require(apply >= 0 && !panel.rowEnabled(apply),
                "Saving next-stream PyroWave must not enable reconnect Apply");
        PyrowaveRuntime::get().available = false;
        session.preferences.videoCodecConfig = StreamingPreferences::VCC_FORCE_AV1;
        session.handleOverlayMenuOptionPayload(OverlayMenuPanel::MenuAction::SelectNextStreamCodec,
                                               "5");
        require(session.preferences.saves == 1 &&
                    session.preferences.videoCodecConfig == StreamingPreferences::VCC_FORCE_AV1 &&
                    session.toast.contains("unavailable"),
                "Missing runtime must not silently persist unsupported PyroWave");
    } catch (const std::exception& error) {
        qCritical("FAIL: %s", error.what());
        return 1;
    }
    qInfo("next-stream-codec passed");
    return 0;
}
