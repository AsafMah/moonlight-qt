#include "../../app/streaming/video/overlaymenupanel.h"
#include <QGuiApplication>
#include <QFontDatabase>
#include <QCursor>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QTest>
#include <QSettings>
#include <QTemporaryDir>
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
        else
            menu.showAtCursor(x(), y(), width(), height(), position() + QPoint(250, 150));
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

// Every placement keeps the menu inside the bounds it is given (the stream,
// cut to its screen), on any monitor layout, and big menus keep their
// top-left corner visible.
static void testPlacementGeometry()
{
    using namespace OverlayMenu;
    const QSize menu(340, 420);
    const QRect boundsList[] = {
        QRect(0, 0, 1920, 1080),
        QRect(-1920, 200, 1280, 720),   // a monitor left of the primary one
        QRect(2560, -1440, 3840, 2160), // above and right, high DPI in logical pixels
        QRect(100, 50, 300, 260),       // a stream smaller than the menu
    };
    const std::optional<QPoint> pointers[] = { std::nullopt, QPoint(5, 5), QPoint(100000, 100000) };
    const std::optional<QPointF> draggedList[] = { std::nullopt, QPointF(0, 0), QPointF(1, 1),
                                                   QPointF(0.3, 0.8) };
    for (const QRect &bounds : boundsList) {
        for (int p = 0; p < int(Placement::Count); p++) {
            const Placement placement = Placement(p);
            for (const auto &pointer : pointers) {
                for (const auto &dragged : draggedList) {
                    const QSize size = placedSize(placement, bounds, menu);
                    const QRect placed(placeMenu(placement, bounds, size, pointer, dragged, 12),
                                       size);
                    const bool fits =
                        size.width() <= bounds.width() && size.height() <= bounds.height();
                    if (fits
                            ? !bounds.contains(placed)
                            : (placed.left() != bounds.left() && size.width() > bounds.width()) ||
                                  (placed.top() != bounds.top() && size.height() > bounds.height()))
                        qFatal("placement %s left the bounds", qPrintable(placementId(placement)));
                }
            }
        }
    }

    const QRect screen(0, 0, 1920, 1080);
    if (placeMenu(Placement::Center, screen, menu, std::nullopt, std::nullopt, 12) !=
            QPoint((1920 - 340) / 2, (1080 - 420) / 2) ||
        placeMenu(Placement::BottomRight, screen, menu, std::nullopt, std::nullopt, 12) !=
            QPoint(1920 - 340 - 12, 1080 - 420 - 12) ||
        placeMenu(Placement::TopLeft, screen, menu, std::nullopt, std::nullopt, 12) !=
            QPoint(12, 12) ||
        placeMenu(Placement::Cursor, screen, menu, QPoint(1900, 1000), std::nullopt, 12) !=
            QPoint(1920 - 340, 1080 - 420))
        qFatal("a placement is in the wrong spot");
    const QSize panelSize = placedSize(Placement::RightPanel, screen, menu);
    if (panelSize.height() != 1080 ||
        placeMenu(Placement::RightPanel, screen, panelSize, std::nullopt, std::nullopt, 12) !=
            QPoint(1920 - 340, 0))
        qFatal("the side panel is not full height against the edge");

    // A dragged position survives a stream of another size
    const QPoint draggedTo(700, 300);
    const QPointF normalized = normalizedPosition(draggedTo, screen);
    if (placeMenu(Placement::Dragged, screen, menu, std::nullopt, normalized, 12) != draggedTo)
        qFatal("a dragged position did not round-trip");
    // ... and a level of another height keeps its title bar where it was
    if (placeMenu(Placement::Dragged, screen, QSize(340, 200), std::nullopt, normalized, 12) !=
        draggedTo)
        qFatal("a shorter level moved the dragged menu");

    for (int p = 0; p < int(Placement::Count); p++) {
        if (placementFromId(placementId(Placement(p)), Placement::Count) != Placement(p))
            qFatal("placement id did not round-trip");
    }
}

