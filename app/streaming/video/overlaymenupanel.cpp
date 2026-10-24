#include "overlaymenupanel.h"
#include "uifont.h"
#include "streaming/streamquality.h"

#include <QScreen>
#include <QGuiApplication>
#include <QCoreApplication>
#include <QCursor>
#include <QFontMetrics>
#include <QtMath>
#include <algorithm>
#include <memory>

namespace {
constexpr qint64 PointerGracePeriodMs = 300;
constexpr int PointerCheckIntervalMs = 150;
// Raster counterpart of gui/theme/Theme.qml (same palette and hard edges).
const QColor MenuSurface("#171A20");
const QColor MenuHover("#1F232B");
const QColor MenuLine("#3C434E");
const QColor MenuText("#EEF0EC");
const QColor MenuDim("#AEB3AB");
const QColor MenuFaint("#7E858E");
const QColor MenuAccent("#39C5BB");
const QColor MenuDanger("#FF876F");

// Piecewise-linear bitrate scale. Values are Kbps.
constexpr int kBitrateMinKbps = 500;
constexpr int kBitrateMaxKbps = 800000;
struct BitrateSegment
{
    int maxKbps;
    int stepKbps;
};
constexpr BitrateSegment kBitrateSegments[] = {
    { 5000, 500 },      // 0.5–5 Mbps: 0.5 Mbps
    { 20000, 1000 },    // 5–20 Mbps: 1 Mbps
    { 50000, 2000 },    // 20–50 Mbps: 2 Mbps
    { 100000, 5000 },   // 50–100 Mbps: 5 Mbps
    { 200000, 10000 },  // 100–200 Mbps: 10 Mbps
    { 400000, 50000 },  // 200–400 Mbps: 50 Mbps
    { 800000, 100000 }, // 400–800 Mbps: 100 Mbps
};

constexpr int bitrateSliderMaximum()
{
    int position = 0;
    int segmentMinKbps = kBitrateMinKbps;
    for (const auto& segment : kBitrateSegments) {
        position += (segment.maxKbps - segmentMinKbps) / segment.stepKbps;
        segmentMinKbps = segment.maxKbps;
    }
    return position;
}
constexpr int kBitrateSliderMax = bitrateSliderMaximum();

double bitrateToSliderPosition(int bitrateKbps)
{
    const int bitrate = qBound(kBitrateMinKbps, bitrateKbps, kBitrateMaxKbps);
    double position = 0;
    int segmentMinKbps = kBitrateMinKbps;
    for (const auto& segment : kBitrateSegments) {
        if (bitrate <= segment.maxKbps) {
            return position + (bitrate - segmentMinKbps) / double(segment.stepKbps);
        }
        position += (segment.maxKbps - segmentMinKbps) / double(segment.stepKbps);
        segmentMinKbps = segment.maxKbps;
    }
    return position;
}

int sliderPositionToBitrate(double sliderPosition)
{
    const int position = qBound(0, qRound(sliderPosition), kBitrateSliderMax);
    int segmentStart = 0;
    int segmentMinKbps = kBitrateMinKbps;
    for (const auto& segment : kBitrateSegments) {
        const int segmentSteps = (segment.maxKbps - segmentMinKbps) / segment.stepKbps;
        if (position <= segmentStart + segmentSteps) {
            return segmentMinKbps + (position - segmentStart) * segment.stepKbps;
        }
        segmentStart += segmentSteps;
        segmentMinKbps = segment.maxKbps;
    }
    return kBitrateMaxKbps;
}
// Idle window after the last scrub tick before the change is committed.
constexpr int kBitrateCommitDelayMs = 450;
// Bottom hint bar height, shown only while a gamepad is connected
constexpr int kGamepadHintBarHeight = 30;
// itemAtPos() codes for the tab bar: kTabHitBase - visible tab index
constexpr int kTabHitBase = -100;

using MenuAction = OverlayMenuPanel::MenuAction;
using MenuTab = OverlayMenuPanel::MenuTab;

struct TabAssignment
{
    MenuAction action;
    MenuTab tab;
};

// Tab of every top-level menu entry, keyed by the entry's MenuAction.
//
// This is the only place that files entries into tabs. To put a new entry
// in a tab, add ONE line here: { MenuAction::YourAction, MenuTab::Display }.
// - Entries appear in their tab in the order of this table (lines of other
//   tabs in between don't matter).
// - An entry missing from the table lands at the end of the Quick tab.
// - A tab with no entries (e.g. Session in release builds today, or Host
//   without host features) is hidden, so a tab needs no other registration.
// Sub-level rows (presets, keys, commands, devices) are not listed: they live
// in their sub-level, which opens from the listed Open* entry.
constexpr TabAssignment kTabAssignments[] = {
    // Quick: the most used toggles and leaving the stream
    { MenuAction::ToggleStatsOverlay, MenuTab::Quick },
    { MenuAction::ToggleMouseMode, MenuTab::Quick },
    { MenuAction::ToggleFullScreen, MenuTab::Quick },
    { MenuAction::ToggleMinimize, MenuTab::Quick },
    { MenuAction::ToggleMicrophone, MenuTab::Quick },
    { MenuAction::Quit, MenuTab::Quick },
    { MenuAction::QuitAndExit, MenuTab::Quick },
    // Display: what the stream looks like
    { MenuAction::OpenStatsOverlay, MenuTab::Display },
    { MenuAction::OpenBitrate, MenuTab::Display },
    { MenuAction::OpenMenuPosition, MenuTab::Display },
    // Input: keys sent to the host, mouse/keyboard/gamepad options, USB
    { MenuAction::OpenSendKeys, MenuTab::Input },
    { MenuAction::ToggleGamepadMouse, MenuTab::Input },
    { MenuAction::ToggleCursorHide, MenuTab::Input },
    { MenuAction::UngrabInput, MenuTab::Input },
    { MenuAction::TogglePointerRegionLock, MenuTab::Input },
    { MenuAction::PasteText, MenuTab::Input },
    { MenuAction::OpenRemoteUsbDevices, MenuTab::Input },
    // Host: features the host offers
    { MenuAction::OpenHostCommands, MenuTab::Host },
    { MenuAction::ShowHostFiles, MenuTab::Host },
// Session: the running session
#ifdef MOONLIGHT_ENABLE_FUNCTION_TESTS
    { MenuAction::OpenStylusReplayPanel, MenuTab::Session },
#endif
};
constexpr int kTabAssignmentCount = int(sizeof(kTabAssignments) / sizeof(kTabAssignments[0]));

// Position of an action in kTabAssignments; unlisted actions sort last
int tabAssignmentOrder(MenuAction action)
{
    for (int i = 0; i < kTabAssignmentCount; i++) {
        if (kTabAssignments[i].action == action) {
            return i;
        }
    }
    return kTabAssignmentCount;
}
}

OverlayMenuPanel::OverlayMenuPanel(QWindow* parent, OverlayWindowMode windowMode)
    : OverlayRasterWindow(parent, windowMode), m_CurrentLevel(0), m_HoveredIndex(-1),
      m_Visible(false), m_HasGamepads(false), m_GamepadUiStyle(GamepadUiStyleXbox),
      m_SwapFaceButtons(false), m_FileMappingState(FileMappingState::Unknown),
      m_FileMappingDetail(tr("Checking")), m_RemoteUsbAvailable(false),
      m_RemoteUsbState(RemoteUsbState::Unavailable), m_RemoteUsbDetail(tr("Unavailable")),
      m_ParentX(0), m_ParentY(0), m_ParentW(0), m_ParentH(0), m_WindowMode(windowMode),
      m_CloseWhenPointerOutside(false), m_ContentOffset(0), m_Closing(false), m_TargetPosition(),
      m_AnchorMode(AnchorMode::RightEdge), m_TriggerPosition(std::nullopt)
{
    Q_ASSERT(m_WindowMode != OverlayWindowMode::WaylandSubsurface || parent != nullptr);
    setFlags(OverlayWindowPolicy::flags(m_WindowMode));

    QSurfaceFormat fmt;
    fmt.setAlphaBufferSize(8);
    setFormat(fmt);

    // Logical (unscaled) values — Qt 6 handles DPI automatically
    // Match the application's square industrial panels.
    m_ItemHeight   = 38;
    m_Padding      = 4;
    m_MenuWidth    = 320;
    m_ShadowMargin = 8;
    m_TitleHeight  = 32;
    m_IconAreaWidth = 28;
    m_TabBarHeight = 30;

    m_LabelFont.setFamilies(UiFont::familyChain(QStringLiteral("Manrope")));
    m_LabelFont.setPointSize(10);
    m_LabelFont.setWeight(QFont::DemiBold);

    m_DetailFont.setFamilies(UiFont::familyChain(QStringLiteral("DM Mono")));
    m_DetailFont.setPointSize(8);

    m_TitleFont = m_DetailFont;
    m_TitleFont.setPointSize(9);
    m_TitleFont.setWeight(QFont::DemiBold);
    m_TitleFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);

    m_TabFont = m_DetailFont;
    m_TabFont.setWeight(QFont::DemiBold);
    m_TabFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);

    // Reuse the same bundled Fluent 24 Regular assets as settings/toolbars.
    // Keep QIcon instances alive so Qt can cache rasterizations for each DPI.
    for (const QString &name : {QStringLiteral("tb-settings"), QStringLiteral("menu-position"),
             QStringLiteral("menu-bitrate"), QStringLiteral("menu-files"),
             QStringLiteral("cat-peripherals"), QStringLiteral("cat-display"),
             QStringLiteral("menu-microphone"), QStringLiteral("cat-gamepad"),
             QStringLiteral("menu-close"), QStringLiteral("menu-next"),
             QStringLiteral("tb-back")}) {
        m_MenuIcons.insert(name, QIcon(QStringLiteral(":/res/fluent/%1.svg").arg(name)));
    }
    for (const QString& name : { QStringLiteral("cat-input"), QStringLiteral("cat-host"),
                                 QStringLiteral("tb-network") }) {
        m_MenuIcons.insert(name, QIcon(QStringLiteral(":/res/fluent/%1.svg").arg(name)));
    }

    // --- Animations ---
    m_OpacityAnim = new QPropertyAnimation(this, "opacity", this);
    m_SlideAnim   = new QPropertyAnimation(this, "x", this);

    m_ContentSlideAnim = new QVariantAnimation(this);
    m_ContentSlideAnim->setDuration(150);
    m_ContentSlideAnim->setEasingCurve(QEasingCurve::OutQuad);
    connect(m_ContentSlideAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& val) {
        m_ContentOffset = val.toReal();
        forceRepaint();
    });
    connect(m_ContentSlideAnim, &QVariantAnimation::finished, this, [this]() {
        m_ContentOffset = 0;
        forceRepaint();
    });

    m_LeaveTimer.setSingleShot(true);
    connect(&m_LeaveTimer, &QTimer::timeout, this, [this]() {
        if (!m_Visible || !m_CloseWhenPointerOutside) {
            return;
        }

        // A Wayland child surface has reliable enter/leave events but no
        // compositor-wide cursor position. Re-entry cancels this timer in
        // event(), so an outstanding timeout means the pointer stayed out.
        if (OverlayWindowPolicy::usesParentCoordinates(m_WindowMode)) {
            closeMenu();
            return;
        }

        const QRect contentGeometry = geometry().adjusted(
                m_ShadowMargin, m_ShadowMargin,
                -m_ShadowMargin, -m_ShadowMargin);
        if (!contentGeometry.contains(QCursor::pos())) {
            closeMenu();
        }
        else {
            schedulePointerOutsideCheck();
        }
    });

    // Keeps the Session level's numbers live while it is shown
    m_SessionStatsTimer.setInterval(1000);
    connect(&m_SessionStatsTimer, &QTimer::timeout, this, [this]() {
        if (!m_Visible || m_CurrentLevel != m_SessionStatsLevel) {
            m_SessionStatsTimer.stop();
            return;
        }
        refreshSessionStats();
        forceRepaint();
    });

    m_BitrateCommitTimer.setSingleShot(true);
    connect(&m_BitrateCommitTimer, &QTimer::timeout, this, [this]() {
        commitBitrateNow();
    });

    // Bindings can only change in the settings page, never mid-stream
    m_ShortcutProfile = ShortcutProfile::loadFromSettings();

    buildMenuLevels();
}

OverlayMenuPanel::~OverlayMenuPanel()
{
}

