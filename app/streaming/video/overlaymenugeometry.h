#pragma once

#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QString>

#include <optional>

// Placement, layout and trigger options of the in-stream overlay menu
// (OverlayMenuPanel), and the geometry behind them. QtCore only, so it is unit
// tested on its own (tests/overlay_menu_navigation).
//
// Every option has a default that keeps the menu exactly as it was before the
// options existed: placement Auto and the Auto trigger modes follow the
// "Overlay menu position" preference (StreamingPreferences::overlayMenuPosition).
namespace OverlayMenu {

// Where the menu opens. Ids are persisted: "auto", "center", ...
enum class Placement
{
    Auto,   // as the overlay menu position preference says (edge or button)
    Center, // centered on the stream
    Cursor, // next to the pointer
    Top,
    Bottom,
    Left,
    Right,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    LeftPanel,  // full-height panel along the left edge
    RightPanel, // full-height panel along the right edge
    Dragged,    // where the user last dragged it (see draggedPosition)
    Count
};

QString placementId(Placement placement);
Placement placementFromId(const QString& id, Placement fallback);

// Row size and tab arrangement
enum class Layout
{
    Compact,     // the original look
    Comfortable, // bigger rows and text, for a TV or a gamepad
    SidePanel,   // tabs as a vertical rail on the left of the rows
    Count
};

QString layoutId(Layout layout);
Layout layoutFromId(const QString& id, Layout fallback);

// What Esc (and the gamepad's B) does
enum class EscapeAction
{
    Back,  // up one level, closing from a tab (the original behavior)
    Close, // close the menu from any level
};

// Gamepad button that opens the menu when held (with Gamepad Mouse off)
enum class GamepadTrigger
{
    HoldStart, // the original trigger
    HoldBack,  // Back / Select / View
    Off,
};

// Auto follows the overlay menu position preference; On and Off override it
enum class TriggerMode
{
    Auto,
    On,
    Off,
};

// The part of the stream a pointer can rest on to open the menu
enum class EdgeZone
{
    Top,
    Bottom,
    Left,
    Right,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    Count
};

QString edgeZoneId(EdgeZone zone);
EdgeZone edgeZoneFromId(const QString& id, EdgeZone fallback);

constexpr int MinScalePercent = 75;
constexpr int MaxScalePercent = 150;
constexpr int DefaultScalePercent = 100;
constexpr int MinOpacityPercent = 40;
constexpr int MaxOpacityPercent = 100;
constexpr int DefaultOpacityPercent = 100;
constexpr int MinEdgeDelayMs = 0;
constexpr int MaxEdgeDelayMs = 2000;
constexpr int DefaultEdgeDelayMs = 400;

int clampScalePercent(int percent);
int clampOpacityPercent(int percent);
int clampEdgeDelayMs(int delayMs);

// Placements that take the bounds' full height
bool isFullHeight(Placement placement);

// Top-left corner of a menu of the given size. bounds is where the menu may
// be (the stream, cut down to the screen it is on); margin keeps it off
// the edges it is placed against. pointer is used by Cursor, dragged
// (0..1 of the free space, see normalizedPosition()) by Dragged. The result
// always lies inside bounds; a menu bigger than bounds keeps its top-left
// corner visible.
QPoint placeMenu(Placement placement, const QRect& bounds, const QSize& size,
                 std::optional<QPoint> pointer, std::optional<QPointF> dragged, int margin);

// The menu's size for a placement: full-height placements take the bounds'
// height, never less than the content's
QSize placedSize(Placement placement, const QRect& bounds, const QSize& contentSize);

QPoint clampToBounds(const QPoint& topLeft, const QSize& size, const QRect& bounds);

// Position of a menu as a fraction of the space it can move in, so it lands
// in the same place on a stream of another size
QPointF normalizedPosition(const QPoint& topLeft, const QSize& size, const QRect& bounds);

// The direction a menu slides in from: -1, 0 or +1 on each axis
QPoint slideDirection(Placement placement);

// Whether a pointer is in a zone along the inside of bounds. Edges are
// thickness pixels deep; corners are cornerSize pixels square.
bool pointerInZone(const QPoint& pointer, const QRect& bounds, EdgeZone zone, int thickness,
                   int cornerSize);

// Opens the menu once the pointer has rested in a zone for a delay. It fires
// once per visit: the pointer must leave the zone before it can fire again.
class EdgeDwell
{
public:
    // Returns true when the menu should open now
    bool update(bool inZone, qint64 nowMs, int delayMs);
    // Milliseconds until update() would fire, or -1 when not waiting
    qint64 remainingMs(qint64 nowMs, int delayMs) const;
    void reset();

private:
    qint64 m_EnteredMs = -1;
    bool m_Fired = false;
};

} // namespace OverlayMenu