static void testEdgeTrigger()
{
    using namespace OverlayMenu;
    const QRect bounds(0, 0, 1920, 1080);
    if (!pointerInZone(QPoint(1919, 500), bounds, EdgeZone::Right, 6, 48) ||
        pointerInZone(QPoint(1900, 500), bounds, EdgeZone::Right, 6, 48) ||
        !pointerInZone(QPoint(500, 1079), bounds, EdgeZone::Bottom, 6, 48) ||
        !pointerInZone(QPoint(2, 30), bounds, EdgeZone::TopLeft, 6, 48) ||
        pointerInZone(QPoint(2, 500), bounds, EdgeZone::TopLeft, 6, 48) ||
        pointerInZone(QPoint(30, 30), bounds, EdgeZone::TopLeft, 6, 48) ||
        pointerInZone(QPoint(-5, 500), bounds, EdgeZone::Left, 6, 48))
        qFatal("edge zone hit test is wrong");

    EdgeDwell dwell;
    if (dwell.update(true, 1000, 400) || dwell.remainingMs(1100, 400) != 300 ||
        dwell.update(true, 1399, 400) || !dwell.update(true, 1400, 400))
        qFatal("edge dwell did not wait for its delay");
    if (dwell.update(true, 3000, 400) || dwell.remainingMs(3000, 400) != -1)
        qFatal("edge dwell fired twice in one visit");
    if (dwell.update(false, 3100, 400) || dwell.update(true, 3200, 400) ||
        !dwell.update(true, 3600, 400))
        qFatal("edge dwell did not re-arm after leaving");
    EdgeDwell immediate;
    if (!immediate.update(true, 0, 0))
        qFatal("a zero delay did not fire at once");
}

static void testOpeningClickSuppression(OverlayMenuPanel &panel)
{
    using namespace OverlayMenu;
    panel.showWithPlacement(Placement::TopLeft, 0, 0, 1600, 1200, std::nullopt, std::nullopt);
    QTest::qWait(240);

    const QPoint openingRelease = panel.geometry().bottomRight() + QPoint(20, 20);
    panel.ignoreOpeningClickAt(openingRelease);
    panel.dismissOnOutsideClick(openingRelease);
    if (!panel.isMenuVisible()) {
        qFatal("the floating-button opening release dismissed the menu");
    }

    // The suppression is one-shot; the next outside click closes normally.
    panel.dismissOnOutsideClick(openingRelease);
    waitClosed(panel);
    if (panel.isMenuVisible()) {
        qFatal("outside click was still suppressed after the opening release");
    }

    panel.showWithPlacement(Placement::TopLeft, 0, 0, 1600, 1200, std::nullopt, std::nullopt);
    QTest::qWait(240);
    panel.ignoreOpeningClickAt(openingRelease);
    panel.dismissOnOutsideClick(openingRelease + QPoint(1, 0));
    waitClosed(panel);
    if (panel.isMenuVisible()) {
        qFatal("a different outside click was suppressed");
    }
}

