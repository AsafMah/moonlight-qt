#pragma once

#include <QObject>
#include <QString>

// Settings and helpers of the end-of-session summary, shared by the settings
// page and the summary dialog as the SessionStatsSettings QML singleton.
//
// "Record session stats to CSV" is the core SessionStatsRecorder setting
// (sessionstats/recordcsv), which Session reads when a stream starts.
// "Show summary after streaming" only affects the UI.
class SessionStatsSettings : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool recordCsv READ recordCsv WRITE setRecordCsv NOTIFY recordCsvChanged)
    Q_PROPERTY(bool showSummary READ showSummary WRITE setShowSummary NOTIFY showSummaryChanged)
    Q_PROPERTY(QString statsDirectory READ statsDirectory CONSTANT)

public:
    explicit SessionStatsSettings(QObject* parent = nullptr);

    bool recordCsv() const;
    void setRecordCsv(bool enabled);

    bool showSummary() const;
    void setShowSummary(bool enabled);

    // Where the session CSVs go: "stats" beside the logs, so portable builds
    // keep them next to the executable
    static QString statsDirectory();

    // Opens the CSV folder in the file manager, creating it if needed
    Q_INVOKABLE bool openStatsFolder() const;

    // Puts text on the system clipboard
    Q_INVOKABLE void copyText(const QString& text) const;

signals:
    void recordCsvChanged();
    void showSummaryChanged();
};
