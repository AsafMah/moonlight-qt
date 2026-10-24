#include "pyrowavevulkan.h"

const char* pyrowaveStageName(PyrowaveStage stage)
{
    switch (stage) {
    case PyrowaveStage::Create:
        return "create";
    case PyrowaveStage::Decode:
        return "decode";
    case PyrowaveStage::Sync:
        return "sync";
    case PyrowaveStage::Color:
        return "color";
    case PyrowaveStage::Present:
        return "present";
    }

    return "unknown";
}

#ifdef HAVE_LIBPLACEBO_VULKAN

#include "pyrowavecapabilities.h"
#include "pyrowaveruntime.h"
#include "streaming/streamutils.h"

#include <QStringList>

#include <cstddef>
#include <cstring>

using namespace PyrowaveAbi;

namespace {

// SDR planes (client contract, section 3). HDR (PW-3) uses R16.
constexpr VkFormat k_PlaneFormat = VK_FORMAT_R8_UNORM;

const char k_PortabilityEnumeration[] = "VK_KHR_portability_enumeration";
const char k_PortabilitySubset[] = "VK_KHR_portability_subset";
const char k_GetSurfaceCapabilities2[] = "VK_KHR_get_surface_capabilities2";
const char k_KhrSurfaceMaintenance1[] = "VK_KHR_surface_maintenance1";
const char k_ExtSurfaceMaintenance1[] = "VK_EXT_surface_maintenance1";
const char k_SwapchainColorspace[] = "VK_EXT_swapchain_colorspace";

// VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR, absent from older headers.
constexpr VkInstanceCreateFlags k_EnumeratePortabilityBit = 0x00000001;

bool hasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name)
{
    for (const VkExtensionProperties& extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

bool containsName(const std::vector<const char*>& names, const char* name)
{
    for (const char* existing : names) {
        if (std::strcmp(existing, name) == 0) {
            return true;
        }
    }
    return false;
}

void addUnique(std::vector<const char*>& names, const char* name)
{
    if (!containsName(names, name)) {
        names.push_back(name);
    }
}

QString vulkanVersionString(uint32_t version)
{
    return QStringLiteral("%1.%2")
        .arg(VK_API_VERSION_MAJOR(version))
        .arg(VK_API_VERSION_MINOR(version));
}

// The core feature structures are sType, pNext and then only VkBool32
// members, which lets them be combined field by field.
struct BoolSpan
{
    VkBool32* data;
    size_t count;
};

#define PYROWAVE_FEATURE_SPAN(object, type, first, last)                                           \
    BoolSpan{ &(object).first,                                                                     \
              (offsetof(type, last) - offsetof(type, first)) / sizeof(VkBool32) + 1 }

BoolSpan span(VkPhysicalDeviceFeatures& f)
{
    return PYROWAVE_FEATURE_SPAN(f, VkPhysicalDeviceFeatures, robustBufferAccess, inheritedQueries);
}

BoolSpan span(VkPhysicalDeviceVulkan11Features& f)
{
    return PYROWAVE_FEATURE_SPAN(f, VkPhysicalDeviceVulkan11Features, storageBuffer16BitAccess,
                                 shaderDrawParameters);
}

BoolSpan span(VkPhysicalDeviceVulkan12Features& f)
{
    return PYROWAVE_FEATURE_SPAN(f, VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge,
                                 subgroupBroadcastDynamicId);
}

BoolSpan span(VkPhysicalDeviceVulkan13Features& f)
{
    return PYROWAVE_FEATURE_SPAN(f, VkPhysicalDeviceVulkan13Features, robustImageAccess,
                                 maintenance4);
}

#undef PYROWAVE_FEATURE_SPAN

void orInto(BoolSpan destination, BoolSpan source)
{
    for (size_t i = 0; i < destination.count && i < source.count; i++) {
        if (source.data[i]) {
            destination.data[i] = VK_TRUE;
        }
    }
}

void andInto(BoolSpan destination, BoolSpan wanted, BoolSpan supported)
{
    for (size_t i = 0; i < destination.count; i++) {
        destination.data[i] = (wanted.data[i] && supported.data[i]) ? VK_TRUE : VK_FALSE;
    }
}

template <typename T> T& mutableNode(const VkBaseInStructure* node)
{
    // Only read through the result; span() needs a non-const object.
    return *const_cast<T*>(reinterpret_cast<const T*>(node));
}

// The device features as the four core structures, chained in order.
struct FeatureChain
{
    VkPhysicalDeviceFeatures2 core = {};
    VkPhysicalDeviceVulkan11Features v11 = {};
    VkPhysicalDeviceVulkan12Features v12 = {};
    VkPhysicalDeviceVulkan13Features v13 = {};

    FeatureChain()
    {
        core.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        core.pNext = &v11;
        v11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        v11.pNext = &v12;
        v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        v12.pNext = &v13;
        v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        v13.pNext = nullptr;
    }

    FeatureChain(const FeatureChain&) = delete;
    FeatureChain& operator=(const FeatureChain&) = delete;

    // Adds the core features of a chain such as libplacebo's required and
    // recommended lists. Other structures (Vulkan 1.4, extensions) are
    // skipped: this device is created for Vulkan 1.3.
    void merge(const VkPhysicalDeviceFeatures2& source)
    {
        orInto(span(core.features), span(const_cast<VkPhysicalDeviceFeatures&>(source.features)));

        auto node = static_cast<const VkBaseInStructure*>(source.pNext);
        for (; node != nullptr; node = node->pNext) {
            switch (node->sType) {
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
                orInto(span(v11), span(mutableNode<VkPhysicalDeviceVulkan11Features>(node)));
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
                orInto(span(v12), span(mutableNode<VkPhysicalDeviceVulkan12Features>(node)));
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
                orInto(span(v13), span(mutableNode<VkPhysicalDeviceVulkan13Features>(node)));
                break;
            default:
                break;
            }
        }
    }

    void setToIntersection(FeatureChain& wanted, FeatureChain& supported)
    {
        andInto(span(core.features), span(wanted.core.features), span(supported.core.features));
        andInto(span(v11), span(wanted.v11), span(supported.v11));
        andInto(span(v12), span(wanted.v12), span(supported.v12));
        andInto(span(v13), span(wanted.v13), span(supported.v13));
    }
};

}

// Everything vkCreateDevice() was given. pyrowave_create_device() reads the
// create info again, and requires it to stay valid while the device lives.
struct PyrowaveVulkanRenderer::DeviceSetup
{
    FeatureChain features;
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {};
    std::vector<const char*> extensions;
    VkDeviceCreateInfo createInfo = {};
    pyrowave_device_create_queue_info pyrowaveQueue = {};
};

PyrowaveVulkanInstance::~PyrowaveVulkanInstance()
{
    if (m_Surface != VK_NULL_HANDLE && m_DestroySurface != nullptr) {
        m_DestroySurface(m_Instance, m_Surface, nullptr);
    }
    if (m_Instance != VK_NULL_HANDLE && m_DestroyInstance != nullptr) {
        m_DestroyInstance(m_Instance, nullptr);
    }

    // m_Log must always be the last object destroyed
    pl_log_destroy(&m_Log);
}

bool PyrowaveVulkanInstance::hasExtension(const char* name) const
{
    return containsName(m_Extensions, name);
}

bool PyrowaveVulkanInstance::initialize(SDL_Window* window, PyrowaveFailure& failure)
{
    failure.stage = PyrowaveStage::Create;
    m_Window = window;
    m_Log = PlVkShared::createLog();

    m_GetInstanceProcAddr = PlVkShared::getInstanceProcAddr();
    if (m_GetInstanceProcAddr == nullptr) {
        failure.reason = tr("Vulkan isn't available for the stream window.");
        return false;
    }

    // vkEnumerateInstanceVersion is missing from Vulkan 1.0 loaders.
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        m_GetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    if (enumerateVersion != nullptr && enumerateVersion(&loaderVersion) != VK_SUCCESS) {
        loaderVersion = VK_API_VERSION_1_0;
    }
    if (loaderVersion < VK_API_VERSION_1_3) {
        failure.reason = tr("The Vulkan loader on this PC supports Vulkan %1, but PyroWave needs "
                            "Vulkan 1.3. Updating the graphics driver may help.")
                             .arg(vulkanVersionString(loaderVersion));
        return false;
    }

    if (!PlVkShared::getInstanceExtensions(window, m_Extensions)) {
        failure.reason = tr("The stream window can't be used with Vulkan.");
        return false;
    }

    auto enumerateExtensions = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        m_GetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(
        m_GetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    if (enumerateExtensions == nullptr || createInstance == nullptr) {
        failure.reason = tr("The Vulkan loader on this PC is incomplete.");
        return false;
    }

    std::vector<VkExtensionProperties> available;
    uint32_t count = 0;
    if (enumerateExtensions(nullptr, &count, nullptr) == VK_SUCCESS) {
        available.resize(count);
        if (enumerateExtensions(nullptr, &count, available.data()) != VK_SUCCESS) {
            available.clear();
        }
        available.resize(std::min<size_t>(available.size(), count));
    }

    // The optional extensions libplacebo would enable for its own instance:
    // HDR color spaces (PW-3), surface queries, and portability drivers such
    // as MoltenVK, which the loader hides without the enumeration extension.
    VkInstanceCreateFlags flags = 0;
    const char* const optionalExtensions[] = {
        k_SwapchainColorspace,    k_GetSurfaceCapabilities2, k_KhrSurfaceMaintenance1,
        k_ExtSurfaceMaintenance1, k_PortabilityEnumeration,
    };
    for (const char* name : optionalExtensions) {
        if (::hasExtension(available, name)) {
            addUnique(m_Extensions, name);
            if (std::strcmp(name, k_PortabilityEnumeration) == 0) {
                flags |= k_EnumeratePortabilityBit;
            }
        }
    }

    m_AppInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    m_AppInfo.pApplicationName = "Moonlight";
    m_AppInfo.pEngineName = "Moonlight PyroWave";
    m_AppInfo.apiVersion = VK_API_VERSION_1_3;

    m_CreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    m_CreateInfo.flags = flags;
    m_CreateInfo.pApplicationInfo = &m_AppInfo;
    m_CreateInfo.enabledExtensionCount = static_cast<uint32_t>(m_Extensions.size());
    m_CreateInfo.ppEnabledExtensionNames = m_Extensions.data();

    VkResult result = createInstance(&m_CreateInfo, nullptr, &m_Instance);
    if (result != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PyroWave: vkCreateInstance() failed: %d",
                     (int)result);
        m_Instance = VK_NULL_HANDLE;
        failure.reason = tr("Vulkan couldn't start (VkResult %1).").arg((int)result);
        return false;
    }

    m_DestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
        m_GetInstanceProcAddr(m_Instance, "vkDestroyInstance"));
    m_DestroySurface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(
        m_GetInstanceProcAddr(m_Instance, "vkDestroySurfaceKHR"));
    if (m_DestroyInstance == nullptr || m_DestroySurface == nullptr) {
        failure.reason = tr("The Vulkan loader on this PC is incomplete.");
        return false;
    }

    if (!PlVkShared::createWindowSurface(window, m_Instance, &m_Surface)) {
        m_Surface = VK_NULL_HANDLE;
        failure.reason = tr("Vulkan can't draw to the stream window.");
        return false;
    }

    return true;
}