static void testOverlayAttachmentOptions(OverlayMenuPanel &panel)
{
    using namespace OverlayMenu;
    panel.updateMenuAttachmentState(false);
    panel.updateStatsOverlayState(StatsOverlay::Preset::Compact, true, false, 85);
    panel.showWithPlacement(Placement::TopLeft, 0, 0, 1600, 1200, std::nullopt, std::nullopt);
    QTest::qWait(240);
    if (!panel.showLevel(MenuAction::OpenMenuPosition) ||
        panel.rowOfAction(MenuAction::ToggleMenuDetached) < 0) {
        qFatal("the menu position level is missing the live attachment action");
    }
    MenuAction action = MenuAction::MenuActionMax;
    panel.setActionCallback([&](MenuAction selected) { action = selected; });
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier,
                      rowCenter(panel, rowOf(panel, MenuAction::ToggleMenuDetached)));
    waitClosed(panel);
    if (action != MenuAction::ToggleMenuDetached) {
        qFatal("the menu attachment choice did not dispatch");
    }
    panel.setDetached(true);
    if (!panel.isDetached()) {
        qFatal("the menu did not detach after its close completed");
    }
    panel.setDetached(false);
    if (panel.isDetached()) {
        qFatal("the menu did not reattach");
    }

    panel.showWithPlacement(Placement::TopLeft, 0, 0, 1600, 1200, std::nullopt, std::nullopt);
    QTest::qWait(240);
    if (!panel.showLevel(MenuAction::OpenStatsOverlay) ||
        panel.rowOfAction(MenuAction::ToggleStatsOverlayDetached) < 0 ||
        panel.rowOfAction(MenuAction::OpenStatsOpacity) < 0) {
        qFatal("the performance overlay level is missing attachment or transparency controls");
    }
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier,
                      rowCenter(panel, rowOf(panel, MenuAction::ToggleStatsOverlayDetached)));
    waitClosed(panel);
    if (action != MenuAction::ToggleStatsOverlayDetached) {
        qFatal("the performance overlay attachment choice did not dispatch");
    }

    panel.showWithPlacement(Placement::TopLeft, 0, 0, 1600, 1200, std::nullopt, std::nullopt);
    QTest::qWait(240);
    if (!panel.showLevel(MenuAction::OpenStatsOverlay)) {
        qFatal("the performance overlay level could not be reopened");
    }
    if (!panel.showLevel(MenuAction::OpenStatsOpacity) ||
        panel.rowOfAction(MenuAction::SetStatsOpacity, QStringLiteral("50")) < 0) {
        qFatal("the transparency level is missing its 50% option");
    }

    MenuAction payloadAction = MenuAction::MenuActionMax;
    QString payload;
    panel.setPayloadActionCallback([&](MenuAction action, const QString &value) {
        payloadAction = action;
        payload = value;
    });
    QTest::mouseClick(
        &panel, Qt::LeftButton, Qt::NoModifier,
        rowCenter(panel, rowOf(panel, MenuAction::SetStatsOpacity, QStringLiteral("50"))));
    waitClosed(panel);
    if (payloadAction != MenuAction::SetStatsOpacity || payload != QStringLiteral("50")) {
        qFatal("the transparency choice did not dispatch its selected percentage");
    }
    panel.setActionCallback({});
    panel.setPayloadActionCallback({});
}

static QRect contentRect(const OverlayMenuPanel &panel)
{
    return panel.contentGeometry();
}

