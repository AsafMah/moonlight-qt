// Session's integration with the performance overlay (OverlayStatsController).
// Kept out of session.cpp so the hooks there stay one-liners.

#include "session.h"
#include "streaming/video/overlaystatscontroller.h"

#include <QCursor>

void Session::setupStatsOverlayShortcuts()
{
    ShortcutDispatcher& dispatcher = m_InputHandler->getShortcutDispatcher();
    m_InputHandler->setMouseReleaseModeCallback([this](bool persistent) {
        if (persistent) {
            m_WasCapturedBeforeStatsEdit = false;
            m_WasCapturedBeforeMenu = false;
            showStreamingToast(tr("Mouse free: click the overlay or menu to edit. "
                                  "Double-tap Right Alt or press Esc to finish."),
                               3500);
        }
        if (m_StatsOverlay) {
            m_StatsOverlay->setPointerEditMode(persistent);
        }
        if (m_MenuPanel) {
            m_MenuPanel->setPointerEditMode(persistent);
            if (!persistent && m_MenuPanel->isMenuVisible()) {
                m_MenuPanel->closeMenu();
            }
        }
    });

    // Replaces the input handler's own toggle, which only knows the text overlay.
    // Until the overlay exists (before the stream starts) keep that behavior.
    dispatcher.setHandler(QLatin1String(ShortcutActions::ToggleStatsOverlay), [this] {
        if (m_StatsOverlay) {
            m_StatsOverlay->toggle();
        } else {
            m_OverlayManager.setOverlayState(
                Overlay::OverlayDebug, !m_OverlayManager.isOverlayEnabled(Overlay::OverlayDebug));
        }
    });
    dispatcher.setHandler(QLatin1String(ShortcutActions::CycleOverlayPreset), [this] {
        if (m_StatsOverlay) {
            m_StatsOverlay->cyclePreset();
        }
    });
    dispatcher.setHandler(QLatin1String(ShortcutActions::EditOverlayPosition), [this] {
        if (m_StatsOverlay) {
            m_StatsOverlay->toggleEditing();
        }
    });
}

void Session::createStatsOverlay(OverlayWindowMode windowMode)
{
    m_StatsOverlay = std::make_unique<OverlayStatsController>(
        m_OverlayManager, m_StatsModel, m_WaylandStreamWindow, windowMode,
        m_Preferences->showPerformanceOverlay);

    m_StatsOverlay->setExtrasProvider([this] {
        OverlayStatsWindow::Extras extras;
        extras.abrEnabled = m_SunshineAbrEnabled;
        // Updated by ABR feedback and by runtime bitrate changes
        const int appliedKbps = m_AbrCurrentBitrateKbps->load();
        extras.targetBitrateKbps = appliedKbps > 0 ? appliedKbps : m_StreamConfig.bitrate;
        extras.vrrCapFps = m_VrrPresentCapFps;
        return extras;
    });

    m_StatsOverlay->setEditModeCallback([this](bool editing) {
        if (editing) {
            if (m_InputHandler->isMouseReleasePersistent()) {
                m_WasCapturedBeforeStatsEdit = false;
                return;
            }
            // The pointer has to be free to drag the overlay. If the overlay
            // menu started this, take over its pending capture restore so the
            // menu's close animation does not recapture mid-edit.
            // A hold to release the mouse ends here too, or letting go of
            // its key would capture the pointer in the middle of a drag.
            m_WasCapturedBeforeStatsEdit =
                m_WasCapturedBeforeStatsEdit || m_WasCapturedBeforeMenu ||
                m_InputHandler->cancelMouseHoldRelease() || m_InputHandler->isCaptureActive();
            m_WasCapturedBeforeMenu = false;
            m_InputHandler->setCaptureActive(false);
            showStreamingToast(tr("Drag the overlay to move it. Press Esc when done."), 2500);
        } else if (m_WasCapturedBeforeStatsEdit) {
            m_WasCapturedBeforeStatsEdit = false;
            if (isStreamingWindowVisible() && !(m_MenuPanel && m_MenuPanel->isMenuVisible())) {
                m_InputHandler->setCaptureActive(true);
            }
        }
    });

    // Tooltips follow the pointer whenever the stream does not have it. The
    // overlay menu owns a free pointer while it is open.
    m_StatsOverlay->setPointerProvider([this]() -> std::optional<QPoint> {
        if (m_InputHandler->isCaptureActive() || (m_MenuPanel && m_MenuPanel->isMenuVisible())) {
            return std::nullopt;
        }
        if (m_WaylandStreamWindow != nullptr && !(m_StatsOverlay && m_StatsOverlay->isDetached())) {
            // Native Wayland children use parent-local coordinates, and Qt
            // cannot read the global pointer position there
            int x = 0;
            int y = 0;
            SDL_GetMouseState(&x, &y);
            return qtOverlayPositionForSdlPoint(x, y);
        }
        return QCursor::pos();
    });
    m_StatsOverlay->setParentGeometryProvider([this] { return qtOverlayParentGeometry(); });
    m_StatsOverlay->syncWithStreamWindow(isStreamingWindowVisible());
}

bool Session::handleStatsOverlayMenuAction(OverlayMenuPanel::MenuAction action)
{
    if (const auto preset = OverlayMenuPanel::statsPresetForAction(action)) {
        if (m_StatsOverlay) {
            m_StatsOverlay->setPreset(*preset);
        }
        return true;
    }

    if (action == OverlayMenuPanel::MenuAction::EditStatsOverlayPosition) {
        if (m_StatsOverlay) {
            m_StatsOverlay->beginEditing();
        }
        return true;
    }

    if (action == OverlayMenuPanel::MenuAction::ToggleStatsOverlayDetached) {
        if (m_StatsOverlay) {
            m_PendingStatsOverlayDetached = !m_StatsOverlay->isDetached();
        }
        return true;
    }

    return false;
}
