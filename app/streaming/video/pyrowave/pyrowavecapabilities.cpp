#include "pyrowavecapabilities.h"
#include "pyrowaveruntime.h"

#ifdef HAVE_LIBPLACEBO_VULKAN
#include <QDir>
#include <QFileInfo>
#include <QLibrary>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>
#include <vector>
#endif

namespace {

// Translations for the free functions below share the structs' context.
class CapabilityText
{
    Q_DECLARE_TR_FUNCTIONS(PyrowaveCapabilities)
};

// Values of VkSubgroupFeatureFlagBits and VK_SHADER_STAGE_COMPUTE_BIT, kept
// here so the report can be formatted without the Vulkan headers.
constexpr uint32_t k_SubgroupBasic = 0x01;
constexpr uint32_t k_SubgroupVote = 0x02;
constexpr uint32_t k_SubgroupArithmetic = 0x04;
constexpr uint32_t k_SubgroupBallot = 0x08;
constexpr uint32_t k_SubgroupShuffle = 0x10;
constexpr uint32_t k_SubgroupShuffleRelative = 0x20;
constexpr uint32_t k_SubgroupClustered = 0x40;
constexpr uint32_t k_SubgroupQuad = 0x80;
constexpr uint32_t k_ShaderStageCompute = 0x20;

// The decoder's subgroup needs, from pyrowave.h. Clustered operations are
// only needed by the encoder.
constexpr uint32_t k_RequiredSubgroupOperations = k_SubgroupBasic | k_SubgroupVote |
                                                  k_SubgroupArithmetic | k_SubgroupBallot |
                                                  k_SubgroupShuffle | k_SubgroupShuffleRelative;

constexpr uint32_t makeVersion(uint32_t major, uint32_t minor)
{
    return (major << 22) | (minor << 12);
}

// The shared-device path hands pyrowave_create_device() a Vulkan 1.3 device.
constexpr uint32_t k_RequiredDeviceApiVersion = makeVersion(1, 3);

// Matches the Android capability report: 8-bit storage, or a texel buffer
// large enough for the fallback path.
constexpr uint32_t k_MinTexelBufferElements = 16u * 1024 * 1024;

QString versionString(uint32_t version)
{
    const uint32_t major = (version >> 22) & 0x7f;
    const uint32_t minor = (version >> 12) & 0x3ff;
    const uint32_t patch = version & 0xfff;
    return QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(patch);
}

QString yesNo(bool value)
{
    return value ? CapabilityText::tr("yes") : CapabilityText::tr("no");
}

QString subgroupOperationNames(uint32_t operations)
{
    static const struct
    {
        uint32_t bit;
        const char* name;
    } k_Names[] = {
        { k_SubgroupBasic, "basic" },           { k_SubgroupVote, "vote" },
        { k_SubgroupArithmetic, "arithmetic" }, { k_SubgroupBallot, "ballot" },
        { k_SubgroupShuffle, "shuffle" },       { k_SubgroupShuffleRelative, "shuffle_relative" },
        { k_SubgroupClustered, "clustered" },   { k_SubgroupQuad, "quad" },
    };

    // Vulkan's own names, left untranslated on purpose.
    QStringList names;
    for (const auto& entry : k_Names) {
        if (operations & entry.bit) {
            names.append(QString::fromLatin1(entry.name));
        }
    }
    return names.join(QStringLiteral(", "));
}

bool hasUsableSubgroupSizeRange(const PyrowaveGpuCapabilities& gpu)
{
    // Same rule as Granite (and the Android report): any range inside
    // wave4..wave128 works; outside it, compute must allow a required size.
    if (!gpu.subgroupSizeControl || !gpu.computeFullSubgroups) {
        return false;
    }
    if (gpu.minSubgroupSize == 0 || gpu.maxSubgroupSize < gpu.minSubgroupSize) {
        return false;
    }
    if (gpu.minSubgroupSize >= 4 && gpu.maxSubgroupSize <= 128) {
        return true;
    }
    if (gpu.maxSubgroupSize < 4 || gpu.minSubgroupSize > 128) {
        return false;
    }
    return (gpu.requiredSubgroupSizeStages & k_ShaderStageCompute) != 0;
}

}

