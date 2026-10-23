#pragma once

#include "supercmds.h"

#include <QSettings>

class NvApp
{
public:
    NvApp() {}
    explicit NvApp(QSettings& settings);

    bool operator==(const NvApp& other) const
    {
        return id == other.id && name == other.name && hdrSupported == other.hdrSupported &&
               isAppCollectorGame == other.isAppCollectorGame && hidden == other.hidden &&
               directLaunch == other.directLaunch &&
               superCmdsSupported == other.superCmdsSupported && superCmds == other.superCmds;
    }

    bool operator!=(const NvApp& other) const
    {
        return !operator==(other);
    }

    bool isInitialized()
    {
        // We use isNull() instead of isEmpty() here because we want
        // to detect cases where the name is unassigned, not empty.
        return id != 0 && !name.isNull();
    }

    void
    serialize(QSettings& settings) const;

    int id = 0;
    QString name;
    bool hdrSupported = false;
    bool isAppCollectorGame = false;
    bool hidden = false;
    bool directLaunch = false;

    // Foundation Sunshine super commands (see supercmds.h). Supported means
    // the host sent a <SuperCmds> element for this app, even an empty one.
    bool superCmdsSupported = false;
    QVector<SuperCmd> superCmds;
};

Q_DECLARE_METATYPE(NvApp)
