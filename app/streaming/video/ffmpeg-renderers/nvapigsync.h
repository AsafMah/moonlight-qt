#pragma once

#include "pacer/vrrengagementdetector.h"

#include <d3d11.h>
#include <dxgi.h>

// Asks the NVIDIA driver whether G-Sync is active for a swapchain, through
// NvAPI_D3D_IsGSyncActive. nvapi64.dll is loaded on first use, never linked:
// without an NVIDIA driver the answer is Unavailable, and a failed query is
// Error (the detector retries it a few times).
// The query is documented as only valid after the first present and as
// too slow to call every frame.
namespace NvapiGsync {

VrrEngagementDetector::GsyncState query(ID3D11Device* device, IDXGISwapChain* swapChain);

}
