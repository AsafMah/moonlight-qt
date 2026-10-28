#pragma once

#include "overlaystatspreset.h"
#include "overlaystatswindow.h"
#include "overlaywindowpolicy.h"
#include "settings/statsoverlaysettings.h"

#include <QElapsedTimer>
#include <QRect>

#include <functional>
#include <optional>

class OverlayStatsTooltip;
class QScreen;
class QWindow;
class StreamStatsModel;

namespace Overlay {
class OverlayManager;
}

/**
 * OverlayStatsController - owns the performance overlay during a stream.
 *
 * Where the OS compositor can show an extra window, the overlay is an
 * OverlayStatsWindow with presets, edit mode and per-display placement.
 * Elsewhere (Steam Link, KMSDRM/EGLFS without a window system) it falls back
 * to the SDL_ttf text overlay drawn by the renderers, and presets only switch
 * it on and off.
 *
 * Session drives it from the SDL loop: tick() repaints when due,
 * nextEventDelayMs() bounds the loop's wait, and needsEventProcessing() asks
 * for Qt event pumping while a window is being exposed or dragged.
 *
 * While the stream does not have the mouse (edit mode, the pointer released,
 * or a hold-to-release key), hovering a metric shows a tooltip about it. The
 * overlay stays click-through outside edit/persistent-release selection mode.
 * Ordinary tooltips poll PointerProvider rather than taking mouse events.
 *
 * While detached, the overlay is a top-level window that the user can drag
 * anywhere, including another screen. It is not anchored to the stream; its
 * position is remembered per stream display, and its size per display and
 * preset is shared with the attached overlay.
 */
class OverlayStatsController
{
public:
    using ParentGeometryProvider = std::function<QRect()>;
    using ExtrasProvider = std::function<OverlayStatsWindow::Extras()>;
    using EditModeCallback = std::function<void(bool editing)>;
    // The pointer in the parent rect's coordinate space, or nothing while the
    // stream has captured it
    using PointerProvider = std::function<std::optional<QPoint>()>;

    // Margin from the stream edges, in logical pixels
    static constexpr int EdgeMargin = 16;

    OverlayStatsController(Overlay::OverlayManager& overlayManager, StreamStatsModel& model,
                           QWindow* parent, OverlayWindowMode windowMode, bool initiallyVisible);
    ~OverlayStatsController();

    OverlayStatsController(const OverlayStatsController&) = delete;
    OverlayStatsController& operator=(const OverlayStatsController&) = delete;

    // False where an extra compositor window cannot be shown
    static bool compositorWindowSupported();
    bool usesCompositorWindow() const { return m_Window != nullptr; }
    bool isDetached() const { return m_Detached && m_Window != nullptr; }
    int opacityPercent() const { return m_Settings.opacityPercent(); }

    void setParentGeometryProvider(ParentGeometryProvider provider);
    void setExtrasProvider(ExtrasProvider provider) { m_ExtrasProvider = std::move(provider); }
    void setEditModeCallback(EditModeCallback callback)
    {
        m_EditModeCallback = std::move(callback);
    }
    void setPointerProvider(PointerProvider provider) { m_PointerProvider = std::move(provider); }

    // Off while hidden
    StatsOverlay::Preset preset() const;
    // Shows the given preset (and remembers it for this display), or hides
    void setPreset(StatsOverlay::Preset preset);
    // toggle-stats-overlay: Off <-> the last preset
    void toggle();
    // cycle-overlay-preset: Off -> Minimal -> Compact -> Detailed -> Custom
    void cyclePreset();
    void setDetached(bool detached);
    void setOpacityPercent(int percent);

    // edit-overlay-position: drag to move, drag the edges to resize (panel
    // presets), wheel to zoom, Esc to finish
    bool isEditing() const { return m_Editing; }
    void beginEditing();
    void endEditing();
    void toggleEditing();
    void setPointerEditMode(bool enabled);

    // Session calls these from the SDL loop
    void syncWithStreamWindow(bool streamVisible);
    void tick();
    int nextEventDelayMs() const;
    bool needsEventProcessing() const;

private:
    bool windowShouldShow() const;
    void applyVisibility();
    void refreshNow();
    void scheduleRefresh() { m_RefreshDue = true; }
    int refreshIntervalMs() const;
    void updateDisplay();
    QScreen* streamScreen() const;
    QString currentDisplayKey() const;
    double currentDisplayHz() const;
    void reposition();
    void repositionDetached(const QRect& parentRect);
    void createTooltip();
    void onDetachedMoved(const QPoint& topLeft);
    void saveDetachedPosition();
    void applyStyle();
    void onDragFinished(const QPoint& center);
    void onResizeFinished(const QPoint& center, const QSize& bodySize);
    void onWheel(int steps, bool adjustOpacity);
    bool pointerIsFree() const;
    void updateTooltip();
    void hideTooltip();

    Overlay::OverlayManager& m_OverlayManager;
    StreamStatsModel& m_Model;
    QWindow* m_Parent;
    OverlayWindowMode m_WindowMode;
    OverlayStatsWindow* m_Window = nullptr;
    OverlayStatsTooltip* m_Tooltip = nullptr;
    StatsOverlaySettings m_Settings;

    ParentGeometryProvider m_ParentGeometryProvider;
    ExtrasProvider m_ExtrasProvider;
    EditModeCallback m_EditModeCallback;
    PointerProvider m_PointerProvider;

    bool m_Detached = false;
    // Wayland windows cannot place themselves or learn where they are
    bool m_DetachedPositionKnown = true;
    bool m_DetachedPlaced = false;
    QPoint m_DetachedPos; // where the window is, in global coordinates
    bool m_DetachedSaveDue = false;
    qint64 m_DetachedMovedAtMs = 0;

    bool m_Visible;
    bool m_StreamVisible = true;
    bool m_Editing = false;
    StatsOverlay::Preset m_Preset;
    StatsOverlay::Anchor m_Anchor = StatsOverlay::Anchor::TopLeft;
    std::optional<QPoint> m_AttachedPosition;
    QVector<StatsOverlay::Metric> m_Metrics;
    QString m_DisplayKey;
    QSize m_WindowSize; // only grows between style changes, see refreshNow()

    QElapsedTimer m_Clock;
    qint64 m_NextRefreshMs = 0;
    bool m_RefreshDue = true;

    // Hover tooltip: the metric under the pointer, and since when
    std::optional<StatsOverlay::Metric> m_HoverMetric;
    qint64 m_HoverSinceMs = 0;
    qint64 m_NextHoverCheckMs = 0;
};
