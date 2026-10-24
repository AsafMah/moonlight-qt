#pragma once

#include <QCoreApplication>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <cstdint>

// Where a PyroWave frame or setup failed. Recovery and the error shown when
// it gives up both name the stage (client contract, section 6).
enum class PyrowaveStage
{
    Create,
    Decode,
    Sync,
    Color,
    Present,
};

struct PyrowaveFailure
{
    PyrowaveStage stage = PyrowaveStage::Create;
    // Translated; shown to the user when the connection has to end.
    QString reason;
};

// English stage name for logs.
const char* pyrowaveStageName(PyrowaveStage stage);

#ifdef HAVE_LIBPLACEBO_VULKAN

#include "pyrowaveabi.h"
#include "pyrowavecolor.h"
#include "streaming/video/ffmpeg-renderers/plvk.h"

#include <memory>
#include <string>
#include <vector>

// The VkInstance and the window surface. Created on the main thread (macOS
// needs that for the surface) and kept while devices are rebuilt on top.
class PyrowaveVulkanInstance
{
    Q_DECLARE_TR_FUNCTIONS(PyrowaveVulkanInstance)

public:
    PyrowaveVulkanInstance() = default;
    ~PyrowaveVulkanInstance();
    PyrowaveVulkanInstance(const PyrowaveVulkanInstance&) = delete;
    PyrowaveVulkanInstance& operator=(const PyrowaveVulkanInstance&) = delete;

    bool initialize(SDL_Window* window, PyrowaveFailure& failure);

    SDL_Window* window() const { return m_Window; }
    pl_log log() const { return m_Log; }
    VkInstance instance() const { return m_Instance; }
    VkSurfaceKHR surface() const { return m_Surface; }
    PFN_vkGetInstanceProcAddr getProcAddr() const { return m_GetInstanceProcAddr; }
    bool hasExtension(const char* name) const;

    // pyrowave_create_device() keeps a pointer to this for the device's life.
    const VkInstanceCreateInfo* createInfo() const { return &m_CreateInfo; }

private:
    SDL_Window* m_Window = nullptr;
    pl_log m_Log = nullptr;
    PFN_vkGetInstanceProcAddr m_GetInstanceProcAddr = nullptr;
    PFN_vkDestroyInstance m_DestroyInstance = nullptr;
    PFN_vkDestroySurfaceKHR m_DestroySurface = nullptr;

    std::vector<const char*> m_Extensions;
    VkApplicationInfo m_AppInfo = {};
    VkInstanceCreateInfo m_CreateInfo = {};
    VkInstance m_Instance = VK_NULL_HANDLE;
    VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
};

struct PyrowavePresentParams
{
    int width = 0;
    int height = 0;
    // The session's YCbCr range (StreamConfig.colorRange), which the host
    // encodes with and every frame's native metadata must match.
    bool fullRange = false;
    // 0 = SDR, 1 = HDR10, 2 = HLG (StreamConfig.hdrMode). Fixed for the
    // stream: HDR planes, color checks and swapchain follow it.
    int hdrMode = 0;
    bool enableVsync = false;
    bool ignoreAspectRatio = false;
};

// Client-thread time spent on one frame (decoder design, section 8). Neither
// is pure GPU time.
struct PyrowaveFrameTimings
{
    // Push, ready check, color check and the GPU decode submission.
    uint64_t decodeUs = 0;
    // Swapchain acquire, YUV conversion and overlays, and the present call.
    uint64_t renderUs = 0;
};

// One VkDevice shared by the PyroWave decoder and libplacebo, the decoded
// Y/Cb/Cr planes, and presentation to the window through a libplacebo
// swapchain. A failed frame throws the whole object away and builds a new
// one on the same PyrowaveVulkanInstance (see PyrowaveVideoDecoder).
//
// submitFrame() runs on the decode thread; notifyOverlayUpdated() on the
// overlay thread. The caller serializes them, and both with destruction.
class PyrowaveVulkanRenderer
{
    Q_DECLARE_TR_FUNCTIONS(PyrowaveVulkanRenderer)

public:
    enum class FrameResult
    {
        // Decoded and presented.
        Presented,
        // Incomplete frame: the decoder and the last shown image are kept.
        Dropped,
        // The window has no swapchain image right now (occluded, minimized,
        // resizing). Not a success, and not a failure either.
        NotPresented,
        // See the failure argument. The renderer must be rebuilt.
        Failed,
    };

    explicit PyrowaveVulkanRenderer(PyrowaveVulkanInstance& instance);
    ~PyrowaveVulkanRenderer();
    PyrowaveVulkanRenderer(const PyrowaveVulkanRenderer&) = delete;
    PyrowaveVulkanRenderer& operator=(const PyrowaveVulkanRenderer&) = delete;

    bool initialize(const PyrowavePresentParams& params, PyrowaveFailure& failure);

    FrameResult submitFrame(const uint8_t* data, size_t length, PyrowaveFrameTimings& timings,
                            PyrowaveFailure& failure);

    void notifyOverlayUpdated(Overlay::OverlayType type);

    // The session's static HDR metadata. Applied to the frame and the
    // swapchain from the next present on. Ignored for SDR.
    void setHdrSnapshot(const PyrowaveColor::HdrSnapshot& snapshot);

    const std::string& deviceName() const { return m_DeviceName; }

    // For the stats overlay, e.g. "PyroWave (Vulkan compute)". English.
    QString decoderDescription() const;

private:
    struct DeviceSetup;
    struct Plane
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        pl_tex tex = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    static void lockQueue(void* context);
    static void unlockQueue(void* context);

