import QtQuick
import QtTest

TestCase {
    id: testCase
    name: "VrrModeInteraction"
    width: 760
    height: 640
    visible: true
    when: windowShown

    property var preferences
    property int writes: 0

    function state(enabled, implementation, smooth) {
        var result = {
            enableVrr: enabled,
            vrrImplementation: implementation,
            VRR_IMPL_FORK: 0,
            VRR_IMPL_NONARY: 1
        }
        var storedSmooth = smooth
        Object.defineProperty(result, "vrrSmooth", {
            get: function() { return storedSmooth },
            set: function(value) { storedSmooth = value; ++testCase.writes }
        })
        return result
    }

    function productionRow() {
        var request = new XMLHttpRequest()
        request.open("GET", Qt.resolvedUrl("../../app/gui/settings/DisplaySettingsPage.qml"), false)
        request.send()
        var source = request.responseText
        var title = source.indexOf('title: qsTr("Smooth VRR")')
        verify(title >= 0, "Smooth VRR production row must exist")
        var start = source.lastIndexOf("ToggleRow {", title)
        var end = source.indexOf("\n        }", title)
        verify(start >= 0 && end > start)
        // Keep the production row/bindings/handler; substitute only the preference backend.
        var row = source.substring(start, end + "\n        }".length)
                        .replace(/StreamingPreferences\./g, "testCase.preferences.")
        return Qt.createQmlObject('import QtQuick\nimport "' +
                                  Qt.resolvedUrl("../../app/gui/settings").toString() +
                                  '"\n' + row, testCase, "production-smooth-row.qml")
    }

    function test_saved_choice_survives_disabled_modes() {
        writes = 0
        preferences = state(true, 0, true)
        var row = productionRow()
        verify(row)
        row.width = 700
        tryCompare(row, "controlEnabled", true)
        compare(row.checked, true)
        compare(writes, 0)
        preferences = state(true, 1, true)
        tryCompare(row, "controlEnabled", false)
        compare(row.checked, false)
        compare(preferences.vrrSmooth, true)
        verify(row.description.indexOf("Present on arrival") >= 0)
        verify(row.settingsSearchKeywords.indexOf("nonary") >= 0)
        var control = row.controlContent[0]
        mouseClick(control, control.width / 2, control.height / 2)
        compare(writes, 0, "a disabled Smooth control must not overwrite the saved preference")
        preferences = state(false, 0, true)
        tryCompare(row, "controlEnabled", false)
        compare(row.checked, false)
        compare(preferences.vrrSmooth, true)
        preferences = state(true, 0, true)
        tryCompare(row, "controlEnabled", true)
        compare(row.checked, true)
        compare(writes, 0)
        mouseClick(control, control.width / 2, control.height / 2)
        compare(writes, 1)
        compare(preferences.vrrSmooth, false)
        row.destroy()
    }
}
