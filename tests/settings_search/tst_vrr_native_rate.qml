import QtQuick
import QtTest

TestCase {
    id: testCase
    name: "VrrNativeRate"
    width: 760
    height: 640
    visible: true
    when: windowShown

    property var preferences
    property int writes: 0

    function state(enabled, fps, nativeRate) {
        var result = {
            enableVrr: enabled,
            fps: fps,
            // floor(r - r^2/3600), as VrrPolicy::vrrStreamFps computes it
            vrrBelowRefreshFps: function(refresh) { return Math.floor(refresh - refresh * refresh / 3600) }
        }
        var storedNative = nativeRate
        Object.defineProperty(result, "vrrNativeRate", {
            get: function() { return storedNative },
            set: function(value) { storedNative = value; ++testCase.writes }
        })
        return result
    }

    function productionRow() {
        var request = new XMLHttpRequest()
        request.open("GET", Qt.resolvedUrl("../../app/gui/settings/DisplaySettingsPage.qml"), false)
        request.send()
        var source = request.responseText
        var title = source.indexOf('title: qsTr("Stream at the display\'s maximum refresh rate")')
        verify(title >= 0, "VRR native-rate production row must exist")
        var start = source.lastIndexOf("ToggleRow {", title)
        var end = source.indexOf("\n        }", title)
        verify(start >= 0 && end > start)
        var row = source.substring(start, end + "\n        }".length)
                        .replace(/StreamingPreferences\./g, "testCase.preferences.")
        return Qt.createQmlObject('import QtQuick\nimport "' +
                                  Qt.resolvedUrl("../../app/gui/settings").toString() +
                                  '"\n' + row, testCase, "production-vrr-native-rate-row.qml")
    }

    function test_description_shows_requested_rate() {
        writes = 0
        preferences = state(true, 180, false)
        var row = productionRow()
        verify(row)
        tryCompare(row, "controlEnabled", true)
        compare(row.checked, false)
        verify(row.description.indexOf("171 FPS") >= 0, row.description)
        verify(row.description.indexOf("180 Hz") >= 0, row.description)
        preferences = state(true, 144, false)
        verify(row.description.indexOf("138 FPS") >= 0, row.description)
        verify(row.description.indexOf("144 Hz") >= 0, row.description)
        verify(row.searchKeywords.indexOf("vrr") >= 0)
        verify(row.searchKeywords.indexOf("171") >= 0)
        compare(writes, 0)
        row.destroy()
    }

    function test_saved_choice_survives_vrr_off() {
        writes = 0
        preferences = state(true, 180, true)
        var row = productionRow()
        verify(row)
        row.width = 700
        tryCompare(row, "controlEnabled", true)
        compare(row.checked, true)
        preferences = state(false, 180, true)
        tryCompare(row, "controlEnabled", false)
        compare(row.checked, false)
        verify(row.description.indexOf("variable refresh rate") >= 0, row.description)
        var control = row.controlContent[0]
        mouseClick(control, control.width / 2, control.height / 2)
        compare(writes, 0, "a disabled control must not overwrite the saved preference")
        compare(preferences.vrrNativeRate, true)
        preferences = state(true, 180, true)
        tryCompare(row, "controlEnabled", true)
        compare(row.checked, true)
        mouseClick(control, control.width / 2, control.height / 2)
        compare(writes, 1)
        compare(preferences.vrrNativeRate, false)
        row.destroy()
    }
}