QStringList PyrowaveGpuCapabilities::missingPrerequisites() const
{
    QStringList missing;

    if (apiVersion < k_RequiredDeviceApiVersion) {
        missing.append(tr("Vulkan 1.3 (this GPU reports %1)").arg(versionString(apiVersion)));
    }

    const uint32_t missingOperations = k_RequiredSubgroupOperations & ~subgroupOperations;
    if (missingOperations != 0) {
        const QString names = subgroupOperationNames(missingOperations);
        missing.append(tr("subgroup operations: %1").arg(names));
    }
    if ((subgroupStages & k_ShaderStageCompute) == 0) {
        missing.append(tr("subgroup operations in compute shaders"));
    }

    if (!subgroupSizeControl) {
        missing.append(tr("subgroup size control"));
    } else if (!computeFullSubgroups) {
        missing.append(tr("full compute subgroups"));
    } else if (!hasUsableSubgroupSizeRange(*this)) {
        const QString range = tr("a usable subgroup size range (reports %1 to %2)");
        missing.append(range.arg(minSubgroupSize).arg(maxSubgroupSize));
    }

    if (!storageBuffer8BitAccess && maxTexelBufferElements < k_MinTexelBufferElements) {
        missing.append(tr("8-bit storage buffers"));
    }

    if (!graphicsComputeQueue) {
        missing.append(tr("a queue with both graphics and compute"));
    }

    return missing;
}

bool PyrowaveCapabilityReport::anyDeviceMeetsPrerequisites() const
{
    if (failure != Failure::None) {
        return false;
    }

    for (const PyrowaveGpuCapabilities& device : devices) {
        if (device.meetsPrerequisites()) {
            return true;
        }
    }
    return false;
}

QString PyrowaveCapabilityReport::summary() const
{
    switch (failure) {
    case Failure::NotCompiled:
        return tr("This build can't check Vulkan capabilities.");
    case Failure::LoaderUnavailable:
        return tr("No Vulkan loader was found on this PC.");
    case Failure::LoaderTooOld:
        return tr("The Vulkan loader is too old to check (%1).").arg(versionString(loaderVersion));
    case Failure::InstanceCreationFailed:
        return tr("Vulkan couldn't start (VkResult %1).").arg(vkResult);
    case Failure::QueryFailed:
        return tr("The Vulkan driver didn't answer the capability query.");
    case Failure::NoDevices:
        return tr("No Vulkan GPU was found.");
    case Failure::None:
        break;
    }

    for (const PyrowaveGpuCapabilities& device : devices) {
        if (device.meetsPrerequisites()) {
            return tr("%1 meets the basic requirements.").arg(device.name);
        }
    }

    if (devices.isEmpty()) {
        return tr("No Vulkan GPU was found.");
    }

    const PyrowaveGpuCapabilities& first = devices.first();
    const QString missing = first.missingPrerequisites().join(QStringLiteral("; "));
    return tr("%1 is missing %2.").arg(first.name, missing);
}

QString PyrowaveCapabilityReport::toText() const
{
    const QString indent = QStringLiteral("  ");
    QStringList lines;

    lines.append(tr("Runtime: %1").arg(PyrowaveRuntime::get().userMessage()));
    if (failure == Failure::NotCompiled || failure == Failure::LoaderUnavailable) {
        lines.append(tr("Vulkan loader: not available"));
    } else {
        lines.append(tr("Vulkan loader: %1 (%2)").arg(versionString(loaderVersion), loaderPath));
    }

    for (int i = 0; i < devices.size(); i++) {
        const PyrowaveGpuCapabilities& gpu = devices.at(i);
        const bool computeSubgroups = (gpu.subgroupStages & k_ShaderStageCompute) != 0;
        const QString operations = subgroupOperationNames(gpu.subgroupOperations);
        const QString sizeControl =
            tr("Subgroup size control: %1 (sizes %2 to %3, full compute subgroups: %4)");
        const QString storage = tr("8-bit storage buffers: %1, 16-bit integers: %2");
        const QString queue = tr("Graphics and compute queue: %1");
        const QString storage8Bit = yesNo(gpu.storageBuffer8BitAccess);

        lines.append(QString());
        lines.append(tr("GPU %1: %2 (%3)").arg(i + 1).arg(gpu.name, gpu.deviceType));
        lines.append(indent + tr("Vulkan version: %1").arg(versionString(gpu.apiVersion)));
        lines.append(indent + tr("Subgroup size: %1").arg(gpu.subgroupSize));
        lines.append(indent + tr("Subgroup operations: %1 (compute shaders: %2)")
                                  .arg(operations, yesNo(computeSubgroups)));
        lines.append(indent + sizeControl.arg(yesNo(gpu.subgroupSizeControl))
                                  .arg(gpu.minSubgroupSize)
                                  .arg(gpu.maxSubgroupSize)
                                  .arg(yesNo(gpu.computeFullSubgroups)));
        lines.append(indent + storage.arg(storage8Bit, yesNo(gpu.shaderInt16)));
        lines.append(indent + queue.arg(yesNo(gpu.graphicsComputeQueue)));

        const QStringList missing = gpu.missingPrerequisites();
        if (missing.isEmpty()) {
            lines.append(indent + tr("Result: basic requirements met"));
        } else {
            lines.append(indent + tr("Result: missing %1").arg(missing.join(QStringLiteral("; "))));
        }
    }

    lines.append(QString());
    lines.append(summary());
    lines.append(tr("This check only reads device properties. Starting a stream still checks "
                    "the window, image formats, resolution and HDR on the GPU that will be used."));
    return lines.join(QLatin1Char('\n'));
}

