// Session file transfers: files dropped onto the stream window are sent to
// the host through the clipboard helper, plus the matching overlay toasts and
// cancel shortcut. Kept apart from session.cpp like the other session features.
#include "session.h"

#include "clipboardhelperclient.h"
#include "input/shortcuts.h"

#include "SDL_compat.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>

namespace {

QString shortcutText(const char* actionId)
{
    const ShortcutProfile profile = ShortcutProfile::loadFromSettings();
    for (const ShortcutBinding& binding : profile.bindings(QLatin1String(actionId))) {
        if (!binding.isNull()) {
            return binding.toDisplayString();
        }
    }
    return QString();
}

QString formatBytes(qint64 bytes)
{
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

} // namespace

void Session::registerFileTransferShortcuts()
{
    m_InputHandler->getShortcutDispatcher().setHandler(
        QLatin1String(ShortcutActions::CancelFileTransfer), [this] { cancelFileTransfers(); });
}

void Session::cancelFileTransfers()
{
    if ((!m_FileUploadActive && !m_FileUploadPending) || m_ClipboardHelper == nullptr) {
        showStreamingToast(tr("No file transfer to cancel."));
        return;
    }
    m_ClipboardHelper->cancelTransfers();
}

bool Session::handleFileDropEvent(const SDL_Event& event)
{
    switch (event.type) {
    case SDL_DROPBEGIN:
        m_DroppedPaths.clear();
        m_DropInProgress = true;
        return true;
    case SDL_DROPFILE:
        if (event.drop.file != nullptr) {
            m_DroppedPaths.append(QString::fromUtf8(event.drop.file));
            SDL_free(event.drop.file);
        }
        // Platforms without begin/complete events deliver one path per drop.
        if (!m_DropInProgress) {
            finishFileDrop();
        }
        return true;
    case SDL_DROPTEXT:
        if (event.drop.file != nullptr) {
            SDL_free(event.drop.file);
        }
        return true;
    case SDL_DROPCOMPLETE:
        m_DropInProgress = false;
        finishFileDrop();
        return true;
    default:
        return false;
    }
}

void Session::finishFileDrop()
{
    QStringList paths;
    paths.swap(m_DroppedPaths);
    if (paths.isEmpty()) {
        return;
    }

    if (!m_Preferences->clipboardSyncEnabled || !m_Preferences->clipboardDropFiles) {
        showStreamingToast(tr("To send files to the host, turn on \"Drop files onto the stream\" "
                              "in Moonlight's Clipboard settings."),
                           5000);
        return;
    }
    if (m_ClipboardHelper == nullptr) {
        showStreamingToast(tr("Sending files isn't available in this session."), 4000);
        return;
    }
    if (m_FileUploadActive || m_FileUploadPending) {
        showStreamingToast(tr("Still sending the previous files. Wait for them to finish."), 4000);
        return;
    }
    if (paths.size() > ClipboardIpc::MAX_UPLOAD_PATHS) {
        showStreamingToast(tr("Too many files (limit %1).").arg(ClipboardIpc::MAX_UPLOAD_PATHS),
                           4000);
        return;
    }

    ClipboardIpc::UploadFiles upload;
    upload.paths = paths;
    upload.intent = QStringLiteral("drop");

    // Where the files were dropped, as a fraction of the stream window. The
    // host uses it to drop the files at the matching cursor position.
    int mouseX = 0, mouseY = 0, winX = 0, winY = 0, winW = 0, winH = 0;
    SDL_GetGlobalMouseState(&mouseX, &mouseY);
    SDL_GetWindowPosition(m_Window, &winX, &winY);
    SDL_GetWindowSize(m_Window, &winW, &winH);
    if (winW > 0 && winH > 0) {
        upload.x = std::clamp(double(mouseX - winX) / winW, 0.0, 1.0);
        upload.y = std::clamp(double(mouseY - winY) / winH, 0.0, 1.0);
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Sending %d dropped path(s) to the host",
                static_cast<int>(paths.size()));
    if (m_ClipboardHelper->uploadFiles(upload)) {
        m_FileUploadPending = true;
    } else {
        showStreamingToast(tr("Sending files isn't ready yet. Try again in a moment."), 4000);
    }
}

void Session::showCopiedFilesToast(const ClipboardIpc::TransferEvent& event)
{
    if (event.state == ClipboardIpc::TransferState::Finished) {
        showStreamingToast(
            tr("Copied %n file(s) from the host. Paste them with Ctrl+V.", "", event.count), 4000);
    } else if (event.state == ClipboardIpc::TransferState::Failed) {
        showStreamingToast(tr("Couldn't copy files from the host: %1").arg(event.error), 5000);
    }
}

void Session::onClipboardHelperLost()
{
    if (m_FileUploadActive || m_FileUploadPending) {
        m_FileUploadActive = false;
        m_FileUploadPending = false;
        m_FileUploadId.clear();
        showStreamingToast(tr("Sending files stopped because the clipboard helper restarted."),
                           5000);
    }
}

void Session::showFileUploadToast(const ClipboardIpc::TransferEvent& event)
{
    switch (event.state) {
    case ClipboardIpc::TransferState::Started: {
        m_FileUploadPending = false;
        m_FileUploadActive = true;
        m_FileUploadId = event.id;
        m_FileUploadToastTicks = SDL_GetTicks();
        const QString cancel = shortcutText(ShortcutActions::CancelFileTransfer);
        QString message = tr("Sending %n file(s) (%1) to the host…", "", event.count)
                              .arg(formatBytes(event.size));
        if (!cancel.isEmpty()) {
            message += QLatin1Char(' ') + tr("%1 cancels.").arg(cancel);
        }
        showStreamingToast(message, 3000);
        break;
    }
    case ClipboardIpc::TransferState::Progress:
        if (event.id == m_FileUploadId && event.size > 0 &&
            SDL_TICKS_PASSED(SDL_GetTicks(), m_FileUploadToastTicks + 1000)) {
            m_FileUploadToastTicks = SDL_GetTicks();
            showStreamingToast(
                tr("Sending files to the host… %1%").arg(int(event.bytes * 100 / event.size)),
                1500);
        }
        break;
    case ClipboardIpc::TransferState::Finished:
        if (event.id != m_FileUploadId) {
            break;
        }
        m_FileUploadActive = false;
        m_FileUploadId.clear();
        showStreamingToast(
            tr("Sent %n file(s). The host put them on its clipboard: paste with Ctrl+V.", "",
               event.count),
            5000);
        break;
    case ClipboardIpc::TransferState::Failed:
        // A refusal for some other request (e.g. a drop while the helper was
        // still planning this one) must not end the running upload.
        if (m_FileUploadActive && event.id != m_FileUploadId) {
            break;
        }
        m_FileUploadActive = false;
        m_FileUploadPending = false;
        m_FileUploadId.clear();
        // An empty error means a newer copy replaced this upload.
        if (!event.error.isEmpty()) {
            showStreamingToast(tr("Couldn't send files: %1").arg(event.error), 6000);
        }
        break;
    }
}
