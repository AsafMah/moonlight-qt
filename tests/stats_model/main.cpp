#include "streaming/stats/statsmath.h"
#include "streaming/stats/streamstatsmodel.h"

#include <QCoreApplication>
#include <QTextStream>

#include <cmath>
#include <vector>

namespace {
bool require(bool condition, const QString& message, QTextStream& err)
{
    if (!condition) {
        err << "FAIL: " << message << '\n';
    }
    return condition;
}

bool near(double actual, double expected, double tolerance)
{
    return std::fabs(actual - expected) <= tolerance;
}

QString describe(const char* what, double actual, double expected)
{
    return QStringLiteral("%1: got %2, expected %3")
        .arg(QLatin1String(what))
        .arg(actual)
        .arg(expected);
}

const double k_FastFrameMs = 1000.0 / 120.0;

// 998 frames at 120 FPS with two 50 ms hitches in the middle
std::vector<double> hitchyFrameTimes()
{
    std::vector<double> frameTimes(998, k_FastFrameMs);
    frameTimes.insert(frameTimes.begin() + 300, 50.0);
    frameTimes.insert(frameTimes.begin() + 700, 50.0);
    return frameTimes;
}

bool testPercentile(QTextStream& err)
{
    bool ok = true;

    std::vector<double> values;
    for (int i = 100; i >= 1; i--) {
        values.push_back(i);
    }

    std::vector<double> copy = values;
    ok &= require(StatsMath::percentile(copy, 0.99) == 99, QStringLiteral("p99 of 1..100"), err);
    copy = values;
    ok &=
        require(StatsMath::percentile(copy, 0.999) == 100, QStringLiteral("p99.9 of 1..100"), err);
    copy = values;
    ok &= require(StatsMath::percentile(copy, 0.5) == 50, QStringLiteral("median of 1..100"), err);
    copy = values;
    ok &=
        require(StatsMath::percentile(copy, 0) == 1, QStringLiteral("p0 is not the minimum"), err);
    copy = values;
    ok &= require(StatsMath::percentile(copy, 1.5) == 100,
                  QStringLiteral("p > 1 is not clamped to the maximum"), err);

    std::vector<double> empty;
    ok &= require(StatsMath::percentile(empty, 0.99) == 0, QStringLiteral("empty percentile"), err);

    std::vector<double> single{ 7.5 };
    ok &= require(StatsMath::percentile(single, 0.99) == 7.5,
                  QStringLiteral("single sample percentile"), err);

    return ok;
}

bool testFrameTimeSummary(QTextStream& err)
{
    bool ok = true;

    const FrameTimeSummary summary = StatsMath::summarizeFrameTimes(hitchyFrameTimes());
    ok &= require(summary.samples == 1000, QStringLiteral("summary sample count"), err);
    ok &= require(near(summary.averageMs, (998 * k_FastFrameMs + 100) / 1000, 1e-9),
                  describe("average frame time", summary.averageMs, 8.4167), err);
    ok &= require(summary.maxMs == 50, describe("max frame time", summary.maxMs, 50), err);

    // Two hitches are 0.2% of frames: below the 1% low, inside the 0.1% low
    ok &= require(near(summary.onePercentLowFps, 120, 1e-6),
                  describe("1% low", summary.onePercentLowFps, 120), err);
    ok &= require(near(summary.pointOnePercentLowFps, 20, 1e-9),
                  describe("0.1% low", summary.pointOnePercentLowFps, 20), err);
    ok &= require(near(summary.fps, 1000 / summary.averageMs, 1e-9),
                  QStringLiteral("summary fps is not 1000 / average"), err);

    // Twenty hitches (2%) pull the 1% low down to the hitch rate
    std::vector<double> frameTimes(980, k_FastFrameMs);
    frameTimes.insert(frameTimes.end(), 20, 50.0);
    const FrameTimeSummary bad = StatsMath::summarizeFrameTimes(frameTimes);
    ok &= require(near(bad.onePercentLowFps, 20, 1e-9),
                  describe("1% low with 2% hitches", bad.onePercentLowFps, 20), err);

    const FrameTimeSummary none = StatsMath::summarizeFrameTimes({});
    ok &= require(none.samples == 0 && none.onePercentLowFps == 0 && none.fps == 0,
                  QStringLiteral("empty summary is not zero"), err);

    return ok;
}

bool testPresentTimeRing(QTextStream& err)
{
    bool ok = true;

    StatsMath::PresentTimeRing ring(8);
    for (uint64_t t = 1000; t <= 10000; t += 1000) {
        ring.add(t);
    }
    ok &= require(ring.size() == 8, QStringLiteral("ring did not cap at its capacity"), err);

    std::vector<double> frameTimes = ring.frameTimesMs(0, 1000000);
    ok &= require(frameTimes.size() == 7, QStringLiteral("wrapped ring interval count"), err);
    for (double frameTime : frameTimes) {
        ok &= require(frameTime == 1.0, describe("wrapped ring interval", frameTime, 1), err);
    }

    ring.add(9000);
    ring.add(10000);
    ok &= require(ring.frameTimesMs(0, 1000000).size() == 7,
                  QStringLiteral("non-advancing timestamps were recorded"), err);

    // A two second gap is a pause, not a frame
    StatsMath::PresentTimeRing gappy(16);
    for (uint64_t t : { 1000, 2000, 3000, 2003000, 2004000 }) {
        gappy.add(t);
    }
    frameTimes = gappy.frameTimesMs(0, 1000000);
    ok &= require(frameTimes.size() == 3, QStringLiteral("gap was not skipped"), err);

    frameTimes = gappy.frameTimesMs(2000, 1000000);
    ok &= require(frameTimes.size() == 2 && frameTimes[0] == 1.0 && frameTimes[1] == 1.0,
                  QStringLiteral("sinceUs filter is wrong"), err);

    gappy.clear();
    ok &= require(gappy.size() == 0 && gappy.frameTimesMs(0, 1000000).empty(),
                  QStringLiteral("cleared ring is not empty"), err);

    return ok;
}

bool testFrameTimeHistogram(QTextStream& err)
{
    bool ok = true;

    StatsMath::FrameTimeHistogram histogram;
    for (double frameTime : hitchyFrameTimes()) {
        histogram.add(frameTime);
    }

    ok &= require(histogram.count() == 1000, QStringLiteral("histogram count"), err);
    // Exact to the 0.1 ms bucket width
    ok &= require(near(histogram.percentileMs(0.99), k_FastFrameMs, 0.1),
                  describe("histogram p99", histogram.percentileMs(0.99), k_FastFrameMs), err);
    ok &= require(near(histogram.percentileMs(0.999), 50, 1e-9),
                  describe("histogram p99.9", histogram.percentileMs(0.999), 50), err);

    const FrameTimeSummary summary = histogram.summary();
    ok &= require(near(summary.pointOnePercentLowFps, 20, 1e-6),
                  describe("histogram 0.1% low", summary.pointOnePercentLowFps, 20), err);
    ok &= require(near(summary.onePercentLowFps, 120, 120 * 0.1 / k_FastFrameMs),
                  describe("histogram 1% low", summary.onePercentLowFps, 120), err);

    // Beyond the last bucket the maximum is reported
    histogram.add(300);
    histogram.add(400);
    ok &= require(histogram.percentileMs(1.0) == 400,
                  describe("overflow percentile", histogram.percentileMs(1.0), 400), err);

    histogram.add(-1);
    histogram.add(std::nan(""));
    ok &=
        require(histogram.count() == 1002, QStringLiteral("invalid frame times were counted"), err);

    histogram.clear();
    ok &= require(histogram.count() == 0 && histogram.percentileMs(0.99) == 0,
                  QStringLiteral("cleared histogram is not empty"), err);

    return ok;
}

bool testJitter(QTextStream& err)
{
    bool ok = true;

    // 60 FPS: 1500 ticks of the 90 kHz clock, 16666 us after integer division
    const uint32_t rtpStep = 1500;
    const uint64_t frameUs = 16666;

    // Perfect pacing, across a 32-bit RTP wraparound
    StatsMath::JitterEstimator perfect;
    uint32_t rtp = 0xFFFFFFFFu - 10 * rtpStep;
    uint64_t arrival = 5000000;
    for (int i = 0; i < 100; i++) {
        perfect.addFrame(arrival, rtp);
        rtp += rtpStep;
        arrival += frameUs;
    }
    ok &= require(perfect.jitterMs() == 0, describe("paced jitter", perfect.jitterMs(), 0), err);

    // Arrivals alternately 2 ms late and 2 ms early relative to the RTP clock
    StatsMath::JitterEstimator alternating;
    rtp = 1000;
    arrival = 5000000;
    for (int i = 0; i < 200; i++) {
        alternating.addFrame(arrival, rtp);
        rtp += rtpStep;
        arrival += frameUs + (i % 2 == 0 ? 2000 : -2000);
    }
    ok &= require(near(alternating.jitterMs(), 2.0, 0.01),
                  describe("alternating jitter", alternating.jitterMs(), 2.0), err);

    // A 2 s stall restarts the estimate instead of dominating it
    arrival += 2000000;
    alternating.addFrame(arrival, rtp);
    ok &= require(alternating.jitterMs() == 0,
                  describe("jitter after a stall", alternating.jitterMs(), 0), err);
    rtp += rtpStep;
    arrival += frameUs;
    alternating.addFrame(arrival, rtp);
    ok &= require(alternating.jitterMs() == 0,
                  describe("jitter after resuming", alternating.jitterMs(), 0), err);

    alternating.reset();
    ok &= require(alternating.jitterMs() == 0, QStringLiteral("reset jitter is not zero"), err);

    return ok;
}

bool testLatency(QTextStream& err)
{
    bool ok = true;

    StatsWindow window;
    window.framesWithHostProcessingLatency = 10;
    window.totalHostProcessingLatency = 500; // 0.1 ms units: 5 ms average
    window.rttMs = 20;
    window.decodedFrames = 10;
    window.totalDecodeTimeUs = 30000;
    window.renderedFrames = 10;
    window.totalPacerTimeUs = 20000;
    window.totalRenderTimeUs = 10000;

    LatencyBreakdown latency = StatsMath::estimateLatency(window);
    ok &= require(latency.hasHost && latency.hostMs == 5, describe("host", latency.hostMs, 5), err);
    ok &= require(latency.hasNetwork && latency.networkMs == 10,
                  describe("network", latency.networkMs, 10), err);
    ok &= require(latency.decodeMs == 3, describe("decode", latency.decodeMs, 3), err);
    ok &= require(latency.queueMs == 2, describe("queue", latency.queueMs, 2), err);
    ok &= require(latency.renderMs == 1, describe("render", latency.renderMs, 1), err);
    ok &= require(latency.totalMs() == 21, describe("total", latency.totalMs(), 21), err);

    // Without host reports or an RTT estimate those stages are absent, not 0
    window.framesWithHostProcessingLatency = 0;
    window.rttMs = 0;
    latency = StatsMath::estimateLatency(window);
    ok &= require(!latency.hasHost && !latency.hasNetwork && latency.totalMs() == 6,
                  describe("total without host and network", latency.totalMs(), 6), err);

    // No frames at all must not divide by zero
    latency = StatsMath::estimateLatency(StatsWindow());
    ok &= require(latency.totalMs() == 0, QStringLiteral("empty window latency"), err);

    ok &= require(StatsMath::lossPercent(1, 200) == 0.5 && StatsMath::lossPercent(5, 0) == 0,
                  QStringLiteral("loss percent"), err);

    return ok;
}

bool testModel(QTextStream& err)
{
    bool ok = true;
    StreamStatsModel model;

    ok &= require(!model.snapshot(0).valid && model.sequence() == 0,
                  QStringLiteral("new model is not empty"), err);

    // 1000 presents at 120 FPS with two 50 ms hitches: 999 intervals
    const uint64_t startUs = 1000000;
    uint64_t now = startUs;
    model.recordFrameArrival(startUs - 5000, 0);
    for (int i = 0; i < 1000; i++) {
        model.recordFramePresented(now);
        now += (i == 300 || i == 700) ? 50000 : 8333;
    }
    const uint64_t lastPresentUs = now - 8333;

    StatsWindow recent;
    recent.width = 2560;
    recent.height = 1440;
    recent.targetFps = 120;
    recent.codec = QStringLiteral("HEVC");
    recent.totalFrames = 300;
    recent.networkDroppedFrames = 3;
    recent.decodedFrames = 200;
    recent.pacerDroppedFrames = 4;
    recent.rttMs = 8;
    recent.rttVarianceMs = 1;
    recent.renderedFps = 119.5;
    recent.bitrateMbps = 50;

    StatsWindow completed;
    completed.totalFrames = 120;
    completed.receivedFrames = 118;
    completed.networkDroppedFrames = 2;
    completed.renderedFrames = 117;

    model.publishWindow(recent, completed, lastPresentUs);
    model.publishWindow(recent, completed, lastPresentUs);

    const StatsSnapshot snapshot = model.snapshot(lastPresentUs);
    ok &= require(snapshot.valid && snapshot.sequence == 2 && model.sequence() == 2,
                  QStringLiteral("published snapshot is not valid"), err);
    ok &= require(snapshot.width == 2560 && snapshot.height == 1440 && snapshot.targetFps == 120 &&
                      snapshot.codec == QLatin1String("HEVC"),
                  QStringLiteral("stream description was not copied"), err);
    ok &= require(snapshot.frameTimes.samples == 999,
                  describe("recent frame time samples", snapshot.frameTimes.samples, 999), err);
    ok &= require(near(snapshot.frameTimes.onePercentLowFps, 1000.0 / 8.333, 1e-6),
                  describe("model 1% low", snapshot.frameTimes.onePercentLowFps, 120), err);
    ok &= require(near(snapshot.frameTimes.pointOnePercentLowFps, 20, 1e-9),
                  describe("model 0.1% low", snapshot.frameTimes.pointOnePercentLowFps, 20), err);
    ok &= require(snapshot.sessionFrameTimes.samples == 999,
                  QStringLiteral("session histogram missed frames"), err);
    ok &= require(snapshot.frameTimeHistoryMs.size() == StreamStatsModel::FrameTimeHistoryLength &&
                      near(snapshot.frameTimeHistoryMs.back(), 8.333, 1e-4),
                  QStringLiteral("frame time history is wrong"), err);
    ok &= require(near(snapshot.networkLossPercent, 1.0, 1e-9) &&
                      near(snapshot.pacerDropPercent, 2.0, 1e-9),
                  QStringLiteral("loss percentages are wrong"), err);
    ok &= require(snapshot.hasRtt && snapshot.rttMs == 8 && snapshot.latency.networkMs == 4,
                  QStringLiteral("RTT was not carried into the snapshot"), err);
    ok &= require(snapshot.bitrateMbps == 50, QStringLiteral("bitrate was not copied"), err);
    ok &= require(snapshot.sessionFramesTotal == 240 && snapshot.sessionFramesReceived == 236 &&
                      snapshot.sessionFramesNetworkDropped == 4 &&
                      snapshot.sessionFramesRendered == 234,
                  QStringLiteral("session totals did not add up completed windows"), err);
    ok &= require(snapshot.sessionDurationUs == lastPresentUs - (startUs - 5000),
                  QStringLiteral("session duration is wrong"), err);
    ok &= require(snapshot.fecRecoveredFrames == -1 && snapshot.fecUnrecoverableFrames == -1,
                  QStringLiteral("unreported FEC counters are not -1"), err);

    // Ten seconds later the recent window is empty, the session one is not
    const StatsSnapshot later = model.snapshot(lastPresentUs + 11000000);
    ok &= require(later.frameTimes.samples == 0 && later.sessionFrameTimes.samples == 999,
                  QStringLiteral("recent window did not expire"), err);

    model.reset();
    const StatsSnapshot cleared = model.snapshot(lastPresentUs);
    ok &= require(!cleared.valid && cleared.sequence == 0 && cleared.frameTimes.samples == 0 &&
                      cleared.sessionFramesTotal == 0,
                  QStringLiteral("reset did not clear the model"), err);

    return ok;
}
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    QTextStream err(stderr);

    bool ok = true;
    ok &= testPercentile(err);
    ok &= testFrameTimeSummary(err);
    ok &= testPresentTimeRing(err);
    ok &= testFrameTimeHistogram(err);
    ok &= testJitter(err);
    ok &= testLatency(err);
    ok &= testModel(err);

    out << (ok ? "PASS" : "FAILED") << '\n';
    return ok ? 0 : 1;
}
