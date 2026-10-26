#pragma once

#include "backend/nvapp.h"

#include <QVector>

namespace AppPicker {
enum class TypeFilter
{
    All,
    Games,
    OtherApps,
};

enum class RunningFilter
{
    All,
    Running,
    NotRunning,
};

enum class HiddenFilter
{
    All,
    Visible,
    Hidden,
};

enum class SortOrder
{
    HostOrder,
    NameAscending,
    NameDescending,
    RunningFirst,
};

enum class GroupMode
{
    None,
    Type,
    Status,
};

enum GroupLabel
{
    NoGroupLabel,
    GamesLabel,
    OtherAppsLabel,
    RunningLabel,
    NotRunningLabel,
};

struct Options
{
    QString searchText;
    TypeFilter typeFilter = TypeFilter::All;
    RunningFilter runningFilter = RunningFilter::All;
    HiddenFilter hiddenFilter = HiddenFilter::All;
    SortOrder sortOrder = SortOrder::HostOrder;
    GroupMode groupMode = GroupMode::None;
};

QVector<NvApp> transform(const QVector<NvApp>& apps, int runningAppId, const Options& options);
int groupLabel(const NvApp& app, int runningAppId, GroupMode groupMode);
}