// ---------------------------------------------------------------------------
// Synchronous repaint — requestUpdate() is async on Windows and may delay
// the visual update by up to 1 second (until the next SDL event arrives).
// This method directly delivers an UpdateRequest event so the paintEvent()
// runs immediately within the current call frame.
// ---------------------------------------------------------------------------

void OverlayMenuPanel::forceRepaint()
{
    if (isExposed()) {
        // Mark entire window as dirty (sets the dirty region for QPaintDeviceWindow)
        update(QRect(0, 0, width(), height()));
        // Synchronously deliver UpdateRequest to trigger paintEvent + backing store flush
        QEvent ev(QEvent::UpdateRequest);
        QCoreApplication::sendEvent(this, &ev);
    }
}

// ---------------------------------------------------------------------------
// Menu structure
// ---------------------------------------------------------------------------

QString OverlayMenuPanel::shortcutActionId(MenuAction action)
{
    switch (action) {
    case MenuAction::Quit:
        return QLatin1String(ShortcutActions::Quit);
    case MenuAction::QuitAndExit:
        return QLatin1String(ShortcutActions::QuitAndExit);
    case MenuAction::ToggleFullScreen:
        return QLatin1String(ShortcutActions::ToggleFullScreen);
    case MenuAction::ToggleStatsOverlay:
        return QLatin1String(ShortcutActions::ToggleStatsOverlay);
    case MenuAction::ToggleMouseMode:
        return QLatin1String(ShortcutActions::ToggleMouseMode);
    case MenuAction::ToggleCursorHide:
        return QLatin1String(ShortcutActions::ToggleCursorHide);
    case MenuAction::ToggleMinimize:
        return QLatin1String(ShortcutActions::Minimize);
    case MenuAction::UngrabInput:
        return QLatin1String(ShortcutActions::UngrabInput);
    case MenuAction::PasteText:
        return QLatin1String(ShortcutActions::PasteText);
    case MenuAction::TogglePointerRegionLock:
        return QLatin1String(ShortcutActions::TogglePointerRegionLock);
    default:
        return QString();
    }
}

QString OverlayMenuPanel::shortcutDetail(MenuAction action) const
{
    QStringList texts;
    for (const ShortcutBinding& binding : m_ShortcutProfile.bindings(shortcutActionId(action))) {
        if (!binding.isNull()) {
            texts.append(binding.toDisplayString());
        }
    }
    return texts.join(QStringLiteral(" / "));
}

OverlayMenuPanel::MenuTab OverlayMenuPanel::tabForAction(MenuAction action)
{
    const int order = tabAssignmentOrder(action);
    return order < kTabAssignmentCount ? kTabAssignments[order].tab : MenuTab::Quick;
}

QString OverlayMenuPanel::tabTitle(MenuTab tab)
{
    switch (tab) {
    case MenuTab::Quick:
        return tr("Quick");
    case MenuTab::Display:
        return tr("Display");
    case MenuTab::Input:
        return tr("Input");
    case MenuTab::Host:
        return tr("Host");
    case MenuTab::Session:
        return tr("Session");
    case MenuTab::Count:
        break;
    }
    return QString();
}

bool OverlayMenuPanel::isTabRootLevel(int level) const
{
    return level >= 0 && level < static_cast<int>(m_MenuLevels.size()) &&
           m_MenuLevels[level].tab.has_value();
}

int OverlayMenuPanel::tabRootLevel() const
{
    for (int level : m_TabLevels) {
        if (*m_MenuLevels[level].tab == m_CurrentTab) {
            return level;
        }
    }
    // The remembered tab is empty right now; show the first one instead
    // but keep remembering it, in case its entries come back.
    return m_TabLevels.empty() ? 0 : m_TabLevels.front();
}

int OverlayMenuPanel::levelForOpenAction(MenuAction action) const
{
    if (action == MenuAction::MenuActionMax) {
        return -1;
    }
    for (int i = 0; i < static_cast<int>(m_MenuLevels.size()); i++) {
        if (m_MenuLevels[i].openAction == action) {
            return i;
        }
    }
    return -1;
}

OverlayMenuPanel::MenuItem* OverlayMenuPanel::findRootItem(MenuAction action)
{
    for (int level : m_TabLevels) {
        for (MenuItem& item : m_MenuLevels[level].items) {
            if (item.action == action) {
                return &item;
            }
        }
    }
    return nullptr;
}

std::vector<OverlayMenuPanel::MenuTab> OverlayMenuPanel::visibleTabs() const
{
    std::vector<MenuTab> tabs;
    for (int level : m_TabLevels) {
        tabs.push_back(*m_MenuLevels[level].tab);
    }
    return tabs;
}

OverlayMenuPanel::MenuTab OverlayMenuPanel::currentTab() const
{
    const int level = tabRootLevel();
    return isTabRootLevel(level) ? *m_MenuLevels[level].tab : m_CurrentTab;
}

