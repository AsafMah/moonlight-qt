#include "supercmds.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QVariant>

bool SuperCmds::parse(const QString& text, QVector<SuperCmd>& commands)
{
    commands.clear();

    // QJsonDocument only holds arrays and objects, so handle the JSON null
    // that Foundation Sunshine sends for apps without commands here.
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed == QLatin1String("null")) {
        return true;
    }

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(trimmed.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) {
        return false;
    }

    const QJsonArray array = document.array();
    for (const QJsonValue& entry : array) {
        if (!entry.isObject()) {
            continue;
        }

        const QJsonObject object = entry.toObject();
        const QJsonValue idValue = object.value(QStringLiteral("id"));
        QString id;
        if (idValue.isString()) {
            id = idValue.toString();
        } else if (idValue.isDouble()) {
            id = idValue.toVariant().toString();
        }
        if (id.isEmpty()) {
            continue;
        }

        QString name = object.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty()) {
            name = id;
        }
        commands.append(SuperCmd{ id, name });
    }
    return true;
}

QString SuperCmds::toJson(const QVector<SuperCmd>& commands)
{
    QJsonArray array;
    for (const SuperCmd& command : commands) {
        QJsonObject object;
        object.insert(QStringLiteral("id"), command.id);
        object.insert(QStringLiteral("name"), command.name);
        array.append(object);
    }
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}
