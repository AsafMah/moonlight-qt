// Session's integration with SessionStatsRecorder: the optional per-window
// CSV and the end-of-stream summary. Kept out of session.cpp so the hooks
// there stay one-liners.

#include "session.h"
#include "settings/sessionstatssettings.h"

#include <Limelight.h>

#include <QtDebug>

QString Session::sessionStatsDirectory()
{
    // Moonlight V+: beside the logs, which follow portable mode
    return SessionStatsSettings::statsDirectory();
}

void Session::startSessionStatsRecorder()
{
    // Only one recording per Session; a reconnect keeps the same one
    if (m_StatsRecorder) {
        return;
    }

    SessionStatsRecorder::Options options;
    options.host = m_Computer != nullptr ? m_Computer->name : QString();
    options.app = m_App.name;
    options.writeCsv = SessionStatsRecorder::csvRecordingEnabled();
    options.directory = sessionStatsDirectory();

    m_SessionSummary = SessionSummary();
    m_StatsRecorder = std::make_unique<SessionStatsRecorder>(m_StatsModel, options,
                                                             [] { return LiGetMicroseconds(); });
    m_StatsRecorder->start();
}

void Session::finishSessionStatsRecorder()
{
    if (!m_StatsRecorder) {
        return;
    }

    m_SessionSummary = m_StatsRecorder->finish();
    m_StatsRecorder.reset();

    if (m_SessionSummary.valid) {
        for (const QString& line : m_SessionSummary.toText().split(QLatin1Char('\n'))) {
            qInfo().noquote() << "Session summary:" << line;
        }
    }
}

QVariantMap Session::sessionSummary() const
{
    // Only a stream that ended the way the user asked has a summary to show;
    // a connection failure gets its error dialog instead
    if (m_UnexpectedTermination || !m_SessionSummary.valid) {
        return QVariantMap();
    }
    return m_SessionSummary.toVariantMap();
}