static void testMenuOptions(OverlayMenuPanel &panel)
{
    using namespace OverlayMenu;
    const QRect stream(0, 0, 1920, 1080);

    // A long label widens the panel instead of being cut off
    panel.updateRemoteUsbState(
        true, OverlayMenuPanel::RemoteUsbState::Available,
        { { QStringLiteral("9-9"),
            QStringLiteral("A keyboard with a remarkably long product name that never ends"),
            QStringLiteral("1234:ABCD"), true } },
        {}, QStringLiteral("1 available"));

    // No title, tab or label is cut off in any layout, scale or position
    const Layout layouts[] = { Layout::Compact, Layout::Comfortable, Layout::SidePanel };
    const Placement placements[] = { Placement::Center, Placement::BottomRight,
                                     Placement::LeftPanel, Placement::Cursor };
    for (Layout layout : layouts) {
        for (int scale : { 75, 100, 150 }) {
            for (Placement placement : placements) {
                panel.setStyle(layout, scale, 90);
                panel.setGamepadHints(true, GamepadUiStyleXbox, false,
                                      QStringLiteral("Back+Start"));
                panel.showWithPlacement(placement, stream.x(), stream.y(), stream.width(),
                                        stream.height(), QPoint(1800, 1000), std::nullopt);
                QTest::qWait(200); // past the slide-in
                const QStringList clipped = panel.clippedTexts();
                if (!clipped.isEmpty())
                    qFatal("%s at %d%% cuts off: %s", qPrintable(layoutId(layout)), scale,
                           qPrintable(clipped.join(QStringLiteral(", "))));
                if (!stream.contains(contentRect(panel)))
                    qFatal("%s at %d%% (%s) is off the stream", qPrintable(layoutId(layout)), scale,
                           qPrintable(placementId(placement)));
                // Every sub-level fits too, and keeps the panel on the stream
                for (MenuAction open : { MenuAction::OpenMenuPosition, MenuAction::OpenMenuLayout,
                                         MenuAction::OpenRemoteUsbDevices }) {
                    if (!panel.showLevel(open))
                        qFatal("level %d is missing", int(open));
                    if (!stream.contains(contentRect(panel))) {
                        const QRect content = contentRect(panel);
                        qFatal("sub-level %d of %s at %d%% (%s) left the stream: %d,%d %dx%d",
                               int(open), qPrintable(layoutId(layout)), scale,
                               qPrintable(placementId(placement)), content.x(), content.y(),
                               content.width(), content.height());
                    }
                }
                panel.closeMenu();
                waitClosed(panel);
            }
        }
    }
    panel.setGamepadHints(false, GamepadUiStyleXbox, false, QString());

    // Scale grows the window
    panel.setStyle(Layout::Compact, 100, 100);
    panel.showWithPlacement(Placement::Center, 0, 0, 1920, 1080, std::nullopt, std::nullopt);
    QTest::qWait(200); // past the slide-in
    const int normalWidth = panel.width();
    panel.closeMenu();
    waitClosed(panel);
    panel.setStyle(Layout::Compact, 150, 100);
    panel.showWithPlacement(Placement::Center, 0, 0, 1920, 1080, std::nullopt, std::nullopt);
    QTest::qWait(200); // past the slide-in
    if (panel.width() < normalWidth * 14 / 10)
        qFatal("150%% scale did not grow the panel");
    // Rows are still found and clicked by identity at another scale
    panel.showTab(MenuTab::Display);
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier,
                      panel.rowGeometry(rowOf(panel, MenuAction::OpenMenuLayout)).center());
    if (panel.isAtTabRoot() || panel.rowOfAction(MenuAction::SetMenuLayoutSidePanel) < 0)
        qFatal("clicking a row at 150%% did not open its level");
    panel.closeMenu();
    waitClosed(panel);

    // Side panel: the tabs are a vertical rail, driven by gamepad, keys and mouse
    panel.setStyle(Layout::SidePanel, 100, 100);
    panel.showWithPlacement(Placement::RightPanel, 0, 0, 1920, 1080, std::nullopt, std::nullopt);
    QTest::qWait(200); // past the slide-in
    panel.showTab(MenuTab::Quick);
    const QRect quickTab = panel.tabGeometry(MenuTab::Quick);
    const QRect displayTab = panel.tabGeometry(MenuTab::Display);
    if (quickTab.isEmpty() || displayTab.x() != quickTab.x() || displayTab.y() <= quickTab.y())
        qFatal("side panel tabs are not a vertical rail");
    if (panel.rowGeometry(0).left() <= quickTab.right())
        qFatal("side panel rows overlap the rail");
    if (contentRect(panel).height() != 1080 || contentRect(panel).right() != 1919)
        qFatal("the right side panel is not full height against the edge");
    panel.gamepadSwitchTab(1);
    if (panel.currentTab() != MenuTab::Display)
        qFatal("RB did not move down the rail");
    panel.handleNavigationKey(Qt::Key_Tab);
    if (panel.currentTab() != MenuTab::Input)
        qFatal("Tab did not move down the rail");
    panel.handleNavigationKey(Qt::Key_PageUp);
    if (panel.currentTab() != MenuTab::Display)
        qFatal("Page Up did not move up the rail");
    panel.gamepadMoveHorizontal(1);
    if (panel.currentTab() != MenuTab::Input)
        qFatal("D-pad right did not switch rail tabs");
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier,
                      panel.tabGeometry(MenuTab::Session).center());
    if (panel.currentTab() != MenuTab::Session)
        qFatal("clicking a rail tab did not switch to it");
    panel.showTab(MenuTab::Display);
    panel.gamepadMoveDown();
    panel.gamepadSelect();
    if (panel.isAtTabRoot())
        qFatal("gamepad select in the side panel did not open a level");
    panel.handleNavigationKey(Qt::Key_Backspace);
    if (!panel.isAtTabRoot() || panel.currentTab() != MenuTab::Display)
        qFatal("Backspace did not go back to the rail tab");
    panel.closeMenu();
    waitClosed(panel);

    // Escape: back one level by default, or close from anywhere
    panel.setStyle(Layout::Compact, 100, 100);
    panel.setEscapeAction(EscapeAction::Back);
    panel.showWithPlacement(Placement::Center, 0, 0, 1920, 1080, std::nullopt, std::nullopt);
    QTest::qWait(200); // past the slide-in
    panel.showLevel(MenuAction::OpenBitrate);
    panel.handleNavigationKey(Qt::Key_Escape);
    if (!panel.isMenuVisible() || !panel.isAtTabRoot())
        qFatal("Escape did not go back one level");
    panel.closeMenu();
    waitClosed(panel);
    panel.setEscapeAction(EscapeAction::Close);
    panel.showWithPlacement(Placement::Center, 0, 0, 1920, 1080, std::nullopt, std::nullopt);
    QTest::qWait(200); // past the slide-in
    panel.showLevel(MenuAction::OpenBitrate);
    panel.handleNavigationKey(Qt::Key_Escape);
    if (panel.isMenuVisible())
        qFatal("Escape did not close the menu from a sub-level");
    waitClosed(panel);
    panel.setEscapeAction(EscapeAction::Back);

    // Dragging the title bar moves the panel, keeps it on the stream and
    // reports where it was left
    std::optional<QPointF> dropped;
    panel.setDragFinishedCallback([&](const QPointF &position) { dropped = position; });
    panel.showWithPlacement(Placement::Center, 0, 0, 1920, 1080, std::nullopt, std::nullopt);
    QTest::qWait(200); // past the slide-in
    const QPoint before = panel.position();
    const QPoint grip(60, 8 + 12);
    QTest::mousePress(&panel, Qt::LeftButton, Qt::NoModifier, grip);
    QTest::mouseMove(&panel, grip + QPoint(40, 30));
    QTest::mouseMove(&panel, grip + QPoint(5000, 5000));
    QTest::mouseRelease(&panel, Qt::LeftButton, Qt::NoModifier, grip + QPoint(5000, 5000));
    if (panel.position() == before || !dropped ||
        !QRect(0, 0, 1920, 1080).contains(contentRect(panel)))
        qFatal("dragging the title bar did not move the panel within the stream");
    const QRect droppedRect = contentRect(panel);
    if (droppedRect.right() != 1919 || droppedRect.bottom() != 1079)
        qFatal("a drag past the corner did not stop at the corner");
    panel.closeMenu();
    waitClosed(panel);
    panel.showWithPlacement(Placement::Dragged, 0, 0, 1920, 1080, std::nullopt, dropped);
    QTest::qWait(200); // past the slide-in
    if (contentRect(panel).topLeft() != droppedRect.topLeft())
        qFatal("the dragged position was not restored");
    panel.closeMenu();
    waitClosed(panel);
}

