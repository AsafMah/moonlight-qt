#include "overlaymenupanel.h"
#include "uifont.h"
#include "streaming/streamquality.h"

#include <QScreen>
#include <QGuiApplication>
#include <QCoreApplication>
#include <QSettings>
#include <QCursor>
#include <QFontMetrics>
#include <QtMath>
#include <algorithm>
#include <memory>
#include <tuple>

namespace {
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
// - A tab with no entries (e.g. Host without host features) is hidden, so a
//   tab needs no other registration.
// Sub-level rows (presets, keys, commands, devices) are not listed: they live
// in their sub-level, which opens from the listed Open* entry.
constexpr TabAssignment kTabAssignments[] = {
    // Quick clones canonical rows below; these are its fixed session actions.
    { MenuAction::ToggleMinimize, MenuTab::Quick },
    { MenuAction::Quit, MenuTab::Quick },
    { MenuAction::QuitAndExit, MenuTab::Quick },
    { MenuAction::OpenStreamQuality, MenuTab::Stream },
    { MenuAction::OpenBitrate, MenuTab::Stream },
    { MenuAction::ToggleAudioMute, MenuTab::Stream },
    { MenuAction::ToggleMicrophone, MenuTab::Stream },
    { MenuAction::OpenWindowMode, MenuTab::Display },
    { MenuAction::ToggleResolutionMatch, MenuTab::Display },
    { MenuAction::MatchResolutionNow, MenuTab::Display },
    { MenuAction::OpenStatsOverlay, MenuTab::Display },
    { MenuAction::OpenMouseMode, MenuTab::Input },
    { MenuAction::UngrabInput, MenuTab::Input },
    { MenuAction::ToggleCursorHide, MenuTab::Input },
    { MenuAction::TogglePointerRegionLock, MenuTab::Input },
    { MenuAction::ToggleKeyboardGrab, MenuTab::Input },
    { MenuAction::OpenSendKeys, MenuTab::Input },
    { MenuAction::PasteText, MenuTab::Input },
    { MenuAction::ToggleGamepadMouse, MenuTab::Input },
    { MenuAction::OpenRemoteUsbDevices, MenuTab::Host },
    // Host: features the host offers
    { MenuAction::OpenHostCommands, MenuTab::Host },
    { MenuAction::ShowHostFiles, MenuTab::Host },
    { MenuAction::CancelFileTransfer, MenuTab::Host },
    // Session: the running session
    { MenuAction::OpenSessionStats, MenuTab::Session },
    { MenuAction::ToggleMuteOnFocusLoss, MenuTab::Session },
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
      m_ParentX(0), m_ParentY(0), m_ParentW(0), m_ParentH(0), m_OriginalParent(parent),
      m_OriginalWindowMode(windowMode), m_WindowMode(windowMode), m_ContentOffset(0),
      m_Closing(false), m_TargetPosition(), m_AnchorMode(AnchorMode::RightEdge),
      m_TriggerPosition(std::nullopt)
{
    Q_ASSERT(m_WindowMode != OverlayWindowMode::WaylandSubsurface || parent != nullptr);
    setFlags(OverlayWindowPolicy::flags(m_WindowMode));

    QSurfaceFormat fmt;
    fmt.setAlphaBufferSize(8);
    setFormat(fmt);

    // Sizes come from the layout, see applyMetrics()
    m_ShadowMargin = 8;

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

    applyMetrics();

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

    m_ShortcutProfile = ShortcutProfile::loadFromSettings();
    const QStringList defaultPins = {
        QString::number(int(MenuAction::OpenStatsOverlay)),
        QString::number(int(MenuAction::OpenWindowMode)),
        QString::number(int(MenuAction::OpenMouseMode)),
        QString::number(int(MenuAction::ToggleAudioMute)),
        QString::number(int(MenuAction::ToggleMicrophone)),
        QString::number(int(MenuAction::OpenStreamQuality)),
        QString::number(int(MenuAction::OpenSendKeys)),
    };
    m_QuickPins =
        QSettings().value(QStringLiteral("overlay-menu/quick-pins"), defaultPins).toStringList();
    m_QuitConfirmTimer.setSingleShot(true);
    m_QuitConfirmTimer.setInterval(3000);
    connect(&m_QuitConfirmTimer, &QTimer::timeout, this, [this] {
        applyOwnedState();
        forceRepaint();
    });
    m_ShortcutRefreshTimer.setInterval(250);
    connect(&m_ShortcutRefreshTimer, &QTimer::timeout, this, &OverlayMenuPanel::refreshShortcuts);

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
// Layout options
// ---------------------------------------------------------------------------

void OverlayMenuPanel::applyMetrics()
{
    // Logical (unscaled) values — Qt 6 handles DPI automatically. Compact
    // keeps the original sizes, matching the application's square panels.
    const bool comfortable = m_Layout == OverlayMenu::Layout::Comfortable;
    m_ItemHeight = comfortable ? 48 : 38;
    m_Padding = comfortable ? 6 : 4;
    m_TitleHeight = comfortable ? 38 : 32;
    m_IconAreaWidth = comfortable ? 32 : 28;
    m_TabBarHeight = m_Layout == OverlayMenu::Layout::SidePanel ? 0 : (comfortable ? 36 : 30);
    m_BaseMenuWidth = comfortable ? 380 : 320;

    m_LabelFont.setPointSizeF(comfortable ? 12 : 10);
    m_DetailFont.setPointSizeF(comfortable ? 9.5 : 8);
    m_TitleFont.setPointSizeF(comfortable ? 10.5 : 9);
    m_TabFont.setPointSizeF(comfortable ? 9.5 : 8);

    m_RailWidth = 0;
    if (m_Layout == OverlayMenu::Layout::SidePanel) {
        // The rail fits its longest tab title
        const QFontMetrics tabMetrics(m_TabFont);
        int widest = 0;
        for (int t = 0; t < static_cast<int>(MenuTab::Count); t++) {
            widest = qMax(
                widest, tabMetrics.horizontalAdvance(tabTitle(static_cast<MenuTab>(t)).toUpper()));
        }
        m_RailWidth = qMax(96, widest + 28);
        m_BaseMenuWidth += m_RailWidth;
    }
    m_MenuWidth = m_BaseMenuWidth;
}

void OverlayMenuPanel::setStyle(OverlayMenu::Layout layout, int scalePercent, int opacityPercent)
{
    m_Layout = layout;
    m_RequestedScale = OverlayMenu::clampScalePercent(scalePercent) / 100.0;
    m_Scale = m_RequestedScale;
    m_PanelOpacity = OverlayMenu::clampOpacityPercent(opacityPercent) / 100.0;
    applyMetrics();
    if (m_Visible) {
        repositionWindow();
        forceRepaint();
    }
}

QRect OverlayMenuPanel::contentGeometry() const
{
    const int shadow = qRound(m_ShadowMargin * m_Scale);
    return geometry().adjusted(shadow, shadow, -shadow, -shadow);
}

void OverlayMenuPanel::setPointerEditMode(bool enabled)
{
    m_PointerEditMode = enabled;
    if (!enabled) {
        m_Editing = false;
        m_EditResizeEdges = {};
        m_TitlePressed = m_TitleDragging = false;
        setMouseGrabEnabled(false);
        unsetCursor();
        if (m_Visible) {
            repositionWindow();
            forceRepaint();
        }
    }
}

Qt::Edges OverlayMenuPanel::editEdgesAt(const QPoint& point) const
{
    Qt::Edges edges;
    const QRect rect = contentGeometry();
    if (point.x() <= rect.left() + 12)
        edges |= Qt::LeftEdge;
    else if (point.x() >= rect.right() - 12)
        edges |= Qt::RightEdge;
    if (point.y() <= rect.top() + 12)
        edges |= Qt::TopEdge;
    else if (point.y() >= rect.bottom() - 12)
        edges |= Qt::BottomEdge;
    return edges;
}

void OverlayMenuPanel::resizeForEditing(const QPoint& point)
{
    const QPoint delta = point - m_TitlePressPosition;
    const bool horizontal =
        m_EditResizeEdges.testFlag(Qt::LeftEdge) || m_EditResizeEdges.testFlag(Qt::RightEdge);
    const bool vertical =
        m_EditResizeEdges.testFlag(Qt::TopEdge) || m_EditResizeEdges.testFlag(Qt::BottomEdge);
    const qreal dx = (m_EditResizeEdges.testFlag(Qt::LeftEdge) ? -delta.x() : delta.x()) /
                     qreal(m_EditPressGeometry.width());
    const qreal dy = (m_EditResizeEdges.testFlag(Qt::TopEdge) ? -delta.y() : delta.y()) /
                     qreal(m_EditPressGeometry.height());
    const qreal change = horizontal && (!vertical || std::abs(dx) >= std::abs(dy)) ? dx : dy;
    setStyle(m_Layout, qRound(m_EditPressScale * (1 + change)), opacityPercent());
    const QSize content = contentGeometry().size();
    QPoint topLeft = m_EditPressGeometry.topLeft();
    if (m_EditResizeEdges.testFlag(Qt::LeftEdge)) {
        topLeft.setX(m_EditPressGeometry.right() + 1 - content.width());
    }
    if (m_EditResizeEdges.testFlag(Qt::TopEdge)) {
        topLeft.setY(m_EditPressGeometry.bottom() + 1 - content.height());
    }
    const QRect bounds =
        m_Bounds.isValid() ? m_Bounds : QRect(m_ParentX, m_ParentY, m_ParentW, m_ParentH);
    const int shadow = qRound(m_ShadowMargin * m_Scale);
    setPosition(OverlayMenu::clampToBounds(topLeft, content, bounds) - QPoint(shadow, shadow));
    m_TargetPosition = position();
}

void OverlayMenuPanel::saveEditedStyle()
{
    finishTitleDrag();
    if (m_StyleEditedCallback) {
        m_StyleEditedCallback(scalePercent(), opacityPercent());
    }
}

QPoint OverlayMenuPanel::layoutPosition(const QPointF& windowPosition) const
{
    return (windowPosition / m_Scale).toPoint();
}

QPoint OverlayMenuPanel::parentPosition(QMouseEvent* event) const
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPoint local = event->position().toPoint();
    const QPoint global = event->globalPosition().toPoint();
#else
    const QPoint local = event->pos();
    const QPoint global = event->globalPos();
#endif
    // Native Wayland children live in parent-local coordinates
    return OverlayWindowPolicy::usesParentCoordinates(m_WindowMode) ? position() + local : global;
}

QPoint OverlayMenuPanel::slideVector() const
{
    switch (m_AnchorMode) {
    case AnchorMode::TopEdge:
        return QPoint(0, -1);
    case AnchorMode::LeftEdge:
        return QPoint(-1, 0);
    case AnchorMode::Placed:
        return OverlayMenu::slideDirection(m_Placement);
    case AnchorMode::RightEdge:
    case AnchorMode::AtCursor:
    default:
        return QPoint(1, 0);
    }
}

namespace {

// One text of the panel and the panel width it needs to be shown in full
struct TextNeed
{
    QString text;
    int width;
};

} // namespace

// Shared by requiredMenuWidth() and clippedTexts(). Mirrors the painting in
// paintOverlay(); keep the two in step.
static std::vector<TextNeed>
textNeeds(const QFontMetrics& label, const QFontMetrics& detail, const QFontMetrics& title,
          int iconAreaWidth, int titleHeight, int rowsLeft,
          const std::vector<std::pair<QString, bool>>& titles,
          const std::vector<std::tuple<QString, QString, int, bool>>& rows)
{
    const int textPad = 16;
    std::vector<TextNeed> needs;
    for (const auto& levelTitle : titles) {
        // Back icon on sub-levels, then the title, then the close button
        needs.push_back(
            { levelTitle.first, textPad + (levelTitle.second ? 0 : 28) +
                                    title.horizontalAdvance(levelTitle.first.toUpper()) + 4 +
                                    titleHeight });
    }
    for (const auto& row : rows) {
        const QString& text = std::get<0>(row);
        const QString& detailText = std::get<1>(row);
        const int type = std::get<2>(row);
        const int labelX = rowsLeft + textPad + (std::get<3>(row) ? iconAreaWidth : 0);
        const int labelWidth = label.horizontalAdvance(text);
        const int detailWidth = detail.horizontalAdvance(detailText);
        int longestDetailLine = 0;
        for (const QString& line : detailText.split(QLatin1Char('\n')))
            longestDetailLine = qMax(longestDetailLine, detail.horizontalAdvance(line));
        int need = 0;
        switch (static_cast<OverlayMenuPanel::MenuItemType>(type)) {
        case OverlayMenuPanel::MenuItemType::SubMenu:
            // label, detail, then the arrow
            need =
                labelX + labelWidth + (detailText.isEmpty() ? 0 : 12 + detailWidth) + 20 + textPad;
            break;
        case OverlayMenuPanel::MenuItemType::Toggle:
            need = labelX + qMax(labelWidth, detailWidth) + 68;
            break;
        case OverlayMenuPanel::MenuItemType::Header:
        case OverlayMenuPanel::MenuItemType::Info:
        case OverlayMenuPanel::MenuItemType::Action:
            if (detailText.length() > 3) {
                // two lines
                need = labelX + qMax(labelWidth, longestDetailLine) + textPad;
            } else if (!detailText.isEmpty()) {
                need = labelX + labelWidth + 8 + qMax(20, detailWidth + 8) + textPad;
            } else {
                need = labelX + labelWidth + textPad;
            }
            break;
        case OverlayMenuPanel::MenuItemType::Back:
            need = labelX + detail.horizontalAdvance(text) + textPad;
            break;
        case OverlayMenuPanel::MenuItemType::Slider:
        default:
            break;
        }
        if (need > 0) {
            needs.push_back({ text, need });
        }
    }
    return needs;
}

QString OverlayMenuPanel::gamepadHintText() const
{
    return gamepadLeftShoulderName(m_GamepadUiStyle) + QStringLiteral("/") +
           gamepadRightShoulderName(m_GamepadUiStyle) + QStringLiteral(" ") + tr("Tabs") +
           QStringLiteral("    ") + QStringLiteral("\xe2\x86\x91\xe2\x86\x93 ") + tr("Move") +
           QStringLiteral("    ") +
           gamepadFaceButtonGlyph(m_GamepadUiStyle, m_SwapFaceButtons ? 1 : 0) +
           QStringLiteral(" ") + tr("Select") + QStringLiteral("    ") +
           gamepadFaceButtonGlyph(m_GamepadUiStyle, m_SwapFaceButtons ? 0 : 1) +
           QStringLiteral(" ") + tr("Back");
}

int OverlayMenuPanel::gamepadHintWidth() const
{
    // The hint bar runs across the rows, padded on both sides
    return m_HasGamepads
               ? rowsLeft() + 32 + QFontMetrics(m_DetailFont).horizontalAdvance(gamepadHintText())
               : 0;
}

int OverlayMenuPanel::requiredMenuWidth() const
{
    int width = m_BaseMenuWidth;
    std::vector<std::pair<QString, bool>> titles;
    std::vector<std::tuple<QString, QString, int, bool>> rows;
    for (int l = 0; l < static_cast<int>(m_MenuLevels.size()); l++) {
        const bool tabRoot = isTabRootLevel(l);
        titles.push_back({ m_MenuLevels[l].title, tabRoot });
        for (const MenuItem& item : m_MenuLevels[l].items) {
            rows.emplace_back(item.label, displayDetail(item), static_cast<int>(item.type),
                              tabRoot);
        }
    }
    for (const TextNeed& need :
         textNeeds(QFontMetrics(m_LabelFont), QFontMetrics(m_DetailFont), QFontMetrics(m_TitleFont),
                   m_IconAreaWidth, m_TitleHeight, rowsLeft(), titles, rows)) {
        width = qMax(width, need.width);
    }

    // A tab bar splits the width evenly; the rail has its own width
    if (m_Layout != OverlayMenu::Layout::SidePanel && !m_TabLevels.empty()) {
        const QFontMetrics tabMetrics(m_TabFont);
        int widest = 0;
        for (int level : m_TabLevels) {
            widest = qMax(
                widest, tabMetrics.horizontalAdvance(tabTitle(*m_MenuLevels[level].tab).toUpper()));
        }
        width = qMax(width, (widest + 8) * static_cast<int>(m_TabLevels.size()) + 2);
    }
    return qMax(width, gamepadHintWidth());
}

QStringList OverlayMenuPanel::clippedTexts() const
{
    QStringList clipped;
    std::vector<std::pair<QString, bool>> titles;
    std::vector<std::tuple<QString, QString, int, bool>> rows;
    for (int l = 0; l < static_cast<int>(m_MenuLevels.size()); l++) {
        const bool tabRoot = isTabRootLevel(l);
        titles.push_back({ m_MenuLevels[l].title, tabRoot });
        for (const MenuItem& item : m_MenuLevels[l].items) {
            const QString detail =
                item.action == MenuAction::UngrabInput
                    ? releaseDetailLines(item, m_MenuWidth - rowsLeft() - 32 -
                                                   (tabRoot ? m_IconAreaWidth : 0))
                          .join('\n')
                    : displayDetail(item);
            rows.emplace_back(item.label, detail, static_cast<int>(item.type), tabRoot);
        }
    }
    for (const TextNeed& need :
         textNeeds(QFontMetrics(m_LabelFont), QFontMetrics(m_DetailFont), QFontMetrics(m_TitleFont),
                   m_IconAreaWidth, m_TitleHeight, rowsLeft(), titles, rows)) {
        if (need.width > m_MenuWidth) {
            clipped.append(need.text);
        }
    }

    const QFontMetrics tabMetrics(m_TabFont);
    for (int i = 0; i < static_cast<int>(m_TabLevels.size()); i++) {
        const QString text = tabTitle(*m_MenuLevels[m_TabLevels[i]].tab).toUpper();
        const QRect tab = tabRect(i, m_MenuWidth);
        // Horizontal tabs pad their text by 2 on each side; rail tabs by 14
        const int room =
            m_Layout == OverlayMenu::Layout::SidePanel ? tab.width() - 28 : tab.width() - 4;
        if (tabMetrics.horizontalAdvance(text) > room) {
            clipped.append(text);
        }
    }
    if (gamepadHintWidth() > m_MenuWidth) {
        clipped.append(gamepadHintText());
    }
    return clipped;
}

QStringList OverlayMenuPanel::releaseDetailLines(const MenuItem& item, int availableWidth) const
{
    const QString detail = displayDetail(item);
    if (QFontMetrics(m_DetailFont).horizontalAdvance(detail) <= availableWidth)
        return { detail };
    const QString shortcut = shortcutDetail(item.action, item.payload);
    return shortcut.isEmpty() ? QStringList{ item.detail } : QStringList{ item.detail, shortcut };
}

int OverlayMenuPanel::rowHeight(const MenuItem& item) const
{
    if (item.action == MenuAction::UngrabInput &&
        releaseDetailLines(item, rowsWidth() - 32 - m_IconAreaWidth).size() > 1)
        return qMax(m_ItemHeight, QFontMetrics(m_LabelFont).height() +
                                      2 * QFontMetrics(m_DetailFont).height() + 8);
    return m_ItemHeight;
}

int OverlayMenuPanel::rowTop(int row) const
{
    int top = 0;
    const auto& items = m_MenuLevels[m_CurrentLevel].items;
    for (int i = 0; i < row; ++i)
        top += rowHeight(items[i]);
    return top;
}

int OverlayMenuPanel::rowsHeight(const MenuLevel& level) const
{
    int height = 0;
    for (const MenuItem& item : level.items)
        height += rowHeight(item);
    return height;
}

int OverlayMenuPanel::menuHeight() const
{
    int contentHeight = rowsHeight(m_MenuLevels[m_CurrentLevel]);
    if (isTabRootLevel(m_CurrentLevel)) {
        // All tab roots share the tallest tab's height, so switching tabs
        // doesn't move or resize the panel.
        for (int level : m_TabLevels) {
            contentHeight = qMax(contentHeight, rowsHeight(m_MenuLevels[level]));
        }
    }
    int height = m_TitleHeight + m_TabBarHeight + contentHeight + m_Padding * 2;
    if (m_Layout == OverlayMenu::Layout::SidePanel) {
        // The rail fits every tab
        height = qMax(height, m_TitleHeight + m_Padding * 2 +
                                  static_cast<int>(m_TabLevels.size()) * railTabHeight());
    }
    if (m_HasGamepads || m_Editing) {
        height += kGamepadHintBarHeight;
    }
    return height;
}

std::optional<OverlayMenu::Layout> OverlayMenuPanel::layoutForAction(MenuAction action)
{
    switch (action) {
    case MenuAction::SetMenuLayoutCompact:
        return OverlayMenu::Layout::Compact;
    case MenuAction::SetMenuLayoutComfortable:
        return OverlayMenu::Layout::Comfortable;
    case MenuAction::SetMenuLayoutSidePanel:
        return OverlayMenu::Layout::SidePanel;
    default:
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// Menu structure
// ---------------------------------------------------------------------------

QString OverlayMenuPanel::shortcutActionId(MenuAction action, const QString& payload)
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
    case MenuAction::ToggleKeyboardGrab:
        return QLatin1String(ShortcutActions::ToggleKeyboardGrab);
    case MenuAction::CycleOverlayPreset:
        return QLatin1String(ShortcutActions::CycleOverlayPreset);
    case MenuAction::CancelFileTransfer:
        return QLatin1String(ShortcutActions::CancelFileTransfer);
    case MenuAction::EditStatsOverlayPosition:
        return QLatin1String(ShortcutActions::EditOverlayPosition);
    case MenuAction::OpenHostCommands:
        return QLatin1String(ShortcutActions::OpenHostCommands);
    case MenuAction::SendHostKeys:
        return payload == QLatin1String("special:ctrl-alt-del")
                   ? QLatin1String(ShortcutActions::SendCtrlAltDel)
                   : QString();
    default:
        return QString();
    }
}

QString OverlayMenuPanel::shortcutDetail(MenuAction action, const QString& payload) const
{
    QStringList texts;
    for (const ShortcutBinding& binding :
         m_ShortcutProfile.bindings(shortcutActionId(action, payload))) {
        if (!binding.isNull()) {
            texts.append(binding.toDisplayString());
        }
    }
    return texts.join(QStringLiteral(" / "));
}

QString OverlayMenuPanel::displayDetail(const MenuItem& item) const
{
    const QString shortcut = shortcutDetail(item.action, item.payload);
    if (shortcut.isEmpty() || shortcut == item.detail) {
        return item.detail;
    }
    return item.detail.isEmpty() ? shortcut : item.detail + QStringLiteral(" · ") + shortcut;
}

void OverlayMenuPanel::refreshShortcuts()
{
    const ShortcutProfile profile = ShortcutProfile::loadFromSettings();
    bool changed = false;
    for (const ShortcutAction& action : ShortcutRegistry::instance().actions()) {
        changed |= profile.bindings(action.id) != m_ShortcutProfile.bindings(action.id);
    }
    m_ShortcutProfile = profile;
    if (m_ShortcutProfileCallback) {
        m_ShortcutProfileCallback(profile);
    }
    if (changed && m_Visible) {
        repositionWindow();
        requestUpdate();
    }
}

QStringList OverlayMenuPanel::menuShortcutActions() const
{
    QStringList actions;
    for (const MenuLevel& level : m_MenuLevels) {
        for (const MenuItem& item : level.items) {
            const QString action = shortcutActionId(item.action, item.payload);
            if (!action.isEmpty()) {
                actions.append(action);
            }
        }
    }
    actions.removeDuplicates();
    return actions;
}

QString OverlayMenuPanel::rowDetail(int row) const
{
    return displayDetail(m_MenuLevels.at(m_CurrentLevel).items.at(row));
}

OverlayMenuPanel::MenuTab OverlayMenuPanel::tabForAction(MenuAction action)
{
    switch (action) {
    case MenuAction::SelectStreamResolution:
    case MenuAction::SelectStreamFps:
    case MenuAction::ApplyStreamQuality:
    case MenuAction::SaveStreamQualityAsDefault:
    case MenuAction::SetBitrate:
        return MenuTab::Stream;
    case MenuAction::SetWindowModeWindowed:
    case MenuAction::SetWindowModeBorderless:
    case MenuAction::SetWindowModeFullscreen:
    case MenuAction::ToggleFullScreen:
    case MenuAction::ToggleStatsOverlay:
    case MenuAction::SetStatsPresetOff:
    case MenuAction::SetStatsPresetMinimal:
    case MenuAction::SetStatsPresetCompact:
    case MenuAction::SetStatsPresetDetailed:
    case MenuAction::SetStatsPresetCustom:
    case MenuAction::OpenStatsOpacity:
    case MenuAction::SetStatsOpacity:
    case MenuAction::EditStatsOverlayPosition:
    case MenuAction::ToggleStatsOverlayDetached:
    case MenuAction::CycleOverlayPreset:
        return MenuTab::Display;
    case MenuAction::ToggleMouseMode:
    case MenuAction::SetMouseModeGame:
    case MenuAction::SetMouseModeDesktop:
    case MenuAction::SendHostKeys:
        return MenuTab::Input;
    case MenuAction::SelectRemoteUsbDevice:
    case MenuAction::ReleaseRemoteUsbDevice:
    case MenuAction::RunHostCommand:
        return MenuTab::Host;
    default:
        // Global gear actions have no content-tab owner; Quick is their
        // fallback only. Navigation preserves the actual initiating tab.
        break;
    }
    const int order = tabAssignmentOrder(action);
    return order < kTabAssignmentCount ? kTabAssignments[order].tab : MenuTab::Quick;
}

QString OverlayMenuPanel::tabTitle(MenuTab tab)
{
    switch (tab) {
    case MenuTab::Quick:
        return tr("Quick");
    case MenuTab::Stream:
        return tr("Stream");
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

void OverlayMenuPanel::linkSubMenuRows(MenuLevel& level) const
{
    for (MenuItem& item : level.items) {
        if (item.type == MenuItemType::SubMenu) {
            const int target = levelForOpenAction(item.action);
            if (target >= 0) {
                item.targetLevel = target;
            }
        }
    }
}

OverlayMenuPanel::MenuItem* OverlayMenuPanel::findRootItem(MenuAction action)
{
    for (int level : m_TabLevels) {
        if (*m_MenuLevels[level].tab == MenuTab::Quick && action != MenuAction::Quit &&
            action != MenuAction::QuitAndExit && action != MenuAction::ToggleMinimize)
            continue;
        for (MenuItem& item : m_MenuLevels[level].items) {
            if (item.action == action) {
                return &item;
            }
        }
    }
    for (MenuLevel& level : m_MenuLevels) {
        if (level.openAction != MenuAction::OpenMenuSettings)
            continue;
        for (MenuItem& item : level.items)
            if (item.action == action)
                return &item;
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
    const int focusCode = m_HoveredIndex;
    const bool hasFocusedRow = m_CurrentLevel >= 0 && m_CurrentLevel < int(m_MenuLevels.size()) &&
                               m_HoveredIndex >= 0 &&
                               m_HoveredIndex < int(m_MenuLevels[m_CurrentLevel].items.size());
    const MenuAction focusedAction = hasFocusedRow
                                         ? m_MenuLevels[m_CurrentLevel].items[m_HoveredIndex].action
                                         : MenuAction::MenuActionMax;
    const QString focusedPayload =
        hasFocusedRow ? m_MenuLevels[m_CurrentLevel].items[m_HoveredIndex].payload : QString();

    m_MenuLevels.clear();
    m_TabLevels.clear();

    // === Top-level entries. tabForAction() files them into tabs; their
    // order inside a tab is the order of the tab table. ===
    std::vector<MenuItem> entries;

    // Quick
    entries.push_back({ tr("Minimize"), QString(), MenuItemType::Action, MenuAction::ToggleMinimize,
                        0, true, false, true });
    entries.push_back({ tr("Microphone"), QString(), MenuItemType::Toggle,
                        MenuAction::ToggleMicrophone, 0, true, false, true });
    entries.push_back({ tr("Disconnect"), m_HasGamepads ? m_QuitComboGlyphs : QString(),
                        MenuItemType::Action, MenuAction::Quit, 0, true, false, false });
    entries.push_back({ tr("Quit Moonlight"), QString(), MenuItemType::Action,
                        MenuAction::QuitAndExit, 0, true, false, false });
    // Display. SubMenu target levels are filled in once the levels exist.
    entries.push_back({ tr("Window Mode"), QString(), MenuItemType::SubMenu,
                        MenuAction::OpenWindowMode, 0, true, false, false });
    entries.push_back({ tr("Resolution & frame rate"),
                        m_StreamQuality.fps > 0
                            ? tr("%1 · %2 FPS")
                                  .arg(StreamQuality::resolutionName(m_StreamQuality.resolution))
                                  .arg(m_StreamQuality.fps)
                            : QString(),
                        MenuItemType::SubMenu, MenuAction::OpenStreamQuality, 0, true, false,
                        false });
    entries.push_back({ tr("Overlay level & edit"), QString(), MenuItemType::SubMenu,
                        MenuAction::OpenStatsOverlay, 0, true, false, false });
    entries.push_back({ tr("Bitrate"), QString(), MenuItemType::SubMenu, MenuAction::OpenBitrate, 0,
                        true, false, false });
    entries.push_back({ tr("Mute stream audio"),
                        {},
                        MenuItemType::Toggle,
                        MenuAction::ToggleAudioMute,
                        0,
                        true,
                        false,
                        false });
    if (m_MatchSupported) {
        entries.push_back({ tr("Match window resolution"),
                            {},
                            MenuItemType::Toggle,
                            MenuAction::ToggleResolutionMatch,
                            0,
                            true,
                            false,
                            false });
        entries.push_back({ tr("Match now"),
                            {},
                            MenuItemType::Action,
                            MenuAction::MatchResolutionNow,
                            0,
                            true,
                            false,
                            false });
    }
    entries.push_back({ tr("Mouse mode"),
                        {},
                        MenuItemType::SubMenu,
                        MenuAction::OpenMouseMode,
                        0,
                        true,
                        false,
                        false });
    // Input
    entries.push_back({ tr("Send Keys"), QString(), MenuItemType::SubMenu, MenuAction::OpenSendKeys,
                        0, true, false, true });
    // Only show Gamepad Mouse toggle when a gamepad is actually connected
    if (m_HasGamepads) {
        entries.push_back({ tr("Controller as mouse"), QString(), MenuItemType::Toggle,
                            MenuAction::ToggleGamepadMouse, 0, true, false, false });
    }
    entries.push_back({ tr("Show local cursor"), QString(), MenuItemType::Toggle,
                        MenuAction::ToggleCursorHide, 0, true, false, false });
    entries.push_back({ tr("Release mouse"), QString(), MenuItemType::Action,
                        MenuAction::UngrabInput, 0, true, false, false });
    entries.push_back({ tr("Pointer region lock"), QString(), MenuItemType::Toggle,
                        MenuAction::TogglePointerRegionLock, 0, true, false, false });
    entries.push_back({ tr("System-key capture"), QString(), MenuItemType::Toggle,
                        MenuAction::ToggleKeyboardGrab, 0, true, false, false });
    entries.push_back({ tr("Paste Clipboard"), QString(), MenuItemType::Action,
                        MenuAction::PasteText, 0, true, false, m_RemoteUsbAvailable });
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
    entries.push_back({ tr("Cancel Sending Files"), QString(), MenuItemType::Action,
                        MenuAction::CancelFileTransfer, 0, m_FileTransferActive, false, false });
    // Session
    entries.push_back({ tr("Mute when unfocused"),
                        {},
                        MenuItemType::Toggle,
                        MenuAction::ToggleMuteOnFocusLoss,
                        0,
                        true,
                        false,
                        false });
#ifdef MOONLIGHT_ENABLE_FUNCTION_TESTS
    entries.push_back({ tr("Function Tests"), tr("Developer"), MenuItemType::Action,
                        MenuAction::OpenStylusReplayPanel, 0, true, false, false });
#endif

    std::stable_sort(entries.begin(), entries.end(), [](const MenuItem& a, const MenuItem& b) {
        return tabAssignmentOrder(a.action) < tabAssignmentOrder(b.action);
    });
    m_PinCatalog.clear();
    for (const MenuItem& item : entries) {
        if (item.action != MenuAction::Quit && item.action != MenuAction::QuitAndExit &&
            item.action != MenuAction::ToggleMinimize)
            m_PinCatalog.push_back(item);
    }

    // === Tab roots, one level per tab with entries (empty tabs are hidden) ===
    for (int t = 0; t < static_cast<int>(MenuTab::Count); t++) {
        MenuLevel tabLevel;
        tabLevel.title = tr("Overlay Menu");
        tabLevel.tab = static_cast<MenuTab>(t);
        auto header = [&](const QString& label) {
            tabLevel.items.push_back({ label,
                                       {},
                                       MenuItemType::Header,
                                       MenuAction::MenuActionMax,
                                       0,
                                       false,
                                       false,
                                       false });
        };
        if (*tabLevel.tab == MenuTab::Quick) {
            for (const QString& pin : m_QuickPins)
                for (const MenuItem& item : m_PinCatalog)
                    if (QString::number(int(item.action)) == pin)
                        tabLevel.items.push_back(item);
        }
        for (const MenuItem& item : entries) {
            if (tabForAction(item.action) == *tabLevel.tab) {
                switch (item.action) {
                case MenuAction::OpenStreamQuality:
                    header(tr("Quality"));
                    break;
                case MenuAction::ToggleAudioMute:
                    header(tr("Audio"));
                    break;
                case MenuAction::OpenWindowMode:
                    header(tr("Window"));
                    break;
                case MenuAction::OpenStatsOverlay:
                    header(tr("Overlay"));
                    break;
                case MenuAction::OpenMouseMode:
                    header(tr("Mouse"));
                    break;
                case MenuAction::ToggleKeyboardGrab:
                    header(tr("Keyboard"));
                    break;
                case MenuAction::ToggleGamepadMouse:
                    header(tr("Controller"));
                    break;
                case MenuAction::OpenRemoteUsbDevices:
                    header(tr("Devices"));
                    break;
                default:
                    break;
                }
                tabLevel.items.push_back(item);
                if (item.action == MenuAction::OpenMouseMode) {
                    tabLevel.items.push_back({ tr("Pointer capture"),
                                               {},
                                               MenuItemType::Info,
                                               MenuAction::MenuActionMax,
                                               0,
                                               false,
                                               false,
                                               false,
                                               QStringLiteral("capture-info") });
                }
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
    bitrate.items.push_back({ QString(), QString(), MenuItemType::Slider, MenuAction::MenuActionMax,
                              0, true, false, true });
    static const int kBitratePresets[] = { 1000, 2000, 5000, 10000, 20000, 30000, 50000, 100000 };
    for (int kbps : kBitratePresets) {
        bitrate.items.push_back({ formatBitrateKbps(kbps), QString(), MenuItemType::Action,
                                  MenuAction::SetBitrate, 0, true, false, false,
                                  QString::number(kbps) });
    }
    m_MenuLevels.push_back(std::move(bitrate));

    // === Overlay menu placement ===
    MenuLevel placement;
    placement.title = tr("Menu Position");
    placement.openAction = MenuAction::OpenMenuPosition;
    placement.items.push_back({ tr("Detach menu"), QString(), MenuItemType::Action,
                                MenuAction::ToggleMenuDetached, 0, true, false, true });
    placement.items.push_back({ tr("Top edge"), QString(), MenuItemType::Action,
                                MenuAction::SetMenuPlacementTop, 0, true, false, false });
    placement.items.push_back({ tr("Right edge"), QString(), MenuItemType::Action,
                                MenuAction::SetMenuPlacementRight, 0, true, false, false });
    placement.items.push_back({ tr("Left edge"), QString(), MenuItemType::Action,
                                MenuAction::SetMenuPlacementLeft, 0, true, false, false });
    placement.items.push_back({ tr("Floating button"), QString(), MenuItemType::Action,
                                MenuAction::SetMenuPlacementButton, 0, true, false, false });
    placement.items.push_back({ tr("Disabled"), QString(), MenuItemType::Action,
                                MenuAction::SetMenuPlacementDisabled, 0, true, false, true });
    m_MenuLevels.push_back(std::move(placement));

    // === Overlay menu layout and scale ===
    MenuLevel menuLayout;
    menuLayout.title = tr("Menu Layout");
    menuLayout.openAction = MenuAction::OpenMenuLayout;
    menuLayout.items.push_back({ tr("Compact"), QString(), MenuItemType::Action,
                                 MenuAction::SetMenuLayoutCompact, 0, true, false, false });
    menuLayout.items.push_back({ tr("Comfortable"), QString(), MenuItemType::Action,
                                 MenuAction::SetMenuLayoutComfortable, 0, true, false, false });
    menuLayout.items.push_back({ tr("Side panel"), QString(), MenuItemType::Action,
                                 MenuAction::SetMenuLayoutSidePanel, 0, true, false, true });
    for (int percent : { 75, 100, 125, 150 }) {
        menuLayout.items.push_back({ tr("Scale %1%").arg(percent), QString(), MenuItemType::Action,
                                     MenuAction::SetMenuScale, 0, true, false, false,
                                     QString::number(percent) });
    }
    m_MenuLevels.push_back(std::move(menuLayout));

    // === Remote USB devices ===
    if (m_RemoteUsbAvailable) {
        MenuLevel usb;
        usb.title = tr("USB Devices");
        usb.openAction = MenuAction::OpenRemoteUsbDevices;
        if (m_RemoteUsbDevices.empty()) {
            const QString emptyDetail = m_RemoteUsbState == RemoteUsbState::Discovering
                                            ? tr("Scanning")
                                            : tr("No devices found");
            usb.items.push_back({ tr("USB Devices"), emptyDetail, MenuItemType::Action,
                                  MenuAction::MenuActionMax, 0, false, false, false });
        } else {
            for (const RemoteUsbDevice& device : m_RemoteUsbDevices) {
                const bool active =
                    !m_RemoteUsbActiveDeviceId.isEmpty() && device.id == m_RemoteUsbActiveDeviceId;
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
                const bool hasOpenDevice = m_RemoteUsbState == RemoteUsbState::Open &&
                                           !m_RemoteUsbActiveDeviceId.isEmpty();
                if (m_RemoteUsbNeedsActivation && device.supported) {
                    detail = tr("Select to activate");
                    if (!device.detail.isEmpty()) {
                        detail += QStringLiteral(" · ") + device.detail;
                    }
                }
                const bool canRelease = active && (m_RemoteUsbState == RemoteUsbState::Opening ||
                                                   m_RemoteUsbState == RemoteUsbState::Open);
                usb.items.push_back(
                    { device.label, detail, MenuItemType::Action,
                      canRelease ? MenuAction::ReleaseRemoteUsbDevice
                                 : MenuAction::SelectRemoteUsbDevice,
                      0, canRelease || (device.supported && !busy && (!hasOpenDevice || active)),
                      active, false, device.id });
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
    stats.items.push_back({ tr("Cycle Preset"), QString(), MenuItemType::Action,
                            MenuAction::CycleOverlayPreset, 0, true, false, false });
    stats.items.push_back({ tr("Edit position"), QString(), MenuItemType::Action,
                            MenuAction::EditStatsOverlayPosition, 0, m_StatsOverlayEditable, false,
                            false });
    stats.items.push_back({ tr("Detach overlay"), QString(), MenuItemType::Action,
                            MenuAction::ToggleStatsOverlayDetached, 0, m_StatsOverlayEditable,
                            false, false });
    stats.items.push_back({ tr("Opacity"), QString(), MenuItemType::SubMenu,
                            MenuAction::OpenStatsOpacity, 0, true, false, false });
    m_MenuLevels.push_back(std::move(stats));

    MenuLevel statsOpacity;
    statsOpacity.title = tr("Overlay Opacity");
    statsOpacity.openAction = MenuAction::OpenStatsOpacity;
    for (int percent : { 30, 50, 70, 85, 100 }) {
        statsOpacity.items.push_back({ tr("%1%").arg(percent), QString(), MenuItemType::Action,
                                       MenuAction::SetStatsOpacity, 0, true, false, false,
                                       QString::number(percent) });
    }
    m_MenuLevels.push_back(std::move(statsOpacity));

    MenuLevel windowMode;
    windowMode.title = tr("Window Mode");
    windowMode.openAction = MenuAction::OpenWindowMode;
    windowMode.items = {
        { tr("Windowed"), QString(), MenuItemType::Action, MenuAction::SetWindowModeWindowed, 0,
          true, false, false },
        { tr("Borderless"), QString(), MenuItemType::Action, MenuAction::SetWindowModeBorderless, 0,
          true, false, false },
        { tr("Fullscreen"), QString(), MenuItemType::Action, MenuAction::SetWindowModeFullscreen, 0,
          true, false, false },
    };
    m_MenuLevels.push_back(std::move(windowMode));

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
    m_MenuLevels.push_back(buildStreamQualityLevel());

    MenuLevel mouseMode;
    mouseMode.title = tr("Mouse mode");
    mouseMode.openAction = MenuAction::OpenMouseMode;
    mouseMode.items = { { tr("Game"),
                          {},
                          MenuItemType::Action,
                          MenuAction::SetMouseModeGame,
                          0,
                          true,
                          false,
                          false },
                        { tr("Desktop"),
                          {},
                          MenuItemType::Action,
                          MenuAction::SetMouseModeDesktop,
                          0,
                          true,
                          false,
                          false } };
    m_MenuLevels.push_back(std::move(mouseMode));
    MenuLevel settings;
    settings.title = tr("Menu settings");
    settings.openAction = MenuAction::OpenMenuSettings;
    settings.items = { { tr("Position"),
                         {},
                         MenuItemType::SubMenu,
                         MenuAction::OpenMenuPosition,
                         0,
                         true,
                         false,
                         false },
                       { tr("Layout & scale"),
                         {},
                         MenuItemType::SubMenu,
                         MenuAction::OpenMenuLayout,
                         0,
                         true,
                         false,
                         false },
                       { tr("Shortcuts"),
                         {},
                         MenuItemType::SubMenu,
                         MenuAction::OpenShortcuts,
                         0,
                         true,
                         false,
                         false },
                       { tr("Quick pins"),
                         {},
                         MenuItemType::SubMenu,
                         MenuAction::OpenQuickPins,
                         0,
                         true,
                         false,
                         false } };
    m_MenuLevels.push_back(std::move(settings));
    MenuLevel pins;
    pins.title = tr("Quick pins");
    pins.openAction = MenuAction::OpenQuickPins;
    for (const MenuItem& item : m_PinCatalog)
        pins.items.push_back({ item.label,
                               {},
                               MenuItemType::Toggle,
                               MenuAction::ToggleQuickPin,
                               0,
                               true,
                               m_QuickPins.contains(QString::number(int(item.action))),
                               false,
                               QString::number(int(item.action)) });
    m_MenuLevels.push_back(std::move(pins));
    MenuLevel shortcuts;
    shortcuts.title = tr("Shortcuts");
    shortcuts.openAction = MenuAction::OpenShortcuts;
    for (const ShortcutAction& action : ShortcutRegistry::instance().actions())
        shortcuts.items.push_back({ action.translatedName(),
                                    {},
                                    MenuItemType::Action,
                                    MenuAction::OpenShortcutAction,
                                    0,
                                    true,
                                    false,
                                    false,
                                    action.id });
    m_MenuLevels.push_back(std::move(shortcuts));
    MenuLevel shortcut;
    shortcut.title = tr("Edit shortcut");
    shortcut.openAction = MenuAction::OpenShortcutAction;
    m_MenuLevels.push_back(std::move(shortcut));
    rebuildShortcutEditor();

    // Point SubMenu rows at their levels by the Open* action they carry. This
    // covers tab entries and rows inside a sub-level that link to another
    // one (e.g. the Bitrate row of Stream Quality, tagged MenuAction::OpenBitrate).
    for (MenuLevel& level : m_MenuLevels) {
        linkSubMenuRows(level);
    }
    m_StatsOverlayLevel = levelForOpenAction(MenuAction::OpenStatsOverlay);
    m_StatsOpacityLevel = levelForOpenAction(MenuAction::OpenStatsOpacity);
    m_SendKeysLevel = levelForOpenAction(MenuAction::OpenSendKeys);
    m_HostCommandsLevel = levelForOpenAction(MenuAction::OpenHostCommands);
    m_StreamQualityLevel = levelForOpenAction(MenuAction::OpenStreamQuality);
    for (int level : m_TabLevels)
        if (*m_MenuLevels[level].tab == MenuTab::Session)
            m_SessionStatsLevel = level;
    applyStatsOverlayState();
    updateWindowModeState(m_StreamWindowMode, m_CanWindowed, m_CanBorderless, m_CanFullscreen);
    refreshSessionStats();

    // Stay in the open sub-level if it still exists, else in the tab
    const int reopenedLevel = levelForOpenAction(openLevelAction);
    m_CurrentLevel = reopenedLevel >= 0 ? reopenedLevel : tabRootLevel();

    // Rebuilds (gamepad set / USB refresh) wipe derived details; re-stamp
    // the bitrate state so the scrubber row and preset checkmarks survive.
    refreshBitrateDetails();
    updateMenuStyleState();
    if (m_ActivePlacementAction) {
        updateMenuPositionState(*m_ActivePlacementAction);
    }
    applyMenuAttachmentState();
    applyOwnedState();
    m_HoveredIndex = hasFocusedRow ? rowOfAction(focusedAction, focusedPayload) : focusCode;
    if (m_Visible)
        repositionWindow();
}

// ---------------------------------------------------------------------------
// Dynamic state updates
// ---------------------------------------------------------------------------

void OverlayMenuPanel::updateMicrophoneState(bool enabled)
{
    m_MicrophoneEnabled = enabled;
    applyOwnedState();
    forceRepaint();
}

void OverlayMenuPanel::updateGamepadMouseState(bool enabled)
{
    m_GamepadMouseEnabled = enabled;
    applyOwnedState();
    forceRepaint();
}

void OverlayMenuPanel::syncQuick()
{
    for (int level : m_TabLevels) {
        if (*m_MenuLevels[level].tab != MenuTab::Quick)
            continue;
        for (MenuItem& item : m_MenuLevels[level].items) {
            if (MenuItem* canonical = findRootItem(item.action)) {
                if (canonical != &item)
                    item = *canonical;
            }
        }
    }
}

void OverlayMenuPanel::applyOwnedState()
{
    for (MenuLevel& level : m_MenuLevels) {
        for (MenuItem& item : level.items) {
            switch (item.action) {
            case MenuAction::OpenMouseMode:
                item.detail = m_DesktopMode ? tr("Desktop") : tr("Game");
                break;
            case MenuAction::SetMouseModeGame:
            case MenuAction::SetMouseModeDesktop:
                item.detail = (item.action == MenuAction::SetMouseModeDesktop) == m_DesktopMode
                                  ? QString(QChar(0x2713))
                                  : QString();
                break;
            case MenuAction::UngrabInput:
                item.enabled = m_CapturedBeforeMenu;
                item.detail = tr("Click the stream to recapture");
                break;
            case MenuAction::ToggleCursorHide:
                item.enabled = m_DesktopMode;
                item.toggleState = m_CursorVisible;
                item.detail = m_DesktopMode ? QString() : tr("Desktop only");
                break;
            case MenuAction::TogglePointerRegionLock:
                item.enabled = m_DesktopMode;
                item.toggleState = m_RegionLock;
                item.detail = m_DesktopMode ? QString() : tr("Desktop only");
                break;
            case MenuAction::ToggleKeyboardGrab:
                item.toggleState = m_SystemKeyCapture;
                break;
            case MenuAction::ToggleMicrophone:
                item.toggleState = m_MicrophoneEnabled;
                break;
            case MenuAction::ToggleGamepadMouse:
                item.toggleState = m_GamepadMouseEnabled;
                break;
            case MenuAction::ToggleAudioMute:
                item.toggleState = m_ManuallyMuted;
                break;
            case MenuAction::ToggleMuteOnFocusLoss:
                item.toggleState = m_MuteOnFocusLoss;
                break;
            case MenuAction::ToggleResolutionMatch:
                item.toggleState = m_AutomaticMatch;
                item.detail =
                    m_MatchFullscreen ? tr("Configured stream size") : tr("Exact even window size");
                break;
            case MenuAction::MatchResolutionNow:
                item.enabled = m_CanMatchNow;
                item.detail = m_MatchFullscreen
                                  ? tr("Request stream size · best effort")
                                  : tr("Request exact even window size · best effort");
                break;
            case MenuAction::QuitAndExit:
                item.detail = m_QuitConfirmTimer.isActive()
                                  ? tr("Press again within 3 seconds to quit")
                                  : QString();
                break;
            default:
                break;
            }
            if (item.payload == QStringLiteral("capture-info"))
                item.detail = m_CapturedBeforeMenu ? tr("Released in menu · restores on close")
                                                   : tr("Released in menu · stays free on close");
        }
    }
    syncQuick();
}

void OverlayMenuPanel::updateInputState(bool desktopMode, bool capturedBeforeMenu,
                                        bool cursorVisible, bool regionLock, bool systemKeyCapture)
{
    m_DesktopMode = desktopMode;
    m_CapturedBeforeMenu = capturedBeforeMenu;
    m_CursorVisible = cursorVisible;
    m_RegionLock = regionLock;
    m_SystemKeyCapture = systemKeyCapture;
    applyOwnedState();
    forceRepaint();
}

void OverlayMenuPanel::updateAudioState(bool manuallyMuted, bool muteOnFocusLoss)
{
    m_ManuallyMuted = manuallyMuted;
    m_MuteOnFocusLoss = muteOnFocusLoss;
    applyOwnedState();
    forceRepaint();
}

void OverlayMenuPanel::updateResolutionMatchState(bool hostSupported, bool automatic,
                                                  bool fullscreen, bool canMatchNow)
{
    const bool rebuild = m_MatchSupported != hostSupported;
    m_MatchSupported = hostSupported;
    m_AutomaticMatch = automatic;
    m_MatchFullscreen = fullscreen;
    m_CanMatchNow = canMatchNow;
    if (rebuild)
        buildMenuLevels();
    else
        applyOwnedState();
    forceRepaint();
}

void OverlayMenuPanel::setConnectionInfo(std::vector<std::pair<QString, QString>> facts)
{
    m_ConnectionInfo = std::move(facts);
    refreshSessionStats();
    forceRepaint();
}

void OverlayMenuPanel::rebuildShortcutEditor()
{
    const int index = levelForOpenAction(MenuAction::OpenShortcutAction);
    if (index < 0)
        return;
    auto& level = m_MenuLevels[index];
    level.items.clear();
    const ShortcutAction* action = ShortcutRegistry::instance().find(m_EditingShortcut);
    if (!action)
        return;
    level.title = action->translatedName();
    if (m_EditingShortcut == QLatin1String(ShortcutActions::RightAltMouseRelease)) {
        level.items.push_back({ tr("Hold Right Alt to release the mouse"),
                                {},
                                MenuItemType::Toggle,
                                MenuAction::ToggleRightAltRelease,
                                0,
                                true,
                                m_ShortcutProfile.rightAltReleaseEnabled(),
                                false });
        level.items.push_back(
            { tr("Right Alt is reserved"), tr("Fixed hold shortcut · enable or disable it here"),
              MenuItemType::Info, MenuAction::MenuActionMax, 0, false, false, false });
        return;
    }
    const auto bindings = m_ShortcutProfile.bindings(m_EditingShortcut);
    for (int slot = 0; slot < ShortcutRegistry::MaxBindings; ++slot) {
        level.items.push_back({ tr("Binding %1").arg(slot + 1),
                                m_CaptureShortcutSlot == slot
                                    ? tr("Press a key combination · Esc cancels")
                                : bindings[slot].isNull() ? tr("Not assigned")
                                                          : bindings[slot].toDisplayString(),
                                MenuItemType::Action, MenuAction::CaptureShortcut, 0, true, false,
                                false, QString::number(slot) });
        level.items.push_back({ tr("Clear binding %1").arg(slot + 1),
                                {},
                                MenuItemType::Action,
                                MenuAction::ClearShortcut,
                                0,
                                true,
                                false,
                                false,
                                QString::number(slot) });
    }
    level.items.push_back({ tr("Reset to default"),
                            {},
                            MenuItemType::Action,
                            MenuAction::ResetShortcut,
                            0,
                            true,
                            false,
                            false });
    if (!m_ShortcutError.isEmpty())
        level.items.push_back({ tr("Shortcut not saved"), m_ShortcutError, MenuItemType::Info,
                                MenuAction::MenuActionMax, 0, false, false, false });
}

void OverlayMenuPanel::saveShortcutCandidate(const ShortcutProfile& candidate, int slot)
{
    QStringList conflicts;
    if (slot >= 0)
        conflicts = candidate.conflictingActions(m_EditingShortcut, slot);
    else
        for (int i = 0; i < ShortcutRegistry::MaxBindings; ++i)
            conflicts.append(candidate.conflictingActions(m_EditingShortcut, i));
    if (!conflicts.isEmpty()) {
        m_ShortcutError =
            tr("Conflicts with %1 · choose another binding").arg(conflicts.join(", "));
    } else {
        m_ShortcutProfile = candidate;
        m_ShortcutProfile.saveToSettings();
        m_ShortcutError.clear();
        if (m_ShortcutProfileCallback)
            m_ShortcutProfileCallback(m_ShortcutProfile);
    }
    rebuildShortcutEditor();
    repositionWindow();
    forceRepaint();
}

bool OverlayMenuPanel::handleEditorItem(const MenuItem& item)
{
    switch (item.action) {
    case MenuAction::ToggleRightAltRelease: {
        ShortcutProfile candidate = m_ShortcutProfile;
        candidate.setRightAltReleaseEnabled(!candidate.rightAltReleaseEnabled());
        saveShortcutCandidate(candidate, -1);
        return true;
    }
    case MenuAction::OpenShortcutAction:
        m_EditingShortcut = item.payload;
        m_CaptureShortcutSlot = -1;
        m_ShortcutError.clear();
        rebuildShortcutEditor();
        navigateToLevel(levelForOpenAction(MenuAction::OpenShortcutAction));
        return true;
    case MenuAction::CaptureShortcut:
        m_CaptureShortcutSlot = item.payload.toInt();
        m_ShortcutModifierRecorder.reset();
        m_ShortcutCapturedKeys.clear();
        m_ShortcutError.clear();
        rebuildShortcutEditor();
        forceRepaint();
        return true;
    case MenuAction::ClearShortcut: {
        ShortcutProfile candidate = m_ShortcutProfile;
        candidate.setBinding(m_EditingShortcut, item.payload.toInt(), {});
        saveShortcutCandidate(candidate, item.payload.toInt());
        return true;
    }
    case MenuAction::ResetShortcut: {
        ShortcutProfile candidate = m_ShortcutProfile;
        candidate.resetAction(m_EditingShortcut);
        saveShortcutCandidate(candidate, -1);
        return true;
    }
    case MenuAction::ToggleQuickPin:
        if (m_QuickPins.contains(item.payload))
            m_QuickPins.removeAll(item.payload);
        else
            m_QuickPins.append(item.payload);
        QSettings().setValue(QStringLiteral("overlay-menu/quick-pins"), m_QuickPins);
        buildMenuLevels();
        forceRepaint();
        return true;
    default:
        return false;
    }
}

bool OverlayMenuPanel::focusable(const MenuItem& item) const
{
    return item.enabled && item.type != MenuItemType::Info && item.type != MenuItemType::Header;
}

void OverlayMenuPanel::activateItem(MenuItem item)
{
    if (!focusable(item))
        return;
    beginInteraction();
    if (handleEditorItem(item))
        return;
    switch (item.type) {
    case MenuItemType::SubMenu:
        navigateToLevel(item.targetLevel);
        break;
    case MenuItemType::Back:
        goBack();
        break;
    case MenuItemType::Toggle:
        // Session is the single state owner. It pushes the accepted snapshot,
        // synchronously or later; do not optimistically mutate a Quick clone.
        if (m_ActionCallback)
            m_ActionCallback(item.action);
        break;
    case MenuItemType::Action:
        if (item.action == MenuAction::SetBitrate)
            selectBitratePreset(item);
        else
            dispatchActionItem(item);
        break;
    case MenuItemType::Slider:
        commitBitrateNow();
        break;
    default:
        break;
    }
}

void OverlayMenuPanel::updateBitrateState(int bitrateKbps)
{
    if (m_MenuLevels.empty())
        return;

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
    if (m_MenuLevels.empty())
        return;

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
                                  ? QString::fromUtf8("\342\234\223")
                                  : QString();
            }
        }
    }

    // The Stream Quality level links to the Bitrate level and shows its value.
    if (m_StreamQualityLevel >= 0 && m_StreamQualityLevel < (int)m_MenuLevels.size()) {
        for (auto& item : m_MenuLevels[m_StreamQualityLevel].items) {
            if (item.type == MenuItemType::SubMenu && item.action == MenuAction::OpenBitrate) {
                item.detail = formatBitrateKbps(m_BitrateKbps);
            }
        }
    }
    syncQuick();
}

double OverlayMenuPanel::bitrateFraction() const
{
    return qBound(0.0, bitrateToSliderPosition(m_BitrateKbps) / kBitrateSliderMax, 1.0);
}

void OverlayMenuPanel::setBitrateKbps(int bitrateKbps)
{
    bitrateKbps = qBound(kBitrateMinKbps, bitrateKbps, kBitrateMaxKbps);
    bitrateKbps = sliderPositionToBitrate(bitrateToSliderPosition(bitrateKbps));
    if (bitrateKbps == m_BitrateKbps)
        return;

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
    if (m_BitrateKbps == m_CommittedBitrateKbps)
        return;

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
    r.plus = QRect(contentWidth - textPad - buttonSize, itemY + (m_ItemHeight - buttonSize) / 2,
                   buttonSize, buttonSize);
    r.minus = QRect(r.plus.x() - buttonSize - buttonGap, r.plus.y(), buttonSize, buttonSize);
    const int trackLeft = r.value.right() + 1 + 8;
    const int trackRight = r.minus.x() - 6;
    r.track = QRect(trackLeft, itemY + m_ItemHeight / 2 - 3, qMax(0, trackRight - trackLeft), 6);
    return r;
}

OverlayMenuPanel::SliderZone OverlayMenuPanel::sliderZoneAt(const QPoint& windowPos,
                                                            int rowIdx) const
{
    const auto& items = m_MenuLevels[m_CurrentLevel].items;
    if (rowIdx < 0 || rowIdx >= (int)items.size() || items[rowIdx].type != MenuItemType::Slider) {
        return SliderZone::None;
    }

    // Row rects live in row coordinates; callers hand us layout coords.
    const QPoint contentPos = windowPos - QPoint(m_ShadowMargin + rowsLeft(), m_ShadowMargin);
    const int itemY = contentTop() + rowTop(rowIdx) - m_ScrollOffset;
    const SliderRowRects r = sliderRowRects(rowsWidth(), itemY);

    // Buttons win over the track so their hit area feels solid.
    if (r.minus.adjusted(-2, -2, 2, 2).contains(contentPos))
        return SliderZone::Minus;
    if (r.plus.adjusted(-2, -2, 2, 2).contains(contentPos))
        return SliderZone::Plus;
    if (QRect(r.track.x() - 4, itemY, r.track.width() + 8, m_ItemHeight).contains(contentPos)) {
        return SliderZone::Track;
    }
    return SliderZone::None;
}

void OverlayMenuPanel::gamepadAdjustSlider(int direction)
{
    if (!m_Visible)
        return;
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
    if (rowIdx < 0)
        return;

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

std::optional<OverlayMenuPanel::WindowMode> OverlayMenuPanel::windowModeForAction(MenuAction action)
{
    switch (action) {
    case MenuAction::SetWindowModeWindowed:
        return WindowMode::Windowed;
    case MenuAction::SetWindowModeBorderless:
        return WindowMode::Borderless;
    case MenuAction::SetWindowModeFullscreen:
        return WindowMode::Fullscreen;
    default:
        return std::nullopt;
    }
}

void OverlayMenuPanel::updateWindowModeState(WindowMode current, bool windowed, bool borderless,
                                             bool fullscreen)
{
    m_StreamWindowMode = current;
    m_CanWindowed = windowed;
    m_CanBorderless = borderless;
    m_CanFullscreen = fullscreen;
    QString currentLabel;
    for (MenuLevel& level : m_MenuLevels) {
        for (MenuItem& item : level.items) {
            if (const auto mode = windowModeForAction(item.action)) {
                item.enabled = *mode == WindowMode::Windowed     ? windowed
                               : *mode == WindowMode::Borderless ? borderless
                                                                 : fullscreen;
                item.detail = *mode == current ? QString::fromUtf8("\342\234\223")
                              : !item.enabled  ? tr("Not supported")
                                               : QString();
                if (*mode == current)
                    currentLabel = item.label;
            }
        }
    }
    for (MenuLevel& level : m_MenuLevels) {
        for (MenuItem& item : level.items) {
            if (item.action == MenuAction::OpenWindowMode)
                item.detail = currentLabel;
        }
    }
    syncQuick();
    requestUpdate();
}

void OverlayMenuPanel::updateMenuPositionState(MenuAction activePlacementAction)
{
    m_ActivePlacementAction = activePlacementAction;
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

void OverlayMenuPanel::updateMenuStyleState()
{
    const int layoutLevel = levelForOpenAction(MenuAction::OpenMenuLayout);
    if (layoutLevel < 0) {
        return;
    }

    const QString check = QString::fromUtf8("\342\234\223");
    QString activeLabel;
    for (MenuItem& item : m_MenuLevels[layoutLevel].items) {
        bool active = false;
        if (const auto layout = layoutForAction(item.action)) {
            active = *layout == m_Layout;
            if (active) {
                activeLabel = item.label;
            }
        } else if (item.action == MenuAction::SetMenuScale) {
            active = item.payload.toInt() == scalePercent();
        }
        item.detail = active ? check : QString();
    }

    if (MenuItem* entry = findRootItem(MenuAction::OpenMenuLayout)) {
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

void OverlayMenuPanel::updateStatsOverlayState(StatsOverlay::Preset preset, bool canEditPosition,
                                               bool detached, int opacityPercent)
{
    m_StatsOverlayPreset = preset;
    m_StatsOverlayEditable = canEditPosition;
    m_StatsOverlayDetached = detached;
    m_StatsOverlayOpacityPercent =
        qBound(StatsOverlay::MinOpacityPercent, opacityPercent, StatsOverlay::MaxOpacityPercent);
    applyStatsOverlayState();
    if (m_Visible) {
        repositionWindow();
    }
    forceRepaint();
}

void OverlayMenuPanel::updateMenuAttachmentState(bool detached)
{
    m_MenuDetached = detached;
    applyMenuAttachmentState();
    if (m_Visible) {
        repositionWindow();
    }
    forceRepaint();
}

void OverlayMenuPanel::setDetached(bool detached)
{
    if (detached == m_Detached || m_Visible || m_Closing) {
        return;
    }

    const int shadow = qRound(m_ShadowMargin * m_Scale);
    if (detached) {
        QPoint windowTopLeft = m_TargetPosition;
        if (OverlayWindowPolicy::usesParentCoordinates(m_WindowMode) && parent() != nullptr) {
            windowTopLeft = parent()->mapToGlobal(windowTopLeft);
            QScreen* targetScreen = QGuiApplication::screenAt(windowTopLeft);
            if (targetScreen == nullptr) {
                targetScreen = parent()->screen();
            }
            if (targetScreen != nullptr &&
                !targetScreen->availableGeometry().contains(windowTopLeft)) {
                windowTopLeft =
                    targetScreen->availableGeometry().center() - QPoint(width() / 2, height() / 2);
            }
        }
        m_DetachedPosition = windowTopLeft + QPoint(shadow, shadow);
    }

    const OverlayWindowMode targetMode =
        detached ? OverlayWindowMode::Detached : m_OriginalWindowMode;
    QWindow* const targetParent = detached ? nullptr : m_OriginalParent;
    if (targetMode != m_WindowMode || targetParent != parent()) {
        reconfigureSurface(targetParent, targetMode, OverlayWindowPolicy::flags(targetMode));
        m_WindowMode = targetMode;
    }

    m_Detached = detached;
    m_MenuDetached = detached;
    if (detached && m_DetachedPosition) {
        setPosition(*m_DetachedPosition - QPoint(shadow, shadow));
        m_TargetPosition = position();
    } else if (!detached) {
        m_DetachedPosition.reset();
        m_DraggedPosition.reset();
    }
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
    items.erase(std::remove_if(items.begin(), items.end(),
                               [](const MenuItem& item) {
                                   return item.type == MenuItemType::Info ||
                                          item.type == MenuItemType::Header;
                               }),
                items.end());
    std::vector<MenuItem> info;
    info.push_back({ tr("Connection"),
                     {},
                     MenuItemType::Header,
                     MenuAction::MenuActionMax,
                     0,
                     false,
                     false,
                     false });
    for (const auto& fact : m_ConnectionInfo)
        info.push_back({ fact.first, fact.second, MenuItemType::Info, MenuAction::OpenSessionStats,
                         0, false, false, false });
    info.push_back({ tr("Live totals"),
                     {},
                     MenuItemType::Header,
                     MenuAction::MenuActionMax,
                     0,
                     false,
                     false,
                     false });
    for (auto& row : rows) {
        info.push_back({ std::move(row.first), std::move(row.second), MenuItemType::Info,
                         MenuAction::OpenSessionStats, 0, false, false, false });
    }
    items.insert(items.begin(), info.begin(), info.end());

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
        if (item.action == MenuAction::ToggleStatsOverlayDetached) {
            item.label = m_StatsOverlayDetached ? tr("Attach overlay") : tr("Detach overlay");
            item.enabled =
                m_StatsOverlayEditable && m_StatsOverlayPreset != StatsOverlay::Preset::Off;
            continue;
        }
        if (item.action == MenuAction::OpenStatsOpacity) {
            item.detail = tr("%1%").arg(m_StatsOverlayOpacityPercent);
            item.enabled =
                m_StatsOverlayEditable && m_StatsOverlayPreset != StatsOverlay::Preset::Off;
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
    syncQuick();

    if (m_StatsOpacityLevel >= 0 &&
        m_MenuLevels.size() > static_cast<size_t>(m_StatsOpacityLevel)) {
        for (auto& item : m_MenuLevels[m_StatsOpacityLevel].items) {
            bool ok = false;
            const int percent = item.payload.toInt(&ok);
            item.detail = ok && percent == m_StatsOverlayOpacityPercent
                              ? QString::fromUtf8("\342\234\223")
                              : QString();
        }
    }
}

void OverlayMenuPanel::applyMenuAttachmentState()
{
    const int placementLevel = levelForOpenAction(MenuAction::OpenMenuPosition);
    if (placementLevel >= 0 && m_MenuLevels.size() > static_cast<size_t>(placementLevel)) {
        for (auto& item : m_MenuLevels[placementLevel].items) {
            if (item.action == MenuAction::ToggleMenuDetached) {
                item.label = m_MenuDetached ? tr("Attach menu") : tr("Detach menu");
            }
        }
    }

    if (MenuItem* entry = findRootItem(MenuAction::OpenMenuPosition)) {
        entry->detail = m_MenuDetached ? tr("Detached") : QString();
    }
}

void OverlayMenuPanel::updateFileMappingState(FileMappingState state, const QString& detail)
{
    m_FileMappingState = state;
    m_FileMappingDetail = detail;

    if (MenuItem* item = findRootItem(MenuAction::ShowHostFiles)) {
        item->detail = detail;
        item->toggleState = state == FileMappingState::Available || state == FileMappingState::Open;
        syncQuick();
        forceRepaint();
    }
}

void OverlayMenuPanel::updateFileTransferState(bool active)
{
    if (m_FileTransferActive == active) {
        return;
    }
    m_FileTransferActive = active;

    if (MenuItem* item = findRootItem(MenuAction::CancelFileTransfer)) {
        item->enabled = active;
        syncQuick();
        forceRepaint();
    }
}

void OverlayMenuPanel::updateRemoteUsbState(bool available, RemoteUsbState state,
                                            std::vector<RemoteUsbDevice> devices,
                                            const QString& activeDeviceId, const QString& detail)
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
    return showLevel(MenuAction::OpenHostCommands);
}

bool OverlayMenuPanel::showSendKeysLevel()
{
    if (!m_Visible || m_SendKeysLevel < 0) {
        return false;
    }
    return showLevel(MenuAction::OpenSendKeys);
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
    // Pointer activation navigates directly and retains its initiating Quick
    // clone. Programmatic opens use that clone only if present on this tab;
    // otherwise establish the canonical root before recording Back.
    int row = isAtTabRoot() ? rowOfAction(openAction) : -1;
    const bool global =
        openAction == MenuAction::OpenMenuSettings || openAction == MenuAction::OpenMenuPosition ||
        openAction == MenuAction::OpenMenuLayout || openAction == MenuAction::OpenShortcuts ||
        openAction == MenuAction::OpenShortcutAction || openAction == MenuAction::OpenQuickPins;
    if (row < 0 && !global) {
        showTab(tabForAction(openAction));
        row = rowOfAction(openAction);
    }
    if (row >= 0)
        m_HoveredIndex = row;
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

static QRect scaledRect(const QRect& rect, qreal scale)
{
    return QRectF(rect.x() * scale, rect.y() * scale, rect.width() * scale, rect.height() * scale)
        .toAlignedRect();
}

QRect OverlayMenuPanel::rowGeometry(int row) const
{
    if (row < 0 || row >= static_cast<int>(m_MenuLevels[m_CurrentLevel].items.size())) {
        return QRect();
    }
    return scaledRect(QRect(m_ShadowMargin + rowsLeft(),
                            m_ShadowMargin + contentTop() + rowTop(row) - m_ScrollOffset,
                            rowsWidth(), rowHeight(m_MenuLevels[m_CurrentLevel].items[row])),
                      m_Scale);
}

QRect OverlayMenuPanel::gearGeometry() const
{
    return scaledRect(QRect(m_ShadowMargin + m_MenuWidth - 2 * m_TitleHeight, m_ShadowMargin,
                            m_TitleHeight, m_TitleHeight),
                      m_Scale);
}

QRect OverlayMenuPanel::bitrateTrackGeometry(int row) const
{
    const auto& items = m_MenuLevels[m_CurrentLevel].items;
    if (row < 0 || row >= int(items.size()) || items[row].type != MenuItemType::Slider)
        return {};
    const auto rects = sliderRowRects(rowsWidth(), contentTop() + rowTop(row) - m_ScrollOffset);
    return scaledRect(rects.track.translated(m_ShadowMargin + rowsLeft(), m_ShadowMargin), m_Scale);
}

int OverlayMenuPanel::viewportHeight() const
{
    const int available =
        qMax(0, qRound(height() / m_Scale) - 2 * m_ShadowMargin - contentTop() - m_Padding -
                    ((m_HasGamepads || m_Editing) ? kGamepadHintBarHeight : 0));
    return available >= m_ItemHeight ? available / m_ItemHeight * m_ItemHeight : available;
}

QRect OverlayMenuPanel::rowViewportGeometry() const
{
    return scaledRect(QRect(m_ShadowMargin + rowsLeft(), m_ShadowMargin + contentTop(), rowsWidth(),
                            viewportHeight()),
                      m_Scale);
}

bool OverlayMenuPanel::rowEnabled(int row) const
{
    const auto& items = m_MenuLevels[m_CurrentLevel].items;
    return row >= 0 && row < int(items.size()) && focusable(items[row]);
}

bool OverlayMenuPanel::rowToggleState(int row) const
{
    const auto& items = m_MenuLevels[m_CurrentLevel].items;
    return row >= 0 && row < int(items.size()) && items[row].toggleState;
}

void OverlayMenuPanel::ensureRowVisible(int row)
{
    const int count = int(m_MenuLevels[m_CurrentLevel].items.size());
    if (row < 0 || row >= count)
        return;
    const int top = rowTop(row);
    const int bottom = top + rowHeight(m_MenuLevels[m_CurrentLevel].items[row]);
    if (top < m_ScrollOffset)
        m_ScrollOffset = top;
    if (bottom > m_ScrollOffset + viewportHeight())
        m_ScrollOffset = bottom - viewportHeight();
    m_ScrollOffset = qBound(0, m_ScrollOffset,
                            qMax(0, rowsHeight(m_MenuLevels[m_CurrentLevel]) - viewportHeight()));
    forceRepaint();
}

QImage OverlayMenuPanel::renderImage()
{
    QImage image(size() * devicePixelRatio(), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(devicePixelRatio());
    image.fill(Qt::transparent);
    QPainter painter(&image);
    paintOverlay(painter);
    return image;
}

QRect OverlayMenuPanel::tabGeometry(MenuTab tab) const
{
    for (int i = 0; i < static_cast<int>(m_TabLevels.size()); i++) {
        if (*m_MenuLevels[m_TabLevels[i]].tab == tab) {
            return scaledRect(tabRect(i, m_MenuWidth).translated(m_ShadowMargin, m_ShadowMargin),
                              m_Scale);
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
    m_BackStack.clear();
    m_CaptureShortcutSlot = -1;
    m_ScrollOffset = 0;
    m_HoveredIndex = -1;
    m_SliderHotZone = SliderZone::None;
    repositionWindow();
    if (level == m_SessionStatsLevel)
        m_SessionStatsTimer.start();
    else
        m_SessionStatsTimer.stop();

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
    if (m_CaptureShortcutSlot >= 0) {
        const int scancode = ShortcutKeys::fromQtKeyEvent(key, 0);
        if (ShortcutKeys::isModifierKey(scancode) && key != Qt::Key_AltGr)
            return true;
        const bool handled = handleShortcutKey(scancode, modifiers, true);
        // This API is down-only. SDL callers use handleShortcutKey directly
        // with both halves; Qt prefixes arrive in the final key's modifiers.
        handleShortcutKey(scancode, modifiers, false);
        return handled;
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
        goBack();
        return true;
    case Qt::Key_Escape:
        gamepadBack();
        return true;
    default:
        return false;
    }
}

bool OverlayMenuPanel::handleShortcutKey(int scancode, Qt::KeyboardModifiers modifiers,
                                         bool pressed)
{
    if (!isCapturingShortcut())
        return false;
    if (pressed)
        m_ShortcutCapturedKeys.insert(scancode);
    else
        m_ShortcutCapturedKeys.remove(scancode);
    if (m_CaptureShortcutSlot < 0)
        return true; // Drain captured key releases.
    if (scancode == ShortcutKeys::Escape) {
        if (pressed) {
            m_CaptureShortcutSlot = -1;
            m_ShortcutModifierRecorder.reset();
            rebuildShortcutEditor();
            forceRepaint();
        }
        return true;
    }
    ShortcutBinding binding;
    const quint8 mods = ShortcutBinding::fromQtModifiers(int(modifiers));
    if (pressed) {
        m_ShortcutModifierRecorder.press(scancode, mods);
        if (ShortcutKeys::isModifierKey(scancode))
            return true;
        binding = ShortcutBinding(mods, scancode);
    } else {
        binding = m_ShortcutModifierRecorder.release(scancode);
        if (binding.isNull())
            return true;
    }
    const ShortcutAction* action = ShortcutRegistry::instance().find(m_EditingShortcut);
    const int slot = m_CaptureShortcutSlot;
    m_CaptureShortcutSlot = -1;
    if (!action || !action->accepts(binding)) {
        m_ShortcutError = tr("Invalid binding · use Ctrl, Alt or Win with a key");
        rebuildShortcutEditor();
        repositionWindow();
    } else {
        ShortcutProfile candidate = m_ShortcutProfile;
        if (!candidate.setBinding(m_EditingShortcut, slot, binding)) {
            m_ShortcutError = tr("This binding cannot be edited");
            rebuildShortcutEditor();
        } else
            saveShortcutCandidate(candidate, slot);
    }
    if (!m_ShortcutError.isEmpty())
        ensureRowVisible(int(m_MenuLevels[m_CurrentLevel].items.size()) - 1);
    forceRepaint();
    return true;
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
    level.openAction = MenuAction::OpenStreamQuality;

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
        const QString note = preset.current ? tr("Current")
                             : preset.display
                                 ? tr("Display", "FPS preset at the display refresh rate")
                                 : QString();
        level.items.push_back({ tr("%1 FPS").arg(preset.fps),
                                detailText(preset.fps == m_PendingStreamFps, note),
                                MenuItemType::Action, MenuAction::SelectStreamFps, 0, true, false,
                                i == frameRates.size() - 1, QString::number(preset.fps) });
    }

    // Bitrate changes apply to the running stream, so they reuse the Bitrate level.
    level.items.push_back({ tr("Bitrate"), formatBitrateKbps(m_BitrateKbps), MenuItemType::SubMenu,
                            MenuAction::OpenBitrate, 0, true, false, false });
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
            // The rebuilt level's Bitrate row must point at its level again
            m_MenuLevels[m_StreamQualityLevel] = buildStreamQualityLevel();
            linkSubMenuRows(m_MenuLevels[m_StreamQualityLevel]);
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
    const MenuAction action = item.action;
    if (action == MenuAction::SetMouseModeGame || action == MenuAction::SetMouseModeDesktop ||
        action == MenuAction::MatchResolutionNow) {
        if (m_ActionCallback)
            m_ActionCallback(action);
        return;
    }
    if (action == MenuAction::QuitAndExit && !m_QuitConfirmTimer.isActive()) {
        m_QuitConfirmTimer.start();
        applyOwnedState();
        forceRepaint();
        return;
    }
    if (action == MenuAction::UngrabInput) {
        // Notify before closing: Session must clear its restore flag in this
        // same call stack, even if an animation is interrupted.
        if (m_ActionCallback)
            m_ActionCallback(action);
        closeMenu();
        return;
    }
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
    if (item.action == MenuAction::SendHostKeys || item.action == MenuAction::RunHostCommand ||
        item.action == MenuAction::SetMenuScale || item.action == MenuAction::SetStatsOpacity) {
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
                                       std::optional<QPoint> pointerGlobalPosition)
{
    m_AnchorMode = AnchorMode::RightEdge;
    m_ParentX = parentX;
    m_ParentY = parentY;
    m_ParentW = parentW;
    m_ParentH = parentH;
    m_TriggerPosition = pointerGlobalPosition;
    showInternal();
}

void OverlayMenuPanel::showAtLeftEdge(int parentX, int parentY, int parentW, int parentH,
                                      std::optional<QPoint> pointerGlobalPosition)
{
    m_AnchorMode = AnchorMode::LeftEdge;
    m_ParentX = parentX;
    m_ParentY = parentY;
    m_ParentW = parentW;
    m_ParentH = parentH;
    m_TriggerPosition = pointerGlobalPosition;
    showInternal();
}

void OverlayMenuPanel::showAtTopEdge(int parentX, int parentY, int parentW, int parentH,
                                     std::optional<QPoint> pointerGlobalPosition)
{
    m_AnchorMode = AnchorMode::TopEdge;
    m_ParentX = parentX;
    m_ParentY = parentY;
    m_ParentW = parentW;
    m_ParentH = parentH;
    m_TriggerPosition = pointerGlobalPosition;
    showInternal();
}

void OverlayMenuPanel::showAtCursor(int parentX, int parentY, int parentW, int parentH,
                                    const QPoint& cursorPosition)
{
    m_AnchorMode = AnchorMode::AtCursor;
    m_ParentX = parentX;
    m_ParentY = parentY;
    m_ParentW = parentW;
    m_ParentH = parentH;
    m_TriggerPosition = cursorPosition;
    showInternal();
}

void OverlayMenuPanel::showWithPlacement(OverlayMenu::Placement placement, int parentX, int parentY,
                                         int parentW, int parentH,
                                         std::optional<QPoint> pointerPosition,
                                         std::optional<QPointF> draggedPosition)
{
    const QRect bounds(parentX, parentY, parentW, parentH);
    if (m_Detached && m_DetachedPosition) {
        placement = OverlayMenu::Placement::Dragged;
        draggedPosition = OverlayMenu::normalizedPosition(*m_DetachedPosition, bounds);
        pointerPosition.reset();
    }
    m_AnchorMode = AnchorMode::Placed;
    m_Placement = placement;
    m_DraggedPosition = draggedPosition;
    m_ParentX = parentX;
    m_ParentY = parentY;
    m_ParentW = parentW;
    m_ParentH = parentH;
    m_TriggerPosition = pointerPosition;
    showInternal();
}

void OverlayMenuPanel::showInternal()
{
    refreshShortcuts();
    m_ShortcutRefreshTimer.start();
    m_UserInteracted = false;
    m_TitlePressed = false;
    m_TitleDragging = false;
    // Reopen on the last tab of this session
    m_CurrentLevel = tabRootLevel();
    m_BackStack.clear();
    m_ScrollOffset = 0;
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
    if (m_CurrentLevel == m_SessionStatsLevel)
        m_SessionStatsTimer.start();

    // Calculate target geometry
    repositionWindow();
    m_TargetPosition = position();

    // Slide direction depends on anchor mode
    const QPoint slide = slideVector();
    const bool verticalSlide = slide.y() != 0;
    const int slideDirection = verticalSlide ? slide.y() : slide.x();
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
    m_OpacityAnim->setEndValue(m_PanelOpacity);
    m_OpacityAnim->setEasingCurve(QEasingCurve::OutQuad);

    m_SlideAnim->start();
    m_OpacityAnim->start();

    forceRepaint();
}

void OverlayMenuPanel::repositionWindow()
{
    int qpX = m_ParentX;
    int qpY = m_ParentY;
    int qpW = m_ParentW;
    int qpH = m_ParentH;

    // The original anchors stay inside the stream. The placement options also
    // stay on the screen the stream is on, for a window spanning monitors.
    QRect bounds(qpX, qpY, qpW, qpH);
    if (m_AnchorMode == AnchorMode::Placed &&
        !OverlayWindowPolicy::usesParentCoordinates(m_WindowMode)) {
        if (QScreen* screen = QGuiApplication::screenAt(bounds.center())) {
            const QRect onScreen = bounds.intersected(screen->geometry());
            if (!onScreen.isEmpty()) {
                bounds = onScreen;
            }
        }
    }
    m_Bounds = bounds;

    // Widen to fit every text, as far as the stream allows
    m_Scale = m_RequestedScale;
    const int maxWidth = qMax(1, int(bounds.width() / m_Scale) - 2 * m_ShadowMargin);
    m_MenuWidth = qMin(requiredMenuWidth(), maxWidth);
    int menuHeight = this->menuHeight();
    menuHeight = qMin(menuHeight, qMax(1, int(bounds.height() / m_Scale) - 2 * m_ShadowMargin));

    // On-screen sizes; everything else here is in unscaled layout units
    int scaledWidth = qRound(m_MenuWidth * m_Scale);
    int scaledHeight = qRound(menuHeight * m_Scale);
    const int scaledShadow = qRound(m_ShadowMargin * m_Scale);

    const QPoint triggerPosition = m_TriggerPosition.value_or(QPoint());

    int cx, cy; // content top-left position

    switch (m_AnchorMode) {
    case AnchorMode::Placed: {
        const QSize size =
            OverlayMenu::placedSize(m_Placement, bounds, QSize(scaledWidth, scaledHeight));
        scaledHeight = size.height();
        const QPoint topLeft = OverlayMenu::placeMenu(m_Placement, bounds, size, m_TriggerPosition,
                                                      m_DraggedPosition, qRound(12 * m_Scale));
        cx = topLeft.x();
        cy = topLeft.y();
        break;
    }

    case AnchorMode::LeftEdge:
        cx = qpX;
        if (m_TriggerPosition.has_value()) {
            cy = triggerPosition.y() - qRound((contentTop() + m_ItemHeight / 2) * m_Scale);
        }
        else {
            cy = qpY + (qpH - scaledHeight) / 2;
        }
        break;

    case AnchorMode::TopEdge:
        if (m_TriggerPosition.has_value()) {
            cx = triggerPosition.x() - scaledWidth / 2;
        }
        else {
            cx = qpX + (qpW - scaledWidth) / 2;
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
        cx = qpX + qpW - scaledWidth;
        if (m_TriggerPosition.has_value()) {
            cy = triggerPosition.y() - qRound((contentTop() + m_ItemHeight / 2) * m_Scale);
        }
        else {
            cy = qpY + (qpH - scaledHeight) / 2;
        }
        break;
    }

    // Clamp within the parent. If the menu is larger than the streaming
    // window, keep its origin visible instead of pushing it past an edge.
    const QPoint clamped =
        OverlayMenu::clampToBounds(QPoint(cx, cy), QSize(scaledWidth, scaledHeight), bounds);
    cx = clamped.x();
    cy = clamped.y();

    // Window includes shadow margin around content
    setGeometry(cx - scaledShadow, cy - scaledShadow, scaledWidth + 2 * scaledShadow,
                scaledHeight + 2 * scaledShadow);
    m_ScrollOffset = qBound(0, m_ScrollOffset,
                            qMax(0, rowsHeight(m_MenuLevels[m_CurrentLevel]) - viewportHeight()));
    if (m_HoveredIndex >= 0)
        ensureRowVisible(m_HoveredIndex);
}

void OverlayMenuPanel::navigateToLevel(int level)
{
    if (level < 0 || level >= (int)m_MenuLevels.size()) return;

    // Navigation commits a peeked menu as an interaction even if the pointer
    // later moves outside it.
    beginInteraction();
    // Into a sub-level slides; back to a tab root is instant
    bool goingForward = !isTabRootLevel(level);
    if (goingForward && level != m_CurrentLevel) {
        const auto& items = m_MenuLevels[m_CurrentLevel].items;
        const bool focused = m_HoveredIndex >= 0 && m_HoveredIndex < int(items.size());
        m_BackStack.push_back({ m_MenuLevels[m_CurrentLevel].openAction,
                                focused ? items[m_HoveredIndex].action : MenuAction::MenuActionMax,
                                focused ? items[m_HoveredIndex].payload : QString(),
                                m_HoveredIndex });
    }
    m_ContentSlideAnim->stop();
    m_ContentOffset = 0;

    m_CurrentLevel = level;
    m_ScrollOffset = 0;
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
    m_UserInteracted = true;
}

void OverlayMenuPanel::dismissOnOutsideClick(const QPoint& parentPosition)
{
    if (m_PointerEditMode) {
        return;
    }
    if (m_OpeningClickPosition) {
        const bool openingClick = parentPosition == *m_OpeningClickPosition;
        m_OpeningClickPosition.reset();
        if (openingClick) {
            return;
        }
    }

    if (m_Visible && !geometry().contains(parentPosition)) {
        closeMenu();
    }
}

void OverlayMenuPanel::ignoreOpeningClickAt(const QPoint& parentPosition)
{
    m_OpeningClickPosition = parentPosition;
}

void OverlayMenuPanel::closeMenu()
{
    m_ShortcutRefreshTimer.stop();
    m_SessionStatsTimer.stop();
    m_QuitConfirmTimer.stop();
    m_CaptureShortcutSlot = -1;
    m_ShortcutCapturedKeys.clear();
    applyOwnedState();
    if (!m_Visible) return;
    if (m_Closing) return;  // already animating close

    if (m_TitlePressed) {
        m_TitlePressed = false;
        m_TitleDragging = false;
        setMouseGrabEnabled(false);
    }

    m_Visible = false;
    m_Closing = true;
    m_OpeningClickPosition.reset();
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
    const QPoint slide = slideVector();
    const bool verticalSlide = slide.y() != 0;
    const int slideDirection = verticalSlide ? slide.y() : slide.x();
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
    if (m_Layout == OverlayMenu::Layout::SidePanel) {
        // A vertical rail under the title bar
        return QRect(1, m_TitleHeight + m_Padding + visibleIndex * railTabHeight(), m_RailWidth - 2,
                     railTabHeight());
    }

    // Equal-width tabs across the bar; the last one takes the remainder
    const int count = qMax(1, static_cast<int>(m_TabLevels.size()));
    const int innerWidth = contentWidth - 2;
    const int tabWidth = innerWidth / count;
    const int x = 1 + visibleIndex * tabWidth;
    const int width = visibleIndex == count - 1 ? innerWidth - visibleIndex * tabWidth : tabWidth;
    return QRect(x, m_TitleHeight, width, m_TabBarHeight - 1);
}

int OverlayMenuPanel::railTabHeight() const
{
    const int boundsHeight = m_Bounds.isEmpty() ? m_ParentH : m_Bounds.height();
    if (m_Layout != OverlayMenu::Layout::SidePanel || boundsHeight <= 0)
        return m_ItemHeight;
    const int available =
        int(boundsHeight / m_RequestedScale) - 2 * m_ShadowMargin - m_TitleHeight - 2 * m_Padding;
    return qMin(m_ItemHeight, qMax(22, available / qMax(1, int(m_TabLevels.size()))));
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
        if (lx >= m_MenuWidth - 2 * m_TitleHeight)
            return -4;
        if (!isTabRootLevel(m_CurrentLevel)) {
            return -2; // back button
        }
        return -1;
    }
    if (m_Layout == OverlayMenu::Layout::SidePanel ? lx < m_RailWidth
                                                   : ly < titleH + m_TabBarHeight) {
        for (int i = 0; i < static_cast<int>(m_TabLevels.size()); i++) {
            if (tabRect(i, m_MenuWidth).contains(lx, ly)) {
                return kTabHitBase - i;
            }
        }
        return -1;
    }
    if (lx < rowsLeft())
        return -1;
    int localY = ly - contentTop();
    if (localY < 0 || localY >= viewportHeight())
        return -1;
    const auto& items = m_MenuLevels[m_CurrentLevel].items;
    int top = 0;
    for (int idx = 0; idx < int(items.size()); ++idx) {
        top += rowHeight(items[idx]);
        if (localY + m_ScrollOffset < top)
            return idx;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void OverlayMenuPanel::paintOverlay(QPainter& p)
{
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setRenderHint(QPainter::TextAntialiasing);

    // Paint in layout units; the window is m_Scale times as large
    p.scale(m_Scale, m_Scale);
    int w = qRound(width() / m_Scale);
    int h = qRound(height() / m_Scale);
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
    p.setPen(QPen(m_Editing ? MenuAccent : MenuLine, 1, m_Editing ? Qt::DashLine : Qt::SolidLine));
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
    QRect titleRect(textPad + backWidth, 0, cw - textPad - backWidth - 2 * m_TitleHeight, titleH);
    p.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, level.title.toUpper());
    drawIcon(QStringLiteral("menu-close"),
             QRect(closeRect.center().x() - 9, closeRect.center().y() - 9, 18, 18));
    const QRect gearRect(cw - 2 * m_TitleHeight, 0, m_TitleHeight, m_TitleHeight);
    if (m_HoveredIndex == -4) {
        p.fillRect(gearRect.adjusted(1, 1, -1, -1), MenuHover);
        p.setPen(QPen(MenuAccent, 1));
        p.drawRect(gearRect.adjusted(1, 1, -2, -2));
    }
    drawIcon(QStringLiteral("tb-settings"),
             QRect(gearRect.center().x() - 10, gearRect.center().y() - 10, 20, 20));

    // --- Tab bar: the active tab (or the one a sub-level belongs to) is
    // underlined in the accent. It stays put while the content slides. ---
    const int activeTabLevel = tabRootLevel();
    const bool rail = m_Layout == OverlayMenu::Layout::SidePanel;
    if (rail) {
        // Side panel: the tabs are a column left of the rows
        p.fillRect(QRect(m_RailWidth - 1, titleH, 1, ch - titleH - 1), MenuLine);
    } else {
        p.fillRect(QRect(1, titleH + m_TabBarHeight - 1, cw - 2, 1), MenuLine);
    }
    p.setFont(m_TabFont);
    for (int i = 0; rail && i < static_cast<int>(m_TabLevels.size()); i++) {
        const QRect tab = tabRect(i, cw);
        const MenuTab menuTab = *m_MenuLevels[m_TabLevels[i]].tab;
        const bool active = m_TabLevels[i] == activeTabLevel;
        const bool hovered = m_HoveredIndex == kTabHitBase - i;
        if (active || hovered) {
            p.fillRect(tab, MenuHover);
        }
        if (active) {
            p.fillRect(QRect(tab.x(), tab.y() + 6, 3, tab.height() - 12), MenuAccent);
        }
        p.setPen(active || hovered ? MenuText : MenuDim);
        p.drawText(tab.adjusted(14, 0, -14, 0), Qt::AlignLeft | Qt::AlignVCenter,
                   tabTitle(menuTab).toUpper());
        if (m_RemoteUsbNeedsActivation &&
            menuTab == tabForAction(MenuAction::OpenRemoteUsbDevices)) {
            p.fillRect(QRect(tab.right() - 8, tab.center().y() - 2, 4, 4), MenuAccent);
        }
    }
    for (int i = 0; !rail && i < static_cast<int>(m_TabLevels.size()); i++) {
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
    // The rows (and the gamepad hints) sit right of the tab rail
    p.translate(rowsLeft(), 0);
    const int rw = cw - rowsLeft();

    const auto& items = level.items;
    const int itemsTop = contentTop();

    // Sub-levels have no fixed index, so entries match by action
    auto itemIcon = [](const MenuItem& item) -> QString {
        switch (item.action) {
        case MenuAction::OpenBitrate:
            return QStringLiteral("menu-bitrate");
        case MenuAction::OpenMenuPosition:
            return QStringLiteral("menu-position");
        case MenuAction::OpenMenuLayout:
            return QStringLiteral("tb-settings");
        case MenuAction::OpenRemoteUsbDevices:
            return QStringLiteral("cat-peripherals");
        case MenuAction::OpenStreamQuality:
        case MenuAction::OpenWindowMode:
        case MenuAction::OpenStatsOverlay:
        case MenuAction::ToggleFullScreen:
            return QStringLiteral("cat-display");
        case MenuAction::OpenSessionStats:
            return QStringLiteral("menu-bitrate");
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

    p.save();
    p.setClipRect(QRect(0, contentTop(), rw, viewportHeight()), Qt::IntersectClip);
    for (int i = 0; i < (int)items.size(); i++) {
        int itemY = itemsTop + rowTop(i) - m_ScrollOffset;
        const auto& item = items[i];
        const int itemHeight = rowHeight(item);
        if (itemY + itemHeight <= contentTop() || itemY >= contentTop() + viewportHeight())
            continue;
        if (item.type == MenuItemType::Header) {
            p.setFont(m_DetailFont);
            p.setPen(MenuAccent);
            p.drawText(QRect(textPad, itemY + 8, rw - 2 * textPad, m_ItemHeight - 8),
                       Qt::AlignLeft | Qt::AlignVCenter, item.label.toUpper());
            continue;
        }
        if (item.type == MenuItemType::Info) {
            p.setFont(m_LabelFont);
            p.setPen(MenuDim);
            p.drawText(
                QRect(textPad, itemY, rw - 2 * textPad, m_ItemHeight / 2),
                Qt::AlignLeft | Qt::AlignBottom,
                QFontMetrics(m_LabelFont).elidedText(item.label, Qt::ElideRight, rw - 2 * textPad));
            p.setFont(m_DetailFont);
            p.drawText(QRect(textPad, itemY + m_ItemHeight / 2, rw - 2 * textPad, m_ItemHeight / 2),
                       Qt::AlignLeft | Qt::AlignTop,
                       QFontMetrics(m_DetailFont)
                           .elidedText(item.detail, Qt::ElideRight, rw - 2 * textPad));
            continue;
        }

        // Square focus/hover outline and the shared 4px accent marker.
        if (i == m_HoveredIndex && item.enabled) {
            const QRect row(4, itemY + 1, rw - 8, itemHeight - 2);
            p.fillRect(row, MenuHover);
            p.setPen(QPen(MenuAccent, 1));
            p.drawRect(row.adjusted(0, 0, -1, -1));
            p.fillRect(QRect(4, itemY + 1, 4, itemHeight - 2), MenuAccent);
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
        const QString detail = displayDetail(item);
        if (item.type == MenuItemType::SubMenu) {
            p.setFont(m_LabelFont);
            p.setPen(activationPrompt ? MenuAccent : item.enabled ? MenuText : MenuFaint);
            // Detail text (e.g., "20 Mbps") right before the arrow, the
            // label in the rest of the row
            const int arrowSpace = textPad + 20;
            const int detailWidth =
                detail.isEmpty()
                    ? 0
                    : qMin(rw / 2, QFontMetrics(m_DetailFont).horizontalAdvance(detail));
            QRect lr(labelX, itemY,
                     rw - labelX - arrowSpace - (detailWidth > 0 ? detailWidth + 12 : 0),
                     m_ItemHeight);
            p.drawText(
                lr, Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(m_LabelFont).elidedText(item.label, Qt::ElideRight, lr.width()));

            if (!detail.isEmpty()) {
                p.setFont(m_DetailFont);
                p.setPen(activationPrompt ? MenuAccent : MenuDim);
                QRect dr(rw - arrowSpace - detailWidth, itemY, detailWidth, m_ItemHeight);
                p.drawText(
                    dr, Qt::AlignRight | Qt::AlignVCenter,
                    QFontMetrics(m_DetailFont).elidedText(detail, Qt::ElideRight, dr.width()));
            }

            drawIcon(QStringLiteral("menu-next"),
                     QRect(rw - textPad - 16, itemY + (m_ItemHeight - 16) / 2, 16, 16), 0.85);
        }
        // --- Toggle item ---
        else if (item.type == MenuItemType::Toggle) {
            p.setFont(m_LabelFont);
            p.setPen(item.enabled ? MenuText : MenuFaint);
            QRect lr(labelX, itemY, rw - labelX - 68,
                     detail.isEmpty() ? m_ItemHeight : m_ItemHeight * 3 / 5);
            p.drawText(
                lr, Qt::AlignLeft | (detail.isEmpty() ? Qt::AlignVCenter : Qt::AlignBottom),
                QFontMetrics(m_LabelFont).elidedText(item.label, Qt::ElideRight, lr.width()));
            if (!detail.isEmpty()) {
                p.setFont(m_DetailFont);
                p.setPen(MenuDim);
                const QRect dr(labelX, itemY + lr.height(), lr.width(), m_ItemHeight - lr.height());
                p.drawText(
                    dr, Qt::AlignLeft | Qt::AlignTop,
                    QFontMetrics(m_DetailFont).elidedText(detail, Qt::ElideRight, dr.width()));
            }

            // Square track and square thumb, matching HardSwitch.qml.
            const int trackW = 40, trackH = 20;
            const int trackX = rw - textPad - trackW;
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
            const SliderRowRects r = sliderRowRects(rw, itemY);
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

            bool hasLongDetail = !detail.isEmpty() && detail.length() > 3;
            bool hasShortDetail = !detail.isEmpty() && detail.length() <= 3;

            if (item.action == MenuAction::UngrabInput &&
                releaseDetailLines(item, rw - labelX - textPad).size() > 1) {
                const int labelHeight = QFontMetrics(m_LabelFont).height();
                const int detailHeight = QFontMetrics(m_DetailFont).height();
                p.drawText(QRect(labelX, itemY + 4, rw - labelX - textPad, labelHeight),
                           Qt::AlignLeft | Qt::AlignVCenter, item.label);
                p.setFont(m_DetailFont);
                p.setPen(MenuDim);
                const QStringList lines = releaseDetailLines(item, rw - labelX - textPad);
                for (int line = 0; line < lines.size(); ++line)
                    p.drawText(QRect(labelX, itemY + 4 + labelHeight + line * detailHeight,
                                     rw - labelX - textPad, detailHeight),
                               Qt::AlignLeft | Qt::AlignVCenter, lines[line]);
            } else if (hasLongDetail) {
                int topH = qRound(m_ItemHeight * 0.58);
                QRect lb(labelX, itemY, rw - labelX - textPad, topH);
                p.drawText(lb, Qt::AlignLeft | Qt::AlignBottom, item.label);

                p.setFont(m_DetailFont);
                p.setPen(activationPrompt ? MenuAccent : MenuDim);
                QRect sr(labelX, itemY + topH, rw - labelX - textPad, m_ItemHeight - topH);
                p.drawText(
                    sr, Qt::AlignLeft | Qt::AlignTop,
                    QFontMetrics(m_DetailFont).elidedText(detail, Qt::ElideRight, sr.width()));
            } else {
                int detailWidth = 0;
                if (hasShortDetail) {
                    const QFontMetrics detailMetrics(m_DetailFont);
                    detailWidth = qMax(20, detailMetrics.horizontalAdvance(detail) + 8);
                }
                const int detailGap = hasShortDetail ? 8 : 0;
                QRect lr(labelX, itemY, rw - labelX - textPad - detailWidth - detailGap,
                         m_ItemHeight);
                p.drawText(lr, Qt::AlignLeft | Qt::AlignVCenter, item.label);

                if (hasShortDetail) {
                    // Short status text or checkmark in the shared brand accent.
                    p.setFont(m_DetailFont);
                    p.setPen(MenuAccent);
                    QRect cr(rw - textPad - detailWidth, itemY, detailWidth, m_ItemHeight);
                    p.drawText(cr, Qt::AlignRight | Qt::AlignVCenter, detail);
                }
            }
        }
        // --- Back item (fallback, normally handled by title bar) ---
        else if (item.type == MenuItemType::Back) {
            p.setFont(m_DetailFont);
            p.setPen(MenuDim);
            QRect lr(labelX, itemY, rw - labelX - textPad, m_ItemHeight);
            p.drawText(lr, Qt::AlignLeft | Qt::AlignVCenter, item.label);
        }

        // Group separator — only where explicitly flagged
        if (item.separatorAfter && i < (int)items.size() - 1) {
            p.setPen(QPen(MenuLine, 1));
            int sepY = itemY + itemHeight - 1;
            p.drawLine(labelX, sepY, rw - textPad, sepY);
        }
    }

    p.restore(); // Row viewport.
    const int totalHeight = rowsHeight(level);
    if (totalHeight > viewportHeight() && viewportHeight() > 0) {
        const int thumbHeight = qMax(12, viewportHeight() * viewportHeight() / totalHeight);
        const int thumbTop = contentTop() + (viewportHeight() - thumbHeight) * m_ScrollOffset /
                                                (totalHeight - viewportHeight());
        p.fillRect(QRect(rw - 3, thumbTop, 2, thumbHeight), MenuAccent);
    }

    // Gamepad hint bar: pad-only players get the menu bindings on screen
    // instead of having to know them. Faces follow the controller style
    // (PS: ✕ Select / ○ Back; Nintendo: B Select / A Back), compensated for
    // swapFaceButtons so the hint names the physical button to press.
    if (m_HasGamepads || m_Editing) {
        const int hintTop = ch - m_Padding - kGamepadHintBarHeight;
        p.setPen(QPen(MenuLine, 1));
        p.drawLine(textPad, hintTop, rw - textPad, hintTop);
        p.setFont(m_DetailFont);
        p.setPen(MenuDim);
        const QString hint =
            m_Editing ? tr("Wheel: size  Ctrl+wheel: opacity  Esc: done") : gamepadHintText();
        p.drawText(QRect(textPad, hintTop + 1, rw - textPad * 2, kGamepadHintBarHeight - 1),
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
    if (m_EditResizeEdges) {
        resizeForEditing(parentPosition(event));
        return;
    }
    if (m_PointerEditMode && !m_TitlePressed) {
        const Qt::Edges edges = editEdgesAt(parentPosition(event));
        const bool horizontal = edges.testFlag(Qt::LeftEdge) || edges.testFlag(Qt::RightEdge);
        const bool vertical = edges.testFlag(Qt::TopEdge) || edges.testFlag(Qt::BottomEdge);
        setCursor(horizontal && vertical
                      ? (edges.testFlag(Qt::LeftEdge) == edges.testFlag(Qt::TopEdge)
                             ? Qt::SizeFDiagCursor
                             : Qt::SizeBDiagCursor)
                  : horizontal ? Qt::SizeHorCursor
                  : vertical   ? Qt::SizeVerCursor
                               : Qt::SizeAllCursor);
        return;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPoint pos = layoutPosition(event->position());
#else
    const QPoint pos = layoutPosition(event->pos());
#endif

    if (m_TitlePressed) {
        // The title bar drags the panel once the pointer really moves
        const QPoint delta = parentPosition(event) - m_TitlePressPosition;
        if (!m_TitleDragging && delta.manhattanLength() > 4) {
            m_TitleDragging = true;
            beginInteraction();
        }
        if (m_TitleDragging) {
            const int shadow = qRound(m_ShadowMargin * m_Scale);
            const QSize content(width() - 2 * shadow, height() - 2 * shadow);
            const QRect bounds =
                m_Bounds.isValid() ? m_Bounds : QRect(m_ParentX, m_ParentY, m_ParentW, m_ParentH);
            const QPoint contentTopLeft = OverlayMenu::clampToBounds(
                m_TitlePressWindowPos + delta + QPoint(shadow, shadow), content, bounds);
            setPosition(contentTopLeft - QPoint(shadow, shadow));
        }
        return;
    }

    if (m_SliderDragging) {
        // Keep scrubbing even when the pointer leaves the panel (mouse grab).
        const auto& items = m_MenuLevels[m_CurrentLevel].items;
        for (int i = 0; i < (int)items.size(); i++) {
            if (items[i].type != MenuItemType::Slider) continue;
            const int itemY = contentTop() + rowTop(i) - m_ScrollOffset;
            const SliderRowRects r = sliderRowRects(rowsWidth(), itemY);
            const double frac =
                (pos.x() - m_ShadowMargin - rowsLeft() - r.track.x()) / double(r.track.width());
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
    if (m_PointerEditMode) {
        beginInteraction();
        if (!m_Editing) {
            finishTitleDrag();
            m_Editing = true;
            repositionWindow();
            forceRepaint();
        }
        m_TitlePressPosition = parentPosition(event);
        m_EditPressGeometry = contentGeometry();
        m_EditPressScale = scalePercent();
        m_TitlePressWindowPos = position();
        m_EditResizeEdges = editEdgesAt(m_TitlePressPosition);
        m_TitlePressed = !m_EditResizeEdges;
        m_TitleDragging = false;
        m_TitlePressIndex = -1;
        setMouseGrabEnabled(true);
        return;
    }
    beginInteraction();

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPoint pos = layoutPosition(event->position());
#else
    const QPoint pos = layoutPosition(event->pos());
#endif
    int idx = itemAtPos(pos);

    if (idx == -3) {
        closeMenu();
        return;
    }
    if (idx == -4) {
        m_HoveredIndex = -4;
        navigateToLevel(levelForOpenAction(MenuAction::OpenMenuSettings));
        return;
    }

    // The title bar (and the back button in it) drags the panel. A press
    // that doesn't move is a click: back, on release.
    const int titleY = pos.y() - m_ShadowMargin;
    if (idx == -2 || (idx == -1 && titleY >= 0 && titleY < m_TitleHeight)) {
        m_TitlePressed = true;
        m_TitleDragging = false;
        m_TitlePressIndex = idx;
        m_TitlePressPosition = parentPosition(event);
        m_TitlePressWindowPos = position();
        setMouseGrabEnabled(true);
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
    if (idx >= (int)items.size() || !focusable(items[idx]))
        return;

    const MenuItem item = items[idx];
    m_HoveredIndex = idx;
    if (item.type != MenuItemType::Slider) {
        activateItem(item);
        return;
    }

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
            const int itemY = contentTop() + rowTop(idx) - m_ScrollOffset;
            const SliderRowRects r = sliderRowRects(rowsWidth(), itemY);
            m_SliderDragging = true;
            setMouseGrabEnabled(true);
            const double frac =
                (pos.x() - m_ShadowMargin - rowsLeft() - r.track.x()) / double(r.track.width());
            setBitrateFromFraction(frac);
        }
        forceRepaint();
        break;
    }

    case MenuItemType::Toggle:
    {
        activateItem(item);
        break;
    }
    case MenuItemType::Header:
    case MenuItemType::Info:
        break;
    }
}

void OverlayMenuPanel::finishTitleDrag()
{
    const int shadow = qRound(m_ShadowMargin * m_Scale);
    const QRect bounds =
        m_Bounds.isValid() ? m_Bounds : QRect(m_ParentX, m_ParentY, m_ParentW, m_ParentH);
    const QPointF normalized =
        OverlayMenu::normalizedPosition(position() + QPoint(shadow, shadow), bounds);

    // Reopen (and lay out again) where it was left
    m_AnchorMode = AnchorMode::Placed;
    m_Placement = OverlayMenu::Placement::Dragged;
    m_DraggedPosition = normalized;
    m_TargetPosition = position();
    if (m_Detached) {
        m_DetachedPosition = position() + QPoint(shadow, shadow);
    }
    if (m_DragFinishedCallback) {
        m_DragFinishedCallback(normalized);
    }
}

void OverlayMenuPanel::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_EditResizeEdges && event->button() == Qt::LeftButton) {
        m_EditResizeEdges = {};
        setMouseGrabEnabled(false);
        saveEditedStyle();
        return;
    }
    if (m_TitlePressed && event->button() == Qt::LeftButton) {
        const bool dragged = m_TitleDragging;
        const int pressIndex = m_TitlePressIndex;
        m_TitlePressed = false;
        m_TitleDragging = false;
        setMouseGrabEnabled(false);
        if (dragged) {
            finishTitleDrag();
        } else if (pressIndex == -2) {
            goBack();
        }
        return;
    }

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
    if (m_Editing) {
        const int steps = event->angleDelta().y() / 120;
        const bool opacity = event->modifiers().testFlag(Qt::ControlModifier);
        setStyle(m_Layout, scalePercent() + (opacity ? 0 : steps * 5),
                 opacityPercent() + (opacity ? steps * 5 : 0));
        saveEditedStyle();
        event->accept();
        return;
    }

    // QWheelEvent::pos() is unavailable on the SteamLink Qt 5.14 build;
    // position() exists everywhere from 5.14 on.
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    const QPoint pos = layoutPosition(event->position());
#else
    const QPoint pos = layoutPosition(event->pos());
#endif
    const int idx = itemAtPos(pos);
    if (idx < 0 || idx >= (int)m_MenuLevels[m_CurrentLevel].items.size()
            || m_MenuLevels[m_CurrentLevel].items[idx].type != MenuItemType::Slider) {
        // Don't carry a partial notch into a later scrub session.
        m_WheelAccum = 0;
        const int delta = !event->pixelDelta().isNull()
                              ? event->pixelDelta().y()
                              : event->angleDelta().y() * m_ItemHeight / 120;
        m_ScrollOffset =
            qBound(0, m_ScrollOffset - delta,
                   qMax(0, rowsHeight(m_MenuLevels[m_CurrentLevel]) - viewportHeight()));
        beginInteraction();
        forceRepaint();
        event->accept();
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
    moveFocus(-1);
}

void OverlayMenuPanel::gamepadMoveDown()
{
    moveFocus(1);
}

void OverlayMenuPanel::moveFocus(int direction)
{
    if (!m_Visible) return;
    beginInteraction();
    std::vector<int> stops;
    const auto& items = m_MenuLevels[m_CurrentLevel].items;
    for (int i = 0; i < int(items.size()); ++i)
        if (focusable(items[i]))
            stops.push_back(i);
    stops.push_back(-4); // Gear exists even on an all-info level.
    if (!isTabRootLevel(m_CurrentLevel))
        stops.push_back(-2);
    const auto it = std::find(stops.begin(), stops.end(), m_HoveredIndex);
    const int count = int(stops.size());
    const int index = it == stops.end() ? (direction > 0 ? -1 : 0) : int(it - stops.begin());
    m_HoveredIndex = stops[(index + direction + count) % count];
    ensureRowVisible(m_HoveredIndex);
    forceRepaint();
}

void OverlayMenuPanel::gamepadSelect()
{
    if (!m_Visible) return;

    // Title bar → back
    if (m_HoveredIndex == -2) {
        goBack();
        return;
    }
    if (m_HoveredIndex == -4) {
        navigateToLevel(levelForOpenAction(MenuAction::OpenMenuSettings));
        return;
    }

    if (m_HoveredIndex < 0) return;

    const auto& items = m_MenuLevels[m_CurrentLevel].items;
    if (m_HoveredIndex >= (int)items.size() || !items[m_HoveredIndex].enabled) return;

    activateItem(items[m_HoveredIndex]);
}

void OverlayMenuPanel::gamepadBack()
{
    if (!m_Visible) return;

    if (m_EscapeAction == OverlayMenu::EscapeAction::Close) {
        closeMenu();
    } else {
        goBack();
    }
}

void OverlayMenuPanel::goBack()
{
    if (!m_Visible)
        return;

    if (!isTabRootLevel(m_CurrentLevel)) {
        m_CaptureShortcutSlot = -1;
        if (!m_BackStack.empty()) {
            const auto back = m_BackStack.back();
            m_BackStack.pop_back();
            const int parentLevel = levelForOpenAction(back.openAction);
            // A return is not another forward navigation.
            m_CurrentLevel = parentLevel >= 0 ? parentLevel : tabRootLevel();
            m_ContentSlideAnim->stop();
            m_ContentOffset = 0;
            m_ScrollOffset = 0;
            m_HoveredIndex =
                back.focusCode >= 0 ? rowOfAction(back.focusAction, back.payload) : back.focusCode;
            if (m_CurrentLevel == m_SessionStatsLevel)
                m_SessionStatsTimer.start();
            else
                m_SessionStatsTimer.stop();
            repositionWindow();
            ensureRowVisible(m_HoveredIndex);
            forceRepaint();
        } else
            navigateToLevel(tabRootLevel());
    } else {
        closeMenu();
    }
}
