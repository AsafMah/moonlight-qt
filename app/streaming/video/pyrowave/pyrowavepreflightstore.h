#pragma once

#include <QDateTime>
#include <QString>

// Remembers the most recent stream preflight (PyrowaveVideoDecoder::
// preflight()), so Settings can show which GPU and decode path a PyroWave
// stream used, or why it couldn't start. Stored in QSettings under
// "pyrowave/lastPreflight/". Safe to call from any thread.
namespace PyrowavePreflightStore {

struct Entry
{
    bool valid = false;
    bool ok = false;
    // The GPU the stream used, or the one that failed; empty when the check
    // ended before a GPU was picked.
    QString gpu;
    // How frames are decoded, e.g. "Vulkan compute" or "CPU staging". Only
    // when ok.
    QString decodePath;
    // Translated. Only when not ok.
    QString reason;
    int width = 0;
    int height = 0;
    int frameRate = 0;
    // 0 = SDR, 1 = HDR10, 2 = HLG
    int hdrMode = 0;
    QDateTime time;
};

void record(const Entry& entry);

Entry last();

}
