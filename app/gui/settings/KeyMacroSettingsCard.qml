pragma ComponentBehavior: Bound
import QtQuick 2.9
import QtQuick.Controls
import "."
import "../theme"

import KeyMacroModel 1.0

// 自定义按键宏：命名的组合键，出现在串流菜单的「发送按键」页。
// 录入方式与快捷键卡片相同(Esc 取消、Backspace 清除)，但宏可以不带修饰键。
// 改动即时写入设置，菜单每次打开时读取。
SettingsCard {
    id: macroCard

    title: qsTr("Key macros")
    subtitle: qsTr("Named key combinations for the Send Keys page of the in-stream menu, next to keys like Ctrl+Alt+Del and Win. Up to %1 macros.").arg(KeyMacroModel.maxMacros)

    property bool capturing: false
    property bool captureRejected: false

    function stopCapture() {
        capturing = false
        captureRejected = false
    }

    Repeater {
        model: KeyMacroModel

        delegate: SettingsRow {
            id: macroRow

            required property int index
            required property string name
            required property string keys

            title: name

            Row {
                spacing: Theme.spaceSm

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: macroRow.keys
                    color: Theme.textDim
                    font.family: Theme.fontMono
                    font.pointSize: Theme.fontBody
                }

                HardLink {
                    text: qsTr("Delete")
                    Accessible.name: qsTr("Delete %1").arg(macroRow.name)
                    onClicked: KeyMacroModel.removeMacro(macroRow.index)
                }
            }
        }
    }

    SettingsRow {
        title: qsTr("New macro")
        description: {
            if (macroCard.captureRejected) {
                return qsTr("This key can't be sent to the host. Try another key.")
            }
            if (!KeyMacroModel.canAdd) {
                return qsTr("You have %1 macros, the most allowed. Delete one to add another.").arg(KeyMacroModel.maxMacros)
            }
            return qsTr("Enter a name, click the key button and press the combination. Keys the system keeps for itself, like Alt+Tab, can't be recorded here.")
        }

        Row {
            spacing: Theme.spaceSm

            HardTextField {
                id: nameField

                width: 160
                enabled: KeyMacroModel.canAdd
                placeholderText: qsTr("Name")
                maximumLength: KeyMacroModel.maxNameLength
                Accessible.name: qsTr("Macro name")
            }

            HardButton {
                id: captureButton

                width: 150
                enabled: KeyMacroModel.canAdd
                primary: macroCard.capturing
                font.family: Theme.fontMono
                font.bold: false
                palette.buttonText: macroCard.capturing ? Theme.ink : Theme.text
                text: macroCard.capturing ? qsTr("Press keys...")
                                          : (KeyMacroModel.pendingKeys !== ""
                                             ? KeyMacroModel.pendingKeys : qsTr("Set keys"))
                Accessible.name: qsTr("Macro keys") + " " + text

                onClicked: {
                    macroCard.capturing = true
                    macroCard.captureRejected = false
                    forceActiveFocus()
                }

                onActiveFocusChanged: {
                    if (!activeFocus) {
                        macroCard.stopCapture()
                    }
                }

                // 录入时抢在窗口级快捷键之前拿到按键
                Keys.onShortcutOverride: function(event) {
                    event.accepted = macroCard.capturing
                }

                Keys.onPressed: function(event) {
                    if (!macroCard.capturing) {
                        return
                    }

                    // 录入中的按键一律吞掉，避免触发 Tab 导航或按钮点击
                    event.accepted = true
                    if (event.isAutoRepeat) {
                        return
                    }

                    var result = KeyMacroModel.captureKey(event.key, event.modifiers,
                                                          event.nativeScanCode)
                    if (result === KeyMacroModel.CaptureInvalid) {
                        macroCard.captureRejected = true
                    }
                    else if (result !== KeyMacroModel.CaptureIgnored) {
                        macroCard.stopCapture()
                    }
                }
            }

            HardButton {
                text: qsTr("Add")
                enabled: KeyMacroModel.canAdd && KeyMacroModel.pendingKeys !== "" &&
                         nameField.text.trim() !== ""
                onClicked: {
                    macroCard.stopCapture()
                    if (KeyMacroModel.addMacro(nameField.text)) {
                        nameField.text = ""
                    }
                }
            }
        }
    }
}
