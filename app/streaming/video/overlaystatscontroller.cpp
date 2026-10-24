#include "overlaystatscontroller.h"
#include "overlaymanager.h"
#include "streaming/stats/streamstatsmodel.h"

#include <Limelight.h>
#include "SDL_compat.h"

#include <QGuiApplication>
#include <QScreen>
#include <QWindow>

#include <algorithm>

using namespace StatsOverlay;

namespace {

Qt::Alignment alignmentFor(Anchor anchor)
{
    const int index = static_cast<int>(anchor);
    Qt::Alignment alignment;
    switch (index % 3) {
    case 0:
        alignment |= Qt::AlignLeft;
        break;
    case 1:
        alignment |= Qt::AlignHCenter;
        break;
    default:
        alignment |= Qt::AlignRight;
        break;
    }
    switch (index / 3) {
    case 0:
        alignment |= Qt::AlignTop;
        break;
    case 1:
        alignment |= Qt::AlignVCenter;
        break;
    default:
        alignment |= Qt::AlignBottom;
        break;
    }
    return alignment;
}

} // namespace

OverlayStatsController::OverlayStatsController(Overlay::OverlayManager& overlayManager,
                                               StreamStatsModel& model, QWindow* parent,
                                               OverlayWindowMode windowMode, bool initiallyVisible)
    : m_OverlayManager(overlayManager), m_Model(model), m_Parent(parent), m_WindowMode(windowMode),
      m_Visible(initiallyVisible), m_Preset(m_Settings.presetValue())
{
    m_Clock.start();

    if (compositorWindowSupported()) {
        m_Window = new OverlayStatsWindow(parent, windowMode);
        m_Window->setDragFinishedCallback([this](const QPoint& center) { onDragFinished(center); });
        m_Window->setWheelCallback(
            [this](int steps, bool adjustOpacity) { onWheel(steps, adjustOpacity); });
        m_Window->setEndEditingCallback([this] { endEditing(); });
        applyStyle();
    }

    applyVisibility();
}

OverlayStatsController::~OverlayStatsController()
{
    if (m_Window != nullptr) {
        m_Window->hide();
        delete m_Window;
        m_Window = nullptr;
    }
}

bool OverlayStatsController::compositorWindowSupported()
{
#ifdef STEAM_LINK
    return false;
#else
    // Platforms without a window system draw everything into one surface
    const QString platform = QGuiApplication::platformName();
    if (platform == QLatin1String("eglfs") || platform == QLatin1String("linuxfb") ||
        platform == QLatin1String("vkkhrdisplay") || platform == QLatin1String("minimal") ||
        platform == QLatin1String("offscreen")) {
        return false;
    }

    const char* driver = SDL_GetCurrentVideoDriver();
    if (driver != nullptr && SDL_strcasecmp(driver, "KMSDRM") == 0) {
        return false;
    }

    return true;
#endif
}

void OverlayStatsController::setParentGeometryProvider(ParentGeometryProvider provider)
{
    m_ParentGeometryProvider = std::move(provider);
    updateDisplay();
    applyVisibility();
}

Preset OverlayStatsController::preset() const
{
    return m_Visible ? m_Preset : Preset::Off;
}

void OverlayStatsController::setPreset(Preset preset)
{
    if (preset == Preset::Off) {
        endEditing();
        m_Visible = false;
    } else {
        m_Visible = true;
        m_Preset = preset;
        if (!m_DisplayKey.isEmpty()) {
            m_Settings.setDisplayPreset(m_DisplayKey, preset);
        }
        applyStyle();
    }

    applyVisibility();
}

void OverlayStatsController::toggle()
{
    setPreset(m_Visible ? Preset::Off : m_Preset);
}

void OverlayStatsController::cyclePreset()
{
    setPreset(nextPreset(preset()));
}

void OverlayStatsController::beginEditing()
{
    if (m_Window == nullptr || m_Editing) {
        return;
    }

    if (!m_Visible) {
        m_Visible = true;
    }

    m_Editing = true;
    m_Window->setEditing(true);
    applyVisibility();

    if (m_EditModeCallback) {
        m_EditModeCallback(true);
    }
}

void OverlayStatsController::endEditing()
{
    if (!m_Editing) {
        return;
    }

    m_Editing = false;
    if (m_Window != nullptr) {
        m_Window->setEditing(false);
    }
    // The edit hint goes away, so the window may shrink
    m_WindowSize = QSize();
    applyVisibility();

    if (m_EditModeCallback) {
        m_EditModeCallback(false);
    }
}

void OverlayStatsController::toggleEditing()
{
    if (m_Editing) {
        endEditing();
    } else {
        beginEditing();
    }
}

void OverlayStatsController::syncWithStreamWindow(bool streamVisible)
{
    m_StreamVisible = streamVisible;
    if (!streamVisible) {
        endEditing();
    }

    updateDisplay();
    applyVisibility();
}

void OverlayStatsController::tick()
{
    if (m_Window == nullptr || !m_Window->isVisible() || m_Window->isDragging()) {
        return;
    }

    if (m_RefreshDue || m_Clock.elapsed() >= m_NextRefreshMs) {
        refreshNow();
    }
}

int OverlayStatsController::nextEventDelayMs() const
{
    if (m_Window == nullptr || !m_Window->isVisible()) {
        return -1;
    }
    if (m_RefreshDue) {
        return 0;
    }
    return static_cast<int>(std::max<qint64>(m_NextRefreshMs - m_Clock.elapsed(), 0));
}

