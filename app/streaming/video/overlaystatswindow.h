#pragma once

#include "overlayrasterwindow.h"
#include "overlaystatslayout.h"
#include "overlaystatspreset.h"
#include "streaming/stats/statssnapshot.h"

#include <QFont>
#include <QVector>

#include <functional>
#include <optional>

class QMouseEvent;
class QWheelEvent;
class QKeyEvent;
class QMoveEvent;

/**
 * OverlayStatsWindow - the performance overlay, drawn from a StatsSnapshot
 * into its own window so the OS compositor blends it the same way on every
 * renderer (like OverlayMenuPanel and OverlayToast).
 *
 * The window is click-through except in edit mode, where it can be dragged
 * (snapping to nine anchors, decided by OverlayStatsController), zoomed with
 * the mouse wheel and, for the panel presets, resized by its edges and corners.
 *
 * Layout and painting only depend on the window's own size, never on the
 * stream window. A standalone (detached) overlay is an ordinary top-level
 * window that always takes the mouse; see setStandalone().
 */
class OverlayStatsWindow : public OverlayRasterWindow
{
    Q_OBJECT

public:
    // Fork-only values that do not come from the stats model
    struct Extras
    {
        bool abrEnabled = false;   // Sunshine adaptive bitrate is running
        int targetBitrateKbps = 0; // what the host was asked to send
        double displayHz = 0;      // refresh rate of the stream's display
        int vrrCapFps = -1;        // VRR mode's present cap; 0 without one, -1 when VRR is off
    };

    // What a hover tooltip shows for a metric
    struct TooltipInfo
    {
        StatsOverlay::Metric metric = StatsOverlay::Metric::Fps;
        QString value; // the current value, with units
    };

    using DragFinishedCallback = std::function<void(const QPoint& center)>;
    // bodySize is unscaled, see setBodySize()
    using ResizeFinishedCallback = std::function<void(const QPoint& center, const QSize& bodySize)>;
    using WheelCallback = std::function<void(int steps, bool adjustOpacity)>;
    using EndEditingCallback = std::function<void()>;
    using MovedCallback = std::function<void(const QPoint& topLeft)>;

    explicit OverlayStatsWindow(QWindow* parent = nullptr,
                                OverlayWindowMode windowMode = OverlayWindowMode::Detached);

    void setStats(const StatsSnapshot& snapshot, const Extras& extras);
    void setPreset(StatsOverlay::Preset preset);
    void setMetrics(const QVector<StatsOverlay::Metric>& metrics);
    void setScalePercent(int percent);
    void setOpacityPercent(int percent);

    // Size of a panel preset's body at 100% scale, or an invalid size for the
    // default (default width, as tall as the content). Ignored by the other
    // presets, which always fit their content.
    void setBodySize(const QSize& size) { m_BodySize = size; }
    void setMaximumBodySize(const QSize& size) { m_MaximumBodySize = size; }
    bool isResizable() const { return StatsOverlay::presetIsResizable(m_Preset); }

    // Where the body sits when the window is larger than it (the controller
    // only grows the window while numbers change width, to avoid jitter)
    void setBodyAlignment(Qt::Alignment alignment) { m_BodyAlignment = alignment; }

    // A top-level window of its own, apart from the stream: never
    // click-through, so it can be dragged (to another screen too), resized by
    // its edges and zoomed with the wheel at any time. Call before showing.
    void setStandalone(bool standalone);
    void configureWindow(QWindow* parent, OverlayWindowMode windowMode, bool standalone);
    bool isStandalone() const { return m_Standalone; }

    // The pointer, in window coordinates, while it is over a standalone window
    std::optional<QPoint> hoverPosition() const { return m_HoverPosition; }

    // Edit mode accepts mouse input; otherwise the window is click-through
    // (unless it is standalone)
    void setEditing(bool editing);
    bool isEditing() const { return m_Editing; }
    // Moving or resizing with the mouse
    bool isDragging() const { return m_Dragging || m_ResizeEdges != 0; }

    // Logical window size for the current content, preset and scale
    QSize preferredSize() const;

    // The metric under a point in window coordinates, as last painted
    std::optional<TooltipInfo> tooltipAt(const QPoint& windowPoint) const;

    // Paints and flushes immediately. requestUpdate() would wait for a Qt
    // event pump, which the SDL loop skips while the overlay is idle.
    void repaintNow();

