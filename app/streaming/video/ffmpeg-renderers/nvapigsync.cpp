#include "nvapigsync.h"

#include <SDL.h>
#include <windows.h>
#include <wrl/client.h>

#include <mutex>

using Microsoft::WRL::ComPtr;

namespace {

// Minimal NVAPI declarations (nvapi.h, nvapi_interface.h). Every entry point
// is reached through nvapi_QueryInterface by ID and returns NvAPI_Status.
struct NvDxObjectHandleOpaque;
using NvDxObjectHandle = NvDxObjectHandleOpaque*;

using QueryInterfaceFn = void*(__cdecl*)(unsigned int id);
using InitializeFn = int(__cdecl*)();
using GetObjectHandleForResourceFn = int(__cdecl*)(IUnknown* device, IUnknown* resource,
                                                   NvDxObjectHandle* handle);
using GsyncQueryFn = int(__cdecl*)(IUnknown* deviceOrContext, NvDxObjectHandle primarySurface,
                                   BOOL* result);

constexpr unsigned int kInitializeId = 0x0150e828;
constexpr unsigned int kGetObjectHandleForResourceId = 0xfceac864;
constexpr unsigned int kIsGsyncCapableId = 0x9c1eed78;
constexpr unsigned int kIsGsyncActiveId = 0xe942b0ff;
constexpr int kNvapiOk = 0;

// Statuses that won't change for this driver and device (nvapi_lite_common.h):
// library not found, no implementation, not initialized, no NVIDIA device
// driving a display, not supported. Anything else may pass.
bool nvapiUnsupported(int status)
{
    return status == -2 || status == -3 || status == -4 || status == -6 || status == -104;
}

VrrEngagementDetector::GsyncState queryFailed(const char* call, int status)
{
    using GsyncState = VrrEngagementDetector::GsyncState;

    // Transient failures can repeat every poll
    static int s_LoggedFailures = 0;
    if (s_LoggedFailures < 10) {
        s_LoggedFailures++;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s failed: %d (0x%08x)", call, status,
                    static_cast<unsigned int>(status));
    }
    return nvapiUnsupported(status) ? GsyncState::Unavailable : GsyncState::Error;
}

struct NvapiFunctions
{
    bool ready = false;
    GetObjectHandleForResourceFn getObjectHandleForResource = nullptr;
    GsyncQueryFn isGsyncCapable = nullptr;
    GsyncQueryFn isGsyncActive = nullptr;
};

NvapiFunctions loadNvapi()
{
    NvapiFunctions functions;
#if defined(_M_X64)
    const wchar_t* dllName = L"nvapi64.dll";
#elif defined(_M_IX86)
    const wchar_t* dllName = L"nvapi.dll";
#else
    const wchar_t* dllName = nullptr;
#endif
    if (dllName == nullptr) {
        return functions;
    }

    HMODULE module = LoadLibraryExW(dllName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "NVAPI is not available (error %lu); G-Sync state cannot be queried",
                    GetLastError());
        return functions;
    }

    auto queryInterface =
        reinterpret_cast<QueryInterfaceFn>(GetProcAddress(module, "nvapi_QueryInterface"));
    if (queryInterface == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "NVAPI has no nvapi_QueryInterface export");
        FreeLibrary(module);
        return functions;
    }

    auto initialize = reinterpret_cast<InitializeFn>(queryInterface(kInitializeId));
    functions.getObjectHandleForResource = reinterpret_cast<GetObjectHandleForResourceFn>(
        queryInterface(kGetObjectHandleForResourceId));
    functions.isGsyncCapable = reinterpret_cast<GsyncQueryFn>(queryInterface(kIsGsyncCapableId));
    functions.isGsyncActive = reinterpret_cast<GsyncQueryFn>(queryInterface(kIsGsyncActiveId));
    if (initialize == nullptr || functions.getObjectHandleForResource == nullptr ||
        functions.isGsyncActive == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "NVAPI lacks the G-Sync query entry points; G-Sync state cannot be queried");
        FreeLibrary(module);
        return NvapiFunctions();
    }

    const int status = initialize();
    if (status != kNvapiOk) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "NvAPI_Initialize() failed: %d", status);
        FreeLibrary(module);
        return NvapiFunctions();
    }

    // The module stays loaded for the life of the process
    functions.ready = true;
    return functions;
}

const NvapiFunctions& nvapi()
{
    static std::once_flag s_Once;
    static NvapiFunctions s_Functions;
    std::call_once(s_Once, [] { s_Functions = loadNvapi(); });
    return s_Functions;
}

} // namespace

VrrEngagementDetector::GsyncState NvapiGsync::query(ID3D11Device* device, IDXGISwapChain* swapChain)
{
    using GsyncState = VrrEngagementDetector::GsyncState;

    if (device == nullptr || swapChain == nullptr) {
        return GsyncState::Unavailable;
    }
    const NvapiFunctions& functions = nvapi();
    if (!functions.ready) {
        return GsyncState::Unavailable;
    }

    const Uint64 startTicks = SDL_GetPerformanceCounter();
    ComPtr<ID3D11Resource> primarySurface;
    HRESULT hr = swapChain->GetBuffer(0, IID_PPV_ARGS(&primarySurface));
    if (FAILED(hr)) {
        return queryFailed("IDXGISwapChain::GetBuffer() for the G-Sync query", hr);
    }

    NvDxObjectHandle primaryHandle = nullptr;
    int status = functions.getObjectHandleForResource(device, primarySurface.Get(), &primaryHandle);
    if (status != kNvapiOk) {
        return queryFailed("NvAPI_D3D_GetObjectHandleForResource()", status);
    }

    BOOL active = FALSE;
    status = functions.isGsyncActive(device, primaryHandle, &active);
    if (status != kNvapiOk) {
        return queryFailed("NvAPI_D3D_IsGSyncActive()", status);
    }

    // One line per swapchain: capability, first answer and what the query costs
    static IDXGISwapChain* s_LoggedSwapChain = nullptr;
    if (s_LoggedSwapChain != swapChain) {
        s_LoggedSwapChain = swapChain;
        const double elapsedMs = static_cast<double>(SDL_GetPerformanceCounter() - startTicks) *
                                 1000.0 / static_cast<double>(SDL_GetPerformanceFrequency());
        BOOL capable = FALSE;
        const int capableStatus = functions.isGsyncCapable != nullptr
                                      ? functions.isGsyncCapable(device, primaryHandle, &capable)
                                      : -1;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "NVAPI G-Sync: capable=%s active=%s (query %.2f ms)",
                    capableStatus == kNvapiOk ? (capable ? "yes" : "no") : "unknown",
                    active ? "yes" : "no", elapsedMs);
    }

    return active ? GsyncState::Active : GsyncState::Inactive;
}
