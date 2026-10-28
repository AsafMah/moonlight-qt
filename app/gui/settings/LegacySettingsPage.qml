import QtQuick 2.9
import QtQuick.Controls
import Qt.labs.platform 1.1
import "."
import ".."
import "../theme"
import "../Brand.js" as Brand

import StreamingPreferences 1.0
import OverlayMenuSettings 1.0
import SdlGamepadKeyNavigation 1.0
import ComputerManager 1.0
import SystemProperties 1.0
import ImageUtils 1.0
import PyrowaveStatus 1.0

// 这六个分类保留在同一文件中，以维持现有 LegacySettingsPage 翻译上下文。
// 视觉和交互已经迁移到 SettingsCard / SettingsRow / ToggleRow。
Column {
    id: settingsPage

    signal languageChanged()

    property string category: ""
    // 关闭捕获时保留用户上次选择的作用范围，重新开启后恢复原模式。
    property int captureSysKeysSelection: StreamingPreferences.CSK_FULLSCREEN
    // PyroWave fixes some video pipeline settings (see PyrowaveCodecPanel).
    // Their rows are greyed out and keep the saved values.
    readonly property bool pyrowaveSelected: PyrowaveStatus.runtimeAvailable &&
        StreamingPreferences.videoCodecConfig === StreamingPreferences.VCC_FORCE_PYROWAVE

    function pyrowaveLockNote(reason) {
        return qsTranslate("PyrowaveCodecPanel", "Fixed while PyroWave is selected") + ": " + reason
    }

    function applyLocalBackgroundImage(fileUrl) {
        var validationError = localBackgroundImageUtils.validateLocalBackgroundImage(fileUrl)
        if (validationError !== "") {
            localBackgroundErrorDialog.text = validationError
            localBackgroundErrorDialog.open()
            return
        }

        StreamingPreferences.backgroundImageLocalPath = fileUrl
    }

    width: parent ? parent.width : 0
    spacing: Theme.spaceLg

    // ================= 音频 =================
    SettingsCard {
        settingsCategory: "audio"
        visible: settingsPage.category === settingsCategory && hasVisibleContent
        title: qsTr("Audio Settings")

        ChoiceRow {
            settingsSearchKeywords: "audio sound surround stereo 5.1 7.1 speakers channels"
            title: qsTr("Audio configuration")
            selectedValue: StreamingPreferences.audioConfig
            onValueActivated: function(value) { StreamingPreferences.audioConfig = value }

            model: ListModel {
                ListElement { text: qsTr("Stereo"); val: StreamingPreferences.AC_STEREO }
                ListElement { text: qsTr("5.1 surround sound"); val: StreamingPreferences.AC_51_SURROUND }
                ListElement { text: qsTr("7.1 surround sound"); val: StreamingPreferences.AC_71_SURROUND }
                ListElement { text: qsTr("7.1.4 surround sound"); val: StreamingPreferences.AC_714_SURROUND }
            }
        }

        ToggleRow {
            settingsSearchKeywords: "sound audio silence"
            title: qsTr("Mute host PC speakers while streaming")
            description: qsTr("You must restart any game currently in progress for this setting to take effect")
            checked: !StreamingPreferences.playAudioOnHost
            onToggled: function(value) { StreamingPreferences.playAudioOnHost = !value }
        }

        ToggleRow {
            applicable: SystemProperties.hasDesktopEnvironment
            settingsSearchKeywords: "sound background focus"
            title: Brand.text(qsTr("Mute audio stream when Moonlight is not the active window"))
            description: Brand.text(qsTr("Mutes Moonlight's audio when you Alt+Tab out of the stream or click on a different window."))
            checked: StreamingPreferences.muteOnFocusLoss
            onToggled: function(value) { StreamingPreferences.muteOnFocusLoss = value }
        }

        ToggleRow {
            settingsSearchKeywords: "microphone mic voice"
            title: qsTr("Enable microphone streaming (test)")
            checked: StreamingPreferences.enableMicrophone
            onToggled: function(value) { StreamingPreferences.enableMicrophone = value }
        }
    }

    // ================= 主机 =================
    SettingsCard {
        settingsCategory: "host"
        visible: settingsPage.category === settingsCategory && hasVisibleContent
        title: qsTr("Screen Combination Mode")
        subtitle: qsTr("Sunshine Foundation display control")

        ScreenCombinationModeSelector {
            width: parent.width
        }
    }

    SettingsCard {
        settingsCategory: "host"
        visible: settingsPage.category === settingsCategory && hasVisibleContent
        title: qsTr("Host Settings")

        ToggleRow {
            settingsSearchKeywords: "geforce experience game settings"
            title: qsTr("Optimize game settings for streaming")
            checked: StreamingPreferences.gameOptimizations
            onToggled: function(value) { StreamingPreferences.gameOptimizations = value }
        }

        ToggleRow {
            title: qsTr("Quit app on host PC after ending stream")
            description: qsTr("This will close the app or game you are streaming when you end your stream. You will lose any unsaved progress!")
            checked: StreamingPreferences.quitAppAfter
            onToggled: function(value) { StreamingPreferences.quitAppAfter = value }
        }
    }

    // ================= 软件 =================
    SettingsCard {
        settingsCategory: "ui"
        visible: settingsPage.category === settingsCategory && hasVisibleContent
        title: qsTr("Software Settings")

        ChoiceRow {
            id: languageRow
            settingsSearchKeywords: "language translation locale chinese english"
            title: qsTr("Language")
            maximumControlWidth: 260
            controlWidth: 260
            selectedValue: StreamingPreferences.language
            onValueActivated: function(value) {
                if (StreamingPreferences.language === value) {
                    return
                }

                StreamingPreferences.language = value
                if (!StreamingPreferences.retranslate()) {
                    ToolTip.show(Brand.text(qsTr("You must restart Moonlight for this change to take effect")), 5000)
                }
                else {
                    window.clearOnBack = true
                    settingsPage.languageChanged()
                    languageRow.syncSelection()
                }
            }

            model: ListModel {
                ListElement { text: qsTr("Automatic"); val: StreamingPreferences.LANG_AUTO }
                ListElement { text: "Deutsch"; val: StreamingPreferences.LANG_DE }
                ListElement { text: "English"; val: StreamingPreferences.LANG_EN }
                ListElement { text: "Français"; val: StreamingPreferences.LANG_FR }
                ListElement { text: "简体中文"; val: StreamingPreferences.LANG_ZH_CN }
                ListElement { text: "Norwegian Bokmål"; val: StreamingPreferences.LANG_NB_NO }
                ListElement { text: "русский"; val: StreamingPreferences.LANG_RU }
                ListElement { text: "Español"; val: StreamingPreferences.LANG_ES }
                ListElement { text: "日本語"; val: StreamingPreferences.LANG_JA }
                ListElement { text: "Tiếng Việt"; val: StreamingPreferences.LANG_VI }
                ListElement { text: "ภาษาไทย"; val: StreamingPreferences.LANG_TH }
                ListElement { text: "한국어"; val: StreamingPreferences.LANG_KO }
                ListElement { text: "Magyar"; val: StreamingPreferences.LANG_HU }
                ListElement { text: "Nederlands"; val: StreamingPreferences.LANG_NL }
                ListElement { text: "Svenska"; val: StreamingPreferences.LANG_SV }
                ListElement { text: "Türkçe"; val: StreamingPreferences.LANG_TR }
                ListElement { text: "繁體中文"; val: StreamingPreferences.LANG_ZH_TW }
                ListElement { text: "Português"; val: StreamingPreferences.LANG_PT }
                ListElement { text: "Português do Brasil"; val: StreamingPreferences.LANG_PT_BR }
                ListElement { text: "Ελληνικά"; val: StreamingPreferences.LANG_EL }
                ListElement { text: "Italiano"; val: StreamingPreferences.LANG_IT }
                ListElement { text: "Język polski"; val: StreamingPreferences.LANG_PL }
                ListElement { text: "Čeština"; val: StreamingPreferences.LANG_CS }
                ListElement { text: "Български"; val: StreamingPreferences.LANG_BG }
                ListElement { text: "தமிழ்"; val: StreamingPreferences.LANG_TA }
            }
        }

        ChoiceRow {
            applicable: SystemProperties.hasDesktopEnvironment
            title: qsTr("Window display mode")
            description: qsTr("Choose how Moonlight opens when the application starts.")
            maximumControlWidth: 260
            controlWidth: 260
            selectedValue: StreamingPreferences.uiDisplayMode
            onValueActivated: function(value) { StreamingPreferences.uiDisplayMode = value }

            model: ListModel {
                ListElement { text: qsTr("Windowed"); val: StreamingPreferences.UI_WINDOWED }
                ListElement { text: qsTr("Maximized"); val: StreamingPreferences.UI_MAXIMIZED }
                ListElement { text: qsTr("Fullscreen"); val: StreamingPreferences.UI_FULLSCREEN }
            }
        }

        ToggleRow {
            applicable: SystemProperties.hasDesktopEnvironment &&
                        (!SystemProperties.isRunningWayland || SystemProperties.isRunningXWayland)
            title: qsTr("Remember window position and size")
            description: qsTr("Restore the previous window position and size the next time Moonlight starts.")
            checked: StreamingPreferences.rememberWindowPosition
            onToggled: function(value) { StreamingPreferences.rememberWindowPosition = value }
        }
    }

    SettingsCard {
        settingsCategory: "ui"
        visible: settingsPage.category === settingsCategory && hasVisibleContent
        title: qsTr("Background image")
        subtitle: qsTr("Choose the background shown on the computer list and shared application pages.")

        ChoiceRow {
            id: backgroundSourceRow
            settingsSearchKeywords: "wallpaper background image"
            title: qsTr("Background source")
            description: qsTr("Choose photography, Anime, a custom API, a local image, or no background.")
            maximumControlWidth: 260
            controlWidth: 260
            selectedValue: StreamingPreferences.backgroundSource
            onValueActivated: function(value) {
                if (value === StreamingPreferences.BGS_LOCAL &&
                        StreamingPreferences.backgroundImageLocalPath === "") {
                    backgroundSourceRow.syncSelection()
                    localBackgroundFileDialog.open()
                    return
                }
                if (value === StreamingPreferences.BGS_API &&
                        StreamingPreferences.backgroundImageApi.trim() === "") {
                    backgroundSourceRow.syncSelection()
                    Qt.callLater(function() {
                        backgroundApiField.forceActiveFocus(Qt.TabFocusReason)
                    })
                    return
                }
                StreamingPreferences.backgroundSource = value
            }

            model: ListModel {
                ListElement { text: qsTr("Photography (Lorem Picsum)"); val: StreamingPreferences.BGS_PHOTOGRAPHY }
                ListElement { text: qsTr("Anime (Pipw)"); val: StreamingPreferences.BGS_ANIME }
                ListElement { text: qsTr("Custom API"); val: StreamingPreferences.BGS_API }
                ListElement { text: qsTr("Local image"); val: StreamingPreferences.BGS_LOCAL }
                ListElement { text: qsTr("No background"); val: StreamingPreferences.BGS_NONE }
            }
        }

        SettingsRow {
            id: backgroundOpacityRow
            title: qsTr("Background overlay opacity")
            description: qsTr("Adjust the opacity of the dark overlay on background images. 0% keeps the image clear; 100% makes it fully dark.")

            Row {
                width: Math.min(360, Math.max(220, backgroundOpacityRow.width - Theme.spaceMd * 2))
                height: Math.max(backgroundOpacitySlider.implicitHeight,
                                 backgroundOpacityValue.implicitHeight)
                spacing: Theme.spaceSm

                HardSlider {
                    id: backgroundOpacitySlider
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - backgroundOpacityValue.width - parent.spacing
                    from: 0
                    to: 100
                    stepSize: 1
                    snapMode: Slider.SnapAlways
                    value: StreamingPreferences.backgroundOverlayOpacity
                    Accessible.name: backgroundOpacityRow.title
                    onMoved: StreamingPreferences.backgroundOverlayOpacity = Math.round(value)
                }

                Text {
                    id: backgroundOpacityValue
                    anchors.verticalCenter: parent.verticalCenter
                    width: 52
                    text: Math.round(backgroundOpacitySlider.value) + "%"
                    color: Theme.accent
                    font.family: Theme.fontMono
                    font.pointSize: Theme.fontBody
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignRight
                }
            }
        }

        SettingsRow {
            id: backgroundApiRow
            title: qsTr("Background image API")
            description: qsTr("Enter an image API or a direct image URL. Leaving it empty restores Photography.")

            HardTextField {
                id: backgroundApiField
                width: Math.min(360, Math.max(120, backgroundApiRow.width - Theme.spaceMd * 2))
                placeholderText: "https://example.com/image"

                Component.onCompleted: text = StreamingPreferences.backgroundImageApi
                onEditingFinished: {
                    StreamingPreferences.backgroundImageApi = text
                    text = StreamingPreferences.backgroundImageApi
                }

                Connections {
                    target: StreamingPreferences
                    function onBackgroundConfigurationChanged() {
                        if (!backgroundApiField.activeFocus) {
                            backgroundApiField.text = StreamingPreferences.backgroundImageApi
                        }
                    }
                }
            }
        }

        SettingsRow {
            title: qsTr("Local background image")
            description: qsTr("Choose a JPG, PNG, WebP, or BMP image. You can also drag one image onto the computer list to replace the background.")

            HardButton {
                text: qsTr("Choose image")
                onClicked: localBackgroundFileDialog.open()
            }
        }

        SettingsRow {
            title: qsTr("Restore default background")
            description: qsTr("Clear the custom API and local image, then return to Photography.")

            HardButton {
                text: qsTr("Restore default")
                onClicked: StreamingPreferences.resetBackgroundConfiguration()
            }
        }
    }

    FileDialog {
        id: localBackgroundFileDialog
        title: qsTr("Choose a background image")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Image files (*.jpg *.jpeg *.png *.webp *.bmp)")]
        onAccepted: {
            settingsPage.applyLocalBackgroundImage(file.toString())
        }
    }

    ImageUtils {
        id: localBackgroundImageUtils
    }

    ErrorMessageDialog {
        id: localBackgroundErrorDialog
        text: ""
    }

    SettingsCard {
        settingsCategory: "ui"
        visible: settingsPage.category === settingsCategory && hasVisibleContent
        title: qsTr("Notifications and integrations")

        ToggleRow {
            title: qsTr("Show connection quality warnings")
            checked: StreamingPreferences.connectionWarnings
            onToggled: function(value) { StreamingPreferences.connectionWarnings = value }
        }

        ToggleRow {
            title: qsTr("Show configuration warnings")
            checked: StreamingPreferences.configurationWarnings
            onToggled: function(value) { StreamingPreferences.configurationWarnings = value }
        }

        ToggleRow {
            applicable: SystemProperties.hasDiscordIntegration
            settingsSearchKeywords: "discord rich presence status"
            title: qsTr("Discord Rich Presence integration")
            description: qsTr("Updates your Discord status to display the name of the game you're streaming.")
            checked: StreamingPreferences.richPresence
            onToggled: function(value) { StreamingPreferences.richPresence = value }
        }

        ToggleRow {
            settingsSearchKeywords: "sleep screensaver power"
            title: qsTr("Keep the display awake while streaming")
            description: qsTr("Prevents the screensaver from starting or the display from going to sleep while streaming.")
            checked: StreamingPreferences.keepAwake
            onToggled: function(value) { StreamingPreferences.keepAwake = value }
        }

        ToggleRow {
            settingsSearchKeywords: "update version upgrade"
            title: qsTr("Automatically check for updates")
            description: Brand.text(qsTr("Check for new versions of Moonlight when the app starts."))
            checked: StreamingPreferences.autoUpdateCheck
            onToggled: function(value) { StreamingPreferences.autoUpdateCheck = value }
        }
    }

    // ================= 输入 =================
    SettingsCard {
        settingsCategory: "input"
        visible: settingsPage.category === settingsCategory && hasVisibleContent
        title: qsTr("Mouse")

        ToggleRow {
            settingsSearchKeywords: "mouse remote desktop absolute pointer"
            title: qsTr("Optimize mouse for remote desktop instead of games")
            description: qsTr("This enables seamless mouse control without capturing the client's mouse cursor. It is ideal for remote desktop usage but will not work in most games.") + " " +
                         qsTr("You can toggle this while streaming using Ctrl+Alt+Shift+M.") + "\n\n" +
                         qsTr("NOTE: Due to a bug in GeForce Experience, this option may not work properly if your host PC has multiple monitors.")
            checked: StreamingPreferences.absoluteMouseMode
            onToggled: function(value) { StreamingPreferences.absoluteMouseMode = value }
        }

        ToggleRow {
            settingsSearchKeywords: "mouse pointer cursor"
            title: qsTr("Show local cursor")
            description: qsTr("This makes the client's mouse cursor visible in the stream.") + " " +
                         qsTr("You can toggle this while streaming using Ctrl+Alt+Shift+C.")
            checked: StreamingPreferences.showLocalCursor
            onToggled: function(value) { StreamingPreferences.showLocalCursor = value }
        }

        ToggleRow {
            settingsSearchKeywords: "mouse left handed click"
            title: qsTr("Swap left and right mouse buttons")
            checked: StreamingPreferences.swapMouseButtons
            onToggled: function(value) { StreamingPreferences.swapMouseButtons = value }
        }

        ToggleRow {
            settingsSearchKeywords: "keyboard mac command option"
            title: qsTr("Swap Alt and Win keys")
            checked: StreamingPreferences.swapWinAltKeys
            onToggled: function(value) { StreamingPreferences.swapWinAltKeys = value }
        }

        ToggleRow {
            settingsSearchKeywords: "mouse scroll wheel natural invert"
            title: qsTr("Reverse mouse scrolling direction")
            checked: StreamingPreferences.reverseScrollDirection
            onToggled: function(value) { StreamingPreferences.reverseScrollDirection = value }
        }
    }

    SettingsCard {
        settingsCategory: "input"
        visible: settingsPage.category === settingsCategory && hasVisibleContent
        title: qsTr("Keyboard")

        ToggleRow {
            id: captureSysKeysToggle
            applicable: SystemProperties.hasDesktopEnvironment
            settingsSearchKeywords: "keyboard hotkeys alt tab win key"
            title: qsTr("Capture system keyboard shortcuts")
            description: qsTr("This enables the capture of system-wide keyboard shortcuts like Alt+Tab that would normally be handled by the client OS while streaming.") + "\n\n" +
                         Brand.text(qsTr("NOTE: Certain keyboard shortcuts like Ctrl+Alt+Del on Windows cannot be intercepted by any application, including Moonlight."))
            checked: StreamingPreferences.captureSysKeysMode !== StreamingPreferences.CSK_OFF
            onToggled: function(value) {
                StreamingPreferences.captureSysKeysMode = value
                    ? settingsPage.captureSysKeysSelection
                    : StreamingPreferences.CSK_OFF
            }
        }

        ChoiceRow {
            applicable: SystemProperties.hasDesktopEnvironment
            title: qsTr("Capture mode")
            description: qsTr("Choose when system keyboard shortcuts are captured.")
            controlEnabled: captureSysKeysToggle.checked
            selectedValue: settingsPage.captureSysKeysSelection
            onValueActivated: function(value) {
                settingsPage.captureSysKeysSelection = value
                if (captureSysKeysToggle.checked) {
                    StreamingPreferences.captureSysKeysMode = value
                }
            }

            model: ListModel {
                ListElement { text: qsTr("in fullscreen"); val: StreamingPreferences.CSK_FULLSCREEN }
                ListElement { text: qsTr("always"); val: StreamingPreferences.CSK_ALWAYS }
            }
        }
    }

    ShortcutSettingsCard {
        settingsCategory: "input"
        visible: settingsPage.category === settingsCategory && hasVisibleContent
    }

    KeyMacroSettingsCard {
        settingsCategory: "input"
        visible: settingsPage.category === settingsCategory && hasVisibleContent
    }

    SettingsCard {
        settingsCategory: "input"
        visible: settingsPage.category === settingsCategory && hasVisibleContent
        title: qsTr("Touch input")

        ToggleRow {
            settingsSearchKeywords: "touch touchscreen trackpad"
            title: qsTr("Use touchscreen as a virtual trackpad")
            description: qsTr("When checked, the touchscreen acts like a trackpad. When unchecked, the touchscreen will directly control the mouse pointer.")
            checked: !StreamingPreferences.absoluteTouchMode
            onToggled: function(value) { StreamingPreferences.absoluteTouchMode = !value }
        }

        ToggleRow {
            settingsSearchKeywords: "touchpad trackpad gestures"
            title: qsTr("Use precision touchpad input when available")
            description: qsTr("Sends native multi-touch trackpad contacts to compatible Sunshine hosts. Unsupported devices and hosts fall back to pointer input. Changes apply to the next stream.")
            checked: StreamingPreferences.enableNativeTouchpad
            onToggled: function(value) { StreamingPreferences.enableNativeTouchpad = value }
        }
    }

    SettingsCard {
        settingsCategory: "gamepad"
        visible: settingsPage.category === settingsCategory && hasVisibleContent
        title: qsTr("DualSense haptics")

        ChoiceRow {
            id: dualSenseHapticsRow
            settingsSearchKeywords: "ps5 dualsense rumble vibration haptic feedback controller"
            title: qsTr("DualSense haptics")
            description: selectedValue === StreamingPreferences.DSHM_PHYSICAL
                ? qsTr("Sends the original authored PCM to channels 3 and 4 of a USB-connected DualSense. The endpoint is checked before connecting. Changes apply to the next stream.")
                : qsTr("Receives a compact analyzed haptics signal and renders it through the connected controller's vibration motors. Changes apply to the next stream.")
            selectedValue: StreamingPreferences.dualSenseHapticsMode
            onValueActivated: function(value) { StreamingPreferences.dualSenseHapticsMode = value }

            model: ListModel {
                id: dualSenseHapticsModeListModel
                ListElement { text: qsTr("Physical DualSense (native HD haptics)"); val: StreamingPreferences.DSHM_PHYSICAL }
                ListElement { text: qsTr("Simulated DualSense (analyzed vibration)"); val: StreamingPreferences.DSHM_EMULATED }
            }
        }
    }

    // ================= 手柄 =================
    SettingsCard {
        settingsCategory: "gamepad"
        visible: settingsPage.category === settingsCategory && hasVisibleContent
        title: qsTr("Gamepad Settings")

        ToggleRow {
            settingsSearchKeywords: "controller nintendo layout abxy"
            title: qsTr("Swap A/B and X/Y gamepad buttons")
            description: qsTr("This switches gamepads into a Nintendo-style button layout")
            checked: StreamingPreferences.swapFaceButtons
            onToggled: function(value) { StreamingPreferences.swapFaceButtons = value }
        }

        SettingsRow {
            id: stickDeadzoneRow
            settingsSearchKeywords: "stick joystick controller drift"
            title: qsTr("Joystick deadzone")
            description: qsTr("Ignores stick deflections below this percentage, e.g. to compensate for a drifting stick. 0% sends stick input unmodified.")

            Row {
                width: Math.min(360, Math.max(220, stickDeadzoneRow.width - Theme.spaceMd * 2))
                height: Math.max(stickDeadzoneSlider.implicitHeight,
                                 stickDeadzoneValue.implicitHeight)
                spacing: Theme.spaceSm

                HardSlider {
                    id: stickDeadzoneSlider
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - stickDeadzoneValue.width - parent.spacing
                    from: 0
                    to: 30
                    stepSize: 1
                    snapMode: Slider.SnapAlways
                    value: StreamingPreferences.gamepadDeadzone
                    Accessible.name: stickDeadzoneRow.title
                    onMoved: StreamingPreferences.gamepadDeadzone = Math.round(value)
                }

                Text {
                    id: stickDeadzoneValue
                    anchors.verticalCenter: parent.verticalCenter
                    width: 52
                    text: Math.round(stickDeadzoneSlider.value) + "%"
                    color: Theme.accent
                    font.family: Theme.fontMono
                    font.pointSize: Theme.fontBody
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignRight
                }
            }
        }

        ToggleRow {
            settingsSearchKeywords: "controller gamepad"
            title: qsTr("Force gamepad #1 always connected")
            description: qsTr("Forces a single gamepad to always stay connected to the host, even if no gamepads are actually connected to this PC.") + " " +
                         qsTr("Only enable this option when streaming a game that doesn't support gamepads being connected after startup.")
            checked: !StreamingPreferences.multiController
            onToggled: function(value) { StreamingPreferences.multiController = !value }
        }

        ToggleRow {
            settingsSearchKeywords: "controller mouse emulation"
            title: qsTr("Enable mouse control with gamepads by holding the '%1' button").arg(SdlGamepadKeyNavigation.startButtonName())
            description: qsTr("While this is off, holding %1 opens the in-stream overlay menu instead.").arg(SdlGamepadKeyNavigation.startButtonName())
            checked: StreamingPreferences.gamepadMouse
            onToggled: function(value) { StreamingPreferences.gamepadMouse = value }
        }

        ToggleRow {
            applicable: SystemProperties.hasDesktopEnvironment
            title: Brand.text(qsTr("Process gamepad input when Moonlight is in the background"))
            description: Brand.text(qsTr("Allows Moonlight to capture gamepad inputs even if it's not the current window in focus"))
            checked: StreamingPreferences.backgroundGamepad
            onToggled: function(value) { StreamingPreferences.backgroundGamepad = value }
        }
    }

    SettingsCard {
        settingsCategory: "gamepad"
        visible: settingsPage.category === settingsCategory && hasVisibleContent
        title: qsTr("Gamepad quit combo")

        // 按键名按当前连接的手柄风格显示(PS 显示 Options/Share/✕,
        // Switch 显示 +/−/B/A);swapFaceButtons 时补偿到实际要按的物理键。
        // 插拔手柄或切进本页时重建
        function rebuildQuitComboModel() {
            quitComboModel.clear()
            var nav = SdlGamepadKeyNavigation
            var swap = StreamingPreferences.swapFaceButtons
            var face = function(i) { return nav.faceButtonGlyph(swap ? (i ^ 1) : i) }
            var lb = nav.leftShoulderName()
            var rb = nav.rightShoulderName()
            quitComboModel.append({
                text: nav.startButtonName() + "+" + nav.selectButtonName() + "+" + lb + "+" + rb + " " + qsTr("(Default)"),
                val: StreamingPreferences.GQC_DEFAULT
            })
            quitComboModel.append({
                text: nav.selectButtonName() + "+" + lb + "+" + rb + "+" + face(3),
                val: StreamingPreferences.GQC_SELECT_L1_R1_Y
            })
            quitComboModel.append({
                text: nav.startButtonName() + "+" + lb + "+" + rb + "+" + face(0),
                val: StreamingPreferences.GQC_START_L1_R1_A
            })
            quitComboModel.append({
                text: nav.startButtonName() + "+" + lb + "+" + rb + "+" + face(1),
                val: StreamingPreferences.GQC_START_L1_R1_B
            })
            quitComboModel.append({
                text: lb + "+" + rb + "+" + face(2) + "+" + face(3),
                val: StreamingPreferences.GQC_L1_R1_X_Y
            })
            quitComboModel.append({
                text: lb + "+" + rb + "+" + face(0) + "+" + face(1),
                val: StreamingPreferences.GQC_L1_R1_A_B
            })
            quitComboRow.syncSelection()
        }

        Component.onCompleted: rebuildQuitComboModel()
        onVisibleChanged: if (visible) rebuildQuitComboModel()

        ChoiceRow {
            id: quitComboRow
            settingsSearchKeywords: "controller exit shortcut"
            title: qsTr("Gamepad quit combo")
            description: qsTr("Choose which button combination exits streaming. Use alternatives if the default doesn't work on your device.")
            selectedValue: StreamingPreferences.gamepadQuitCombo
            onValueActivated: function(value) { StreamingPreferences.gamepadQuitCombo = value }

            model: ListModel {
                id: quitComboModel
            }
        }
    }

    // ================= 高级 =================
    SettingsCard {
        settingsCategory: "advanced"
        visible: settingsPage.category === settingsCategory && hasVisibleContent
        title: qsTr("Video pipeline")

        ChoiceRow {
            settingsSearchKeywords: "hardware software decoding gpu dxva d3d11va vaapi"
            title: qsTr("Video decoder")
            description: settingsPage.pyrowaveSelected
                         ? settingsPage.pyrowaveLockNote(qsTranslate("PyrowaveCodecPanel", "PyroWave always decodes on the GPU with Vulkan. Your saved setting is kept and still applies to other codecs, including the fallback codec."))
                         : ""
            controlEnabled: !settingsPage.pyrowaveSelected
            selectedValue: StreamingPreferences.videoDecoderSelection
            onValueActivated: function(value) { StreamingPreferences.videoDecoderSelection = value }

            model: ListModel {
                ListElement { text: qsTr("Automatic (Recommended)"); val: StreamingPreferences.VDS_AUTO }
                ListElement { text: qsTr("Force software decoding"); val: StreamingPreferences.VDS_FORCE_SOFTWARE }
                ListElement { text: qsTr("Force hardware decoding"); val: StreamingPreferences.VDS_FORCE_HARDWARE }
            }
        }

        ChoiceRow {
            applicable: SystemProperties.isDarwin
            settingsSearchKeywords: "metal vulkan opengl"
            title: qsTr("Renderer")
            selectedValue: StreamingPreferences.rendererSelection
            onValueActivated: function(value) { StreamingPreferences.rendererSelection = value }

            model: ListModel {
                ListElement { text: qsTr("Automatic (Recommended)"); val: StreamingPreferences.RS_AUTO }
                ListElement { text: "Vulkan"; val: StreamingPreferences.RS_VULKAN }
                ListElement { text: "Metal"; val: StreamingPreferences.RS_METAL }
                ListElement { text: "AVSampleBufferDisplayLayer"; val: StreamingPreferences.RS_AVSBDL }
            }
        }

        ToggleRow {
            settingsSearchKeywords: "chroma 444 subsampling text clarity"
            title: qsTr("Enable YUV 4:4:4")
            description: qsTr("Good for streaming desktop and text-heavy games, but not recommended for fast-paced games.") +
                         (settingsPage.pyrowaveSelected
                          ? "\n" + settingsPage.pyrowaveLockNote(qsTranslate("PyrowaveCodecPanel", "The PyroWave stream contract covers 4:2:0 only, and its fallback codec streams 4:2:0 too. Your saved setting is kept."))
                          : "")
            // Shows what PyroWave streams (off) while the saved value is kept
            controlEnabled: !settingsPage.pyrowaveSelected
            checked: StreamingPreferences.enableYUV444 && !settingsPage.pyrowaveSelected
            onToggled: function(value) {
                if (StreamingPreferences.enableYUV444 === value) {
                    return
                }

                StreamingPreferences.enableYUV444 = value
                if (StreamingPreferences.autoAdjustBitrate) {
                    StreamingPreferences.bitrateKbps = StreamingPreferences.getDefaultBitrate(
                        StreamingPreferences.width,
                        StreamingPreferences.height,
                        StreamingPreferences.fps,
                        value)
                }
            }
        }
    }

    SettingsCard {
        settingsCategory: "advanced"
        visible: settingsPage.category === settingsCategory && hasVisibleContent
        title: qsTr("Network")

        ToggleRow {
            settingsSearchKeywords: "mdns discovery lan network"
            title: qsTr("Automatically find PCs on the local network (Recommended)")
            checked: StreamingPreferences.enableMdns
            onToggled: function(value) {
                if (StreamingPreferences.enableMdns === value) {
                    return
                }

                StreamingPreferences.enableMdns = value
                if (window.pollingActive) {
                    ComputerManager.stopPollingAsync()
                    ComputerManager.startPolling()
                }
            }
        }

        ToggleRow {
            settingsSearchKeywords: "nat firewall ports network"
            title: qsTr("Automatically detect blocked connections (Recommended)")
            checked: StreamingPreferences.detectNetworkBlocking
            onToggled: function(value) { StreamingPreferences.detectNetworkBlocking = value }
        }
    }

    SettingsCard {
        settingsCategory: "advanced"
        visible: settingsPage.category === settingsCategory && hasVisibleContent
        title: qsTr("Diagnostics")

        ToggleRow {
            settingsSearchKeywords: "stats fps latency overlay osd"
            title: qsTr("Show performance stats while streaming")
            description: qsTr("Display real-time stream performance information while streaming.") + "\n\n" +
                         qsTr("You can toggle it at any time while streaming using %1 or %2.")
                             .arg("Ctrl+Alt+Shift+S")
                             .arg(SdlGamepadKeyNavigation.selectButtonName() + "+" +
                                  SdlGamepadKeyNavigation.leftShoulderName() + "+" +
                                  SdlGamepadKeyNavigation.rightShoulderName() + "+" +
                                  SdlGamepadKeyNavigation.faceButtonGlyph(
                                      StreamingPreferences.swapFaceButtons ? 3 : 2)) + "\n\n" +
                         qsTr("The performance overlay is not supported on Steam Link or Raspberry Pi.")
            checked: StreamingPreferences.showPerformanceOverlay
            onToggled: function(value) { StreamingPreferences.showPerformanceOverlay = value }
        }
    }

    SettingsCard {
        id: overlayMenuCard
        settingsCategory: "advanced"
        visible: settingsPage.category === settingsCategory && hasVisibleContent
        title: qsTr("Overlay menu position")

        readonly property bool menuEnabled:
            StreamingPreferences.overlayMenuPosition !== StreamingPreferences.OMP_DISABLED

        Component.onCompleted: OverlayMenuSettings.reload()

        ChoiceRow {
            settingsSearchKeywords: "overlay menu osd in-stream"
            title: qsTr("Overlay menu position")
            description: qsTr("With a gamepad connected, holding %1 also opens the menu while this is not Disabled (requires Gamepad Mouse to be off).").arg(SdlGamepadKeyNavigation.startButtonName())
            selectedValue: StreamingPreferences.overlayMenuPosition
            onValueActivated: function(value) { StreamingPreferences.overlayMenuPosition = value }

            model: ListModel {
                ListElement { text: qsTr("Top edge"); val: StreamingPreferences.OMP_TOP_EDGE }
                ListElement { text: qsTr("Right edge"); val: StreamingPreferences.OMP_RIGHT_EDGE }
                ListElement { text: qsTr("Left edge"); val: StreamingPreferences.OMP_LEFT_EDGE }
                ListElement { text: qsTr("Floating button (default)"); val: StreamingPreferences.OMP_BUTTON }
                ListElement { text: qsTr("Disabled"); val: StreamingPreferences.OMP_DISABLED }
            }
        }

        ChoiceRow {
            applicable: overlayMenuCard.menuEnabled
            title: qsTr("Menu layout")
            selectedValue: OverlayMenuSettings.layout
            onValueActivated: function(value) { OverlayMenuSettings.layout = value }

            model: ListModel {
                ListElement { text: qsTr("Compact (default)"); val: OverlayMenuSettings.LayoutCompact }
                ListElement { text: qsTr("Comfortable: bigger rows for a TV or gamepad"); val: OverlayMenuSettings.LayoutComfortable }
                ListElement { text: qsTr("Side panel: tabs in a column"); val: OverlayMenuSettings.LayoutSidePanel }
            }
        }
        SettingsRow {
            id: overlayMenuScaleRow
            applicable: overlayMenuCard.menuEnabled
            title: qsTr("Menu scale")

            Row {
                width: Math.min(360, Math.max(220, overlayMenuScaleRow.width - Theme.spaceMd * 2))
                height: Math.max(overlayMenuScaleSlider.implicitHeight, overlayMenuScaleValue.implicitHeight)
                spacing: Theme.spaceSm

                HardSlider {
                    id: overlayMenuScaleSlider
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - overlayMenuScaleValue.width - parent.spacing
                    from: OverlayMenuSettings.minScalePercent
                    to: OverlayMenuSettings.maxScalePercent
                    stepSize: 5
                    snapMode: Slider.SnapAlways
                    value: OverlayMenuSettings.scalePercent
                    Accessible.name: overlayMenuScaleRow.title
                    onMoved: OverlayMenuSettings.scalePercent = Math.round(value)
                }

                Text {
                    id: overlayMenuScaleValue
                    anchors.verticalCenter: parent.verticalCenter
                    width: 64
                    text: Math.round(overlayMenuScaleSlider.value) + "%"
                    color: Theme.accent
                    font.family: Theme.fontMono
                    font.pointSize: Theme.fontBody
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignRight
                }
            }
        }
        SettingsRow {
            id: overlayMenuOpacityRow
            applicable: overlayMenuCard.menuEnabled
            title: qsTr("Menu opacity")

            Row {
                width: Math.min(360, Math.max(220, overlayMenuOpacityRow.width - Theme.spaceMd * 2))
                height: Math.max(overlayMenuOpacitySlider.implicitHeight, overlayMenuOpacityValue.implicitHeight)
                spacing: Theme.spaceSm

                HardSlider {
                    id: overlayMenuOpacitySlider
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - overlayMenuOpacityValue.width - parent.spacing
                    from: OverlayMenuSettings.minOpacityPercent
                    to: OverlayMenuSettings.maxOpacityPercent
                    stepSize: 5
                    snapMode: Slider.SnapAlways
                    value: OverlayMenuSettings.opacityPercent
                    Accessible.name: overlayMenuOpacityRow.title
                    onMoved: OverlayMenuSettings.opacityPercent = Math.round(value)
                }

                Text {
                    id: overlayMenuOpacityValue
                    anchors.verticalCenter: parent.verticalCenter
                    width: 64
                    text: Math.round(overlayMenuOpacitySlider.value) + "%"
                    color: Theme.accent
                    font.family: Theme.fontMono
                    font.pointSize: Theme.fontBody
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignRight
                }
            }
        }

        ChoiceRow {
            applicable: overlayMenuCard.menuEnabled
            title: qsTr("Floating button")
            selectedValue: OverlayMenuSettings.buttonMode
            onValueActivated: function(value) { OverlayMenuSettings.buttonMode = value }

            model: ListModel {
                ListElement { text: qsTr("Follow the position above (default)"); val: OverlayMenuSettings.TriggerAuto }
                ListElement { text: qsTr("Always show"); val: OverlayMenuSettings.TriggerOn }
                ListElement { text: qsTr("Never show"); val: OverlayMenuSettings.TriggerOff }
            }
        }

        ChoiceRow {
            applicable: overlayMenuCard.menuEnabled
            title: qsTr("Screen edge trigger")
            description: qsTr("Open the menu when the pointer rests on an edge or corner of the stream while the mouse is free.")
            selectedValue: OverlayMenuSettings.edgeTriggerMode
            onValueActivated: function(value) { OverlayMenuSettings.edgeTriggerMode = value }

            model: ListModel {
                ListElement { text: qsTr("Follow the position above (default)"); val: OverlayMenuSettings.TriggerAuto }
                ListElement { text: qsTr("On"); val: OverlayMenuSettings.TriggerOn }
                ListElement { text: qsTr("Off"); val: OverlayMenuSettings.TriggerOff }
            }
        }

        ChoiceRow {
            applicable: overlayMenuCard.menuEnabled &&
                        OverlayMenuSettings.edgeTriggerMode === OverlayMenuSettings.TriggerOn
            title: qsTr("Trigger edge or corner")
            selectedValue: OverlayMenuSettings.edgeZone
            onValueActivated: function(value) { OverlayMenuSettings.edgeZone = value }

            model: ListModel {
                ListElement { text: qsTr("Top edge"); val: OverlayMenuSettings.ZoneTop }
                ListElement { text: qsTr("Bottom edge"); val: OverlayMenuSettings.ZoneBottom }
                ListElement { text: qsTr("Left edge"); val: OverlayMenuSettings.ZoneLeft }
                ListElement { text: qsTr("Right edge"); val: OverlayMenuSettings.ZoneRight }
                ListElement { text: qsTr("Top-left corner"); val: OverlayMenuSettings.ZoneTopLeft }
                ListElement { text: qsTr("Top-right corner"); val: OverlayMenuSettings.ZoneTopRight }
                ListElement { text: qsTr("Bottom-left corner"); val: OverlayMenuSettings.ZoneBottomLeft }
                ListElement { text: qsTr("Bottom-right corner"); val: OverlayMenuSettings.ZoneBottomRight }
            }
        }
        SettingsRow {
            id: overlayMenuEdgeDelayRow
            applicable: overlayMenuCard.menuEnabled && OverlayMenuSettings.edgeTriggerMode === OverlayMenuSettings.TriggerOn
            title: qsTr("Trigger delay")

            Row {
                width: Math.min(360, Math.max(220, overlayMenuEdgeDelayRow.width - Theme.spaceMd * 2))
                height: Math.max(overlayMenuEdgeDelaySlider.implicitHeight, overlayMenuEdgeDelayValue.implicitHeight)
                spacing: Theme.spaceSm

                HardSlider {
                    id: overlayMenuEdgeDelaySlider
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - overlayMenuEdgeDelayValue.width - parent.spacing
                    from: 0
                    to: OverlayMenuSettings.maxEdgeDelayMs
                    stepSize: 100
                    snapMode: Slider.SnapAlways
                    value: OverlayMenuSettings.edgeDelayMs
                    Accessible.name: overlayMenuEdgeDelayRow.title
                    onMoved: OverlayMenuSettings.edgeDelayMs = Math.round(value)
                }

                Text {
                    id: overlayMenuEdgeDelayValue
                    anchors.verticalCenter: parent.verticalCenter
                    width: 64
                    text: Math.round(overlayMenuEdgeDelaySlider.value) + " ms"
                    color: Theme.accent
                    font.family: Theme.fontMono
                    font.pointSize: Theme.fontBody
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignRight
                }
            }
        }

        ToggleRow {
            applicable: overlayMenuCard.menuEnabled
            title: qsTr("Hold the shortcut to peek")
            description: qsTr("Holding the overlay menu shortcut shows the menu until you let go. Use the menu meanwhile and it stays open.")
            checked: OverlayMenuSettings.holdToPeek
            onToggled: function(value) { OverlayMenuSettings.holdToPeek = value }
        }

        ChoiceRow {
            applicable: overlayMenuCard.menuEnabled
            title: qsTr("Esc and gamepad B")
            selectedValue: OverlayMenuSettings.escapeAction
            onValueActivated: function(value) { OverlayMenuSettings.escapeAction = value }

            model: ListModel {
                ListElement { text: qsTr("Go back one level (default)"); val: OverlayMenuSettings.EscapeBack }
                ListElement { text: qsTr("Close the menu"); val: OverlayMenuSettings.EscapeClose }
            }
        }

        ChoiceRow {
            applicable: overlayMenuCard.menuEnabled
            title: qsTr("Gamepad button")
            description: qsTr("Hold it to open the menu while Gamepad Mouse is off.")
            selectedValue: OverlayMenuSettings.gamepadTrigger
            onValueActivated: function(value) { OverlayMenuSettings.gamepadTrigger = value }

            model: ListModel {
                ListElement { text: qsTr("Hold Start (default)"); val: OverlayMenuSettings.GamepadHoldStart }
                ListElement { text: qsTr("Hold Back / Select"); val: OverlayMenuSettings.GamepadHoldBack }
                ListElement { text: qsTr("Off"); val: OverlayMenuSettings.GamepadOff }
            }
        }
    }

    Component.onCompleted: {
        if (StreamingPreferences.captureSysKeysMode !== StreamingPreferences.CSK_OFF) {
            captureSysKeysSelection = StreamingPreferences.captureSysKeysMode
        }

        // Physical haptics needs an OS audio backend for the controller's
        // four-channel endpoint, so it is only compiled in on some platforms.
        if (!SystemProperties.hasPhysicalDualSenseHaptics) {
            for (var i = 0; i < dualSenseHapticsModeListModel.count; i++) {
                if (dualSenseHapticsModeListModel.get(i).val === StreamingPreferences.DSHM_PHYSICAL) {
                    dualSenseHapticsModeListModel.remove(i)
                    break
                }
            }
            dualSenseHapticsRow.syncSelection()
        }
    }
}
