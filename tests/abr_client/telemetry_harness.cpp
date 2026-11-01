#include <QCoreApplication>
#include <QJsonObject>
#include <QDebug>
#include <QSslCertificate>
#include <memory>
#include <cstring>
#include <limits>
#include <stdexcept>
#include "streaming/sunshineabrruntime.h"
#include "streaming/stats/streamstatsmodel.h"
#include "streaming/video/framegapclassifier.h"

struct RTP_VIDEO_STATS
{
    uint32_t packetCountVideo = 0;
    uint32_t packetCountFec = 0;
    uint32_t packetCountFecFailed = 0;
    uint32_t packetCountOOS = 0;
    uint32_t packetCountInvalid = 0;
    uint32_t packetCountFecInvalid = 0;
};
RTP_VIDEO_STATS counters;
const RTP_VIDEO_STATS* LiGetRTPVideoStats()
{
    return &counters;
}
uint64_t LiGetMicroseconds()
{
    return 3000000;
}
void LiGetEstimatedRttInfo(uint32_t* rtt, uint32_t* variance)
{
    *rtt = 10;
    *variance = 1;
}

// LiGetVideoLocalDiscardStats() layout, with a small ring
struct FakeRange
{
    uint32_t firstFrameNumber = 0;
    uint32_t lastFrameNumber = 0;
    uint32_t frames = 0;
};
struct FakeDiscards
{
    uint64_t framesDiscarded = 0;
    uint64_t rangesRecorded = 0;
    FakeRange ranges[4];
    void add(uint32_t first, uint32_t last, uint32_t frames)
    {
        ranges[rangesRecorded++ % 4] = { first, last, frames };
        framesDiscarded += frames;
    }
};
struct Computer
{
    int activeAddress = 0;
    uint16_t activeHttpsPort = 47984;
    QSslCertificate serverCert;
    QString uuid;
};
class AbrFeedbackTask
{
public:
    inline static int calls = 0;
    inline static double loss = 0, decodeFps = 0;
    inline static int dropped = 0;
    AbrFeedbackTask(int, uint16_t, QSslCertificate, QString, double packetLoss, double, double fps,
                    int frames, std::shared_ptr<SunshineAbrRuntime> runtime,
                    SunshineAbrRuntime::Job job)
    {
        ++calls;
        loss = packetLoss;
        decodeFps = fps;
        dropped = frames;
        runtime->finish(job, SunshineAbrRuntime::State::Active);
    }
};
class QThreadPool
{
public:
    static QThreadPool* globalInstance()
    {
        static QThreadPool pool;
        return &pool;
    }
    void start(AbrFeedbackTask* task) { delete task; }
};
class Session
{
public:
    Computer computer;
    Computer* m_Computer = &computer;
    std::shared_ptr<SunshineAbrRuntime> m_AbrRuntime = std::make_shared<SunshineAbrRuntime>();
    RTP_VIDEO_STATS m_LastAbrVideoStats;
    StreamStatsModel m_StatsModel;
    uint64_t m_LastAbrFramesTotal = 0;
    uint64_t m_LastAbrFramesDropped = 0;
    uint64_t m_LastAbrStatsSequence = 0;
    double m_ActiveVideoFrameRate = 60;
    void sendSunshineAbrFeedback();
};
// TELEMETRY

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    auto require = [](bool value, const char* message) {
        if (!value)
            throw std::runtime_error(message);
    };
    try {
        Session session;
        const auto job = session.m_AbrRuntime->begin(true, 400000);
        session.m_AbrRuntime->finish(job, SunshineAbrRuntime::State::Active);
        StatsWindow window;
        window.totalFrames = 100;
        window.networkDroppedFrames = 60;
        window.receivedFrames = 40;
        window.decodedFrames = 40;
        window.decodedFps = 24;
        window.totalFps = 60;
        // Video's legacy RTP failure counters remain zero even when frames are lost.
        counters.packetCountVideo = 10000;
        session.m_StatsModel.publishWindow(window, window, 2000000);
        session.sendSunshineAbrFeedback();
        require(AbrFeedbackTask::calls == 1 && AbrFeedbackTask::loss == 60 &&
                    AbrFeedbackTask::dropped == 60 && AbrFeedbackTask::decodeFps == 24,
                "Published 60% frame-loss/24 decoded FPS incorrectly sent as zero loss/60 FPS");
        session.sendSunshineAbrFeedback();
        require(AbrFeedbackTask::calls == 1, "Same published window was reported twice");
        // Normal monotonic interval: the session counters keep the earlier loss.
        StatsWindow recent = window;
        StatsWindow completed;
        completed.totalFrames = completed.receivedFrames = completed.decodedFrames = 60;
        recent.networkDroppedFrames = 0;
        recent.decodedFps = 59;
        completed.decodedFps = 1;
        session.m_StatsModel.publishWindow(recent, completed, 3000000);
        const auto cumulative = session.m_StatsModel.snapshot(3000000);
        require(cumulative.sessionFramesTotal == 160 &&
                    cumulative.sessionFramesNetworkDropped == 60,
                "Fixture did not produce cumulative session counters 160/60");
        session.sendSunshineAbrFeedback();
        require(AbrFeedbackTask::calls == 2 && AbrFeedbackTask::loss == 0 &&
                    AbrFeedbackTask::dropped == 0 && AbrFeedbackTask::decodeFps == 59,
                "Healthy cumulative interval retained earlier loss or used the wrong FPS window");
        session.m_StatsModel.reset();
        session.sendSunshineAbrFeedback();
        require(AbrFeedbackTask::calls == 2, "Uninitialized stats were presented as healthy");
        // Counters restarting below the previous baseline count from zero.
        completed.totalFrames = 30;
        completed.networkDroppedFrames = 3;
        session.m_StatsModel.publishWindow(recent, completed, 4000000);
        session.sendSunshineAbrFeedback();
        require(AbrFeedbackTask::calls == 3 && AbrFeedbackTask::loss == 10 &&
                    AbrFeedbackTask::dropped == 3,
                "Counter reset produced a wrapped or stale delta");

        // Decode unit queue overflow: moonlight-common-c discards the complete
        // frames 1..16, then the decoder dequeues frame 17 after frame 0.
        FakeDiscards discards;
        FrameGapClassifier gaps;
        gaps.start(discards);
        discards.add(1, 16, 16);
        StatsWindow flushed;
        addFrameGap(flushed, gaps.classify(17, 16, discards));
        flushed.receivedFrames = flushed.decodedFrames = 1;
        flushed.totalFrames += 1;
        flushed.decodedFps = 60;
        session.m_StatsModel.publishWindow(flushed, flushed, 5000000);
        const auto local = session.m_StatsModel.snapshot(5000000);
        session.sendSunshineAbrFeedback();
        require(AbrFeedbackTask::calls == 4 && AbrFeedbackTask::loss == 0 &&
                    AbrFeedbackTask::dropped == 0,
                "Locally discarded frames were sent to ABR as network loss");
        require(local.networkLossPercent == 0 && local.sessionFramesLocalDropped == 16 &&
                    local.localDropPercent > 94 && local.sessionFramesTotal == 47,
                "Locally discarded frames were not shown as local drops");

        // Straddling race: after frame 2, a flush discards 3..6 and the
        // network loses 7 and 8. The decoder dequeues 9, and before it reads
        // the stats an overflow discards 10..12. The earlier batch is local
        // now; the later one belongs to the next gap.
        FakeDiscards straddle;
        FrameGapClassifier race;
        race.start(straddle);
        straddle.add(3, 6, 4);
        straddle.add(10, 12, 3);
        FrameGapSplit split = race.classify(9, 6, straddle);
        require(split.networkDropped == 2 && split.localDropped == 4,
                "A straddling gap was not split into its earlier local batch and network loss");
        split = race.classify(13, 3, straddle);
        require(split.networkDropped == 0 && split.localDropped == 3,
                "A batch discarded after the dequeue was lost before its gap");
        // A flushed queue already missing frame 17 to the network.
        straddle.add(14, 20, 6);
        split = race.classify(21, 7, straddle);
        require(split.networkDropped == 1 && split.localDropped == 6,
                "A gap with network and local drops was not split");
        // More batches than the ring holds: the uncertain gap is local.
        for (uint32_t frame = 22; frame < 32; frame += 2) {
            straddle.add(frame, frame, 1);
        }
        split = race.classify(32, 10, straddle);
        require(split.networkDropped == 0 && split.localDropped == 10,
                "Discards lost from the ring were charged to the network");
        // Discards from before the first frame never reach a gap.
        FakeDiscards early;
        early.add(1, 3, 3);
        FrameGapClassifier late;
        late.start(early);
        split = late.classify(10, 2, early);
        require(split.networkDropped == 2 && split.localDropped == 0,
                "Discards before the first frame were charged to a later gap");
        // Counters that restarted with a new connection are read from zero.
        FakeDiscards restarted;
        restarted.add(40, 40, 1);
        split = race.classify(41, 1, restarted);
        require(split.networkDropped == 0 && split.localDropped == 1,
                "Restarted discard counters were misread");
    } catch (const std::exception& error) {
        qCritical("FAIL: %s", error.what());
        return 1;
    }
    qInfo("telemetry-regressions passed");
    return 0;
}