PyrowaveVulkanRenderer::PyrowaveVulkanRenderer(PyrowaveVulkanInstance& instance)
    : m_Instance(instance)
{
}

PyrowaveVulkanRenderer::~PyrowaveVulkanRenderer()
{
    destroy();
}

void PyrowaveVulkanRenderer::lockQueue(void* context)
{
    auto me = static_cast<PyrowaveVulkanRenderer*>(context);
    me->m_Vulkan->lock_queue(me->m_Vulkan, me->m_QueueFamily, 0);
}

void PyrowaveVulkanRenderer::unlockQueue(void* context)
{
    auto me = static_cast<PyrowaveVulkanRenderer*>(context);
    me->m_Vulkan->unlock_queue(me->m_Vulkan, me->m_QueueFamily, 0);
}

bool PyrowaveVulkanRenderer::initialize(const PyrowavePresentParams& params,
                                        PyrowaveFailure& failure)
{
    m_Params = params;
    failure.stage = PyrowaveStage::Create;

    if (!loadEntryPoints(failure) || !createDevice(failure) || !createPyrowaveDecoder(failure) ||
        !createPlanes(failure) || !createPresentation(failure)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PyroWave renderer setup failed: %s",
                     qUtf8Printable(failure.reason));
        destroy();
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PyroWave renderer ready on '%s' (%dx%d, %s range, %s path)", m_DeviceName.c_str(),
                m_Params.width, m_Params.height, m_Params.fullRange ? "full" : "limited",
                m_FragmentPath ? "fragment" : "compute");
    return true;
}

