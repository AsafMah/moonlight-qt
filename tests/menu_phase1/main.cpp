#include "../../app/streaming/video/overlaymenupanel.h"
#include <QGuiApplication>
#include <QSettings>
#include <QTest>
#include <QDir>
#include <QImage>
#include <QFontDatabase>

using A = OverlayMenuPanel::MenuAction;
using T = OverlayMenuPanel::MenuTab;

class Phase1 : public QObject
{
    Q_OBJECT
private:
    void open(OverlayMenuPanel& p, QSize size = QSize(1280, 900))
    {
        p.showWithPlacement(OverlayMenu::Placement::TopLeft, 0, 0, size.width(), size.height(),
                            std::nullopt, std::nullopt);
        QTest::qWait(160);
    }
    void click(OverlayMenuPanel& p, A action, const QString& payload = {})
    {
        if (action == A::OpenMenuSettings) {
            QTest::mouseClick(&p, Qt::LeftButton, Qt::NoModifier, p.gearGeometry().center());
            return;
        }
        const int row = p.rowOfAction(action, payload);
        QVERIFY2(row >= 0, "required action missing");
        p.ensureRowVisible(row);
        QTest::mouseClick(&p, Qt::LeftButton, Qt::NoModifier, p.rowGeometry(row).center());
    }
private slots:
    void init() { QSettings().clear(); }
    void mouseChoices()
    {
        OverlayMenuPanel p;
        bool desktop = false;
        std::vector<A> actions;
        p.setActionCallback([&](A a) {
            actions.push_back(a);
            if (a == A::SetMouseModeDesktop)
                desktop = true;
            if (a == A::SetMouseModeGame)
                desktop = false;
            p.updateInputState(desktop, true, true, false, true);
        });
        p.updateInputState(false, true, true, false, true);
        open(p);
        click(p, A::OpenMouseMode);
        click(p, A::SetMouseModeDesktop);
        QVERIFY(p.isMenuVisible());
        QCOMPARE(actions.back(), A::SetMouseModeDesktop);
        QVERIFY(p.rowDetail(p.rowOfAction(A::SetMouseModeDesktop)).contains(QChar(0x2713)));
        p.gamepadMoveUp();
        p.gamepadSelect();
        QCOMPARE(actions.back(), A::SetMouseModeGame);
        QVERIFY(p.isMenuVisible());
        p.handleNavigationKey(Qt::Key_Down);
        p.handleNavigationKey(Qt::Key_Return);
        QCOMPARE(actions.back(), A::SetMouseModeDesktop);
        p.gamepadBack();
        QCOMPARE(p.currentTab(), T::Quick);
        QCOMPARE(p.focusedRow(), p.rowOfAction(A::OpenMouseMode));
        QVERIFY(p.rowDetail(p.rowOfAction(A::OpenMouseMode)).contains("Desktop"));
    }
    void streamConfigurationPendingApply()
    {
        OverlayMenuPanel p;
        OverlayMenuPanel::StreamConfigurationState state;
        state.currentCodec = "HEVC 10-bit HDR";
        state.currentHdr = "HDR10/PQ";
        state.currentAudio = "6 channels";
        state.requestedCodec = 0;
        state.requestedHdrMode = 1;
        state.requestedAudioConfig = 0;
        p.updateStreamConfigurationState(state);

        OverlayMenuPanel::StreamConfigurationRequest sent;
        bool applied = false;
        p.setStreamConfigurationCallback([&](const auto& request) {
            sent = request;
            applied = true;
        });

        open(p);
        p.showTab(T::Stream);
        click(p, A::OpenStreamConfiguration);
        QVERIFY(p.rowDetail(p.rowOfAction(A::SelectStreamCodec, "0")).contains("Requested"));
        QVERIFY(p.rowDetail(p.rowOfAction(A::MenuActionMax)).contains("HEVC 10-bit HDR"));
        click(p, A::SelectStreamCodec, "4");
        click(p, A::SelectStreamHdr, "2");
        click(p, A::SelectStreamAudio, "3");
        QVERIFY(p.rowEnabled(p.rowOfAction(A::ApplyStreamConfiguration)));
        QVERIFY(p.rowDetail(p.rowOfAction(A::ApplyStreamConfiguration)).contains("Pending"));
        click(p, A::ApplyStreamConfiguration);
        QVERIFY(applied);
        QCOMPARE(sent.codec, 4);
        QCOMPARE(sent.hdrMode, 2);
        QCOMPARE(sent.audioConfig, 3);
        QVERIFY(!p.isMenuVisible());

        state.pyrowaveSelected = true;
        p.updateStreamConfigurationState(state);
        open(p);
        p.showTab(T::Stream);
        click(p, A::OpenStreamConfiguration);
        QVERIFY(!p.rowEnabled(p.rowOfAction(A::SelectStreamCodec, "0")));
        QVERIFY(p.rowDetail(p.rowOfAction(A::SelectStreamCodec, "0"))
                    .contains("PyroWave remains selected"));

        state.pyrowaveSelected = false;
        state.pending = true;
        state.pendingCodec = 4;
        state.pendingHdrMode = 2;
        state.pendingAudioConfig = 3;
        p.updateStreamConfigurationState(state);
        open(p);
        p.showTab(T::Stream);
        click(p, A::OpenStreamConfiguration);
        QVERIFY(!p.rowEnabled(p.rowOfAction(A::ApplyStreamConfiguration)));
        QVERIFY(p.rowDetail(p.rowOfAction(A::ApplyStreamConfiguration)).contains("Pending"));
        QVERIFY(!p.rowEnabled(p.rowOfAction(A::SelectStreamCodec, "0")));
    }
    void streamConfigurationNarrowLayoutAndFocus()
    {
        for (QSize size : { QSize(320, 240), QSize(640, 360) }) {
            for (int scale : { 100, 125, 150 }) {
                OverlayMenuPanel p;
                OverlayMenuPanel::StreamConfigurationState state;
                state.currentCodec = "HEVC 10-bit HDR";
                state.currentHdr = "HDR10/PQ";
                state.currentAudio = "6 channels";
                state.requestedCodec = 0;
                state.requestedHdrMode = 1;
                state.requestedAudioConfig = 0;
                p.updateStreamConfigurationState(state);
                p.setStyle(OverlayMenu::Layout::Compact, scale, 100);
                open(p, size);
                QVERIFY(p.showTab(T::Stream));
                click(p, A::OpenStreamConfiguration);

                const int codecRow = p.rowOfAction(A::SelectStreamCodec, "4");
                const int hdrRow = p.rowOfAction(A::SelectStreamHdr, "2");
                const int audioRow = p.rowOfAction(A::SelectStreamAudio, "3");
                for (int row :
                     { codecRow, hdrRow, audioRow, p.rowOfAction(A::ApplyStreamConfiguration) }) {
                    QVERIFY(row >= 0);
                    p.ensureRowVisible(row);
                    QVERIFY(p.rowViewportGeometry().contains(p.rowGeometry(row)));
                }

                p.ensureRowVisible(codecRow);
                const QRect codecGeometry = p.rowGeometry(codecRow);
                QTest::mouseClick(&p, Qt::LeftButton, Qt::NoModifier, codecGeometry.center());
                QVERIFY(
                    p.rowDetail(p.rowOfAction(A::SelectStreamCodec, "4")).contains(QChar(0x2713)));
                const int focusedCodecRow = p.rowOfAction(A::SelectStreamCodec, "4");
                QCOMPARE(p.focusedRow(), focusedCodecRow);

                state.pending = true;
                state.pendingCodec = 4;
                state.pendingHdrMode = 2;
                state.pendingAudioConfig = 3;
                p.updateStreamConfigurationState(state);
                QCOMPARE(p.focusedRow(), p.rowOfAction(A::SelectStreamCodec, "4"));
                QVERIFY(p.rowViewportGeometry().contains(p.rowGeometry(p.focusedRow())));
                QVERIFY(!p.rowEnabled(p.rowOfAction(A::ApplyStreamConfiguration)));

                p.showTab(T::Quick);
                p.showTab(T::Stream);
                click(p, A::OpenStreamConfiguration);
                const int pendingApplyRow = p.rowOfAction(A::ApplyStreamConfiguration);
                p.ensureRowVisible(pendingApplyRow);
                QVERIFY(p.rowViewportGeometry().contains(p.rowGeometry(pendingApplyRow)));
                QVERIFY(p.rowDetail(pendingApplyRow).contains("Pending"));
                p.closeMenu();
                QTest::qWait(160);
            }
        }
    }
    void streamQualityLiveCostAndFailureActions()
    {
        OverlayMenuPanel p;
        OverlayMenuPanel::StreamQualityState state;
        state.resolution = QSize(1920, 1080);
        state.fps = 60;
        state.hostSupportsLiveResolution = true;
        state.hostDisplaySize = QSize(1920, 1080);
        p.updateStreamQualityState(state);

        open(p);
        p.showTab(T::Stream);
        click(p, A::OpenStreamQuality);
        QVERIFY(
            p.rowDetail(p.rowOfAction(A::SelectStreamResolution, "2560x1440")).contains("Live"));
        click(p, A::SelectStreamResolution, "2560x1440");
        QVERIFY(p.rowEnabled(p.rowOfAction(A::ApplyStreamQuality)));
        QVERIFY(p.rowDetail(p.rowOfAction(A::ApplyStreamQuality)).contains("Live"));

        state.liveRequestPending = true;
        state.pendingResolution = QSize(2560, 1440);
        p.updateStreamQualityState(state);
        QVERIFY(!p.rowEnabled(p.rowOfAction(A::ApplyStreamQuality)));
        QVERIFY(p.rowDetail(p.rowOfAction(A::ApplyStreamQuality)).contains("Pending"));

        state.liveRequestPending = false;
        state.resultMessage = "Host did not apply 1440p";
        state.canRetryLiveResolution = true;
        state.retryLiveResolutionEnabled = true;
        state.canReconnectStreamQuality = true;
        state.failedResolution = QSize(2560, 1440);
        state.failedFps = 60;
        p.updateStreamQualityState(state);
        QVERIFY(p.rowEnabled(p.rowOfAction(A::RetryLiveResolution)));
        QVERIFY(p.rowEnabled(p.rowOfAction(A::ReconnectStreamQuality)));
        QVERIFY(p.rowDetail(p.rowOfAction(A::ReconnectStreamQuality)).contains("Reconnect"));
        state.retryLiveResolutionEnabled = false;
        p.updateStreamQualityState(state);
        QVERIFY(!p.rowEnabled(p.rowOfAction(A::RetryLiveResolution)));
    }
    void canonicalPinsAndGates()
    {
        OverlayMenuPanel p;
        p.updateMicrophoneState(true);
        p.updateAudioState(true, true);
        p.updateInputState(false, false, true, true, true);
        open(p);
        QVERIFY(p.rowToggleState(p.rowOfAction(A::ToggleMicrophone)));
        QVERIFY(p.rowToggleState(p.rowOfAction(A::ToggleAudioMute)));
        p.showTab(T::Stream);
        QVERIFY(p.rowToggleState(p.rowOfAction(A::ToggleAudioMute)));
        p.showTab(T::Input);
        QVERIFY(!p.rowEnabled(p.rowOfAction(A::UngrabInput)));
        QVERIFY(!p.rowEnabled(p.rowOfAction(A::ToggleCursorHide)));
        QVERIFY(p.rowOfAction(A::ToggleGamepadMouse) < 0);
        p.setGamepadHints(true, GamepadUiStyleXbox, false, {});
        p.updateGamepadMouseState(true);
        QVERIFY(p.rowToggleState(p.rowOfAction(A::ToggleGamepadMouse)));
        p.updateInputState(true, true, false, false, false);
        QVERIFY(p.rowEnabled(p.rowOfAction(A::UngrabInput)));
        QVERIFY(p.rowEnabled(p.rowOfAction(A::ToggleCursorHide)));
        p.updateSendKeys({ { "Test", {}, "keys:A" } }, {});
        QVERIFY(p.rowEnabled(p.rowOfAction(A::UngrabInput)));
        QVERIFY(!p.rowToggleState(p.rowOfAction(A::ToggleCursorHide)));
        p.setGamepadHints(false, GamepadUiStyleXbox, false, {});
        QVERIFY(p.rowOfAction(A::ToggleGamepadMouse) < 0);
        p.showTab(T::Host);
        QVERIFY(p.rowOfAction(A::OpenHostCommands) < 0);
        p.updateHostCommands(true, { { "command", "Command" } });
        QVERIFY(p.rowOfAction(A::OpenHostCommands) >= 0);
        p.updateHostCommands(false, {});
        QVERIFY(p.rowOfAction(A::OpenHostCommands) < 0);
        p.showTab(T::Display);
        QVERIFY(p.rowOfAction(A::MatchResolutionNow) < 0);
        p.updateResolutionMatchState(true, true, false, true);
        QVERIFY(p.rowEnabled(p.rowOfAction(A::MatchResolutionNow)));
        QVERIFY(p.rowToggleState(p.rowOfAction(A::ToggleResolutionMatch)));
        p.updateResolutionMatchState(false, true, false, true);
        QVERIFY(p.rowOfAction(A::MatchResolutionNow) < 0);
        p.showTab(T::Quick);
        click(p, A::OpenMenuSettings);
        click(p, A::OpenQuickPins);
        click(p, A::ToggleQuickPin, QString::number(int(A::ToggleAudioMute)));
        p.gamepadBack();
        p.gamepadBack();
        QCOMPARE(p.currentTab(), T::Quick);
        QVERIFY(p.rowOfAction(A::ToggleAudioMute) < 0);
        click(p, A::OpenMenuSettings);
        click(p, A::OpenQuickPins);
        click(p, A::ToggleQuickPin, QString::number(int(A::ToggleAudioMute)));
        p.gamepadBack();
        p.gamepadBack();
        QVERIFY(p.rowToggleState(p.rowOfAction(A::ToggleAudioMute)));
    }
    void quitAndRelease()
    {
        OverlayMenuPanel p;
        std::vector<A> actions;
        p.setActionCallback([&](A a) { actions.push_back(a); });
        open(p);
        click(p, A::QuitAndExit);
        QVERIFY(p.isMenuVisible());
        QVERIFY(actions.empty());
        QVERIFY(p.rowDetail(p.rowOfAction(A::QuitAndExit)).contains("again"));
        QTest::qWait(3100);
        QVERIFY(!p.rowDetail(p.rowOfAction(A::QuitAndExit)).contains("again"));
        click(p, A::QuitAndExit);
        p.closeMenu();
        QTest::qWait(160);
        open(p);
        click(p, A::QuitAndExit);
        QVERIFY(actions.empty());
        click(p, A::QuitAndExit);
        QVERIFY(!p.isMenuVisible());
        QCOMPARE(actions.back(), A::QuitAndExit);
        QTest::qWait(160);
        open(p);
        p.showTab(T::Input);
        p.updateInputState(true, false, true, false, false);
        click(p, A::UngrabInput);
        QVERIFY(p.isMenuVisible());
        p.updateInputState(true, true, true, false, false);
        click(p, A::UngrabInput);
        QVERIFY(!p.isMenuVisible());
        QCOMPARE(actions.back(), A::UngrabInput);
        QTest::qWait(160);
        open(p);
        p.showTab(T::Quick);
        click(p, A::Quit);
        QVERIFY(!p.isMenuVisible());
        QCOMPARE(actions.back(), A::Quit);
    }
    void shortcutEditor()
    {
        OverlayMenuPanel p;
        int reloads = 0;
        p.setShortcutProfileCallback([&](const ShortcutProfile&) { ++reloads; });
        open(p, QSize(640, 360));
        p.showTab(T::Host);
        click(p, A::OpenMenuSettings);
        click(p, A::OpenShortcuts);
        click(p, A::OpenShortcutAction, ShortcutActions::Minimize);
        const auto original =
            ShortcutProfile::loadFromSettings().bindings(ShortcutActions::Minimize);
        click(p, A::CaptureShortcut, "0");
        QVERIFY(p.isCapturingShortcut());
        QVERIFY(p.handleNavigationKey(Qt::Key_A));
        QVERIFY(!p.isCapturingShortcut());
        QCOMPARE(ShortcutProfile::loadFromSettings().bindings(ShortcutActions::Minimize), original);
        QVERIFY(p.rowDetail(p.rowOfAction(A::MenuActionMax)).contains("Invalid"));
        click(p, A::CaptureShortcut, "0");
        const auto quit = ShortcutProfile::loadFromSettings().bindings(ShortcutActions::Quit)[0];
        p.handleShortcutKey(quit.scancode,
                            Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier, true);
        QVERIFY(p.isCapturingShortcut());
        QVERIFY(p.handleShortcutKey(quit.scancode, Qt::NoModifier, false));
        QVERIFY(!p.isCapturingShortcut());
        QVERIFY(p.rowDetail(p.rowOfAction(A::MenuActionMax)).contains("Conflicts"));
        QCOMPARE(ShortcutProfile::loadFromSettings().bindings(ShortcutActions::Minimize), original);
        click(p, A::CaptureShortcut, "0");
        p.handleNavigationKey(Qt::Key_F23);
        QCOMPARE(ShortcutProfile::loadFromSettings().bindings(ShortcutActions::Minimize)[0],
                 ShortcutBinding(0, ShortcutKeys::F24 - 1));
        click(p, A::CaptureShortcut, "1");
        p.handleNavigationKey(Qt::Key_F22);
        QCOMPARE(ShortcutProfile::loadFromSettings().bindings(ShortcutActions::Minimize)[1],
                 ShortcutBinding(0, ShortcutKeys::F24 - 2));
        QVERIFY(p.rowOfAction(A::CaptureShortcut, "2") < 0);
        click(p, A::ClearShortcut, "0");
        QVERIFY(
            ShortcutProfile::loadFromSettings().bindings(ShortcutActions::Minimize)[0].isNull());
        click(p, A::ResetShortcut);
        QCOMPARE(ShortcutProfile::loadFromSettings().bindings(ShortcutActions::Minimize), original);
        QVERIFY(reloads >= 5);
        p.gamepadBack();
        click(p, A::OpenShortcutAction, ShortcutActions::RightAltMouseRelease);
        QVERIFY(p.rowOfAction(A::CaptureShortcut) < 0);
        click(p, A::ToggleRightAltRelease);
        QVERIFY(ShortcutProfile::loadFromSettings().rightAltReleaseEnabled());
        QCOMPARE(
            ShortcutProfile::loadFromSettings().bindings(ShortcutActions::RightAltMouseRelease)[0],
            ShortcutBinding(0, ShortcutKeys::RightAlt));
        click(p, A::ToggleRightAltRelease);
        QVERIFY(!ShortcutProfile::loadFromSettings().rightAltReleaseEnabled());
        p.gamepadBack();
        click(p, A::OpenShortcutAction, ShortcutActions::Minimize);
        click(p, A::CaptureShortcut, "1");
        p.handleNavigationKey(Qt::Key_Escape);
        QVERIFY(!p.isCapturingShortcut());
        QVERIFY(p.isMenuVisible());
        p.gamepadBack();
        p.gamepadBack();
        p.gamepadBack();
        QCOMPARE(p.currentTab(), T::Host);
        QCOMPARE(p.focusedRow(), -4);
    }
    void pinsMirrorsAndPersistence()
    {
        OverlayMenuPanel p;
        p.updateStatsOverlayState(StatsOverlay::Preset::Detailed, true, false, 70);
        p.updateWindowModeState(OverlayMenuPanel::WindowMode::Borderless, true, true, true);
        p.updateMicrophoneState(true);
        open(p);
        QVERIFY(p.rowDetail(p.rowOfAction(A::OpenStatsOverlay)).contains("Detailed"));
        QVERIFY(p.rowDetail(p.rowOfAction(A::OpenWindowMode)).contains("Borderless"));
        bool muted = false;
        int toggles = 0;
        p.setActionCallback([&](A a) {
            if (a == A::ToggleAudioMute) {
                ++toggles;
                muted = !muted;
                p.updateAudioState(muted, false);
            }
        });
        click(p, A::ToggleAudioMute);
        QVERIFY(p.isMenuVisible());
        QVERIFY(p.rowToggleState(p.rowOfAction(A::ToggleAudioMute)));
        p.showTab(T::Stream);
        QVERIFY(p.rowToggleState(p.rowOfAction(A::ToggleAudioMute)));
        click(p, A::ToggleAudioMute);
        p.showTab(T::Quick);
        QVERIFY(!p.rowToggleState(p.rowOfAction(A::ToggleAudioMute)));
        QCOMPARE(toggles, 2);
        p.updateSendKeys({ { "Keys", {}, "keys:A" } }, {});
        QVERIFY(p.rowToggleState(p.rowOfAction(A::ToggleMicrophone)));
        QVERIFY(p.rowDetail(p.rowOfAction(A::OpenWindowMode)).contains("Borderless"));
        click(p, A::OpenMenuSettings);
        click(p, A::OpenQuickPins);
        click(p, A::ToggleQuickPin, QString::number(int(A::ToggleMicrophone)));
        OverlayMenuPanel second;
        second.updateMicrophoneState(true);
        open(second);
        QVERIFY(second.rowOfAction(A::ToggleMicrophone) < 0);
        second.showTab(T::Stream);
        QVERIFY(second.rowToggleState(second.rowOfAction(A::ToggleMicrophone)));
    }
    void scrollWheelAndSlider()
    {
        OverlayMenuPanel p;
        open(p, QSize(640, 360));
        p.showTab(T::Input);
        const int paste = p.rowOfAction(A::PasteText);
        QVERIFY(!p.rowViewportGeometry().contains(p.rowGeometry(paste)));
        const QPoint point = p.rowViewportGeometry().center();
        QWheelEvent wheel(point, p.mapToGlobal(point), {}, QPoint(0, -600), Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
        QGuiApplication::sendEvent(&p, &wheel);
        QVERIFY(p.rowViewportGeometry().contains(p.rowGeometry(paste)));
        p.showTab(T::Stream);
        click(p, A::OpenBitrate);
        const int old = p.rowDetail(p.rowOfAction(A::SetBitrate, "10000")).size();
        Q_UNUSED(old);
        int bitrate = 0;
        p.setBitrateChangeCallback([&](int value) { bitrate = value; });
        const QPoint slider = p.rowGeometry(0).center();
        QWheelEvent scrub(slider, p.mapToGlobal(slider), {}, QPoint(0, 120), Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
        QGuiApplication::sendEvent(&p, &scrub);
        QTest::qWait(500);
        QCOMPARE(bitrate, 11000);
        p.gamepadBack();
        QCOMPARE(p.currentTab(), T::Stream);
        QCOMPARE(p.focusedRow(), p.rowOfAction(A::OpenBitrate));
    }
    void boundedLayoutsAndFocus()
    {
        OverlayMenuPanel p;
        p.updateSendKeys({}, {});
        open(p, QSize(640, 360));
        for (auto layout : { OverlayMenu::Layout::Compact, OverlayMenu::Layout::Comfortable,
                             OverlayMenu::Layout::SidePanel }) {
            for (int scale : { 100, 125, 150 }) {
                p.setStyle(layout, scale, 100);
                QCOMPARE(p.effectiveScalePercent(), scale);
                for (T tab : p.visibleTabs()) {
                    QVERIFY(p.rowViewportGeometry().width() > 0);
                    QVERIFY(p.rowViewportGeometry().height() >= p.rowGeometry(0).height());
                    QVERIFY(QRect(QPoint(), p.size()).contains(p.tabGeometry(tab)));
                    p.showTab(tab);
                    bool reachedGear = false;
                    for (int i = 0; i < 32; ++i) {
                        p.gamepadMoveDown();
                        if (p.focusedRow() == -4)
                            reachedGear = true;
                        else {
                            QVERIFY(p.rowEnabled(p.focusedRow()));
                            QVERIFY(
                                p.rowViewportGeometry().contains(p.rowGeometry(p.focusedRow())));
                        }
                    }
                    QVERIFY(reachedGear);
                }
            }
        }
        p.showTab(T::Input);
        click(p, A::OpenSendKeys);
        for (int i = 0; i < 40; ++i) {
            p.gamepadMoveDown();
            QVERIFY(p.focusedRow() == -4 || p.focusedRow() == -2);
        }
        p.gamepadBack();
        QCOMPARE(p.currentTab(), T::Input);
        QCOMPARE(p.focusedRow(), p.rowOfAction(A::OpenSendKeys));
    }
    void releaseDetailFits()
    {
        OverlayMenuPanel p;
        p.updateInputState(true, true, true, false, false);
        for (QSize size : { QSize(1280, 900), QSize(640, 360) }) {
            open(p, size);
            p.showTab(T::Input);
            const int row = p.rowOfAction(A::UngrabInput);
            p.ensureRowVisible(row);
            QVERIFY(p.rowDetail(row).contains("Click the stream to recapture"));
            QVERIFY(p.rowDetail(row).contains("Ctrl+Alt+Shift+Z"));
            QVERIFY2(!p.clippedTexts().contains("Release mouse"), "Release copy must fit exactly");
            QVERIFY(p.rowViewportGeometry().contains(p.rowGeometry(row)));
            QCOMPARE(p.effectiveScalePercent(), 100);
            const QString artifacts = qEnvironmentVariable("MENU_PHASE1_ARTIFACTS");
            if (!artifacts.isEmpty())
                p.renderImage().save(artifacts + QString("/release-fit-%1-dpr%2.png")
                                                     .arg(size.width())
                                                     .arg(p.devicePixelRatio()));
            p.closeMenu();
            QTest::qWait(160);
        }
    }
    void bitrateTrackEndpoints()
    {
        OverlayMenuPanel p;
        int bitrate = 0;
        p.setBitrateChangeCallback([&](int value) { bitrate = value; });
        open(p);
        p.showTab(T::Stream);
        QVERIFY(p.showLevel(A::OpenBitrate));
        const QRect track = p.bitrateTrackGeometry();
        QVERIFY(!track.isEmpty());
        QVERIFY(p.rowGeometry(0).contains(track));
        QTest::mousePress(&p, Qt::LeftButton, Qt::NoModifier,
                          QPoint(track.left(), track.center().y()));
        QTest::mouseRelease(&p, Qt::LeftButton, Qt::NoModifier,
                            QPoint(track.left(), track.center().y()));
        QTest::qWait(500);
        QCOMPARE(bitrate, 500);
        const QPoint maximum(track.x() + track.width(), track.center().y());
        QTest::mousePress(&p, Qt::LeftButton, Qt::NoModifier, maximum);
        QTest::mouseRelease(&p, Qt::LeftButton, Qt::NoModifier, maximum);
        QTest::qWait(500);
        QCOMPARE(bitrate, 800000);
    }
    void canonicalProgrammaticLevels()
    {
        OverlayMenuPanel p;
        p.updateRemoteUsbState(true, OverlayMenuPanel::RemoteUsbState::Available,
                               { { "usb", "USB controller", "Available", true } }, {}, "Available");
        open(p);
        QCOMPARE(OverlayMenuPanel::tabForAction(A::OpenStreamQuality), T::Stream);
        QCOMPARE(OverlayMenuPanel::tabForAction(A::OpenBitrate), T::Stream);
        QCOMPARE(OverlayMenuPanel::tabForAction(A::OpenRemoteUsbDevices), T::Host);
        QVERIFY(p.showLevel(A::OpenRemoteUsbDevices));
        p.updateRemoteUsbState(true, OverlayMenuPanel::RemoteUsbState::Open,
                               { { "usb", "USB controller", "Connected", true } }, "usb",
                               "Connected");
        p.gamepadBack();
        QCOMPARE(p.currentTab(), OverlayMenuPanel::tabForAction(A::OpenRemoteUsbDevices));
        QCOMPARE(p.focusedRow(), p.rowOfAction(A::OpenRemoteUsbDevices));
    }
    void scrollAndFacts()
    {
        OverlayMenuPanel p;
        p.setConnectionInfo({ { "Host", "Gaming workstation" },
                              { "Host version", "2026.1031.143000" },
                              { "App", "Steam" },
                              { "Codec", "AV1" },
                              { "HDR", "Off" },
                              { "Resolution", "2560 x 1440" },
                              { "Frame rate", "120 FPS" },
                              { "Audio", "2 channels" },
                              { "Decoder", "D3D11VA" },
                              { "Renderer", "Direct3D 11" },
                              { "Encoder", "Not reported" } });
        p.setSessionStatsProvider([] {
            return std::vector<std::pair<QString, QString>>{
                { "Duration", "0:12:34" },
                { "Average FPS", "119.8" },
                { "1% / 0.1% low", "115.2 / 110.4 FPS" },
                { "Frame time, 99th percentile", "8.68 ms" },
                { "Network loss", "0.02% (18 frames)" },
                { "Pacer drops", "0.01% (9 of 90480 eligible frames)" },
                { "Bitrate", "98.1 Mbps" },
                { "Latency", "12.4 ms" },
                { "FEC frames", "143 recovered, 18 unrecoverable" }
            };
        });
        open(p, QSize(640, 360));
        QCOMPARE(p.effectiveScalePercent(), 100);
        p.showTab(T::Input);
        for (int i = 0; i < 40; ++i) {
            p.gamepadMoveDown();
            if (p.focusedRow() >= 0)
                QVERIFY(p.rowViewportGeometry().contains(p.rowGeometry(p.focusedRow())));
        }
        p.showTab(T::Session);
        QVERIFY(p.rowOfAction(A::OpenSessionStats) >= 0);
        for (int i = 0; i < 40; ++i)
            p.gamepadMoveDown();
        QVERIFY(p.isMenuVisible());
        p.showTab(T::Display);
        p.gamepadMoveUp();
        p.gamepadSelect();
        QVERIFY(!p.isAtTabRoot());
        p.gamepadBack();
        QCOMPARE(p.currentTab(), T::Display);
        const QString dir = qEnvironmentVariable("MENU_PHASE1_ARTIFACTS");
        if (!dir.isEmpty()) {
            QDir().mkpath(dir);
            p.updateResolutionMatchState(true, true, false, true);
            p.updateStreamQualityState({ QSize(1920, 1080), 120, QSize(2560, 1440), 144 });
            p.updateInputState(true, true, true, false, true);
            p.updateMicrophoneState(true);
            p.updateStatsOverlayState(StatsOverlay::Preset::Compact, true, false, 70);
            p.updateHostCommands(true, { { "test", "Open Steam" } });
            p.updateFileMappingState(OverlayMenuPanel::FileMappingState::Available, "Available");
            p.updateSendKeys({ { "Ctrl+Alt+Delete", "Ctrl+Alt+Delete", "special:ctrl-alt-del" } },
                             {});
            p.setGamepadHints(true, GamepadUiStyleXbox, false, "Back+Start+LB+RB");
            p.updateGamepadMouseState(false);
            for (QSize size : { QSize(1280, 900), QSize(640, 360) }) {
                p.closeMenu();
                QTest::qWait(160);
                open(p, size);
                const qreal dpr = p.devicePixelRatio();
                const QSize cell = QSize(p.width() + 24, p.height() + 48);
                QImage composite(QSize(cell.width() * 3, cell.height() * 2) * dpr,
                                 QImage::Format_ARGB32_Premultiplied);
                composite.setDevicePixelRatio(dpr);
                composite.fill(QColor("#303841"));
                QPainter painter(&composite);
                int index = 0;
                for (T tab : { T::Quick, T::Stream, T::Display, T::Input, T::Host, T::Session }) {
                    p.showTab(tab);
                    QTest::qWait(160);
                    const QImage panel = p.renderImage();
                    panel.save(dir + QString("/%1-%2-dpr%3.png")
                                         .arg(size.width())
                                         .arg(OverlayMenuPanel::tabTitle(tab))
                                         .arg(dpr));
                    const QPoint origin((index % 3) * cell.width() + 12,
                                        (index / 3) * cell.height() + 32);
                    painter.drawImage(origin, panel);
                    painter.setPen(Qt::white);
                    painter.drawText(origin + QPoint(0, -10), OverlayMenuPanel::tabTitle(tab));
                    ++index;
                }
                painter.end();
                composite.save(dir + QString("/composite-%1-dpr%2.png").arg(size.width()).arg(dpr));
            }
        }
        // Exercise and render every actual Session row, not just the first viewport.
        for (QSize size : { QSize(1280, 900), QSize(640, 360) }) {
            p.closeMenu();
            QTest::qWait(160);
            open(p, size);
            p.showTab(T::Session);
            const QStringList clipped = p.clippedTexts();
            // clippedTexts checks every menu level. Validate the complete
            // production Session fixture, not unrelated tabs' shortcut widths.
            for (const QString& label : { QString("Host"),
                                          QString("Host version"),
                                          QString("App"),
                                          QString("Codec"),
                                          QString("HDR"),
                                          QString("Resolution"),
                                          QString("Frame rate"),
                                          QString("Audio"),
                                          QString("Decoder"),
                                          QString("Renderer"),
                                          QString("Encoder"),
                                          QString("Duration"),
                                          QString("Average FPS"),
                                          QString("1% / 0.1% low"),
                                          QString("Frame time, 99th percentile"),
                                          QString("Network loss"),
                                          QString("Pacer drops"),
                                          QString("Bitrate"),
                                          QString("Latency"),
                                          QString("FEC frames") })
                QVERIFY2(!clipped.contains(label), qPrintable(label));
            const qreal dpr = p.devicePixelRatio();
            std::vector<QImage> pages;
            for (int row = 0; row < 23; ++row) {
                p.ensureRowVisible(row);
                QVERIFY(p.rowViewportGeometry().contains(p.rowGeometry(row)));
                if (row == 0 || row == 6 || row == 12 || row == 18 || row == 22)
                    pages.push_back(p.renderImage());
            }
            if (!dir.isEmpty()) {
                const QSize cell(p.width() + 24, p.height() + 36);
                QImage image(QSize(cell.width() * int(pages.size()), cell.height()) * dpr,
                             QImage::Format_ARGB32_Premultiplied);
                image.setDevicePixelRatio(dpr);
                image.fill(QColor("#303841"));
                QPainter painter(&image);
                for (int i = 0; i < int(pages.size()); ++i) {
                    painter.setPen(Qt::white);
                    painter.drawText(QPoint(i * cell.width() + 12, 20),
                                     QString("Session viewport %1").arg(i + 1));
                    painter.drawImage(QPoint(i * cell.width() + 12, 28), pages[i]);
                }
                painter.end();
                image.save(dir +
                           QString("/session-content-%1-dpr%2.png").arg(size.width()).arg(dpr));
            }
        }
    }
};
int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName("MoonlightPanelPhase1Tests");
    QCoreApplication::setApplicationName("Panel");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QDir::currentPath() + "\\settings");
    QSettings().clear();
    for (const QString& font : { QString("Manrope-Regular.ttf"), QString("Manrope-SemiBold.ttf"),
                                 QString("DMMono-Regular.ttf") })
        QFontDatabase::addApplicationFont(":/fonts/" + font);
    Phase1 tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "main.moc"
