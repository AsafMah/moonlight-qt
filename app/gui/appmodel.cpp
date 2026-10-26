#include "appmodel.h"
#include "../backend/nvhttp.h"

#include <QReadLocker>
#include <QWriteLocker>

namespace {
QString getAddressType(const NvAddress& address,
                       const NvAddress& localAddress,
                       const NvAddress& remoteAddress,
                       const NvAddress& manualAddress,
                       const NvAddress& ipv6Address)
{
    if (address == localAddress) {
        return AppModel::tr("Local network");
    }
    if (address == remoteAddress) {
        return AppModel::tr("Remote network");
    }
    if (address == manualAddress) {
        return AppModel::tr("Manual");
    }
    if (address == ipv6Address) {
        return AppModel::tr("IPv6 network");
    }

    return AppModel::tr("Other network");
}
}

AppModel::AppModel(QObject *parent)
    : QAbstractListModel(parent)
{
    connect(&m_BoxArtManager, &BoxArtManager::boxArtLoadComplete,
            this, &AppModel::handleBoxArtLoaded);
}

void AppModel::initialize(ComputerManager* computerManager, int computerIndex, bool showHiddenGames)
{
    m_ComputerManager = computerManager;
    connect(m_ComputerManager, &ComputerManager::computerStateChanged,
            this, &AppModel::handleComputerStateChanged);

    Q_ASSERT(computerIndex < m_ComputerManager->getComputers().count());
    m_Computer = m_ComputerManager->getComputers().at(computerIndex);
    m_CurrentGameId = m_Computer->currentGameId;
    m_ShowHiddenGames = showHiddenGames;

    updateAppList(m_Computer->appList);
}

int AppModel::getRunningAppId()
{
    return m_CurrentGameId;
}

QString AppModel::getRunningAppName()
{
    if (m_CurrentGameId != 0) {
        for (int i = 0; i < m_AllApps.count(); i++) {
            if (m_AllApps[i].id == m_CurrentGameId) {
                return m_AllApps[i].name;
            }
        }
    }

    return nullptr;
}

Session* AppModel::createSessionForApp(int appId, const QString& displayId)
{
    const NvApp* app = findAppById(appId);
    if (app == nullptr || m_Computer == nullptr) {
        qWarning() << "Cannot create a session for unknown app ID:" << appId;
        return nullptr;
    }

    NvApp appCopy = *app;
    if (displayId == QStringLiteral("vdd")) {
        return new Session(m_Computer, appCopy, nullptr, QString(), true);
    }
    if (!displayId.isEmpty()) {
        return new Session(m_Computer, appCopy, nullptr, displayId, false);
    }

    return new Session(m_Computer, appCopy);
}

QUrl AppModel::getAppBoxArt(int appId)
{
    const NvApp* app = findAppById(appId);
    if (app == nullptr || m_Computer == nullptr) {
        qWarning() << "Cannot load box art for unknown app ID:" << appId;
        return QUrl();
    }
    NvApp appCopy = *app;
    return m_BoxArtManager.loadBoxArt(m_Computer, appCopy);
}

int AppModel::getAppIndexById(int appId) const
{
    for (int i = 0; i < m_VisibleApps.count(); i++) {
        if (m_VisibleApps[i].id == appId) {
            return i;
        }
    }
    return -1;
}

const NvApp* AppModel::findAppById(int appId) const
{
    for (const NvApp& app : m_AllApps) {
        if (app.id == appId) {
            return &app;
        }
    }
    return nullptr;
}

QVariantList AppModel::getDisplayList()
{
    QVariantList displays;

    if (!m_Computer || m_Computer->activeAddress.isNull()) {
        return displays;
    }

    try {
        NvHTTP http(m_Computer);
        displays = http.getDisplays();
    } catch (...) {
        qWarning() << "Failed to get display list";
    }

    return displays;
}

int AppModel::getDirectLaunchAppIndex()
{
    for (int i = 0; i < m_VisibleApps.count(); i++) {
        if (m_VisibleApps[i].directLaunch) {
            return i;
        }
    }

    return -1;
}

int AppModel::rowCount(const QModelIndex &parent) const
{
    // For list models only the root node (an invalid parent) should return the list's size. For all
    // other (valid) parents, rowCount() should return 0 so that it does not become a tree model.
    if (parent.isValid())
        return 0;

    return m_VisibleApps.count();
}

QString AppModel::searchText() const
{
    return m_PickerOptions.searchText;
}

void AppModel::setSearchText(const QString& searchText)
{
    if (m_PickerOptions.searchText == searchText) {
        return;
    }
    m_PickerOptions.searchText = searchText;
    refreshVisibleApps();
    emit pickerOptionsChanged();
}