#define PYROWAVE_LOAD_INSTANCE_FN(member, name)                                                    \
    member = reinterpret_cast<PFN_##name>(getProcAddr(instance, #name));                           \
    if (member == nullptr) {                                                                       \
        missing = #name;                                                                           \
    }

bool PyrowaveVulkanRenderer::loadEntryPoints(PyrowaveFailure& failure)
{
    const PyrowaveRuntime& runtime = PyrowaveRuntime::get();
    if (!runtime.isAvailable()) {
        failure.reason = runtime.userMessage();
        return false;
    }

    using EntryPoint = PyrowaveRuntime::EntryPoint;
    m_PwCreateDevice = runtime.entryPoint<PFN_pyrowave_create_device>(EntryPoint::CreateDevice);
    m_PwDestroyDevice = runtime.entryPoint<PFN_pyrowave_device_destroy>(EntryPoint::DeviceDestroy);
    m_PwSetQueueType =
        runtime.entryPoint<PFN_pyrowave_device_set_queue_type>(EntryPoint::DeviceSetQueueType);
    m_PwPrefersFragment = runtime.entryPoint<PFN_pyrowave_decoder_device_prefers_fragment_path>(
        EntryPoint::DecoderDevicePrefersFragmentPath);
    m_PwCreateDecoder = runtime.entryPoint<PFN_pyrowave_decoder_create>(EntryPoint::DecoderCreate);
    m_PwClearDecoder = runtime.entryPoint<PFN_pyrowave_decoder_clear>(EntryPoint::DecoderClear);
    m_PwPushPacket =
        runtime.entryPoint<PFN_pyrowave_decoder_push_packet>(EntryPoint::DecoderPushPacket);
    m_PwDecodeIsReady =
        runtime.entryPoint<PFN_pyrowave_decoder_decode_is_ready>(EntryPoint::DecoderDecodeIsReady);
    m_PwDecodeGpu = runtime.entryPoint<PFN_pyrowave_decoder_decode_gpu_buffer>(
        EntryPoint::DecoderDecodeGpuBuffer);
    m_PwGetColorMetadata = runtime.entryPoint<PFN_pyrowave_decoder_get_color_metadata>(
        EntryPoint::DecoderGetColorMetadata);
    m_PwDestroyDecoder =
        runtime.entryPoint<PFN_pyrowave_decoder_destroy>(EntryPoint::DecoderDestroy);

    const PFN_vkGetInstanceProcAddr getProcAddr = m_Instance.getProcAddr();
    const VkInstance instance = m_Instance.instance();
    const char* missing = nullptr;
    PYROWAVE_LOAD_INSTANCE_FN(m_GetPhysicalDeviceProperties, vkGetPhysicalDeviceProperties)
    PYROWAVE_LOAD_INSTANCE_FN(m_GetPhysicalDeviceFeatures2, vkGetPhysicalDeviceFeatures2)
    PYROWAVE_LOAD_INSTANCE_FN(m_GetQueueFamilyProperties, vkGetPhysicalDeviceQueueFamilyProperties)
    PYROWAVE_LOAD_INSTANCE_FN(m_GetSurfaceSupport, vkGetPhysicalDeviceSurfaceSupportKHR)
    PYROWAVE_LOAD_INSTANCE_FN(m_GetSurfacePresentModes, vkGetPhysicalDeviceSurfacePresentModesKHR)
    PYROWAVE_LOAD_INSTANCE_FN(m_GetFormatProperties, vkGetPhysicalDeviceFormatProperties)
    PYROWAVE_LOAD_INSTANCE_FN(m_GetMemoryProperties, vkGetPhysicalDeviceMemoryProperties)
    PYROWAVE_LOAD_INSTANCE_FN(m_EnumeratePhysicalDevices, vkEnumeratePhysicalDevices)
    PYROWAVE_LOAD_INSTANCE_FN(m_EnumerateDeviceExtensions, vkEnumerateDeviceExtensionProperties)
    PYROWAVE_LOAD_INSTANCE_FN(m_CreateDevice, vkCreateDevice)
    PYROWAVE_LOAD_INSTANCE_FN(m_GetDeviceProcAddr, vkGetDeviceProcAddr)
    if (missing != nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PyroWave: missing Vulkan function %s", missing);
        failure.reason = tr("The Vulkan loader on this PC is incomplete.");
        return false;
    }

    return true;
}

#undef PYROWAVE_LOAD_INSTANCE_FN

bool PyrowaveVulkanRenderer::createDevice(PyrowaveFailure& failure)
{
    const VkInstance instance = m_Instance.instance();
    const VkSurfaceKHR surface = m_Instance.surface();
    const std::vector<VkPhysicalDevice> devices = PlVkShared::devicesInPreferenceOrder(
        instance, m_EnumeratePhysicalDevices, m_GetPhysicalDeviceProperties);
    if (devices.empty()) {
        failure.reason = tr("No Vulkan GPU was found.");
        return false;
    }

    QString firstReason;
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties properties;
        m_GetPhysicalDeviceProperties(device, &properties);
        const QString name = QString::fromUtf8(properties.deviceName);

        QStringList missing;
        uint32_t queueFamily = UINT32_MAX;
        if (PlVkShared::isSoftwareDeviceRejected(properties)) {
            missing.append(tr("a hardware GPU (this is a software renderer)"));
        } else {
            // The same checks as the capability report, judged per GPU.
            const PyrowaveGpuCapabilities capabilities =
                readPyrowaveGpuCapabilities(m_Instance.getProcAddr(), instance, device);
            missing.append(capabilities.missingPrerequisites());

            std::vector<VkExtensionProperties> extensions;
            uint32_t count = 0;
            if (m_EnumerateDeviceExtensions(device, nullptr, &count, nullptr) == VK_SUCCESS) {
                extensions.resize(count);
                if (m_EnumerateDeviceExtensions(device, nullptr, &count, extensions.data()) !=
                    VK_SUCCESS) {
                    extensions.clear();
                }
            }
            if (!hasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
                missing.append(tr("presentation to windows (VK_KHR_swapchain)"));
            }

            uint32_t familyCount = 0;
            m_GetQueueFamilyProperties(device, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            m_GetQueueFamilyProperties(device, &familyCount, families.data());
            const VkQueueFlags graphicsCompute = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            for (uint32_t i = 0; i < familyCount && queueFamily == UINT32_MAX; i++) {
                VkBool32 present = VK_FALSE;
                if (families[i].queueCount > 0 &&
                    (families[i].queueFlags & graphicsCompute) == graphicsCompute &&
                    m_GetSurfaceSupport(device, i, surface, &present) == VK_SUCCESS &&
                    present == VK_TRUE) {
                    queueFamily = i;
                }
            }
            if (queueFamily == UINT32_MAX) {
                missing.append(tr("a graphics and compute queue that can draw to the stream "
                                  "window"));
            }

            // The decoder writes the planes from compute (or, on the fragment
            // path, as color attachments) and libplacebo samples them.
            VkFormatProperties format = {};
            m_GetFormatProperties(device, k_PlaneFormat, &format);
            const VkFormatFeatureFlags sampled = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            const VkFormatFeatureFlags writable =
                VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
            if ((format.optimalTilingFeatures & sampled) != sampled ||
                (format.optimalTilingFeatures & writable) == 0) {
                missing.append(tr("8-bit plane images that can be written and sampled"));
            }
        }

        QString reason;
        if (missing.isEmpty() && createLogicalDevice(device, queueFamily, reason)) {
            m_PhysicalDevice = device;
            m_DeviceName = properties.deviceName;
            return true;
        }

        if (reason.isEmpty()) {
            reason = tr("%1 is missing %2.").arg(name, missing.join(QStringLiteral("; ")));
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "PyroWave can't use Vulkan device '%s': %s",
                    properties.deviceName, qUtf8Printable(reason));
        if (firstReason.isEmpty()) {
            firstReason = reason;
        }
    }

    failure.reason = firstReason;
    return false;
}

