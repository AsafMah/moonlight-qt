#include "pyrowavepreflightstore.h"

#include <QSettings>

#define SER_PYROWAVE_PREFLIGHT "pyrowave/lastPreflight"
#define SER_PYROWAVE_OK "ok"
#define SER_PYROWAVE_GPU "gpu"
#define SER_PYROWAVE_DECODE_PATH "decodePath"
#define SER_PYROWAVE_REASON "reason"
#define SER_PYROWAVE_WIDTH "width"
#define SER_PYROWAVE_HEIGHT "height"
#define SER_PYROWAVE_FRAME_RATE "frameRate"
#define SER_PYROWAVE_HDR_MODE "hdrMode"
#define SER_PYROWAVE_TIME "time"

namespace PyrowavePreflightStore {

void record(const Entry& entry)
{
    QSettings settings;
    settings.beginGroup(SER_PYROWAVE_PREFLIGHT);
    settings.setValue(SER_PYROWAVE_OK, entry.ok);
    settings.setValue(SER_PYROWAVE_GPU, entry.gpu);
    settings.setValue(SER_PYROWAVE_DECODE_PATH, entry.decodePath);
    settings.setValue(SER_PYROWAVE_REASON, entry.reason);
    settings.setValue(SER_PYROWAVE_WIDTH, entry.width);
    settings.setValue(SER_PYROWAVE_HEIGHT, entry.height);
    settings.setValue(SER_PYROWAVE_FRAME_RATE, entry.frameRate);
    settings.setValue(SER_PYROWAVE_HDR_MODE, entry.hdrMode);
    settings.setValue(SER_PYROWAVE_TIME,
                      entry.time.isValid() ? entry.time : QDateTime::currentDateTimeUtc());
    settings.endGroup();
}

Entry last()
{
    Entry entry;

    QSettings settings;
    settings.beginGroup(SER_PYROWAVE_PREFLIGHT);
    if (!settings.contains(SER_PYROWAVE_TIME)) {
        return entry;
    }

    entry.valid = true;
    entry.ok = settings.value(SER_PYROWAVE_OK).toBool();
    entry.gpu = settings.value(SER_PYROWAVE_GPU).toString();
    entry.decodePath = settings.value(SER_PYROWAVE_DECODE_PATH).toString();
    entry.reason = settings.value(SER_PYROWAVE_REASON).toString();
    entry.width = settings.value(SER_PYROWAVE_WIDTH).toInt();
    entry.height = settings.value(SER_PYROWAVE_HEIGHT).toInt();
    entry.frameRate = settings.value(SER_PYROWAVE_FRAME_RATE).toInt();
    entry.hdrMode = settings.value(SER_PYROWAVE_HDR_MODE).toInt();
    entry.time = settings.value(SER_PYROWAVE_TIME).toDateTime();
    return entry;
}

}
