import QtQuick
import QtTest

TestCase {
    id: testCase
    name: "CodecBitrateRecommendation"
    visible: true
    when: windowShown
    width: 760
    height: 400
    property var preferences
    property var pyro: ({recommendedBitrateKbps: 400000})

    function productionButton() {
        var request = new XMLHttpRequest()
        request.open("GET", Qt.resolvedUrl("../../app/gui/settings/BasicSettingsPage.qml"), false)
        request.send()
        var source = request.responseText
        var title = source.indexOf('text: qsTr("Use recommended bitrate')
        verify(title >= 0)
        var start = source.lastIndexOf("HardButton {", title)
        var end = source.indexOf("\n            }", title)
        var button = source.substring(start, end + "\n            }".length)
            .replace(/StreamingPreferences\./g, "testCase.preferences.")
            .replace(/PyrowaveStatus\./g, "testCase.pyro.")
        return Qt.createQmlObject('import QtQuick\nimport "' +
            Qt.resolvedUrl("../../app/gui").toString() + '"\nimport "' +
            Qt.resolvedUrl("../../app/gui/theme").toString() + '"\n' + button,
            testCase, "production-recommendation.qml")
    }

    function test_codec_changes_guidance_not_manual_bitrate() {
        preferences = {videoCodecConfig: 0, VCC_FORCE_PYROWAVE: 5,
            width: 3840, height: 2160, fps: 120, enableYUV444: false,
            autoAdjustBitrate: false, bitrateKbps: 123000,
            getDefaultBitrate: function() { return 113000 }}
        var button = productionButton()
        compare(button.defaultBitrate, 113000)
        preferences = Object.assign({}, preferences, {videoCodecConfig: 5})
        tryCompare(button, "defaultBitrate", 400000)
        compare(preferences.bitrateKbps, 123000, "codec choice must not overwrite manual bitrate")
        verify(button.text.indexOf("experimental") >= 0)
        mouseClick(button, button.width / 2, button.height / 2)
        compare(preferences.bitrateKbps, 400000)
        compare(preferences.autoAdjustBitrate, false, "Pyro guidance is an explicit fixed budget")
        button.destroy()
    }
}