bool PyrowaveVulkanRenderer::createLogicalDevice(VkPhysicalDevice device, uint32_t queueFamily,
                                                 QString& reason)
{
    VkPhysicalDeviceProperties properties;
    m_GetPhysicalDeviceProperties(device, &properties);
    const QString name = QString::fromUtf8(properties.deviceName);

    auto setup = std::make_unique<DeviceSetup>();

    FeatureChain supported;
    m_GetPhysicalDeviceFeatures2(device, &supported.core);

    // What libplacebo needs and would enable on a device it created itself,
    // plus what pyrowave.h lists for the decoder (and the Android client
    // enables). Each is enabled only where the GPU supports it.
    FeatureChain wanted;
    wanted.merge(pl_vulkan_required_features);
    wanted.merge(pl_vulkan_recommended_features);
    wanted.core.features.shaderInt16 = VK_TRUE;
    wanted.core.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    wanted.v12.storageBuffer8BitAccess = VK_TRUE;
    wanted.v12.shaderInt8 = VK_TRUE;
    wanted.v12.shaderFloat16 = VK_TRUE;
    wanted.v12.timelineSemaphore = VK_TRUE;
    wanted.v13.subgroupSizeControl = VK_TRUE;
    wanted.v13.computeFullSubgroups = VK_TRUE;
    wanted.v13.synchronization2 = VK_TRUE;
    // Never: it costs performance and nothing here relies on it.
    wanted.core.features.robustBufferAccess = VK_FALSE;

    FeatureChain& enabled = setup->features;
    enabled.setToIntersection(wanted, supported);

    const struct
    {
        VkBool32 enabled;
        const char* name;
    } requiredFeatures[] = {
        // libplacebo (pl_vulkan_required_features)
        { enabled.v12.hostQueryReset, "hostQueryReset" },
        { enabled.v12.timelineSemaphore, "timelineSemaphore" },
        // PyroWave's decoder (pyrowave.h)
        { enabled.v13.subgroupSizeControl, "subgroupSizeControl" },
        { enabled.v13.computeFullSubgroups, "computeFullSubgroups" },
        // Granite records its barriers with synchronization2
        { enabled.v13.synchronization2, "synchronization2" },
    };
    for (const auto& feature : requiredFeatures) {
        if (!feature.enabled) {
            reason = tr("%1 is missing the Vulkan feature %2.")
                         .arg(name, QString::fromLatin1(feature.name));
            return false;
        }
    }

    std::vector<VkExtensionProperties> available;
    uint32_t count = 0;
    if (m_EnumerateDeviceExtensions(device, nullptr, &count, nullptr) == VK_SUCCESS) {
        available.resize(count);
        if (m_EnumerateDeviceExtensions(device, nullptr, &count, available.data()) != VK_SUCCESS) {
            available.clear();
        }
    }

    // VK_KHR_swapchain, plus libplacebo's recommended extensions where
    // supported (as pl_vulkan_create() does for PlVkRenderer). The spec
    // requires VK_KHR_portability_subset whenever the device offers it.
    setup->extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (hasExtension(available, k_PortabilitySubset)) {
        addUnique(setup->extensions, k_PortabilitySubset);
    }
    for (int i = 0; i < pl_vulkan_num_recommended_extensions; i++) {
        const char* extension = pl_vulkan_recommended_extensions[i];
        if (!hasExtension(available, extension)) {
            continue;
        }

        // Skip extensions whose instance-level dependency isn't enabled.
        if (std::strcmp(extension, "VK_EXT_full_screen_exclusive") == 0 &&
            !m_Instance.hasExtension(k_GetSurfaceCapabilities2)) {
            continue;
        }
        if (std::strcmp(extension, "VK_KHR_swapchain_maintenance1") == 0 &&
            !m_Instance.hasExtension(k_KhrSurfaceMaintenance1)) {
            continue;
        }
        if (std::strcmp(extension, "VK_EXT_swapchain_maintenance1") == 0 &&
            !m_Instance.hasExtension(k_ExtSurfaceMaintenance1)) {
            continue;
        }

        addUnique(setup->extensions, extension);
    }

    setup->queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    setup->queueInfo.queueFamilyIndex = queueFamily;
    setup->queueInfo.queueCount = 1;
    setup->queueInfo.pQueuePriorities = &setup->queuePriority;

    setup->createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    setup->createInfo.pNext = &enabled.core;
    setup->createInfo.queueCreateInfoCount = 1;
    setup->createInfo.pQueueCreateInfos = &setup->queueInfo;
    setup->createInfo.enabledExtensionCount = static_cast<uint32_t>(setup->extensions.size());
    setup->createInfo.ppEnabledExtensionNames = setup->extensions.data();

    VkResult result;
    {
        // Don't let Qt take DRM master from us during device creation
        DrmMasterLocker locker;
        result = m_CreateDevice(device, &setup->createInfo, nullptr, &m_Device);
    }
    if (result != VK_SUCCESS) {
        m_Device = VK_NULL_HANDLE;
        reason = tr("%1 couldn't create a Vulkan device (VkResult %2).").arg(name).arg((int)result);
        return false;
    }

    const char* missing = nullptr;
#define PYROWAVE_LOAD_DEVICE_FN(member, fn)                                                        \
    member = reinterpret_cast<PFN_##fn>(m_GetDeviceProcAddr(m_Device, #fn));                       \
    if (member == nullptr) {                                                                       \
        missing = #fn;                                                                             \
    }
    PYROWAVE_LOAD_DEVICE_FN(m_DestroyDevice, vkDestroyDevice)
    PYROWAVE_LOAD_DEVICE_FN(m_GetDeviceQueue, vkGetDeviceQueue)
    PYROWAVE_LOAD_DEVICE_FN(m_DeviceWaitIdle, vkDeviceWaitIdle)
    PYROWAVE_LOAD_DEVICE_FN(m_CreateImage, vkCreateImage)
    PYROWAVE_LOAD_DEVICE_FN(m_DestroyImage, vkDestroyImage)
    PYROWAVE_LOAD_DEVICE_FN(m_GetImageMemoryRequirements, vkGetImageMemoryRequirements)
    PYROWAVE_LOAD_DEVICE_FN(m_AllocateMemory, vkAllocateMemory)
    PYROWAVE_LOAD_DEVICE_FN(m_FreeMemory, vkFreeMemory)
    PYROWAVE_LOAD_DEVICE_FN(m_BindImageMemory, vkBindImageMemory)
#undef PYROWAVE_LOAD_DEVICE_FN
    if (missing != nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PyroWave: missing Vulkan function %s", missing);
        if (m_DestroyDevice != nullptr) {
            m_DestroyDevice(m_Device, nullptr);
        }
        m_Device = VK_NULL_HANDLE;
        reason = tr("The Vulkan driver for %1 is incomplete.").arg(name);
        return false;
    }

    m_QueueFamily = queueFamily;
    m_GetDeviceQueue(m_Device, queueFamily, 0, &m_Queue);

    // One graphics and compute queue for everything. libplacebo falls back to
    // it for compute and transfers, so every submission is on one queue.
    pl_vulkan_import_params importParams = {};
    importParams.instance = m_Instance.instance();
    importParams.get_proc_addr = m_Instance.getProcAddr();
    importParams.phys_device = device;
    importParams.device = m_Device;
    importParams.extensions = setup->extensions.data();
    importParams.num_extensions = static_cast<int>(setup->extensions.size());
    importParams.queue_graphics.index = queueFamily;
    importParams.queue_graphics.count = 1;
    importParams.features = &enabled.core;
    m_Vulkan = pl_vulkan_import(m_Instance.log(), &importParams);
    if (m_Vulkan == nullptr) {
        m_DestroyDevice(m_Device, nullptr);
        m_Device = VK_NULL_HANDLE;
        reason = tr("libplacebo couldn't use the Vulkan device on %1.").arg(name);
        return false;
    }

    setup->pyrowaveQueue.queue = m_Queue;
    setup->pyrowaveQueue.familyIndex = queueFamily;
    setup->pyrowaveQueue.index = 0;
    m_Setup = std::move(setup);
    return true;
}

