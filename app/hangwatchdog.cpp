#include "hangwatchdog.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "windowslogappend.h"
#include <dbghelp.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace HangWatchdog {
namespace {
constexpr LONG Version = 1;
#if defined(MOONLIGHT_HANG_WATCHDOG_TESTING) && defined(MOONLIGHT_HANG_TEST_THRESHOLD_MS)
constexpr DWORD ThresholdMs = MOONLIGHT_HANG_TEST_THRESHOLD_MS;
#else
constexpr DWORD ThresholdMs = 12000;
#endif
constexpr ULONGLONG Threshold = ThresholdMs * 10000ULL;
#if defined(MOONLIGHT_HANG_WATCHDOG_TESTING) && defined(MOONLIGHT_HANG_TEST_WRITER_TIMEOUT_MS)
constexpr DWORD WriterTimeoutMs = MOONLIGHT_HANG_TEST_WRITER_TIMEOUT_MS;
#else
constexpr DWORD WriterTimeoutMs = 30000;
#endif
constexpr int SlotCount = 64;
struct Slot
{
    volatile LONG owner;
    volatile LONG sequence;
    char stage[96];
};
struct Shared
{
    LONG version;
    DWORD pid;
    DWORD mainThread;
    volatile LONG stopped;
    volatile LONG writerTimedOut;
    volatile LONG sdl;
    alignas(8) volatile LONG64 heartbeat;
    Slot slots[SlotCount];
    wchar_t directory[32768];
    wchar_t log[32768];
};
Shared* state;
HANDLE mapping;
HANDLE helper;
bool helperExitReported;

ULONGLONG now()
{
    ULONGLONG value = 0;
    QueryUnbiasedInterruptTime(&value);
    return value;
}

void logLine(const Shared* shared, const std::string& message)
{
    const std::wstring fallback =
        std::wstring(shared->directory) + L"\\Moonlight-hang-watchdog.log";
    std::wstring path = shared->log[0] ? shared->log : fallback;
    std::string line = "Hang watchdog: " + message + "\r\n";
    for (int attempt = 0; attempt < 2; ++attempt) {
        HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            bool written = appendWindowsLog(file, line.data(), static_cast<DWORD>(line.size()));
            DWORD code = GetLastError();
            CloseHandle(file);
            if (written)
                return;
            SetLastError(code);
        }
        line = "Hang watchdog: diagnostic append failed, Win32 error " +
               std::to_string(GetLastError()) + "; " + message + "\r\n";
        OutputDebugStringA(line.c_str());
        if (path == fallback)
            break;
        path = fallback;
    }
}

std::string utf8(const std::wstring& text)
{
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr,
                                   0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size,
                        nullptr, nullptr);
    return result;
}

void error(const Shared* shared, const char* operation)
{
    DWORD code = GetLastError();
    logLine(shared, std::string(operation) + " failed, Win32 error " + std::to_string(code));
}

void beat()
{
    if (state && !state->stopped) {
        InterlockedExchange64(&state->heartbeat, static_cast<LONG64>(now()));
        if (helper && !helperExitReported && WaitForSingleObject(helper, 0) == WAIT_OBJECT_0) {
            logLine(state, state->writerTimedOut
                               ? "dump writer timed out; helper exited, target was not interrupted"
                               : "helper exited unexpectedly; continuing without hang snapshots");
            helperExitReported = true;
        }
    }
}

void publish(Slot& slot, const char* name)
{
    InterlockedIncrement(&slot.sequence);
    strncpy_s(slot.stage, name, _TRUNCATE);
    InterlockedIncrement(&slot.sequence);
}

