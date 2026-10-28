import QtQuick 2.9
import QtQuick.Controls
import "../theme"

// Settings search results. Shown in place of the settings content while a
// query is active; styled as a settings card whose rows mirror SettingsRow
// (title, wrapped description, accent bar on the current row) with a
// "Category › Section" micro label so each hit says where it lives.
//
// Keys: Up/Down and Tab/Shift+Tab (gamepad D-pad in UI-nav mode) move, Up from
// the first result returns to the field, Enter/Space (gamepad A) opens, Esc
// clears the search and returns to the field.
FocusScope {
    id: view

    property Item controller: null
    property Item searchField: null

    readonly property int currentIndex: list.currentIndex
    readonly property bool listFocused: list.activeFocus
    readonly property int count: list.count
    readonly property bool emptyStateVisible: emptyState.visible

    visible: controller ? controller.active : false

    function focusFirst() {
        list.currentIndex = 0
        list.forceActiveFocus(Qt.TabFocusReason)
    }

    function itemAtIndex(index) {
        return list.itemAtIndex(index)
    }

    function step(delta) {
        var next = list.currentIndex + delta
        if (next < 0) {
            if (searchField) {
                searchField.focusField()
            }
            return
        }
        if (next < list.count) {
            list.currentIndex = next
            list.positionViewAtIndex(next, ListView.Contain)
        }
    }

    function openCurrent() {
        if (controller && list.currentIndex >= 0 && list.currentIndex < controller.results.length) {
            controller.open(controller.results[list.currentIndex])
        }
    }

    Panel {
        anchors {
            fill: parent
            rightMargin: Theme.shadowOffset
            bottomMargin: Theme.shadowOffset
        }
        fill: Theme.surfaceLayer
        accentBarColor: Theme.accent
        accentBarWidth: Theme.accentBar

        Item {
            anchors {
                fill: parent
                margins: Theme.spaceLg
            }

            Row {
                id: header
                width: parent.width
                spacing: Theme.spaceSm

                Text {
                    text: qsTr("Search results")
                    color: Theme.accent
                    font.family: Theme.fontSans
                    font.pointSize: Theme.fontCardTitle
                    font.weight: Font.ExtraBold
                    font.capitalization: Font.AllUppercase
                    font.letterSpacing: Theme.tracking(Theme.fontCardTitle, 0.08)
                }

                MicroLabel {
                    height: parent.height
                    text: list.count
                    visible: list.count > 0
                }
            }

            Text {
                id: emptyState
                anchors {
                    top: header.bottom
                    topMargin: Theme.spaceMd
                }
                width: parent.width
                visible: list.count === 0
                text: view.controller
                      ? qsTr("No settings match \"%1\".").arg(view.controller.query.trim())
                      : ""
                color: Theme.textSettingsSubtitle
                font.family: Theme.fontSans
                font.pointSize: Theme.fontSettingsSubtitle
                wrapMode: Text.Wrap
            }

            ListView {
                id: list

                anchors {
                    top: header.bottom
                    topMargin: Theme.spaceMd
                    left: parent.left
                    right: parent.right
                    bottom: parent.bottom
                }
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                keyNavigationEnabled: false
                model: view.controller ? view.controller.results : []
                activeFocusOnTab: false
                Accessible.role: Accessible.List
                Accessible.name: qsTr("Search results")

                ScrollBar.vertical: ScrollBar {
                    policy: list.contentHeight > list.height ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
                }

                Keys.onUpPressed: view.step(-1)
                Keys.onBacktabPressed: view.step(-1)
                Keys.onDownPressed: view.step(1)
                Keys.onTabPressed: view.step(1)
                Keys.onReturnPressed: view.openCurrent()
                Keys.onEnterPressed: view.openCurrent()
                // Gamepad A is Space in UI-nav mode
                Keys.onSpacePressed: view.openCurrent()
                Keys.onEscapePressed: function(event) {
                    event.accepted = true
                    if (view.controller) {
                        view.controller.clear()
                    }
                    if (view.searchField) {
                        view.searchField.focusField()
                    }
                }

                delegate: Item {
                    id: resultRow

                    readonly property bool current: ListView.isCurrentItem && list.activeFocus

                    width: list.width
                    height: rowContent.implicitHeight + Theme.spaceMd * 2
                    Accessible.role: Accessible.ListItem
                    Accessible.name: modelData.title
                    Accessible.description: modelData.description

                    Rectangle {
                        anchors.fill: parent
                        radius: 0
                        color: resultRow.current || rowMouse.containsMouse ? Theme.surface2 : "transparent"

                        Rectangle {
                            anchors {
                                left: parent.left
                                top: parent.top
                                bottom: parent.bottom
                            }
                            width: resultRow.current ? Theme.accentBar : 0
                            visible: width > 0
                            color: Theme.accent
                        }

                        Rectangle {
                            anchors {
                                left: parent.left
                                right: parent.right
                                bottom: parent.bottom
                            }
                            height: 1
                            color: Theme.line
                            visible: index < list.count - 1
                        }

                        Behavior on color {
                            ColorAnimation { duration: Theme.durFast }
                        }
                    }

                    Column {
                        id: rowContent
                        anchors {
                            left: parent.left
                            right: parent.right
                            verticalCenter: parent.verticalCenter
                            leftMargin: Theme.spaceMd
                            rightMargin: Theme.spaceMd
                        }
                        spacing: Theme.spaceXs

                        MicroLabel {
                            width: parent.width
                            color: Theme.accent
                            text: modelData.section !== "" && modelData.section !== modelData.title
                                  && modelData.section !== modelData.categoryTitle
                                  ? modelData.categoryTitle + "  ›  " + modelData.section
                                  : modelData.categoryTitle
                        }

                        Text {
                            width: parent.width
                            text: modelData.title
                            color: Theme.text
                            font.family: Theme.fontSans
                            font.pointSize: Theme.fontRowTitle
                            font.weight: Font.DemiBold
                            wrapMode: Text.Wrap
                        }

                        Text {
                            width: parent.width
                            text: modelData.description
                            visible: text !== ""
                            color: Theme.textSettingsSubtitle
                            font.family: Theme.fontSans
                            font.pointSize: Theme.fontSettingsSubtitle
                            font.weight: Font.Medium
                            wrapMode: Text.Wrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                        }
                    }

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            list.currentIndex = index
                            view.openCurrent()
                        }
                    }
                }
            }
        }
    }
}
