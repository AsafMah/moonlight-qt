import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.2

import ShortcutModel 1.0

GroupBox {
    id: shortcutsGroupBox
    padding: 12
    title: "<font color=\"skyblue\">" + qsTr("Keyboard Shortcuts") + "</font>"
    font.pointSize: 12

    // The binding being captured, or -1 when idle
    property int captureRow: -1
    property int captureSlot: -1
    property bool captureRejected: false

    function startCapture(row, slot) {
        captureRow = row
        captureSlot = slot
        captureRejected = false
    }

    function stopCapture() {
        ShortcutModel.cancelCapture()
        captureRow = -1
        captureSlot = -1
        captureRejected = false
    }

    Connections {
        target: ShortcutModel

        function onCaptureFinished(row, slot) {
            if (shortcutsGroupBox.captureRow === row &&
                shortcutsGroupBox.captureSlot === slot) {
                shortcutsGroupBox.stopCapture()
            }
        }

        function onCaptureRejected(row, slot) {
            if (shortcutsGroupBox.captureRow === row &&
                shortcutsGroupBox.captureSlot === slot) {
                shortcutsGroupBox.captureRejected = true
            }
        }
    }

    Column {
        anchors.fill: parent
        spacing: 5

        Label {
            width: parent.width
            text: qsTr("Click a shortcut, then press the new key combination. Esc cancels and Backspace removes the shortcut.")
            font.pointSize: 9
            wrapMode: Text.Wrap
        }

        Row {
            spacing: 5
            width: parent.width

            Label {
                text: qsTr("Default modifier keys:")
                font.pointSize: 12
                anchors.verticalCenter: parent.verticalCenter
            }

            AutoResizingComboBox {
                id: prefixComboBox
                textRole: "text"
                model: ListModel {
                    id: prefixListModel
                    ListElement {
                        text: "Ctrl+Alt+Shift"
                        val: ShortcutModel.PrefixCtrlAltShift
                    }
                    ListElement {
                        text: "Ctrl+Alt"
                        val: ShortcutModel.PrefixCtrlAlt
                    }
                }

                function syncIndex() {
                    for (var i = 0; i < prefixListModel.count; i++) {
                        if (prefixListModel.get(i).val === ShortcutModel.defaultPrefix) {
                            currentIndex = i
                            return
                        }
                    }
                    currentIndex = 0
                }

                Component.onCompleted: syncIndex()

                // ::onActivated must be used, as it only listens for when the index is changed by a human
                onActivated: {
                    ShortcutModel.defaultPrefix = prefixListModel.get(currentIndex).val
                }

                Connections {
                    target: ShortcutModel
                    onDefaultPrefixChanged: prefixComboBox.syncIndex()
                }

                ToolTip.delay: 1000
                ToolTip.timeout: 10000
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Changes every shortcut that still uses its default key combination.") + "\n\n" +
                              qsTr("NOTE: Ctrl+Alt is also how AltGr is reported on some keyboard layouts, so Ctrl+Alt shortcuts can trigger while typing.")
            }
        }

        CheckBox {
            text: qsTr("Hold Right Alt to release the mouse")
            checked: ShortcutModel.rightAltReleaseEnabled
            onToggled: ShortcutModel.rightAltReleaseEnabled = checked
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Hold to release the mouse. While held, click the performance overlay to edit it; menu clicks work normally. Let go to recapture unless an editor or menu owns the mouse. Right Alt and AltGr are not sent to the host.")
        }

        Repeater {
            model: ShortcutModel

            delegate: Column {
                id: actionDelegate
                width: parent.width
                spacing: 2

                property int actionRow: index
                property bool actionHold: model.hold
                property var actionBindings: [model.binding1, model.binding2]
                property var actionConflicts: [model.conflict1, model.conflict2]

                Label {
                    visible: model.firstInGroup
                    topPadding: actionDelegate.actionRow > 0 ? 10 : 0
                    text: model.group
                    font.pointSize: 12
                    font.bold: true
                }

                RowLayout {
                    width: parent.width
                    spacing: 5

                    Label {
                        Layout.fillWidth: true
                        text: model.name
                        font.pointSize: 11
                        wrapMode: Text.Wrap
                    }

                    Repeater {
                        model: 2

                        delegate: Button {
                            id: bindingButton

                            property bool capturing: shortcutsGroupBox.captureRow === actionDelegate.actionRow &&
                                                     shortcutsGroupBox.captureSlot === index
                            property bool conflicted: actionDelegate.actionConflicts[index] !== ""

                            Layout.preferredWidth: 170
                            hoverEnabled: true
                            font.pointSize: 10
                            font.capitalization: Font.MixedCase
                            highlighted: capturing
                            text: capturing ? qsTr("Press keys...") :
                                  (actionDelegate.actionBindings[index] !== "" ? actionDelegate.actionBindings[index] : qsTr("None"))

                            contentItem: Label {
                                text: bindingButton.text
                                font: bindingButton.font
                                color: bindingButton.conflicted && !bindingButton.capturing ? "#ff6e6e" : "white"
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                            }

                            onClicked: {
                                shortcutsGroupBox.startCapture(actionDelegate.actionRow, index)
                                forceActiveFocus()
                            }

                            // Claim keys that would otherwise trigger window shortcuts
                            Keys.onShortcutOverride: event.accepted = capturing

                            Keys.onPressed: {
                                if (!capturing) {
                                    return
                                }

                                // Always consume the key so it can't navigate or activate anything
                                event.accepted = true
                                if (event.isAutoRepeat) {
                                    return
                                }

                                var result = ShortcutModel.captureKey(actionDelegate.actionRow, index,
                                                                      event.key, event.modifiers,
                                                                      event.nativeScanCode !== undefined ? event.nativeScanCode : 0)
                                if (result === ShortcutModel.CaptureInvalid) {
                                    shortcutsGroupBox.captureRejected = true
                                }
                                else if (result !== ShortcutModel.CaptureIgnored) {
                                    shortcutsGroupBox.stopCapture()
                                }
                            }

                            // A hold action may be bound to a modifier key on its own,
                            // which is recorded when that key comes up
                            Keys.onReleased: {
                                if (!capturing) {
                                    return
                                }

                                event.accepted = true
                                if (event.isAutoRepeat) {
                                    return
                                }

                                var result = ShortcutModel.captureKeyRelease(actionDelegate.actionRow, index, event.key,
                                                                             event.nativeScanCode !== undefined ? event.nativeScanCode : 0)
                                if (result === ShortcutModel.CaptureInvalid) {
                                    shortcutsGroupBox.captureRejected = true
                                }
                                else if (result === ShortcutModel.CaptureAccepted) {
                                    shortcutsGroupBox.stopCapture()
                                }
                            }

                            ToolTip.delay: 1000
                            ToolTip.timeout: 5000
                            ToolTip.visible: hovered && !capturing
                            ToolTip.text: qsTr("Click to change this shortcut")
                        }
                    }

                    Button {
                        flat: true
                        enabled: model.customized
                        text: qsTr("Reset")
                        font.pointSize: 10
                        onClicked: ShortcutModel.resetAction(actionDelegate.actionRow)
                    }
                }

                Label {
                    width: parent.width
                    visible: text !== ""
                    text: actionDelegate.actionConflicts.filter(function(c) { return c !== "" }).join("\n")
                    color: "#ff6e6e"
                    font.pointSize: 9
                    wrapMode: Text.Wrap
                }

                Label {
                    width: parent.width
                    visible: actionDelegate.actionHold
                    text: qsTr("Works while the key is held down. A modifier key on its own works too, e.g. Right Alt; it then no longer reaches the host, so AltGr typing stops there.")
                    font.pointSize: 9
                    wrapMode: Text.Wrap
                }

                Label {
                    width: parent.width
                    visible: shortcutsGroupBox.captureRejected && shortcutsGroupBox.captureRow === actionDelegate.actionRow
                    text: actionDelegate.actionHold ?
                              qsTr("Use a modifier key on its own, or Ctrl, Alt or the Windows/Command key with another key.") :
                              qsTr("Use Ctrl, Alt or the Windows/Command key with this key. Only F13-F24 and media keys work on their own.")
                    color: "#ff6e6e"
                    font.pointSize: 9
                    wrapMode: Text.Wrap
                }
            }
        }

        Button {
            text: qsTr("Reset all shortcuts")
            font.pointSize: 10
            onClicked: {
                shortcutsGroupBox.stopCapture()
                ShortcutModel.resetAll()
            }
        }
    }
}
