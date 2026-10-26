#include "gui/apppicker.h"

#include <QCoreApplication>
#include <QTextStream>

namespace {
bool require(bool condition, const QString& message, QTextStream& err)
{
    if (!condition) {
        err << "FAIL: " << message << '\n';
    }
    return condition;
}

NvApp app(int id, const QString& name, bool game, bool hidden = false)
{
    NvApp result;
    result.id = id;
    result.name = name;
    result.isAppCollectorGame = game;
    result.hidden = hidden;
    return result;
}

QVector<int> ids(const QVector<NvApp>& apps)
{
    QVector<int> result;
    for (const NvApp& item : apps) {
        result.append(item.id);
    }
    return result;
}

bool testFiltering(QTextStream& err)
{
    const QVector<NvApp> source = {
        app(1, QStringLiteral("Desktop"), false),
        app(2, QStringLiteral("Asterion"), true),
        app(3, QStringLiteral("Hidden Game"), true, true),
        app(4, QStringLiteral("Borealis"), true),
    };

    AppPicker::Options options;
    options.searchText = QStringLiteral("  ASTER  ");
    options.typeFilter = AppPicker::TypeFilter::Games;
    options.runningFilter = AppPicker::RunningFilter::NotRunning;
    options.hiddenFilter = AppPicker::HiddenFilter::Visible;

    bool ok = true;
    ok &= require(ids(AppPicker::transform(source, 3, options)) == QVector<int>({ 2 }),
                  QStringLiteral("search/type/status/hidden filters should compose"), err);

    options.searchText = QStringLiteral("no matching app");
    ok &= require(AppPicker::transform(source, 3, options).isEmpty(),
                  QStringLiteral("search with no matches should return an empty result"), err);

    options.searchText.clear();
    options.typeFilter = AppPicker::TypeFilter::OtherApps;
    options.runningFilter = AppPicker::RunningFilter::All;
    options.hiddenFilter = AppPicker::HiddenFilter::All;
    ok &= require(ids(AppPicker::transform(source, 3, options)) == QVector<int>({ 1 }),
                  QStringLiteral("other-app filter should use the app-collector role"), err);

    options.typeFilter = AppPicker::TypeFilter::All;
    options.hiddenFilter = AppPicker::HiddenFilter::Hidden;
    ok &= require(ids(AppPicker::transform(source, 3, options)) == QVector<int>({ 3 }),
                  QStringLiteral("hidden-only filter"), err);

    options.hiddenFilter = AppPicker::HiddenFilter::All;
    options.runningFilter = AppPicker::RunningFilter::Running;
    ok &= require(ids(AppPicker::transform(source, 3, options)) == QVector<int>({ 3 }),
                  QStringLiteral("running-only filter"), err);
    return ok;
}

bool testSortingAndGrouping(QTextStream& err)
{
    const QVector<NvApp> source = {
        app(7, QStringLiteral("Beta"), false),
        app(2, QStringLiteral("alpha"), true),
        app(5, QStringLiteral("ALPHA"), false),
        app(3, QStringLiteral("Gamma"), true),
    };

    AppPicker::Options options;
    bool ok = true;
    ok &= require(ids(AppPicker::transform(source, 3, options)) == QVector<int>({ 7, 2, 5, 3 }),
                  QStringLiteral("host order should be preserved by default"), err);

    options.sortOrder = AppPicker::SortOrder::NameAscending;
    ok &= require(ids(AppPicker::transform(source, 3, options)) == QVector<int>({ 2, 5, 7, 3 }),
                  QStringLiteral("ascending sort should preserve host order for equal names"), err);

    options.sortOrder = AppPicker::SortOrder::NameDescending;
    ok &=
        require(ids(AppPicker::transform(source, 3, options)) == QVector<int>({ 3, 7, 2, 5 }),
                QStringLiteral("descending sort should preserve host order for equal names"), err);

    options.sortOrder = AppPicker::SortOrder::RunningFirst;
    ok &=
        require(ids(AppPicker::transform(source, 3, options)) == QVector<int>({ 3, 7, 2, 5 }),
                QStringLiteral("running-first sort should keep host order within each state"), err);

    options.sortOrder = AppPicker::SortOrder::HostOrder;
    options.groupMode = AppPicker::GroupMode::Type;
    ok &= require(ids(AppPicker::transform(source, 3, options)) == QVector<int>({ 2, 3, 7, 5 }),
                  QStringLiteral("type grouping should produce contiguous stable groups"), err);

    options.sortOrder = AppPicker::SortOrder::RunningFirst;
    ok &= require(ids(AppPicker::transform(source, 3, options)) == QVector<int>({ 3, 2, 7, 5 }),
                  QStringLiteral("running-first sorting should stay within type groups"), err);

    options.sortOrder = AppPicker::SortOrder::HostOrder;
    options.groupMode = AppPicker::GroupMode::Status;
    ok &= require(ids(AppPicker::transform(source, 3, options)) == QVector<int>({ 3, 7, 2, 5 }),
                  QStringLiteral("status grouping should place running apps first"), err);

    ok &= require(AppPicker::groupLabel(source.at(1), 3, AppPicker::GroupMode::Type) ==
                          AppPicker::GamesLabel &&
                      AppPicker::groupLabel(source.at(0), 3, AppPicker::GroupMode::Type) ==
                          AppPicker::OtherAppsLabel &&
                      AppPicker::groupLabel(source.at(3), 3, AppPicker::GroupMode::Status) ==
                          AppPicker::RunningLabel &&
                      AppPicker::groupLabel(source.at(0), 3, AppPicker::GroupMode::Status) ==
                          AppPicker::NotRunningLabel &&
                      AppPicker::groupLabel(source.at(0), 3, AppPicker::GroupMode::None) ==
                          AppPicker::NoGroupLabel,
                  QStringLiteral("group labels should match group membership"), err);
    return ok;
}
}

int main(int argc, char* argv[])
{
    QCoreApplication appInstance(argc, argv);
    QTextStream err(stderr);

    bool ok = true;
    ok &= testFiltering(err);
    ok &= testSortingAndGrouping(err);

    if (!ok) {
        return 1;
    }

    QTextStream(stdout) << "app_picker=passed\n";
    return 0;
}
