import QtQuick
import QtTest
import "../../app/gui/settings"

TestCase {
    name: "CaptureModeChoice"
    width: 760
    height: 640
    visible: true
    when: windowShown

    FontLoader { id: manrope; source: "../../app/res/fonts/Manrope-Regular.ttf" }

    Component {
        id: captureChoice
        ChoiceRow {
            property int storedMode: 3
            title: "Capture mode"
            description: "Borderless windows must cover their monitor."
            selectedValue: storedMode
            onValueActivated: function(value) { storedMode = value }
            model: ListModel {
                ListElement { text: "in fullscreen"; val: 1 }
                ListElement { text: "in fullscreen or borderless window"; val: 3 }
                ListElement { text: "always"; val: 2 }
            }
        }
    }

    function test_choice_data() {
        return [{ tag: "narrow", rowWidth: 360 }, { tag: "wide", rowWidth: 740 }]
    }

    function test_choice(data) {
        var row = createTemporaryObject(captureChoice, this, { width: data.rowWidth })
        verify(row)
        compare(manrope.status, FontLoader.Ready)
        var control = row.controlContent[0]
        // Match main.cpp's Windows application font rather than the test runner's font.
        control.font.family = manrope.name
        control.font.pointSize = 9
        control.popup.font = control.font
        waitForRendering(row)
        compare(control.currentIndex, 1)
        var writes = 0
        row.valueActivated.connect(function() { ++writes })
        row.storedMode = 2
        compare(control.currentIndex, 2)
        row.storedMode = 1
        compare(control.currentIndex, 0)
        compare(writes, 0)
        control.recalculateWidth()
        verify(control.width > 0)
        verify(control.height > 0)
        verify(control.desiredWidth > 0)
        verify(control.desiredWidth <= control.width + 1,
               "new capture label is clipped: wanted " + control.desiredWidth +
               ", actual " + control.width + ", row " + row.width)
        verify(control.mapToItem(row, control.width, 0).x <= row.width + 1)
        mouseClick(control, control.width / 2, control.height / 2)
        tryCompare(control.popup, "opened", true)
        var list = control.popup.contentItem
        tryVerify(function() { return list.itemAtIndex(1) !== null })
        var option = list.itemAtIndex(1)
        compare(option.contentItem.text, "in fullscreen or borderless window")
        verify(!option.contentItem.truncated, "borderless option is truncated in the popup")
        mouseClick(option, option.width / 2, option.height / 2)
        compare(row.storedMode, 3)
        compare(writes, 1)
        compare(control.currentIndex, 1)
    }
}
