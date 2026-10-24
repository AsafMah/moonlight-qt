#pragma once

#include "overlaystatspreset.h"
#include "overlaystatswindow.h"
#include "overlaywindowpolicy.h"
#include "settings/statsoverlaysettings.h"

#include <QElapsedTimer>
#include <QRect>

#include <functional>

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
 */
class OverlayStatsController
{
public:
    using ParentGeometryProvider = std::function<QRect()>;
    using ExtrasProvider = std::function<OverlayStatsWindow::Extras()>;
    using EditModeCallback = std::function<void(bool editing)>;

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

    void setParentGeometryProvider(ParentGeometryProvider provider);
    void setExtrasProvider(ExtrasProvider provider) { m_ExtrasProvider = std::move(provider); }
    void setEditModeCallback(EditModeCallback callback)
    {
        m_EditModeCallback = std::move(callback);
    }

    // Off while hidden
    StatsOverlay::Preset preset() const;
    // Shows the given preset (and remembers it for this display), or hides
    void setPreset(StatsOverlay::Preset preset);
    // toggle-stats-overlay: Off <-> the last preset
    void toggle();
    // cycle-overlay-preset: Off -> Minimal -> Compact -> Detailed -> Custom
    void cyclePreset();

    // edit-overlay-position: drag to move, wheel to resize, Esc to finish
    bool isEditing() const { return m_Editing; }
    void beginEditing();
    void endEditing();
    void toggleEditing();

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
    QString currentDisplayKey() const;
    double currentDisplayHz() const;
    void reposition();
    void applyStyle();
    void onDragFinished(const QPoint& center);
    void onWheel(int steps, bool adjustOpacity);

    Overlay::OverlayManager& m_OverlayManager;
    StreamStatsModel& m_Model;
    QWindow* m_Parent;
    OverlayWindowMode m_WindowMode;
    OverlayStatsWindow* m_Window = nullptr;
    StatsOverlaySettings m_Settings;

    ParentGeometryProvider m_ParentGeometryProvider;
    ExtrasProvider m_ExtrasProvider;
    EditModeCallback m_EditModeCallback;

    bool m_Visible;
    bool m_StreamVisible = true;
    bool m_Editing = false;
    StatsOverlay::Preset m_Preset;
    StatsOverlay::Anchor m_Anchor = StatsOverlay::Anchor::TopLeft;
    QVector<StatsOverlay::Metric> m_Metrics;
    QString m_DisplayKey;
    QSize m_WindowSize; // only grows between style changes, see refreshNow()

    QElapsedTimer m_Clock;
    qint64 m_NextRefreshMs = 0;
    bool m_RefreshDue = true;
};