void OverlayMenuPanel::buildMenuLevels()
{
    // Level indices change between builds; follow an open sub-level by the
    // action of the entry that opened it.
    const MenuAction openLevelAction =
        m_CurrentLevel >= 0 && m_CurrentLevel < static_cast<int>(m_MenuLevels.size())
            ? m_MenuLevels[m_CurrentLevel].openAction
            : MenuAction::MenuActionMax;

    // The Stream Quality level is appended after the fixed ones, so its index
    // can change between builds; follow it by identity.
    const bool onStreamQualityLevel =
        m_StreamQualityLevel >= 0 && m_CurrentLevel == m_StreamQualityLevel;
    const bool inSessionStatsLevel =
        m_SessionStatsLevel >= 0 && m_CurrentLevel == m_SessionStatsLevel;

    m_MenuLevels.clear();
    m_TabLevels.clear();

    // === Top-level entries. tabForAction() files them into tabs; their
    // order inside a tab is the order of the tab table. ===
    std::vector<MenuItem> entries;

    QString statsDetail = shortcutDetail(MenuAction::ToggleStatsOverlay);
    if (m_HasGamepads) {
        // 手柄组合键与键盘快捷键并列展示,按键名按实体手柄风格显示;
        // swapFaceButtons 时同样补偿到实际要按的物理键
        const int statsFace = m_SwapFaceButtons ? 3 : 2;
        if (!statsDetail.isEmpty()) {
            statsDetail += QString::fromUtf8(" \xc2\xb7 ");
        }
        statsDetail += gamepadSelectButtonName(m_GamepadUiStyle) + QStringLiteral("+") +
                       gamepadLeftShoulderName(m_GamepadUiStyle) + QStringLiteral("+") +
                       gamepadRightShoulderName(m_GamepadUiStyle) + QStringLiteral("+") +
                       gamepadFaceButtonGlyph(m_GamepadUiStyle, statsFace);
    }
    // Quick
    entries.push_back({ tr("Performance Stats"), statsDetail, MenuItemType::Action,
                        MenuAction::ToggleStatsOverlay, 0, true, false, false });
    entries.push_back({ tr("Mouse Mode"), shortcutDetail(MenuAction::ToggleMouseMode),
                        MenuItemType::Action, MenuAction::ToggleMouseMode, 0, true, false, false });
    entries.push_back({ tr("Toggle Fullscreen"), shortcutDetail(MenuAction::ToggleFullScreen),
                        MenuItemType::Action, MenuAction::ToggleFullScreen, 0, true, false,
                        false });
    entries.push_back({ tr("Minimize"), shortcutDetail(MenuAction::ToggleMinimize),
                        MenuItemType::Action, MenuAction::ToggleMinimize, 0, true, false, true });
    entries.push_back({ tr("Microphone"), QString(), MenuItemType::Toggle,
                        MenuAction::ToggleMicrophone, 0, true, false, true });
    entries.push_back({ tr("Disconnect"), m_HasGamepads ? m_QuitComboGlyphs : QString(),
                        MenuItemType::Action, MenuAction::Quit, 0, true, false, false });
    entries.push_back({ tr("Quit Moonlight"), shortcutDetail(MenuAction::QuitAndExit),
                        MenuItemType::Action, MenuAction::QuitAndExit, 0, true, false, false });
    // Display. SubMenu target levels are filled in once the levels exist.
    entries.push_back({ tr("Performance Overlay"), QString(), MenuItemType::SubMenu,
                        MenuAction::OpenStatsOverlay, 0, true, false, false });
    entries.push_back({ tr("Bitrate"), QString(), MenuItemType::SubMenu, MenuAction::OpenBitrate, 0,
                        true, false, false });
    entries.push_back({ tr("Menu Position"), QString(), MenuItemType::SubMenu,
                        MenuAction::OpenMenuPosition, 0, true, false, false });
    // Input
    entries.push_back({ tr("Send Keys"), QString(), MenuItemType::SubMenu, MenuAction::OpenSendKeys,
                        0, true, false, true });
    // Only show Gamepad Mouse toggle when a gamepad is actually connected
    if (m_HasGamepads) {
        entries.push_back({ tr("Gamepad Mouse"), QString(), MenuItemType::Toggle,
                            MenuAction::ToggleGamepadMouse, 0, true, false, false });
    }
    entries.push_back({ tr("Show/Hide Cursor"), shortcutDetail(MenuAction::ToggleCursorHide),
                        MenuItemType::Action, MenuAction::ToggleCursorHide, 0, true, false,
                        false });
    entries.push_back({ tr("Ungrab Mouse"), shortcutDetail(MenuAction::UngrabInput),
                        MenuItemType::Action, MenuAction::UngrabInput, 0, true, false, false });
    entries.push_back({ tr("Pointer Region Lock"),
                        shortcutDetail(MenuAction::TogglePointerRegionLock), MenuItemType::Action,
                        MenuAction::TogglePointerRegionLock, 0, true, false, false });
    entries.push_back({ tr("Paste Clipboard"), shortcutDetail(MenuAction::PasteText),
                        MenuItemType::Action, MenuAction::PasteText, 0, true, false,
                        m_RemoteUsbAvailable });
    if (m_RemoteUsbAvailable) {
        entries.push_back({ tr("USB Devices"),
                            m_RemoteUsbNeedsActivation ? tr("Activate device") : m_RemoteUsbDetail,
                            MenuItemType::SubMenu, MenuAction::OpenRemoteUsbDevices, 0, true, false,
                            false });
    }
    // Host
    if (m_HostCommandsSupported) {
        entries.push_back(
            { tr("Host Commands"),
              m_HostCommands.empty() ? QString() : QString::number(m_HostCommands.size()),
              MenuItemType::SubMenu, MenuAction::OpenHostCommands, 0, true, false, false });
    }
    entries.push_back({ tr("Host Files"), m_FileMappingDetail, MenuItemType::Action,
                        MenuAction::ShowHostFiles, 0, true,
                        m_FileMappingState == FileMappingState::Available ||
                            m_FileMappingState == FileMappingState::Open,
                        false });
    // Session
#ifdef MOONLIGHT_ENABLE_FUNCTION_TESTS
    entries.push_back({ tr("Function Tests"), tr("Developer"), MenuItemType::Action,
                        MenuAction::OpenStylusReplayPanel, 0, true, false, false });
#endif

    std::stable_sort(entries.begin(), entries.end(), [](const MenuItem& a, const MenuItem& b) {
        return tabAssignmentOrder(a.action) < tabAssignmentOrder(b.action);
    });

    // === Tab roots, one level per tab with entries (empty tabs are hidden) ===
    for (int t = 0; t < static_cast<int>(MenuTab::Count); t++) {
        MenuLevel tabLevel;
        tabLevel.title = tr("Overlay Menu");
        tabLevel.tab = static_cast<MenuTab>(t);
        for (const MenuItem& item : entries) {
            if (tabForAction(item.action) == *tabLevel.tab) {
                tabLevel.items.push_back(item);
            }
        }
        if (tabLevel.items.empty()) {
            continue;
        }
        m_TabLevels.push_back(static_cast<int>(m_MenuLevels.size()));
        m_MenuLevels.push_back(std::move(tabLevel));
    }

    // === Bitrate (piecewise-linear scrubber row + presets) ===
    MenuLevel bitrate;
    bitrate.title = tr("Bitrate");
    bitrate.openAction = MenuAction::OpenBitrate;
    bitrate.items.push_back({QString(), QString(), MenuItemType::Slider,
                             MenuAction::MenuActionMax, 0, true, false, true});
    static const int kBitratePresets[] = {
        1000, 2000, 5000, 10000, 20000, 30000, 50000, 100000
    };
    for (int kbps : kBitratePresets) {
        bitrate.items.push_back({formatBitrateKbps(kbps), QString(), MenuItemType::Action,
                                 MenuAction::SetBitrate, 0, true, false, false,
                                 QString::number(kbps)});
    }
    m_MenuLevels.push_back(std::move(bitrate));

    // === Overlay menu placement ===
    MenuLevel placement;
    placement.title = tr("Menu Position");
    placement.openAction = MenuAction::OpenMenuPosition;
    placement.items.push_back({tr("Top edge"), QString(), MenuItemType::Action,
                               MenuAction::SetMenuPlacementTop, 0, true, false, false});
    placement.items.push_back({tr("Right edge"), QString(), MenuItemType::Action,
                               MenuAction::SetMenuPlacementRight, 0, true, false, false});
    placement.items.push_back({tr("Left edge"), QString(), MenuItemType::Action,
                               MenuAction::SetMenuPlacementLeft, 0, true, false, false});
    placement.items.push_back({tr("Floating button"), QString(), MenuItemType::Action,
                               MenuAction::SetMenuPlacementButton, 0, true, false, false});
    placement.items.push_back({tr("Disabled"), QString(), MenuItemType::Action,
                               MenuAction::SetMenuPlacementDisabled, 0, true, false, false});
    m_MenuLevels.push_back(std::move(placement));

    // === Remote USB devices ===
    if (m_RemoteUsbAvailable) {
        MenuLevel usb;
        usb.title = tr("USB Devices");
        usb.openAction = MenuAction::OpenRemoteUsbDevices;
        if (m_RemoteUsbDevices.empty()) {
            const QString emptyDetail =
                m_RemoteUsbState == RemoteUsbState::Discovering
                    ? tr("Scanning") : tr("No devices found");
            usb.items.push_back({tr("USB Devices"), emptyDetail,
                                 MenuItemType::Action,
                                 MenuAction::MenuActionMax, 0, false, false,
                                 false});
        }
        else {
            for (const RemoteUsbDevice& device : m_RemoteUsbDevices) {
                const bool active = !m_RemoteUsbActiveDeviceId.isEmpty() &&
                                    device.id == m_RemoteUsbActiveDeviceId;
                QString detail = device.detail;
                if (active) {
                    switch (m_RemoteUsbState) {
                    case RemoteUsbState::Opening:
                        detail = tr("Connecting — select to cancel");
                        break;
                    case RemoteUsbState::Open:
                        detail = tr("Connected — select to release");
                        break;
                    case RemoteUsbState::Stopping:
                        detail = tr("Releasing");
                        break;
                    default:
                        break;
                    }
                }
                const bool busy = m_RemoteUsbState == RemoteUsbState::Opening ||
                                  m_RemoteUsbState == RemoteUsbState::Stopping;
                const bool hasOpenDevice =
                    m_RemoteUsbState == RemoteUsbState::Open &&
                    !m_RemoteUsbActiveDeviceId.isEmpty();
                if (m_RemoteUsbNeedsActivation && device.supported) {
                    detail = tr("Select to activate");
                    if (!device.detail.isEmpty()) {
                        detail += QStringLiteral(" · ") + device.detail;
                    }
                }
                const bool canRelease = active &&
                    (m_RemoteUsbState == RemoteUsbState::Opening ||
                     m_RemoteUsbState == RemoteUsbState::Open);
                usb.items.push_back({device.label, detail,
                                     MenuItemType::Action,
                                     canRelease
                                         ? MenuAction::ReleaseRemoteUsbDevice
                                         : MenuAction::SelectRemoteUsbDevice,
                                     0,
                                     canRelease || (device.supported && !busy &&
                                         (!hasOpenDevice || active)),
                                     active,
                                     false,
                                     device.id});
            }
        }
        m_MenuLevels.push_back(std::move(usb));
    }

    // === Performance overlay ===
    MenuLevel stats;
    stats.title = tr("Performance Overlay");
    stats.openAction = MenuAction::OpenStatsOverlay;
    stats.items.push_back({ tr("Off"), QString(), MenuItemType::Action,
                            MenuAction::SetStatsPresetOff, 0, true, false, false });
    stats.items.push_back({ tr("Minimal"), QString(), MenuItemType::Action,
                            MenuAction::SetStatsPresetMinimal, 0, true, false, false });
    stats.items.push_back({ tr("Compact"), QString(), MenuItemType::Action,
                            MenuAction::SetStatsPresetCompact, 0, true, false, false });
    stats.items.push_back({ tr("Detailed"), QString(), MenuItemType::Action,
                            MenuAction::SetStatsPresetDetailed, 0, true, false, false });
    stats.items.push_back({ tr("Custom"), QString(), MenuItemType::Action,
                            MenuAction::SetStatsPresetCustom, 0, true, false, true });
    stats.items.push_back({ tr("Edit position"), QString(), MenuItemType::Action,
                            MenuAction::EditStatsOverlayPosition, 0, m_StatsOverlayEditable, false,
                            false });
    m_MenuLevels.push_back(std::move(stats));

    // === Send Keys: special keys, then the user's key macros ===
    MenuLevel sendKeys;
    sendKeys.title = tr("Send Keys");
    sendKeys.openAction = MenuAction::OpenSendKeys;
    for (size_t i = 0; i < m_SpecialKeyItems.size(); i++) {
        const SendKeyItem& key = m_SpecialKeyItems[i];
        sendKeys.items.push_back(
            { key.label, key.detail, MenuItemType::Action, MenuAction::SendHostKeys, 0, true, false,
              i + 1 == m_SpecialKeyItems.size() && !m_KeyMacroItems.empty(), key.payload });
    }
    for (const SendKeyItem& key : m_KeyMacroItems) {
        sendKeys.items.push_back({ key.label, key.detail, MenuItemType::Action,
                                   MenuAction::SendHostKeys, 0, true, false, false, key.payload });
    }
    if (sendKeys.items.empty()) {
        sendKeys.items.push_back({ tr("No keys"), QString(), MenuItemType::Action,
                                   MenuAction::MenuActionMax, 0, false, false, false });
    }
    m_MenuLevels.push_back(std::move(sendKeys));

    // === Host Commands (Foundation Sunshine super commands) ===
    if (m_HostCommandsSupported) {
        MenuLevel hostCommands;
        hostCommands.title = tr("Host Commands");
        hostCommands.openAction = MenuAction::OpenHostCommands;
        for (const HostCommandItem& command : m_HostCommands) {
            hostCommands.items.push_back({ command.name, QString(), MenuItemType::Action,
                                           MenuAction::RunHostCommand, 0, true, false, false,
                                           command.id });
        }
        if (hostCommands.items.empty()) {
            hostCommands.items.push_back(
                { tr("No host commands"), tr("Set them up in Foundation Sunshine"),
                  MenuItemType::Action, MenuAction::MenuActionMax, 0, false, false, false });
        }
        m_MenuLevels.push_back(std::move(hostCommands));
    }

    // === Stream Quality (resolution / FPS for this session) ===
    m_StreamQualityLevel = static_cast<int>(m_MenuLevels.size());
    m_MenuLevels.push_back(buildStreamQualityLevel());
    for (MenuItem& item : m_MenuLevels[0].items) {
        if (item.action == MenuAction::OpenStreamQuality) {
            item.targetLevel = m_StreamQualityLevel;
        }
    }
    if (onStreamQualityLevel) {
        m_CurrentLevel = m_StreamQualityLevel;
    }

    // === Session (live totals) ===
    MenuLevel session;
    session.title = tr("Session");
    m_SessionStatsLevel = static_cast<int>(m_MenuLevels.size());
    m_MenuLevels.push_back(std::move(session));
    for (MenuItem& item : m_MenuLevels[0].items) {
        if (item.action == MenuAction::OpenSessionStats) {
            item.targetLevel = m_SessionStatsLevel;
        }
    }
    if (inSessionStatsLevel) {
        m_CurrentLevel = m_SessionStatsLevel;
    }
    refreshSessionStats();

    // Point SubMenu rows at their levels by the Open* action they carry. This
    // covers tab entries and rows inside a sub-level that link to another
    // one (e.g. a Bitrate row tagged MenuAction::OpenBitrate).
    for (MenuLevel& level : m_MenuLevels) {
        for (MenuItem& item : level.items) {
            if (item.type == MenuItemType::SubMenu) {
                const int target = levelForOpenAction(item.action);
                if (target >= 0) {
                    item.targetLevel = target;
                }
            }
        }
    }
    m_StatsOverlayLevel = levelForOpenAction(MenuAction::OpenStatsOverlay);
    m_SendKeysLevel = levelForOpenAction(MenuAction::OpenSendKeys);
    m_HostCommandsLevel = levelForOpenAction(MenuAction::OpenHostCommands);
    applyStatsOverlayState();

    // Stay in the open sub-level if it still exists, else in the tab
    const int reopenedLevel = levelForOpenAction(openLevelAction);
    m_CurrentLevel = reopenedLevel >= 0 ? reopenedLevel : tabRootLevel();

    // Rebuilds (gamepad set / USB refresh) wipe derived details; re-stamp
    // the bitrate state so the scrubber row and preset checkmarks survive.
    refreshBitrateDetails();
}

// ---------------------------------------------------------------------------
// Dynamic state updates
// ---------------------------------------------------------------------------

void OverlayMenuPanel::updateMicrophoneState(bool enabled)
{
    if (MenuItem* item = findRootItem(MenuAction::ToggleMicrophone)) {
        item->toggleState = enabled;
        forceRepaint();
    }
}

void OverlayMenuPanel::updateGamepadMouseState(bool enabled)
{
    if (MenuItem* item = findRootItem(MenuAction::ToggleGamepadMouse)) {
        item->toggleState = enabled;
        forceRepaint();
    }
}

void OverlayMenuPanel::updateBitrateState(int bitrateKbps)
{
    if (m_MenuLevels.empty()) return;

    if (m_BitrateCommitTimer.isActive()) {
        // A scrub is still pending commit; it will land shortly and update
        // the preference. Don't clobber the slider with the stale value.
        return;
    }

    m_BitrateKbps = qBound(kBitrateMinKbps, bitrateKbps, kBitrateMaxKbps);
    m_CommittedBitrateKbps = m_BitrateKbps;
    refreshBitrateDetails();
    forceRepaint();
}

// ---------------------------------------------------------------------------
// Bitrate slider row
// ---------------------------------------------------------------------------

QString OverlayMenuPanel::formatBitrateKbps(int kbps)
{
    if (kbps >= 1000000) {
        const bool whole = kbps % 1000000 == 0;
        return QStringLiteral("%1 Gbps").arg(kbps / 1000000.0, 0, 'f', whole ? 0 : 1);
    }
    if (kbps >= 1000) {
        const bool whole = kbps % 1000 == 0;
        return QStringLiteral("%1 Mbps").arg(kbps / 1000.0, 0, 'f', whole ? 0 : 1);
    }
    return QStringLiteral("%1 kbps").arg(kbps);
}

void OverlayMenuPanel::refreshBitrateDetails()
{
    if (m_MenuLevels.empty()) return;

    // Current bitrate as detail text on the Bitrate entry of its tab
    if (MenuItem* entry = findRootItem(MenuAction::OpenBitrate)) {
        entry->detail = formatBitrateKbps(m_BitrateKbps);
    }

    // Mark the active preset (✓) in the Bitrate level; custom values stay
    // unmarked since the scrubber row itself shows the exact value.
    const int bitrateLevel = levelForOpenAction(MenuAction::OpenBitrate);
    if (bitrateLevel >= 0) {
        for (auto& item : m_MenuLevels[bitrateLevel].items) {
            if (item.action == MenuAction::SetBitrate) {
                item.detail = (item.payload.toInt() == m_BitrateKbps)
                                  ? QString::fromUtf8("\342\234\223") : QString();
            }
        }
    }

    // The Stream Quality level links to the Bitrate level and shows its value.
    if (m_StreamQualityLevel >= 0 && m_StreamQualityLevel < (int)m_MenuLevels.size()) {
        for (auto& item : m_MenuLevels[m_StreamQualityLevel].items) {
            if (item.type == MenuItemType::SubMenu && item.targetLevel == 2) {
                item.detail = formatBitrateKbps(m_BitrateKbps);
            }
        }
    }
}

double OverlayMenuPanel::bitrateFraction() const
{
    return qBound(0.0, bitrateToSliderPosition(m_BitrateKbps) / kBitrateSliderMax, 1.0);
}

