#include <atomic>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

using Uint32 = std::uint32_t;

class QString
{
public:
    QString() = default;
    QString(const char* text) : value(text) {}
    QString arg(QString replacement) const
    {
        std::string result = value;
        const auto marker = result.find("%1");
        if (marker != std::string::npos) {
            result.replace(marker, 2, replacement.value);
        }
        return QString(result.c_str());
    }

    std::string value;
};

static QString tr(const char* text)
{
    return QString(text);
}

namespace OverlayMenuPanel {
static QString formatBitrateKbps(int bitrateKbps)
{
    return QString(std::to_string(bitrateKbps).c_str());
}
}

struct AbrRuntime
{
    std::uint64_t currentGeneration = 1;
    int bitrate = 0;
    std::uint64_t generation() const { return currentGeneration; }
    bool setBitrate(std::uint64_t generation, int value)
    {
        if (generation != currentGeneration) {
            return false;
        }
        bitrate = value;
        return true;
    }
};

struct Session
{
    struct StreamConfig
    {
        int bitrate = 0;
    } m_StreamConfig;
    struct QualityOverride
    {
        int bitrateKbps = 0;
    };

    AbrRuntime abr;
    AbrRuntime* m_AbrRuntime = &abr;
    std::optional<QualityOverride> m_StreamQualityOverride;
    std::atomic<Uint32> m_ConnectionGeneration{ 1 };
    std::atomic<bool> m_ConnectionCancel{ false };
    std::atomic_int m_PendingRuntimeBitrateKbps{ 0 };
    std::atomic_bool m_RuntimeBitrateInFlight{ true };
    std::atomic_int m_LastRuntimeBitrateApplied{ -1 };
    bool m_ShouldExit = false;
    bool m_SuppressConnectionErrorDialog = false;
    std::atomic<bool> m_ConnectionInterrupted{ false };
    std::atomic<bool> m_ReconnectInProgress{ false };
    int toasts = 0;
    int workersStarted = 0;

    void showStreamingToast(const QString&, int = 2000) { ++toasts; }
    void startRuntimeBitrateWorker()
    {
        ++workersStarted;
        m_PendingRuntimeBitrateKbps.exchange(0);
    }
    void finishRuntimeBitrateChange(int applied, Uint32 connectionGeneration,
                                    std::uint64_t abrGeneration);
};

// METHOD

static bool require(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return condition;
}

int main()
{
    bool ok = true;
    {
        Session session;
        session.m_StreamConfig.bitrate = 18000;
        session.m_StreamQualityOverride = Session::QualityOverride{ 18000 };
        session.finishRuntimeBitrateChange(24000, 1, 1);
        ok &= require(session.abr.bitrate == 24000 && session.m_StreamConfig.bitrate == 24000 &&
                          session.m_StreamQualityOverride->bitrateKbps == 24000 &&
                          !session.m_RuntimeBitrateInFlight,
                      "current completion commits the accepted bitrate to both config owners");
    }
    {
        Session session;
        session.m_StreamConfig.bitrate = 18000;
        session.m_StreamQualityOverride = Session::QualityOverride{ 18000 };
        session.m_ConnectionGeneration = 2;
        session.finishRuntimeBitrateChange(48000, 1, 1);
        ok &= require(session.abr.bitrate == 0 && session.m_StreamConfig.bitrate == 18000 &&
                          session.m_StreamQualityOverride->bitrateKbps == 18000 &&
                          !session.m_RuntimeBitrateInFlight,
                      "old connection completion cannot write config after host recovery");
    }
    {
        Session session;
        session.m_StreamConfig.bitrate = 18000;
        session.m_StreamQualityOverride = Session::QualityOverride{ 18000 };
        session.m_SuppressConnectionErrorDialog = true;
        session.finishRuntimeBitrateChange(48000, 1, 1);
        ok &= require(session.m_StreamConfig.bitrate == 18000 &&
                          session.m_StreamQualityOverride->bitrateKbps == 18000,
                      "completion cannot write config while reconnect owns the stream");
    }
    {
        Session session;
        session.m_StreamConfig.bitrate = 18000;
        session.m_StreamQualityOverride = Session::QualityOverride{ 18000 };
        session.m_ConnectionInterrupted = true;
        session.finishRuntimeBitrateChange(48000, 1, 1);
        ok &= require(session.m_StreamConfig.bitrate == 18000 &&
                          session.m_StreamQualityOverride->bitrateKbps == 18000,
                      "interrupted connection completion cannot write before recovery begins");
    }
    {
        Session session;
        session.m_StreamConfig.bitrate = 18000;
        session.m_StreamQualityOverride = Session::QualityOverride{ 18000 };
        session.m_ReconnectInProgress = true;
        session.finishRuntimeBitrateChange(48000, 1, 1);
        ok &= require(session.m_StreamConfig.bitrate == 18000 &&
                          session.m_StreamQualityOverride->bitrateKbps == 18000,
                      "reconnect worker completion cannot write stream configuration");
    }
    {
        Session session;
        session.m_StreamConfig.bitrate = 18000;
        session.m_StreamQualityOverride = Session::QualityOverride{ 18000 };
        session.m_ConnectionCancel = true;
        session.finishRuntimeBitrateChange(48000, 1, 1);
        ok &= require(session.m_StreamConfig.bitrate == 18000 &&
                          session.m_StreamQualityOverride->bitrateKbps == 18000,
                      "cancelled connection completion cannot write stream configuration");
    }
    {
        Session session;
        session.m_StreamConfig.bitrate = 18000;
        session.m_StreamQualityOverride = Session::QualityOverride{ 18000 };
        session.m_ShouldExit = true;
        session.finishRuntimeBitrateChange(48000, 1, 1);
        ok &= require(session.m_StreamConfig.bitrate == 18000 &&
                          session.m_StreamQualityOverride->bitrateKbps == 18000,
                      "exiting session completion cannot write stream configuration");
    }
    {
        Session session;
        session.m_StreamConfig.bitrate = 18000;
        session.m_StreamQualityOverride = Session::QualityOverride{ 18000 };
        session.abr.currentGeneration = 2;
        session.finishRuntimeBitrateChange(48000, 1, 1);
        ok &= require(session.abr.bitrate == 0 && session.m_StreamConfig.bitrate == 18000 &&
                          session.m_StreamQualityOverride->bitrateKbps == 18000,
                      "stale ABR generation cannot write stream configuration");
    }
    return ok ? 0 : 1;
}