int AppModel::typeFilter() const
{
    return static_cast<int>(m_PickerOptions.typeFilter);
}

void AppModel::setTypeFilter(int typeFilter)
{
    if (typeFilter < TypeAll || typeFilter > TypeOtherApps) {
        qWarning() << "Ignoring invalid app type filter:" << typeFilter;
        return;
    }
    if (typeFilter == this->typeFilter()) {
        return;
    }
    m_PickerOptions.typeFilter = static_cast<AppPicker::TypeFilter>(typeFilter);
    refreshVisibleApps();
    emit pickerOptionsChanged();
}

int AppModel::runningFilter() const
{
    return static_cast<int>(m_PickerOptions.runningFilter);
}

void AppModel::setRunningFilter(int runningFilter)
{
    if (runningFilter < RunningAll || runningFilter > NotRunningOnly) {
        qWarning() << "Ignoring invalid app running filter:" << runningFilter;
        return;
    }
    if (runningFilter == this->runningFilter()) {
        return;
    }
    m_PickerOptions.runningFilter = static_cast<AppPicker::RunningFilter>(runningFilter);
    refreshVisibleApps();
    emit pickerOptionsChanged();
}

int AppModel::hiddenFilter() const
{
    return static_cast<int>(m_PickerOptions.hiddenFilter);
}

void AppModel::setHiddenFilter(int hiddenFilter)
{
    if (hiddenFilter < HiddenAll || hiddenFilter > HiddenOnly) {
        qWarning() << "Ignoring invalid app hidden filter:" << hiddenFilter;
        return;
    }
    if (hiddenFilter == this->hiddenFilter()) {
        return;
    }
    m_PickerOptions.hiddenFilter = static_cast<AppPicker::HiddenFilter>(hiddenFilter);
    refreshVisibleApps();
    emit pickerOptionsChanged();
}

int AppModel::sortOrder() const
{
    return static_cast<int>(m_PickerOptions.sortOrder);
}

void AppModel::setSortOrder(int sortOrder)
{
    if (sortOrder < HostOrder || sortOrder > RunningFirst) {
        qWarning() << "Ignoring invalid app sort order:" << sortOrder;
        return;
    }
    if (sortOrder == this->sortOrder()) {
        return;
    }
    m_PickerOptions.sortOrder = static_cast<AppPicker::SortOrder>(sortOrder);
    refreshVisibleApps();
    emit pickerOptionsChanged();
}

int AppModel::groupMode() const
{
    return static_cast<int>(m_PickerOptions.groupMode);
}

void AppModel::setGroupMode(int groupMode)
{
    if (groupMode < NoGrouping || groupMode > GroupByStatus) {
        qWarning() << "Ignoring invalid app group mode:" << groupMode;
        return;
    }
    if (groupMode == this->groupMode()) {
        return;
    }
    m_PickerOptions.groupMode = static_cast<AppPicker::GroupMode>(groupMode);
    refreshVisibleApps({ GroupLabelRole });
    emit pickerOptionsChanged();
}

void AppModel::resetPickerOptions()
{
    m_PickerOptions = AppPicker::Options();
    refreshVisibleApps({ GroupLabelRole });
    emit pickerOptionsChanged();
}

