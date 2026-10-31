import QtQuick
import QtQuick.Controls
import AutoUpdateChecker 1.0

NavigableMessageDialog {
    property string downloadUrl
    property string releaseVersion

    title: qsTr("Download and install update")
    text: qsTr("Download version %1 and install it now? Moonlight will restart. Finish any stream before continuing.").arg(releaseVersion)
    standardButtons: Dialog.Ok | Dialog.Cancel
    onOpened: {
        footer.standardButton(Dialog.Ok).text = qsTr("Download and install")
        footer.standardButton(Dialog.Cancel).forceActiveFocus(Qt.TabFocusReason)
    }
    onAccepted: AutoUpdateChecker.installUpdate(downloadUrl)
}