#ifndef HAVE_LIBPLACEBO_VULKAN

PyrowaveCapabilityReport probePyrowaveCapabilities()
{
    PyrowaveCapabilityReport report;
    report.failure = PyrowaveCapabilityReport::Failure::NotCompiled;
    return report;
}

#else

namespace {

constexpr uint32_t k_MinimumLoaderVersion = makeVersion(1, 1);

const char k_PortabilityEnumerationExtension[] = "VK_KHR_portability_enumeration";

// VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR, absent from older headers.
constexpr VkInstanceCreateFlags k_EnumeratePortabilityBit = 0x00000001;

QStringList vulkanLoaderCandidates()
{
    QStringList candidates;
#if defined(Q_OS_WIN32)
    candidates << QStringLiteral("vulkan-1.dll");
#elif defined(Q_OS_DARWIN)
    // The app bundle ships MoltenVK (and possibly a loader) in Frameworks.
    const QString appDir = QCoreApplication::applicationDirPath();
    const QDir frameworks(QDir::cleanPath(appDir + QStringLiteral("/../Frameworks")));
    const QStringList names = { QStringLiteral("libvulkan.1.dylib"),
                                QStringLiteral("libMoltenVK.dylib") };
    for (const QString& name : names) {
        const QString bundled = frameworks.absoluteFilePath(name);
        if (QFileInfo::exists(bundled)) {
            candidates << bundled;
        }
    }
    candidates << names;
#else
    candidates << QStringLiteral("libvulkan.so.1");
#endif
    return candidates;
}

bool hasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name)
{
    for (const VkExtensionProperties& extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

QString deviceTypeName(VkPhysicalDeviceType type)
{
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return CapabilityText::tr("integrated");
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return CapabilityText::tr("discrete");
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return CapabilityText::tr("virtual");
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return CapabilityText::tr("CPU");
    default:
        return CapabilityText::tr("other");
    }
}

struct InstanceFunctions
{
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    VkInstance instance = VK_NULL_HANDLE;

    PFN_vkDestroyInstance destroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices enumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties getPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 getPhysicalDeviceProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceFeatures getPhysicalDeviceFeatures = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 getPhysicalDeviceFeatures2 = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties getQueueFamilyProperties = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties enumerateDeviceExtensionProperties = nullptr;

    template <typename Fn> void load(Fn& target, const char* name)
    {
        target = reinterpret_cast<Fn>(getInstanceProcAddr(instance, name));
    }

    bool complete() const
    {
        return destroyInstance != nullptr && enumeratePhysicalDevices != nullptr &&
               getPhysicalDeviceProperties != nullptr && getPhysicalDeviceProperties2 != nullptr &&
               getPhysicalDeviceFeatures != nullptr && getPhysicalDeviceFeatures2 != nullptr &&
               getQueueFamilyProperties != nullptr && enumerateDeviceExtensionProperties != nullptr;
    }
};

std::vector<VkExtensionProperties> deviceExtensions(const InstanceFunctions& vk,
                                                    VkPhysicalDevice device)
{
    uint32_t count = 0;
    std::vector<VkExtensionProperties> extensions;
    if (vk.enumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS) {
        return extensions;
    }

    extensions.resize(count);
    VkResult result =
        vk.enumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data());
    if (result != VK_SUCCESS) {
        extensions.clear();
    }
    return extensions;
}