void OverlayMenuPanel::setBitrateKbps(int bitrateKbps)
{
    bitrateKbps = qBound(kBitrateMinKbps, bitrateKbps, kBitrateMaxKbps);
    bitrateKbps = sliderPositionToBitrate(bitrateToSliderPosition(bitrateKbps));
    if (bitrateKbps == m_BitrateKbps) return;

    m_BitrateKbps = bitrateKbps;
    refreshBitrateDetails();
    m_BitrateCommitTimer.start(kBitrateCommitDelayMs);
    forceRepaint();
}

void OverlayMenuPanel::setBitrateFromFraction(double fraction)
{
    fraction = qBound(0.0, fraction, 1.0);
    setBitrateKbps(sliderPositionToBitrate(fraction * kBitrateSliderMax));
}

void OverlayMenuPanel::adjustBitrateStep(int direction, int multiplier)
{
    const double position = bitrateToSliderPosition(m_BitrateKbps);
    const int alignedPosition = direction > 0 ? qFloor(position) : qCeil(position);
    const int nextPosition = alignedPosition + direction * qMax(1, multiplier);
    setBitrateKbps(sliderPositionToBitrate(nextPosition));
}

void OverlayMenuPanel::commitBitrateNow()
{
    m_BitrateCommitTimer.stop();
    if (m_BitrateKbps == m_CommittedBitrateKbps) return;

    m_CommittedBitrateKbps = m_BitrateKbps;
    if (m_BitrateChangeCallback) {
        m_BitrateChangeCallback(m_BitrateKbps);
    }
}

void OverlayMenuPanel::selectBitratePreset(const MenuItem& item)
{
    // Presets snap the scrubber and commit immediately, but keep the menu
    // open so the value can be fine-tuned right away.
    beginInteraction();
    m_BitrateKbps = qBound(kBitrateMinKbps, item.payload.toInt(), kBitrateMaxKbps);
    refreshBitrateDetails();
    commitBitrateNow();
    forceRepaint();
}

OverlayMenuPanel::SliderRowRects OverlayMenuPanel::sliderRowRects(int contentWidth, int itemY) const
{
    const int textPad = 16;
    const int buttonSize = 22;
    const int buttonGap = 4;

    SliderRowRects r;
    r.value = QRect(textPad, itemY, 78, m_ItemHeight);
    r.plus = QRect(contentWidth - textPad - buttonSize,
                   itemY + (m_ItemHeight - buttonSize) / 2, buttonSize, buttonSize);
    r.minus = QRect(r.plus.x() - buttonSize - buttonGap, r.plus.y(),
                    buttonSize, buttonSize);
    const int trackLeft = r.value.right() + 1 + 8;
    const int trackRight = r.minus.x() - 6;
    r.track = QRect(trackLeft, itemY + m_ItemHeight / 2 - 3,
                    qMax(0, trackRight - trackLeft), 6);
    return r;
}

OverlayMenuPanel::SliderZone OverlayMenuPanel::sliderZoneAt(const QPoint& windowPos, int rowIdx) const
{
    const auto& items = m_MenuLevels[m_CurrentLevel].items;
    if (rowIdx < 0 || rowIdx >= (int)items.size()
            || items[rowIdx].type != MenuItemType::Slider) {
        return SliderZone::None;
    }

    // Row rects live in content coordinates; callers hand us window coords.
    const QPoint contentPos = windowPos - QPoint(m_ShadowMargin, m_ShadowMargin);
    const int itemY = contentTop() + rowIdx * m_ItemHeight;
    const SliderRowRects r = sliderRowRects(width() - 2 * m_ShadowMargin, itemY);

    // Buttons win over the track so their hit area feels solid.
    if (r.minus.adjusted(-2, -2, 2, 2).contains(contentPos)) return SliderZone::Minus;
    if (r.plus.adjusted(-2, -2, 2, 2).contains(contentPos)) return SliderZone::Plus;
    if (QRect(r.track.x() - 4, itemY, r.track.width() + 8, m_ItemHeight).contains(contentPos)) {
        return SliderZone::Track;
    }
    return SliderZone::None;
}

void OverlayMenuPanel::gamepadAdjustSlider(int direction)
{
    if (!m_Visible) return;
    const auto& items = m_MenuLevels[m_CurrentLevel].items;

    // D-pad left/right anywhere in the scrubber's submenu drives the slider;
    // focus follows so the highlight shows what is being adjusted.
    int rowIdx = -1;
    for (int i = 0; i < (int)items.size(); i++) {
        if (items[i].type == MenuItemType::Slider) {
            rowIdx = i;
            break;
        }
    }
    if (rowIdx < 0) return;

    beginInteraction();
    if (m_HoveredIndex != rowIdx) {
        m_HoveredIndex = rowIdx;
        m_SliderAdjustStreak = 0;
        forceRepaint();
    }
    if (m_SliderAdjustClock.isValid() && m_SliderAdjustClock.elapsed() <= 350) {
        m_SliderAdjustStreak++;
    } else {
        m_SliderAdjustStreak = 0;
    }
    m_SliderAdjustClock.start();
    // 1x → 2x → 4x → 8x (capped) while the d-pad is held or rapidly tapped.
    adjustBitrateStep(direction, 1 << qMin(m_SliderAdjustStreak / 2, 3));
}

void OverlayMenuPanel::updateMenuPositionState(MenuAction activePlacementAction)
{
    const int placementLevel = levelForOpenAction(MenuAction::OpenMenuPosition);
    if (placementLevel < 0) {
        return;
    }

    QString activeLabel;
    for (auto& item : m_MenuLevels[placementLevel].items) {
        const bool active = item.action == activePlacementAction;
        item.detail = active ? QString::fromUtf8("\342\234\223") : QString();
        if (active) {
            activeLabel = item.label;
        }
    }

    if (MenuItem* entry = findRootItem(MenuAction::OpenMenuPosition)) {
        entry->detail = activeLabel;
    }
    forceRepaint();
}

std::optional<StatsOverlay::Preset> OverlayMenuPanel::statsPresetForAction(MenuAction action)
{
    switch (action) {
    case MenuAction::SetStatsPresetOff:
        return StatsOverlay::Preset::Off;
    case MenuAction::SetStatsPresetMinimal:
        return StatsOverlay::Preset::Minimal;
    case MenuAction::SetStatsPresetCompact:
        return StatsOverlay::Preset::Compact;
    case MenuAction::SetStatsPresetDetailed:
        return StatsOverlay::Preset::Detailed;
    case MenuAction::SetStatsPresetCustom:
        return StatsOverlay::Preset::Custom;
    default:
        return std::nullopt;
    }
}

void OverlayMenuPanel::updateStatsOverlayState(StatsOverlay::Preset preset, bool canEditPosition)
{
    m_StatsOverlayPreset = preset;
    m_StatsOverlayEditable = canEditPosition;
    applyStatsOverlayState();
    forceRepaint();
}

void OverlayMenuPanel::setSessionStatsProvider(SessionStatsProvider provider)
{
    m_SessionStatsProvider = std::move(provider);
    refreshSessionStats();
}

void OverlayMenuPanel::refreshSessionStats()
{
    if (m_SessionStatsLevel < 0 ||
        m_MenuLevels.size() <= static_cast<size_t>(m_SessionStatsLevel)) {
        return;
    }

    std::vector<std::pair<QString, QString>> rows;
    if (m_SessionStatsProvider) {
        rows = m_SessionStatsProvider();
    }
    if (rows.empty()) {
        rows.emplace_back(tr("No stats yet"), QString());
    }

    auto& items = m_MenuLevels[m_SessionStatsLevel].items;
    const size_t previousCount = items.size();
    items.clear();
    for (auto& row : rows) {
        // Read-only rows: disabled, so they never take focus or dispatch
        items.push_back({ std::move(row.first), std::move(row.second), MenuItemType::Action,
                          MenuAction::OpenSessionStats, 0, false, false, false });
    }

    if (previousCount != items.size() && m_Visible && m_CurrentLevel == m_SessionStatsLevel) {
        repositionWindow();
    }
}

void OverlayMenuPanel::applyStatsOverlayState()
{
    if (m_StatsOverlayLevel < 0 ||
        m_MenuLevels.size() <= static_cast<size_t>(m_StatsOverlayLevel)) {
        return;
    }

    QString activeLabel;
    for (auto& item : m_MenuLevels[m_StatsOverlayLevel].items) {
        if (item.action == MenuAction::EditStatsOverlayPosition) {
            item.enabled = m_StatsOverlayEditable;
            continue;
        }
        const bool active = statsPresetForAction(item.action) == m_StatsOverlayPreset;
        item.detail = active ? QString::fromUtf8("\342\234\223") : QString();
        if (active) {
            activeLabel = item.label;
        }
    }

    if (MenuItem* entry = findRootItem(MenuAction::OpenStatsOverlay)) {
        entry->detail = activeLabel;
    }
}

void OverlayMenuPanel::updateFileMappingState(FileMappingState state, const QString& detail)
{
    m_FileMappingState = state;
    m_FileMappingDetail = detail;

    if (MenuItem* item = findRootItem(MenuAction::ShowHostFiles)) {
        item->detail = detail;
        item->toggleState = state == FileMappingState::Available || state == FileMappingState::Open;
        forceRepaint();
    }
}

void OverlayMenuPanel::updateRemoteUsbState(
    bool available,
    RemoteUsbState state,
    std::vector<RemoteUsbDevice> devices,
    const QString& activeDeviceId,
    const QString& detail)
{
    m_RemoteUsbAvailable = available;
    m_RemoteUsbState = state;
    m_RemoteUsbDevices = std::move(devices);
    m_RemoteUsbActiveDeviceId = activeDeviceId;
    m_RemoteUsbNeedsActivation =
        available && state == RemoteUsbState::Available && activeDeviceId.isEmpty() &&
        std::any_of(m_RemoteUsbDevices.cbegin(), m_RemoteUsbDevices.cend(),
                    [](const RemoteUsbDevice& device) { return device.supported; });
    m_RemoteUsbDetail = detail;
    // The rebuild keeps an open USB level open
    buildMenuLevels();
    if (m_Visible) {
        repositionWindow();
    }
    forceRepaint();
}

void OverlayMenuPanel::updateSendKeys(std::vector<SendKeyItem> specialKeys,
                                      std::vector<SendKeyItem> macros)
{
    m_SpecialKeyItems = std::move(specialKeys);
    m_KeyMacroItems = std::move(macros);
    buildMenuLevels();
    if (m_Visible) {
        repositionWindow();
    }
    forceRepaint();
}

void OverlayMenuPanel::updateHostCommands(bool supported, std::vector<HostCommandItem> commands)
{
    m_HostCommandsSupported = supported;
    m_HostCommands = std::move(commands);
    buildMenuLevels();
    if (m_Visible) {
        repositionWindow();
    }
    forceRepaint();
}

bool OverlayMenuPanel::showHostCommandsLevel()
{
    if (!m_Visible || m_HostCommandsLevel < 0) {
        return false;
    }
    // Back from the level returns to the tab that holds its entry
    m_CurrentTab = tabForAction(MenuAction::OpenHostCommands);
    navigateToLevel(m_HostCommandsLevel);
    return true;
}

bool OverlayMenuPanel::showSendKeysLevel()
{
    if (!m_Visible || m_SendKeysLevel < 0) {
        return false;
    }
    m_CurrentTab = tabForAction(MenuAction::OpenSendKeys);
    navigateToLevel(m_SendKeysLevel);
    return true;
}

bool OverlayMenuPanel::showTab(MenuTab tab)
{
    if (!m_Visible) {
        return false;
    }
    for (int level : m_TabLevels) {
        if (*m_MenuLevels[level].tab == tab) {
            beginInteraction();
            showTabLevel(level, 0);
            return true;
        }
    }
    return false;
}

bool OverlayMenuPanel::showLevel(MenuAction openAction)
{
    const int level = levelForOpenAction(openAction);
    if (!m_Visible || level < 0) {
        return false;
    }
    m_CurrentTab = tabForAction(openAction);
    navigateToLevel(level);
    return true;
}

int OverlayMenuPanel::rowOfAction(MenuAction action, const QString& payload) const
{
    const auto& items = m_MenuLevels[m_CurrentLevel].items;
    for (int i = 0; i < static_cast<int>(items.size()); i++) {
        if (items[i].action == action && (payload.isEmpty() || items[i].payload == payload)) {
            return i;
        }
    }
    return -1;
}

