#include "../../app/streaming/video/overlaymenupanel.h"
#include <QGuiApplication>
#include <QFontDatabase>
#include <QCursor>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QTest>
#include <optional>

// Native preview of the production panel, with simulated USB status changes.
class MenuPreview : public QRasterWindow {
public:
    explicit MenuPreview(OverlayMenuPanel &panel) : menu(panel) {
        setTitle(QStringLiteral("USB Menu Interaction Check"));
        setGeometry(150, 100, 900, 700);
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(QRect(QPoint(), size()), QColor(35, 38, 45));
        painter.setPen(Qt::white);
        painter.drawText(QRect(30, 30, 800, 100), Qt::AlignLeft,
            QStringLiteral("USB menu preview - simulated device states\nClick outside to close; click the background to reopen.\nEscape: back, then close."));
    }
    void mouseReleaseEvent(QMouseEvent *) override {
        if (menu.isMenuVisible()) menu.dismissOnOutsideClick(QCursor::pos());
        else menu.showAtCursor(x(), y(), width(), height(), position() + QPoint(250, 150), false);
    }
    void keyReleaseEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Escape) menu.gamepadBack();
    }
private:
    OverlayMenuPanel &menu;
};

using MenuAction = OverlayMenuPanel::MenuAction;
using MenuTab = OverlayMenuPanel::MenuTab;

// Rows and tabs are found by identity, never by pixel offsets: tabs and
// other branches' entries move them.
static QPoint rowCenter(const OverlayMenuPanel &panel, int row)
{
    const QRect rect = panel.rowGeometry(row);
    if (rect.isEmpty())
        qFatal("row %d is not on the shown level", row);
    return QPoint(140, rect.center().y());
}

static int rowOf(const OverlayMenuPanel &panel, MenuAction action, const QString &payload = {})
{
    const int row = panel.rowOfAction(action, payload);
    if (row < 0)
        qFatal("action %d is not on the shown level", int(action));
    return row;
}