PyrowaveGpuCapabilities readDevice(const InstanceFunctions& vk, VkPhysicalDevice device)
{
    PyrowaveGpuCapabilities gpu;

    VkPhysicalDeviceProperties properties = {};
    vk.getPhysicalDeviceProperties(device, &properties);
    gpu.name = QString::fromUtf8(properties.deviceName);
    gpu.deviceType = deviceTypeName(properties.deviceType);
    gpu.apiVersion = properties.apiVersion;
    gpu.maxTexelBufferElements = properties.limits.maxTexelBufferElements;

    VkPhysicalDeviceFeatures features = {};
    vk.getPhysicalDeviceFeatures(device, &features);
    gpu.shaderInt16 = features.shaderInt16 == VK_TRUE;

    uint32_t familyCount = 0;
    vk.getQueueFamilyProperties(device, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vk.getQueueFamilyProperties(device, &familyCount, families.data());
    const VkQueueFlags graphicsCompute = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    for (const VkQueueFamilyProperties& family : families) {
        if (family.queueCount > 0 && (family.queueFlags & graphicsCompute) == graphicsCompute) {
            gpu.graphicsComputeQueue = true;
        }
    }

    // Subgroup properties are Vulkan 1.1 device features; a 1.0 device has
    // none of what PyroWave needs, and its report says so.
    if (properties.apiVersion < makeVersion(1, 1)) {
        return gpu;
    }

    const std::vector<VkExtensionProperties> extensions = deviceExtensions(vk, device);
    const bool hasSizeControl = properties.apiVersion >= makeVersion(1, 3) ||
                                hasExtension(extensions, "VK_EXT_subgroup_size_control");
    const bool has8BitStorage = properties.apiVersion >= makeVersion(1, 2) ||
                                hasExtension(extensions, "VK_KHR_8bit_storage");

    // The core 1.2/1.3 structures are aliases of these EXT/KHR ones with the
    // same sType values, and the aliases exist in older headers too.
    VkPhysicalDeviceSubgroupProperties subgroupProps = {};
    subgroupProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    VkPhysicalDeviceSubgroupSizeControlPropertiesEXT sizeProps = {};
    sizeProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2 properties2 = {};
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties2.pNext = &subgroupProps;
    if (hasSizeControl) {
        subgroupProps.pNext = &sizeProps;
    }
    vk.getPhysicalDeviceProperties2(device, &properties2);

    gpu.subgroupSize = subgroupProps.subgroupSize;
    gpu.subgroupOperations = subgroupProps.supportedOperations;
    gpu.subgroupStages = subgroupProps.supportedStages;
    if (hasSizeControl) {
        gpu.minSubgroupSize = sizeProps.minSubgroupSize;
        gpu.maxSubgroupSize = sizeProps.maxSubgroupSize;
        gpu.requiredSubgroupSizeStages = sizeProps.requiredSubgroupSizeStages;
    }

    VkPhysicalDeviceSubgroupSizeControlFeaturesEXT sizeFeatures = {};
    sizeFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES_EXT;
    VkPhysicalDevice8BitStorageFeaturesKHR storageFeatures = {};
    storageFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES_KHR;
    VkPhysicalDeviceFeatures2 features2 = {};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    void** next = &features2.pNext;
    if (hasSizeControl) {
        *next = &sizeFeatures;
        next = &sizeFeatures.pNext;
    }
    if (has8BitStorage) {
        *next = &storageFeatures;
    }
    vk.getPhysicalDeviceFeatures2(device, &features2);

    gpu.subgroupSizeControl = hasSizeControl && sizeFeatures.subgroupSizeControl == VK_TRUE;
    gpu.computeFullSubgroups = hasSizeControl && sizeFeatures.computeFullSubgroups == VK_TRUE;
    gpu.storageBuffer8BitAccess =
        has8BitStorage && storageFeatures.storageBuffer8BitAccess == VK_TRUE;

    return gpu;
}

PFN_vkGetInstanceProcAddr loadVulkanLoader(QString& loaderPath)
{
    // The loader stays loaded: some drivers misbehave when unloaded, and the
    // streaming renderer loads it again anyway.
    for (const QString& candidate : vulkanLoaderCandidates()) {
        QLibrary* loader = new QLibrary(candidate);
        if (loader->load()) {
            QFunctionPointer function = loader->resolve("vkGetInstanceProcAddr");
            if (function != nullptr) {
                loaderPath = loader->fileName();
                return reinterpret_cast<PFN_vkGetInstanceProcAddr>(function);
            }
        }
        delete loader;
    }
    return nullptr;
}

std::vector<VkExtensionProperties> instanceExtensions(PFN_vkGetInstanceProcAddr gipa)
{
    std::vector<VkExtensionProperties> extensions;
    auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        gipa(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
    if (enumerate == nullptr) {
        return extensions;
    }

    uint32_t count = 0;
    if (enumerate(nullptr, &count, nullptr) != VK_SUCCESS) {
        return extensions;
    }
    extensions.resize(count);
    if (enumerate(nullptr, &count, extensions.data()) != VK_SUCCESS) {
        extensions.clear();
    }
    return extensions;
}

}

PyrowaveCapabilityReport probePyrowaveCapabilities()
{
    using Failure = PyrowaveCapabilityReport::Failure;

    PyrowaveCapabilityReport report;
    report.failure = Failure::None;

    PFN_vkGetInstanceProcAddr gipa = loadVulkanLoader(report.loaderPath);
    if (gipa == nullptr) {
        report.failure = Failure::LoaderUnavailable;
        return report;
    }

    // vkEnumerateInstanceVersion is missing from Vulkan 1.0 loaders.
    report.loaderVersion = makeVersion(1, 0);
    auto enumerateInstanceVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        gipa(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    if (enumerateInstanceVersion != nullptr) {
        enumerateInstanceVersion(&report.loaderVersion);
    }
    if (report.loaderVersion < k_MinimumLoaderVersion) {
        report.failure = Failure::LoaderTooOld;
        return report;
    }

    auto createInstance =
        reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (createInstance == nullptr) {
        report.failure = Failure::QueryFailed;
        return report;
    }

    // Without this, the loader hides MoltenVK and other portability drivers.
    std::vector<const char*> enabledExtensions;
    VkInstanceCreateFlags instanceFlags = 0;
    if (hasExtension(instanceExtensions(gipa), k_PortabilityEnumerationExtension)) {
        enabledExtensions.push_back(k_PortabilityEnumerationExtension);
        instanceFlags |= k_EnumeratePortabilityBit;
    }

    VkApplicationInfo applicationInfo = {};
    applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    applicationInfo.pApplicationName = "Moonlight PyroWave capability check";
    applicationInfo.apiVersion = std::min(report.loaderVersion, k_RequiredDeviceApiVersion);

    VkInstanceCreateInfo instanceInfo = {};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.flags = instanceFlags;
    instanceInfo.pApplicationInfo = &applicationInfo;
    instanceInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    if (!enabledExtensions.empty()) {
        instanceInfo.ppEnabledExtensionNames = enabledExtensions.data();
    }

    InstanceFunctions vk;
    vk.getInstanceProcAddr = gipa;
    VkResult result = createInstance(&instanceInfo, nullptr, &vk.instance);
    if (result != VK_SUCCESS) {
        report.failure = Failure::InstanceCreationFailed;
        report.vkResult = static_cast<int>(result);
        return report;
    }

    vk.load(vk.destroyInstance, "vkDestroyInstance");
    vk.load(vk.enumeratePhysicalDevices, "vkEnumeratePhysicalDevices");
    vk.load(vk.getPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties");
    vk.load(vk.getPhysicalDeviceProperties2, "vkGetPhysicalDeviceProperties2");
    vk.load(vk.getPhysicalDeviceFeatures, "vkGetPhysicalDeviceFeatures");
    vk.load(vk.getPhysicalDeviceFeatures2, "vkGetPhysicalDeviceFeatures2");
    vk.load(vk.getQueueFamilyProperties, "vkGetPhysicalDeviceQueueFamilyProperties");
    vk.load(vk.enumerateDeviceExtensionProperties, "vkEnumerateDeviceExtensionProperties");

    if (vk.destroyInstance == nullptr) {
        // Nothing safe to do with the instance; leak it rather than crash.
        report.failure = Failure::QueryFailed;
        return report;
    }
    if (!vk.complete()) {
        report.failure = Failure::QueryFailed;
        vk.destroyInstance(vk.instance, nullptr);
        return report;
    }

    uint32_t deviceCount = 0;
    result = vk.enumeratePhysicalDevices(vk.instance, &deviceCount, nullptr);
    std::vector<VkPhysicalDevice> devices(deviceCount);
    if (result == VK_SUCCESS && deviceCount > 0) {
        result = vk.enumeratePhysicalDevices(vk.instance, &deviceCount, devices.data());
        devices.resize(deviceCount);
    }

    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        report.failure = Failure::QueryFailed;
        report.vkResult = static_cast<int>(result);
    } else if (devices.empty()) {
        report.failure = Failure::NoDevices;
    } else {
        // Each GPU is judged on its own; features are never combined.
        for (VkPhysicalDevice device : devices) {
            report.devices.append(readDevice(vk, device));
        }
    }

    vk.destroyInstance(vk.instance, nullptr);
    return report;
}

#endif
