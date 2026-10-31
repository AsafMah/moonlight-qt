import QtQuick
import QtQuick.Controls
import QtTest
import AutoUpdateChecker 1.0
import "../../app/gui"

TestCase {
    name: "UpdateConsent"
    width: 720
    height: 600
    visible: true
    when: windowShown
    Item { id: stackView }

    Component { id: confirmation; UpdateConfirmationDialog {} }

    function test_consent_data() {
        return [{tag: "decline", action: "cancel"}, {tag: "escape", action: "escape"},
                {tag: "install", action: "install"}]
    }

    function test_consent(data) {
        AutoUpdateChecker.downloads = []
        var url = "https://github.com/AsafMah/moonlight-qt/releases/download/v6.5.3.182/update.zip"
        var dialog = createTemporaryObject(confirmation, this,
                                           {downloadUrl: url, releaseVersion: "6.5.3.182"})
        dialog.open()
        tryCompare(dialog, "opened", true)
        var cancel = dialog.footer.standardButton(Dialog.Cancel)
        var install = dialog.footer.standardButton(Dialog.Ok)
        verify(cancel.activeFocus, "Decline must be the initial keyboard/gamepad target")
        compare(install.text, "Download and install")
        compare(AutoUpdateChecker.downloads.length, 0, "Opening must not download")
        if (data.action === "escape") {
            keyClick(Qt.Key_Escape)
        } else {
            mouseClick(data.action === "install" ? install : cancel)
        }
        tryCompare(dialog, "visible", false)
        compare(AutoUpdateChecker.downloads.length, data.action === "install" ? 1 : 0)
        if (data.action === "install") {
            compare(AutoUpdateChecker.downloads[0], url)
        }
    }
}
