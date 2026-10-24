#include "pyrowavepacing.h"

namespace PyrowavePacing {

uint64_t frameIntervalUs(int streamFps, int displayFps)
{
    int slowest = 0;
    if (streamFps > 0) {
        slowest = streamFps;
    }
    if (displayFps > 0 && (slowest == 0 || displayFps < slowest)) {
        slowest = displayFps;
    }
    if (slowest == 0) {
        slowest = 60;
    }
    return 1000000ULL / (uint64_t)slowest;
}

void SwapchainDepth::configure(int streamFps, int displayFps)
{
    m_StreamFps = streamFps > 0 ? streamFps : 60;
    m_ThresholdUs = frameIntervalUs(streamFps, displayFps) * 11 / 10;
}

bool SwapchainDepth::recordPresent(uint64_t presentUs)
{
    if (m_Depth != k_DoubleBuffered) {
        return false;
    }

    if (presentUs > m_ThresholdUs) {
        m_DelayedPresents++;
    } else if (m_DelayedPresents > 0) {
        m_DelayedPresents--;
    }

    // PlVkRenderer switches at exactly half the stream rate; >= also covers
    // a stream rate of 1, where half of it is 0.
    if (m_DelayedPresents >= m_StreamFps / 2 && m_DelayedPresents > 0) {
        m_Depth = k_TripleBuffered;
        return true;
    }
    return false;
}

void FramePacer::configure(bool enabled, int streamFps, int displayFps)
{
    m_StreamFps = streamFps;
    m_DisplayFps = displayFps;
    m_Decimating = enabled && streamFps > 0 && displayFps > 0 && displayFps < streamFps;
    // Starts one refresh short, so the first frame is presented.
    m_Credit = m_Decimating ? m_StreamFps - m_DisplayFps : 0;
}

bool FramePacer::shouldPresent()
{
    if (!m_Decimating) {
        return true;
    }

    // Every frame earns displayFps; a present costs streamFps. Over
    // streamFps frames that presents exactly displayFps of them.
    m_Credit += m_DisplayFps;
    if (m_Credit >= m_StreamFps) {
        m_Credit -= m_StreamFps;
        return true;
    }
    return false;
}

}
