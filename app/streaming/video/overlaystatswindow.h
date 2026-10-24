#pragma once

#include "overlayrasterwindow.h"
#include "overlaystatspreset.h"
#include "streaming/stats/statssnapshot.h"

#include <QFont>
#include <QVector>

#include <functional>

class QMouseEvent;
class QWheelEvent;
class QKeyEvent;

/**
 * OverlayStatsWindow - the performance overlay, drawn from a StatsSnapshot
 * into its own window so the OS compositor blends it the same way on every
 * renderer (like OverlayMenuPanel and OverlayToast).
 *
 * The window is click-through except in edit mode, where it can be dragged
 * (snapping to nine anchors, decided by OverlayStatsController) and resized
 * with the mouse wheel.
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
    };

    using DragFinishedCallback = std::function<void(const QPoint& center)>;
    using WheelCallback = std::function<void(int steps, bool adjustOpacity)>;
    using EndEditingCallback = std::function<void()>;

    explicit OverlayStatsWindow(QWindow* parent = nullptr,
                                OverlayWindowMode windowMode = OverlayWindowMode::Detached);

    void setStats(const StatsSnapshot& snapshot, const Extras& extras);
    void setPreset(StatsOverlay::Preset preset);
    void setMetrics(const QVector<StatsOverlay::Metric>& metrics);
    void setScalePercent(int percent);
    void setOpacityPercent(int percent);

    // Where the body sits when the window is larger than it (the controller
    // only grows the window while numbers change width, to avoid jitter)
    void setBodyAlignment(Qt::Alignment alignment) { m_BodyAlignment = alignment; }

    // Edit mode accepts mouse input; otherwise the window is click-through
    void setEditing(bool editing);
    bool isEditing() const { return m_Editing; }
    bool isDragging() const { return m_Dragging; }

    // Logical window size for the current content, preset and scale
    QSize preferredSize() const;

    // Paints and flushes immediately. requestUpdate() would wait for a Qt
    // event pump, which the SDL loop skips while the overlay is idle.
    void repaintNow();

    // Called with the window's center, in the same coordinate space as the
    // parent rect (global for detached windows, parent-local on Wayland)
    void setDragFinishedCallback(DragFinishedCallback cb)
    {
        m_DragFinishedCallback = std::move(cb);
    }
    void setWheelCallback(WheelCallback cb) { m_WheelCallback = std::move(cb); }
    void setEndEditingCallback(EndEditingCallback cb) { m_EndEditingCallback = std::move(cb); }

protected:
    void paintOverlay(QPainter& painter) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    struct Cell
    {
        QString label;
        QString value;
        StatsOverlay::Grade grade = StatsOverlay::Grade::Neutral;
    };

    Cell cellFor(StatsOverlay::Metric metric) const;
    QVector<Cell> cellsFor(const QVector<StatsOverlay::Metric>& metrics) const;
    QVector<StatsOverlay::Metric> panelMetrics() const;
    bool panelHas(StatsOverlay::Metric metric) const;

    // Each layout function measures when painter is null, and returns the
    // unscaled size of the body (without the hard shadow)
    QSize layoutMinimal(QPainter* painter) const;
    QSize layoutCompact(QPainter* painter) const;
    QSize layoutPanel(QPainter* painter) const;
    QSize layoutBody(QPainter* painter) const;

    int paintPanelHeader(QPainter* painter, int y, int width) const;
    int paintFrameTimeGraph(QPainter* painter, int y, int width) const;
    int paintLatencyBar(QPainter* painter, int y, int width) const;
    int paintGrid(QPainter* painter, int y, int width, const QVector<Cell>& cells) const;
    int paintEditHint(QPainter* painter, int y, int width) const;

    qreal scale() const { return m_ScalePercent / 100.0; }
    QPoint eventPosition(QMouseEvent* event) const;

    StatsSnapshot m_Snapshot;
    Extras m_Extras;
    StatsOverlay::Preset m_Preset = StatsOverlay::Preset::Compact;
    QVector<StatsOverlay::Metric> m_Metrics;
    int m_ScalePercent = StatsOverlay::DefaultScalePercent;
    int m_OpacityPercent = StatsOverlay::DefaultOpacityPercent;

    Qt::Alignment m_BodyAlignment = Qt::AlignLeft | Qt::AlignTop;

    bool m_Editing = false;
    bool m_Dragging = false;
    QPoint m_PressPosition;  // pointer, in parent coordinates
    QPoint m_PressWindowPos; // window position at press

    QFont m_LabelFont;
    QFont m_ValueFont;
    QFont m_BigFont;

    OverlayWindowMode m_WindowMode;
    DragFinishedCallback m_DragFinishedCallback;
    WheelCallback m_WheelCallback;
    EndEditingCallback m_EndEditingCallback;
};
