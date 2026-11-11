#pragma once

#include <cmath>
#include <cstdint>

// Tells whether VRR is actually engaged on the display while the session runs
// in VRR mode. The VRR preference only says what the user asked for: with
// adaptive sync off in the monitor's menu, or G-Sync off in the driver, the
// panel keeps a fixed refresh rate, and the tearing present-on-arrival path
// tears and drops frames against a clock it does not know about.
//
// Two sources, in order of authority:
// - NVAPI (NvAPI_D3D_IsGSyncActive on our swapchain). Once it has answered,
//   it alone decides, unless its query keeps failing. A state must hold for
//   2 s of polls (1 Hz) to count.
// - The vblank cadence from DXGI frame statistics, for other GPUs. Over 2 s
//   windows, V = vblanks per second (SyncRefreshCount) and P = presents per
//   second (PresentCount). VRR refreshes once per present (V ~ P); a fixed
//   panel keeps V at its refresh rate while P is lower. It needs headroom
//   (P below 97% of the refresh rate) and real vblanks: Windows' vblank
//   virtualization reports a steady virtual cadence, so the cadence is only
//   read when virtualization is disabled. Present() blocking is not used:
//   a driver frame cap also blocks Present() with VRR engaged.
class VrrEngagementDetector
{
public:
    enum class Verdict
    {
        Unknown,
        Engaged,
        NotEngaged,
    };

    enum class Source
    {
        None,
        Nvapi,
        VblankCadence,
    };

    enum class GsyncState
    {
        // No NVIDIA driver, or not the NVIDIA GPU driving the display
        Unavailable,
        Active,
        Inactive,
        // The query failed this time (handle invalidated by a mode change,
        // swapchain busy resizing)
        Error,
    };

    struct Config
    {
        double displayHz = 0;
        bool vblankVirtualizationDisabled = false;
    };

    // Swapchain creation, the first presents and mode changes settle first;
    // NVAPI documents its answer as valid only after the first present
    static constexpr uint64_t WarmupUs = 2000000;
    // NvAPI_D3D_IsGSyncActive is documented as time-consuming
    static constexpr uint64_t NvapiPollUs = 1000000;
    static constexpr uint64_t NvapiStableUs = 2000000;
    // Consecutive failed queries before NVAPI is treated as unavailable
    static constexpr int MaxNvapiErrors = 5;
    static constexpr uint64_t CadenceWindowUs = 2000000;
    // Presents at or above this share of the refresh rate leave no room to
    // tell a fixed refresh from VRR
    static constexpr double HeadroomRatio = 0.97;
    static constexpr double MatchTolerance = 0.02;
    static constexpr double FixedRatioMinimum = 1.03;
    static constexpr double RefreshTolerance = 0.01;

    explicit VrrEngagementDetector(const Config& config) : m_Config(config) {}

    bool nvapiPollDue(uint64_t nowUs)
    {
        anchorStart(nowUs);
        if (m_NvapiDisabled || nowUs - m_StartUs < WarmupUs) {
            return false;
        }
        return !m_NvapiPolled || nowUs - m_LastNvapiPollUs >= NvapiPollUs;
    }

    // Returns true when the verdict changed
    bool observeNvapi(uint64_t nowUs, GsyncState state)
    {
        anchorStart(nowUs);
        m_NvapiPolled = true;
        m_LastNvapiPollUs = nowUs;
        if (state == GsyncState::Unavailable) {
            m_NvapiDisabled = true;
            return false;
        }
        if (state == GsyncState::Error) {
            // The current state must hold for a full run again
            m_NvapiRunValid = false;
            if (++m_NvapiErrors >= MaxNvapiErrors) {
                m_NvapiDisabled = true;
                // Let the cadence heuristic take over
                m_NvapiAnswered = false;
            }
            return false;
        }

        m_NvapiErrors = 0;
        m_NvapiAnswered = true;
        if (!m_NvapiRunValid || state != m_NvapiRunState) {
            m_NvapiRunValid = true;
            m_NvapiRunState = state;
            m_NvapiRunStartUs = nowUs;
        }
        if (nowUs - m_NvapiRunStartUs < NvapiStableUs) {
            return false;
        }
        return setVerdict(state == GsyncState::Active ? Verdict::Engaged : Verdict::NotEngaged,
                          Source::Nvapi);
    }

