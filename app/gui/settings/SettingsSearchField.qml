import QtQuick 2.9
import QtQuick.Controls
import "../theme"

// Settings search box. Typing filters live; Down/Tab (gamepad D-pad down in
// UI-nav mode) moves into the results, Enter opens the top result, Esc clears
// the query and — only when already empty — falls through to the page's own
// Esc handling. Gamepad users get the on-screen keyboard with X through
// HardTextField.
HardTextField {
    id: field

    property Item controller: null
    // SettingsSearchResults
    property Item resultsView: null

    function focusField() {
        forceActiveFocus(Qt.TabFocusReason)
        cursorPosition = text.length
    }

    placeholderText: qsTr("Search settings")
    Accessible.name: qsTr("Search settings")
    // Free text, not data: the proportional UI face reads better than mono
    font.family: Theme.fontSans
    leftPadding: Theme.spaceSm + searchIcon.width + Theme.spaceSm
    rightPadding: clearButton.visible ? clearButton.width + Theme.spaceXs : Theme.spaceSm

    onTextChanged: {
        if (controller && controller.query !== text) {
            controller.query = text
        }
    }

    Connections {
        target: field.controller
        function onQueryChanged() {
            if (field.text !== field.controller.query) {
                field.text = field.controller.query
            }
        }
    }

    function enterResults(event) {
        if (!controller || !controller.active) {
            // Not searching: keep normal focus-chain behavior
            event.accepted = false
            return
        }
        if (resultsView && controller.results.length > 0) {
            resultsView.focusFirst()
        }
        event.accepted = true
    }

    Keys.onDownPressed: function(event) { enterResults(event) }
    Keys.onTabPressed: function(event) { enterResults(event) }

    function openTop(event) {
        if (controller && controller.results.length > 0) {
            controller.open(controller.results[0])
            event.accepted = true
        }
        else {
            event.accepted = false
        }
    }

    Keys.onReturnPressed: function(event) { openTop(event) }
    Keys.onEnterPressed: function(event) { openTop(event) }

    Keys.onEscapePressed: function(event) {
        if (controller && controller.query !== "") {
            event.accepted = true
            controller.clear()
        }
        else {
            event.accepted = false
        }
    }

    Image {
        id: searchIcon
        anchors {
            left: parent.left
            leftMargin: Theme.spaceSm
            verticalCenter: parent.verticalCenter
        }
        source: "qrc:/res/fluent/search.svg"
        sourceSize.width: 16
        sourceSize.height: 16
        width: 16
        height: 16
        opacity: field.activeFocus ? 0.9 : 0.55
    }

    AbstractButton {
        id: clearButton

        anchors {
            right: parent.right
            rightMargin: Theme.spaceXs
            verticalCenter: parent.verticalCenter
        }
        width: 24
        height: 24
        visible: field.text !== ""
        // Esc already clears from the keyboard; keep this out of the Tab chain
        focusPolicy: Qt.NoFocus
        Accessible.name: qsTr("Clear search")
        ToolTip.text: qsTr("Clear search")
        ToolTip.delay: 700
        ToolTip.visible: hovered
        hoverEnabled: true

        background: Rectangle {
            color: clearButton.hovered ? Theme.surface2 : "transparent"
        }

        contentItem: Image {
            source: "qrc:/res/fluent/menu-close.svg"
            sourceSize.width: 14
            sourceSize.height: 14
            fillMode: Image.Pad
            opacity: clearButton.hovered ? 1.0 : 0.6
        }

        onClicked: {
            if (field.controller) {
                field.controller.clear()
            }
            field.focusField()
        }
    }
}