QRect OverlayMenuPanel::rowGeometry(int row) const
{
    if (row < 0 || row >= static_cast<int>(m_MenuLevels[m_CurrentLevel].items.size())) {
        return QRect();
    }
    return QRect(m_ShadowMargin, m_ShadowMargin + contentTop() + row * m_ItemHeight, m_MenuWidth,
                 m_ItemHeight);
}

QRect OverlayMenuPanel::tabGeometry(MenuTab tab) const
{
    for (int i = 0; i < static_cast<int>(m_TabLevels.size()); i++) {
        if (*m_MenuLevels[m_TabLevels[i]].tab == tab) {
            return tabRect(i, m_MenuWidth).translated(m_ShadowMargin, m_ShadowMargin);
        }
    }
    return QRect();
}

void OverlayMenuPanel::showTabLevel(int level, int direction)
{
    if (!isTabRootLevel(level)) {
        return;
    }
    m_CurrentTab = *m_MenuLevels[level].tab;
    m_ContentSlideAnim->stop();
    m_ContentOffset = 0;
    m_CurrentLevel = level;
    m_HoveredIndex = -1;
    m_SliderHotZone = SliderZone::None;
    repositionWindow();

    if (direction != 0) {
        // Content slides in from the side of the new tab
        m_ContentSlideAnim->setStartValue(8.0 * direction);
        m_ContentSlideAnim->setEndValue(0.0);
        m_ContentSlideAnim->start();
    } else {
        forceRepaint();
    }
}

void OverlayMenuPanel::gamepadSwitchTab(int direction)
{
    if (!m_Visible || m_TabLevels.empty() || direction == 0) {
        return;
    }
    beginInteraction();
    const int count = static_cast<int>(m_TabLevels.size());
    const auto current = std::find(m_TabLevels.cbegin(), m_TabLevels.cend(), tabRootLevel());
    const int index =
        current == m_TabLevels.cend() ? 0 : static_cast<int>(current - m_TabLevels.cbegin());
    const int step = direction > 0 ? 1 : -1;
    showTabLevel(m_TabLevels[((index + step) % count + count) % count], step);
}

void OverlayMenuPanel::gamepadMoveHorizontal(int direction)
{
    if (!m_Visible) {
        return;
    }
    const auto& items = m_MenuLevels[m_CurrentLevel].items;
    const bool hasSlider = std::any_of(items.cbegin(), items.cend(), [](const MenuItem& item) {
        return item.type == MenuItemType::Slider;
    });
    if (hasSlider) {
        gamepadAdjustSlider(direction);
    } else if (isTabRootLevel(m_CurrentLevel)) {
        gamepadSwitchTab(direction);
    }
}

bool OverlayMenuPanel::handleNavigationKey(int key, Qt::KeyboardModifiers modifiers)
{
    if (!m_Visible) {
        return false;
    }
    switch (key) {
    case Qt::Key_Up:
        gamepadMoveUp();
        return true;
    case Qt::Key_Down:
        gamepadMoveDown();
        return true;
    case Qt::Key_Left:
        gamepadMoveHorizontal(-1);
        return true;
    case Qt::Key_Right:
        gamepadMoveHorizontal(1);
        return true;
    case Qt::Key_Tab:
        gamepadSwitchTab(modifiers.testFlag(Qt::ShiftModifier) ? -1 : 1);
        return true;
    case Qt::Key_Backtab:
    case Qt::Key_PageUp:
        gamepadSwitchTab(-1);
        return true;
    case Qt::Key_PageDown:
        gamepadSwitchTab(1);
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        gamepadSelect();
        return true;
    case Qt::Key_Backspace:
    case Qt::Key_Escape:
        gamepadBack();
        return true;
    default:
        return false;
    }
}

OverlayMenuPanel::MenuLevel OverlayMenuPanel::buildStreamQualityLevel() const
{
    const QString check = QString::fromUtf8("\342\234\223");
    auto detailText = [&check](bool selected, const QString& note) {
        if (!selected) {
            return note;
        }
        return note.isEmpty() ? check : check + QStringLiteral(" ") + note;
    };
    auto summary = [](QSize resolution, int fps) {
        return tr("%1 · %2 FPS").arg(StreamQuality::resolutionName(resolution)).arg(fps);
    };
    auto sizeText = [](QSize size) {
        return QStringLiteral("%1%2%3").arg(size.width()).arg(QChar(0x00D7)).arg(size.height());
    };

    MenuLevel level;
    level.title = tr("Stream Quality");

    const bool known = m_StreamQuality.resolution.isValid() && m_StreamQuality.fps > 0;
    const bool changed = known && (m_PendingStreamResolution != m_StreamQuality.resolution ||
                                   m_PendingStreamFps != m_StreamQuality.fps);

    // Apply row first, so the pending choice and its confirmation stay in view.
    level.items.push_back(
        { changed ? tr("Switch to %1").arg(summary(m_PendingStreamResolution, m_PendingStreamFps))
                  : tr("Switch stream"),
          changed ? QString() : tr("Pick a resolution or frame rate"), MenuItemType::Action,
          MenuAction::ApplyStreamQuality, 0, changed, false, true });

    const auto resolutions = StreamQuality::resolutionPresets(m_StreamQuality.nativeResolution,
                                                              m_StreamQuality.resolution);
    for (int i = 0; i < resolutions.size(); i++) {
        const StreamQuality::ResolutionPreset& preset = resolutions[i];
        const QString label =
            preset.native ? tr("Native display") : StreamQuality::resolutionName(preset.size);
        const QString note = preset.current ? tr("Current") : sizeText(preset.size);
        level.items.push_back(
            { label, detailText(preset.size == m_PendingStreamResolution, note),
              MenuItemType::Action, MenuAction::SelectStreamResolution, 0, true, false,
              i == resolutions.size() - 1,
              QStringLiteral("%1x%2").arg(preset.size.width()).arg(preset.size.height()) });
    }

    const auto frameRates =
        StreamQuality::fpsPresets(m_StreamQuality.displayHz, m_StreamQuality.fps);
    for (int i = 0; i < frameRates.size(); i++) {
        const StreamQuality::FpsPreset& preset = frameRates[i];
        const QString note = preset.current   ? tr("Current")
                             : preset.display ? tr("Display")
                                              : QString();
        level.items.push_back({ tr("%1 FPS").arg(preset.fps),
                                detailText(preset.fps == m_PendingStreamFps, note),
                                MenuItemType::Action, MenuAction::SelectStreamFps, 0, true, false,
                                i == frameRates.size() - 1, QString::number(preset.fps) });
    }

    // Bitrate changes apply to the running stream, so they reuse the Bitrate level.
    level.items.push_back({ tr("Bitrate"), formatBitrateKbps(m_BitrateKbps), MenuItemType::SubMenu,
                            MenuAction::MenuActionMax, 2, true, false, false });
    level.items.push_back(
        { tr("Save as Default"),
          known ? summary(m_PendingStreamResolution, m_PendingStreamFps) : QString(),
          MenuItemType::Action, MenuAction::SaveStreamQualityAsDefault, 0, known, false, false });
    return level;
}

void OverlayMenuPanel::updateStreamQualityState(const StreamQualityState& state)
{
    m_StreamQuality = state;
    m_PendingStreamResolution = state.resolution;
    m_PendingStreamFps = state.fps;
    buildMenuLevels();
    if (m_Visible) {
        repositionWindow();
    }
    forceRepaint();
}

bool OverlayMenuPanel::handleStreamQualityItem(const MenuItem& item)
{
    switch (item.action) {
    case MenuAction::SelectStreamResolution:
    case MenuAction::SelectStreamFps: {
        // Rebuilding the level replaces the item, so read it first.
        const MenuAction action = item.action;
        const QString payload = item.payload;
        if (action == MenuAction::SelectStreamResolution) {
            const QStringList parts = payload.split(QLatin1Char('x'));
            if (parts.size() == 2) {
                m_PendingStreamResolution = QSize(parts[0].toInt(), parts[1].toInt());
            }
        } else {
            m_PendingStreamFps = payload.toInt();
        }
        beginInteraction();
        if (m_StreamQualityLevel >= 0 && m_StreamQualityLevel < (int)m_MenuLevels.size()) {
            m_MenuLevels[m_StreamQualityLevel] = buildStreamQualityLevel();
        }
        forceRepaint();
        return true;
    }
    case MenuAction::ApplyStreamQuality:
    case MenuAction::SaveStreamQualityAsDefault: {
        StreamQualityRequest request;
        request.resolution = m_PendingStreamResolution;
        request.fps = m_PendingStreamFps;
        request.saveAsDefault = item.action == MenuAction::SaveStreamQualityAsDefault;
        closeMenu();
        if (m_StreamQualityCallback) {
            m_StreamQualityCallback(request);
        }
        return true;
    }
    default:
        return false;
    }
}

void OverlayMenuPanel::dispatchActionItem(const MenuItem& item)
{
    if (handleStreamQualityItem(item)) {
        return;
    }
    // USB callbacks can synchronously rebuild the device list. Own the payload
    // before calling out so a refresh cannot invalidate the selected identity.
    const QString payload = item.payload;
    if (item.action == MenuAction::SelectRemoteUsbDevice) {
        beginInteraction();
        if (m_RemoteUsbDeviceCallback) {
            m_RemoteUsbDeviceCallback(payload);
        }
        return;
    }
    if (item.action == MenuAction::ReleaseRemoteUsbDevice) {
        beginInteraction();
        if (m_RemoteUsbReleaseCallback) {
            m_RemoteUsbReleaseCallback();
        }
        return;
    }
    if (item.action == MenuAction::SendHostKeys || item.action == MenuAction::RunHostCommand) {
        closeMenu();
        if (m_PayloadActionCallback) {
            m_PayloadActionCallback(item.action, payload);
        }
        return;
    }
    closeMenu();
    if (m_ActionCallback) {
        m_ActionCallback(item.action);
    }
}

// ---------------------------------------------------------------------------
// Show / hide / navigate
// ---------------------------------------------------------------------------

void OverlayMenuPanel::showAtRightEdge(int parentX, int parentY, int parentW, int parentH,
                                       std::optional<QPoint> pointerGlobalPosition,
                                       bool closeWhenPointerOutside)
{
    m_AnchorMode = AnchorMode::RightEdge;
    m_ParentX = parentX;
    m_ParentY = parentY;
    m_ParentW = parentW;
    m_ParentH = parentH;
    m_TriggerPosition = pointerGlobalPosition;
    m_CloseWhenPointerOutside = closeWhenPointerOutside && pointerGlobalPosition.has_value();
    showInternal();
}

void OverlayMenuPanel::showAtLeftEdge(int parentX, int parentY, int parentW, int parentH,
                                      std::optional<QPoint> pointerGlobalPosition,
                                      bool closeWhenPointerOutside)
{
    m_AnchorMode = AnchorMode::LeftEdge;
    m_ParentX = parentX;
    m_ParentY = parentY;
    m_ParentW = parentW;
    m_ParentH = parentH;
    m_TriggerPosition = pointerGlobalPosition;
    m_CloseWhenPointerOutside = closeWhenPointerOutside && pointerGlobalPosition.has_value();
    showInternal();
}

void OverlayMenuPanel::showAtTopEdge(int parentX, int parentY, int parentW, int parentH,
                                     std::optional<QPoint> pointerGlobalPosition,
                                     bool closeWhenPointerOutside)
{
    m_AnchorMode = AnchorMode::TopEdge;
    m_ParentX = parentX;
    m_ParentY = parentY;
    m_ParentW = parentW;
    m_ParentH = parentH;
    m_TriggerPosition = pointerGlobalPosition;
    m_CloseWhenPointerOutside = closeWhenPointerOutside && pointerGlobalPosition.has_value();
    showInternal();
}

void OverlayMenuPanel::showAtCursor(int parentX, int parentY, int parentW, int parentH,
                                    const QPoint& cursorPosition, bool pointerTriggered)
{
    m_AnchorMode = AnchorMode::AtCursor;
    m_ParentX = parentX;
    m_ParentY = parentY;
    m_ParentW = parentW;
    m_ParentH = parentH;
    m_TriggerPosition = cursorPosition;
    m_CloseWhenPointerOutside = pointerTriggered;
    showInternal();
}

