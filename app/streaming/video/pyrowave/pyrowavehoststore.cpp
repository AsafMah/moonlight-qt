#include "pyrowavehoststore.h"

#include <QSettings>

#define SER_PYROWAVE_LAST_HOST "pyrowave/lastHost"
#define SER_PYROWAVE_NAME "name"
#define SER_PYROWAVE_OFFER "offer"
#define SER_PYROWAVE_LAST_CONNECTED "lastConnected"

namespace {

PyrowaveHostOffer offerFromInt(int value)
{
    switch (value) {
    case static_cast<int>(PyrowaveHostOffer::Offered):
        return PyrowaveHostOffer::Offered;
    case static_cast<int>(PyrowaveHostOffer::NotOffered):
        return PyrowaveHostOffer::NotOffered;
    default:
        return PyrowaveHostOffer::Unknown;
    }
}

QString hostGroup(const QString& uuid)
{
    return QStringLiteral("pyrowave/hosts/") + uuid;
}

}

namespace PyrowaveHostStore {

void recordConnection(const QString& uuid, const QString& name, PyrowaveHostOffer offer)
{
    if (uuid.isEmpty()) {
        return;
    }

    QSettings settings;
    settings.beginGroup(hostGroup(uuid));
    settings.setValue(SER_PYROWAVE_NAME, name);
    settings.setValue(SER_PYROWAVE_LAST_CONNECTED, QDateTime::currentDateTimeUtc());
    if (offer != PyrowaveHostOffer::Unknown) {
        settings.setValue(SER_PYROWAVE_OFFER, static_cast<int>(offer));
    }
    settings.endGroup();
    settings.setValue(SER_PYROWAVE_LAST_HOST, uuid);
}

Entry host(const QString& uuid)
{
    Entry entry;
    if (uuid.isEmpty()) {
        return entry;
    }

    QSettings settings;
    settings.beginGroup(hostGroup(uuid));
    if (!settings.contains(SER_PYROWAVE_LAST_CONNECTED)) {
        return entry;
    }

    entry.valid = true;
    entry.uuid = uuid;
    entry.name = settings.value(SER_PYROWAVE_NAME).toString();
    // A missing value reads as 0, which is PyrowaveHostOffer::Unknown.
    entry.offer = offerFromInt(settings.value(SER_PYROWAVE_OFFER).toInt());
    entry.lastConnected = settings.value(SER_PYROWAVE_LAST_CONNECTED).toDateTime();
    return entry;
}

Entry lastConnectedHost()
{
    QSettings settings;
    return host(settings.value(SER_PYROWAVE_LAST_HOST).toString());
}

}
