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
    } catch (const std::exception& error) {
        qCritical("FAIL: %s", error.what());
        return 1;
    }
    qInfo("telemetry-regressions passed");
    return 0;
}