    // One DXGI_FRAME_STATISTICS sample. valid is false when the query failed
    // (DXGI_ERROR_FRAME_STATISTICS_DISJOINT and similar). Returns true when
    // the verdict changed.
    bool observeFrameStatistics(uint64_t nowUs, bool valid, uint32_t presentCount,
                                uint32_t syncRefreshCount, uint64_t syncTimeUs)
    {
        anchorStart(nowUs);
        if (m_NvapiAnswered || !m_Config.vblankVirtualizationDisabled ||
            !(m_Config.displayHz > 0) || nowUs - m_StartUs < WarmupUs) {
            return false;
        }
        if (!valid || syncTimeUs == 0) {
            m_CadenceAnchorValid = false;
            return false;
        }
        if (!m_CadenceAnchorValid || syncTimeUs < m_AnchorSyncUs) {
            anchorCadence(presentCount, syncRefreshCount, syncTimeUs);
            return false;
        }

        const uint64_t elapsedUs = syncTimeUs - m_AnchorSyncUs;
        const int64_t presents = static_cast<int64_t>(presentCount) - m_AnchorPresentCount;
        const int64_t refreshes = static_cast<int64_t>(syncRefreshCount) - m_AnchorRefreshCount;
        // Counters restart with a new swapchain; a long hole is not one window
        if (presents < 0 || refreshes < 0 || elapsedUs > 2 * CadenceWindowUs) {
            anchorCadence(presentCount, syncRefreshCount, syncTimeUs);
            return false;
        }
        if (elapsedUs < CadenceWindowUs) {
            return false;
        }

        const double seconds = static_cast<double>(elapsedUs) / 1000000.0;
        m_VblankHz = static_cast<double>(refreshes) / seconds;
        m_PresentHz = static_cast<double>(presents) / seconds;
        anchorCadence(presentCount, syncRefreshCount, syncTimeUs);

        const Verdict verdict = classifyCadence(m_VblankHz, m_PresentHz, m_Config.displayHz);
        if (verdict == Verdict::Unknown) {
            return false;
        }
        return setVerdict(verdict, Source::VblankCadence);
    }

    static Verdict classifyCadence(double vblankHz, double presentHz, double displayHz)
    {
        if (!(presentHz > 0) || !(vblankHz > 0) || !(displayHz > 0) ||
            presentHz >= HeadroomRatio * displayHz) {
            return Verdict::Unknown;
        }
        if (std::fabs(vblankHz - presentHz) <= MatchTolerance * presentHz) {
            return Verdict::Engaged;
        }

        const double ratio = vblankHz / presentHz;
        if (ratio < FixedRatioMinimum ||
            std::fabs(vblankHz - displayHz) > RefreshTolerance * displayHz) {
            // Includes a stall with VRR engaged: the panel's own refreshes
            // run at its minimum rate, not at the refresh rate
            return Verdict::Unknown;
        }
        // Every 2nd or 3rd vblank on a fixed panel looks like low framerate
        // compensation on a VRR panel
        const double multiple = std::round(ratio);
        if (multiple >= 2 && std::fabs(ratio - multiple) <= MatchTolerance * multiple) {
            return Verdict::Unknown;
        }
        return Verdict::NotEngaged;
    }

    // Whether a renderer should tell the session about the current verdict:
    // in VRR mode when VRR is not engaged, in the fixed-refresh fallback when
    // NVAPI sees G-Sync active again
    static bool noticeWanted(bool fallbackMode, Verdict verdict, Source source)
    {
        if (fallbackMode) {
            return verdict == Verdict::Engaged && source == Source::Nvapi;
        }
        return verdict == Verdict::NotEngaged;
    }

    Verdict verdict() const { return m_Verdict; }
    Source source() const { return m_Source; }
    // False once NVAPI has answered, or when the cadence can't be read at all
    bool wantsFrameStatistics() const
    {
        return !m_NvapiAnswered && m_Config.vblankVirtualizationDisabled && m_Config.displayHz > 0;
    }
    // Rates measured over the last complete cadence window
    double vblankHz() const { return m_VblankHz; }
    double presentHz() const { return m_PresentHz; }

private:
    void anchorStart(uint64_t nowUs)
    {
        if (!m_Started) {
            m_Started = true;
            m_StartUs = nowUs;
        }
    }

