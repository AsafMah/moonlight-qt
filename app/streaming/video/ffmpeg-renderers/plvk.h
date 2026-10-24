#pragma once

#include "renderer.h"

#include <atomic>

#ifdef Q_OS_WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>

#include <functional>
#include <vector>

#ifdef HAVE_DRM_MASTER_HOOKS
extern "C" {
void lockDrmMaster();
void unlockDrmMaster();
}
#endif

// Many operations like setting a display mode or creating a swapchain
// may require the Vulkan implementation to have DRM master in a KMSDRM
// environment. Since this will not necessarily be the case during decoder
// probing (when the Qt UI is still rendering), we need to grab the DRM
// master lock to prevent Qt from taking it out from under us.
class DrmMasterLocker
{
public:
    DrmMasterLocker()
    {
#ifdef HAVE_DRM_MASTER_HOOKS
        lockDrmMaster();
#endif
    }

    ~DrmMasterLocker()
    {
#ifdef HAVE_DRM_MASTER_HOOKS
        unlockDrmMaster();
#endif
    }

    // Disallow copies and moves
    DrmMasterLocker(const DrmMasterLocker&) = delete;
    DrmMasterLocker& operator=(const DrmMasterLocker&) = delete;
    DrmMasterLocker(DrmMasterLocker&&) noexcept = delete;
    DrmMasterLocker& operator=(DrmMasterLocker&&) noexcept = delete;
};

// Vulkan setup shared by PlVkRenderer and the PyroWave decoder, which creates
// its own VkDevice and imports it into libplacebo (see
// streaming/video/pyrowave/pyrowavevulkan.h). Each helper does what
// PlVkRenderer used to do inline, including its logging.
namespace PlVkShared {

// A libplacebo log routed to SDL logging. PLVK_LOG_LEVEL overrides the level.
pl_log createLog();

// The loader entry point SDL loaded for SDL_WINDOW_VULKAN windows.
PFN_vkGetInstanceProcAddr getInstanceProcAddr();

// The instance extensions SDL needs to create a surface for the window.
bool getInstanceExtensions(SDL_Window* window, std::vector<const char*>& extensions);

// Creates a surface for the window, holding DRM master while doing so.
bool createWindowSurface(SDL_Window* window, VkInstance instance, VkSurfaceKHR* surface);

void getDrawableSize(SDL_Window* window, int* width, int* height);

// Every physical device in the order to try them: the first one (device
// selection layers put the preferred GPU there), then integrated GPUs, then
// discrete GPUs, then everything else.
std::vector<VkPhysicalDevice>
devicesInPreferenceOrder(VkInstance instance, PFN_vkEnumeratePhysicalDevices enumerate,
                         PFN_vkGetPhysicalDeviceProperties getProperties);

// True (and logged) for software renderers unless PLVK_ALLOW_SOFTWARE=1.
bool isSoftwareDeviceRejected(const VkPhysicalDeviceProperties& properties);

// FIFO with V-Sync. Without it, the first supported of Immediate, FIFO
// Relaxed and Mailbox, falling back to FIFO.
VkPresentModeKHR choosePresentMode(bool enableVsync,
                                   const std::function<bool(VkPresentModeKHR)>& isSupported);

// Logs and returns null on failure. Holds DRM master while creating.
pl_swapchain createSwapchain(pl_vulkan vulkan, VkSurfaceKHR surface, VkPresentModeKHR presentMode,
                             int depth);

}

// The overlay textures a libplacebo renderer draws over the video, and the
// handoff from the overlay update thread to the render thread.
class PlVkOverlays
{
public:
    // Takes ownership of surface in all cases!
    static bool createOverlay(pl_gpu gpu, pl_overlay* overlay, SDL_Surface* surface);

    // Overlay update thread
    void notifyUpdated(pl_gpu gpu, Overlay::OverlayType type);