void OverlayMenuPanel::showInternal()
{
    m_LeaveTimer.stop();
    // Reopen on the last tab of this session
    m_CurrentLevel = tabRootLevel();
    m_HoveredIndex = -1;
    m_ContentOffset = 0;
    m_WheelAccum = 0;
    m_SliderDragging = false;
    m_SliderPressedZone = SliderZone::None;
    m_SliderHotZone = SliderZone::None;

    // If closing animation is in progress, cancel it
    if (m_Closing) {
        m_OpacityAnim->stop();
        m_SlideAnim->stop();
        m_Closing = false;
    }

    m_Visible = true;
    m_ShowTimer.start();

    // Calculate target geometry
    repositionWindow();
    m_TargetPosition = position();

    // Slide direction depends on anchor mode
    const bool verticalSlide = m_AnchorMode == AnchorMode::TopEdge;
    const int slideDirection = (m_AnchorMode == AnchorMode::LeftEdge || verticalSlide) ? -1 : 1;
    const int slideDistance = 8;
    const int targetCoordinate = verticalSlide ? m_TargetPosition.y() : m_TargetPosition.x();
    const int startCoordinate = targetCoordinate + slideDistance * slideDirection;
    m_SlideAnim->setPropertyName(verticalSlide ? QByteArrayLiteral("y") : QByteArrayLiteral("x"));
    if (verticalSlide) {
        setY(startCoordinate);
    }
    else {
        setX(startCoordinate);
    }
    setOpacity(0.0);

    show();
    raise();

    // Animate slide
    m_SlideAnim->setDuration(120);
    m_SlideAnim->setStartValue(startCoordinate);
    m_SlideAnim->setEndValue(targetCoordinate);
    m_SlideAnim->setEasingCurve(QEasingCurve::OutQuad);

    // Short, mechanical reveal.
    m_OpacityAnim->setDuration(120);
    m_OpacityAnim->setStartValue(0.0);
    m_OpacityAnim->setEndValue(1.0);
    m_OpacityAnim->setEasingCurve(QEasingCurve::OutQuad);

    m_SlideAnim->start();
    m_OpacityAnim->start();

    if (m_CloseWhenPointerOutside && !OverlayWindowPolicy::usesParentCoordinates(m_WindowMode)) {
        schedulePointerOutsideCheck();
    }

    forceRepaint();
}

void OverlayMenuPanel::schedulePointerOutsideCheck()
{
    if (!m_Visible || !m_CloseWhenPointerOutside) {
        return;
    }

    const qint64 remainingGrace = PointerGracePeriodMs - m_ShowTimer.elapsed();
    if (remainingGrace > 0) {
        m_LeaveTimer.start(static_cast<int>(remainingGrace));
    } else if (OverlayWindowPolicy::usesParentCoordinates(m_WindowMode)) {
        m_LeaveTimer.start(0);
    } else {
        m_LeaveTimer.start(PointerCheckIntervalMs);
    }
}

void OverlayMenuPanel::repositionWindow()
{
    int qpX = m_ParentX;
    int qpY = m_ParentY;
    int qpW = m_ParentW;
    int qpH = m_ParentH;

    int itemCount  = (int)m_MenuLevels[m_CurrentLevel].items.size();
    if (isTabRootLevel(m_CurrentLevel)) {
        // All tab roots share the tallest tab's height, so switching tabs
        // doesn't move or resize the panel.
        for (int level : m_TabLevels) {
            itemCount = std::max(itemCount, static_cast<int>(m_MenuLevels[level].items.size()));
        }
    }
    int titleH = m_TitleHeight + m_TabBarHeight;
    int menuHeight = titleH + itemCount * m_ItemHeight + m_Padding * 2;
    if (m_HasGamepads) {
        menuHeight += kGamepadHintBarHeight;
    }

    const QPoint triggerPosition = m_TriggerPosition.value_or(QPoint());

    int cx, cy; // content top-left position

    switch (m_AnchorMode) {
    case AnchorMode::LeftEdge:
        cx = qpX;
        if (m_TriggerPosition.has_value()) {
            cy = triggerPosition.y() - contentTop() - m_ItemHeight / 2;
        }
        else {
            cy = qpY + (qpH - menuHeight) / 2;
        }
        break;

    case AnchorMode::TopEdge:
        if (m_TriggerPosition.has_value()) {
            cx = triggerPosition.x() - m_MenuWidth / 2;
        }
        else {
            cx = qpX + (qpW - m_MenuWidth) / 2;
        }
        cy = qpY;
        break;

    case AnchorMode::AtCursor: {
        // Position menu so cursor is near top-left corner
        cx = triggerPosition.x();
        cy = triggerPosition.y();
        break;
    }

    case AnchorMode::RightEdge:
    default:
        cx = qpX + qpW - m_MenuWidth;
        if (m_TriggerPosition.has_value()) {
            cy = triggerPosition.y() - contentTop() - m_ItemHeight / 2;
        }
        else {
            cy = qpY + (qpH - menuHeight) / 2;
        }
        break;
    }

    // Clamp within the parent. If the menu is larger than the streaming
    // window, keep its origin visible instead of pushing it past an edge.
    cx = qpW >= m_MenuWidth
            ? qBound(qpX, cx, qpX + qpW - m_MenuWidth)
            : qpX;
    cy = qpH >= menuHeight
            ? qBound(qpY, cy, qpY + qpH - menuHeight)
            : qpY;

    // Window includes shadow margin around content
    setGeometry(cx - m_ShadowMargin, cy - m_ShadowMargin,
                m_MenuWidth + 2 * m_ShadowMargin, menuHeight + 2 * m_ShadowMargin);
}

void OverlayMenuPanel::navigateToLevel(int level)
{
    if (level < 0 || level >= (int)m_MenuLevels.size()) return;

    // Explicit navigation commits to interacting with the menu. A shorter
    // submenu can resize out from under the pointer, so keep it open until
    // an action or explicit dismissal instead of racing a leave timeout.
    beginInteraction();
    // Into a sub-level slides; back to a tab root is instant
    bool goingForward = !isTabRootLevel(level);
    m_ContentSlideAnim->stop();
    m_ContentOffset = 0;

    m_CurrentLevel = level;
    m_HoveredIndex = -1;
    if (level == m_SessionStatsLevel) {
        refreshSessionStats();
        m_SessionStatsTimer.start();
    } else {
        m_SessionStatsTimer.stop();
    }
    repositionWindow();

    if (goingForward) {
        // Forward: content slides in from right
        m_ContentSlideAnim->setStartValue(8.0);
        m_ContentSlideAnim->setEndValue(0.0);
        m_ContentSlideAnim->start();
    } else {
        // Back: instant switch, no animation (avoids jarring resize + slide combo)
        forceRepaint();
    }
}

void OverlayMenuPanel::beginInteraction()
{
    m_CloseWhenPointerOutside = false;
    m_LeaveTimer.stop();
}

void OverlayMenuPanel::dismissOnOutsideClick(const QPoint& parentPosition)
{
    if (m_Visible && !geometry().contains(parentPosition)) {
        closeMenu();
    }
}

void OverlayMenuPanel::dismissOnOutsidePointerMove(const QPoint& parentPosition)
{
    if (!m_Visible || !m_CloseWhenPointerOutside || m_ShowTimer.elapsed() < PointerGracePeriodMs) {
        return;
    }

    if (!geometry().contains(parentPosition)) {
        closeMenu();
    }
}

void OverlayMenuPanel::closeMenu()
{
    m_LeaveTimer.stop();
    if (!m_Visible) return;
    if (m_Closing) return;  // already animating close

    m_Visible = false;
    m_Closing = true;
    m_HoveredIndex = -1;
    m_WheelAccum = 0;

    // A close can land mid-drag (e.g. window focus loss). Drop the drag and
    // the mouse grab so the next show doesn't treat motion as scrubbing.
    if (m_SliderDragging) {
        m_SliderDragging = false;
        setMouseGrabEnabled(false);
    }
    m_SliderPressedZone = SliderZone::None;
    m_SliderHotZone = SliderZone::None;

    // Stop any show/level animations
    m_SlideAnim->stop();
    m_OpacityAnim->stop();
    m_ContentSlideAnim->stop();
    m_ContentOffset = 0;

    // Animate away from the edge that opened the menu.
    const bool verticalSlide = m_AnchorMode == AnchorMode::TopEdge;
    const int slideDirection = (m_AnchorMode == AnchorMode::LeftEdge || verticalSlide) ? -1 : 1;
    const int slideDistance = 8;
    const int startCoordinate = verticalSlide ? y() : x();
    m_SlideAnim->setPropertyName(verticalSlide ? QByteArrayLiteral("y") : QByteArrayLiteral("x"));
    m_SlideAnim->setDuration(120);
    m_SlideAnim->setStartValue(startCoordinate);
    m_SlideAnim->setEndValue(startCoordinate + slideDistance * slideDirection);
    m_SlideAnim->setEasingCurve(QEasingCurve::OutQuad);

    // Animate opacity: current → 0
    m_OpacityAnim->setDuration(120);
    m_OpacityAnim->setStartValue(opacity());
    m_OpacityAnim->setEndValue(0.0);
    m_OpacityAnim->setEasingCurve(QEasingCurve::OutQuad);

    // When fade-out completes, finalize (use disconnect to emulate single-shot for Qt 5 compat)
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(m_OpacityAnim, &QPropertyAnimation::finished, this, [this, conn]() {
        disconnect(*conn);
        m_Closing = false;
        m_CurrentLevel = tabRootLevel();
        hide();
        setOpacity(1.0);   // reset for next show
        if (m_CloseCallback) {
            m_CloseCallback();
        }
    });

    m_SlideAnim->start();
    m_OpacityAnim->start();
}

QRect OverlayMenuPanel::tabRect(int visibleIndex, int contentWidth) const
{
    // Equal-width tabs across the bar; the last one takes the remainder
    const int count = qMax(1, static_cast<int>(m_TabLevels.size()));
    const int innerWidth = contentWidth - 2;
    const int tabWidth = innerWidth / count;
    const int x = 1 + visibleIndex * tabWidth;
    const int width = visibleIndex == count - 1 ? innerWidth - visibleIndex * tabWidth : tabWidth;
    return QRect(x, m_TitleHeight, width, m_TabBarHeight - 1);
}