bool OverlayStatsController::needsEventProcessing() const
{
    if (m_Window == nullptr || !m_Window->isVisible()) {
        return false;
    }

    // Dragging needs every mouse event; a fresh window needs its expose event
    // before repaintNow() can draw into it
    return m_Editing || !m_Window->isExposed();
}

bool OverlayStatsController::windowShouldShow() const
{
    return m_Visible && m_StreamVisible;
}

void OverlayStatsController::applyVisibility()
{
    if (m_Window == nullptr) {
        // SDL_ttf fallback: the renderers draw the decoder's stats text
        m_OverlayManager.setOverlayState(Overlay::OverlayDebug, m_Visible);
        return;
    }

    // Never show both overlays at once
    m_OverlayManager.setOverlayState(Overlay::OverlayDebug, false);

    if (!windowShouldShow() || !m_ParentGeometryProvider) {
        if (m_Window->isVisible()) {
            m_Window->hide();
        }
        return;
    }

    const bool wasVisible = m_Window->isVisible();
    refreshNow();
    if (!wasVisible) {
        m_Window->show();
        m_Window->raise();
        // Painted once Qt has exposed the window, see needsEventProcessing()
        scheduleRefresh();
    }
}

int OverlayStatsController::refreshIntervalMs() const
{
    // The frame time graph scrolls, everything else changes about once a second
    const bool hasGraph =
        m_Preset == Preset::Detailed ||
        (m_Preset == Preset::Custom && m_Metrics.contains(Metric::FrameTimeGraph));
    return hasGraph ? 250 : 500;
}

void OverlayStatsController::refreshNow()
{
    if (m_Window == nullptr) {
        return;
    }

    OverlayStatsWindow::Extras extras;
    if (m_ExtrasProvider) {
        extras = m_ExtrasProvider();
    }
    if (extras.displayHz <= 0) {
        extras.displayHz = currentDisplayHz();
    }
    m_Window->setStats(m_Model.snapshot(LiGetMicroseconds()), extras);

    reposition();
    m_Window->repaintNow();

    m_RefreshDue = !m_Window->isExposed();
    m_NextRefreshMs = m_Clock.elapsed() + refreshIntervalMs();
}

void OverlayStatsController::updateDisplay()
{
    const QString key = currentDisplayKey();
    if (key == m_DisplayKey) {
        return;
    }

    m_DisplayKey = key;
    const StatsOverlaySettings::DisplayState state = m_Settings.displayState(key);
    m_Anchor = state.hasAnchor ? state.anchor : Anchor::TopLeft;
    m_Preset = state.hasPreset ? state.preset : m_Settings.presetValue();
    applyStyle();
}

QString OverlayStatsController::currentDisplayKey() const
{
    QScreen* screen = nullptr;
    if (OverlayWindowPolicy::usesParentCoordinates(m_WindowMode) && m_Parent != nullptr) {
        screen = m_Parent->screen();
    } else if (m_ParentGeometryProvider) {
        const QRect parentRect = m_ParentGeometryProvider();
        if (parentRect.isValid()) {
            screen = QGuiApplication::screenAt(parentRect.center());
        }
    }
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    return displayKey(screen != nullptr ? screen->name() : QString());
}

double OverlayStatsController::currentDisplayHz() const
{
    if (m_Window != nullptr && m_Window->screen() != nullptr) {
        return m_Window->screen()->refreshRate();
    }
    return 0;
}

void OverlayStatsController::reposition()
{
    if (m_Window == nullptr || !m_ParentGeometryProvider) {
        return;
    }

    const QRect parentRect = m_ParentGeometryProvider();
    if (!parentRect.isValid()) {
        return;
    }

    // Numbers change width every refresh. Growing only keeps the window from
    // twitching; applyStyle() resets it when the layout really changes.
    m_WindowSize = m_WindowSize.expandedTo(m_Window->preferredSize());

    // Detached windows must follow the stream's monitor explicitly; a native
    // Wayland child inherits its parent's screen
    if (!OverlayWindowPolicy::usesParentCoordinates(m_WindowMode)) {
        QScreen* targetScreen = QGuiApplication::screenAt(parentRect.center());
        if (targetScreen != nullptr && m_Window->screen() != targetScreen) {
            m_Window->setScreen(targetScreen);
        }
    }

    const QRect target(anchoredPosition(m_Anchor, parentRect, m_WindowSize, EdgeMargin),
                       m_WindowSize);
    if (m_Window->geometry() != target) {
        m_Window->setGeometry(target);
    }
}

void OverlayStatsController::applyStyle()
{
    if (m_Window == nullptr) {
        return;
    }

    m_Metrics = m_Settings.metricList();
    m_Window->setPreset(m_Preset);
    m_Window->setMetrics(m_Metrics);
    m_Window->setScalePercent(m_Settings.scalePercent());
    m_Window->setOpacityPercent(m_Settings.opacityPercent());
    m_Window->setBodyAlignment(alignmentFor(m_Anchor));
    m_WindowSize = QSize();
    scheduleRefresh();
}

void OverlayStatsController::onDragFinished(const QPoint& center)
{
    if (!m_ParentGeometryProvider) {
        return;
    }

    m_Anchor = nearestAnchor(m_ParentGeometryProvider(), center);
    if (!m_DisplayKey.isEmpty()) {
        m_Settings.setDisplayAnchor(m_DisplayKey, m_Anchor);
    }
    applyStyle();
    refreshNow();
}

void OverlayStatsController::onWheel(int steps, bool adjustOpacity)
{
    if (adjustOpacity) {
        m_Settings.setOpacityPercent(m_Settings.opacityPercent() + steps * 5);
    } else {
        m_Settings.setScalePercent(m_Settings.scalePercent() + steps * 5);
    }
    applyStyle();
    refreshNow();
}
