#pragma once

// How the D3D11 renderer queues presents. Pure constants so the present
// queue simulation in tests/dxgi_present_queue uses the same values.
namespace D3D11PresentPolicy {

// Frames DXGI may hold for display before the next one has to wait. With the
// default of 3, a window DWM drains slower than frames arrive keeps three
// frames queued in DXGI and blocks every Present() for a refresh.
constexpr unsigned int MaxFrameLatency = 1;

// Whether the renderer waits on the swapchain's frame latency waitable object
// in waitToRender(), so the Pacer only picks a frame once the swapchain can
// take it without blocking, and then picks the newest one.
constexpr bool WaitBeforeLatch = true;

} // namespace D3D11PresentPolicy
