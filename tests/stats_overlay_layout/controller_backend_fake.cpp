#include "streaming/video/overlaymanager.h"

#include <Limelight.h>
#include <QElapsedTimer>

// Keep the real controller/window/input path, without a video renderer or connection.
Overlay::OverlayManager::OverlayManager() : m_Renderer(nullptr)
{
    memset(m_Overlays, 0, sizeof(m_Overlays));
}

Overlay::OverlayManager::~OverlayManager() = default;

void Overlay::OverlayManager::setOverlayState(OverlayType type, bool enabled)
{
    m_Overlays[type].enabled = enabled;
}

uint64_t LiGetMicroseconds()
{
    static const QElapsedTimer clock = [] {
        QElapsedTimer timer;
        timer.start();
        return timer;
    }();
    return clock.nsecsElapsed() / 1000;
}