std::string breadcrumbs(Shared* shared)
{
    bool mainStage = false;
    std::string text =
        "Moonlight hang snapshot v1\r\npid=" + std::to_string(shared->pid) +
        "\r\nactive_loop=" + (InterlockedCompareExchange(&shared->sdl, 0, 0) ? "SDL" : "Qt") +
        "\r\nthreshold_ms=" + std::to_string(ThresholdMs) +
        "\r\nmain_thread=" + std::to_string(shared->mainThread) + "\r\n";
    for (auto& slot : shared->slots) {
        LONG owner = InterlockedCompareExchange(&slot.owner, 0, 0);
        if (!owner)
            continue;
        // A writer may be frozen mid-update. Never wait for its lock or seqlock.
        LONG before = InterlockedCompareExchange(&slot.sequence, 0, 0);
        char name[96];
        memcpy(name, slot.stage, sizeof(name));
        MemoryBarrier();
        LONG after = InterlockedCompareExchange(&slot.sequence, 0, 0);
        name[95] = 0;
        if (owner != InterlockedCompareExchange(&slot.owner, 0, 0))
            continue;
        if (static_cast<DWORD>(owner) == shared->mainThread)
            mainStage = true;
        text += "thread=" + std::to_string(static_cast<DWORD>(owner)) + " stage=" +
                (before == after && !(after & 1) ? name : "<stage update in progress>") + "\r\n";
    }
    if (!mainStage) {
        text += "thread=" + std::to_string(shared->mainThread) +
                " stage=" + (shared->sdl ? "SDL event loop" : "Qt event dispatch") + "\r\n";
    }
    return text;
}

void prune(Shared* shared)
{
    std::wstring base = std::wstring(shared->directory) + L"\\";
    WIN32_FIND_DATAW entry;
    HANDLE search = FindFirstFileW((base + L"Moonlight-hang-v1-*.dmp").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE)
        return;
    std::vector<std::wstring> names;
    do {
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            names.emplace_back(entry.cFileName);
    } while (FindNextFileW(search, &entry));
    FindClose(search);
    std::sort(names.rbegin(), names.rend());
    for (size_t i = 3; i < names.size(); ++i) {
        std::wstring path = base + names[i];
        if (!DeleteFileW(path.c_str()))
            error(shared, "prune hang dump");
        path.replace(path.size() - 4, 4, L".txt");
        if (!DeleteFileW(path.c_str()))
            error(shared, "prune hang breadcrumb");
    }
}

struct Capture
{
    Shared* shared;
    HANDLE parent;
    std::string text;
};

