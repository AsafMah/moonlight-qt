#include "streamquality.h"

#include <algorithm>

namespace StreamQuality {

namespace {
constexpr int kMaxDimension = 16384;

bool usableSize(QSize size)
{
    return size.width() > 0 && size.height() > 0 && size.width() <= kMaxDimension &&
           size.height() <= kMaxDimension;
}

qint64 pixelCount(QSize size)
{
    return qint64(size.width()) * size.height();
}
} // namespace

QVector<ResolutionPreset> resolutionPresets(QSize nativeDisplay, QSize current)
{
    QVector<ResolutionPreset> presets;
    auto add = [&presets](QSize size, bool native, bool isCurrent) {
        if (!usableSize(size)) {
            return;
        }
        for (ResolutionPreset& preset : presets) {
            if (preset.size == size) {
                preset.native |= native;
                preset.current |= isCurrent;
                return;
            }
        }
        presets.append({ size, native, isCurrent });
    };

    add(nativeDisplay, true, nativeDisplay == current);
    for (QSize standard :
         { QSize(3840, 2160), QSize(2560, 1440), QSize(1920, 1080), QSize(1280, 720) }) {
        add(standard, false, standard == current);
    }
    add(current, false, true);

    // Native stays on top; the rest go largest first.
    std::stable_sort(presets.begin(), presets.end(),
                     [](const ResolutionPreset& a, const ResolutionPreset& b) {
                         if (a.native != b.native) {
                             return a.native;
                         }
                         return pixelCount(a.size) > pixelCount(b.size);
                     });
    return presets;
}

QVector<FpsPreset> fpsPresets(int displayHz, int currentFps)
{
    QVector<FpsPreset> presets;
    auto add = [&presets](int fps, bool display, bool isCurrent) {
        if (fps <= 0 || fps > 1000) {
            return;
        }
        for (FpsPreset& preset : presets) {
            if (preset.fps == fps) {
                preset.display |= display;
                preset.current |= isCurrent;
                return;
            }
        }
        presets.append({ fps, display, isCurrent });
    };

    for (int fps : { 30, 60, 90, 120, 144 }) {
        add(fps, false, fps == currentFps);
    }
    add(displayHz, true, displayHz == currentFps);
    add(currentFps, false, true);

    // Streaming faster than the display can show is rarely wanted and makes
    // the menu long, so those presets go unless the stream already runs there.
    if (displayHz > 0) {
        presets.erase(std::remove_if(presets.begin(), presets.end(),
                                     [displayHz](const FpsPreset& preset) {
                                         return preset.fps > displayHz && !preset.current;
                                     }),
                      presets.end());
    }

    std::sort(presets.begin(), presets.end(),
              [](const FpsPreset& a, const FpsPreset& b) { return a.fps < b.fps; });
    return presets;
}

QString resolutionName(QSize size)
{
    if (size == QSize(3840, 2160)) {
        return QStringLiteral("4K");
    }
    if (size == QSize(2560, 1440) || size == QSize(1920, 1080) || size == QSize(1280, 720)) {
        return QStringLiteral("%1p").arg(size.height());
    }
    return QStringLiteral("%1%2%3").arg(size.width()).arg(QChar(0x00D7)).arg(size.height());
}

Change diff(const Settings& from, const Settings& to)
{
    Change change;
    change.resolution = from.width != to.width || from.height != to.height;
    change.fps = from.fps != to.fps;
    change.bitrate = to.bitrateKbps > 0 && from.bitrateKbps != to.bitrateKbps;
    return change;
}

int bitrateForSwitch(bool autoAdjustBitrate, int currentKbps, int newDefaultKbps)
{
    if (autoAdjustBitrate && newDefaultKbps > 0) {
        return newDefaultKbps;
    }
    return currentKbps > 0 ? currentKbps : newDefaultKbps;
}

Path choosePath(const Change& change, bool hostSupportsLiveResolution)
{
    if (!change.any()) {
        return Path::None;
    }
    if (change.fps) {
        return Path::Reconnect;
    }
    if (change.resolution) {
        return hostSupportsLiveResolution ? Path::LiveResolution : Path::Reconnect;
    }
    return Path::LiveBitrate;
}

} // namespace StreamQuality
