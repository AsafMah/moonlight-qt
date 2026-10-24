#pragma once

#include <QSize>
#include <QString>
#include <QVector>

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
    // Resolution and FPS are negotiated when the stream starts. Bitrate can
    // be changed on a running stream (Sunshine /bitrate).
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
};

// hostSupportsLiveResolution: the host and moonlight-common-c can change the
// resolution of a running stream. No released host can today (see
// Session::tryApplyStreamQualityLive()), but the decision lives here so the
// live path only has to flip that flag. Live FPS changes are not supported
// by any host, so an FPS change always reconnects.
Path choosePath(const Change& change, bool hostSupportsLiveResolution);

} // namespace StreamQuality
