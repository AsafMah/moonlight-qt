#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

template <typename T> struct QVector : std::vector<T>
{
    QVector(std::initializer_list<T> values) : std::vector<T>(values) {}
    bool isEmpty() const { return this->empty(); }
};

#define Q_UNUSED(value) (void)value
#define SDL_LOG_CATEGORY_APPLICATION 0

class QString
{
public:
    QString() = default;
    QString(const char* text) : value(text) {}
    std::string value;
};

struct SDL_Window
{
};

namespace StreamingPreferences {
enum VideoDecoderSelection
{
    VDS_AUTO,
    VDS_FORCE_SOFTWARE,
    VDS_FORCE_HARDWARE
};
enum VideoCodecConfig
{
    VCC_AUTO,
    VCC_FORCE_H264,
    VCC_FORCE_HEVC,
    VCC_FORCE_HEVC_HDR_DEPRECATED,
    VCC_FORCE_AV1,
    VCC_FORCE_PYROWAVE
};
}

enum class DecoderAvailability
{
    None,
    Software,
    Hardware
};

constexpr int VIDEO_FORMAT_H264 = 1 << 0;
constexpr int VIDEO_FORMAT_H264_HIGH8_444 = 1 << 1;
constexpr int VIDEO_FORMAT_H265 = 1 << 2;
constexpr int VIDEO_FORMAT_H265_MAIN10 = 1 << 3;
constexpr int VIDEO_FORMAT_H265_REXT8_444 = 1 << 4;
constexpr int VIDEO_FORMAT_H265_REXT10_444 = 1 << 5;
constexpr int VIDEO_FORMAT_AV1_MAIN8 = 1 << 6;
constexpr int VIDEO_FORMAT_AV1_MAIN10 = 1 << 7;
constexpr int VIDEO_FORMAT_AV1_HIGH8_444 = 1 << 8;
constexpr int VIDEO_FORMAT_AV1_HIGH10_444 = 1 << 9;
constexpr int VIDEO_FORMAT_MASK_H264 = VIDEO_FORMAT_H264 | VIDEO_FORMAT_H264_HIGH8_444;
constexpr int VIDEO_FORMAT_MASK_H265 = VIDEO_FORMAT_H265 | VIDEO_FORMAT_H265_MAIN10 |
                                       VIDEO_FORMAT_H265_REXT8_444 | VIDEO_FORMAT_H265_REXT10_444;
constexpr int VIDEO_FORMAT_MASK_AV1 = VIDEO_FORMAT_AV1_MAIN8 | VIDEO_FORMAT_AV1_MAIN10 |
                                      VIDEO_FORMAT_AV1_HIGH8_444 | VIDEO_FORMAT_AV1_HIGH10_444;
constexpr int VIDEO_FORMAT_MASK_10BIT = VIDEO_FORMAT_H265_MAIN10 | VIDEO_FORMAT_H265_REXT10_444 |
                                        VIDEO_FORMAT_AV1_MAIN10 | VIDEO_FORMAT_AV1_HIGH10_444;
constexpr int VIDEO_FORMAT_MASK_YUV444 = VIDEO_FORMAT_H264_HIGH8_444 | VIDEO_FORMAT_H265_REXT8_444 |
                                         VIDEO_FORMAT_H265_REXT10_444 | VIDEO_FORMAT_AV1_HIGH8_444 |
                                         VIDEO_FORMAT_AV1_HIGH10_444;
constexpr int SCM_MASK_AV1 = VIDEO_FORMAT_MASK_AV1;

struct SupportedVideoFormatList : std::vector<int>
{
    SupportedVideoFormatList(std::initializer_list<int> values) : std::vector<int>(values) {}