int OverlayMenuPanel::itemAtPos(const QPoint& pos) const
{
    // Adjust for shadow margin
    int lx = pos.x() - m_ShadowMargin;
    int ly = pos.y() - m_ShadowMargin;
    if (lx < 0 || lx >= m_MenuWidth || ly < 0) return -1;

    int titleH = m_TitleHeight;

    if (ly < titleH) {
        if (lx >= m_MenuWidth - m_TitleHeight) {
            return -3; // close button
        }
        if (!isTabRootLevel(m_CurrentLevel)) {
            return -2; // back button
        }
        return -1;
    }
    if (ly < titleH + m_TabBarHeight) {
        for (int i = 0; i < static_cast<int>(m_TabLevels.size()); i++) {
            if (tabRect(i, m_MenuWidth).contains(lx, ly)) {
                return kTabHitBase - i;
            }
        }
        return -1;
    }
    int localY = ly - contentTop();
    if (localY < 0) return -1;
    int idx = localY / m_ItemHeight;
    const auto& items = m_MenuLevels[m_CurrentLevel].items;
    if (idx < 0 || idx >= (int)items.size()) return -1;
    return idx;
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void OverlayMenuPanel::paintOverlay(QPainter& p)
{
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setRenderHint(QPainter::TextAntialiasing);

    int w = width();
    int h = height();
    int sm = m_ShadowMargin;
    int cw = w - 2 * sm;   // content width
    int ch = h - 2 * sm;   // content height

    // Clear to transparent
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(0, 0, w, h, Qt::transparent);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    // The same 6px, zero-blur offset shadow used by Panel.qml.
    p.fillRect(QRect(sm + 6, sm + 6, cw, ch), QColor(0, 0, 0, 140));
    p.save();
    p.translate(sm, sm);
    p.fillRect(QRect(0, 0, cw, ch), MenuSurface);
    p.setPen(QPen(MenuLine, 1));
    p.drawRect(QRect(0, 0, cw - 1, ch - 1));
    p.setClipRect(QRect(1, 1, cw - 2, ch - 2));
    p.fillRect(QRect(1, 1, 4, m_TitleHeight - 1), MenuAccent);
    p.fillRect(QRect(1, m_TitleHeight - 1, cw - 2, 1), MenuLine);

    const auto drawIcon = [&](const QString &name, const QRect &rect, qreal opacity = 1.0) {
        const auto icon = m_MenuIcons.constFind(name);
        if (icon == m_MenuIcons.cend()) return;
        p.save();
        p.setOpacity(opacity);
        icon.value().paint(&p, rect, Qt::AlignCenter, QIcon::Normal, QIcon::Off);
        p.restore();
    };

    // --- Title bar: back navigation on sub-levels and close on every level ---
    const auto& level = m_MenuLevels[m_CurrentLevel];
    int textPad = 16;
    int titleH = m_TitleHeight;
    const bool backHovered = !isTabRootLevel(m_CurrentLevel) && m_HoveredIndex == -2;
    const bool closeHovered = m_HoveredIndex == -3;

    if (backHovered) {
        p.fillRect(QRect(6, 1, cw - m_TitleHeight - 6, m_TitleHeight - 2), MenuHover);
    }

    const QRect closeRect(cw - m_TitleHeight, 0, m_TitleHeight, m_TitleHeight);
    if (closeHovered) {
        p.fillRect(closeRect, MenuDanger);
    }

    p.setFont(m_TitleFont);
    p.setPen(backHovered ? MenuAccent : MenuDim);
    const int backWidth = isTabRootLevel(m_CurrentLevel) ? 0 : 28;
    if (backWidth) {
        drawIcon(QStringLiteral("tb-back"), QRect(textPad, (titleH - 20) / 2, 20, 20));
    }
    QRect titleRect(textPad + backWidth, 0, cw - textPad - backWidth - m_TitleHeight, titleH);
    p.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, level.title.toUpper());
    drawIcon(QStringLiteral("menu-close"),
             QRect(closeRect.center().x() - 9, closeRect.center().y() - 9, 18, 18));

    // --- Tab bar: the active tab (or the one a sub-level belongs to) is
    // underlined in the accent. It stays put while the content slides. ---
    const int activeTabLevel = tabRootLevel();
    p.fillRect(QRect(1, titleH + m_TabBarHeight - 1, cw - 2, 1), MenuLine);
    p.setFont(m_TabFont);
    for (int i = 0; i < static_cast<int>(m_TabLevels.size()); i++) {
        const QRect tab = tabRect(i, cw);
        const MenuTab menuTab = *m_MenuLevels[m_TabLevels[i]].tab;
        const bool active = m_TabLevels[i] == activeTabLevel;
        const bool hovered = m_HoveredIndex == kTabHitBase - i;
        if (hovered && !active) {
            p.fillRect(tab, MenuHover);
        }
        p.setPen(active || hovered ? MenuText : MenuDim);
        p.drawText(tab.adjusted(2, 0, -2, 0), Qt::AlignCenter, tabTitle(menuTab).toUpper());
        if (active) {
            p.fillRect(QRect(tab.x() + 6, tab.bottom() - 2, tab.width() - 12, 3), MenuAccent);
        }
        // A USB device waiting for activation marks its tab, too
        if (m_RemoteUsbNeedsActivation &&
            menuTab == tabForAction(MenuAction::OpenRemoteUsbDevices)) {
            p.fillRect(QRect(tab.right() - 8, tab.y() + 6, 4, 4), MenuAccent);
        }
    }

    // Apply content offset for level navigation animation
    if (m_ContentSlideAnim->state() != QAbstractAnimation::Running) {
        m_ContentOffset = 0;
    }
    p.save();
    if (m_ContentOffset != 0) {
        p.translate(m_ContentOffset, 0);
    }

    const auto& items = level.items;
    const int itemsTop = contentTop();

    // Sub-levels have no fixed index, so entries match by action
    auto itemIcon = [](const MenuItem& item) -> QString {
        switch (item.action) {
        case MenuAction::OpenBitrate:
            return QStringLiteral("menu-bitrate");
        case MenuAction::OpenMenuPosition:
            return QStringLiteral("menu-position");
        case MenuAction::OpenRemoteUsbDevices:
            return QStringLiteral("cat-peripherals");
        case MenuAction::OpenStatsOverlay:
        case MenuAction::ToggleFullScreen:
            return QStringLiteral("cat-display");
        case MenuAction::ToggleStatsOverlay:
            return QStringLiteral("tb-network");
        case MenuAction::OpenSendKeys:
            return QStringLiteral("cat-input");
        case MenuAction::OpenHostCommands:
            return QStringLiteral("cat-host");
        case MenuAction::ShowHostFiles:
            return QStringLiteral("menu-files");
        case MenuAction::ToggleMicrophone:
            return QStringLiteral("menu-microphone");
        case MenuAction::ToggleGamepadMouse:
            return QStringLiteral("cat-gamepad");
        case MenuAction::Quit:
        case MenuAction::QuitAndExit:
            return QStringLiteral("menu-close");
#ifdef MOONLIGHT_ENABLE_FUNCTION_TESTS
        case MenuAction::OpenStylusReplayPanel:
            return QStringLiteral("tb-settings");
#endif
        default:
            return {};
        }
    };
    // Icon column: only on tab roots
    bool hasIcons = isTabRootLevel(m_CurrentLevel);
    int iconW = hasIcons ? m_IconAreaWidth : 0;
    int labelX = textPad + iconW;

    for (int i = 0; i < (int)items.size(); i++) {
        int itemY = itemsTop + i * m_ItemHeight;
        const auto& item = items[i];

        // Square focus/hover outline and the shared 4px accent marker.
        if (i == m_HoveredIndex && item.enabled) {
            const QRect row(4, itemY + 1, cw - 8, m_ItemHeight - 2);
            p.fillRect(row, MenuHover);
            p.setPen(QPen(MenuAccent, 1));
            p.drawRect(row.adjusted(0, 0, -1, -1));
            p.fillRect(QRect(4, itemY + 1, 4, m_ItemHeight - 2), MenuAccent);
        }
        const bool activationPrompt =
            m_RemoteUsbNeedsActivation &&
            ((hasIcons && item.action == MenuAction::OpenRemoteUsbDevices) ||
             (m_CurrentLevel == levelForOpenAction(MenuAction::OpenRemoteUsbDevices) &&
              item.action == MenuAction::SelectRemoteUsbDevice && item.enabled));
        if (activationPrompt) {
            p.fillRect(QRect(4, itemY + 4, 4, m_ItemHeight - 8), MenuAccent);
        }

        if (hasIcons) {
            drawIcon(itemIcon(item), QRect(textPad, itemY + (m_ItemHeight - 20) / 2, 20, 20),
                     item.enabled ? (i == m_HoveredIndex ? 1.0 : 0.85) : 0.4);
        }

        // --- SubMenu item ---
        if (item.type == MenuItemType::SubMenu) {
            p.setFont(m_LabelFont);
            p.setPen(activationPrompt ? MenuAccent : item.enabled ? MenuText : MenuFaint);
            QRect lr(labelX, itemY, cw - labelX - 36, m_ItemHeight);
            p.drawText(lr, Qt::AlignLeft | Qt::AlignVCenter, item.label);

            // Detail text (e.g., "20 Mbps")
            if (!item.detail.isEmpty()) {
                p.setFont(m_DetailFont);
                p.setPen(activationPrompt ? MenuAccent : MenuDim);
                QRect dr(cw / 2, itemY, cw / 2 - textPad - 20, m_ItemHeight);
                p.drawText(dr, Qt::AlignRight | Qt::AlignVCenter, item.detail);
            }

            drawIcon(QStringLiteral("menu-next"),
                     QRect(cw - textPad - 16, itemY + (m_ItemHeight - 16) / 2, 16, 16), 0.85);
        }
        // --- Toggle item ---
        else if (item.type == MenuItemType::Toggle) {
            p.setFont(m_LabelFont);
            p.setPen(item.enabled ? MenuText : MenuFaint);
            QRect lr(labelX, itemY, cw - labelX - 52, m_ItemHeight);
            p.drawText(lr, Qt::AlignLeft | Qt::AlignVCenter, item.label);

            // Square track and square thumb, matching HardSwitch.qml.
            const int trackW = 40, trackH = 20;
            const int trackX = cw - textPad - trackW;
            const int trackY = itemY + (m_ItemHeight - trackH) / 2;
            const QRect track(trackX, trackY, trackW, trackH);
            p.fillRect(track, item.toggleState ? MenuAccent : MenuHover);
            p.setPen(QPen(item.toggleState ? MenuAccent : MenuLine, 1));
            p.drawRect(track.adjusted(0, 0, -1, -1));
            p.fillRect(QRect(trackX + (item.toggleState ? trackW - 16 : 4),
                             trackY + 4, 12, 12), item.toggleState ? MenuSurface : MenuDim);
        }
        // --- Slider item (bitrate scrubber) ---
        else if (item.type == MenuItemType::Slider) {
            const SliderRowRects r = sliderRowRects(cw, itemY);
            const bool sliderFocused = i == m_HoveredIndex && item.enabled;

            // Current value in the shared brand accent.
            p.setFont(m_LabelFont);
            p.setPen(sliderFocused ? MenuAccent : MenuText);
            p.drawText(r.value, Qt::AlignLeft | Qt::AlignVCenter,
                       formatBitrateKbps(m_BitrateKbps));

            // Log-scale track: hover base, accent fill up to the square thumb.
            p.fillRect(r.track, MenuHover);
            p.setPen(QPen(MenuLine, 1));
            p.drawRect(r.track);
            const double frac = bitrateFraction();
            const int fillW = qRound(r.track.width() * frac);
            if (fillW > 0) {
                p.fillRect(QRect(r.track.x(), r.track.y(), fillW, r.track.height()),
                           MenuAccent);
            }
            const int thumbW = 10, thumbH = 16;
            const int thumbX = qBound(r.track.x() - thumbW / 2,
                                      r.track.x() + fillW - thumbW / 2,
                                      r.track.x() + r.track.width() - thumbW / 2);
            const QRect thumb(thumbX, itemY + (m_ItemHeight - thumbH) / 2, thumbW, thumbH);
            p.fillRect(thumb, m_SliderDragging ? MenuAccent : MenuText);
            p.setPen(QPen(MenuLine, 1));
            p.drawRect(thumb);

            // −/+ steppers with hover/pressed feedback
            const QRect zones[] = { r.minus, r.plus };
            const SliderZone zoneIds[] = { SliderZone::Minus, SliderZone::Plus };
            const QString glyphs[] = { QStringLiteral("-"), QStringLiteral("+") };
            for (int z = 0; z < 2; z++) {
                const bool hot = sliderFocused && m_SliderHotZone == zoneIds[z];
                const bool pressed = m_SliderPressedZone == zoneIds[z];
                if (hot || pressed) {
                    p.fillRect(zones[z], pressed ? MenuAccent : MenuHover);
                }
                p.setPen(QPen(pressed ? MenuAccent : MenuLine, 1));
                p.drawRect(zones[z]);
                p.setFont(m_LabelFont);
                p.setPen(hot || pressed ? MenuText : MenuDim);
                p.drawText(zones[z], Qt::AlignCenter, glyphs[z]);
            }
        }
        // --- Action item ---
        else if (item.type == MenuItemType::Action) {
            p.setFont(m_LabelFont);
            p.setPen(item.enabled ? MenuText : MenuFaint);

            bool hasLongDetail = !item.detail.isEmpty() && item.detail.length() > 3;
            bool hasShortDetail = !item.detail.isEmpty() && item.detail.length() <= 3;

            if (hasLongDetail) {
                int topH = qRound(m_ItemHeight * 0.58);
                QRect lb(labelX, itemY, cw - labelX - textPad, topH);
                p.drawText(lb, Qt::AlignLeft | Qt::AlignBottom, item.label);

                p.setFont(m_DetailFont);
                p.setPen(activationPrompt ? MenuAccent : MenuDim);
                QRect sr(labelX, itemY + topH, cw - labelX - textPad, m_ItemHeight - topH);
                p.drawText(sr, Qt::AlignLeft | Qt::AlignTop, item.detail);
            } else {
                int detailWidth = 0;
                if (hasShortDetail) {
                    const QFontMetrics detailMetrics(m_DetailFont);
                    detailWidth = qMax(20, detailMetrics.horizontalAdvance(item.detail) + 8);
                }

                const int detailGap = hasShortDetail ? 8 : 0;
                QRect lr(labelX, itemY,
                         cw - labelX - textPad - detailWidth - detailGap,
                         m_ItemHeight);
                p.drawText(lr, Qt::AlignLeft | Qt::AlignVCenter, item.label);

                if (hasShortDetail) {
                    // Short status text or checkmark in the shared brand accent.
                    p.setFont(m_DetailFont);
                    p.setPen(MenuAccent);
                    QRect cr(cw - textPad - detailWidth, itemY,
                             detailWidth, m_ItemHeight);
                    p.drawText(cr, Qt::AlignRight | Qt::AlignVCenter, item.detail);
                }
            }
        }
        // --- Back item (fallback, normally handled by title bar) ---
        else if (item.type == MenuItemType::Back) {
            p.setFont(m_DetailFont);
            p.setPen(MenuDim);
            QRect lr(labelX, itemY, cw - labelX - textPad, m_ItemHeight);
            p.drawText(lr, Qt::AlignLeft | Qt::AlignVCenter, item.label);
        }

        // Group separator — only where explicitly flagged
        if (item.separatorAfter && i < (int)items.size() - 1) {
            p.setPen(QPen(MenuLine, 1));
            int sepY = itemY + m_ItemHeight - 1;
            p.drawLine(labelX, sepY, cw - textPad, sepY);
        }
    }

    // Gamepad hint bar: pad-only players get the menu bindings on screen
    // instead of having to know them. Faces follow the controller style
    // (PS: ✕ Select / ○ Back; Nintendo: B Select / A Back), compensated for
    // swapFaceButtons so the hint names the physical button to press.
    if (m_HasGamepads) {
        const int hintTop = ch - m_Padding - kGamepadHintBarHeight;
        p.setPen(QPen(MenuLine, 1));
        p.drawLine(textPad, hintTop, cw - textPad, hintTop);
        p.setFont(m_DetailFont);
        p.setPen(MenuDim);
        const QString hint = gamepadLeftShoulderName(m_GamepadUiStyle) + QStringLiteral("/") +
                             gamepadRightShoulderName(m_GamepadUiStyle) + QStringLiteral(" ") +
                             tr("Tabs") + QStringLiteral("    ") +
                             QStringLiteral("\xe2\x86\x91\xe2\x86\x93 ") + tr("Move") +
                             QStringLiteral("    ") +
                             gamepadFaceButtonGlyph(m_GamepadUiStyle, m_SwapFaceButtons ? 1 : 0) +
                             QStringLiteral(" ") + tr("Select") + QStringLiteral("    ") +
                             gamepadFaceButtonGlyph(m_GamepadUiStyle, m_SwapFaceButtons ? 0 : 1) +
                             QStringLiteral(" ") + tr("Back");
        p.drawText(QRect(textPad, hintTop + 1, cw - textPad * 2, kGamepadHintBarHeight - 1),
                   Qt::AlignLeft | Qt::AlignVCenter, hint);
    }

    p.restore();  // content offset
    p.restore();  // shadow margin translate
}

