// Ported from Nonary/moonlight-qt (https://github.com/Nonary/moonlight-qt),
// commit c822745 ("Merge VRR policy cleanup into master").
// Original work Copyright (C) Chase Payne (Nonary) and the Moonlight
// contributors, distributed under the GNU General Public License version 3.
// MODIFIED for AsafMah/moonlight-qt on 2026-10-30: imported as part of the Nonary VRR pacing port; see git history for local changes.

#pragma once

#include <vector>

// VRR rate selection and deterministic stream/display qualification live in
// one small policy object. It has no dependency on SDL, QSettings, or a
// renderer, so the arithmetic can be tested independently.
enum class VrrFpsChoiceKind {
    Fixed,
    Custom,
};

struct VrrFpsChoice {
    int fps;
    VrrFpsChoiceKind kind;
};

class VrrRatePolicy
{
public:
    // Below-refresh recommendation: floor(refresh - refresh^2 / 3600).
    static int vrrRateForRefresh(int refreshHz);

    // Below-refresh choice and native-presentation protection cutoff.
    // Keep floor(r - r*r/3600), including its rounding.
    static constexpr int protectedRateForRefresh(int refreshHz)
    {
        const long long numerator = static_cast<long long>(refreshHz) *
                                    (3600LL - refreshHz);
        return refreshHz > 1 && numerator > 0 ?
            static_cast<int>(numerator / 3600LL) : 0;
    }

    // Accept sources through native refresh. Per-frame presentation enforces
    // scanout safety; session admission no longer reserves a fixed FPS margin.
    static bool hasAdaptiveHeadroom(int streamRateHz, int displayRefreshHz);

    // Build baseline, below-refresh, native, and saved custom choices.
    static std::vector<VrrFpsChoice> buildChoices(const std::vector<int>& refreshRates,
                                                   int savedFps);

};