static void waitClosed(const OverlayMenuPanel &panel)
{
    for (int i = 0; i < 40 && panel.isVisible(); i++)
        QTest::qWait(50);
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Manrope-Regular.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Manrope-SemiBold.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/DMMono-Regular.ttf"));
    OverlayMenuPanel panel;
    QString selected;
    int releaseCount = 0;
    const std::vector<OverlayMenuPanel::RemoteUsbDevice> devices {
        {QStringLiteral("1-1"), QStringLiteral("Phone"), QStringLiteral("18D1:4EE7"), true},
        {QStringLiteral("1-2"), QStringLiteral("Keyboard"), QStringLiteral("046D:B34D"), true}
    };
    panel.setRemoteUsbDeviceCallback([&](const QString &id) {
        selected = id;
        panel.updateRemoteUsbState(true, OverlayMenuPanel::RemoteUsbState::Opening,
                                   devices, id, QStringLiteral("Connecting"));
    });
    panel.setRemoteUsbReleaseCallback([&] {
        ++releaseCount;
        panel.updateRemoteUsbState(true, OverlayMenuPanel::RemoteUsbState::Stopping,
                                   devices, selected, QStringLiteral("Releasing"));
    });
    panel.updateRemoteUsbState(true, OverlayMenuPanel::RemoteUsbState::Available,
                               devices, {}, QStringLiteral("1 available"));
    if (app.arguments().contains(QStringLiteral("--interactive"))) {
        MenuPreview preview(panel);
        preview.show();
        panel.setTransientParent(&preview);
        panel.setTitle(QStringLiteral("USB Menu Interaction Check"));
        panel.setRemoteUsbDeviceCallback([&](const QString &id) {
            selected = id;
            panel.updateRemoteUsbState(true, OverlayMenuPanel::RemoteUsbState::Opening,
                                       devices, id, QStringLiteral("Connecting"));
            QTimer::singleShot(1200, &panel, [&] {
                panel.updateRemoteUsbState(true, OverlayMenuPanel::RemoteUsbState::Open,
                                           devices, selected, QStringLiteral("Connected"));
            });
        });
        panel.setRemoteUsbReleaseCallback([&] {
            panel.updateRemoteUsbState(true, OverlayMenuPanel::RemoteUsbState::Stopping,
                                       devices, selected, QStringLiteral("Releasing"));
            QTimer::singleShot(1200, &panel, [&] {
                panel.updateRemoteUsbState(true, OverlayMenuPanel::RemoteUsbState::Available,
                                           devices, {}, QStringLiteral("1 available"));
            });
        });
        panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 250), false);
        return app.exec();
    }
    // Offscreen cursor remains outside this panel. Explicitly entering a
    // shorter submenu must not trigger the pointer-leave dismissal timer.
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400), true);
    QTest::qWait(20);
    if (panel.currentTab() != MenuTab::Quick || !panel.isAtTabRoot())
        qFatal("menu did not open on the Quick tab");
    const int initialHeight = panel.height();
    if (!panel.showLevel(MenuAction::OpenRemoteUsbDevices))
        qFatal("USB submenu missing");
    if (panel.height() >= initialHeight) qFatal("USB submenu did not open");
    QTest::qWait(700);
    if (!panel.isVisible()) qFatal("submenu closed while moving toward its devices");
    panel.updateRemoteUsbState(true, OverlayMenuPanel::RemoteUsbState::Available,
                               devices, {}, QStringLiteral("1 available"));
    QTest::qWait(700);
    if (!panel.isVisible()) qFatal("device refresh dismissed submenu");
    if (panel.isAtTabRoot())
        qFatal("device refresh left the USB submenu");
    // Device rows follow the device list order.
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, rowCenter(panel, 0));
    if (selected != QStringLiteral("1-1")) qFatal("device selection was not dispatched");
    QTest::qWait(220);
    if (!panel.isVisible()) qFatal("connecting status dismissed menu");
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, rowCenter(panel, 1));
    if (selected != QStringLiteral("1-1") || releaseCount != 0)
        qFatal("connecting state allowed switching to another device");
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, rowCenter(panel, 0));
    if (releaseCount != 1 || !panel.isVisible()) qFatal("connecting device cannot be cancelled");
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, rowCenter(panel, 0));
    if (releaseCount != 1) qFatal("stopping device allowed duplicate release");
    panel.updateRemoteUsbState(true, OverlayMenuPanel::RemoteUsbState::Open,
                               devices, selected, QStringLiteral("Connected"));
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, rowCenter(panel, 0));
    if (releaseCount != 2 || !panel.isVisible()) qFatal("release did not keep status visible");
    panel.updateRemoteUsbState(true, OverlayMenuPanel::RemoteUsbState::Available,
                               devices, {}, QStringLiteral("1 available"));
    panel.gamepadBack();
    QTest::qWait(700);
    if (!panel.isVisible() || panel.height() != initialHeight) qFatal("back lost active interaction");
    // Back returns to the tab holding the USB entry, not to the first tab.
    if (!panel.isAtTabRoot() ||
        panel.currentTab() != OverlayMenuPanel::tabForAction(MenuAction::OpenRemoteUsbDevices))
        qFatal("back did not return to the USB entry's tab");
    panel.dismissOnOutsideClick(panel.geometry().center());
    if (!panel.isMenuVisible()) qFatal("inside click dismissed menu");
    panel.dismissOnOutsideClick(QPoint(-100, -100));
    // The close animation needs a beat; tolerate slow CI timers.
    waitClosed(panel);
    if (panel.isVisible()) qFatal("outside click failed to dismiss menu");
    // Reopening restores transient pointer-triggered behavior.
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400), true);
    QTest::qWait(700);
    if (panel.isVisible()) qFatal("fresh pointer-triggered menu lost auto-dismiss");
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400), false);
    QTest::qWait(240);
    panel.gamepadBack();
    QTest::qWait(220);
    if (panel.isVisible()) qFatal("back at top level failed to close menu");

    // --- Bitrate scrubber: preset tap, debounced scrub, wheel, flush ---
    int bitrateCommits = 0;
    int lastBitrate = 0;
    panel.setBitrateChangeCallback([&](int kbps) {
        lastBitrate = kbps;
        ++bitrateCommits;
    });
    panel.updateBitrateState(10000);
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400), false);
    QTest::qWait(240);
    // Enter Bitrate from its entry in the Display tab, then tap 5 Mbps.
    if (!panel.showTab(MenuTab::Display))
        qFatal("Display tab missing");
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier,
                      rowCenter(panel, rowOf(panel, MenuAction::OpenBitrate)));
    if (panel.isAtTabRoot())
        qFatal("Bitrate entry did not open its level");
    QTest::mouseClick(
        &panel, Qt::LeftButton, Qt::NoModifier,
        rowCenter(panel, rowOf(panel, MenuAction::SetBitrate, QStringLiteral("5000"))));
    if (lastBitrate != 5000 || bitrateCommits != 1) qFatal("preset tap did not commit");
    if (!panel.isMenuVisible()) qFatal("preset tap closed the menu");
    // D-pad right scrubs upward (not a tab switch inside the slider's level)
    // but must wait out the commit debounce.
    panel.gamepadMoveHorizontal(1);
    if (panel.isAtTabRoot())
        qFatal("d-pad right in the slider level switched tabs");
    if (bitrateCommits != 1 || lastBitrate != 5000) qFatal("scrub committed before debounce");
    QTest::qWait(600);
    if (bitrateCommits != 2 || lastBitrate <= 5000) qFatal("scrub never committed");
    const int afterScrub = lastBitrate;
    // The scrubber is the level's first row.
    const QPoint sliderRow = rowCenter(panel, 0);
    // One wheel notch on the slider row steps again; A flushes immediately.
    QWheelEvent wheelUp(QPointF(sliderRow), QPointF(), QPoint(), QPoint(0, 120), Qt::NoButton,
                        Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&panel, &wheelUp);
    panel.gamepadSelect();
    if (bitrateCommits != 3 || lastBitrate <= afterScrub) qFatal("wheel step not flushed");
    if (!panel.isMenuVisible()) qFatal("slider interaction closed the menu");
    // Saturating the wheel must clamp at the Sunshine /bitrate cap (800 Mbps).
    for (int i = 0; i < 250; i++) {
        QWheelEvent wheelMax(QPointF(sliderRow), QPointF(), QPoint(), QPoint(0, 120), Qt::NoButton,
                             Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&panel, &wheelMax);
    }
    panel.gamepadSelect();
    if (lastBitrate != 800000) qFatal("scrubber did not clamp at 800000 Kbps");
    // Track press scrubs to the click position (window → content coords):
    // near the left end lands near the minimum, near the right end at the cap.
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, QPoint(8 + 102 + 4, sliderRow.y()));
    panel.gamepadSelect();
    if (bitrateCommits != 5 || lastBitrate > 2000)
        qFatal("track press near minimum failed: commits=%d last=%d", bitrateCommits, lastBitrate);
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, QPoint(8 + 252, sliderRow.y()));
    panel.gamepadSelect();
    if (bitrateCommits != 6 || lastBitrate != 800000)
        qFatal("track press near maximum failed: commits=%d last=%d", bitrateCommits, lastBitrate);
    panel.dismissOnOutsideClick(QPoint(-100, -100));
    waitClosed(panel);
    if (panel.isVisible()) qFatal("outside click after slider use failed to dismiss");

    // --- Host actions: Send Keys and Foundation Sunshine host commands ---
    OverlayMenuPanel::MenuAction payloadAction = OverlayMenuPanel::MenuAction::MenuActionMax;
    QString payloadValue;
    panel.setPayloadActionCallback(
        [&](OverlayMenuPanel::MenuAction action, const QString &payload) {
            payloadAction = action;
            payloadValue = payload;
        });
    panel.updateSendKeys(
        { { QStringLiteral("Start menu"), QStringLiteral("Win"), QStringLiteral("special:win") } },
        { { QStringLiteral("Save all"), QStringLiteral("Ctrl+Shift+S"),
            QStringLiteral("keys:ctrl+shift+S") } });
    panel.updateHostCommands(false, {});
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400), false);
    QTest::qWait(240);
    // The last tab of the session is remembered across close and reopen.
    if (panel.currentTab() != MenuTab::Display || !panel.isAtTabRoot())
        qFatal("reopened menu forgot the last tab");
    if (panel.showHostCommandsLevel())
        qFatal("host commands opened without host support");
    if (!panel.showTab(MenuTab::Host) || panel.rowOfAction(MenuAction::OpenHostCommands) >= 0)
        qFatal("Host Commands entry shown without host support");
    // Open Send Keys directly; its second row is the macro.
    if (!panel.showSendKeysLevel())
        qFatal("Send Keys level did not open");
    if (panel.currentTab() != OverlayMenuPanel::tabForAction(MenuAction::OpenSendKeys))
        qFatal("Send Keys level is not under its entry's tab");
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier,
                      rowCenter(panel, rowOf(panel, MenuAction::SendHostKeys,
                                             QStringLiteral("keys:ctrl+shift+S"))));
    if (payloadAction != OverlayMenuPanel::MenuAction::SendHostKeys ||
        payloadValue != QStringLiteral("keys:ctrl+shift+S"))
        qFatal("macro row did not send its keys");
    waitClosed(panel);
    if (panel.isVisible())
        qFatal("sending keys did not close the menu");

    // A supporting host without commands gets the entry and a setup hint.
    panel.updateHostCommands(true, {});
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400), false);
    QTest::qWait(240);
    if (!panel.showTab(MenuTab::Host))
        qFatal("Host tab missing");
    const int hostCommandsRow = panel.rowOfAction(MenuAction::OpenHostCommands);
    if (hostCommandsRow < 0)
        qFatal("Host Commands entry missing");
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, rowCenter(panel, hostCommandsRow));
    if (panel.isAtTabRoot())
        qFatal("Host Commands level did not open");
    payloadValue.clear();
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, rowCenter(panel, 0));
    if (!payloadValue.isEmpty() || !panel.isMenuVisible())
        qFatal("setup hint row was clickable");
    // A refresh keeps the open level, and a command row reports its id.
    panel.updateHostCommands(true, { { QStringLiteral("7"), QStringLiteral("Restart Steam") } });
    QTest::mouseClick(
        &panel, Qt::LeftButton, Qt::NoModifier,
        rowCenter(panel, rowOf(panel, MenuAction::RunHostCommand, QStringLiteral("7"))));
    if (payloadAction != OverlayMenuPanel::MenuAction::RunHostCommand ||
        payloadValue != QStringLiteral("7"))
        qFatal("host command row did not dispatch its id");
    waitClosed(panel);
    if (panel.isVisible())
        qFatal("running a host command did not close the menu");

    // --- Tabs: assignment table, hiding, switching by gamepad/keyboard/mouse ---
    if (OverlayMenuPanel::tabForAction(MenuAction::ToggleStatsOverlay) != MenuTab::Quick ||
        OverlayMenuPanel::tabForAction(MenuAction::Quit) != MenuTab::Quick ||
        OverlayMenuPanel::tabForAction(MenuAction::OpenStreamQuality) != MenuTab::Display ||
        OverlayMenuPanel::tabForAction(MenuAction::OpenStatsOverlay) != MenuTab::Display ||
        OverlayMenuPanel::tabForAction(MenuAction::OpenBitrate) != MenuTab::Display ||
        OverlayMenuPanel::tabForAction(MenuAction::OpenSendKeys) != MenuTab::Input ||
        OverlayMenuPanel::tabForAction(MenuAction::OpenHostCommands) != MenuTab::Host ||
        OverlayMenuPanel::tabForAction(MenuAction::OpenSessionStats) != MenuTab::Session ||
        OverlayMenuPanel::tabForAction(MenuAction::SetBitrate) != MenuTab::Quick)
        qFatal("tab assignment table changed unexpectedly");
    int actionCount = 0;
    OverlayMenuPanel::MenuAction lastAction = OverlayMenuPanel::MenuAction::MenuActionMax;
    panel.setActionCallback([&](OverlayMenuPanel::MenuAction action) {
        lastAction = action;
        ++actionCount;
    });
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400), false);
    QTest::qWait(240);
    // Every tab has entries; Session holds the live totals entry.
    const std::vector<MenuTab> expectedTabs{ MenuTab::Quick, MenuTab::Display, MenuTab::Input,
                                             MenuTab::Host, MenuTab::Session };
    if (panel.visibleTabs() != expectedTabs)
        qFatal("unexpected visible tabs");
    if (panel.tabGeometry(MenuTab::Session).isEmpty())
        qFatal("Session tab is not drawn");
    if (panel.currentTab() != MenuTab::Host)
        qFatal("last tab not remembered after an action");
    const int tabHeight = panel.height();
    // LB/RB step through the visible tabs and wrap at both ends.
    panel.gamepadSwitchTab(1);
    if (panel.currentTab() != MenuTab::Session || !panel.isAtTabRoot())
        qFatal("RB did not switch to the Session tab");
    panel.gamepadSwitchTab(1);
    if (panel.currentTab() != MenuTab::Quick || !panel.isAtTabRoot())
        qFatal("RB did not wrap");
    panel.gamepadSwitchTab(-1);
    if (panel.currentTab() != MenuTab::Session)
        qFatal("LB did not wrap");
    if (panel.height() != tabHeight)
        qFatal("switching tabs resized the panel");
    // Keyboard: Tab / Shift+Tab / PageUp / PageDown switch tabs.
    if (!panel.handleNavigationKey(Qt::Key_Tab) || panel.currentTab() != MenuTab::Quick)
        qFatal("Tab did not switch to the next tab");
    panel.handleNavigationKey(Qt::Key_Tab, Qt::ShiftModifier);
    if (panel.currentTab() != MenuTab::Session)
        qFatal("Shift+Tab did not switch back");
    panel.handleNavigationKey(Qt::Key_PageDown);
    if (panel.currentTab() != MenuTab::Quick)
        qFatal("PageDown did not switch tabs");
    panel.handleNavigationKey(Qt::Key_PageUp);
    if (panel.currentTab() != MenuTab::Session)
        qFatal("PageUp did not switch tabs");
    // D-pad / arrow left/right on a tab root switches tabs.
    panel.gamepadMoveHorizontal(1);
    if (panel.currentTab() != MenuTab::Quick)
        qFatal("d-pad right did not switch tabs");
    panel.handleNavigationKey(Qt::Key_Right);
    if (panel.currentTab() != MenuTab::Display)
        qFatal("arrow right did not switch tabs");
    panel.handleNavigationKey(Qt::Key_Left);
    if (panel.currentTab() != MenuTab::Quick)
        qFatal("arrow left did not switch tabs");
    if (panel.handleNavigationKey(Qt::Key_A))
        qFatal("non-menu key was consumed");
    // Mouse: clicking a tab shows it.
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier,
                      panel.tabGeometry(MenuTab::Input).center());
    if (panel.currentTab() != MenuTab::Input || !panel.isAtTabRoot())
        qFatal("tab click did not switch tabs");
    // Keyboard into a sub-level: Down to the Send Keys entry, Enter opens it,
    // Backspace returns to the same tab.
    const int sendKeysRow = rowOf(panel, MenuAction::OpenSendKeys);
    for (int i = 0; i <= sendKeysRow; i++)
        panel.handleNavigationKey(Qt::Key_Down);
    panel.handleNavigationKey(Qt::Key_Return);
    if (panel.isAtTabRoot())
        qFatal("Enter did not open the Send Keys level");
    // Inside a sub-level the bar still shows its tab; arrows don't switch.
    panel.handleNavigationKey(Qt::Key_Right);
    if (panel.isAtTabRoot() || panel.currentTab() != MenuTab::Input)
        qFatal("arrow right left the Send Keys level");
    panel.handleNavigationKey(Qt::Key_Backspace);
    if (!panel.isAtTabRoot() || panel.currentTab() != MenuTab::Input)
        qFatal("Backspace did not return to the Input tab");
    // RB from a sub-level goes to the next tab's root.
    if (!panel.showLevel(MenuAction::OpenBitrate))
        qFatal("Bitrate level missing");
    if (panel.currentTab() != MenuTab::Display)
        qFatal("Bitrate level is not under Display");
    panel.gamepadSwitchTab(1);
    if (panel.currentTab() != MenuTab::Input || !panel.isAtTabRoot())
        qFatal("RB in a sub-level did not go to the next tab");
    // Enter on the first Quick row dispatches its action and closes.
    panel.showTab(MenuTab::Quick);
    panel.handleNavigationKey(Qt::Key_Down);
    panel.handleNavigationKey(Qt::Key_Return);
    if (actionCount != 1 || lastAction != MenuAction::ToggleStatsOverlay)
        qFatal("Enter did not dispatch the Performance Stats action");
    waitClosed(panel);
    if (panel.isVisible())
        qFatal("action did not close the menu");
    if (panel.handleNavigationKey(Qt::Key_Down))
        qFatal("hidden menu consumed a key");
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400), false);
    QTest::qWait(240);
    if (panel.currentTab() != MenuTab::Quick || !panel.isAtTabRoot())
        qFatal("reopened menu forgot the Quick tab");
    // B / Escape at a tab root closes the menu.
    panel.handleNavigationKey(Qt::Key_Escape);
    waitClosed(panel);
    if (panel.isVisible())
        qFatal("Escape at a tab root did not close the menu");

    // --- Stream Quality: level found by action, pending choice, apply ---
    std::optional<OverlayMenuPanel::StreamQualityRequest> qualityRequest;
    panel.setStreamQualityCallback(
        [&](const OverlayMenuPanel::StreamQualityRequest &request) { qualityRequest = request; });
    OverlayMenuPanel::StreamQualityState qualityState;
    qualityState.resolution = QSize(1920, 1080);
    qualityState.fps = 60;
    panel.updateStreamQualityState(qualityState);
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400), false);
    QTest::qWait(240);
    if (!panel.showTab(MenuTab::Display))
        qFatal("Display tab missing");
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier,
                      rowCenter(panel, rowOf(panel, MenuAction::OpenStreamQuality)));
    if (panel.isAtTabRoot() || panel.currentTab() != MenuTab::Display)
        qFatal("Stream Quality level did not open under Display");
    // Switch stays disabled until the choice differs from the running stream.
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier,
                      rowCenter(panel, rowOf(panel, MenuAction::ApplyStreamQuality)));
    if (qualityRequest || !panel.isMenuVisible())
        qFatal("unchanged stream quality was applied");
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier,
                      rowCenter(panel, rowOf(panel, MenuAction::SelectStreamResolution,
                                             QStringLiteral("2560x1440"))));
    if (qualityRequest || !panel.isMenuVisible() || panel.isAtTabRoot())
        qFatal("picking a resolution closed the menu or left the level");
    // The Bitrate row links to the Bitrate level, also after the pick above
    // rebuilt the level.
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier,
                      rowCenter(panel, rowOf(panel, MenuAction::OpenBitrate)));
    if (panel.rowOfAction(MenuAction::SetBitrate, QStringLiteral("10000")) < 0)
        qFatal("Stream Quality's Bitrate row did not open the Bitrate level");
    // The pending choice survives leaving the level.
    if (!panel.showLevel(MenuAction::OpenStreamQuality))
        qFatal("Stream Quality level missing");
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier,
                      rowCenter(panel, rowOf(panel, MenuAction::ApplyStreamQuality)));
    if (!qualityRequest || qualityRequest->resolution != QSize(2560, 1440) ||
        qualityRequest->fps != 60 || qualityRequest->saveAsDefault)
        qFatal("stream quality switch did not report 1440p60");
    waitClosed(panel);
    if (panel.isVisible())
        qFatal("applying stream quality did not close the menu");

    // --- Session: entry in the Session tab, rows refreshed while shown ---
    int sessionPolls = 0;
    panel.setSessionStatsProvider([&]() {
        ++sessionPolls;
        return std::vector<std::pair<QString, QString>>{
            { QStringLiteral("Duration"), QString::number(sessionPolls) },
            { QStringLiteral("Average FPS"), QStringLiteral("60") }
        };
    });
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400), false);
    QTest::qWait(240);
    if (!panel.showTab(MenuTab::Session))
        qFatal("Session tab missing");
    const int pollsBeforeOpen = sessionPolls;
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier,
                      rowCenter(panel, rowOf(panel, MenuAction::OpenSessionStats)));
    if (panel.isAtTabRoot() || panel.currentTab() != MenuTab::Session)
        qFatal("Session level did not open from its tab");
    if (sessionPolls <= pollsBeforeOpen)
        qFatal("opening the Session level did not refresh it");
    // Read-only rows: clicking one neither closes the menu nor leaves the level.
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, rowCenter(panel, 0));
    if (!panel.isMenuVisible() || panel.isAtTabRoot())
        qFatal("a Session row was clickable");
    const int pollsWhileOpen = sessionPolls;
    QTest::qWait(1300);
    if (sessionPolls <= pollsWhileOpen)
        qFatal("the Session level was not refreshed while shown");
    // A rebuild (e.g. a USB refresh) keeps the level open and refreshing.
    panel.updateRemoteUsbState(true, OverlayMenuPanel::RemoteUsbState::Available, devices,
                               QString(), QStringLiteral("Ready"));
    if (panel.isAtTabRoot() || panel.rowOfAction(MenuAction::OpenSessionStats) != 0)
        qFatal("a rebuild left the Session level");
    const int pollsAfterRebuild = sessionPolls;
    QTest::qWait(1300);
    if (sessionPolls <= pollsAfterRebuild)
        qFatal("the Session level stopped refreshing after a rebuild");
    // Leaving the level stops the refresh.
    panel.gamepadBack();
    if (!panel.isAtTabRoot() || panel.currentTab() != MenuTab::Session)
        qFatal("back did not return to the Session tab");
    const int pollsAfterBack = sessionPolls;
    QTest::qWait(1300);
    if (sessionPolls != pollsAfterBack)
        qFatal("the Session level kept refreshing after it was left");
    panel.closeMenu();
    waitClosed(panel);
    return 0;
}
