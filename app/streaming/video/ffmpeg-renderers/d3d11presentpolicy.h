// Ported from Nonary/moonlight-qt (https://github.com/Nonary/moonlight-qt),
// commit c822745 ("Merge VRR policy cleanup into master").
// Original work Copyright (C) Chase Payne (Nonary) and the Moonlight
// contributors, distributed under the GNU General Public License version 3.
// MODIFIED for AsafMah/moonlight-qt on 2026-10-30: imported as part of the Nonary VRR pacing port;
// merged into the fork's own D3D11PresentPolicy header (frame latency
// constants for the legacy present path), which shares this path and namespace.

#pragma once

#include <cstdint>

namespace D3D11PresentPolicy {

// --- Fork: legacy (non-Nonary) present path ---

// Frames DXGI may hold for display before the next one has to wait. With the
// default of 3, a window DWM drains slower than frames arrive keeps three
// frames queued in DXGI and blocks every Present() for a refresh.
constexpr unsigned int MaxFrameLatency = 1;

// Whether the renderer waits on the swapchain's frame latency waitable object
// in waitToRender(), so the Pacer only picks a frame once the swapchain can
// take it without blocking, and then picks the newest one.
constexpr bool WaitBeforeLatch = true;

// --- Nonary VRR presenter ---


// A completion observed after releasing the renderer mutex belongs only to
// that exact prepared marker. Cancellation or replacement invalidates it.
class PreparedCompletion
{
public:
    void begin(uint64_t value) { m_Value = value; m_Ready = false; }
    void clear() { begin(0); }
    bool complete(uint64_t value, bool success)
    {
        if (success && value != 0 && value == m_Value) m_Ready = true;
        return ready(value);
    }
    bool ready(uint64_t value) const
    {
        return m_Ready && value != 0 && value == m_Value;
    }

private:
    uint64_t m_Value = 0;
    bool m_Ready = false;
};

struct FlipObservation
{
    bool predecessorKnown = false;
    bool statisticsValid = false;
    bool pending = false;
    bool rasterValid = false;
    bool inVerticalBlank = false;
    uint64_t rasterQueryStartUs = 0;
    uint64_t rasterQueryEndUs = 0;
};

// A retired predecessor can still be scanning out. Unknown or active scanout
// must not authorize a tearing flip. A fresh blank is a risk filter, not a
// reservation of the phase when a later asynchronous flip reaches the panel.
template<class ObservationQuery, class Clock>
bool latch(bool plannedLatched, bool synchronizeAll, uint64_t protectionWindowUs,
           ObservationQuery&& observationQuery, Clock&& clock)
{
    if (plannedLatched || synchronizeAll) return true;
    if (!protectionWindowUs) return false; // Explicitly disabled protection.
    const FlipObservation observation = observationQuery();
    if (!observation.predecessorKnown || !observation.statisticsValid ||
            observation.pending || !observation.rasterValid ||
            !observation.inVerticalBlank) return true;
    const uint64_t nowUs = clock();
    // Bound the entire observation age, including any preemption after the
    // query. This conservative budget does not claim a remaining blank time.
    const uint64_t precisionUs = protectionWindowUs / 16 < 250 ?
        protectionWindowUs / 16 : 250;
    return !precisionUs || !observation.rasterQueryStartUs ||
        observation.rasterQueryEndUs < observation.rasterQueryStartUs ||
        nowUs < observation.rasterQueryEndUs ||
        nowUs - observation.rasterQueryStartUs > precisionUs;
}

}