    // Called with the window's center, in the same coordinate space as the
    // parent rect (global for detached windows, parent-local on Wayland)
    void setDragFinishedCallback(DragFinishedCallback cb)
    {
        m_DragFinishedCallback = std::move(cb);
    }
    void setResizeFinishedCallback(ResizeFinishedCallback cb)
    {
        m_ResizeFinishedCallback = std::move(cb);
    }
    void setWheelCallback(WheelCallback cb) { m_WheelCallback = std::move(cb); }
    void setEndEditingCallback(EndEditingCallback cb) { m_EndEditingCallback = std::move(cb); }
    // Called whenever the window moves, however it was moved
    void setMovedCallback(MovedCallback cb) { m_MovedCallback = std::move(cb); }

protected:
    void paintOverlay(QPainter& painter) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    bool event(QEvent* event) override;

private:
    struct Cell
    {
        StatsOverlay::Metric metric = StatsOverlay::Metric::Fps;
        QString label;
        QString shortLabel;
        QString value;
        StatsOverlay::Grade grade = StatsOverlay::Grade::Neutral;
    };

    struct HitRegion
    {
        StatsOverlay::Metric metric;
        QRect rect; // body coordinates
        QString value;
    };

    enum Edge
    {
        EdgeLeft = 1,
        EdgeTop = 2,
        EdgeRight = 4,
        EdgeBottom = 8,
    };

    Cell cellFor(StatsOverlay::Metric metric) const;
    QVector<Cell> cellsFor(const QVector<StatsOverlay::Metric>& metrics) const;
    QVector<StatsOverlay::Metric> panelMetrics() const;
    bool panelHas(StatsOverlay::Metric metric) const;

    // Each layout function measures when painter is null, and returns the
    // unscaled size of the body (without the hard shadow)
    QSize layoutMinimal(QPainter* painter) const;
    QSize layoutCompact(QPainter* painter) const;
    QSize layoutBody(QPainter* painter) const;

    StatsOverlay::PanelContent panelContent(QVector<Cell>& cells) const;
    QSize panelBodySize() const;
    void paintPanel(QPainter* painter, const StatsOverlay::PanelLayout& layout,
                    const StatsOverlay::PanelContent& content, const QVector<Cell>& cells) const;
    void paintFrameTimeGraph(QPainter* painter, const QRect& area) const;
    void paintLatencyBar(QPainter* painter, const StatsOverlay::PanelLayout& layout,
                         const StatsOverlay::PanelContent& content) const;

    QString editHintText() const;
    int editHintHeight(int width) const;
    void paintEditHint(QPainter* painter, int y, int width) const;
    void paintResizeGrip(QPainter* painter, const QSize& body) const;

    // Takes mouse input: edit mode, or always when standalone
    bool isInteractive() const { return m_Editing || m_Standalone; }
    qreal scale() const { return m_ScalePercent / 100.0; }
    QPoint eventPosition(QMouseEvent* event) const;
    QPoint localPosition(QMouseEvent* event) const;
    int edgesAt(const QPoint& local) const;
    void updateEdgeCursor(int edges);
    void resizeTo(const QPoint& pointer);

    StatsSnapshot m_Snapshot;
    Extras m_Extras;
    StatsOverlay::Preset m_Preset = StatsOverlay::Preset::Compact;
    QVector<StatsOverlay::Metric> m_Metrics;
    int m_ScalePercent = StatsOverlay::DefaultScalePercent;
    int m_OpacityPercent = StatsOverlay::DefaultOpacityPercent;
    QSize m_BodySize;
    QSize m_MaximumBodySize;

    Qt::Alignment m_BodyAlignment = Qt::AlignLeft | Qt::AlignTop;

    bool m_Editing = false;
    bool m_Standalone = false;
    std::optional<QPoint> m_HoverPosition;
    bool m_Dragging = false;
    int m_ResizeEdges = 0;   // edges being dragged, 0 when not resizing
    QPoint m_PressPosition;  // pointer, in parent coordinates
    QPoint m_PressWindowPos; // window position at press
    QRect m_PressGeometry;   // window geometry at press
    QSize m_PressBodySize;   // unscaled body size at press

    // Filled by each paint, for tooltipAt()
    mutable QVector<HitRegion> m_HitRegions;
    mutable QPointF m_BodyOffset;

    QFont m_LabelFont;
    QFont m_ValueFont;
    QFont m_BigFont;

    OverlayWindowMode m_WindowMode;
    DragFinishedCallback m_DragFinishedCallback;
    ResizeFinishedCallback m_ResizeFinishedCallback;
    WheelCallback m_WheelCallback;
    EndEditingCallback m_EndEditingCallback;
    MovedCallback m_MovedCallback;
};
