import QtQuick
import QtQuick.Controls
import QtTest

TestCase {
    id: testCase
    name: "AutomaticUpdateStatus"
    width: 720
    height: 600
    visible: true
    when: windowShown
    property var updater: fakeUpdater
    QtObject {
        id: fakeUpdater
        property string automaticUpdateStatus: "Restarting to install the update in 15 seconds."
        property bool automaticUpdateInProgress: true
        property int cancellations: 0
        function cancelUpdate() {
            ++cancellations
            automaticUpdateInProgress = false
            automaticUpdateStatus = "Automatic update cancelled."
        }
    }

    function test_nonblocking_status_cancel() {
        var request = new XMLHttpRequest()
        request.open("GET", Qt.resolvedUrl("../../app/gui/main.qml"), false)
        request.send()
        var source = request.responseText.replace(/\r\n/g, "\n")
        var id = source.indexOf("id: automaticUpdateContent")
        verify(id >= 0)
        var start = source.lastIndexOf("Rectangle {", id)
        var end = source.indexOf("\n    }\n", id)
        verify(start >= 0 && end > start)
        var panel = source.substring(start, end + "\n    }".length)
            .replace(/AutoUpdateChecker\./g, "testCase.updater.")
            .replace("text: qsTr(\"Cancel automatic update\")",
                     'objectName: "cancelUpdate"; text: qsTr("Cancel automatic update")')
        var status = Qt.createQmlObject('import QtQuick\nimport QtQuick.Controls\nimport "' +
            Qt.resolvedUrl("../../app/gui/theme").toString() + '"\n' + panel,
            testCase, "production-auto-update-status.qml")
        verify(!(status instanceof Popup), "Automatic status must not be a modal dialog")
        verify(status.visible)
        verify(status.width <= testCase.width)
        var cancel = findChild(status, "cancelUpdate")
        verify(cancel.visible)
        mouseClick(cancel)
        compare(updater.cancellations, 1)
        compare(cancel.visible, false)
        status.destroy()
    }
}
