#pragma once

#include "sessionsummary.h"
#include "streamstatsmodel.h"

#include <QDateTime>
#include <QFile>
#include <QString>
#include <QStringList>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

// Follows a StreamStatsModel for the length of a stream: polls it for newly
// published windows on its own thread (never the render or decoder thread),
// optionally appends one CSV row per window, and at the end builds the
// SessionSummary.
//
// CSV format (UTF-8, comma separated, "\n" line ends, one header line). Empty
// fields mean "not known" (no RTT estimate yet, no bitrate source, FEC
// results not reported, no host latency). Columns, in order:
//
//   timestamp               ISO 8601 local time with offset and milliseconds
//                           of the window, e.g. 2026-10-24T21:05:03.250+03:00
//   elapsed_s               seconds since the first frame arrived
//   width, height           stream resolution
//   target_fps              negotiated frame rate
//   codec, decoder          e.g. "HEVC 10-bit HDR", "hevc (d3d11va)"
//   hw_decode               1 for hardware decoding, 0 for software
//   total_fps, received_fps, decoded_fps, rendered_fps
//                           frame rates over the last two windows
//   frametime_avg_ms, frametime_max_ms, frametime_p99_ms, frametime_p999_ms
//                           present-time frame pacing over the last 10 s
//   low_1pct_fps, low_01pct_fps
//                           1% and 0.1% lows over the last 10 s
//   rtt_ms, rtt_var_ms      ENet round trip time and its variance
//   jitter_ms               RFC 3550 frame arrival jitter
//   network_loss_pct        frames lost by the network
//   pacer_drop_pct          eligible frames dropped by the pacer
//   bitrate_mbps            video bitrate
//   latency_host_ms, latency_network_ms, latency_decode_ms,
//   latency_queue_ms, latency_render_ms, latency_total_ms
//                           estimated client-pipeline stages (not glass-to-glass);
//                           an unavailable queue measurement is blank
//   host_min_ms, host_max_ms  host processing latency range
//   reassembly_ms           first to last packet of a frame
//   video_packets, fec_packets
//                           cumulative RTP packets since stream start
//   fec_recovered_frames, fec_unrecoverable_frames
//                           cumulative frames FEC repaired / could not repair
//   frames_total, frames_received, frames_decoded, frames_rendered,
//   frames_network_dropped, frames_pacer_dropped
//                           cumulative session frame counters
class SessionStatsRecorder
{
public:
    // Monotonic microsecond clock matching the model (LiGetMicroseconds())
    using Clock = std::function<uint64_t()>;
    // Wall clock for CSV timestamps and file names
    using WallClock = std::function<QDateTime()>;

    struct Options
    {
        QString host;
        QString app;
        // Write the CSV. The summary is built either way.
        bool writeCsv = false;
        // Directory for the CSV; created when needed
        QString directory;
        // How often the worker thread looks for a new window
        int pollIntervalMs = 250;
    };

    SessionStatsRecorder(const StreamStatsModel& model, Options options, Clock clock,
                         WallClock wallClock = nullptr);
    ~SessionStatsRecorder();

    SessionStatsRecorder(const SessionStatsRecorder&) = delete;
    SessionStatsRecorder& operator=(const SessionStatsRecorder&) = delete;

    // Starts the worker thread
    void start();

    // Stops the worker thread, records any window it has not seen yet, closes
    // the CSV and returns the summary. Later calls return the same summary.
    SessionSummary finish();

    // Records the model's latest window if it is new. Returns true if it was.
    // The worker thread calls this; tests call it directly instead of start().
    bool poll();

    // Path of the CSV once its first row was written, otherwise empty
    QString csvPath() const;

    // Fixed CSV columns, documented above
    static QStringList csvColumns();
    static QString csvHeader();
    static QString csvRow(const StatsSnapshot& snapshot, const QDateTime& windowTime);

    // File name for a session's CSV: "<host>_<app>_<yyyyMMdd-HHmmss>.csv",
    // with characters that are not allowed in file names replaced by '_'
    static QString csvFileName(const QString& host, const QString& app, const QDateTime& start);

    // Default CSV directory: "stats" under the app's data directory
    static QString defaultDirectory();

    // Persistent "record session stats to CSV" setting (off by default)
    static bool csvRecordingEnabled();
    static void setCsvRecordingEnabled(bool enabled);

private:
    void run();
    void writeRow(const StatsSnapshot& snapshot, uint64_t nowUs);
    bool openCsv();

    const StreamStatsModel& m_Model;
    const Options m_Options;
    const Clock m_Clock;
    const WallClock m_WallClock;
    const QDateTime m_StartTime;

    // Guards everything below; poll() runs on the worker and on finish()
    mutable std::mutex m_Lock;
    uint64_t m_LastSequence = 0;
    SessionSummaryBuilder m_Builder;
    std::unique_ptr<QFile> m_Csv;
    bool m_CsvFailed = false;
    QString m_CsvPath;
    bool m_Finished = false;
    SessionSummary m_Summary;

    std::mutex m_StopLock;
    std::condition_variable m_StopCondition;
    bool m_StopRequested = false;
    std::thread m_Thread;
};
