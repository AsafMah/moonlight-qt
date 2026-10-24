#pragma once

#include "overlayrasterwindow.h"
#include "overlaystatspreset.h"

#include <QFont>

/**
 * OverlayStatsTooltip - the hover tooltip of the performance overlay: a
 * metric's full name, what it means, and its current value.
 *
 * It is painted with QPainter in the overlay's style rather than being a
 * QToolTip, because the overlay is a raster window shown over the stream, and
 * it never takes input. OverlayStatsController shows and places it.
 */
class OverlayStatsTooltip : public OverlayRasterWindow
{
    Q_OBJECT

public:
    explicit OverlayStatsTooltip(QWindow* parent, OverlayWindowMode windowMode);

    void setContent(StatsOverlay::Metric metric, const QString& value);
    StatsOverlay::Metric metric() const { return m_Metric; }
    void setScalePercent(int percent);

    // Logical window size for the current content and scale
    QSize preferredSize() const;

    // See OverlayStatsWindow::repaintNow()
    void repaintNow();

protected:
    void paintOverlay(QPainter& painter) override;

private:
    // Measures when painter is null; returns the unscaled body size
    QSize layout(QPainter* painter) const;
    qreal scale() const { return m_ScalePercent / 100.0; }

    StatsOverlay::Metric m_Metric = StatsOverlay::Metric::Fps;
    QString m_Title;
    QString m_Description;
    QString m_Value;
    int m_ScalePercent = StatsOverlay::DefaultScalePercent;

    QFont m_TitleFont;
    QFont m_BodyFont;
    QFont m_ValueFont;
};