bool PyrowaveVulkanRenderer::createPyrowaveDecoder(PyrowaveFailure& failure)
{
    pyrowave_device_create_info info = {};
    info.GetInstanceProcAddr = m_Instance.getProcAddr();
    info.instance = m_Instance.instance();
    info.physical_device = m_PhysicalDevice;
    info.device = m_Device;
    info.instance_create_info = m_Instance.createInfo();
    info.device_create_info = &m_Setup->createInfo;
    info.queue_info = &m_Setup->pyrowaveQueue;
    info.queue_info_count = 1;
    // libplacebo submits to the same queue, from other threads too (overlay
    // uploads), so PyroWave takes the same lock.
    info.queue_lock_callback = lockQueue;
    info.queue_unlock_callback = unlockQueue;
    info.userdata = this;

    pyrowave_result result = m_PwCreateDevice(&info, &m_PyroDevice);
    if (result != PYROWAVE_SUCCESS || m_PyroDevice == nullptr) {
        m_PyroDevice = nullptr;
        failure.reason = tr("PyroWave couldn't use the Vulkan device on %1 (error %2).")
                             .arg(QString::fromStdString(m_DeviceName))
                             .arg(result);
        return false;
    }

    // Decode on the graphics queue, where libplacebo consumes the planes.
    result = m_PwSetQueueType(m_PyroDevice, VK_QUEUE_GRAPHICS_BIT);
    if (result != PYROWAVE_SUCCESS) {
        failure.reason = tr("PyroWave couldn't select the graphics queue (error %1).").arg(result);
        return false;
    }

    m_FragmentPath = m_PwPrefersFragment(m_PyroDevice);

    pyrowave_decoder_create_info decoderInfo = {};
    decoderInfo.device = m_PyroDevice;
    decoderInfo.width = m_Params.width;
    decoderInfo.height = m_Params.height;
    decoderInfo.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
    decoderInfo.fragment_path = m_FragmentPath;
    result = m_PwCreateDecoder(&decoderInfo, &m_Decoder);
    if (result != PYROWAVE_SUCCESS || m_Decoder == nullptr) {
        m_Decoder = nullptr;
        failure.reason = tr("PyroWave couldn't create a %1x%2 decoder (error %3).")
                             .arg(m_Params.width)
                             .arg(m_Params.height)
                             .arg(result);
        return false;
    }

    return true;
}