    operator int() const
    {
        int mask = 0;
        for (const int format : *this) {
            mask |= format;
        }
        return mask;
    }
    void append(int format) { push_back(format); }
    int maskByServerCodecModes(int modes) const { return static_cast<int>(*this) & modes; }
    void removeByMask(int mask)
    {
        erase(std::remove_if(begin(), end(), [mask](int format) { return (format & mask) != 0; }),
              end());
    }
    void deprioritizeByMask(int mask)
    {
        std::vector<int> moved;
        for (auto iter = begin(); iter != end();) {
            if (*iter & mask) {
                moved.push_back(*iter);
                iter = erase(iter);
            } else {
                ++iter;
            }
        }
        insert(end(), moved.begin(), moved.end());
    }
};

namespace {
DecoderAvailability hevcAvailability = DecoderAvailability::Hardware;
DecoderAvailability av1Availability = DecoderAvailability::Hardware;
DecoderAvailability h264Availability = DecoderAvailability::Hardware;

DecoderAvailability availabilityFor(int format)
{
    if (format & VIDEO_FORMAT_MASK_H265)
        return hevcAvailability;
    if (format & VIDEO_FORMAT_MASK_AV1)
        return av1Availability;
    return h264Availability;
}
}

struct Session
{
    struct Preferences
    {
        StreamingPreferences::VideoDecoderSelection videoDecoderSelection =
            StreamingPreferences::VDS_AUTO;
    }* m_Preferences = new Preferences;

    DecoderAvailability getDecoderAvailability(SDL_Window*,
                                               StreamingPreferences::VideoDecoderSelection,
                                               int format, int, int, int)
    {
        return availabilityFor(format);
    }
    void prioritizeVideoFormats(SupportedVideoFormatList& formats,
                                StreamingPreferences::VideoCodecConfig codec,
                                SDL_Window* testWindow, bool enableHdr, bool enableYUV444,
                                int width, int height, int frameRate, const QString& gfeVersion);
    void applyVideoFormatServerRestrictions(SupportedVideoFormatList& formats,
                                            int serverCodecModeSupport, int maxLumaPixelsHEVC,
                                            StreamingPreferences::VideoCodecConfig codec);
};

#define SDL_LogWarn(...)
#define SDL_assert(value) assert(value)

namespace NvHTTP {
static QVector<int> parseQuad(const QString& version)
{
    int major = 0;
    int minor = 0;
    if (std::sscanf(version.value.c_str(), "%d.%d", &major, &minor) < 2) {
        return {};
    }
    return QVector<int>{ major, minor };
}
}

