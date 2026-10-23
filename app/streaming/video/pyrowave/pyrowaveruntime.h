#pragma once

#include <QCoreApplication>
#include <QString>

#include <cstddef>
#include <cstdint>

class QLibrary;

// Loads the PyroWave C API runtime (libpyrowave-shared) on first use.
//
// Moonlight never links to the runtime. Builds and installs without it keep
// working; PyroWave is then hidden in the UI and never negotiated. The
// runtime must be the AlkaidLab fork's C API line, exactly 101.0.0, which is
// what the Moonlight V+ client contract pins. Upstream 1.x is rejected.
//
// This class only resolves symbols. Typed declarations for the decoder live
// with the decoder (PW-2), which needs the Vulkan headers anyway.
class PyrowaveRuntime
{
    Q_DECLARE_TR_FUNCTIONS(PyrowaveRuntime)

public:
    static constexpr uint32_t k_RequiredApiMajor = 101;
    static constexpr uint32_t k_RequiredApiMinor = 0;
    static constexpr uint32_t k_RequiredApiPatch = 0;

    enum class Status
    {
        Available,
        LibraryMissing,
        VersionEntryPointMissing,
        VersionMismatch,
        EntryPointMissing,
    };

    // Every entry point the decoder path needs. All of them are checked at
    // load time, so a runtime that loads is complete.
    enum class EntryPoint
    {
        GetApiVersion,
        CreateDevice,
        DeviceDestroy,
        DeviceSetQueueType,
        DeviceSetCommandBuffer,
        DecoderDevicePrefersFragmentPath,
        DecoderCreate,
        DecoderClear,
        DecoderPushPacket,
        DecoderDecodeIsReady,
        DecoderDecodeGpuBuffer,
        DecoderDecodeCpuBufferSynchronous,
        DecoderGetColorMetadata,
        DecoderSetYcbcrScalingFactor,
        DecoderDestroy,
        Count
    };

    // Thread-safe. The first call loads the library; later calls return the
    // cached result. The library stays loaded for the life of the process.
    static const PyrowaveRuntime& get();

    bool isAvailable() const { return m_Status == Status::Available; }
    Status status() const { return m_Status; }

    // The file that was loaded (or rejected), empty if none was found.
    QString libraryPath() const { return m_LibraryPath; }

    // Version reported by the library, or 0.0.0 if it could not be read.
    QString apiVersionString() const;

    // English, for logs.
    QString diagnosticMessage() const;

    // Translated, for the settings page and launch errors.
    QString userMessage() const;

    static const char* entryPointName(EntryPoint which);

    // Returns null unless isAvailable(). Callers cast to the matching
    // pyrowave.h function type.
    template <typename Fn>
    Fn entryPoint(EntryPoint which) const { return reinterpret_cast<Fn>(rawEntryPoint(which)); }

    QFunctionPointer rawEntryPoint(EntryPoint which) const;

private:
    PyrowaveRuntime();
    ~PyrowaveRuntime() = default;
    PyrowaveRuntime(const PyrowaveRuntime&) = delete;
    PyrowaveRuntime& operator=(const PyrowaveRuntime&) = delete;

    void load();
    bool tryCandidate(const QString& candidate);

    Status m_Status;
    QLibrary* m_Library;
    QString m_LibraryPath;
    QString m_MissingEntryPoint;
    uint32_t m_ApiMajor;
    uint32_t m_ApiMinor;
    uint32_t m_ApiPatch;
    QFunctionPointer m_EntryPoints[static_cast<size_t>(EntryPoint::Count)];
};
