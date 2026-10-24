// The PyroWave color contract and static HDR metadata (pyrowavecolor.h):
// which frames a session may show, how the planes are read, and how the
// host's SS_HDR_METADATA becomes the snapshot and libplacebo's metadata.

#include "streaming/video/pyrowave/pyrowavecolor.h"

#include <QString>
#include <QTextStream>

#include <cmath>
#include <initializer_list>

using namespace PyrowaveColor;

namespace {

int g_Failures = 0;

void require(bool condition, const QString& what)
{
    if (!condition) {
        g_Failures++;
        QTextStream(stderr) << "FAIL: " << what << "\n";
    }
}

bool near(float a, float b)
{
    return std::fabs(a - b) < 1e-6f;
}

FrameColor frame(int primaries, int transfer, int transform, int range, uint32_t siting = 0)
{
    FrameColor color;
    color.primaries = primaries;
    color.transfer = transfer;
    color.transform = transform;
    color.range = range;
    color.chromaSiting = siting;
    return color;
}

// Display P3-ish mastering display, 1000 nits peak, 0.005 nits black.
SS_HDR_METADATA sampleMetadata()
{
    SS_HDR_METADATA raw = {};
    raw.displayPrimaries[0] = { 34000, 16000 }; // red
    raw.displayPrimaries[1] = { 13250, 34500 }; // green
    raw.displayPrimaries[2] = { 7500, 3000 };   // blue
    raw.whitePoint = { 15635, 16450 };
    raw.maxDisplayLuminance = 1000;
    raw.minDisplayLuminance = 50;
    raw.maxContentLightLevel = 800;
    raw.maxFrameAverageLightLevel = 400;
    raw.maxFullFrameLuminance = 600;
    return raw;
}

void testContractTable()
{
    struct Case
    {
        int mode;
        int primaries;
        int transfer;
        int transform;
    };
    const Case cases[] = {
        { k_ModeSdr, k_PrimariesBt709, k_TransferBt709, k_TransformBt709 },
        { k_ModeHdr10, k_PrimariesBt2020, k_TransferPq, k_TransformBt2020 },
        { k_ModeHlg, k_PrimariesBt2020, k_TransferHlg, k_TransformBt2020 },
    };

    for (const Case& c : cases) {
        for (bool fullRange : { false, true }) {
            const Contract contract(c.mode, fullRange);
            const QString name =
                QStringLiteral("mode %1 %2")
                    .arg(c.mode)
                    .arg(fullRange ? QStringLiteral("full") : QStringLiteral("limited"));
            const int range = fullRange ? k_RangeFull : k_RangeLimited;
            const int otherRange = fullRange ? k_RangeLimited : k_RangeFull;

            require(contract.isValid(), name + ": valid");
            require(contract.isHdr() == (c.mode != k_ModeSdr), name + ": isHdr");

            const FrameColor wanted = contract.expected();
            require(wanted.primaries == c.primaries && wanted.transfer == c.transfer &&
                        wanted.transform == c.transform && wanted.range == range,
                    name + ": expected()");

            // Both chroma sitings are accepted, as signalled.
            require(contract.matches(frame(c.primaries, c.transfer, c.transform, range, 0)),
                    name + ": center siting shown");
            require(contract.matches(frame(c.primaries, c.transfer, c.transform, range, 1)),
                    name + ": left siting shown");
            require(!contract.matches(frame(c.primaries, c.transfer, c.transform, range, 2)),
                    name + ": unknown siting refused");

            // Every single mismatch is refused.
            require(!contract.matches(frame(c.primaries, c.transfer, c.transform, otherRange)),
                    name + ": wrong range refused");
            for (int primaries :
                 std::initializer_list<int>{ k_PrimariesBt709, k_PrimariesBt2020, 7 }) {
                if (primaries != c.primaries) {
                    require(!contract.matches(frame(primaries, c.transfer, c.transform, range)),
                            name + QStringLiteral(": primaries %1 refused").arg(primaries));
                }
            }
            for (int transfer :
                 std::initializer_list<int>{ k_TransferBt709, k_TransferPq, k_TransferHlg, 9 }) {
                if (transfer != c.transfer) {
                    require(!contract.matches(frame(c.primaries, transfer, c.transform, range)),
                            name + QStringLiteral(": transfer %1 refused").arg(transfer));
                }
            }
            for (int transform :
                 std::initializer_list<int>{ k_TransformBt709, k_TransformBt2020, 5 }) {
                if (transform != c.transform) {
                    require(!contract.matches(frame(c.primaries, c.transfer, transform, range)),
                            name + QStringLiteral(": transform %1 refused").arg(transform));
                }
            }
        }
    }

    // A PQ frame in an HLG session and the other way round: the case the HDR
    // checks exist for.
    require(
        !Contract(k_ModeHlg, false)
             .matches(frame(k_PrimariesBt2020, k_TransferPq, k_TransformBt2020, k_RangeLimited)),
        "HLG session refuses PQ");
    require(
        !Contract(k_ModeHdr10, false)
             .matches(frame(k_PrimariesBt2020, k_TransferHlg, k_TransformBt2020, k_RangeLimited)),
        "HDR10 session refuses HLG");
    require(!Contract(k_ModeSdr, true)
                 .matches(frame(k_PrimariesBt2020, k_TransferPq, k_TransformBt2020, k_RangeFull)),
            "SDR session refuses HDR10");

    for (int mode : { -1, 3, 42 }) {
        const Contract contract(mode, false);
        require(!contract.isValid(), QStringLiteral("mode %1 invalid").arg(mode));
        require(!contract.isHdr(), QStringLiteral("mode %1 not HDR").arg(mode));
        require(!contract.matches(contract.expected()),
                QStringLiteral("mode %1 matches nothing").arg(mode));
    }
}

void testPlaneEncoding()
{
    const PlaneEncoding sdr = Contract(k_ModeSdr, false).planeEncoding();
    require(!sdr.sixteenBit && sdr.sampleDepth == 8 && sdr.colorDepth == 8 && sdr.bitShift == 0,
            "SDR planes are 8-bit");

    for (int mode : { k_ModeHdr10, k_ModeHlg }) {
        for (bool fullRange : { false, true }) {
            const PlaneEncoding hdr = Contract(mode, fullRange).planeEncoding();
            require(hdr.sixteenBit && hdr.sampleDepth == 16 && hdr.colorDepth == 10 &&
                        hdr.bitShift == 6,
                    QStringLiteral("mode %1 planes are MSB-aligned 10-bit in 16").arg(mode));
            // sampleDepth - colorDepth == bitShift: MSB-aligned like P010.
            require(hdr.sampleDepth - hdr.colorDepth == hdr.bitShift, "P010 alignment");
        }
    }

    require(QString::fromLatin1(Contract(k_ModeSdr, false).signalName()) == "BT.709 SDR",
            "SDR name");
    require(QString::fromLatin1(Contract(k_ModeHdr10, false).signalName()) == "BT.2020 PQ (HDR10)",
            "HDR10 name");
    require(QString::fromLatin1(Contract(k_ModeHlg, false).signalName()) == "BT.2020 HLG",
            "HLG name");
}

void testParse()
{
    HdrStaticMetadata out;
    require(parseHdrMetadata(sampleMetadata(), out), "sample parses");
    require(near(out.primaries[0].x, 0.68f) && near(out.primaries[0].y, 0.32f), "red");
    require(near(out.primaries[1].x, 0.265f) && near(out.primaries[1].y, 0.69f), "green");
    require(near(out.primaries[2].x, 0.15f) && near(out.primaries[2].y, 0.06f), "blue");
    require(near(out.whitePoint.x, 0.3127f) && near(out.whitePoint.y, 0.329f), "white point");
    require(out.maxLuminance == 1000.0f, "max luminance in nits");
    require(near(out.minLuminance, 0.005f), "min luminance from 1/10000 nits");
    require(out.maxCll == 800.0f && out.maxFall == 400.0f, "MaxCLL and MaxFALL");
    require(out.maxFullFrameLuminance == 600.0f, "full-frame luminance kept apart from MaxFALL");

    // MaxCLL/MaxFALL of 0 don't invalidate the rest.
    SS_HDR_METADATA raw = sampleMetadata();
    raw.maxContentLightLevel = 0;
    raw.maxFrameAverageLightLevel = 0;
    require(parseHdrMetadata(raw, out) && out.maxCll == 0.0f && out.maxFall == 0.0f,
            "no content light level is fine");

    // A true black is fine.
    raw = sampleMetadata();
    raw.minDisplayLuminance = 0;
    require(parseHdrMetadata(raw, out) && out.minLuminance == 0.0f, "zero min luminance");

    const HdrStaticMetadata before = out;
    struct Bad
    {
        const char* what;
        void (*mutate)(SS_HDR_METADATA&);
    };
    const Bad bad[] = {
        { "red x above 1", [](SS_HDR_METADATA& m) { m.displayPrimaries[0].x = 50001; } },
        { "green y above 1", [](SS_HDR_METADATA& m) { m.displayPrimaries[1].y = 65535; } },
        { "blue at 0", [](SS_HDR_METADATA& m) { m.displayPrimaries[2] = { 0, 0 }; } },
        { "white x at 0", [](SS_HDR_METADATA& m) { m.whitePoint.x = 0; } },
        { "white y above 1", [](SS_HDR_METADATA& m) { m.whitePoint.y = 50001; } },
        { "no peak", [](SS_HDR_METADATA& m) { m.maxDisplayLuminance = 0; } },
        { "black above peak",
          [](SS_HDR_METADATA& m) {
              m.maxDisplayLuminance = 1;
              m.minDisplayLuminance = 10001;
          } },
    };
    for (const Bad& b : bad) {
        raw = sampleMetadata();
        b.mutate(raw);
        out = before;
        require(!parseHdrMetadata(raw, out), QStringLiteral("%1 refused").arg(b.what));
        require(out == before, QStringLiteral("%1 leaves the output alone").arg(b.what));
    }

    // Exactly 1.0 and the peak equal to the black level are still accepted.
    raw = sampleMetadata();
    raw.whitePoint = { 50000, 50000 };
    raw.maxDisplayLuminance = 1;
    raw.minDisplayLuminance = 10000;
    require(parseHdrMetadata(raw, out), "boundary values accepted");
}

void testSnapshot()
{
    const SS_HDR_METADATA sample = sampleMetadata();
    HdrStaticMetadata expected;
    parseHdrMetadata(sample, expected);

    HdrSnapshot snapshot;
    require(!snapshot.enabled() && !snapshot.valid(), "starts empty");

    // Nothing is invented while there never was valid metadata.
    require(snapshot.update(true, nullptr) == HdrSnapshot::Update::Missing,
            "unavailable is missing");
    require(snapshot.enabled() && !snapshot.valid(), "enabled without metadata");
    SS_HDR_METADATA zero = {};
    require(snapshot.update(true, &zero) == HdrSnapshot::Update::Missing, "all zero is missing");
    require(!snapshot.valid() && snapshot.metadata() == HdrStaticMetadata(), "no default values");

    require(snapshot.update(true, &sample) == HdrSnapshot::Update::Accepted, "accepted");
    require(snapshot.valid() && snapshot.metadata() == expected, "holds the host's values");

    // Later missing or invalid updates keep the last valid snapshot.
    require(snapshot.update(true, nullptr) == HdrSnapshot::Update::Missing, "missing again");
    require(snapshot.valid() && snapshot.metadata() == expected, "missing keeps the snapshot");
    require(snapshot.update(true, &zero) == HdrSnapshot::Update::Missing, "zero again");
    require(snapshot.valid() && snapshot.metadata() == expected, "zero keeps the snapshot");
    SS_HDR_METADATA invalid = sample;
    invalid.maxDisplayLuminance = 0;
    require(snapshot.update(true, &invalid) == HdrSnapshot::Update::Invalid, "invalid");
    require(snapshot.valid() && snapshot.metadata() == expected, "invalid keeps the snapshot");

    // A new valid one replaces it.
    SS_HDR_METADATA brighter = sample;
    brighter.maxDisplayLuminance = 4000;
    require(snapshot.update(true, &brighter) == HdrSnapshot::Update::Accepted, "replaced");
    require(snapshot.metadata().maxLuminance == 4000.0f, "new peak");

    // Only HDR off clears it.
    const HdrSnapshot copy = snapshot;
    require(copy == snapshot, "copies compare equal");
    require(snapshot.update(false, &sample) == HdrSnapshot::Update::Cleared, "cleared");
    require(!snapshot.enabled() && !snapshot.valid() && snapshot.metadata() == HdrStaticMetadata(),
            "off clears everything");
    require(copy != snapshot, "cleared differs");

    // And after clearing, nothing old comes back.
    require(snapshot.update(true, nullptr) == HdrSnapshot::Update::Missing, "missing after off");
    require(!snapshot.valid(), "no stale snapshot after off");

    require(QString::fromLatin1(updateName(HdrSnapshot::Update::Accepted)) == "accepted",
            "update names");
}

#ifdef HAVE_LIBPLACEBO_VULKAN
void testPlacebo()
{
    HdrSnapshot snapshot;
    const SS_HDR_METADATA sample = sampleMetadata();
    snapshot.update(true, &sample);

    pl_hdr_metadata hdr;
    toPlacebo(snapshot.metadata(), hdr);
    require(near(hdr.prim.red.x, 0.68f) && near(hdr.prim.red.y, 0.32f), "pl red");
    require(near(hdr.prim.green.x, 0.265f) && near(hdr.prim.green.y, 0.69f), "pl green");
    require(near(hdr.prim.blue.x, 0.15f) && near(hdr.prim.blue.y, 0.06f), "pl blue");
    require(near(hdr.prim.white.x, 0.3127f) && near(hdr.prim.white.y, 0.329f), "pl white");
    require(hdr.max_luma == 1000.0f && near(hdr.min_luma, 0.005f), "pl luminance");
    require(hdr.max_cll == 800.0f && hdr.max_fall == 400.0f, "pl content light level");
    require(hdr.scene_max[0] == 0.0f && hdr.scene_avg == 0.0f && hdr.ootf.num_anchors == 0,
            "no dynamic metadata");

    // 0 would read as "unknown" to libplacebo.
    SS_HDR_METADATA black = sample;
    black.minDisplayLuminance = 0;
    HdrSnapshot blackSnapshot;
    blackSnapshot.update(true, &black);
    toPlacebo(blackSnapshot.metadata(), hdr);
    require(hdr.min_luma == PL_COLOR_HDR_BLACK, "true black");

    pl_color_space color = frameColorSpace(Contract(k_ModeSdr, false), snapshot);
    require(color.primaries == PL_COLOR_PRIM_BT_709 && color.transfer == PL_COLOR_TRC_BT_1886 &&
                color.hdr.max_luma == 0.0f,
            "SDR is BT.709 without HDR metadata");

    color = frameColorSpace(Contract(k_ModeHdr10, false), snapshot);
    require(color.primaries == PL_COLOR_PRIM_BT_2020 && color.transfer == PL_COLOR_TRC_PQ,
            "HDR10 is BT.2020 PQ");
    require(color.hdr.max_luma == 1000.0f && color.hdr.max_cll == 800.0f,
            "HDR10 carries the snapshot");

    color = frameColorSpace(Contract(k_ModeHlg, true), snapshot);
    require(color.primaries == PL_COLOR_PRIM_BT_2020 && color.transfer == PL_COLOR_TRC_HLG,
            "HLG is BT.2020 HLG");
    require(color.hdr.max_luma == 1000.0f, "HLG carries the snapshot");

    // Without a valid snapshot the color space has no invented metadata.
    const HdrSnapshot empty;
    color = frameColorSpace(Contract(k_ModeHdr10, false), empty);
    require(color.transfer == PL_COLOR_TRC_PQ && color.hdr.max_luma == 0.0f &&
                color.hdr.min_luma == 0.0f && color.hdr.prim.red.x == 0.0f,
            "no snapshot, no metadata");
    HdrSnapshot off = snapshot;
    off.update(false, nullptr);
    color = frameColorSpace(Contract(k_ModeHdr10, false), off);
    require(color.transfer == PL_COLOR_TRC_PQ && color.hdr.max_luma == 0.0f,
            "host HDR off keeps PQ without metadata");
}
#endif

}

int main()
{
    testContractTable();
    testPlaneEncoding();
    testParse();
    testSnapshot();
#ifdef HAVE_LIBPLACEBO_VULKAN
    testPlacebo();
#else
    QTextStream(stdout) << "libplacebo headers not found; skipping the libplacebo mapping\n";
#endif

    if (g_Failures != 0) {
        QTextStream(stderr) << g_Failures << " check(s) failed\n";
        return 1;
    }
    QTextStream(stdout) << "pyrowave_color: all checks passed\n";
    return 0;
}
