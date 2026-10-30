// Ported from Nonary/moonlight-qt (https://github.com/Nonary/moonlight-qt),
// commit c822745 ("Merge VRR policy cleanup into master").
// Original work Copyright (C) Chase Payne (Nonary) and the Moonlight
// contributors, distributed under the GNU General Public License version 3.
// MODIFIED for AsafMah/moonlight-qt on 2026-10-30: imported as part of the Nonary VRR pacing port; see git history for local changes.

#pragma once

// Keep the reported parameters and the native call together. This boundary is
// independent of the Windows SDK so a fake swapchain can verify the arguments.
struct DxgiPresentParameters
{
    unsigned int syncInterval;
    unsigned int flags;

    static constexpr DxgiPresentParameters adaptive(bool latched,
                                                    unsigned int tearingFlag)
    {
        return latched ? DxgiPresentParameters{1, 0} :
                         DxgiPresentParameters{0, tearingFlag};
    }

    template<typename SwapChain>
    auto present(SwapChain& swapChain) const
    {
        return swapChain.Present(syncInterval, flags);
    }
};