bool PyrowaveVulkanRenderer::createPlane(Plane& plane, uint32_t width, uint32_t height,
                                         VkImageUsageFlags usage)
{
    plane.width = width;
    plane.height = height;

    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = k_PlaneFormat;
    imageInfo.extent = { width, height, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (m_CreateImage(m_Device, &imageInfo, nullptr, &plane.image) != VK_SUCCESS) {
        plane.image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements requirements;
    m_GetImageMemoryRequirements(m_Device, plane.image, &requirements);
    VkPhysicalDeviceMemoryProperties memory;
    m_GetMemoryProperties(m_PhysicalDevice, &memory);
    uint32_t memoryType = UINT32_MAX;
    for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
        if ((requirements.memoryTypeBits & (1u << i)) == 0) {
            continue;
        }
        if (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            memoryType = i;
            break;
        }
        if (memoryType == UINT32_MAX) {
            memoryType = i;
        }
    }
    if (memoryType == UINT32_MAX) {
        return false;
    }

    VkMemoryAllocateInfo allocation = {};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    if (m_AllocateMemory(m_Device, &allocation, nullptr, &plane.memory) != VK_SUCCESS) {
        plane.memory = VK_NULL_HANDLE;
        return false;
    }
    if (m_BindImageMemory(m_Device, plane.image, plane.memory, 0) != VK_SUCCESS) {
        return false;
    }

    pl_vulkan_wrap_params wrap = {};
    wrap.image = plane.image;
    wrap.width = static_cast<int>(width);
    wrap.height = static_cast<int>(height);
    wrap.format = k_PlaneFormat;
    wrap.usage = usage;
    wrap.debug_tag = PL_DEBUG_TAG;
    plane.tex = pl_vulkan_wrap(m_Vulkan->gpu, &wrap);
    if (plane.tex == nullptr) {
        return false;
    }

    // A wrapped image starts out held by us. Give it to libplacebo with no
    // contents, so every frame takes the same hold/decode/release path.
    pl_vulkan_release_params release = {};
    release.tex = plane.tex;
    release.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    release.qf = VK_QUEUE_FAMILY_IGNORED;
    pl_vulkan_release_ex(m_Vulkan->gpu, &release);
    return true;
}

bool PyrowaveVulkanRenderer::createPlanes(PyrowaveFailure& failure)
{
    // PyroWave writes the compute path through storage images and the
    // fragment path through color attachments; libplacebo samples both.
    const VkImageUsageFlags writeUsage =
        m_FragmentPath ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_STORAGE_BIT;
    const VkFormatFeatureFlags writeFeature = m_FragmentPath
                                                  ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT
                                                  : VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    VkFormatProperties format = {};
    m_GetFormatProperties(m_PhysicalDevice, k_PlaneFormat, &format);
    if ((format.optimalTilingFeatures & writeFeature) == 0) {
        failure.reason = tr("%1 can't write 8-bit plane images the way PyroWave needs.")
                             .arg(QString::fromStdString(m_DeviceName));
        return false;
    }

    const uint32_t width = static_cast<uint32_t>(m_Params.width);
    const uint32_t height = static_cast<uint32_t>(m_Params.height);
    for (int i = 0; i < 3; i++) {
        // 4:2:0: chroma planes are half size in both directions.
        const uint32_t planeWidth = i == 0 ? width : width / 2;
        const uint32_t planeHeight = i == 0 ? height : height / 2;
        if (!createPlane(m_Planes[i], planeWidth, planeHeight,
                         VK_IMAGE_USAGE_SAMPLED_BIT | writeUsage)) {
            failure.reason = tr("Couldn't allocate the %1x%2 video planes on the GPU.")
                                 .arg(m_Params.width)
                                 .arg(m_Params.height);
            return false;
        }
    }

    pl_vulkan_sem_params semaphoreParams = {};
    semaphoreParams.type = VK_SEMAPHORE_TYPE_TIMELINE;
    semaphoreParams.initial_value = 0;
    semaphoreParams.debug_tag = PL_DEBUG_TAG;
    m_PlaneSemaphore = pl_vulkan_sem_create(m_Vulkan->gpu, &semaphoreParams);
    m_PlaneTimeline = 0;
    if (m_PlaneSemaphore == VK_NULL_HANDLE) {
        failure.reason = tr("Couldn't create the GPU synchronization objects.");
        return false;
    }

    return true;
}

bool PyrowaveVulkanRenderer::createPresentation(PyrowaveFailure& failure)
{
    const VkSurfaceKHR surface = m_Instance.surface();
    m_PresentMode =
        PlVkShared::choosePresentMode(m_Params.enableVsync, [this, surface](VkPresentModeKHR mode) {
            uint32_t count = 0;
            m_GetSurfacePresentModes(m_PhysicalDevice, surface, &count, nullptr);
            std::vector<VkPresentModeKHR> modes(count);
            m_GetSurfacePresentModes(m_PhysicalDevice, surface, &count, modes.data());
            for (uint32_t i = 0; i < count && i < modes.size(); i++) {
                if (modes[i] == mode) {
                    return true;
                }
            }
            return false;
        });

    // Double-buffered for the lowest display latency, like PlVkRenderer.
    // TODO(PW-4): PlVkRenderer's switch to triple buffering after delayed
    // presents, and frame pacing.
    m_Swapchain = PlVkShared::createSwapchain(m_Vulkan, surface, m_PresentMode, 1);
    if (m_Swapchain == nullptr) {
        failure.reason = tr("Couldn't create a swapchain for the stream window.");
        return false;
    }

#ifdef Q_OS_DARWIN
    // The same BT.709 gamma workaround as PlVkRenderer
    pl_swapchain_colorspace_hint(m_Swapchain, &pl_color_space_srgb);
#else
    // TODO(PW-3): hint BT.2020 PQ or HLG for HDR sessions.
    pl_swapchain_colorspace_hint(m_Swapchain, &pl_color_space_bt709);
#endif

    m_Renderer = pl_renderer_create(m_Instance.log(), m_Vulkan->gpu);
    if (m_Renderer == nullptr) {
        failure.reason = tr("Couldn't create the video renderer.");
        return false;
    }

    return true;
}

void PyrowaveVulkanRenderer::notifyOverlayUpdated(Overlay::OverlayType type)
{
    if (m_Vulkan != nullptr) {
        m_Overlays.notifyUpdated(m_Vulkan->gpu, type);
    }
}

bool PyrowaveVulkanRenderer::checkColorMetadata(PyrowaveFailure& failure)
{
    failure.stage = PyrowaveStage::Color;

    pyrowave_color_metadata metadata = {};
    if (!m_PwGetColorMetadata(m_Decoder, &metadata)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PyroWave frame has no color metadata");
        failure.reason = tr("The host sent a PyroWave frame without color information.");
        return false;
    }

    // SDR is BT.709 with the session's range (client contract, section 3).
    // A frame that doesn't match is never shown as if it did.
    // TODO(PW-3): BT.2020 PQ/HLG for HDR sessions.
    const pyrowave_ycbcr_range expectedRange =
        m_Params.fullRange ? PYROWAVE_YCBCR_FULL : PYROWAVE_YCBCR_LIMITED;
    if (metadata.primaries != PYROWAVE_COLOR_PRIMARIES_BT709 ||
        metadata.transfer != PYROWAVE_TRANSFER_BT709 ||
        metadata.transform != PYROWAVE_YCBCR_BT709 || metadata.range != expectedRange ||
        metadata.chroma_siting > PYROWAVE_CHROMA_SITING_LEFT) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave color metadata mismatch: primaries %d, transfer %d, transform %d, "
                     "range %d, siting %u (expected BT.709 SDR, range %d)",
                     metadata.primaries, metadata.transfer, metadata.transform, metadata.range,
                     metadata.chroma_siting, expectedRange);
        failure.reason = tr("The host sent PyroWave video in a different color format than this "
                            "session uses (BT.709 SDR, %1 range).")
                             .arg(m_Params.fullRange ? tr("full") : tr("limited"));
        return false;
    }

    m_ChromaSiting = metadata.chroma_siting;
    return true;
}

