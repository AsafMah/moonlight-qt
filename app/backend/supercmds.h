#pragma once

#include <QString>
#include <QVector>

// Foundation Sunshine "Super Commands": per-app commands that the host lists
// in /applist and runs on GET /supercmd?cmdId=<id> (HTTPS).
//
// Wire format, from Foundation Sunshine src/nvhttp/apps.cpp (nvhttp::apps::list):
// every <App> carries a <SuperCmds> element whose text is a JSON document,
// nlohmann::json::dump(4) of an array of { "id": string, "name": string }.
// For an app without commands the host dumps a default-constructed json, so
// the text is "null". Stock Sunshine and GFE never send the element.
struct SuperCmd
{
    QString id;
    QString name;

    bool operator==(const SuperCmd& other) const { return id == other.id && name == other.name; }
    bool operator!=(const SuperCmd& other) const { return !(*this == other); }
};

namespace SuperCmds {
// Parses the text of a <SuperCmds> element into commands. "null", blank text
// and an empty array give an empty list. Entries that are not objects or have
// no id are skipped; numeric ids are converted to text, and a missing or blank
// name falls back to the id. Returns false, with an empty list, when the text
// is neither null nor a JSON array.
bool parse(const QString& text, QVector<SuperCmd>& commands);

// Compact JSON array, readable by parse(). Used to cache commands in settings.
QString toJson(const QVector<SuperCmd>& commands);
}
