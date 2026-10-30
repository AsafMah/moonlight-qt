// Ported from Nonary/moonlight-qt (https://github.com/Nonary/moonlight-qt),
// commit c822745 ("Merge VRR policy cleanup into master").
// Original work Copyright (C) Chase Payne (Nonary) and the Moonlight
// contributors, distributed under the GNU General Public License version 3.
// MODIFIED for AsafMah/moonlight-qt on 2026-10-30: imported as part of the Nonary VRR pacing port; see git history for local changes.

#pragma once

#include <cstdint>

namespace Vrr13 {

// A successful native query does not establish the meaning of its timestamp.
// In particular, a DXGI refresh reference can precede the associated Present.
enum class PresentationTimeKind : uint8_t {
    Unavailable = 0,
    RefreshReference = 1,
    DisplayEvent = 2,
};

}
