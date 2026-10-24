#include "pyrowaveruntime.h"

#include "SDL_compat.h"

#ifdef HAVE_LIBPLACEBO_VULKAN
// Checks pyrowaveabi.h against third-party/pyrowave/pyrowave.h when the
// submodule is checked out. Only this file may see both declarations.
#define PYROWAVE_ABI_CHECK
#include "pyrowaveabi.h"
#endif

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QStringList>

namespace {

// pyrowave.h: void pyrowave_get_api_version(uint32_t*, uint32_t*, uint32_t*).
// This entry point is stable across API versions by contract.
using GetApiVersionFn = void (*)(uint32_t*, uint32_t*, uint32_t*);

// Names follow the library file names that the PyroWave CMake build
// produces (SOVERSION 101). See third-party/pyrowave-runtime.
#if defined(Q_OS_WIN32)
const char k_LibraryName[] = "libpyrowave-shared-101.dll";
#elif defined(Q_OS_DARWIN)
const char k_LibraryName[] = "libpyrowave-shared.101.dylib";
#else
const char k_LibraryName[] = "libpyrowave-shared.so.101";
#endif

struct EntryPointInfo
{
    PyrowaveRuntime::EntryPoint id;
    const char* symbol;
};

const EntryPointInfo k_EntryPoints[] = {
    { PyrowaveRuntime::EntryPoint::GetApiVersion, "pyrowave_get_api_version" },
    { PyrowaveRuntime::EntryPoint::CreateDevice, "pyrowave_create_device" },
    { PyrowaveRuntime::EntryPoint::DeviceDestroy, "pyrowave_device_destroy" },
    { PyrowaveRuntime::EntryPoint::DeviceSetQueueType, "pyrowave_device_set_queue_type" },
    { PyrowaveRuntime::EntryPoint::DeviceSetCommandBuffer, "pyrowave_device_set_command_buffer" },
    { PyrowaveRuntime::EntryPoint::DecoderDevicePrefersFragmentPath,
      "pyrowave_decoder_device_prefers_fragment_path" },
    { PyrowaveRuntime::EntryPoint::DecoderCreate, "pyrowave_decoder_create" },
    { PyrowaveRuntime::EntryPoint::DecoderClear, "pyrowave_decoder_clear" },
    { PyrowaveRuntime::EntryPoint::DecoderPushPacket, "pyrowave_decoder_push_packet" },
    { PyrowaveRuntime::EntryPoint::DecoderDecodeIsReady, "pyrowave_decoder_decode_is_ready" },
    { PyrowaveRuntime::EntryPoint::DecoderDecodeGpuBuffer, "pyrowave_decoder_decode_gpu_buffer" },
    { PyrowaveRuntime::EntryPoint::DecoderDecodeCpuBufferSynchronous,
      "pyrowave_decoder_decode_cpu_buffer_synchronous" },
    { PyrowaveRuntime::EntryPoint::DecoderGetColorMetadata, "pyrowave_decoder_get_color_metadata" },
    { PyrowaveRuntime::EntryPoint::DecoderSetYcbcrScalingFactor,
      "pyrowave_decoder_set_ycbcr_scaling_factor" },
    { PyrowaveRuntime::EntryPoint::DecoderDestroy, "pyrowave_decoder_destroy" },
};

static_assert(sizeof(k_EntryPoints) / sizeof(k_EntryPoints[0]) ==
                  static_cast<size_t>(PyrowaveRuntime::EntryPoint::Count),
              "Every PyroWave entry point needs a symbol name");

QStringList candidatePaths()
{
    QStringList candidates;

    // Development override: an absolute path to a runtime build.
    const QString overridePath = qEnvironmentVariable("MOONLIGHT_PYROWAVE_LIBRARY");
    if (!overridePath.isEmpty()) {
        candidates.append(overridePath);
        return candidates;
    }

    const QString name = QString::fromLatin1(k_LibraryName);
    const QString appDir = QCoreApplication::applicationDirPath();

    // Look next to the app first so a bundled copy always wins over anything
    // installed on the system. dlopen() uses the RUNPATH of the caller, which
    // is QtCore rather than Moonlight, so the AppImage's usr/lib is listed
    // explicitly.
    QStringList directories;
#if defined(Q_OS_WIN32)
    directories << appDir;
#elif defined(Q_OS_DARWIN)
    directories << appDir + QStringLiteral("/../Frameworks") << appDir;
#else
    directories << appDir + QStringLiteral("/../lib") << appDir;
#endif

    for (const QString& directory : directories) {
        const QString path = QDir::cleanPath(QDir(directory).absoluteFilePath(name));
        if (QFileInfo::exists(path)) {
            candidates.append(path);
        }
    }

    // Finally let the platform loader search its usual locations.
    candidates.append(name);
    return candidates;
}

}

const PyrowaveRuntime& PyrowaveRuntime::get()
{
    // Function-local statics are initialized exactly once, even when the
    // first calls race (settings page and session start).
    static PyrowaveRuntime runtime;
    return runtime;
}

PyrowaveRuntime::PyrowaveRuntime()
    : m_Status(Status::LibraryMissing), m_Library(nullptr), m_ApiMajor(0), m_ApiMinor(0),
      m_ApiPatch(0)
{
    for (QFunctionPointer& entryPoint : m_EntryPoints) {
        entryPoint = nullptr;
    }

    load();
}

