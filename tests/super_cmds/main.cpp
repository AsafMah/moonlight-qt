// Foundation Sunshine super commands: <SuperCmds> parsing and the cached
// copy NvApp keeps in settings. See app/backend/supercmds.h for the format.
#include "backend/nvapp.h"
#include "backend/supercmds.h"

#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>
#include <QXmlStreamReader>

namespace {
bool require(bool condition, const QString& message, QTextStream& err)
{
    if (!condition) {
        err << "FAIL: " << message << '\n';
    }
    return condition;
}

QVector<SuperCmd> commands(std::initializer_list<SuperCmd> list)
{
    return QVector<SuperCmd>(list);
}

bool testFoundationFormat(QTextStream& err)
{
    bool ok = true;

    // nlohmann::json::dump(4) output, as Foundation Sunshine writes it
    const QString dumped = QString::fromUtf8("[\n"
                                             "    {\n"
                                             "        \"id\": \"1\",\n"
                                             "        \"name\": \"Restart Steam\"\n"
                                             "    },\n"
                                             "    {\n"
                                             "        \"id\": \"vol-up\",\n"
                                             "        \"name\": \"\xe9\x9f\xb3\xe9\x87\x8f +\"\n"
                                             "    }\n"
                                             "]");
    QVector<SuperCmd> parsed;
    ok &= require(SuperCmds::parse(dumped, parsed), QStringLiteral("dump(4) array rejected"), err);
    ok &= require(parsed == commands({ { QStringLiteral("1"), QStringLiteral("Restart Steam") },
                                       { QStringLiteral("vol-up"),
                                         QString::fromUtf8("\xe9\x9f\xb3\xe9\x87\x8f +") } }),
                  QStringLiteral("dump(4) array parsed wrong"), err);

    // An app without commands dumps a default json, which is null
    for (const QString& empty :
         { QStringLiteral("null"), QStringLiteral("  null\n"), QString(), QStringLiteral("[]") }) {
        parsed = commands({ { QStringLiteral("stale"), QStringLiteral("Stale") } });
        ok &= require(SuperCmds::parse(empty, parsed) && parsed.isEmpty(),
                      QStringLiteral("'%1' did not give an empty list").arg(empty), err);
    }

    return ok;
}

bool testXmlEscaping(QTextStream& err)
{
    // boost::property_tree::write_xml escapes the JSON text; getAppList()
    // reads it back with readElementText(), as here.
    const QString xml = QStringLiteral(
        "<root status_code=\"200\"><App><AppTitle>Desktop</AppTitle><ID>1</ID>"
        "<SuperCmds>[{&quot;id&quot;:&quot;a&quot;,&quot;name&quot;:&quot;A &amp; B&quot;}]"
        "</SuperCmds></App></root>");
    QXmlStreamReader reader(xml);
    QString text;
    while (reader.readNextStartElement()) {
        if (reader.name() == QLatin1String("SuperCmds")) {
            text = reader.readElementText();
            break;
        }
        if (reader.name() != QLatin1String("root") && reader.name() != QLatin1String("App")) {
            reader.skipCurrentElement();
        }
    }

    QVector<SuperCmd> parsed;
    return require(SuperCmds::parse(text, parsed) &&
                       parsed == commands({ { QStringLiteral("a"), QStringLiteral("A & B") } }),
                   QStringLiteral("XML-escaped SuperCmds parsed wrong: '%1'").arg(text), err);
}

bool testLenientEntries(QTextStream& err)
{
    bool ok = true;
    QVector<SuperCmd> parsed;

    ok &= require(SuperCmds::parse(QStringLiteral("[{\"id\":7,\"name\":\"Seven\"},"
                                                  "{\"name\":\"No id\"},"
                                                  "{\"id\":\"\",\"name\":\"Empty id\"},"
                                                  "\"not an object\","
                                                  "{\"id\":\"x\"},"
                                                  "{\"id\":\"y\",\"name\":\"   \"}]"),
                                   parsed),
                  QStringLiteral("lenient array rejected"), err);
    ok &= require(parsed == commands({ { QStringLiteral("7"), QStringLiteral("Seven") },
                                       { QStringLiteral("x"), QStringLiteral("x") },
                                       { QStringLiteral("y"), QStringLiteral("y") } }),
                  QStringLiteral("lenient entries parsed wrong"), err);

    for (const QString& bad : { QStringLiteral("{\"id\":\"1\"}"), QStringLiteral("["),
                                QStringLiteral("\"text\""), QStringLiteral("nul") }) {
        parsed = commands({ { QStringLiteral("stale"), QStringLiteral("Stale") } });
        ok &= require(!SuperCmds::parse(bad, parsed) && parsed.isEmpty(),
                      QStringLiteral("malformed '%1' was accepted").arg(bad), err);
    }

    return ok;
}

bool testJsonRoundTrip(QTextStream& err)
{
    const QVector<SuperCmd> original =
        commands({ { QStringLiteral("a\"b"), QString::fromUtf8("\xe5\x85\xb3\xe6\x9c\xba") },
                   { QStringLiteral("2"), QStringLiteral("Two") } });
    QVector<SuperCmd> parsed;
    bool ok = require(SuperCmds::parse(SuperCmds::toJson(original), parsed) && parsed == original,
                      QStringLiteral("toJson() did not round-trip"), err);
    ok &= require(SuperCmds::toJson({}) == QLatin1String("[]"),
                  QStringLiteral("empty list is not []"), err);
    return ok;
}

NvApp makeApp(int id, bool supported, QVector<SuperCmd> superCmds)
{
    NvApp app;
    app.id = id;
    app.name = QStringLiteral("App %1").arg(id);
    app.superCmdsSupported = supported;
    app.superCmds = std::move(superCmds);
    return app;
}

bool testNvAppPersistence(QTextStream& err)
{
    bool ok = true;
    QTemporaryDir dir;
    ok &= require(dir.isValid(), QStringLiteral("no temporary directory"), err);

    const QVector<NvApp> apps = {
        makeApp(1, true, commands({ { QStringLiteral("1"), QStringLiteral("Restart Steam") } })),
        makeApp(2, true, {}),
        makeApp(3, false, {}),
    };

    const QString path = dir.filePath(QStringLiteral("apps.ini"));
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.beginWriteArray(QStringLiteral("apps"));
        for (int i = 0; i < apps.size(); i++) {
            settings.setArrayIndex(i);
            apps[i].serialize(settings);
        }
        settings.endArray();
    }

