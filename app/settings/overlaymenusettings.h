#pragma once

#include "streaming/video/overlaymenugeometry.h"

#include <QObject>
#include <QPointF>

#include <optional>

// Options of the in-stream overlay menu, under "overlaymenu/..." in QSettings.
// Shared by the settings page (as the OverlayMenuSettings QML singleton) and
// Session, which reads them each time the menu opens.
//
// Values are read from QSettings on every access, because the menu changes
// some of them (placement, layout, scale and the dragged position) while the
// settings page's instance is alive. Every default keeps the menu as it was
// before these options existed; see OverlayMenu (overlaymenugeometry.h).
//
// The QML-facing enums mirror the OverlayMenu ones value for value.
class OverlayMenuSettings : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int layout READ layout WRITE setLayout NOTIFY changed)
    Q_PROPERTY(int scalePercent READ scalePercent WRITE setScalePercent NOTIFY changed)
    Q_PROPERTY(int opacityPercent READ opacityPercent WRITE setOpacityPercent NOTIFY changed)
    Q_PROPERTY(int buttonMode READ buttonMode WRITE setButtonMode NOTIFY changed)
    Q_PROPERTY(int edgeTriggerMode READ edgeTriggerMode WRITE setEdgeTriggerMode NOTIFY changed)
    Q_PROPERTY(int edgeZone READ edgeZone WRITE setEdgeZone NOTIFY changed)
    Q_PROPERTY(int edgeDelayMs READ edgeDelayMs WRITE setEdgeDelayMs NOTIFY changed)
    Q_PROPERTY(bool holdToPeek READ holdToPeek WRITE setHoldToPeek NOTIFY changed)
    Q_PROPERTY(int escapeAction READ escapeAction WRITE setEscapeAction NOTIFY changed)
    Q_PROPERTY(int gamepadTrigger READ gamepadTrigger WRITE setGamepadTrigger NOTIFY changed)
    Q_PROPERTY(bool hasDraggedPosition READ hasDraggedPosition NOTIFY changed)
    Q_PROPERTY(int minScalePercent READ minScalePercent CONSTANT)
    Q_PROPERTY(int maxScalePercent READ maxScalePercent CONSTANT)
    Q_PROPERTY(int minOpacityPercent READ minOpacityPercent CONSTANT)
    Q_PROPERTY(int maxOpacityPercent READ maxOpacityPercent CONSTANT)
    Q_PROPERTY(int maxEdgeDelayMs READ maxEdgeDelayMs CONSTANT)

public:
    enum LayoutValue
    {
        LayoutCompact = int(OverlayMenu::Layout::Compact),
        LayoutComfortable = int(OverlayMenu::Layout::Comfortable),
        LayoutSidePanel = int(OverlayMenu::Layout::SidePanel),
    };
    Q_ENUM(LayoutValue)

    enum TriggerModeValue
    {
        TriggerAuto = int(OverlayMenu::TriggerMode::Auto),
        TriggerOn = int(OverlayMenu::TriggerMode::On),
        TriggerOff = int(OverlayMenu::TriggerMode::Off),
    };
    Q_ENUM(TriggerModeValue)

    enum EdgeZoneValue
    {
        ZoneTop = int(OverlayMenu::EdgeZone::Top),
        ZoneBottom = int(OverlayMenu::EdgeZone::Bottom),
        ZoneLeft = int(OverlayMenu::EdgeZone::Left),
        ZoneRight = int(OverlayMenu::EdgeZone::Right),
        ZoneTopLeft = int(OverlayMenu::EdgeZone::TopLeft),
        ZoneTopRight = int(OverlayMenu::EdgeZone::TopRight),
        ZoneBottomLeft = int(OverlayMenu::EdgeZone::BottomLeft),
        ZoneBottomRight = int(OverlayMenu::EdgeZone::BottomRight),
    };
    Q_ENUM(EdgeZoneValue)

    enum EscapeActionValue
    {
        EscapeBack = int(OverlayMenu::EscapeAction::Back),
        EscapeClose = int(OverlayMenu::EscapeAction::Close),
    };
    Q_ENUM(EscapeActionValue)

    enum GamepadTriggerValue
    {
        GamepadHoldStart = int(OverlayMenu::GamepadTrigger::HoldStart),
        GamepadHoldBack = int(OverlayMenu::GamepadTrigger::HoldBack),
        GamepadOff = int(OverlayMenu::GamepadTrigger::Off),
    };
    Q_ENUM(GamepadTriggerValue)

    explicit OverlayMenuSettings(QObject* parent = nullptr);

    OverlayMenu::Layout layoutValue() const;
    int layout() const { return int(layoutValue()); }
    void setLayout(int layout);

    int scalePercent() const;
    void setScalePercent(int percent);

    int opacityPercent() const;
    void setOpacityPercent(int percent);

    // The floating button: Auto shows it when the menu position preference
    // is Floating button
    OverlayMenu::TriggerMode buttonModeValue() const;
    int buttonMode() const { return int(buttonModeValue()); }
    void setButtonMode(int mode);

    // Resting the free pointer on a zone of the stream opens the menu. Auto
    // keeps the original immediate edge trigger of the edge positions.
    OverlayMenu::TriggerMode edgeTriggerModeValue() const;
    int edgeTriggerMode() const { return int(edgeTriggerModeValue()); }
    void setEdgeTriggerMode(int mode);

    OverlayMenu::EdgeZone edgeZoneValue() const;
    int edgeZone() const { return int(edgeZoneValue()); }
    void setEdgeZone(int zone);

    int edgeDelayMs() const;
    void setEdgeDelayMs(int delayMs);

    // Holding the overlay menu shortcut shows the menu until it is released
    bool holdToPeek() const;
    void setHoldToPeek(bool enabled);

    OverlayMenu::EscapeAction escapeActionValue() const;
    int escapeAction() const { return int(escapeActionValue()); }
    void setEscapeAction(int action);

    OverlayMenu::GamepadTrigger gamepadTriggerValue() const;
    int gamepadTrigger() const { return int(gamepadTriggerValue()); }
    void setGamepadTrigger(int trigger);

    // Where the menu was last manually dragged, as OverlayMenu::normalizedPosition()
    std::optional<QPointF> draggedPosition() const;
    bool hasDraggedPosition() const { return draggedPosition().has_value(); }
    // Remembers the position; when present, the menu follows it instead of its trigger.
    void setDraggedPosition(const QPointF& position);
    // Forgets the manual position so the menu opens at its trigger again.
    Q_INVOKABLE void resetDraggedPosition();

    // Re-reads everything, for a page that may have missed in-stream changes
    Q_INVOKABLE void reload() { emit changed(); }

    int minScalePercent() const { return OverlayMenu::MinScalePercent; }
    int maxScalePercent() const { return OverlayMenu::MaxScalePercent; }
    int minOpacityPercent() const { return OverlayMenu::MinOpacityPercent; }
    int maxOpacityPercent() const { return OverlayMenu::MaxOpacityPercent; }
    int maxEdgeDelayMs() const { return OverlayMenu::MaxEdgeDelayMs; }

signals:
    void changed();
};