    // Render thread: picks up staged overlays and positions every enabled one
    // in targetFrame. parts must outlive the render call. Textures that are no
    // longer used are returned for the caller to destroy after rendering.
    void collect(const pl_frame& targetFrame, pl_overlay_part (&parts)[Overlay::OverlayMax],
                 std::vector<pl_overlay>& overlays, std::vector<pl_tex>& texturesToDestroy);

    // Destroys every overlay texture. Call before destroying the pl_vulkan.
    void destroy(pl_gpu gpu);

private:
    static void uploadComplete(void* opaque);

    SDL_SpinLock m_Lock = 0;
    struct
    {
        // The staging overlay state is copied here under the overlay lock in the render thread.
        //
        // These values can be safely read by the render thread outside of the overlay lock,
        // but the copy from stagingOverlay to overlay must only happen under the overlay
        // lock when hasStagingOverlay is true.
        bool hasOverlay;
        pl_overlay overlay;

        // This state is written by the overlay update thread
        //
        // NB: hasStagingOverlay may be false even if there is a staging overlay texture present,
        // because this is how the overlay update path indicates that the overlay is not currently
        // safe for the render thread to read.
        //
        // It is safe for the overlay update thread to write to stagingOverlay outside of the lock,
        // as long as hasStagingOverlay is false.
        bool hasStagingOverlay;
        pl_overlay stagingOverlay;
    } m_Overlays[Overlay::OverlayMax] = {};
};

#ifdef Q_OS_DARWIN
class MetalVulkanTextureFactory {
public:
    MetalVulkanTextureFactory(pl_vulkan vulkan);
    ~MetalVulkanTextureFactory();

    bool mapVideoToolboxToPlacebo(const AVFrame *frame, pl_frame* mappedFrame);
    void unmapVideoToolboxFromPlacebo(pl_frame* mappedFrame);

private:
    pl_vulkan m_Vulkan;
    /* CVMetalTextureCacheRef */ void* m_TextureCache = nullptr;
};

// Work around direct-to-display mode sometimes (but not always!) blocking us
// from getting a new drawable while the current one is getting scanned out.
#define PLVK_USE_DYNAMIC_SWAPCHAIN_DEPTH 1

// MoltenVK will block for the next drawable when we render the next frame
// rather than inside pl_swapchain_start_frame(), so we will force it to wait
// by rendering some no-op work right after we get the new swapchain frame.
#define PLVK_USE_EARLY_RENDER_TO_WAIT 1

#endif

class PlVkRenderer : public IFFmpegRenderer {
public:
    PlVkRenderer(AVHWDeviceType hwDeviceType = AV_HWDEVICE_TYPE_NONE, IFFmpegRenderer *backendRenderer = nullptr);
    virtual ~PlVkRenderer() override;
    virtual bool initialize(PDECODER_PARAMETERS params) override;
    virtual bool prepareDecoderContext(AVCodecContext* context, AVDictionary** options) override;
    virtual void renderFrame(AVFrame* frame) override;
    virtual bool testRenderFrame(AVFrame* frame) override;
    virtual void waitToRender() override;
    virtual void cleanupRenderContext() override;
    virtual void notifyOverlayUpdated(Overlay::OverlayType) override;
    virtual bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO) override;
    virtual int getRendererAttributes() override;
    virtual int getDecoderColorspace() override;
    virtual int getDecoderColorRange() override;
    virtual int getDecoderCapabilities() override;
    virtual bool isPixelFormatSupported(int videoFormat, enum AVPixelFormat pixelFormat) override;
    virtual AVPixelFormat getPreferredPixelFormat(int videoFormat) override;
    virtual ToneMappingSource getActiveToneMappingSource() override;