static void testPointerEditing()
{
    OverlayMenuPanel panel;
    int actions = 0;
    int savedScale = 0;
    int savedOpacity = 0;
    panel.setStyleEditedCallback([&](int scale, int opacity) {
        savedScale = scale;
        savedOpacity = opacity;
    });
    panel.setActionCallback([&](MenuAction) { ++actions; });
    panel.showAtCursor(0, 0, 1920, 1080, QPoint(400, 100));
    QTest::qWait(250);
    panel.showTab(MenuTab::Quick);
    panel.setPointerEditMode(true);
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier,
                      rowCenter(panel, rowOf(panel, MenuAction::ToggleFullScreen)));
    if (actions != 0 || !panel.isEditing() || !panel.isMenuVisible())
        qFatal("persistent-mode click must edit the menu, not fire its action");
    const int scale = panel.scalePercent();
    const QPoint point = panel.contentGeometry().center() - panel.position();
    QWheelEvent zoom(QPointF(point), QPointF(panel.position() + point), QPoint(), QPoint(0, 120),
                     Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&panel, &zoom);
    if (panel.scalePercent() != scale + 5)
        qFatal("editing wheel must zoom the menu instead of touching its controls");
    const int opacity = panel.opacityPercent();
    QWheelEvent fade(QPointF(point), QPointF(panel.position() + point), QPoint(), QPoint(0, -120),
                     Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&panel, &fade);
    if (panel.opacityPercent() != opacity - 5)
        qFatal("editing Ctrl+wheel must change opacity");
    if (savedScale != panel.scalePercent() || savedOpacity != panel.opacityPercent())
        qFatal("edited menu style must reach the persistence callback");
    const auto drag = [&](const QPoint &local, const QPoint &delta) {
        const QPoint global = panel.position() + local;
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(local), QPointF(global), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&panel, &press);
        QMouseEvent move(QEvent::MouseMove, QPointF(local + delta), QPointF(global + delta),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&panel, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(local + delta),
                            QPointF(global + delta), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&panel, &release);
    };
    const QPoint beforeMove = panel.position();
    drag(panel.contentGeometry().center() - panel.position(), QPoint(30, 20));
    if (panel.position() != beforeMove + QPoint(30, 20))
        qFatal("edit-mode dragging over menu controls must move the menu");
    const QRect beforeResize = panel.contentGeometry();
    const QPoint edge =
        QPoint(beforeResize.right() - 6, beforeResize.center().y()) - panel.position();
    drag(edge, QPoint(40, 0));
    if (panel.contentGeometry().width() <= beforeResize.width() ||
        panel.contentGeometry().left() != beforeResize.left() || savedScale != panel.scalePercent())
        qFatal(
            "menu edge drag must resize and persist scale while keeping the opposite edge fixed");
    panel.setPointerEditMode(false);
    if (panel.isEditing())
        qFatal("persistent-mode exit must finish menu editing");
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier,
                      rowCenter(panel, rowOf(panel, MenuAction::ToggleFullScreen)));
    if (actions != 1)
        qFatal("normal menu actions must work again outside persistent editing");
    panel.closeMenu();
    waitClosed(panel);
}

