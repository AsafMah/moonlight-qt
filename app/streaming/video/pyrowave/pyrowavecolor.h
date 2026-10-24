#pragma once

// The PyroWave color contract and static HDR metadata, as pure logic.
//
// Nothing here needs Vulkan, a GPU or a stream, so it is unit tested on its
// own (tests/pyrowave_color). PyrowaveVulkanRenderer applies the results.

#include <Limelight.h>

#include <cstdint>

#ifdef HAVE_LIBPLACEBO_VULKAN
#include <libplacebo/colorspace.h>
#endif

namespace PyrowaveColor {

// The values of pyrowave.h's color enums (pyrowaveabi.h mirrors them with
// the same numbers; pyrowavevulkan.cpp checks that they agree).
enum : int
{
    k_PrimariesBt709 = 0,
    k_PrimariesBt2020 = 1,
};
enum : int
{
    k_TransferBt709 = 0,
    k_TransferPq = 1,
    k_TransferHlg = 2,
};
enum : int
{
    k_TransformBt709 = 0,
    k_TransformBt2020 = 1,
};
enum : int
{
    k_RangeFull = 0,
    k_RangeLimited = 1,
};
enum : uint32_t
{
    k_SitingCenter = 0,
    k_SitingLeft = 1,
};

// STREAM_CONFIGURATION.hdrMode, which the host receives as dynamicRangeMode.
enum : int
{
    k_ModeSdr = 0,
    k_ModeHdr10 = 1,
    k_ModeHlg = 2,
};

// One frame's native color metadata (pyrowave_color_metadata).
struct FrameColor
{
    int primaries = k_PrimariesBt709;
    int transfer = k_TransferBt709;
    int transform = k_TransformBt709;
    int range = k_RangeFull;
    uint32_t chromaSiting = k_SitingCenter;
};

// How the decoded planes are stored and how libplacebo must read them.
struct PlaneEncoding
{
    // R16_UNORM planes instead of R8_UNORM.
    bool sixteenBit = false;
    // pl_bit_encoding
    int sampleDepth = 8;
    int colorDepth = 8;
    int bitShift = 0;
};

// What the session negotiated (client contract, section 3): SDR is BT.709
// 8-bit, HDR10 is BT.2020 PQ 10-bit and HLG is BT.2020 HLG 10-bit, all 4:2:0
// with the session's YCbCr range.
class Contract
{
public:
    Contract(int hdrMode, bool fullRange) : m_HdrMode(hdrMode), m_FullRange(fullRange) {}

    // An hdrMode this client knows. Anything else is refused before the
    // stream starts, never guessed at.
    bool isValid() const;
    bool isHdr() const { return m_HdrMode == k_ModeHdr10 || m_HdrMode == k_ModeHlg; }
    int hdrMode() const { return m_HdrMode; }
    bool fullRange() const { return m_FullRange; }

    // The metadata every frame must carry. The chroma siting is not part of
    // the contract: both sitings are shown, each as signalled.
    FrameColor expected() const;

    // Whether a frame may be shown. A frame that doesn't match is never shown
    // as if it did.
    bool matches(const FrameColor& frame) const;

    PlaneEncoding planeEncoding() const;

    // English, for logs: "BT.709 SDR", "BT.2020 PQ (HDR10)", "BT.2020 HLG".
    const char* signalName() const;

private:
    int m_HdrMode;
    bool m_FullRange;
};

// SS_HDR_METADATA decoded to the units Vulkan and libplacebo use.
struct HdrPoint
{
    float x = 0.0f;
    float y = 0.0f;
};

struct HdrStaticMetadata
{
    // RGB order, CIE 1931 xy
    HdrPoint primaries[3];
    HdrPoint whitePoint;
    // Mastering display, cd/m²
    float maxLuminance = 0.0f;
    float minLuminance = 0.0f;
    // Content light level, cd/m². 0 = not provided.
    float maxCll = 0.0f;
    float maxFall = 0.0f;
    // Kept for completeness. Neither Vulkan nor libplacebo has a field for
    // it, and it is never used as MaxFALL.
    float maxFullFrameLuminance = 0.0f;

    bool operator==(const HdrStaticMetadata& other) const;
    bool operator!=(const HdrStaticMetadata& other) const { return !(*this == other); }
};

// Decodes and validates the host's snapshot (client contract, section 5).
// Rejects coordinates outside (0, 1], a missing or negative peak and a
// minimum above the peak. MaxCLL and MaxFALL may be 0.
bool parseHdrMetadata(const SS_HDR_METADATA& raw, HdrStaticMetadata& out);

// The last valid snapshot for this session.
//
// - HDR off clears it.
// - A missing (all zero or unavailable) or invalid update keeps the previous
//   valid snapshot. Nothing is invented when there never was one.
class HdrSnapshot
{
public:
    enum class Update
    {
        Cleared,
        Missing,
        Invalid,
        Accepted,
    };

    // raw is null when LiGetHdrMetadata() had nothing.
    Update update(bool hdrEnabled, const SS_HDR_METADATA* raw);

    bool enabled() const { return m_Enabled; }
    // Whether metadata() holds an accepted snapshot.
    bool valid() const { return m_Valid; }
    const HdrStaticMetadata& metadata() const { return m_Metadata; }

    bool operator==(const HdrSnapshot& other) const;
    bool operator!=(const HdrSnapshot& other) const { return !(*this == other); }

private:
    bool m_Enabled = false;
    bool m_Valid = false;
    HdrStaticMetadata m_Metadata;
};

const char* updateName(HdrSnapshot::Update update);

#ifdef HAVE_LIBPLACEBO_VULKAN
// The libplacebo color space of the decoded frames, with the snapshot's
// static metadata when it has one (none otherwise; libplacebo then uses its
// own defaults for tone mapping and the swapchain). The same mapping as
// PlVkRenderer gets from the FFmpeg mastering display and content light side
// data that FFmpegVideoDecoder attaches from LiGetHdrMetadata().
pl_color_space frameColorSpace(const Contract& contract, const HdrSnapshot& snapshot);

// libplacebo treats a minimum luminance of 0 as unknown. Like PlVkRenderer,
// a host that sent mastering metadata with 0 means a true black.
void toPlacebo(const HdrStaticMetadata& metadata, pl_hdr_metadata& out);
#endif

}