    bool loadEntryPoints(PyrowaveFailure& failure);
    bool createDevice(PyrowaveFailure& failure);
    void checkHdrOutput(VkPhysicalDevice device,
                        const std::vector<VkExtensionProperties>& extensions, QStringList& missing);
    bool createLogicalDevice(VkPhysicalDevice device, uint32_t queueFamily, QString& reason);
    bool createPyrowaveDecoder(PyrowaveFailure& failure);
    bool createPlanes(PyrowaveFailure& failure);
    bool createPlane(Plane& plane, uint32_t width, uint32_t height, VkImageUsageFlags usage);
    bool createPresentation(PyrowaveFailure& failure);
    void hintSwapchainColorspace();
    bool checkColorMetadata(PyrowaveFailure& failure);
    bool decodeToPlanes(PyrowaveFailure& failure);
    FrameResult present(const pl_swapchain_frame& swapchainFrame, bool decoded,
                        PyrowaveFailure& failure);
    void destroy();

    PyrowaveVulkanInstance& m_Instance;
    PyrowavePresentParams m_Params;
    PyrowaveColor::Contract m_Contract{ 0, false };
    PyrowaveColor::PlaneEncoding m_PlaneEncoding;
    VkFormat m_PlaneFormat = VK_FORMAT_R8_UNORM;

    // Vulkan functions we call directly
    PFN_vkGetPhysicalDeviceProperties m_GetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 m_GetPhysicalDeviceFeatures2 = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties m_GetQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR m_GetSurfaceSupport = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR m_GetSurfacePresentModes = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR m_GetSurfaceFormats = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties m_GetFormatProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties m_GetMemoryProperties = nullptr;
    PFN_vkEnumeratePhysicalDevices m_EnumeratePhysicalDevices = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties m_EnumerateDeviceExtensions = nullptr;
    PFN_vkCreateDevice m_CreateDevice = nullptr;
    PFN_vkGetDeviceProcAddr m_GetDeviceProcAddr = nullptr;
    PFN_vkDestroyDevice m_DestroyDevice = nullptr;
    PFN_vkGetDeviceQueue m_GetDeviceQueue = nullptr;
    PFN_vkDeviceWaitIdle m_DeviceWaitIdle = nullptr;
    PFN_vkCreateImage m_CreateImage = nullptr;
    PFN_vkDestroyImage m_DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements m_GetImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory m_AllocateMemory = nullptr;
    PFN_vkFreeMemory m_FreeMemory = nullptr;
    PFN_vkBindImageMemory m_BindImageMemory = nullptr;

    // PyroWave entry points (PyrowaveRuntime checked that they all exist)
    PyrowaveAbi::PFN_pyrowave_create_device m_PwCreateDevice = nullptr;
    PyrowaveAbi::PFN_pyrowave_device_destroy m_PwDestroyDevice = nullptr;
    PyrowaveAbi::PFN_pyrowave_device_set_queue_type m_PwSetQueueType = nullptr;
    PyrowaveAbi::PFN_pyrowave_decoder_device_prefers_fragment_path m_PwPrefersFragment = nullptr;
    PyrowaveAbi::PFN_pyrowave_decoder_create m_PwCreateDecoder = nullptr;
    PyrowaveAbi::PFN_pyrowave_decoder_clear m_PwClearDecoder = nullptr;
    PyrowaveAbi::PFN_pyrowave_decoder_push_packet m_PwPushPacket = nullptr;
    PyrowaveAbi::PFN_pyrowave_decoder_decode_is_ready m_PwDecodeIsReady = nullptr;
    PyrowaveAbi::PFN_pyrowave_decoder_decode_gpu_buffer m_PwDecodeGpu = nullptr;
    PyrowaveAbi::PFN_pyrowave_decoder_get_color_metadata m_PwGetColorMetadata = nullptr;
    PyrowaveAbi::PFN_pyrowave_decoder_destroy m_PwDestroyDecoder = nullptr;

    // The shared device. The create infos stay alive for pyrowave_device.
    std::unique_ptr<DeviceSetup> m_Setup;
    VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
    std::string m_DeviceName;
    uint32_t m_QueueFamily = 0;
    VkDevice m_Device = VK_NULL_HANDLE;
    VkQueue m_Queue = VK_NULL_HANDLE;
    pl_vulkan m_Vulkan = nullptr;

    // PyroWave
    PyrowaveAbi::pyrowave_device m_PyroDevice = nullptr;
    PyrowaveAbi::pyrowave_decoder m_Decoder = nullptr;
    bool m_FragmentPath = false;
    uint32_t m_ChromaSiting = PyrowaveAbi::PYROWAVE_CHROMA_SITING_CENTER;
    bool m_HasDecodedFrame = false;

    // Y, Cb and Cr. Owned by libplacebo between frames and held by us while
    // PyroWave writes them, with one timeline semaphore ordering the handoffs.
    Plane m_Planes[3];
    VkSemaphore m_PlaneSemaphore = VK_NULL_HANDLE;
    uint64_t m_PlaneTimeline = 0;

    // Presentation
    VkPresentModeKHR m_PresentMode = VK_PRESENT_MODE_FIFO_KHR;
    pl_swapchain m_Swapchain = nullptr;
    pl_renderer m_Renderer = nullptr;
    PlVkOverlays m_Overlays;

    // HDR presentation. The swapchain is hinted with the frames' color space
    // (static metadata included) whenever that changes, like PlVkRenderer.
    PyrowaveColor::HdrSnapshot m_HdrSnapshot;
    pl_color_space m_FrameColor = {};
    pl_color_space m_HintedColor = {};
    bool m_LoggedSdrTarget = false;
};

#endif
