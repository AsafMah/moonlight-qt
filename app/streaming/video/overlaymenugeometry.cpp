#include "overlaymenugeometry.h"

#include <algorithm>

namespace OverlayMenu {

namespace {

const char* const kPlacementIds[] = {
    "auto",    "center",   "cursor",     "top",         "bottom",    "left",       "right",
    "topleft", "topright", "bottomleft", "bottomright", "leftpanel", "rightpanel", "dragged",
};
static_assert(sizeof(kPlacementIds) / sizeof(kPlacementIds[0]) == int(Placement::Count),
              "every placement needs an id");

const char* const kLayoutIds[] = { "compact", "comfortable", "sidepanel" };
static_assert(sizeof(kLayoutIds) / sizeof(kLayoutIds[0]) == int(Layout::Count),
              "every layout needs an id");

const char* const kEdgeZoneIds[] = { "top",     "bottom",   "left",       "right",
                                     "topleft", "topright", "bottomleft", "bottomright" };
static_assert(sizeof(kEdgeZoneIds) / sizeof(kEdgeZoneIds[0]) == int(EdgeZone::Count),
              "every edge zone needs an id");

template <typename Enum, int N>
Enum fromId(const char* const (&ids)[N], const QString& id, Enum fallback)
{
    for (int i = 0; i < N; i++) {
        if (id.compare(QLatin1String(ids[i]), Qt::CaseInsensitive) == 0) {
            return static_cast<Enum>(i);
        }
    }
    return fallback;
}

// Where along one axis: start, middle or end of the free space
enum class Align
{
    Start,
    Middle,
    End,
};

int alignedCoordinate(Align align, int start, int length, int size, int margin)
{
    switch (align) {
    case Align::Start:
        return start + margin;
    case Align::End:
        return start + length - size - margin;
    case Align::Middle:
    default:
        return start + (length - size) / 2;
    }
}

} // namespace

QString placementId(Placement placement)
{
    const int index = int(placement);
    return index >= 0 && index < int(Placement::Count) ? QLatin1String(kPlacementIds[index])
                                                       : QLatin1String(kPlacementIds[0]);
}

Placement placementFromId(const QString& id, Placement fallback)
{
    return fromId(kPlacementIds, id, fallback);
}

QString layoutId(Layout layout)
{
    const int index = int(layout);
    return index >= 0 && index < int(Layout::Count) ? QLatin1String(kLayoutIds[index])
                                                    : QLatin1String(kLayoutIds[0]);
}

Layout layoutFromId(const QString& id, Layout fallback)
{
    return fromId(kLayoutIds, id, fallback);
}

QString edgeZoneId(EdgeZone zone)
{
    const int index = int(zone);
    return index >= 0 && index < int(EdgeZone::Count) ? QLatin1String(kEdgeZoneIds[index])
                                                      : QLatin1String(kEdgeZoneIds[0]);
}

EdgeZone edgeZoneFromId(const QString& id, EdgeZone fallback)
{
    return fromId(kEdgeZoneIds, id, fallback);
}

int clampScalePercent(int percent)
{
    return std::clamp(percent, MinScalePercent, MaxScalePercent);
}

int clampOpacityPercent(int percent)
{
    return std::clamp(percent, MinOpacityPercent, MaxOpacityPercent);
}

int clampEdgeDelayMs(int delayMs)
{
    return std::clamp(delayMs, MinEdgeDelayMs, MaxEdgeDelayMs);
}

bool isFullHeight(Placement placement)
{
    return placement == Placement::LeftPanel || placement == Placement::RightPanel;
}

QSize placedSize(Placement placement, const QRect& bounds, const QSize& contentSize)
{
    if (isFullHeight(placement)) {
        return QSize(contentSize.width(), std::max(contentSize.height(), bounds.height()));
    }
    return contentSize;
}

QPoint clampToBounds(const QPoint& topLeft, const QSize& size, const QRect& bounds)
{
    // A menu bigger than the bounds keeps its top-left corner visible
    const int x =
        size.width() <= bounds.width()
            ? std::clamp(topLeft.x(), bounds.left(), bounds.left() + bounds.width() - size.width())
            : bounds.left();
    const int y =
        size.height() <= bounds.height()
            ? std::clamp(topLeft.y(), bounds.top(), bounds.top() + bounds.height() - size.height())
            : bounds.top();
    return QPoint(x, y);
}

QPoint placeMenu(Placement placement, const QRect& bounds, const QSize& size,
                 std::optional<QPoint> pointer, std::optional<QPointF> dragged, int margin)
{
    Align horizontal = Align::Middle;
    Align vertical = Align::Middle;
    int horizontalMargin = margin;
    int verticalMargin = margin;

    switch (placement) {
    case Placement::Cursor:
        if (pointer) {
            return clampToBounds(*pointer, size, bounds);
        }
        break;
    case Placement::Dragged:
        if (dragged) {
            const qreal fx = std::clamp(dragged->x(), 0.0, 1.0);
            const qreal fy = std::clamp(dragged->y(), 0.0, 1.0);
            const QPoint topLeft(bounds.left() + qRound(fx * bounds.width()),
                                 bounds.top() + qRound(fy * bounds.height()));
            return clampToBounds(topLeft, size, bounds);
        }
        break;
    case Placement::Top:
        vertical = Align::Start;
        break;
    case Placement::Bottom:
        vertical = Align::End;
        break;
    case Placement::Left:
        horizontal = Align::Start;
        break;
    case Placement::Right:
        horizontal = Align::End;
        break;
    case Placement::TopLeft:
        horizontal = Align::Start;
        vertical = Align::Start;
        break;
    case Placement::TopRight:
        horizontal = Align::End;
        vertical = Align::Start;
        break;
    case Placement::BottomLeft:
        horizontal = Align::Start;
        vertical = Align::End;
        break;
    case Placement::BottomRight:
        horizontal = Align::End;
        vertical = Align::End;
        break;
    case Placement::LeftPanel:
    case Placement::RightPanel:
        // Flush against the edge, top to bottom
        horizontal = placement == Placement::LeftPanel ? Align::Start : Align::End;
        vertical = Align::Start;
        horizontalMargin = 0;
        verticalMargin = 0;
        break;
    case Placement::Auto:
    case Placement::Center:
    case Placement::Count:
    default:
        break;
    }

    const QPoint topLeft(
        alignedCoordinate(horizontal, bounds.left(), bounds.width(), size.width(),
                          horizontalMargin),
        alignedCoordinate(vertical, bounds.top(), bounds.height(), size.height(), verticalMargin));
    return clampToBounds(topLeft, size, bounds);
}

QPointF normalizedPosition(const QPoint& topLeft, const QRect& bounds)
{
    const qreal fx = bounds.width() > 0 ? qreal(topLeft.x() - bounds.left()) / bounds.width() : 0.0;
    const qreal fy =
        bounds.height() > 0 ? qreal(topLeft.y() - bounds.top()) / bounds.height() : 0.0;
    return QPointF(std::clamp(fx, 0.0, 1.0), std::clamp(fy, 0.0, 1.0));
}

QPoint slideDirection(Placement placement)
{
    switch (placement) {
    case Placement::Top:
    case Placement::TopLeft:
    case Placement::TopRight:
        return QPoint(0, -1);
    case Placement::Bottom:
    case Placement::BottomLeft:
    case Placement::BottomRight:
        return QPoint(0, 1);
    case Placement::Left:
    case Placement::LeftPanel:
        return QPoint(-1, 0);
    case Placement::Right:
    case Placement::RightPanel:
        return QPoint(1, 0);
    default:
        // Center, cursor and dragged rise into place
        return QPoint(0, 1);
    }
}

bool pointerInZone(const QPoint& pointer, const QRect& bounds, EdgeZone zone, int thickness,
                   int cornerSize)
{
    if (!bounds.contains(pointer)) {
        return false;
    }

    const bool nearLeft = pointer.x() < bounds.left() + thickness;
    const bool nearRight = pointer.x() > bounds.right() - thickness;
    const bool nearTop = pointer.y() < bounds.top() + thickness;
    const bool nearBottom = pointer.y() > bounds.bottom() - thickness;
    const bool cornerLeft = pointer.x() < bounds.left() + cornerSize;
    const bool cornerRight = pointer.x() > bounds.right() - cornerSize;
    const bool cornerTop = pointer.y() < bounds.top() + cornerSize;
    const bool cornerBottom = pointer.y() > bounds.bottom() - cornerSize;

    switch (zone) {
    case EdgeZone::Top:
        return nearTop;
    case EdgeZone::Bottom:
        return nearBottom;
    case EdgeZone::Left:
        return nearLeft;
    case EdgeZone::Right:
        return nearRight;
    // A corner is where both of its edges' bands, widened to cornerSize, meet,
    // and the pointer touches at least one of the edges
    case EdgeZone::TopLeft:
        return cornerTop && cornerLeft && (nearTop || nearLeft);
    case EdgeZone::TopRight:
        return cornerTop && cornerRight && (nearTop || nearRight);
    case EdgeZone::BottomLeft:
        return cornerBottom && cornerLeft && (nearBottom || nearLeft);
    case EdgeZone::BottomRight:
        return cornerBottom && cornerRight && (nearBottom || nearRight);
    case EdgeZone::Count:
    default:
        return false;
    }
}

bool EdgeDwell::update(bool inZone, qint64 nowMs, int delayMs)
{
    if (!inZone) {
        reset();
        return false;
    }
    if (m_EnteredMs < 0) {
        m_EnteredMs = nowMs;
    }
    if (m_Fired || nowMs - m_EnteredMs < delayMs) {
        return false;
    }
    m_Fired = true;
    return true;
}

qint64 EdgeDwell::remainingMs(qint64 nowMs, int delayMs) const
{
    if (m_EnteredMs < 0 || m_Fired) {
        return -1;
    }
    return std::max<qint64>(m_EnteredMs + delayMs - nowMs, 0);
}

void EdgeDwell::reset()
{
    m_EnteredMs = -1;
    m_Fired = false;
}

} // namespace OverlayMenu
