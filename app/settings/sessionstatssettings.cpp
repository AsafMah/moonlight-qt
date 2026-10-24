#include "sessionstatssettings.h"

#include "path.h"
#include "streaming/stats/sessionstatsrecorder.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QGuiApplication>
#include <QSettings>
#include <QUrl>

#define SER_SHOWSUMMARY "sessionstats/showsummary"

SessionStatsSettings::SessionStatsSettings(QObject* parent) : QObject(parent) {}

bool SessionStatsSettings::recordCsv() const
{
    return SessionStatsRecorder::csvRecordingEnabled();
}

void SessionStatsSettings::setRecordCsv(bool enabled)
{
    if (enabled == recordCsv()) {
        return;
    }
    SessionStatsRecorder::setCsvRecordingEnabled(enabled);
    emit recordCsvChanged();
}

bool SessionStatsSettings::showSummary() const
{
    return QSettings().value(QStringLiteral(SER_SHOWSUMMARY), true).toBool();
}

void SessionStatsSettings::setShowSummary(bool enabled)
{
    if (enabled == showSummary()) {
        return;
    }
    QSettings().setValue(QStringLiteral(SER_SHOWSUMMARY), enabled);
    emit showSummaryChanged();
}

QString SessionStatsSettings::statsDirectory()
{
    // Path keeps logs in the app's storage directory (or the portable root),
    // so its parent is where the app's own data lives
    return QDir::cleanPath(Path::getLogDir() + QStringLiteral("/../stats"));
}

bool SessionStatsSettings::openStatsFolder() const
{
    const QString directory = statsDirectory();
    QDir().mkpath(directory);
    return QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
}

void SessionStatsSettings::copyText(const QString& text) const
{
    if (QClipboard* clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(text);
    }
}
