// Moonlight V+: the overlay menu's Session level, showing live session totals
// from StreamStatsModel. Kept out of session.cpp so the hook there stays a
// one-liner.

#include "session.h"

#include <Limelight.h>

#include <cmath>

namespace {

QString formatDuration(uint64_t durationUs)
{
    const uint64_t total = durationUs / 1000000;
    return QStringLiteral("%1:%2:%3")
        .arg(total / 3600)
        .arg((total / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(total % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

std::vector<std::pair<QString, QString>> Session::sessionStatsMenuRows() const
{
    const StatsSnapshot s = m_StatsModel.snapshot(LiGetMicroseconds());
    const QString unknown = QStringLiteral("\342\200\224"); // em dash
    const FrameTimeSummary& frames = s.sessionFrameTimes;
    const bool hasFrames = s.valid && frames.samples > 0;

    std::vector<std::pair<QString, QString>> rows;
    rows.emplace_back(tr("Duration"), formatDuration(s.sessionDurationUs));
    rows.emplace_back(tr("Average FPS"), hasFrames ? QString::number(frames.fps, 'f', 1) : unknown);
    rows.emplace_back(tr("1% / 0.1% low"), hasFrames
                                               ? tr("%1 / %2 FPS")
                                                     .arg(frames.onePercentLowFps, 0, 'f', 1)
                                                     .arg(frames.pointOnePercentLowFps, 0, 'f', 1)
                                               : unknown);
    rows.emplace_back(tr("Frame time, 99th percentile"),
                      hasFrames ? tr("%1 ms").arg(frames.p99Ms, 0, 'f', 2) : unknown);

    const double lossPercent =
        s.sessionFramesTotal > 0
            ? 100.0 * s.sessionFramesNetworkDropped / static_cast<double>(s.sessionFramesTotal)
            : 0;
    const double dropPercent = s.sessionFramesPacerEligible > 0
                                   ? 100.0 * s.sessionFramesPacerDropped /
                                         static_cast<double>(s.sessionFramesPacerEligible)
                                   : 0;
    rows.emplace_back(
        tr("Network loss"),
        s.valid
            ? tr("%1% (%2 frames)").arg(lossPercent, 0, 'f', 2).arg(s.sessionFramesNetworkDropped)
            : unknown);
    rows.emplace_back(tr("Pacer drops"), s.valid ? tr("%1% (%2 of %3 eligible frames)")
                                                       .arg(dropPercent, 0, 'f', 2)
                                                       .arg(s.sessionFramesPacerDropped)
                                                       .arg(s.sessionFramesPacerEligible)
                                                 : unknown);

    rows.emplace_back(tr("Bitrate"), s.valid && !std::isnan(s.bitrateMbps)
                                         ? tr("%1 Mbps").arg(s.bitrateMbps, 0, 'f', 1)
                                         : unknown);
    rows.emplace_back(tr("Latency"), s.valid && s.decodedFps > 0
                                         ? tr("%1 ms").arg(s.latency.totalMs(), 0, 'f', 1)
                                         : unknown);
    QString fec = unknown;
    if (s.valid) {
        fec = s.fecRecoveredFrames >= 0 && s.fecUnrecoverableFrames >= 0
                  ? tr("%1 recovered, %2 unrecoverable")
                        .arg(s.fecRecoveredFrames)
                        .arg(s.fecUnrecoverableFrames)
                  : tr("Not reported");
    }
    rows.emplace_back(tr("FEC frames"), fec);

    return rows;
}

void Session::setupSessionStatsMenu()
{
    if (m_MenuPanel) {
        m_MenuPanel->setSessionStatsProvider([this] { return sessionStatsMenuRows(); });
    }
}