// ---------------------------------------------------------------------------
// Mouse input
// ---------------------------------------------------------------------------

void OverlayMenuPanel::mouseMoveEvent(QMouseEvent* event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPoint pos = event->position().toPoint();
#else
    const QPoint pos = event->pos();
#endif

    if (m_SliderDragging) {
        // Keep scrubbing even when the pointer leaves the panel (mouse grab).
        const auto& items = m_MenuLevels[m_CurrentLevel].items;
        for (int i = 0; i < (int)items.size(); i++) {
            if (items[i].type != MenuItemType::Slider) continue;
            const int itemY = contentTop() + i * m_ItemHeight;
            const SliderRowRects r = sliderRowRects(width() - 2 * m_ShadowMargin, itemY);
            const double frac = (pos.x() - m_ShadowMargin - r.track.x())
                                    / double(r.track.width());
            setBitrateFromFraction(frac);
            break;
        }
        return;
    }

    const int newIdx = itemAtPos(pos);
    const SliderZone hotZone = sliderZoneAt(pos, newIdx);
    if (newIdx != m_HoveredIndex || hotZone != m_SliderHotZone) {
        m_HoveredIndex = newIdx;
        m_SliderHotZone = hotZone;
        setCursor((m_HoveredIndex >= 0 || m_HoveredIndex == -2 || m_HoveredIndex == -3 ||
                   m_HoveredIndex <= kTabHitBase)
                      ? Qt::PointingHandCursor
                      : Qt::ArrowCursor);
        forceRepaint();
    }
}

void OverlayMenuPanel::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return;
    beginInteraction();

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPoint pos = event->position().toPoint();
#else
    const QPoint pos = event->pos();
#endif
    int idx = itemAtPos(pos);

    if (idx == -3) {
        closeMenu();
        return;
    }

    // Title bar click → navigate back
    if (idx == -2) {
        navigateToLevel(tabRootLevel());
        return;
    }

    if (idx <= kTabHitBase) {
        const int tabIndex = kTabHitBase - idx;
        if (tabIndex < static_cast<int>(m_TabLevels.size())) {
            const auto active = std::find(m_TabLevels.cbegin(), m_TabLevels.cend(), tabRootLevel());
            const int activeIndex = static_cast<int>(active - m_TabLevels.cbegin());
            const int level = m_TabLevels[tabIndex];
            showTabLevel(level, level == m_CurrentLevel ? 0 : tabIndex > activeIndex ? 1 : -1);
        }
        return;
    }

    if (idx < 0) return;

    const auto& items = m_MenuLevels[m_CurrentLevel].items;
    if (idx >= (int)items.size() || !items[idx].enabled) return;

    const auto& item = items[idx];

    switch (item.type) {
    case MenuItemType::Back:
        navigateToLevel(tabRootLevel());
        break;

    case MenuItemType::SubMenu:
        navigateToLevel(item.targetLevel);
        break;

    case MenuItemType::Action:
        if (item.action == MenuAction::SetBitrate) {
            selectBitratePreset(item);
            break;
        }
        dispatchActionItem(item);
        break;

    case MenuItemType::Slider:
    {
        const SliderZone zone = sliderZoneAt(pos, idx);
        m_SliderPressedZone = zone;
        m_SliderHotZone = zone;
        if (zone == SliderZone::Minus) {
            adjustBitrateStep(-1, 1);
        }
        else if (zone == SliderZone::Plus) {
            adjustBitrateStep(1, 1);
        }
        else if (zone == SliderZone::Track) {
            const int itemY = contentTop() + idx * m_ItemHeight;
            const SliderRowRects r = sliderRowRects(width() - 2 * m_ShadowMargin, itemY);
            m_SliderDragging = true;
            setMouseGrabEnabled(true);
            const double frac = (pos.x() - m_ShadowMargin - r.track.x())
                                    / double(r.track.width());
            setBitrateFromFraction(frac);
        }
        forceRepaint();
        break;
    }

    case MenuItemType::Toggle:
    {
        // Toggle visual state and dispatch
        auto& mutableItem = m_MenuLevels[m_CurrentLevel].items[idx];
        mutableItem.toggleState = !mutableItem.toggleState;
        forceRepaint();
        if (m_ActionCallback) {
            m_ActionCallback(item.action);
        }
        break;
    }
    }
}

void OverlayMenuPanel::mouseReleaseEvent(QMouseEvent* event)
{
    Q_UNUSED(event);
    if (!m_SliderDragging && m_SliderPressedZone == SliderZone::None) return;

    if (m_SliderDragging) {
        m_SliderDragging = false;
        setMouseGrabEnabled(false);
    }
    m_SliderPressedZone = SliderZone::None;
    m_SliderHotZone = SliderZone::None;
    // The commit timer keeps running; the value lands once the pointer idles.
    forceRepaint();
}

void OverlayMenuPanel::wheelEvent(QWheelEvent* event)
{
    if (!m_Visible) {
        event->ignore();
        return;
    }

    // QWheelEvent::pos() is unavailable on the SteamLink Qt 5.14 build;
    // position() exists everywhere from 5.14 on.
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    const QPoint pos = event->position().toPoint();
#else
    const QPoint pos = event->pos();
#endif
    const int idx = itemAtPos(pos);
    if (idx < 0 || idx >= (int)m_MenuLevels[m_CurrentLevel].items.size()
            || m_MenuLevels[m_CurrentLevel].items[idx].type != MenuItemType::Slider) {
        // Don't carry a partial notch into a later scrub session.
        m_WheelAccum = 0;
        event->ignore();
        return;
    }

    // Wheeling the scrubber counts as interaction; don't race the leave timer.
    beginInteraction();

    // Accumulate high-resolution wheel deltas; one notch = one fine step.
    m_WheelAccum += event->angleDelta().y() / 120.0;
    int notches = 0;
    while (m_WheelAccum >= 1.0) { notches++; m_WheelAccum -= 1.0; }
    while (m_WheelAccum <= -1.0) { notches--; m_WheelAccum += 1.0; }
    if (notches != 0) {
        adjustBitrateStep(notches > 0 ? 1 : -1, qAbs(notches));
    }
    event->accept();
}

// ---------------------------------------------------------------------------
// Gamepad navigation
// ---------------------------------------------------------------------------

void OverlayMenuPanel::gamepadMoveUp()
{
    if (!m_Visible) return;
    beginInteraction();
    const auto& items = m_MenuLevels[m_CurrentLevel].items;
    if (items.empty()) return;

    if (m_HoveredIndex <= 0) {
        // Wrap to last item, or move to title bar if on sub-level
        if (!isTabRootLevel(m_CurrentLevel) && m_HoveredIndex == 0) {
            m_HoveredIndex = -2; // title bar (back button)
        } else {
            m_HoveredIndex = (int)items.size() - 1;
        }
    } else {
        m_HoveredIndex--;
    }
    // Skip disabled items
    if (m_HoveredIndex >= 0 && !items[m_HoveredIndex].enabled) {
        gamepadMoveUp();
        return;
    }
    forceRepaint();
}

void OverlayMenuPanel::gamepadMoveDown()
{
    if (!m_Visible) return;
    beginInteraction();
    const auto& items = m_MenuLevels[m_CurrentLevel].items;
    if (items.empty()) return;

    if (m_HoveredIndex == -2) {
        // From title bar, move to first item
        m_HoveredIndex = 0;
    } else if (m_HoveredIndex < 0 || m_HoveredIndex >= (int)items.size() - 1) {
        // Wrap to title bar on sub-level, or to first item on a tab root
        if (!isTabRootLevel(m_CurrentLevel)) {
            m_HoveredIndex = -2;
        } else {
            m_HoveredIndex = 0;
        }
    } else {
        m_HoveredIndex++;
    }
    // Skip disabled items
    if (m_HoveredIndex >= 0 && !items[m_HoveredIndex].enabled) {
        gamepadMoveDown();
        return;
    }
    forceRepaint();
}

void OverlayMenuPanel::gamepadSelect()
{
    if (!m_Visible) return;

    // Title bar → back
    if (m_HoveredIndex == -2) {
        navigateToLevel(tabRootLevel());
        return;
    }

    if (m_HoveredIndex < 0) return;

    const auto& items = m_MenuLevels[m_CurrentLevel].items;
    if (m_HoveredIndex >= (int)items.size() || !items[m_HoveredIndex].enabled) return;

    const auto& item = items[m_HoveredIndex];

    switch (item.type) {
    case MenuItemType::Back:
        navigateToLevel(tabRootLevel());
        break;
    case MenuItemType::SubMenu:
        navigateToLevel(item.targetLevel);
        break;
    case MenuItemType::Action:
        if (item.action == MenuAction::SetBitrate) {
            selectBitratePreset(item);
            break;
        }
        dispatchActionItem(item);
        break;
    case MenuItemType::Slider:
        // A confirms — flush a pending scrub immediately.
        commitBitrateNow();
        break;
    case MenuItemType::Toggle:
    {
        auto& mutableItem = m_MenuLevels[m_CurrentLevel].items[m_HoveredIndex];
        mutableItem.toggleState = !mutableItem.toggleState;
        forceRepaint();
        if (m_ActionCallback) {
            m_ActionCallback(item.action);
        }
        break;
    }
    }
}

void OverlayMenuPanel::gamepadBack()
{
    if (!m_Visible) return;

    if (!isTabRootLevel(m_CurrentLevel)) {
        navigateToLevel(tabRootLevel());
    } else {
        closeMenu();
    }
}

bool OverlayMenuPanel::event(QEvent* ev)
{
    if (ev->type() == QEvent::Enter && OverlayWindowPolicy::usesParentCoordinates(m_WindowMode)) {
        m_LeaveTimer.stop();
    }
    if (ev->type() == QEvent::Leave) {
        if (m_Visible && m_CloseWhenPointerOutside) {
            // During the grace period, defer the outside check instead of
            // dropping the Leave event. Otherwise, leaving the panel quickly
            // after it opens would keep it visible until the cursor entered
            // and left the panel again.
            schedulePointerOutsideCheck();
        }
        return true;
    }
    return QRasterWindow::event(ev);
}
