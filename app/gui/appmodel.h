#pragma once

#include "backend/boxartmanager.h"
#include "backend/computermanager.h"
#include "apppicker.h"
#include "streaming/session.h"

#include <QAbstractListModel>
#include <QUrl>

class AppModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY pickerOptionsChanged)
    Q_PROPERTY(int typeFilter READ typeFilter WRITE setTypeFilter NOTIFY pickerOptionsChanged)
    Q_PROPERTY(
        int runningFilter READ runningFilter WRITE setRunningFilter NOTIFY pickerOptionsChanged)
    Q_PROPERTY(int hiddenFilter READ hiddenFilter WRITE setHiddenFilter NOTIFY pickerOptionsChanged)
    Q_PROPERTY(int sortOrder READ sortOrder WRITE setSortOrder NOTIFY pickerOptionsChanged)
    Q_PROPERTY(int groupMode READ groupMode WRITE setGroupMode NOTIFY pickerOptionsChanged)

public:
    enum Roles
    {
        NameRole = Qt::UserRole,
        RunningRole,
        BoxArtRole,
        HiddenRole,
        AppIdRole,
        DirectLaunchRole,
        AppCollectorGameRole,
        GroupLabelRole,
    };
    Q_ENUM(Roles)

    enum TypeFilter
    {
        TypeAll = static_cast<int>(AppPicker::TypeFilter::All),
        TypeGames = static_cast<int>(AppPicker::TypeFilter::Games),
        TypeOtherApps = static_cast<int>(AppPicker::TypeFilter::OtherApps),
    };
    Q_ENUM(TypeFilter)

    enum RunningFilter
    {
        RunningAll = static_cast<int>(AppPicker::RunningFilter::All),
        RunningOnly = static_cast<int>(AppPicker::RunningFilter::Running),
        NotRunningOnly = static_cast<int>(AppPicker::RunningFilter::NotRunning),
    };
    Q_ENUM(RunningFilter)

    enum HiddenFilter
    {
        HiddenAll = static_cast<int>(AppPicker::HiddenFilter::All),
        VisibleOnly = static_cast<int>(AppPicker::HiddenFilter::Visible),
        HiddenOnly = static_cast<int>(AppPicker::HiddenFilter::Hidden),
    };
    Q_ENUM(HiddenFilter)

    enum SortOrder
    {
        HostOrder = static_cast<int>(AppPicker::SortOrder::HostOrder),
        NameAscending = static_cast<int>(AppPicker::SortOrder::NameAscending),
        NameDescending = static_cast<int>(AppPicker::SortOrder::NameDescending),
        RunningFirst = static_cast<int>(AppPicker::SortOrder::RunningFirst),
    };
    Q_ENUM(SortOrder)

    enum GroupMode
    {
        NoGrouping = static_cast<int>(AppPicker::GroupMode::None),
        GroupByType = static_cast<int>(AppPicker::GroupMode::Type),
        GroupByStatus = static_cast<int>(AppPicker::GroupMode::Status),
    };
    Q_ENUM(GroupMode)

    enum GroupLabels
    {
        NoGroupLabel = AppPicker::NoGroupLabel,
        GamesLabel = AppPicker::GamesLabel,
        OtherAppsLabel = AppPicker::OtherAppsLabel,
        RunningLabel = AppPicker::RunningLabel,
        NotRunningLabel = AppPicker::NotRunningLabel,
    };
    Q_ENUM(GroupLabels)

    explicit AppModel(QObject *parent = nullptr);

    // Must be called before any QAbstractListModel functions
    Q_INVOKABLE void initialize(ComputerManager* computerManager, int computerIndex, bool showHiddenGames);

    Q_INVOKABLE Session* createSessionForApp(int appId, const QString& displayId = QString());

    Q_INVOKABLE QUrl getAppBoxArt(int appId);

    Q_INVOKABLE int getAppIndexById(int appId) const;

    Q_INVOKABLE QVariantList getDisplayList();

    Q_INVOKABLE int getDirectLaunchAppIndex();

    Q_INVOKABLE int getRunningAppId();

    Q_INVOKABLE QString getRunningAppName();

    Q_INVOKABLE void quitRunningApp();

    Q_INVOKABLE void setAppHidden(int appId, bool hidden);

    Q_INVOKABLE void setAppDirectLaunch(int appId, bool directLaunch);

    Q_INVOKABLE QVariantList getConnectionAddresses();

    // 地址选择框的条目列表。PcView 和 AppView 用同一个 QML 组件
    // （SelectAddressDialog），所以条目形状和「哪一项算选中」的判定必须一致 ——
    // 放在一处，ComputerModel 也调这里。
    //
    // 翻译上下文特意留在 AppModel：这几个字符串在 AppModel 下已经有译文了，
    // 换个上下文会让它们退回英文。
    static QVariantList buildConnectionAddressList(NvComputer* computer);

    Q_INVOKABLE bool hasMultipleConnectionAddresses();

    Q_INVOKABLE bool setActiveAddress(QString address, int port);

    // Undo a setActiveAddress() pin and go back to automatic selection.
    Q_INVOKABLE bool resetToAutomaticAddress();

    Q_INVOKABLE QVariantMap getActiveAddressInfo();

    // Defensive self-heal: re-read m_Computer->currentGameId and force-emit
    // RunningRole dataChanged for the affected rows if it differs from our
    // cached m_CurrentGameId. Useful when the polling thread updated state
    // through a path that bypassed the computerStateChanged signal (e.g.
    // mDNS re-resolution folding via PendingAddTask under contended locks).
    // Cheap when nothing has changed.
    Q_INVOKABLE void forceSyncCurrentGame();

    QString searchText() const;
    void setSearchText(const QString& searchText);

    int typeFilter() const;
    void setTypeFilter(int typeFilter);

    int runningFilter() const;
    void setRunningFilter(int runningFilter);

    int hiddenFilter() const;
    void setHiddenFilter(int hiddenFilter);

    int sortOrder() const;
    void setSortOrder(int sortOrder);

    int groupMode() const;
    void setGroupMode(int groupMode);

    Q_INVOKABLE void resetPickerOptions();

    QVariant data(const QModelIndex &index, int role) const override;

    int rowCount(const QModelIndex &parent) const override;

    virtual QHash<int, QByteArray> roleNames() const override;

private slots:
    void handleComputerStateChanged(NvComputer* computer);

    void handleBoxArtLoaded(NvComputer* computer, NvApp app, QUrl image);

signals:
    void computerLost();
    void pickerOptionsChanged();

private:
    void updateAppList(QVector<NvApp> newList);

    QVector<NvApp> getRouteApps(const QVector<NvApp>& appList);

    bool isAppCurrentlyVisible(const NvApp& app);

    void refreshVisibleApps(const QVector<int>& rolesWhenUnchanged = {});

    AppPicker::Options pickerOptions() const;

    const NvApp* findAppById(int appId) const;

    NvComputer* m_Computer = nullptr;
    BoxArtManager m_BoxArtManager;
    ComputerManager* m_ComputerManager = nullptr;
    QVector<NvApp> m_RouteApps, m_VisibleApps, m_AllApps;
    int m_CurrentGameId = 0;
    bool m_ShowHiddenGames = false;
    AppPicker::Options m_PickerOptions;
};