QVariant AppModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return QVariant();

    Q_ASSERT(index.row() < m_VisibleApps.count());
    NvApp app = m_VisibleApps.at(index.row());

    switch (role)
    {
    case NameRole:
        return app.name;
    case RunningRole:
        return m_CurrentGameId == app.id;
    case BoxArtRole:
        // FIXME: const-correctness
        return const_cast<BoxArtManager&>(m_BoxArtManager).loadBoxArt(m_Computer, app);
    case HiddenRole:
        return app.hidden;
    case AppIdRole:
        return app.id;
    case DirectLaunchRole:
        return app.directLaunch;
    case AppCollectorGameRole:
        return app.isAppCollectorGame;
    case GroupLabelRole:
        return AppPicker::groupLabel(app, m_CurrentGameId, m_PickerOptions.groupMode);
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> AppModel::roleNames() const
{
    QHash<int, QByteArray> names;

    names[NameRole] = "name";
    names[RunningRole] = "running";
    names[BoxArtRole] = "boxart";
    names[HiddenRole] = "hidden";
    names[AppIdRole] = "appid";
    names[DirectLaunchRole] = "directLaunch";
    names[AppCollectorGameRole] = "appCollectorGame";
    names[GroupLabelRole] = "groupLabel";

    return names;
}

void AppModel::quitRunningApp()
{
    m_ComputerManager->quitRunningApp(m_Computer);
}

bool AppModel::isAppCurrentlyVisible(const NvApp& app)
{
    for (const NvApp& routeApp : std::as_const(m_RouteApps)) {
        if (app.id == routeApp.id) {
            return true;
        }
    }

    return false;
}

QVector<NvApp> AppModel::getRouteApps(const QVector<NvApp>& appList)
{
    QVector<NvApp> routeApps;

    for (const NvApp& app : appList) {
        // Don't immediately hide games that were previously visible. This
        // allows users to easily uncheck the "Hide App" checkbox if they
        // check it by mistake.
        if (m_ShowHiddenGames || !app.hidden || isAppCurrentlyVisible(app)) {
            routeApps.append(app);
        }
    }

    return routeApps;
}

void AppModel::updateAppList(QVector<NvApp> newList)
{
    m_AllApps = newList;

    m_RouteApps = getRouteApps(newList);
    refreshVisibleApps();
}

AppPicker::Options AppModel::pickerOptions() const
{
    return m_PickerOptions;
}

void AppModel::refreshVisibleApps(const QVector<int>& rolesWhenUnchanged)
{
    const QVector<NvApp> newVisibleList =
        AppPicker::transform(m_RouteApps, m_CurrentGameId, pickerOptions());
    if (m_VisibleApps != newVisibleList) {
        beginResetModel();
        m_VisibleApps = newVisibleList;
        endResetModel();
    } else if (!rolesWhenUnchanged.isEmpty() && !m_VisibleApps.isEmpty()) {
        emit dataChanged(createIndex(0, 0), createIndex(m_VisibleApps.count() - 1, 0),
                         rolesWhenUnchanged);
    }
}

void AppModel::setAppHidden(int appId, bool hidden)
{
    if (findAppById(appId) == nullptr || m_Computer == nullptr) {
        qWarning() << "Cannot update hidden state for unknown app ID:" << appId;
        return;
    }

    {
        QWriteLocker lock(&m_Computer->lock);

        for (NvApp& app : m_Computer->appList) {
            if (app.id == appId) {
                app.hidden = hidden;
                break;
            }
        }
    }

    m_ComputerManager->clientSideAttributeUpdated(m_Computer);
}

void AppModel::setAppDirectLaunch(int appId, bool directLaunch)
{
    if (findAppById(appId) == nullptr || m_Computer == nullptr) {
        qWarning() << "Cannot update direct launch for unknown app ID:" << appId;
        return;
    }

    {
        QWriteLocker lock(&m_Computer->lock);

        for (NvApp& app : m_Computer->appList) {
            if (directLaunch) {
                // We must clear direct launch from all other apps
                // to set it on the new app.
                app.directLaunch = app.id == appId;
            }
            else if (app.id == appId) {
                // If we're clearing direct launch, we're done once we
                // find our matching app ID.
                app.directLaunch = false;
                break;
            }
        }
    }

    m_ComputerManager->clientSideAttributeUpdated(m_Computer);
}

QVariantList AppModel::getConnectionAddresses()
{
    return buildConnectionAddressList(m_Computer);
}

QVariantList AppModel::buildConnectionAddressList(NvComputer* computer)
{
    QVariantList addresses;
    if (!computer) {
        return addresses;
    }

    QVector<NvAddress> allAddresses = computer->uniqueAddresses();

    NvAddress localAddress;
    NvAddress remoteAddress;
    NvAddress manualAddress;
    NvAddress ipv6Address;
    NvAddress pinnedAddress;

    {
        QReadLocker lock(&computer->lock);
        localAddress = computer->localAddress;
        remoteAddress = computer->remoteAddress;
        manualAddress = computer->manualAddress;
        ipv6Address = computer->ipv6Address;
        pinnedAddress = computer->pinnedAddress;
    }

    // Add "Auto (default)" option
    QVariantMap autoItem;
    autoItem["address"] = "";
    autoItem["port"] = 0;
    autoItem["display"] = tr("Auto (default)");
    autoItem["type"] = tr("Automatic selection with fallback");
    // 没固定地址就是自动模式。这一位是弹窗预选的依据 —— 光看 activeAddress
    // 分不出「自动选中的」和「用户固定的」。
    autoItem["isActive"] = pinnedAddress.isNull();
    autoItem["isAuto"] = true;
    addresses.append(autoItem);

    for (const NvAddress& address : allAddresses) {
        QVariantMap item;
        item["address"] = address.address();
        item["port"] = static_cast<int>(address.port());
        item["display"] = address.toString();
        item["type"] = getAddressType(address, localAddress, remoteAddress, manualAddress, ipv6Address);
        // 具体条目只在「被固定」时算选中；自动模式下选中的是上面的「自动」项，
        // 实际生效的地址交给轮询，不在这里标。
        item["isActive"] = !pinnedAddress.isNull() && address == pinnedAddress;
        item["isAuto"] = false;
        item["isTested"] = computer->hasAddressTestSucceeded(address);
        addresses.append(item);
    }

    return addresses;
}

bool AppModel::hasMultipleConnectionAddresses()
{
    if (!m_Computer) {
        return false;
    }
    return m_Computer->uniqueAddresses().count() > 1;
}

bool AppModel::setActiveAddress(QString address, int port)
{
    if (!m_Computer) {
        return false;
    }

    if (address.isEmpty() || port <= 0) {
        return false;
    }

    NvAddress selectedAddress(address, static_cast<uint16_t>(port));

    // Verify the address is one of the known addresses
    bool found = false;
    for (const NvAddress& addr : m_Computer->uniqueAddresses()) {
        if (addr == selectedAddress) {
            found = true;
            break;
        }
    }
    if (!found) {
        qWarning() << "Address is not a known address:" << selectedAddress.toString();
        return false;
    }

    m_Computer->pinAddress(selectedAddress);

    return true;
}

bool AppModel::resetToAutomaticAddress()
{
    if (!m_Computer) {
        return false;
    }

    return m_Computer->resetToAutomaticAddress();
}

QVariantMap AppModel::getActiveAddressInfo()
{
    QVariantMap info;
    if (!m_Computer) {
        return info;
    }

    NvAddress activeAddress;
    NvAddress localAddress;
    NvAddress remoteAddress;
    NvAddress manualAddress;
    NvAddress ipv6Address;

    {
        QReadLocker lock(&m_Computer->lock);
        activeAddress = m_Computer->activeAddress;
        localAddress = m_Computer->localAddress;
        remoteAddress = m_Computer->remoteAddress;
        manualAddress = m_Computer->manualAddress;
        ipv6Address = m_Computer->ipv6Address;
    }

    info["address"] = activeAddress.address();
    info["port"] = static_cast<int>(activeAddress.port());
    info["display"] = activeAddress.toString();
    info["type"] = getAddressType(activeAddress, localAddress, remoteAddress, manualAddress, ipv6Address);

    return info;
}

void AppModel::handleComputerStateChanged(NvComputer* computer)
{
    // Ignore updates for computers that aren't ours
    if (computer != m_Computer) {
        return;
    }

    // If the computer has gone offline or we've been unpaired,
    // signal the UI so we can go back to the PC view.
    if (m_Computer->state == NvComputer::CS_OFFLINE ||
            m_Computer->pairState == NvComputer::PS_NOT_PAIRED) {
        emit computerLost();
        return;
    }

    // First, process additions/removals from the app list. This
    // is required because the new game may now be running, so
    // we can't check that first.
    if (computer->appList != m_AllApps) {
        updateAppList(computer->appList);
    }

    // Finally, process changes to the active app. This may change the running
    // filter, sort, or group, so rebuild the displayed rows after updating it.
    if (computer->currentGameId != m_CurrentGameId) {
        m_CurrentGameId = computer->currentGameId;
        refreshVisibleApps({ RunningRole, GroupLabelRole });
    }
}

void AppModel::forceSyncCurrentGame()
{
    // Re-read currentGameId directly from the shared NvComputer state and
    // run the same dataChanged emit logic as handleComputerStateChanged
    // when it differs from our cache. This is a defensive self-heal for
    // cases where the polling thread updated NvComputer through a path
    // that didn't reach our slot (e.g. mDNS PendingAddTask fold or a
    // signal dropped due to receiver-side lifecycle timing).
    if (m_Computer == nullptr) {
        return;
    }

    int currentGameId;
    {
        QReadLocker lock(&m_Computer->lock);
        currentGameId = m_Computer->currentGameId;
    }

    if (currentGameId == m_CurrentGameId) {
        return;
    }

    m_CurrentGameId = currentGameId;
    refreshVisibleApps({ RunningRole, GroupLabelRole });
}

void AppModel::handleBoxArtLoaded(NvComputer* computer, NvApp app, QUrl /* image */)
{
    Q_ASSERT(computer == m_Computer);

    const int index = getAppIndexById(app.id);

    // Make sure we're not delivering a callback to an app that's already been removed
    if (index >= 0) {
        // Let our view know the box art data has changed for this app
        emit dataChanged(createIndex(index, 0),
                         createIndex(index, 0),
                         QVector<int>() << BoxArtRole);
    }
    else {
        if (findAppById(app.id) == nullptr) {
            qWarning() << "App removed before box art completed:" << app.name;
        }
    }
}