DWORD WINAPI capture(void* opaque)
{
    auto& request = *static_cast<Capture*>(opaque);
    Shared* shared = request.shared;
#ifdef MOONLIGHT_HANG_WATCHDOG_TESTING
    wchar_t blocked[2];
    if (GetEnvironmentVariableW(L"MOONLIGHT_HANG_TEST_BLOCK_WRITER", blocked, _countof(blocked))) {
        logLine(shared, "test-only blocked dump writer");
        Sleep(INFINITE);
    }
#endif
    FILETIME time;
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER stamp;
    stamp.LowPart = time.dwLowDateTime;
    stamp.HighPart = time.dwHighDateTime;
    wchar_t name[128];
    swprintf_s(name, L"\\Moonlight-hang-v1-%020llu-%lu", stamp.QuadPart, shared->pid);
    std::wstring base = std::wstring(shared->directory) + name;
    std::wstring dump = base + L".dmp";
    std::wstring text = base + L".txt";
    std::wstring pendingDump = dump + L".partial";
    std::wstring pendingText = text + L".partial";
    HANDLE file = CreateFileW(pendingDump.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = false;
    if (file != INVALID_HANDLE_VALUE) {
        ok = MiniDumpWriteDump(request.parent, shared->pid, file, MiniDumpWithThreadInfo, nullptr,
                               nullptr, nullptr) != FALSE;
        if (!ok)
            error(shared, "MiniDumpWriteDump");
        CloseHandle(file);
    } else
        error(shared, "create hang dump");
    if (ok) {
        file = CreateFileW(pendingText.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        ok = file != INVALID_HANDLE_VALUE;
        if (ok) {
            DWORD written = 0;
            ok = WriteFile(file, request.text.data(), static_cast<DWORD>(request.text.size()),
                           &written, nullptr) &&
                 written == request.text.size();
            if (!ok)
                error(shared, "write hang breadcrumb");
            CloseHandle(file);
        } else
            error(shared, "create hang breadcrumb");
    }
    if (ok) {
        ok = MoveFileExW(pendingText.c_str(), text.c_str(), MOVEFILE_WRITE_THROUGH) &&
             MoveFileExW(pendingDump.c_str(), dump.c_str(), MOVEFILE_WRITE_THROUGH);
        if (!ok)
            error(shared, "publish hang pair");
    }
    if (ok) {
        logLine(shared, "snapshot " + utf8(dump) + " (local diagnostic; may contain private data)");
        prune(shared);
    } else {
        DeleteFileW(pendingDump.c_str());
        DeleteFileW(pendingText.c_str());
        DeleteFileW(text.c_str());
    }
    return 0;
}
}

int childMain(int argc, char** argv)
{
    if (argc < 2 || strcmp(argv[1], "--hang-watchdog"))
        return -1;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc != 4)
        return 2;
    HANDLE inheritedMapping = reinterpret_cast<HANDLE>(_strtoui64(argv[2], nullptr, 10));
    HANDLE parent = reinterpret_cast<HANDLE>(_strtoui64(argv[3], nullptr, 10));
    auto shared = static_cast<Shared*>(
        MapViewOfFile(inheritedMapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Shared)));
    if (!shared)
        return 3;
    if (shared->version != Version || GetProcessId(parent) != shared->pid)
        return 4;
    LONG64 captured = 0;
    for (;;) {
        if (WaitForSingleObject(parent, 100) != WAIT_TIMEOUT || shared->stopped)
            break;
        LONG64 heartbeat = InterlockedCompareExchange64(&shared->heartbeat, 0, 0);
        if (!heartbeat || heartbeat == captured || now() - heartbeat < Threshold)
            continue;
        captured = heartbeat;
        Capture request{ shared, parent, breadcrumbs(shared) };
        HANDLE worker = CreateThread(nullptr, 0, capture, &request, 0, nullptr);
        if (!worker) {
            error(shared, "create dump writer");
            continue;
        }
        ULONGLONG started = now();
        HANDLE waits[] = { parent, worker };
        for (;;) {
            DWORD result = WaitForMultipleObjects(2, waits, FALSE, 100);
            if (result == WAIT_OBJECT_0 + 1)
                break;
            const bool timedOut = now() - started > WriterTimeoutMs * 10000ULL;
            if (result == WAIT_OBJECT_0 || shared->stopped || timedOut) {
                if (timedOut)
                    InterlockedExchange(&shared->writerTimedOut, 1);
                // Force-exit this helper only. ExitProcess alone can deadlock
                // in DLL detach if the dump writer holds a DbgHelp/loader lock.
                // The inherited target handle has no termination/suspension rights.
                // Do not log here: a blocked filesystem must not hold up shutdown.
                TerminateProcess(GetCurrentProcess(), 0);
                ExitProcess(0);
            }
        }
        CloseHandle(worker);
    }
    UnmapViewOfFile(shared);
    CloseHandle(inheritedMapping);
    CloseHandle(parent);
    return 0;
}

