pragma ComponentBehavior: Bound
import QtQuick 2.9
import QtQuick.Controls
import "."
import "../theme"

import ShortcutModel 1.0

// 键盘快捷键：每个动作一行，两个绑定槽。点击槽位后按下新组合键录入，
// Esc 取消、Backspace 清除。绑定按物理键位（扫描码）匹配，与键盘布局无关。
// 改动即时写入设置，串流开始时由 SdlInputHandler 读取。
SettingsCard {
    id: shortcutCard

    title: qsTr("Keyboard shortcuts")
    subtitle: qsTr("Click a shortcut, then press the new key combination. Esc cancels and Backspace removes the shortcut. Shortcuts follow key positions, so they keep working when you switch keyboard layouts.")

    // 正在录入的槽位，空闲时为 -1
    property int captureRow: -1
    property int captureSlot: -1
    property bool captureRejected: false

    function startCapture(row, slot) {
        captureRow = row
        captureSlot = slot
        captureRejected = false
    }

    function stopCapture() {
        captureRow = -1
        captureSlot = -1
        captureRejected = false
    }

    ChoiceRow {
        title: qsTr("Default modifier keys")
        description: qsTr("Changes every shortcut that still uses its default key combination.") + " " +
                     qsTr("NOTE: Ctrl+Alt is also how AltGr is reported on some keyboard layouts, so Ctrl+Alt shortcuts can trigger while typing.")
        selectedValue: ShortcutModel.defaultPrefix
        onValueActivated: function(value) { ShortcutModel.defaultPrefix = value }

        model: ListModel {
            ListElement { text: "Ctrl+Alt+Shift"; val: ShortcutModel.PrefixCtrlAltShift }
            ListElement { text: "Ctrl+Alt"; val: ShortcutModel.PrefixCtrlAlt }
        }
    }

    Repeater {
        model: ShortcutModel

        delegate: Column {
            id: actionItem

            required property int index
            required property string name
            required property string group
            required property bool firstInGroup
            required property string binding1
            required property string binding2
            required property string conflict1
            required property string conflict2
            required property bool customized

            readonly property var bindings: [binding1, binding2]
            readonly property var conflicts: [conflict1, conflict2]
            readonly property bool capturingThis: shortcutCard.captureRow === index

            width: parent ? parent.width : 0
            spacing: 0

            MicroLabel {
                visible: actionItem.firstInGroup
                width: parent.width
                topPadding: actionItem.index > 0 ? Theme.spaceMd : 0
                bottomPadding: Theme.spaceXs
                leftPadding: Theme.spaceMd
                text: actionItem.group
            }

            SettingsRow {
                title: actionItem.name
                description: {
                    var lines = actionItem.conflicts.filter(function(c) { return c !== "" })
                    if (shortcutCard.captureRejected && actionItem.capturingThis) {
                        lines.push(qsTr("Use Ctrl, Alt or the Windows/Command key with this key. Only F13-F24 and media keys work on their own."))
                    }
                    return lines.join("\n")
                }

                Row {
                    spacing: Theme.spaceSm

                    Repeater {
                        model: 2

                        delegate: HardButton {
                            id: bindingButton

                            required property int index

                            readonly property bool capturing: actionItem.capturingThis &&
                                                              shortcutCard.captureSlot === index
                            readonly property bool conflicted: actionItem.conflicts[index] !== ""

                            // 按最长的常见组合定宽，各行对齐且按键名不被截断
                            TextMetrics {
                                id: longestBinding
                                font: bindingButton.font
                                text: "Ctrl+Alt+Shift+Win+PgDn"
                            }

                            width: Math.max(150, Math.ceil(longestBinding.width) + leftPadding + rightPadding)
                            primary: capturing
                            font.family: Theme.fontMono
                            font.bold: false
                            palette.buttonText: capturing ? Theme.ink
                                                          : (conflicted ? Theme.danger : Theme.text)
                            text: capturing ? qsTr("Press keys...")
                                            : (actionItem.bindings[index] !== ""
                                               ? actionItem.bindings[index] : qsTr("None"))
                            Accessible.name: actionItem.name + " " + text

                            onClicked: {
                                shortcutCard.startCapture(actionItem.index, index)
                                forceActiveFocus()
                            }

                            onActiveFocusChanged: {
                                if (!activeFocus && capturing) {
                                    shortcutCard.stopCapture()
                                }
                            }

                            // 录入时抢在窗口级快捷键之前拿到按键
                            Keys.onShortcutOverride: function(event) {
                                event.accepted = bindingButton.capturing
                            }

                            Keys.onPressed: function(event) {
                                if (!bindingButton.capturing) {
                                    return
                                }

                                // 录入中的按键一律吞掉，避免触发 Tab 导航或按钮点击
                                event.accepted = true
                                if (event.isAutoRepeat) {
                                    return
                                }

                                var result = ShortcutModel.captureKey(actionItem.index, bindingButton.index,
                                                                      event.key, event.modifiers,
                                                                      event.nativeScanCode)
                                if (result === ShortcutModel.CaptureInvalid) {
                                    shortcutCard.captureRejected = true
                                }
                                else if (result !== ShortcutModel.CaptureIgnored) {
                                    shortcutCard.stopCapture()
                                }
                            }
                        }
                    }

                    HardLink {
                        text: qsTr("Reset")
                        enabled: actionItem.customized
                        onClicked: ShortcutModel.resetAction(actionItem.index)
                    }
                }
            }

            // SettingsRow 只在不是父级最后一个子项时画分隔线，这里包了一层 Column，
            // 所以自己补上
            Rectangle {
                width: parent.width
                height: 1
                color: Theme.line
            }
        }
    }

    SettingsRow {
        title: qsTr("Reset all shortcuts")
        description: qsTr("Restores every default shortcut and the Ctrl+Alt+Shift modifier keys.")

        HardButton {
            text: qsTr("Reset all")
            onClicked: {
                shortcutCard.stopCapture()
                ShortcutModel.resetAll()
            }
        }
    }
}
