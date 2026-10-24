#include "pyrowavecolor.h"

#include <cmath>

namespace PyrowaveColor {

bool Contract::isValid() const
{
    return m_HdrMode == k_ModeSdr || m_HdrMode == k_ModeHdr10 || m_HdrMode == k_ModeHlg;
}

FrameColor Contract::expected() const
{
    FrameColor color;
    if (isHdr()) {
        color.primaries = k_PrimariesBt2020;
        color.transfer = m_HdrMode == k_ModeHlg ? k_TransferHlg : k_TransferPq;
        // BT.2020 non-constant luminance
        color.transform = k_TransformBt2020;
    } else {
        color.primaries = k_PrimariesBt709;
        color.transfer = k_TransferBt709;
        color.transform = k_TransformBt709;
    }
    color.range = m_FullRange ? k_RangeFull : k_RangeLimited;
    color.chromaSiting = k_SitingCenter;
    return color;
}

bool Contract::matches(const FrameColor& frame) const
{
    if (!isValid()) {
        return false;
    }

    const FrameColor wanted = expected();
    return frame.primaries == wanted.primaries && frame.transfer == wanted.transfer &&
           frame.transform == wanted.transform && frame.range == wanted.range &&
           frame.chromaSiting <= k_SitingLeft;
}

PlaneEncoding Contract::planeEncoding() const
{
    PlaneEncoding encoding;
    if (!isHdr()) {
        // 8-bit codes in R8_UNORM
        return encoding;
    }

    // 10-bit codes in R16_UNORM, MSB-aligned as in P010: the decoder returns
    // the floats the host encoded, and Foundation Sunshine encodes HDR from
    // P010 split into R16 planes without a scaling factor. So a texel holds
    // (code << 6) / 65535, which is exactly pl_bit_encoding {16, 10, 6}.
    // pyrowave_decoder_set_ycbcr_scaling_factor() isn't needed for that (it
    // is for LSB-aligned outputs), and it would rule out the fragment path.
    encoding.sixteenBit = true;
    encoding.sampleDepth = 16;
    encoding.colorDepth = 10;
    encoding.bitShift = 6;
    return encoding;
}

const char* Contract::signalName() const
{
    switch (m_HdrMode) {
    case k_ModeSdr:
        return "BT.709 SDR";
    case k_ModeHdr10:
        return "BT.2020 PQ (HDR10)";
    case k_ModeHlg:
        return "BT.2020 HLG";
    default:
        return "unknown";
    }
}

namespace {

bool validCoordinate(const HdrPoint& point)
{
    return std::isfinite(point.x) && std::isfinite(point.y) && point.x > 0.0f && point.y > 0.0f &&
           point.x <= 1.0f && point.y <= 1.0f;
}

bool samePoint(const HdrPoint& a, const HdrPoint& b)
{
    return a.x == b.x && a.y == b.y;
}

HdrPoint decodePoint(uint16_t x, uint16_t y)
{
    // Normalized to 50000 on the wire
    HdrPoint point;
    point.x = static_cast<float>(x) / 50000.0f;
    point.y = static_cast<float>(y) / 50000.0f;
    return point;
}

bool allZero(const SS_HDR_METADATA& raw)
{
    for (int i = 0; i < 3; i++) {
        if (raw.displayPrimaries[i].x != 0 || raw.displayPrimaries[i].y != 0) {
            return false;
        }
    }
    return raw.whitePoint.x == 0 && raw.whitePoint.y == 0 && raw.maxDisplayLuminance == 0 &&
           raw.minDisplayLuminance == 0 && raw.maxContentLightLevel == 0 &&
           raw.maxFrameAverageLightLevel == 0 && raw.maxFullFrameLuminance == 0;
}

}

bool HdrStaticMetadata::operator==(const HdrStaticMetadata& other) const
{
    for (int i = 0; i < 3; i++) {
        if (!samePoint(primaries[i], other.primaries[i])) {
            return false;
        }
    }
    return samePoint(whitePoint, other.whitePoint) && maxLuminance == other.maxLuminance &&
           minLuminance == other.minLuminance && maxCll == other.maxCll &&
           maxFall == other.maxFall && maxFullFrameLuminance == other.maxFullFrameLuminance;
}

bool parseHdrMetadata(const SS_HDR_METADATA& raw, HdrStaticMetadata& out)
{
    HdrStaticMetadata candidate;
    for (int i = 0; i < 3; i++) {
        candidate.primaries[i] = decodePoint(raw.displayPrimaries[i].x, raw.displayPrimaries[i].y);
        if (!validCoordinate(candidate.primaries[i])) {
            return false;
        }
    }
    candidate.whitePoint = decodePoint(raw.whitePoint.x, raw.whitePoint.y);
    if (!validCoordinate(candidate.whitePoint)) {
        return false;
    }

    candidate.maxLuminance = static_cast<float>(raw.maxDisplayLuminance);
    // 1/10000 cd/m² on the wire
    candidate.minLuminance = static_cast<float>(raw.minDisplayLuminance) / 10000.0f;
    candidate.maxCll = static_cast<float>(raw.maxContentLightLevel);
    candidate.maxFall = static_cast<float>(raw.maxFrameAverageLightLevel);
    candidate.maxFullFrameLuminance = static_cast<float>(raw.maxFullFrameLuminance);

    // The mastering luminance must be usable. The unsigned wire fields can't
    // be negative or non-finite.
    if (candidate.maxLuminance <= 0.0f || candidate.minLuminance > candidate.maxLuminance) {
        return false;
    }

    out = candidate;
    return true;
}

HdrSnapshot::Update HdrSnapshot::update(bool hdrEnabled, const SS_HDR_METADATA* raw)
{
    m_Enabled = hdrEnabled;
    if (!hdrEnabled) {
        m_Valid = false;
        m_Metadata = HdrStaticMetadata();
        return Update::Cleared;
    }

    if (raw == nullptr || allZero(*raw)) {
        return Update::Missing;
    }

    HdrStaticMetadata candidate;
    if (!parseHdrMetadata(*raw, candidate)) {
        return Update::Invalid;
    }

    m_Metadata = candidate;
    m_Valid = true;
    return Update::Accepted;
}

bool HdrSnapshot::operator==(const HdrSnapshot& other) const
{
    return m_Enabled == other.m_Enabled && m_Valid == other.m_Valid &&
           m_Metadata == other.m_Metadata;
}

const char* updateName(HdrSnapshot::Update update)
{
    switch (update) {
    case HdrSnapshot::Update::Cleared:
        return "cleared";
    case HdrSnapshot::Update::Missing:
        return "missing";
    case HdrSnapshot::Update::Invalid:
        return "invalid";
    case HdrSnapshot::Update::Accepted:
        return "accepted";
    }
    return "unknown";
}

#ifdef HAVE_LIBPLACEBO_VULKAN

void toPlacebo(const HdrStaticMetadata& metadata, pl_hdr_metadata& out)
{
    out = pl_hdr_metadata{};
    out.prim.red = { metadata.primaries[0].x, metadata.primaries[0].y };
    out.prim.green = { metadata.primaries[1].x, metadata.primaries[1].y };
    out.prim.blue = { metadata.primaries[2].x, metadata.primaries[2].y };
    out.prim.white = { metadata.whitePoint.x, metadata.whitePoint.y };
    out.max_luma = metadata.maxLuminance;
    out.min_luma = metadata.minLuminance > 0.0f ? metadata.minLuminance : PL_COLOR_HDR_BLACK;
    out.max_cll = metadata.maxCll;
    out.max_fall = metadata.maxFall;
}

pl_color_space frameColorSpace(const Contract& contract, const HdrSnapshot& snapshot)
{
    pl_color_space color = {};
    if (!contract.isHdr()) {
        color.primaries = PL_COLOR_PRIM_BT_709;
        color.transfer = PL_COLOR_TRC_BT_1886;
        return color;
    }

    color.primaries = PL_COLOR_PRIM_BT_2020;
    color.transfer = contract.hdrMode() == k_ModeHlg ? PL_COLOR_TRC_HLG : PL_COLOR_TRC_PQ;
    if (snapshot.enabled() && snapshot.valid()) {
        toPlacebo(snapshot.metadata(), color.hdr);
    }
    return color;
}

#endif

}