bool PyrowaveVulkanRenderer::decodeToPlanes(PyrowaveFailure& failure)
{
    pl_gpu gpu = m_Vulkan->gpu;

    // Take the planes from libplacebo. Each hold transitions its plane to
    // GENERAL after libplacebo's last use and signals the next timeline value.
    // Everything is on one queue, so the values signal in order.
    int held = 0;
    for (; held < 3; held++) {
        pl_vulkan_hold_params hold = {};
        hold.tex = m_Planes[held].tex;
        hold.layout = VK_IMAGE_LAYOUT_GENERAL;
        hold.qf = VK_QUEUE_FAMILY_IGNORED;
        hold.semaphore.sem = m_PlaneSemaphore;
        hold.semaphore.value = m_PlaneTimeline + 1;
        if (!pl_vulkan_hold_ex(gpu, &hold)) {
            break;
        }
        m_PlaneTimeline++;
    }
    pl_gpu_flush(gpu);

    pyrowave_result result = PYROWAVE_ERROR_GENERIC;
    if (held == 3) {
        pyrowave_gpu_buffers buffers = {};
        for (int i = 0; i < 3; i++) {
            pyrowave_image_view& view = buffers.planes[i];
            view.image = m_Planes[i].image;
            view.width = m_Planes[i].width;
            view.height = m_Planes[i].height;
            view.image_format = k_PlaneFormat;
            view.view_format = k_PlaneFormat;
            view.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            view.swizzle = VK_COMPONENT_SWIZZLE_IDENTITY;
            view.layout = VK_IMAGE_LAYOUT_GENERAL;
        }

        // Wait for the last hold, write the planes, then signal the next
        // value. The planes are our own images, not imported pyrowave_images,
        // so no external references are passed (sync design, section 3.1).
        pyrowave_gpu_sync_operation acquire = {};
        acquire.sync.semaphore = m_PlaneSemaphore;
        acquire.sync.value = m_PlaneTimeline;
        pyrowave_gpu_sync_operation release = {};
        release.sync.semaphore = m_PlaneSemaphore;
        release.sync.value = m_PlaneTimeline + 1;
        result = m_PwDecodeGpu(m_Decoder, &acquire, &release, &buffers);
        if (result == PYROWAVE_SUCCESS) {
            m_PlaneTimeline++;
        }
    }

    // Hand the planes back. libplacebo waits for the decode, or after a
    // failure for the hold, before it reads or transitions them again.
    // Timeline values need no consumer, so nothing is left pending.
    for (int i = 0; i < held; i++) {
        pl_vulkan_release_params release = {};
        release.tex = m_Planes[i].tex;
        release.layout = VK_IMAGE_LAYOUT_GENERAL;
        release.qf = VK_QUEUE_FAMILY_IGNORED;
        release.semaphore.sem = m_PlaneSemaphore;
        release.semaphore.value = m_PlaneTimeline;
        pl_vulkan_release_ex(gpu, &release);
    }

    if (held != 3) {
        failure.stage = PyrowaveStage::Sync;
        failure.reason = tr("Couldn't hand the video planes to the PyroWave decoder.");
        return false;
    }
    if (result != PYROWAVE_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "pyrowave_decoder_decode_gpu_buffer() failed: %d", result);
        failure.stage = PyrowaveStage::Decode;
        failure.reason = tr("PyroWave couldn't decode a frame (error %1).").arg(result);
        return false;
    }

    return true;
}

PyrowaveVulkanRenderer::FrameResult
PyrowaveVulkanRenderer::submitFrame(const uint8_t* data, size_t length,
                                    PyrowaveFrameTimings& timings, PyrowaveFailure& failure)
{
    timings = PyrowaveFrameTimings();
    const uint64_t decodeStart = LiGetMicroseconds();

    if (pl_gpu_is_failed(m_Vulkan->gpu)) {
        failure.stage = PyrowaveStage::Sync;
        failure.reason = tr("The GPU stopped responding.");
        return FrameResult::Failed;
    }

    // The frame is one complete codec payload. The decoder walks the packets
    // inside it and tells us whether every one it needs arrived.
    if (m_PwPushPacket(m_Decoder, data, length) != PYROWAVE_SUCCESS) {
        failure.stage = PyrowaveStage::Decode;
        failure.reason = tr("The PyroWave decoder rejected a frame.");
        return FrameResult::Failed;
    }
    if (!m_PwDecodeIsReady(m_Decoder, false)) {
        // Keep the decoder and the last shown image (client contract,
        // section 6). Only the incomplete frame is thrown away.
        m_PwClearDecoder(m_Decoder);
        return FrameResult::Dropped;
    }
    if (!checkColorMetadata(failure)) {
        return FrameResult::Failed;
    }
    timings.decodeUs = LiGetMicroseconds() - decodeStart;

    // Get the swapchain image before decoding, so a window that can't show
    // anything right now doesn't cost a decode. Handle resizes like
    // PlVkRenderer::waitToRender().
    const uint64_t acquireStart = LiGetMicroseconds();
    int drawableWidth, drawableHeight;
    PlVkShared::getDrawableSize(m_Instance.window(), &drawableWidth, &drawableHeight);
    if (!pl_swapchain_resize(m_Swapchain, &drawableWidth, &drawableHeight)) {
        // Swapchain (re)creation can fail if the window is occluded
        return FrameResult::NotPresented;
    }
    pl_swapchain_frame swapchainFrame;
    if (!pl_swapchain_start_frame(m_Swapchain, &swapchainFrame)) {
        return FrameResult::NotPresented;
    }
    timings.renderUs = LiGetMicroseconds() - acquireStart;

    // From here on the swapchain frame must be submitted, even on failure.
    const uint64_t gpuDecodeStart = LiGetMicroseconds();
    const bool decoded = decodeToPlanes(failure);
    timings.decodeUs += LiGetMicroseconds() - gpuDecodeStart;

    const uint64_t renderStart = LiGetMicroseconds();
    PyrowaveFailure presentFailure;
    const FrameResult result = present(swapchainFrame, decoded, presentFailure);
    timings.renderUs += LiGetMicroseconds() - renderStart;

    if (!decoded) {
        // failure already describes the decode
        return FrameResult::Failed;
    }
    if (result == FrameResult::Failed) {
        failure = presentFailure;
    }
    return result;
}

