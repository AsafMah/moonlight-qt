#pragma once

#include "streaming/video/decoder.h"
#include "statssnapshot.h"

// Glue between the FFmpeg decoder's VIDEO_STATS windows and StreamStatsModel.
// Kept out of ffmpeg.cpp so the hooks there stay a few lines long.
namespace VideoStatsBridge {

// Copies the counters and frame rates of a VIDEO_STATS window
StatsWindow toStatsWindow(const VIDEO_STATS& stats);

// Display name of a VIDEO_FORMAT_* value, e.g. "HEVC 10-bit HDR"
QString videoFormatName(int videoFormat, bool hdrActive);

// Fills the cumulative RTP packet counters from moonlight-common-c
void readRtpCounters(StatsWindow& window);

// Formats the metrics that need more than one VIDEO_STATS window (frame rate
// lows, jitter) plus a latency estimate for stats, in the style of the
// performance overlay text. sessionWide selects whole-session frame time
// percentiles instead of the recent ones. Returns the snprintf() result: the
// number of characters written, or what would have been written if length
// was too small.
int formatMetrics(const StatsSnapshot& snapshot, const VIDEO_STATS& stats, bool sessionWide,
                  char* output, int length);

// The debug overlay's last line, shared by the FFmpeg and PyroWave decoders:
// frame rate lows, jitter and the latency estimate, starting with a newline.
// sessionWide and the return value are as for formatMetrics().
int formatOverlayLows(const StatsSnapshot& snapshot, const VIDEO_STATS& stats, bool sessionWide,
                      char* output, int length);

} // namespace VideoStatsBridge