private:
    static void lockQueue(AVHWDeviceContext *dev_ctx, uint32_t queue_family, uint32_t index);
    static void unlockQueue(AVHWDeviceContext* dev_ctx, uint32_t queue_family, uint32_t index);

    void beginRenderTiming();
    void endRenderTiming();

    bool createSwapchain(int depth);
    bool mapAvFrameToPlacebo(const AVFrame *frame, pl_frame* mappedFrame);
    void unmapAvFrameFromPlacebo(const AVFrame *frame, pl_frame* mappedFrame);
    bool populateQueues(int videoFormat);
    bool chooseVulkanDevice(PDECODER_PARAMETERS params, bool hdrOutputRequired);
    bool tryInitializeDevice(VkPhysicalDevice device, VkPhysicalDeviceProperties* deviceProps,
                             PDECODER_PARAMETERS decoderParams, bool hdrOutputRequired);
    bool isExtensionSupportedByPhysicalDevice(VkPhysicalDevice device, const char* extensionName);
    bool isPresentModeSupportedByPhysicalDevice(VkPhysicalDevice device, VkPresentModeKHR presentMode);
    bool isColorSpaceSupportedByPhysicalDevice(VkPhysicalDevice device, VkColorSpaceKHR colorSpace);
    bool isSurfacePresentationSupportedByPhysicalDevice(VkPhysicalDevice device);

    // The backend renderer if we're frontend-only
    IFFmpegRenderer* m_Backend;
    AVHWDeviceType m_HwDeviceType;

#ifdef Q_OS_DARWIN
    std::unique_ptr<MetalVulkanTextureFactory> m_MetalTextureFactory;
#endif

#ifdef PLVK_USE_DYNAMIC_SWAPCHAIN_DEPTH
    int m_DelayedPresents = 0;
    Uint32 m_RenderStartTime = 0;
#endif

    // SDL state
    SDL_Window* m_Window = nullptr;

    // Stream state
    int m_MaxVideoFps;

    // The libplacebo rendering state
    pl_log m_Log = nullptr;
    pl_vk_inst m_PlVkInstance = nullptr;
    VkSurfaceKHR m_VkSurface = VK_NULL_HANDLE;
    int m_SwapchainDepth = 0;
    VkPresentModeKHR m_VkPresentMode;
    pl_vulkan m_Vulkan = nullptr;
    pl_swapchain m_Swapchain = nullptr;
    pl_renderer m_Renderer = nullptr;
    pl_tex m_Textures[PL_MAX_PLANES] = {};
    pl_color_space m_LastColorspace = {};

    // What the last rendered frame actually tone mapped against. Written on the render
    // thread in renderFrame(), read on the receive thread by the stats overlay and the
    // HDR10+ log, so it has to be atomic. Purely diagnostic, hence relaxed ordering.
    //
    // Starts at Sdr rather than Unsupported: once plvk is the frontend renderer it is
    // a renderer that tone maps, even before the first frame goes through.
    std::atomic<ToneMappingSource> m_ToneMappingSource { ToneMappingSource::Sdr };

#ifdef PLVK_USE_EARLY_RENDER_TO_WAIT
    pl_overlay m_EmptyOverlay = {};
    pl_overlay_part m_EmptyOverlayPart = {};
#endif

    // Pending swapchain state shared between waitToRender(), renderFrame(), and cleanupRenderContext()
    pl_swapchain_frame m_SwapchainFrame = {};
    bool m_HasPendingSwapchainFrame = false;
    
    bool m_IgnoreAspectRatio = false;

    // Overlay state
    PlVkOverlays m_Overlays;

    // Device context used for hwaccel decoders
    AVBufferRef* m_HwDeviceCtx = nullptr;

    // Vulkan functions we call directly
    PFN_vkDestroySurfaceKHR fn_vkDestroySurfaceKHR = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties2 fn_vkGetPhysicalDeviceQueueFamilyProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR fn_vkGetPhysicalDeviceSurfacePresentModesKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR fn_vkGetPhysicalDeviceSurfaceFormatsKHR = nullptr;
    PFN_vkEnumeratePhysicalDevices fn_vkEnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties fn_vkGetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR fn_vkGetPhysicalDeviceSurfaceSupportKHR = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties fn_vkEnumerateDeviceExtensionProperties = nullptr;
};