bool PyrowaveRuntime::tryCandidate(const QString& candidate)
{
    QLibrary* library = new QLibrary(candidate);
    if (!library->load()) {
        const QString error = library->errorString();
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "PyroWave runtime not loaded from %s: %s",
                    qUtf8Printable(candidate), qUtf8Printable(error));
        delete library;
        return false;
    }

    // Never unloaded: decoders may hold entry points until process exit.
    m_Library = library;
    m_LibraryPath = library->fileName();
    return true;
}

void PyrowaveRuntime::load()
{
    const QStringList candidates = candidatePaths();
    for (const QString& candidate : candidates) {
        // Stop at the first library that loads, even if it is rejected below.
        // A bundled copy with the wrong version should be reported rather
        // than silently replaced by a different system copy.
        if (tryCandidate(candidate)) {
            break;
        }
    }

    if (m_Library == nullptr) {
        m_Status = Status::LibraryMissing;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "%s", qUtf8Printable(diagnosticMessage()));
        return;
    }

    QFunctionPointer versionFunction = m_Library->resolve(k_EntryPoints[0].symbol);
    auto getApiVersion = reinterpret_cast<GetApiVersionFn>(versionFunction);
    if (getApiVersion == nullptr) {
        m_Status = Status::VersionEntryPointMissing;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", qUtf8Printable(diagnosticMessage()));
        return;
    }

    getApiVersion(&m_ApiMajor, &m_ApiMinor, &m_ApiPatch);
    if (m_ApiMajor != k_RequiredApiMajor || m_ApiMinor != k_RequiredApiMinor ||
        m_ApiPatch != k_RequiredApiPatch) {
        m_Status = Status::VersionMismatch;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", qUtf8Printable(diagnosticMessage()));
        return;
    }

    QFunctionPointer resolved[static_cast<size_t>(EntryPoint::Count)] = {};
    for (const EntryPointInfo& info : k_EntryPoints) {
        QFunctionPointer function = m_Library->resolve(info.symbol);
        if (function == nullptr) {
            m_MissingEntryPoint = QString::fromLatin1(info.symbol);
            m_Status = Status::EntryPointMissing;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", qUtf8Printable(diagnosticMessage()));
            return;
        }
        resolved[static_cast<size_t>(info.id)] = function;
    }

    // Publish the table only once it is complete.
    for (size_t i = 0; i < static_cast<size_t>(EntryPoint::Count); i++) {
        m_EntryPoints[i] = resolved[i];
    }
    m_Status = Status::Available;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "%s", qUtf8Printable(diagnosticMessage()));
}

QString PyrowaveRuntime::apiVersionString() const
{
    return QStringLiteral("%1.%2.%3").arg(m_ApiMajor).arg(m_ApiMinor).arg(m_ApiPatch);
}

static QString requiredApiVersionString()
{
    const uint32_t major = PyrowaveRuntime::k_RequiredApiMajor;
    const uint32_t minor = PyrowaveRuntime::k_RequiredApiMinor;
    const uint32_t patch = PyrowaveRuntime::k_RequiredApiPatch;
    return QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(patch);
}

QString PyrowaveRuntime::diagnosticMessage() const
{
    switch (m_Status) {
    case Status::Available:
        return QStringLiteral("PyroWave runtime %1 loaded from %2")
            .arg(apiVersionString(), m_LibraryPath);
    case Status::LibraryMissing:
        return QStringLiteral("PyroWave runtime (%1) not found; PyroWave is unavailable")
            .arg(QString::fromLatin1(k_LibraryName));
    case Status::VersionEntryPointMissing:
        return QStringLiteral("PyroWave runtime at %1 has no pyrowave_get_api_version(); rejected")
            .arg(m_LibraryPath);
    case Status::VersionMismatch:
        return QStringLiteral("PyroWave runtime at %1 reports API %2, but %3 is required; rejected")
            .arg(m_LibraryPath, apiVersionString(), requiredApiVersionString());
    case Status::EntryPointMissing:
        return QStringLiteral("PyroWave runtime at %1 is missing %2(); rejected")
            .arg(m_LibraryPath, m_MissingEntryPoint);
    }

    return QString();
}

QString PyrowaveRuntime::userMessage() const
{
    switch (m_Status) {
    case Status::Available:
        return tr("PyroWave runtime %1 is installed.").arg(apiVersionString());
    case Status::LibraryMissing:
        return tr("This build doesn't include the PyroWave runtime.");
    case Status::VersionEntryPointMissing:
        return tr("The installed PyroWave runtime isn't a supported build.");
    case Status::VersionMismatch:
        return tr("The installed PyroWave runtime is version %1, but this build needs %2.")
            .arg(apiVersionString(), requiredApiVersionString());
    case Status::EntryPointMissing:
        return tr("The installed PyroWave runtime is incomplete (%1).").arg(m_MissingEntryPoint);
    }

    return QString();
}

const char* PyrowaveRuntime::entryPointName(EntryPoint which)
{
    for (const EntryPointInfo& info : k_EntryPoints) {
        if (info.id == which) {
            return info.symbol;
        }
    }

    return "";
}

QFunctionPointer PyrowaveRuntime::rawEntryPoint(EntryPoint which) const
{
    if (m_Status != Status::Available || which == EntryPoint::Count) {
        return nullptr;
    }

    return m_EntryPoints[static_cast<size_t>(which)];
}
