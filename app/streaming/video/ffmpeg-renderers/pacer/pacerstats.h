#pragma once

#include <QMutex>
#include <QMutexLocker>

#include <cstdint>

struct PacerStatsWindow
{
    uint64_t droppedFrames = 0;
    uint64_t renderedFrames = 0;
    uint64_t totalPacerTimeUs = 0;
    uint64_t totalRenderTimeUs = 0;
    uint64_t timedRenderFrames = 0;
    uint64_t totalPrePresentTimeUs = 0;
    uint64_t totalPresentTimeUs = 0;
};

// Pacer and renderer threads publish their counters together so the decoder
// can take a coherent window without racing a copy/reset of VIDEO_STATS.
class PacerStats
{
public:
    void recordDrop()
    {
        QMutexLocker locker(&m_Lock);
        ++m_DroppedFrames;
    }

    void recordPresent(uint64_t pacerTimeUs, uint64_t renderTimeUs)
    {
        QMutexLocker locker(&m_Lock);
        ++m_RenderedFrames;
        m_TotalPacerTimeUs += pacerTimeUs;
        m_TotalRenderTimeUs += renderTimeUs;
    }

    void recordPresent(uint64_t pacerTimeUs, uint64_t renderTimeUs, uint64_t prePresentTimeUs,
                       uint64_t presentTimeUs)
    {
        QMutexLocker locker(&m_Lock);
        ++m_RenderedFrames;
        ++m_TimedRenderFrames;
        m_TotalPacerTimeUs += pacerTimeUs;
        m_TotalRenderTimeUs += renderTimeUs;
        m_TotalPrePresentTimeUs += prePresentTimeUs;
        m_TotalPresentTimeUs += presentTimeUs;
    }

    PacerStatsWindow take()
    {
        QMutexLocker locker(&m_Lock);
        const PacerStatsWindow window{ m_DroppedFrames,     m_RenderedFrames,
                                       m_TotalPacerTimeUs,  m_TotalRenderTimeUs,
                                       m_TimedRenderFrames, m_TotalPrePresentTimeUs,
                                       m_TotalPresentTimeUs };
        m_DroppedFrames = 0;
        m_RenderedFrames = 0;
        m_TotalPacerTimeUs = 0;
        m_TotalRenderTimeUs = 0;
        m_TimedRenderFrames = 0;
        m_TotalPrePresentTimeUs = 0;
        m_TotalPresentTimeUs = 0;
        return window;
    }

private:
    QMutex m_Lock;
    uint64_t m_DroppedFrames = 0;
    uint64_t m_RenderedFrames = 0;
    uint64_t m_TotalPacerTimeUs = 0;
    uint64_t m_TotalRenderTimeUs = 0;
    uint64_t m_TimedRenderFrames = 0;
    uint64_t m_TotalPrePresentTimeUs = 0;
    uint64_t m_TotalPresentTimeUs = 0;
};
