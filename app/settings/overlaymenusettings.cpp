#include "overlaymenusettings.h"

#include <QSettings>

#define SER_OVERLAYMENU "overlaymenu"
#define SER_PLACEMENT "placement"
#define SER_LAYOUT "layout"
#define SER_SCALE "scale"
#define SER_OPACITY "opacity"
#define SER_BUTTON "button"
#define SER_EDGETRIGGER "edgeTrigger"
#define SER_EDGEZONE "edgeZone"
#define SER_EDGEDELAY "edgeDelayMs"
#define SER_HOLDTOPEEK "holdToPeek"
#define SER_ESCAPE "escape"
#define SER_GAMEPAD "gamepadTrigger"
#define SER_DRAGGED "draggedPosition"

using namespace OverlayMenu;

namespace {

QString key(const char* name)
{
    return QStringLiteral(SER_OVERLAYMENU "/") + QLatin1String(name);
}

QVariant value(const char* name, const QVariant& fallback = QVariant())
{
    return QSettings().value(key(name), fallback);
}

// Stores a value, and says whether it changed
bool store(const char* name, const QVariant& newValue)
{
    QSettings settings;
    if (settings.value(key(name)) == newValue) {
        return false;
    }
    settings.setValue(key(name), newValue);
    return true;
}

const char* const kTriggerModeIds[] = { "auto", "on", "off" };
const char* const kEscapeIds[] = { "back", "close" };
const char* const kGamepadIds[] = { "holdstart", "holdback", "off" };

template <int N> int indexOf(const char* const (&ids)[N], const QString& id, int fallback)
{
    for (int i = 0; i < N; i++) {
        if (id.compare(QLatin1String(ids[i]), Qt::CaseInsensitive) == 0) {
            return i;
        }
    }
    return fallback;
}

template <int N> bool validIndex(const char* const (&)[N], int index)
{
    return index >= 0 && index < N;
}

} // namespace

OverlayMenuSettings::OverlayMenuSettings(QObject* parent) : QObject(parent) {}

Placement OverlayMenuSettings::placementValue() const
{
    const Placement placement = placementFromId(value(SER_PLACEMENT).toString(), Placement::Auto);
    // Without a remembered position, Dragged behaves as Auto
    return placement == Placement::Dragged && !hasDraggedPosition() ? Placement::Auto : placement;
}

void OverlayMenuSettings::setPlacement(int placement)
{
    if (placement < 0 || placement >= int(Placement::Count)) {
        return;
    }
    if (store(SER_PLACEMENT, placementId(Placement(placement)))) {
        emit changed();
    }
}

Layout OverlayMenuSettings::layoutValue() const
{
    return layoutFromId(value(SER_LAYOUT).toString(), Layout::Compact);
}

void OverlayMenuSettings::setLayout(int layout)
{
    if (layout < 0 || layout >= int(Layout::Count)) {
        return;
    }
    if (store(SER_LAYOUT, layoutId(Layout(layout)))) {
        emit changed();
    }
}

int OverlayMenuSettings::scalePercent() const
{
    return clampScalePercent(value(SER_SCALE, DefaultScalePercent).toInt());
}

void OverlayMenuSettings::setScalePercent(int percent)
{
    if (store(SER_SCALE, clampScalePercent(percent))) {
        emit changed();
    }
}

int OverlayMenuSettings::opacityPercent() const
{
    return clampOpacityPercent(value(SER_OPACITY, DefaultOpacityPercent).toInt());
}

void OverlayMenuSettings::setOpacityPercent(int percent)
{
    if (store(SER_OPACITY, clampOpacityPercent(percent))) {
        emit changed();
    }
}

TriggerMode OverlayMenuSettings::buttonModeValue() const
{
    return TriggerMode(indexOf(kTriggerModeIds, value(SER_BUTTON).toString(), 0));
}

void OverlayMenuSettings::setButtonMode(int mode)
{
    if (validIndex(kTriggerModeIds, mode) &&
        store(SER_BUTTON, QLatin1String(kTriggerModeIds[mode]))) {
        emit changed();
    }
}

TriggerMode OverlayMenuSettings::edgeTriggerModeValue() const
{
    return TriggerMode(indexOf(kTriggerModeIds, value(SER_EDGETRIGGER).toString(), 0));
}

void OverlayMenuSettings::setEdgeTriggerMode(int mode)
{
    if (validIndex(kTriggerModeIds, mode) &&
        store(SER_EDGETRIGGER, QLatin1String(kTriggerModeIds[mode]))) {
        emit changed();
    }
}

EdgeZone OverlayMenuSettings::edgeZoneValue() const
{
    return edgeZoneFromId(value(SER_EDGEZONE).toString(), EdgeZone::Right);
}

void OverlayMenuSettings::setEdgeZone(int zone)
{
    if (zone >= 0 && zone < int(EdgeZone::Count) &&
        store(SER_EDGEZONE, edgeZoneId(EdgeZone(zone)))) {
        emit changed();
    }
}

int OverlayMenuSettings::edgeDelayMs() const
{
    return clampEdgeDelayMs(value(SER_EDGEDELAY, DefaultEdgeDelayMs).toInt());
}

void OverlayMenuSettings::setEdgeDelayMs(int delayMs)
{
    if (store(SER_EDGEDELAY, clampEdgeDelayMs(delayMs))) {
        emit changed();
    }
}

bool OverlayMenuSettings::holdToPeek() const
{
    return value(SER_HOLDTOPEEK, false).toBool();
}

void OverlayMenuSettings::setHoldToPeek(bool enabled)
{
    if (store(SER_HOLDTOPEEK, enabled)) {
        emit changed();
    }
}

EscapeAction OverlayMenuSettings::escapeActionValue() const
{
    return EscapeAction(indexOf(kEscapeIds, value(SER_ESCAPE).toString(), 0));
}

void OverlayMenuSettings::setEscapeAction(int action)
{
    if (validIndex(kEscapeIds, action) && store(SER_ESCAPE, QLatin1String(kEscapeIds[action]))) {
        emit changed();
    }
}

GamepadTrigger OverlayMenuSettings::gamepadTriggerValue() const
{
    return GamepadTrigger(indexOf(kGamepadIds, value(SER_GAMEPAD).toString(), 0));
}

void OverlayMenuSettings::setGamepadTrigger(int trigger)
{
    if (validIndex(kGamepadIds, trigger) &&
        store(SER_GAMEPAD, QLatin1String(kGamepadIds[trigger]))) {
        emit changed();
    }
}

std::optional<QPointF> OverlayMenuSettings::draggedPosition() const
{
    const QVariant stored = value(SER_DRAGGED);
    if (!stored.isValid()) {
        return std::nullopt;
    }
    const QPointF position = stored.toPointF();
    if (position.x() < 0 || position.x() > 1 || position.y() < 0 || position.y() > 1) {
        return std::nullopt;
    }
    return position;
}

void OverlayMenuSettings::setDraggedPosition(const QPointF& position)
{
    QSettings settings;
    settings.setValue(key(SER_DRAGGED), position);
    settings.setValue(key(SER_PLACEMENT), placementId(Placement::Dragged));
    emit changed();
}

void OverlayMenuSettings::resetDraggedPosition()
{
    QSettings settings;
    settings.remove(key(SER_DRAGGED));
    if (placementFromId(settings.value(key(SER_PLACEMENT)).toString(), Placement::Auto) ==
        Placement::Dragged) {
        settings.setValue(key(SER_PLACEMENT), placementId(Placement::Auto));
    }
    emit changed();
}
