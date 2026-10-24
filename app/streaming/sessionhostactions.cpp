// Session host actions: the overlay menu's Send Keys and Host Commands
// levels and their shortcut actions. Kept apart from session.cpp so the
// feature can move into the tabbed menu as one piece.
#include "session.h"

#include "backend/nvcomputer.h"
#include "hostcommandrunner.h"
#include "input/hostkeys.h"

#include "SDL_compat.h"

#include <QReadLocker>

void Session::registerHostActionShortcuts()
{
    ShortcutDispatcher& dispatcher = m_InputHandler->getShortcutDispatcher();
    dispatcher.setHandler(QLatin1String(ShortcutActions::SendCtrlAltDel), [this] {
        const HostKeys::SpecialKey* key =
            HostKeys::findSpecialKey(QLatin1String(HostKeys::CtrlAltDel));
        if (key != nullptr) {
            sendHostKeys(key->binding);
        }
    });
    dispatcher.setHandler(QLatin1String(ShortcutActions::OpenHostCommands),
                          [this] { openHostCommandsMenu(); });
}

void Session::updateHostActionsMenuState()
{
    if (m_MenuPanel == nullptr) {
        return;
    }

    std::vector<OverlayMenuPanel::SendKeyItem> specialKeys;
    for (const HostKeys::SpecialKey& key : HostKeys::specialKeys()) {
        specialKeys.push_back({ key.translatedName(), HostKeys::displayText(key.binding),
                                HostKeys::specialKeyPayload(key) });
    }

    // Macros can only change in the settings page, but reading them here is
    // cheap and keeps the menu right if that ever changes.
    std::vector<OverlayMenuPanel::SendKeyItem> macros;
    for (const HostKeys::Macro& macro : HostKeys::loadMacrosFromSettings()) {
        macros.push_back(
            { macro.name, HostKeys::displayText(macro.binding), HostKeys::macroPayload(macro) });
    }
    m_MenuPanel->updateSendKeys(std::move(specialKeys), std::move(macros));

    // The computer's app list can be newer than the copy taken at launch.
    // Foundation Sunshine runs the command of the app that is running, which
    // is the one this session launched or resumed.
    NvApp app = m_App;
    if (m_Computer != nullptr) {
        QReadLocker locker(&m_Computer->lock);
        for (const NvApp& candidate : m_Computer->appList) {
            if (candidate.id == m_App.id) {
                app = candidate;
                break;
            }
        }
    }

    m_HostCommands = app.superCmds;
    std::vector<OverlayMenuPanel::HostCommandItem> commands;
    for (const SuperCmd& command : app.superCmds) {
        commands.push_back({ command.id, command.name });
    }
    m_MenuPanel->updateHostCommands(app.superCmdsSupported, std::move(commands));
}

void Session::dispatchQtMenuPayloadAction(OverlayMenuPanel::MenuAction action,
                                          const QString& payload)
{
    switch (action) {
    case OverlayMenuPanel::MenuAction::SendHostKeys:
        sendHostKeys(HostKeys::bindingForPayload(payload));
        break;
    case OverlayMenuPanel::MenuAction::RunHostCommand:
        runHostCommand(payload);
        break;
    default:
        break;
    }
}

void Session::sendHostKeys(const ShortcutBinding& binding)
{
    if (m_InputHandler == nullptr) {
        return;
    }

    // Keys still held on the host, like the modifiers of the shortcut that
    // triggered this, would join the combo. Release them there; the local
    // state stays so their real key-ups still match.
    m_InputHandler->raiseAllKeys(false);

    if (!HostKeys::send(binding)) {
        showStreamingToast(tr("Couldn't send the keys to the host"));
    }
}

void Session::runHostCommand(const QString& commandId)
{
    if (m_HostCommandRunner == nullptr || m_Computer == nullptr || commandId.isEmpty()) {
        return;
    }

    QString name = commandId;
    for (const SuperCmd& command : std::as_const(m_HostCommands)) {
        if (command.id == commandId) {
            name = command.name;
            break;
        }
    }

    const bool started = m_HostCommandRunner->run(
        m_Computer, commandId, [this, name](bool accepted, const QString& error) {
            if (accepted) {
                showStreamingToast(tr("Host command sent: %1").arg(name));
            } else if (error.isEmpty()) {
                showStreamingToast(tr("The host rejected the command: %1").arg(name), 3000);
            } else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Host command '%s' failed: %s",
                            qPrintable(name), qPrintable(error));
                showStreamingToast(tr("Couldn't run host command %1: %2").arg(name, error), 4000);
            }
        });
    if (!started) {
        showStreamingToast(tr("Another host command is still running"));
    }
}

void Session::openHostCommandsMenu()
{
    if (m_MenuPanel == nullptr) {
        return;
    }

    if (!m_MenuPanel->isMenuVisible()) {
        showQtOverlayMenu();
    }

    // Not shown when the user turned the overlay menu off
    if (m_MenuPanel->isMenuVisible() && !m_MenuPanel->showHostCommandsLevel()) {
        showStreamingToast(tr("Host commands need a Foundation Sunshine host"), 3000);
    }
}
