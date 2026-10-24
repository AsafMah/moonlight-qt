#pragma once

#include "pyrowavevideodecoder.h"

#include <QDateTime>
#include <QString>

// Remembers, per host, whether the host offered PyroWave on its last
// connection, so Settings can show it before the next stream starts.
// Stored in QSettings under "pyrowave/". Safe to call from any thread.
namespace PyrowaveHostStore {

struct Entry
{
    bool valid = false;
    QString uuid;
    QString name;
    PyrowaveHostOffer offer = PyrowaveHostOffer::Unknown;
    QDateTime lastConnected;
};

// Records a successful connection. Unknown keeps the previously stored
// answer, since a connection that could not tell says nothing new.
void recordConnection(const QString& uuid, const QString& name, PyrowaveHostOffer offer);

Entry host(const QString& uuid);

// The host of the most recent successful connection.
Entry lastConnectedHost();

}