static void testShortcutParity()
{
    OverlayMenuPanel panel;
    panel.updateHostCommands(true, { { QStringLiteral("test"), QStringLiteral("Test command") } });
    panel.updateSendKeys({ { QStringLiteral("Ctrl+Alt+Del"), QStringLiteral("Ctrl+Alt+Del"),
                             QStringLiteral("special:ctrl-alt-del") } },
                         {});
    const QStringList intentionallyMenuLess{
        QLatin1String(ShortcutActions::ToggleOverlayMenu),   // Opens/closes this menu.
        QLatin1String(ShortcutActions::RightAltMouseRelease) // A physical hold, not a click.
    };
    QStringList missing;
    for (const ShortcutAction &action : ShortcutRegistry::instance().actions()) {
        if (!intentionallyMenuLess.contains(action.id) &&
            !panel.menuShortcutActions().contains(action.id)) {
            missing.append(action.id);
        }
    }
    if (!missing.isEmpty())
        qFatal("registry actions without menu entries: %s", qPrintable(missing.join(", ")));
}

static void testShortcutRebinding()
{
    OverlayMenuPanel panel;
    ShortcutMatcher matcher;
    ShortcutDispatcher dispatcher;
    int calls = 0;
    dispatcher.setHandler(QLatin1String(ShortcutActions::Quit), [&] { ++calls; });
    panel.setShortcutProfileCallback(
        [&](const ShortcutProfile &value) { matcher.setProfile(value); });
    ShortcutProfile profile;
    const ShortcutBinding custom = ShortcutBinding::fromString(QStringLiteral("Ctrl+F9"));
    if (!profile.setBinding(QLatin1String(ShortcutActions::Quit), 0, custom))
        qFatal("test shortcut was rejected");
    profile.saveToSettings();
    panel.setGamepadHints(true, GamepadUiStyleXbox, false, QStringLiteral("Back+Start"));
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400));
    QTest::qWait(240);
    const QString detail = panel.rowDetail(rowOf(panel, MenuAction::Quit));
    if (!detail.contains(custom.toDisplayString()) || !detail.contains("Back+Start"))
        qFatal("Disconnect must show the current shortcut and retain its gamepad hint: %s",
               qPrintable(detail));
    if (!matcher.processKeyEvent(custom.scancode, 0, custom.modifiers, true, false, dispatcher) ||
        calls != 1)
        qFatal("menu hint refresh must update the input matcher too");
    profile.setBinding(QLatin1String(ShortcutActions::Quit), 0, {});
    profile.saveToSettings();
    QTest::qWait(350);
    if (panel.rowDetail(rowOf(panel, MenuAction::Quit)).contains(custom.toDisplayString()))
        qFatal("visible menu retained an unbound shortcut");
    if (!matcher.processKeyEvent(custom.scancode, 0, custom.modifiers, false, false, dispatcher))
        qFatal("rebind lost the swallowed key-up of a shortcut already held");
    if (matcher.processKeyEvent(custom.scancode, 0, custom.modifiers, true, false, dispatcher))
        qFatal("unbound hint disappeared but old shortcut still triggers");
    panel.closeMenu();
    waitClosed(panel);
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400));
    if (panel.rowDetail(rowOf(panel, MenuAction::Quit)).contains(custom.toDisplayString()))
        qFatal("reopening retained an unbound shortcut");
    panel.closeMenu();
    waitClosed(panel);
    ShortcutProfile().saveToSettings();
}

