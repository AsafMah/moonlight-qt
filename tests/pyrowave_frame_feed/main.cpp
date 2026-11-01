// How a complete PyroWave frame is handed to the decoder
// (pyrowaveframefeed.h). The fake below keeps the sequence bookkeeping of the
// pinned library decoder (AlkaidLab/pyrowave cc91cbf6, pyrowave_decoder.cpp):
// push_packet() ignores a sequence more than half the 3-bit counter behind,
// restarts on a newer one, and decode_is_ready() is false once the current
// sequence was decoded. The encoder bumps that counter once per encoded frame
// (pyrowave_encoder.cpp), so frames the client never submits (pacer drops,
// frames lost on the network) leave gaps in it.

#include "streaming/video/pyrowave/pyrowaveframefeed.h"

#include <QString>
#include <QTextStream>

#include <cstdint>
#include <set>

using PyrowaveFrameFeed::Result;

namespace {

int g_Failures = 0;

void require(bool condition, const QString& what)
{
    if (!condition) {
        g_Failures++;
        QTextStream(stderr) << "FAIL: " << what << "\n";
    }
}

constexpr uint32_t kSequenceMask = 0x7;
constexpr uint32_t kNoSequence = UINT32_MAX;
constexpr int kTotalBlocks = 8;

struct Frame
{
    uint32_t encodedIndex;
    int receivedBlocks;
    bool malformed = false;
};

class FakeDecoder
{
public:
    void clear()
    {
        m_Blocks.clear();
        m_LastSeq = kNoSequence;
        m_Decoded = false;
    }

    bool push(const Frame& frame)
    {
        if (frame.malformed) {
            return false;
        }
        const uint32_t seq = frame.encodedIndex & kSequenceMask;
        if (m_LastSeq != kNoSequence) {
            const uint32_t diff = (seq - m_LastSeq) & kSequenceMask;
            if (diff > kSequenceMask / 2) {
                // Older sequence: the whole payload is ignored.
                return true;
            }
            if (diff != 0) {
                clear();
            }
        }
        m_LastSeq = seq;
        for (int i = 0; i < frame.receivedBlocks; i++) {
            m_Blocks.insert(i);
        }
        return true;
    }

    bool isReady() const
    {
        return !m_Decoded && m_LastSeq != kNoSequence && (int)m_Blocks.size() >= kTotalBlocks;
    }

    void decode() { m_Decoded = true; }

private:
    std::set<int> m_Blocks;
    uint32_t m_LastSeq = kNoSequence;
    bool m_Decoded = false;
};

// Mirrors PyrowaveVulkanRenderer::submitFrame(): a ready frame is decoded.
Result submit(FakeDecoder& decoder, const Frame& frame)
{
    const Result result =
        PyrowaveFrameFeed::feed([&] { decoder.clear(); }, [&] { return decoder.push(frame); },
                                [&] { return decoder.isReady(); });
    if (result == Result::Ready) {
        decoder.decode();
    }
    return result;
}

Frame complete(uint32_t encodedIndex)
{
    return { encodedIndex, kTotalBlocks };
}

void testConsecutiveFrames()
{
    FakeDecoder decoder;
    for (uint32_t i = 0; i < 20; i++) {
        require(submit(decoder, complete(i)) == Result::Ready,
                QString("consecutive complete frame %1 is decoded").arg(i));
    }
}

void testFramesAfterGaps()
{
    // After a decoded frame, the next submitted one may be any number of
    // encoded frames later. Every complete one must decode.
    for (uint32_t gap = 1; gap <= 17; gap++) {
        FakeDecoder decoder;
        require(submit(decoder, complete(100)) == Result::Ready, "first frame decodes");
        require(submit(decoder, complete(100 + gap)) == Result::Ready,
                QString("complete frame %1 encoded frames later is decoded, not dropped").arg(gap));
    }
}

void testEveryFourthFrame()
{
    // 120 fps presented at 30 Hz: the pacer submits every fourth frame.
    FakeDecoder decoder;
    int dropped = 0;
    for (uint32_t i = 0; i < 400; i += 4) {
        if (submit(decoder, complete(i)) != Result::Ready) {
            dropped++;
        }
    }
    require(dropped == 0, QString("every fourth frame: %1 of 100 complete frames dropped as "
                                  "incomplete")
                              .arg(dropped));
}

void testIncompleteFrame()
{
    FakeDecoder decoder;
    require(submit(decoder, complete(0)) == Result::Ready, "first frame decodes");
    require(submit(decoder, { 1, kTotalBlocks - 1 }) == Result::Incomplete,
            "a frame missing a block is incomplete");
    require(submit(decoder, complete(2)) == Result::Ready,
            "the frame after an incomplete one decodes");
    require(submit(decoder, { 7, kTotalBlocks - 1 }) == Result::Incomplete,
            "a frame missing a block after a gap is incomplete");
    require(submit(decoder, complete(8)) == Result::Ready, "and the next one decodes");
}

void testRejectedFrame()
{
    FakeDecoder decoder;
    require(submit(decoder, complete(0)) == Result::Ready, "first frame decodes");
    require(submit(decoder, { 1, kTotalBlocks, true }) == Result::Rejected,
            "a payload the decoder refuses is rejected");
}

} // namespace

int main()
{
    testConsecutiveFrames();
    testFramesAfterGaps();
    testEveryFourthFrame();
    testIncompleteFrame();
    testRejectedFrame();

    if (g_Failures != 0) {
        QTextStream(stderr) << g_Failures << " check(s) failed\n";
        return 1;
    }
    QTextStream(stdout) << "pyrowave_frame_feed: all checks passed\n";
    return 0;
}