PyrowaveVulkanRenderer::FrameResult
PyrowaveVulkanRenderer::present(const pl_swapchain_frame& swapchainFrame, bool decoded,
                                PyrowaveFailure& failure)
{
    // A failed decode leaves the planes as they were, so the previous image
    // is shown again. Before the first decode there is nothing to show.
    if (decoded) {
        m_HasDecodedFrame = true;
    }

    pl_frame targetFrame;
    pl_frame_from_swapchain(&targetFrame, &swapchainFrame);

    // Reserve enough space to avoid allocating under the overlay lock
    pl_overlay_part overlayParts[Overlay::OverlayMax] = {};
    std::vector<pl_tex> texturesToDestroy;
    std::vector<pl_overlay> overlays;
    texturesToDestroy.reserve(Overlay::OverlayMax);
    overlays.reserve(Overlay::OverlayMax);
    m_Overlays.collect(targetFrame, overlayParts, overlays, texturesToDestroy);

    pl_frame image = {};
    image.num_planes = 3;
    const int channels[3] = { PL_CHANNEL_Y, PL_CHANNEL_CB, PL_CHANNEL_CR };
    for (int i = 0; i < 3; i++) {
        image.planes[i].texture = m_Planes[i].tex;
        image.planes[i].components = 1;
        image.planes[i].component_mapping[0] = channels[i];
    }
    image.repr.sys = PL_COLOR_SYSTEM_BT_709;
    image.repr.levels = m_Params.fullRange ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
    image.repr.bits.sample_depth = 8;
    image.repr.bits.color_depth = 8;
    image.color = pl_color_space_bt709;
    image.crop.x0 = 0;
    image.crop.y0 = 0;
    image.crop.x1 = (float)m_Params.width;
    image.crop.y1 = (float)m_Params.height;
    pl_frame_set_chroma_location(
        &image, m_ChromaSiting == PYROWAVE_CHROMA_SITING_LEFT ? PL_CHROMA_LEFT : PL_CHROMA_CENTER);

    SDL_Rect src;
    src.x = 0;
    src.y = 0;
    src.w = m_Params.width;
    src.h = m_Params.height;

    SDL_Rect dst;
    dst.x = targetFrame.crop.x0;
    dst.y = targetFrame.crop.y0;
    dst.w = targetFrame.crop.x1 - targetFrame.crop.x0;
    dst.h = targetFrame.crop.y1 - targetFrame.crop.y0;

    // Scale the video to the surface size while preserving the aspect ratio
    if (!m_Params.ignoreAspectRatio) {
        StreamUtils::scaleSourceToDestinationSurface(&src, &dst);
    }

    targetFrame.crop.x0 = dst.x;
    targetFrame.crop.y0 = dst.y;
    targetFrame.crop.x1 = dst.x + dst.w;
    targetFrame.crop.y1 = dst.y + dst.h;

    targetFrame.num_overlays = (int)overlays.size();
    targetFrame.overlays = overlays.data();
    if (!pl_render_image(m_Renderer, m_HasDecodedFrame ? &image : nullptr, &targetFrame,
                         &pl_render_fast_params)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "pl_render_image() failed");
        // NB: We must fallthrough to call pl_swapchain_submit_frame()
    }

    // Submit the frame for display and swap buffers. Unlike PlVkRenderer,
    // which waits in waitToRender() on most platforms, swap right away: this
    // thread decodes the next frame next, so waiting here is the earliest
    // point to know the present has been queued.
    const bool submitted = pl_swapchain_submit_frame(m_Swapchain);
    if (submitted) {
        pl_swapchain_swap_buffers(m_Swapchain);
    }

    // Delete any textures that need to be destroyed
    for (pl_tex& texture : texturesToDestroy) {
        pl_tex_destroy(m_Vulkan->gpu, &texture);
    }

    if (!submitted) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "pl_swapchain_submit_frame() failed");
        failure.stage = PyrowaveStage::Present;
        failure.reason = tr("Couldn't show a frame in the stream window.");
        return FrameResult::Failed;
    }

    return FrameResult::Presented;
}

void PyrowaveVulkanRenderer::destroy()
{
    // Let libplacebo finish everything it queued (rendering and overlay
    // uploads) before anything it may still reference goes away.
    if (m_Vulkan != nullptr) {
        pl_gpu_finish(m_Vulkan->gpu);
    }

    // All decoders before the device. Both wait for the GPU themselves.
    if (m_Decoder != nullptr) {
        m_PwDestroyDecoder(m_Decoder);
        m_Decoder = nullptr;
    }
    if (m_PyroDevice != nullptr) {
        m_PwDestroyDevice(m_PyroDevice);
        m_PyroDevice = nullptr;
    }

    if (m_Vulkan != nullptr) {
        m_Overlays.destroy(m_Vulkan->gpu);
        for (Plane& plane : m_Planes) {
            pl_tex_destroy(m_Vulkan->gpu, &plane.tex);
        }
        if (m_PlaneSemaphore != VK_NULL_HANDLE) {
            pl_vulkan_sem_destroy(m_Vulkan->gpu, &m_PlaneSemaphore);
        }
        pl_renderer_destroy(&m_Renderer);
        {
            // Hold DRM master in case the Vulkan implmentation wants to restore DRM state
            DrmMasterLocker locker;
            pl_swapchain_destroy(&m_Swapchain);
        }

        // An imported device survives this; only libplacebo's state goes.
        pl_vulkan_destroy(&m_Vulkan);
    }

    if (m_Device != VK_NULL_HANDLE) {
        m_DeviceWaitIdle(m_Device);
        for (Plane& plane : m_Planes) {
            if (plane.image != VK_NULL_HANDLE) {
                m_DestroyImage(m_Device, plane.image, nullptr);
            }
            if (plane.memory != VK_NULL_HANDLE) {
                m_FreeMemory(m_Device, plane.memory, nullptr);
            }
            plane = Plane();
        }
        m_DestroyDevice(m_Device, nullptr);
        m_Device = VK_NULL_HANDLE;
    }

    m_Queue = VK_NULL_HANDLE;
    m_PhysicalDevice = VK_NULL_HANDLE;
    m_PlaneSemaphore = VK_NULL_HANDLE;
    m_PlaneTimeline = 0;
    m_HasDecodedFrame = false;
    m_Setup.reset();
}

#endif