    void anchorCadence(uint32_t presentCount, uint32_t syncRefreshCount, uint64_t syncTimeUs)
    {
        m_CadenceAnchorValid = true;
        m_AnchorPresentCount = presentCount;
        m_AnchorRefreshCount = syncRefreshCount;
        m_AnchorSyncUs = syncTimeUs;
    }

    bool setVerdict(Verdict verdict, Source source)
    {
        const bool changed = verdict != m_Verdict;
        m_Verdict = verdict;
        m_Source = source;
        return changed;
    }

    Config m_Config;
    Verdict m_Verdict = Verdict::Unknown;
    Source m_Source = Source::None;
    double m_VblankHz = 0;
    double m_PresentHz = 0;

    bool m_Started = false;
    uint64_t m_StartUs = 0;

    bool m_NvapiDisabled = false;
    int m_NvapiErrors = 0;
    bool m_NvapiPolled = false;
    bool m_NvapiAnswered = false;
    uint64_t m_LastNvapiPollUs = 0;
    bool m_NvapiRunValid = false;
    GsyncState m_NvapiRunState = GsyncState::Unavailable;
    uint64_t m_NvapiRunStartUs = 0;

    bool m_CadenceAnchorValid = false;
    int64_t m_AnchorPresentCount = 0;
    int64_t m_AnchorRefreshCount = 0;
    uint64_t m_AnchorSyncUs = 0;
};

// Session-side policy for the detector's notices. NVAPI is authoritative:
// G-Sync inactive moves the session to fixed-refresh pacing and G-Sync active
// moves it back, each at most once per dwell period, and after two
// switch-backs the session stays on fixed-refresh pacing. The vblank-cadence
// heuristic is unverified on VRR hardware, so it only warns, once.
class VrrFallbackController
{
public:
    enum class Action
    {
        None,
        EnterFallback,
        ExitFallback,
        WarnHeuristic,
        RetryReset,
    };

    static constexpr uint64_t MinDwellUs = 10000000;
    static constexpr int MaxSwitchBacks = 2;

    Action onNotice(uint64_t nowUs, VrrEngagementDetector::Verdict verdict,
                    VrrEngagementDetector::Source source)
    {
        using Verdict = VrrEngagementDetector::Verdict;
        using Source = VrrEngagementDetector::Source;

        // Any notice means the renderer is still in its old mode
        if (m_ResetPending) {
            m_ResetPending = false;
            markTransition(nowUs);
            return Action::RetryReset;
        }
        if (source == Source::VblankCadence) {
            if (verdict == Verdict::NotEngaged && !m_InFallback && !m_HeuristicWarned) {
                m_HeuristicWarned = true;
                return Action::WarnHeuristic;
            }
            return Action::None;
        }
        if (source != Source::Nvapi) {
            return Action::None;
        }

        const bool dwellOver = !m_Transitioned || nowUs - m_LastTransitionUs >= MinDwellUs;
        if (verdict == Verdict::NotEngaged && !m_InFallback && dwellOver) {
            m_InFallback = true;
            markTransition(nowUs);
            return Action::EnterFallback;
        }
        if (verdict == Verdict::Engaged && m_InFallback && dwellOver &&
            m_SwitchBacks < MaxSwitchBacks) {
            m_InFallback = false;
            m_SwitchBacks++;
            markTransition(nowUs);
            return Action::ExitFallback;
        }
        return Action::None;
    }

    bool inFallback() const { return m_InFallback; }

    // The session couldn't queue the renderer reset for the last transition:
    // the next notice asks for it again
    void resetFailed() { m_ResetPending = true; }

private:
    void markTransition(uint64_t nowUs)
    {
        m_Transitioned = true;
        m_LastTransitionUs = nowUs;
    }

    bool m_InFallback = false;
    bool m_ResetPending = false;
    bool m_HeuristicWarned = false;
    bool m_Transitioned = false;
    uint64_t m_LastTransitionUs = 0;
    int m_SwitchBacks = 0;
};