static void testShortcutDispatch()
{
    OverlayMenuPanel panel;
    QString dispatched;
    panel.setActionCallback(
        [&](MenuAction action) { dispatched = OverlayMenuPanel::shortcutActionId(action); });
    const struct
    {
        MenuAction action;
        const char *id;
    } cases[] = {
        { MenuAction::ToggleKeyboardGrab, ShortcutActions::ToggleKeyboardGrab },
        { MenuAction::CycleOverlayPreset, ShortcutActions::CycleOverlayPreset },
        { MenuAction::EditStatsOverlayPosition, ShortcutActions::EditOverlayPosition },
    };
    for (const auto &test : cases) {
        panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400));
        QTest::qWait(240);
        if (test.action == MenuAction::ToggleKeyboardGrab)
            panel.showTab(MenuTab::Input);
        else
            panel.showLevel(MenuAction::OpenStatsOverlay);
        QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier,
                          rowCenter(panel, rowOf(panel, test.action)));
        if (dispatched != QLatin1String(test.id))
            qFatal("menu did not dispatch shortcut %s", test.id);
        waitClosed(panel);
    }

    ShortcutProfile profile;
    profile.setBinding(QLatin1String(ShortcutActions::SendCtrlAltDel), 0,
                       ShortcutBinding::fromString(QStringLiteral("Ctrl+F11")));
    profile.saveToSettings();
    const QString payload = QStringLiteral("special:ctrl-alt-del");
    panel.updateSendKeys(
        { { QStringLiteral("Ctrl+Alt+Del"), QStringLiteral("Ctrl+Alt+Del"), payload },
          { QStringLiteral("Alt+Tab"), QStringLiteral("Alt+Tab"),
            QStringLiteral("special:alt-tab") } },
        {});
    panel.setPayloadActionCallback([&](MenuAction action, const QString &value) {
        if (action != MenuAction::SendHostKeys || value != payload)
            qFatal("Send Keys dispatched the wrong host payload");
        dispatched = OverlayMenuPanel::shortcutActionId(action, value);
    });
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400));
    QTest::qWait(240);
    panel.showSendKeysLevel();
    const int row = rowOf(panel, MenuAction::SendHostKeys, payload);
    const QString detail = panel.rowDetail(row);
    if (!detail.contains("Ctrl+Alt+Del") || !detail.contains("Ctrl+F11"))
        qFatal("Send Keys must retain the remote chord beside the rebound local hint");
    if (!OverlayMenuPanel::shortcutActionId(MenuAction::SendHostKeys,
                                            QStringLiteral("special:alt-tab"))
             .isEmpty())
        qFatal("Ctrl+Alt+Del hint was applied to a different Send Keys payload");
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, rowCenter(panel, row));
    if (dispatched != QLatin1String(ShortcutActions::SendCtrlAltDel))
        qFatal("Send Ctrl+Alt+Del was not reachable through Send Keys");
    waitClosed(panel);
    ShortcutProfile().saveToSettings();
}

