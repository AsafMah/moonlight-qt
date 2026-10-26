#pragma once

#include <cstddef>
#include <cstdint>

class PresentStallDetector
{
public:
    static constexpr uint64_t DrainDurationUs = 500000;
    static constexpr unsigned int RequiredSlowFrames = 6;

    PresentStallDetector(unsigned int streamFps, unsigned int displayFps, bool framePacing,
                         bool vrrEnabled, unsigned int vrrCapFps)
    {
        const unsigned int effectiveFps =
            effectivePresentationFps(streamFps, displayFps, framePacing, vrrEnabled, vrrCapFps);
        m_ThresholdUs = effectiveFps > 0 ? (1250000ULL + effectiveFps - 1) / effectiveFps : 0;
        m_EffectiveFps = effectiveFps;
    }

    static unsigned int effectivePresentationFps(unsigned int streamFps, unsigned int displayFps,
                                                 bool framePacing, bool vrrEnabled,
                                                 unsigned int vrrCapFps)
    {
        if (streamFps == 0) {
            return 0;
        }
        if (vrrEnabled) {
            return vrrCapFps > 0 ? (streamFps < vrrCapFps ? streamFps : vrrCapFps) : streamFps;
        }
        if (framePacing && displayFps > 0) {
            return streamFps < displayFps ? streamFps : displayFps;
        }
        return streamFps;
    }

    uint64_t thresholdUs() const { return m_ThresholdUs; }
    unsigned int effectiveFps() const { return m_EffectiveFps; }

    bool observe(uint64_t nowUs, uint64_t renderUs, size_t queuedFrames, uint64_t prePresentUs = 0,
                 uint64_t presentUs = 0)
    {
        if (m_Triggered) {
            return false;
        }
        if (queuedFrames == 0 || m_ThresholdUs == 0 || renderUs <= m_ThresholdUs) {
            resetStreak();
            return false;
        }

        ++m_SlowFrames;
        m_SumRenderUs += renderUs;
        m_SumPrePresentUs += prePresentUs;
        m_SumPresentUs += presentUs;
        if (m_SlowFrames < RequiredSlowFrames) {
            return false;
        }

        m_Triggered = true;
        m_Draining = true;
        m_DrainEndUs = nowUs + DrainDurationUs;
        return true;
    }

    bool shouldSkipFrame(uint64_t nowUs)
    {
        if (!m_Draining) {
            return false;
        }
        if (nowUs < m_DrainEndUs) {
            return true;
        }
        m_Draining = false;
        m_DrainEnded = true;
        return false;
    }

    bool takeDrainEnded()
    {
        const bool ended = m_DrainEnded;
        m_DrainEnded = false;
        return ended;
    }

    void resetStreak()
    {
        if (!m_Triggered) {
            m_SlowFrames = 0;
            m_SumRenderUs = 0;
            m_SumPrePresentUs = 0;
            m_SumPresentUs = 0;
        }
    }

    bool hasTriggered() const { return m_Triggered; }
    uint64_t triggerAverageRenderUs() const { return average(m_SumRenderUs); }
    uint64_t triggerAveragePrePresentUs() const { return average(m_SumPrePresentUs); }
    uint64_t triggerAveragePresentUs() const { return average(m_SumPresentUs); }

private:
    uint64_t average(uint64_t total) const { return m_SlowFrames > 0 ? total / m_SlowFrames : 0; }

    uint64_t m_ThresholdUs = 0;
    unsigned int m_EffectiveFps = 0;
    unsigned int m_SlowFrames = 0;
    uint64_t m_SumRenderUs = 0;
    uint64_t m_SumPrePresentUs = 0;
    uint64_t m_SumPresentUs = 0;
    uint64_t m_DrainEndUs = 0;
    bool m_Triggered = false;
    bool m_Draining = false;
    bool m_DrainEnded = false;
};