    QSettings settings(path, QSettings::IniFormat);
    const int count = settings.beginReadArray(QStringLiteral("apps"));
    ok &= require(count == apps.size(), QStringLiteral("app count changed"), err);
    for (int i = 0; i < count && i < apps.size(); i++) {
        settings.setArrayIndex(i);
        ok &= require(!(i == 2 && settings.contains(QStringLiteral("supercmds"))),
                      QStringLiteral("unsupported app wrote supercmds"), err);
        const NvApp loaded(settings);
        ok &= require(loaded == apps[i],
                      QStringLiteral("app %1 did not round-trip").arg(apps[i].id), err);
    }
    settings.endArray();

    // The computer only refreshes its app list when the apps differ
    NvApp renamed = apps[0];
    renamed.superCmds[0].name = QStringLiteral("Restart Steam now");
    ok &= require(renamed != apps[0], QStringLiteral("command change not seen by =="), err);
    NvApp unsupported = apps[1];
    unsupported.superCmdsSupported = false;
    ok &= require(unsupported != apps[1], QStringLiteral("support change not seen by =="), err);

    return ok;
}
}

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    QTextStream err(stderr);

    bool ok = true;
    ok &= testFoundationFormat(err);
    ok &= testXmlEscaping(err);
    ok &= testLenientEntries(err);
    ok &= testJsonRoundTrip(err);
    ok &= testNvAppPersistence(err);

    out << (ok ? "PASS" : "FAILED") << '\n';
    return ok ? 0 : 1;
}
