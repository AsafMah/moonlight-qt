import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import AppModel 1.0

import "theme"

Item {
    id: controls

    property var appView
    readonly property var appModel: appView ? appView.appModel : null
    property string searchValue: ""

    height: 48
    visible: appModel !== null

    onAppModelChanged: {
        searchValue = appModel ? appModel.searchText : ""
    }

    onSearchValueChanged: {
        if (appModel && appModel.searchText !== searchValue) {
            appModel.searchText = searchValue
        }
    }

    function focusSearch() {
        searchField.forceActiveFocus(Qt.TabFocusReason)
    }

    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(Theme.ink.r, Theme.ink.g, Theme.ink.b, 0.72)

        Rectangle {
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            height: 1
            color: Theme.line
        }
    }

    RowLayout {
        anchors {
            fill: parent
            leftMargin: Theme.spaceLg
            rightMargin: Theme.spaceLg
        }
        spacing: Theme.spaceSm

        HardTextField {
            id: searchField

            Layout.fillWidth: true
            Layout.minimumWidth: 100
            placeholderText: qsTr("Search apps")
            text: controls.searchValue
            Accessible.name: qsTr("Search apps")

            onTextEdited: controls.searchValue = text

            Keys.onDownPressed: typeCombo.forceActiveFocus(Qt.TabFocusReason)
            Keys.onEscapePressed: function(event) {
                if (controls.searchValue.length > 0) {
                    controls.searchValue = ""
                }
                else {
                    event.accepted = false
                }
            }
        }

        AbstractButton {
            id: clearSearchButton

            Layout.preferredWidth: visible ? 32 : 0
            Layout.minimumWidth: visible ? 32 : 0
            Layout.maximumWidth: visible ? 32 : 0
            Layout.fillHeight: true
            visible: controls.searchValue.length > 0
            Accessible.name: qsTr("Clear search")
            ToolTip.text: qsTr("Clear search")
            ToolTip.delay: 700
            ToolTip.visible: hovered

            background: Rectangle {
                color: clearSearchButton.down ? Theme.surface : Theme.surface2
                border.width: clearSearchButton.visualFocus ? 2 : 1
                border.color: clearSearchButton.visualFocus ? Theme.accent
                                                              : Theme.lineStrong
            }

            contentItem: Text {
                text: "x"
                color: Theme.text
                font.family: Theme.fontSans
                font.pointSize: 18
                font.weight: Font.Bold
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }

            onClicked: {
                controls.searchValue = ""
                searchField.forceActiveFocus(Qt.MouseFocusReason)
            }
        }

        AutoResizingComboBox {
            id: typeCombo
            Layout.minimumWidth: 72
            Layout.maximumWidth: 140
            maximumWidth: 140
            model: [qsTr("Type: All"), qsTr("Type: Games"), qsTr("Type: Apps")]
            currentIndex: controls.appModel ? controls.appModel.typeFilter : AppModel.TypeAll
            Accessible.name: qsTr("Filter by type")
            ToolTip.text: qsTr("Filter by type")
            ToolTip.delay: 700
            ToolTip.visible: hovered
            navUpItem: searchField
            navDownItem: statusCombo
            onActivated: if (controls.appModel) controls.appModel.typeFilter = currentIndex
        }

        AutoResizingComboBox {
            id: statusCombo
            Layout.minimumWidth: 76
            Layout.maximumWidth: 160
            maximumWidth: 160
            model: [qsTr("State: All"), qsTr("State: Running"), qsTr("State: Idle")]
            currentIndex: controls.appModel ? controls.appModel.runningFilter : AppModel.RunningAll
            Accessible.name: qsTr("Filter by running status")
            ToolTip.text: qsTr("Filter by running status")
            ToolTip.delay: 700
            ToolTip.visible: hovered
            navUpItem: typeCombo
            navDownItem: controls.appView && controls.appView.showHiddenGames
                          ? hiddenCombo : sortCombo
            onActivated: if (controls.appModel) controls.appModel.runningFilter = currentIndex
        }

        AutoResizingComboBox {
            id: hiddenCombo
            visible: controls.appView && controls.appView.showHiddenGames
            Layout.minimumWidth: visible ? 76 : 0
            Layout.maximumWidth: visible ? 150 : 0
            maximumWidth: 150
            model: [qsTr("Show: All"), qsTr("Show: Visible"), qsTr("Show: Hidden")]
            currentIndex: controls.appModel ? controls.appModel.hiddenFilter : AppModel.HiddenAll
            Accessible.name: qsTr("Filter by visibility")
            ToolTip.text: qsTr("Filter by visibility")
            ToolTip.delay: 700
            ToolTip.visible: hovered
            navUpItem: statusCombo
            navDownItem: sortCombo
            onActivated: if (controls.appModel) controls.appModel.hiddenFilter = currentIndex
        }

        AutoResizingComboBox {
            id: sortCombo
            Layout.minimumWidth: 82
            Layout.maximumWidth: 160
            maximumWidth: 160
            model: [qsTr("Sort: Host"), qsTr("Sort: A-Z"), qsTr("Sort: Z-A"),
                    qsTr("Sort: Running")]
            currentIndex: controls.appModel ? controls.appModel.sortOrder : AppModel.HostOrder
            Accessible.name: qsTr("Sort apps")
            ToolTip.text: qsTr("Sort apps: host order, name, or running first")
            ToolTip.delay: 700
            ToolTip.visible: hovered
            navUpItem: controls.appView && controls.appView.showHiddenGames
                          ? hiddenCombo : statusCombo
            navDownItem: groupCombo
            onActivated: if (controls.appModel) controls.appModel.sortOrder = currentIndex
        }

        AutoResizingComboBox {
            id: groupCombo
            Layout.minimumWidth: 78
            Layout.maximumWidth: 160
            maximumWidth: 160
            model: [qsTr("Group: None"), qsTr("Group: Type"), qsTr("Group: Status")]
            currentIndex: controls.appModel ? controls.appModel.groupMode : AppModel.NoGrouping
            Accessible.name: qsTr("Group apps")
            ToolTip.text: qsTr("Group apps by type or running status")
            ToolTip.delay: 700
            ToolTip.visible: hovered
            navUpItem: sortCombo
            navDownItem: controls.appView ? controls.appView.pickerNavigationTarget : null
            onActivated: if (controls.appModel) controls.appModel.groupMode = currentIndex
        }
    }

    Connections {
        target: controls.appModel

        function onPickerOptionsChanged() {
            if (controls.appModel && controls.searchValue !== controls.appModel.searchText) {
                controls.searchValue = controls.appModel.searchText
            }
        }
    }
}