// PRIORITIZATION_METHOD
// SERVER_FILTER_METHOD

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
    Session session;
    SDL_Window window;

    const auto select = [&session, &window](StreamingPreferences::VideoCodecConfig codec, bool hdr,
                                            bool yuv444, const QString& gfe) {
        SupportedVideoFormatList formats{ VIDEO_FORMAT_AV1_HIGH10_444,  VIDEO_FORMAT_AV1_MAIN10,
                                          VIDEO_FORMAT_H265_REXT10_444, VIDEO_FORMAT_H265_MAIN10,
                                          VIDEO_FORMAT_AV1_HIGH8_444,   VIDEO_FORMAT_AV1_MAIN8,
                                          VIDEO_FORMAT_H265_REXT8_444,  VIDEO_FORMAT_H265,
                                          VIDEO_FORMAT_H264_HIGH8_444,  VIDEO_FORMAT_H264 };
        session.prioritizeVideoFormats(formats, codec, &window, hdr, yuv444, 1920, 1080, 60, gfe);
        return formats;
    };

    hevcAvailability = DecoderAvailability::Software;
    av1Availability = DecoderAvailability::Software;
    h264Availability = DecoderAvailability::Hardware;
    const auto launchAutoSdr = select(StreamingPreferences::VCC_AUTO, false, false, "3.11");
    const auto reconnectAutoSdr = select(StreamingPreferences::VCC_AUTO, false, false, "3.11");
    ok &= require(launchAutoSdr == reconnectAutoSdr && launchAutoSdr.front() == VIDEO_FORMAT_H264,
                  "launch and reconnect both prefer hardware H.264 over software AV1/HEVC");
    const auto pyrowaveFallback =
        select(StreamingPreferences::VCC_FORCE_PYROWAVE, false, false, "3.11");
    ok &= require(pyrowaveFallback == launchAutoSdr,
                  "PyroWave fallback keeps Automatic's codec order");

    hevcAvailability = DecoderAvailability::Hardware;
    av1Availability = DecoderAvailability::Hardware;
    const auto launchHdr444 = select(StreamingPreferences::VCC_AUTO, true, true, "3.11");
    const auto reconnectHdr444 = select(StreamingPreferences::VCC_AUTO, true, true, "3.11");
    ok &= require(launchHdr444 == reconnectHdr444 &&
                      launchHdr444.front() == VIDEO_FORMAT_H265_REXT10_444,
                  "launch and reconnect share HDR plus 4:4:4 priority");

    const auto forcedAv1 = select(StreamingPreferences::VCC_FORCE_AV1, true, false, "3.11");
    bool hasAv1 = false;
    for (const int format : forcedAv1) {
        hasAv1 |= (format & VIDEO_FORMAT_MASK_AV1) != 0;
        ok &= require((format & VIDEO_FORMAT_MASK_H264) == 0,
                      "forced AV1 fallback candidates exclude H.264");
    }
    ok &= require(hasAv1, "forced AV1 retains AV1 candidates ahead of HEVC fallback");

    SupportedVideoFormatList supportedAv1{ VIDEO_FORMAT_AV1_MAIN8, VIDEO_FORMAT_H265,
                                           VIDEO_FORMAT_H264 };
    session.applyVideoFormatServerRestrictions(
        supportedAv1, VIDEO_FORMAT_MASK_AV1 | VIDEO_FORMAT_MASK_H265 | VIDEO_FORMAT_MASK_H264, 1,
        StreamingPreferences::VCC_FORCE_AV1);
    ok &= require(supportedAv1.size() == 1 && supportedAv1.front() == VIDEO_FORMAT_AV1_MAIN8,
                  "forced AV1 with host support matches launch validation");

    SupportedVideoFormatList unsupportedAv1{ VIDEO_FORMAT_AV1_MAIN8, VIDEO_FORMAT_H265,
                                             VIDEO_FORMAT_H264 };
    session.applyVideoFormatServerRestrictions(unsupportedAv1,
                                               VIDEO_FORMAT_MASK_H265 | VIDEO_FORMAT_MASK_H264, 1,
                                               StreamingPreferences::VCC_FORCE_AV1);
    ok &= require(unsupportedAv1.size() == 2 && unsupportedAv1.front() == VIDEO_FORMAT_H265,
                  "forced AV1 falls back to HEVC when the host lacks AV1");

    SupportedVideoFormatList unsupportedHevc{ VIDEO_FORMAT_AV1_MAIN8, VIDEO_FORMAT_H265,
                                              VIDEO_FORMAT_H264 };
    session.applyVideoFormatServerRestrictions(
        unsupportedHevc, VIDEO_FORMAT_MASK_AV1 | VIDEO_FORMAT_MASK_H265 | VIDEO_FORMAT_MASK_H264, 0,
        StreamingPreferences::VCC_AUTO);
    ok &= require(std::find(unsupportedHevc.begin(), unsupportedHevc.end(), VIDEO_FORMAT_H265) ==
                      unsupportedHevc.end(),
                  "server HEVC luma capability removes HEVC for both launch and reconnect");

#ifdef Q_OS_DARWIN
    const auto oldGfe = select(StreamingPreferences::VCC_AUTO, false, false, "3.10");
    for (const int format : oldGfe) {
        ok &= require((format & VIDEO_FORMAT_MASK_H265) == 0, "old GFE disables HEVC on macOS");
    }
#endif

    return ok ? 0 : 1;
}
