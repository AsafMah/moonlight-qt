#pragma once

#include <QSize>
#include <QString>
#include <QVector>
#include <cstdint>

// Pure logic behind the in-stream "Stream quality" menu: the preset lists it
// shows and the decision of how a requested change is applied. No SDL, no
// session state, so it is unit-tested on its own (tests/stream_quality).
namespace StreamQuality {

// What the stream is running at (or should switch to).
struct Settings
{
    int width = 0;
    int height = 0;
    int fps = 0;
    int bitrateKbps = 0;

    QSize resolution() const { return QSize(width, height); }
    bool operator==(const Settings& other) const
    {
        return width == other.width && height == other.height && fps == other.fps &&
               bitrateKbps == other.bitrateKbps;
    }
    bool operator!=(const Settings& other) const { return !(*this == other); }
};

struct ResolutionPreset
{
    QSize size;
    bool native = false;  // the client display's native resolution
    bool current = false; // what the stream runs at now
};

struct FpsPreset
{
    int fps = 0;
    bool display = false; // the client display's refresh rate
    bool current = false;
};

// Native display first, then 4K/1440p/1080p/720p, largest first. The current
// resolution is added when it is not one of those. Duplicates are merged
// (so a 1440p display shows one row flagged both native and current) and
// presets larger than 16384 or empty are dropped.
QVector<ResolutionPreset> resolutionPresets(QSize nativeDisplay, QSize current);

// 30/60/90/120/144 plus the display refresh rate and the current FPS,
// ascending, merged. With a known display refresh rate, presets above it are
// dropped (the current FPS always stays).
QVector<FpsPreset> fpsPresets(int displayHz, int currentFps);

// Short name for a resolution: "4K", "1440p", "1080p", "720p", or "WxH".
QString resolutionName(QSize size);

struct Change
{
    bool resolution = false;
    bool fps = false;
    bool bitrate = false;

    bool any() const { return resolution || fps || bitrate; }
    // Resolution and FPS affect the stream configuration. An eligible
    // resolution-only change may still be requested in-band.
    bool needsRestart() const { return resolution || fps; }
};

Change diff(const Settings& from, const Settings& to);

// Bitrate for a restarted stream. When the user left bitrate on automatic it
// follows the new resolution/FPS (newDefaultKbps); otherwise their bitrate is
// kept.
int bitrateForSwitch(bool autoAdjustBitrate, int currentKbps, int newDefaultKbps);

// How a change is applied.
enum class Path
{
    None,           // nothing changed
    LiveBitrate,    // only bitrate: change it on the running stream
    LiveResolution, // host takes a client resolution request in-band
    Reconnect,      // stop and resume the stream with the new settings
    Busy,           // another stream/resolution operation owns the arbiter
};

struct LiveResolutionState
{
    bool hostSupported = false; // per-connection LI_FF_CLIENT_RESOLUTION_CHANGE bit
    QSize hostDisplaySize;      // last display size reported by this connection
    QSize streamSize;           // currently negotiated video dimensions
    bool busy = false;          // explicit request, window match, or reconnect in flight
};

bool validResolutionTarget(QSize target);
bool hostStreamingDisplayIsOneToOne(QSize hostDisplaySize, QSize streamSize);
bool canApplyLiveResolution(QSize target, const LiveResolutionState& state);

// FPS changes always reconnect. A same-FPS resolution change is Live only
// when the per-connection capability and known 1:1 display evidence are valid.
Path choosePath(const Change& change, const LiveResolutionState& state, QSize target);

enum class LiveResolutionResult
{
    Waiting,
    Applied,
    AppliedDifferentSize,
    TimedOut,
};

class LiveResolutionTransaction
{
public:
    static constexpr std::uint32_t ExplicitTimeoutMs = 12000;
    static constexpr std::uint32_t WindowMatchTimeoutMs = 5000;

    bool begin(std::uint32_t generation, QSize current, QSize target, std::uint32_t now,
               std::uint32_t timeoutMs, bool explicitRequest);
    bool isActive() const { return m_Active; }
    bool isExplicit() const { return m_ExplicitRequest; }
    QSize target() const { return m_Target; }
    QSize current() const { return m_Current; }
    QSize observedSize() const { return m_ObservedSize; }
    std::uint32_t generation() const { return m_Generation; }
    std::uint32_t deadline() const { return m_Deadline; }
    bool awaitingDecoderReset() const { return m_AwaitingDecoderReset; }

    bool observeHostReport(std::uint32_t generation, QSize previousDisplay, QSize reported);
    bool decoderResetCompleted(std::uint32_t generation);
    LiveResolutionResult firstFrame(std::uint32_t generation);
    bool expire(std::uint32_t now);
    void cancel();

private:
    bool m_Active = false;
    bool m_ExplicitRequest = false;
    bool m_AwaitingDecoderReset = false;
    bool m_DecoderResetCompleted = false;
    std::uint32_t m_Generation = 0;
    std::uint32_t m_Deadline = 0;
    QSize m_Current;
    QSize m_Target;
    QSize m_ObservedSize;
};

} // namespace StreamQuality