bool start(const wchar_t* directory, const wchar_t* log)
{
    if (state)
        return true;
    SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
    mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, sizeof(Shared),
                                 nullptr);
    if (!mapping) {
        OutputDebugStringA("Hang watchdog: CreateFileMapping failed\n");
        return false;
    }
    state = static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!state) {
        CloseHandle(mapping);
        mapping = nullptr;
        return false;
    }
    state->version = Version;
    state->pid = GetCurrentProcessId();
    state->mainThread = GetCurrentThreadId();
    wcsncpy_s(state->directory, directory, _TRUNCATE);
    wcsncpy_s(state->log, log, _TRUNCATE);
    HANDLE parent =
        OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, TRUE, state->pid);
    wchar_t executable[32768];
    DWORD length = GetModuleFileNameW(nullptr, executable, _countof(executable));
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    bool attributes =
        InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes) != FALSE;
    HANDLE handles[] = { mapping, parent };
    bool ready =
        parent && length && length < _countof(executable) && attributes &&
        UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  handles, sizeof(handles), nullptr, nullptr);
    PROCESS_INFORMATION process{};
    if (ready) {
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --hang-watchdog " +
                               std::to_wstring(reinterpret_cast<uintptr_t>(mapping)) + L" " +
                               std::to_wstring(reinterpret_cast<uintptr_t>(parent));
#ifdef MOONLIGHT_HANG_WATCHDOG_TESTING
        wchar_t failStart[2];
        if (GetEnvironmentVariableW(L"MOONLIGHT_HANG_TEST_FAIL_START", failStart,
                                    _countof(failStart))) {
            SetLastError(ERROR_FILE_NOT_FOUND);
            ready = false;
        } else
#endif
            ready = CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
                                   EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr,
                                   nullptr, &startup.StartupInfo, &process) != FALSE;
    }
    if (!ready)
        error(state, "start hidden helper");
    if (attributes)
        DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (parent)
        CloseHandle(parent);
    if (!ready) {
        stop();
        return false;
    }
    CloseHandle(process.hThread);
    helper = process.hProcess;
    helperExitReported = false;
    return true;
}

void stop()
{
    if (state) {
        InterlockedExchange(&state->stopped, 1);
        // Stage scopes on decoder/render threads may still be unwinding.
        // Keep this small process-lifetime view valid until the OS tears it down.
    }
    if (mapping) {
        CloseHandle(mapping);
        mapping = nullptr;
    }
    if (helper) {
        CloseHandle(helper);
        helper = nullptr;
    }
}
void qtHeartbeat()
{
    if (state && !InterlockedCompareExchange(&state->sdl, 0, 0))
        beat();
}
void sdlHeartbeat()
{
    beat();
}
SdlScope::SdlScope()
{
    if (state)
        InterlockedIncrement(&state->sdl);
    beat();
}
SdlScope::~SdlScope()
{
    beat();
    if (state)
        InterlockedDecrement(&state->sdl);
}
Stage::Stage(const char* name)
{
    if (!state || state->stopped)
        return;
    LONG tid = static_cast<LONG>(GetCurrentThreadId());
    for (int i = 0; i < SlotCount; ++i) {
        if (InterlockedCompareExchange(&state->slots[i].owner, 0, 0) == tid) {
            m_Slot = i;
            break;
        }
    }
    if (m_Slot < 0) {
        for (int i = 0; i < SlotCount; ++i) {
            if (InterlockedCompareExchange(&state->slots[i].owner, tid, 0) == 0) {
                m_Slot = i;
                break;
            }
        }
    }
    if (m_Slot >= 0) {
        strcpy_s(m_Previous, state->slots[m_Slot].stage);
        publish(state->slots[m_Slot], name);
    }
}
Stage::~Stage()
{
    if (state && m_Slot >= 0) {
        auto& slot = state->slots[m_Slot];
        publish(slot, m_Previous);
        if (!m_Previous[0])
            InterlockedExchange(&slot.owner, 0);
    }
}
#ifdef MOONLIGHT_HANG_WATCHDOG_TESTING
unsigned long helperProcessId()
{
    return helper ? GetProcessId(helper) : 0;
}
#endif
}
#else
namespace HangWatchdog {
int childMain(int, char**)
{
    return -1;
}
bool start(const wchar_t*, const wchar_t*)
{
    return false;
}
void stop() {}
void qtHeartbeat() {}
void sdlHeartbeat() {}
SdlScope::SdlScope() {}
SdlScope::~SdlScope() {}
Stage::Stage(const char*) {}
Stage::~Stage() {}
}
#endif
