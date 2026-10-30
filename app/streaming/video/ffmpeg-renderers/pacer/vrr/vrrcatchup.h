// Ported from Nonary/moonlight-qt (https://github.com/Nonary/moonlight-qt),
// commit c822745 ("Merge VRR policy cleanup into master").
// Original work Copyright (C) Chase Payne (Nonary) and the Moonlight
// contributors, distributed under the GNU General Public License version 3.
// MODIFIED for AsafMah/moonlight-qt on 2026-10-30: imported as part of the Nonary VRR pacing port; see git history for local changes.

#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace VrrCatchUp {
// A soft cadence floor, not a new source clock or extra buffer demand. Start
// with gentle recovery, then continuously use more available display headroom
// as replaceable queue age approaches the existing two-period stale limit.
inline uint64_t floorUs(uint64_t lastSubmission, uint64_t sourcePeriod,
                        uint64_t displayFloorPeriod, uint64_t queueAge,
                        uint64_t recoveryPerMille)
{
    if (!lastSubmission || !sourcePeriod || !displayFloorPeriod ||
            displayFloorPeriod >= sourcePeriod || !recoveryPerMille ||
            sourcePeriod > 1000000)
        return 0;
    const uint64_t headroom = sourcePeriod - displayFloorPeriod;
    const uint64_t gentle = std::min(headroom,
        sourcePeriod * std::min<uint64_t>(recoveryPerMille, 1000) / 1000);
    const uint64_t pressure = queueAge > sourcePeriod ?
        std::min(sourcePeriod, queueAge - sourcePeriod) : 0;
    const uint64_t recovery = gentle + (headroom - gentle) * pressure / sourcePeriod;
    const uint64_t interval = sourcePeriod - recovery;
    return lastSubmission > std::numeric_limits<uint64_t>::max() - interval ?
        std::numeric_limits<uint64_t>::max() : lastSubmission + interval;
}
}
