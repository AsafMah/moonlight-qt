#pragma once

#include <QCoreApplication>
#include <QList>
#include <QString>
#include <QStringList>

#include <cstdint>

// What one Vulkan GPU reports for the features the PyroWave decoder needs.
// Field meanings match VkPhysicalDeviceSubgroupProperties,
// VkPhysicalDeviceSubgroupSizeControl{Features,Properties} and
// VkPhysicalDevice8BitStorageFeatures.
struct PyrowaveGpuCapabilities
{
    Q_DECLARE_TR_FUNCTIONS(PyrowaveCapabilities)

public:
    QString name;
    QString deviceType;
    uint32_t apiVersion = 0;
    uint32_t subgroupSize = 0;
    uint32_t subgroupOperations = 0;
    uint32_t subgroupStages = 0;
    bool subgroupSizeControl = false;
    bool computeFullSubgroups = false;
    uint32_t minSubgroupSize = 0;
    uint32_t maxSubgroupSize = 0;
    uint32_t requiredSubgroupSizeStages = 0;
    bool storageBuffer8BitAccess = false;
    uint32_t maxTexelBufferElements = 0;
    bool shaderInt16 = false;
    bool graphicsComputeQueue = false;

    // Translated names of the prerequisites this GPU lacks. Empty when the
    // basic checks pass. Passing is necessary but not sufficient: the stream
    // preflight still checks formats, the window, the size and HDR.
    QStringList missingPrerequisites() const;
    bool meetsPrerequisites() const { return missingPrerequisites().isEmpty(); }
};

struct PyrowaveCapabilityReport
{
    Q_DECLARE_TR_FUNCTIONS(PyrowaveCapabilities)

public:
    // A failure means the check couldn't read the GPUs. That is reported
    // differently from a GPU that was read and lacks something.
    enum class Failure
    {
        None,
        NotCompiled,
        LoaderUnavailable,
        LoaderTooOld,
        InstanceCreationFailed,
        QueryFailed,
        NoDevices,
    };

    Failure failure = Failure::NotCompiled;
    int vkResult = 0;
    uint32_t loaderVersion = 0;
    QString loaderPath;
    QList<PyrowaveGpuCapabilities> devices;

    bool anyDeviceMeetsPrerequisites() const;

    // One translated line for the settings chip.
    QString summary() const;

    // The full translated report for the capability check dialog.
    QString toText() const;
};

// Reads Vulkan properties for each GPU with a private VkInstance. It creates
// no logical device, surface or decoder, runs no shaders and changes no
// settings. Blocking: call it off the GUI thread.
PyrowaveCapabilityReport probePyrowaveCapabilities();
