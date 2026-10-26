#include "apppicker.h"

#include <algorithm>

namespace AppPicker {
namespace {
bool isRunning(const NvApp& app, int runningAppId)
{
    return app.id == runningAppId;
}

int groupOrder(const NvApp& app, int runningAppId, GroupMode groupMode)
{
    switch (groupMode) {
    case GroupMode::Type:
        return app.isAppCollectorGame ? 0 : 1;
    case GroupMode::Status:
        return isRunning(app, runningAppId) ? 0 : 1;
    case GroupMode::None:
        return 0;
    }

    return 0;
}
}

QVector<NvApp> transform(const QVector<NvApp>& apps, int runningAppId, const Options& options)
{
    QVector<NvApp> result;
    const QString query = options.searchText.trimmed();

    for (const NvApp& app : apps) {
        const bool running = isRunning(app, runningAppId);

        if (options.typeFilter == TypeFilter::Games && !app.isAppCollectorGame) {
            continue;
        }
        if (options.typeFilter == TypeFilter::OtherApps && app.isAppCollectorGame) {
            continue;
        }
        if (options.runningFilter == RunningFilter::Running && !running) {
            continue;
        }
        if (options.runningFilter == RunningFilter::NotRunning && running) {
            continue;
        }
        if (options.hiddenFilter == HiddenFilter::Visible && app.hidden) {
            continue;
        }
        if (options.hiddenFilter == HiddenFilter::Hidden && !app.hidden) {
            continue;
        }
        if (!query.isEmpty() && !app.name.contains(query, Qt::CaseInsensitive)) {
            continue;
        }

        result.append(app);
    }

    std::stable_sort(
        result.begin(), result.end(),
        [runningAppId, &options](const NvApp& left, const NvApp& right) {
            const int leftGroup = groupOrder(left, runningAppId, options.groupMode);
            const int rightGroup = groupOrder(right, runningAppId, options.groupMode);
            if (leftGroup != rightGroup) {
                return leftGroup < rightGroup;
            }

            switch (options.sortOrder) {
            case SortOrder::NameAscending: {
                const int comparison = QString::localeAwareCompare(left.name.toCaseFolded(),
                                                                   right.name.toCaseFolded());
                return comparison < 0;
            }
            case SortOrder::NameDescending: {
                const int comparison = QString::localeAwareCompare(left.name.toCaseFolded(),
                                                                   right.name.toCaseFolded());
                return comparison > 0;
            }
            case SortOrder::RunningFirst:
                return isRunning(left, runningAppId) && !isRunning(right, runningAppId);
            case SortOrder::HostOrder:
                return false;
            }

            return false;
        });

    return result;
}

int groupLabel(const NvApp& app, int runningAppId, GroupMode groupMode)
{
    switch (groupMode) {
    case GroupMode::Type:
        return app.isAppCollectorGame ? GamesLabel : OtherAppsLabel;
    case GroupMode::Status:
        return isRunning(app, runningAppId) ? RunningLabel : NotRunningLabel;
    case GroupMode::None:
        return NoGroupLabel;
    }

    return NoGroupLabel;
}
}
