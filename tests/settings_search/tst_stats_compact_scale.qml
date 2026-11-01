import QtQuick
import QtTest

TestCase {
    id: testCase
    name: "StatsCompactScaleControl"
    visible: true
    when: windowShown
    width: 760
    height: 400
    property string title: "Overlay scale"
    property var settings: ({minScalePercent: 40, maxScalePercent: 200, scalePercent: 80})

    function readSource(url) {
        var request = new XMLHttpRequest()
        request.open("GET", Qt.resolvedUrl(url), false)
        request.send()
        return request.responseText.replace(/\r\n/g, "\n")
    }

    function test_actual_scale_slider_reaches_compact_floor() {
        verify(readSource("../../app/streaming/video/overlaystatspreset.h")
               .match(/MinScalePercent\s*=\s*40\s*;/),
               "Control's lower bound must match the production C++ settings floor")
        var source = readSource("../../app/gui/settings/DisplaySettingsPage.qml")
        var title = source.indexOf("id: statsOverlayScaleSlider")
        verify(title >= 0)
        var start = source.lastIndexOf("Row {", title)
        var end = source.indexOf("\n            }\n        }", title)
        verify(start >= 0 && end > start, "Production scale control must be extracted intact")
        var row = source.substring(start, end + "\n            }".length)
            .replace(/StatsOverlaySettings\./g, "testCase.settings.")
            .replace(/statsOverlayScaleRow\./g, "testCase.")
            .replace("id: statsOverlayScaleSlider",
                     'id: statsOverlayScaleSlider; objectName: "scaleControl"')
        var control = Qt.createQmlObject('import QtQuick\nimport QtQuick.Controls\nimport "' +
            Qt.resolvedUrl("../../app/gui/theme").toString() + '"\n' + row,
            testCase, "production-stats-scale.qml")
        var slider = findChild(control, "scaleControl")
        compare(slider.from, 40)
        mouseClick(slider, slider.leftPadding + 1, slider.height / 2)
        compare(settings.scalePercent, 40, "Actual slider onMoved must save 40%")
        control.destroy()
    }
}
