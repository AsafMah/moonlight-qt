// Session's integration with the overlay menu options (OverlayMenuSettings):
// placement, layout, the floating button, the edge trigger, hold to peek and
// the gamepad trigger. Kept out of session.cpp so the hooks there stay
// one-liners.

#include "session.h"
#include "settings/overlaymenusettings.h"

#include <QCursor>

using OverlayMenu::Placement;
using OverlayMenu::TriggerMode;

namespace {

// Depth of the zone strips along the stream's edges, and the size of the
// corner squares, in logical pixels
constexpr int kEdgeZoneThickness = 6;
constexpr int kEdgeCornerSize = 48;

// Matches the original edge trigger's debounce after the menu closes
constexpr Uint32 kEdgeReopenDelayMs = 500;

bool isEdgePosition(StreamingPreferences::OverlayMenuPosition position)
{
    return position == StreamingPreferences::OMP_TOP_EDGE ||
           position == StreamingPreferences::OMP_RIGHT_EDGE ||
           position == StreamingPreferences::OMP_LEFT_EDGE;
}

} // namespace

void Session::reloadOverlayMenuOptions()
{
    const OverlayMenuSettings settings;
    const StreamingPreferences::OverlayMenuPosition position = m_Preferences->overlayMenuPosition;
    const bool disabled = position == StreamingPreferences::OMP_DISABLED;

    OverlayMenuOptions& options = m_OverlayMenuOptions;
    switch (settings.buttonModeValue()) {
    case TriggerMode::On:
        options.buttonEnabled = !disabled;
        break;
    case TriggerMode::Off:
        options.buttonEnabled = false;
        break;
    case TriggerMode::Auto:
    default:
        options.buttonEnabled = position == StreamingPreferences::OMP_BUTTON;
        break;
    }

    const TriggerMode edgeMode = settings.edgeTriggerModeValue();
    options.legacyEdgeTrigger = edgeMode == TriggerMode::Auto && isEdgePosition(position);
    options.edgeTrigger = edgeMode == TriggerMode::On && !disabled;
    options.edgeZone = settings.edgeZoneValue();
    options.edgeDelayMs = settings.edgeDelayMs();
    options.holdToPeek = settings.holdToPeek();
    options.gamepadTrigger = settings.gamepadTriggerValue();
    m_MenuEdgeDwell.reset();
}

OverlayMenu::Placement Session::applyOverlayMenuOptionsToPanel()
{
    const OverlayMenuSettings settings;
    m_MenuPanel->setStyle(settings.layoutValue(), settings.scalePercent(),
                          settings.opacityPercent());
    m_MenuPanel->setEscapeAction(settings.escapeActionValue());
    m_MenuPanel->updateMenuStyleState();
    return settings.placementValue();
}

bool Session::showQtOverlayMenuWithPlacement(const QRect& parentRect,
                                             std::optional<QPoint> pointerPosition,
                                             bool closeWhenPointerOutside)
{
    const OverlayMenuSettings settings;
    const Placement placement = settings.placementValue();
    if (placement == Placement::Auto) {
        return false;
    }

    if (placement == Placement::Cursor && !pointerPosition) {
        pointerPosition = currentOverlayPointerPosition();
    }
    m_MenuPanel->showWithPlacement(placement, parentRect.x(), parentRect.y(), parentRect.width(),
                                   parentRect.height(), pointerPosition, settings.draggedPosition(),
                                   closeWhenPointerOutside);
    if (m_MenuButton) {
        m_MenuButton->hideButton();
    }
    return true;
}

QPoint Session::currentOverlayPointerPosition() const
{
    if (m_WaylandStreamWindow != nullptr) {
        // Native Wayland children use parent-local coordinates, and Qt cannot
        // read the global pointer position there
        int x = 0;
        int y = 0;
        SDL_GetMouseState(&x, &y);
        return qtOverlayPositionForSdlPoint(x, y);
    }
    return QCursor::pos();
}

bool Session::handleOverlayMenuOptionAction(OverlayMenuPanel::MenuAction action)
{
    if (const auto placement = OverlayMenuPanel::placementForAction(action)) {
        OverlayMenuSettings().setPlacement(int(*placement));
        return true;
    }
    if (const auto layout = OverlayMenuPanel::layoutForAction(action)) {
        OverlayMenuSettings().setLayout(int(*layout));
        return true;
    }
    return false;
}

bool Session::handleOverlayMenuOptionPayload(OverlayMenuPanel::MenuAction action,
                                             const QString& payload)
{
    if (action != OverlayMenuPanel::MenuAction::SetMenuScale) {
        return false;
    }
    bool ok = false;
    const int percent = payload.toInt(&ok);
    if (ok) {
        OverlayMenuSettings().setScalePercent(percent);
    }
    return true;
}

void Session::setupOverlayMenuShortcut()
{
    // A hold handler sees the key go down and come up. Without hold to peek
    // the key-down toggles the menu, as before.
    m_InputHandler->getShortcutDispatcher().setHoldHandler(
        QLatin1String(ShortcutActions::ToggleOverlayMenu), [this](bool held) {
            if (held) {
                if (m_OverlayMenuOptions.holdToPeek && m_MenuPanel &&
                    !m_MenuPanel->isMenuVisible()) {
                    showQtOverlayMenu();
                    m_MenuPeeking = m_MenuPanel->isMenuVisible();
                } else {
                    toggleQtOverlayMenu();
                }
                return;
            }

            // Peeking ends with the key-up, unless the user started using
            // the menu meanwhile
            if (m_MenuPeeking) {
                m_MenuPeeking = false;
                if (m_MenuPanel && m_MenuPanel->isMenuVisible() &&
                    !m_MenuPanel->hasUserInteracted()) {
                    hideQtOverlayMenu();
                }
            }
        });

    m_MenuPanel->setDragFinishedCallback(
        [](const QPointF& position) { OverlayMenuSettings().setDraggedPosition(position); });
}

void Session::updateOverlayMenuEdgeTrigger()
{
    if (!m_MenuPanel || !m_OverlayMenuOptions.edgeTrigger) {
        return;
    }

    // An open menu keeps the visit: resting on in the zone after it closes
    // does not open it again
    if (m_MenuPanel->isMenuVisible() || m_MenuPanel->isClosing()) {
        return;
    }

    // Only while the pointer is free
    if (m_InputHandler->isCaptureActive() || !isStreamingWindowVisible()) {
        m_MenuEdgeDwell.reset();
        return;
    }

    const QRect parentRect = qtOverlayParentGeometry();
    const QPoint pointer = currentOverlayPointerPosition();
    const bool inZone =
        parentRect.isValid() &&
        OverlayMenu::pointerInZone(pointer, parentRect, m_OverlayMenuOptions.edgeZone,
                                   kEdgeZoneThickness, kEdgeCornerSize);
    if (m_MenuEdgeDwell.update(inZone, SDL_GetTicks(), m_OverlayMenuOptions.edgeDelayMs) &&
        SDL_GetTicks() - m_MenuCloseTicks > kEdgeReopenDelayMs) {
        showQtOverlayMenu(pointer);
    }
}

int Session::overlayMenuEdgeTriggerDelayMs() const
{
    if (!m_OverlayMenuOptions.edgeTrigger) {
        return -1;
    }
    const qint64 remaining =
        m_MenuEdgeDwell.remainingMs(SDL_GetTicks(), m_OverlayMenuOptions.edgeDelayMs);
    return remaining < 0 ? -1 : static_cast<int>(remaining);
}