// Cancel Sending Files lives in the Host tab, is greyed out while nothing is
// being sent, comes alive while the menu is open, and shows its live shortcut.
static void testFileTransferCancel()
{
    OverlayMenuPanel panel;
    QString dispatched;
    panel.setActionCallback(
        [&](MenuAction action) { dispatched = OverlayMenuPanel::shortcutActionId(action); });
    ShortcutProfile profile;
    const ShortcutBinding custom = ShortcutBinding::fromString(QStringLiteral("Ctrl+F10"));
    if (!profile.setBinding(QLatin1String(ShortcutActions::CancelFileTransfer), 0, custom))
        qFatal("test shortcut was rejected");
    profile.saveToSettings();

    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400));
    QTest::qWait(240);
    if (!panel.showTab(MenuTab::Host))
        qFatal("Host tab is missing");
    const int row = rowOf(panel, MenuAction::CancelFileTransfer);
    if (!panel.rowDetail(row).contains(custom.toDisplayString()))
        qFatal("Cancel Sending Files must show its current shortcut: %s",
               qPrintable(panel.rowDetail(row)));
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, rowCenter(panel, row));
    if (!dispatched.isEmpty() || !panel.isMenuVisible())
        qFatal("Cancel Sending Files must be inert while nothing is being sent");

    panel.updateFileTransferState(true);
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier, rowCenter(panel, row));
    if (dispatched != QLatin1String(ShortcutActions::CancelFileTransfer))
        qFatal("Cancel Sending Files did not dispatch the cancel shortcut");
    waitClosed(panel);

    // Ending the transfer greys it out again while the menu is open.
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400));
    QTest::qWait(240);
    panel.showTab(MenuTab::Host);
    panel.updateFileTransferState(false);
    dispatched.clear();
    QTest::mouseClick(&panel, Qt::LeftButton, Qt::NoModifier,
                      rowCenter(panel, rowOf(panel, MenuAction::CancelFileTransfer)));
    if (!dispatched.isEmpty())
        qFatal("Cancel Sending Files stayed enabled after the transfer ended");
    panel.closeMenu();
    waitClosed(panel);
    ShortcutProfile().saveToSettings();
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QTemporaryDir settingsDirectory;
    if (!settingsDirectory.isValid())
        qFatal("cannot create isolated settings");
    QCoreApplication::setOrganizationName(QStringLiteral("MoonlightTests"));
    QCoreApplication::setApplicationName(QStringLiteral("OverlayMenuNavigation"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    if (app.arguments().contains(QStringLiteral("--shortcut-parity"))) {
        testShortcutParity();
        return 0;
    }
    if (app.arguments().contains(QStringLiteral("--shortcut-rebinding"))) {
        testShortcutRebinding();
        testShortcutDispatch();
        testFileTransferCancel();
        return 0;
    }
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
        panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 250));
        return app.exec();
    }
    // Offscreen cursor remains outside this panel. Opening a shorter submenu
    // must not dismiss the menu while the pointer remains outside it.
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400));
    QTest::qWait(200); // past the slide-in
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
    // Moving away from an ordinary menu must not dismiss it; only an explicit
    // outside click should close it.
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400));
    QTest::qWait(700);
    if (!panel.isVisible())
        qFatal("menu closed when the pointer left its trigger");
    panel.dismissOnOutsideClick(QPoint(-100, -100));
    waitClosed(panel);
    if (panel.isVisible())
        qFatal("explicit outside click failed to dismiss menu");
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400));
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
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400));
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
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400));
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
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400));
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
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400));
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
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400));
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
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400));
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
    panel.showAtCursor(0, 0, 1600, 1200, QPoint(400, 400));
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

    testPlacementGeometry();
    testEdgeTrigger();
    testOpeningClickSuppression(panel);
    testOverlayAttachmentOptions(panel);
    testMenuOptions(panel);
    testPointerEditing();
    testShortcutParity();
    testShortcutRebinding();
    testShortcutDispatch();
    testFileTransferCancel();
    return 0;
}
