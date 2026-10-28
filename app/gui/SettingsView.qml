import QtQuick 2.9
import QtQuick.Controls
import QtQuick.Window 2.2

import StreamingPreferences 1.0
import SdlGamepadKeyNavigation 1.0
import SystemProperties 1.0

import "settings"
import "theme"

// 设置页外壳：左侧分类 rail + 右侧卡片内容。
// 「基本设置」「显示」使用独立页面；其余 6 组保留在 LegacySettingsPage.qml
// 中以维持翻译上下文，但内部也已经迁移到卡片和设置行。
// 根用 FocusScope 而不是 Item：工具栏的 Keys.onDownPressed 走的是
// stackView.currentItem.forceActiveFocus()，落在普通 Item 上会停在一个看不见的
// 死点上（PcView / AppView 是 GridView + activeFocusOnTab，所以没这问题）。
// FocusScope 会把焦点转交给内部真正持焦的控件。
FocusScope {
    id: settingsPage
    // 这一页自带壁纸，main.qml 不用再垫一层
    readonly property bool usesOwnBackground: true
    objectName: qsTr("Settings")

    signal languageChanged()

    // 窄窗口时 rail 折叠成顶部横向 tab 条
    readonly property bool compact: width < Theme.compactBreakpoint

    property string category: "basic"

    // 图标取自 Microsoft Fluent UI System Icons（MIT），和 FluentWinUI3 是同一套设计语言。
    // 之前用 emoji，各平台字体不同，渲染出来大小、粗细、配色都对不齐。
    readonly property var rawCategories: [
        { key: "basic",    icon: "qrc:/res/fluent/cat-basic.svg",    title: qsTr("Streaming") },
        { key: "display",  icon: "qrc:/res/fluent/cat-display.svg",  title: qsTr("Display Settings") },
        { key: "audio",    icon: "qrc:/res/fluent/cat-audio.svg",    title: qsTr("Audio Settings") },
        { key: "host",     icon: "qrc:/res/fluent/cat-host.svg",     title: qsTr("Host Settings") },
        { key: "input",    icon: "qrc:/res/fluent/cat-input.svg",    title: qsTr("Input Settings") },
        { key: "gamepad",  icon: "qrc:/res/fluent/cat-gamepad.svg",  title: qsTr("Gamepad Settings") },
        { key: "peripherals", icon: "qrc:/res/fluent/cat-peripherals.svg", title: qsTr("Peripherals Settings") },
        { key: "advanced", icon: "qrc:/res/fluent/cat-advanced.svg", title: qsTr("Compatibility and diagnostics") },
        { key: "ui",       icon: "qrc:/res/fluent/cat-ui.svg",       title: qsTr("Interface") },
        { key: "ecosystem",icon: "qrc:/res/fluent/cat-ecosystem.svg",title: qsTr("AlkaidLab Ecosystem") },
        { key: "about",    icon: "qrc:/res/fluent/cat-about.svg",    title: qsTr("About") }
    ]

    // USB 设备转发只在有本地 USB/IP 后端的平台提供（Windows/macOS）；
    // 没有的平台连分类一起隐藏，避免出现空页。
    readonly property var categories: rawCategories.filter(
        function(c) {
            return c.key !== "peripherals" || SystemProperties.usbForwardingAvailable
        })

    StackView.onActivated: {
        // This enables Tab and BackTab based navigation rather than arrow keys.
        // It is required to shift focus between controls on the settings page.
        SdlGamepadKeyNavigation.setUiNavMode(true)

        // 手柄进来时把焦点放在分类栏的当前分类上，而不是内容区第一个控件。
        //
        // 以前是直接点基本设置页的分辨率下拉，于是用户进设置的第一下左右输入就把
        // 分辨率改了（issue #144）。落在分类栏上就没这个问题：分类栏不吃左右键，
        // 而且 category 是跨次保留的，从分类栏出发永远是可见、可用的那一项。
        if (SdlGamepadKeyNavigation.getConnectedGamepads() > 0) {
            rail.focusCurrent()
        }
    }

    // 焦点在内容区时，B / Esc 先退回分类栏；已经在分类栏了才放行给 main.qml
    // 去弹出整个设置页。之前不分级，手柄用户在内容区随手一个 B 就整页退出了。
    // 搜索框里有内容时 Esc 由搜索框自己吃掉（清空），不会走到这里。
    Keys.onEscapePressed: function(event) {
        event.accepted = !rail.railFocused
        if (event.accepted) {
            rail.focusCurrent()
        }
    }

    // 把焦点交给内容区的第一个可聚焦控件。
    //
    // 不直接引用某个页面的首个控件：那只对基本设置页有效，其余六组还在
    // LegacySettingsPage 里，而且随分类切换。scrollArea 在声明顺序上排在分类栏
    // 之后，往后走一格 Tab 就是它内部第一个可聚焦控件 —— 焦点链本身会跳过
    // 不可见的分类。
    function focusContent() {
        var first = scrollArea.nextItemInFocusChain(true)
        if (first) {
            first.forceActiveFocus(Qt.TabFocusReason)
        }
    }

    StackView.onDeactivating: {
        SdlGamepadKeyNavigation.setUiNavMode(false)

        // Save the prefs so the Session can observe the changes
        StreamingPreferences.save()
    }

    Component.onDestruction: {
        // Also save preferences on destruction, since we won't get a
        // deactivating callback if the user just closes Moonlight
        StreamingPreferences.save()
    }

    // 焦点落到 FocusScope 壳自己身上时（工具栏按向下、或 StackView 切页回来），
    // 转交给分类栏。停在壳上是个看不见的死点，用户得多按一次才有反应。
    onActiveFocusChanged: {
        if (activeFocus && Window.window && Window.window.activeFocusItem === settingsPage) {
            rail.focusCurrent()
        }
    }

    // 手柄 LB/RB 映射成 PageUp/PageDown，用来切分类
    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_PageUp) {
            // 搜索中切分类先退出搜索，结果列表不能盖在另一个分类的页面上
            search.handlePageKey(event)
            rail.step(-1)
            // 切完分类要把焦点收回分类栏：原来持焦的控件已经随着旧分类隐藏了，
            // 焦点会凭空消失，手柄看起来就像失灵。
            rail.focusCurrent()
            event.accepted = true
        }
        else if (event.key === Qt.Key_PageDown) {
            search.handlePageKey(event)
            rail.step(1)
            rail.focusCurrent()
            event.accepted = true
        }
        // 焦点不在输入框时直接打字，等于在设置搜索里输入
        else if (search.handleTypeToSearch(event)) {
            event.accepted = true
        }
    }

    // 设置搜索。索引不是手写清单，而是每次输入时从 scrollArea 里的真实控件现采：
    // SettingsRow / SettingsCard 自带标题、说明和 settingsSearchKeywords，分类取
    // 最近的 settingsCategory 祖先。新增分区只要用这些组件、放在带 settingsCategory
    // 的页面或卡片里就能被搜到；tests/settings_search 里的门禁会检查这一点。
    SettingsSearchController {
        id: search
        categories: settingsPage.categories
        currentCategory: settingsPage.category
        indexRoot: scrollArea.contentItem
        scrollArea: scrollArea
        onCategoryRequested: function(key) {
            settingsPage.category = key
        }
        onFocusFieldRequested: searchField.focusField()
    }

    Shortcut {
        sequences: [StandardKey.Find]
        enabled: settingsPage.StackView.status === StackView.Active
        onActivated: searchField.focusField()
    }

    // Reuse the background already owned by the application window. Creating a
    // hidden PcView here duplicated its model, network work, and scene graph.
    Image {
        anchors.fill: parent
        visible: StreamingPreferences.backgroundSource !== StreamingPreferences.BGS_NONE
        source: Window.window && Window.window.backgroundImageUrl !== ""
                ? Window.window.backgroundImageUrl
                : "qrc:/res/gura.png"
        fillMode: Image.PreserveAspectCrop
        z: -2
    }

    Rectangle {
        anchors.fill: parent
        visible: StreamingPreferences.backgroundSource !== StreamingPreferences.BGS_NONE
        color: Qt.rgba(Theme.ink.r, Theme.ink.g, Theme.ink.b,
                       StreamingPreferences.backgroundOverlayOpacity / 100.0)
        z: -1
    }

    Item {
        id: body

        anchors {
            fill: parent
            // 让出顶部工具栏（56）+ 一格间距
            topMargin: 72
            leftMargin: Theme.spaceLg
            rightMargin: Theme.spaceLg
            bottomMargin: Theme.spaceSm
        }

        Rectangle {
            id: railBackground

            anchors {
                left: parent.left
                top: parent.top
            }
            width: settingsPage.compact ? body.width : Theme.railWidth
            height: settingsPage.compact
                    ? 52 + searchField.height + Theme.spaceXs
                    : body.height

            // 分类栏底板。方角 + 1px 描边，不用硬投影：它贴着窗口左边，
            // 投影只会在右侧和内容区挤在一起。
            radius: 0
            color: Theme.surfaceLayer
            border.width: 1
            border.color: Theme.line

            // 搜索框放在分类栏顶上：窄窗口时分类栏变成横向 tab 条，搜索框跟着变成
            // 整行宽，仍在 tab 条上方。
            SettingsSearchField {
                id: searchField
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    margins: Theme.spaceXs
                }
                controller: search
                resultsView: searchResults
            }

            CategoryRail {
                id: rail
                anchors {
                    left: parent.left
                    right: parent.right
                    top: searchField.bottom
                    bottom: parent.bottom
                    margins: Theme.spaceXs
                }
                compact: settingsPage.compact
                categories: settingsPage.categories
                currentCategory: settingsPage.category
                onCategoryPicked: function(category) {
                    settingsPage.category = category
                    scrollArea.contentY = 0
                }
                onContentRequested: settingsPage.focusContent()
            }
        }

        SettingsScrollArea {
            id: scrollArea

            // 搜索时内容区让给结果列表。用 opacity 而不是 visible：搜索要读当前分类里控件的
            // 真实 visible 来判断它们此刻是否可用，整块隐藏会让它们全变成不可见。
            opacity: search.active ? 0 : 1
            enabled: !search.active

            anchors {
                top: settingsPage.compact ? railBackground.bottom : parent.top
                topMargin: settingsPage.compact ? Theme.spaceMd : 0
                left: settingsPage.compact ? parent.left : railBackground.right
                leftMargin: settingsPage.compact ? 0 : Theme.spaceLg
                right: parent.right
                bottom: parent.bottom
            }

            BasicSettingsPage {
                id: basicPage
                width: parent.width
                readonly property string settingsCategory: "basic"
                visible: settingsPage.category === settingsCategory
                height: visible ? implicitHeight : 0
            }

            DisplaySettingsPage {
                id: displayPage
                y: basicPage.height
                width: parent.width
                readonly property string settingsCategory: "display"
                visible: settingsPage.category === settingsCategory
                height: visible ? implicitHeight : 0
            }

            LegacySettingsPage {
                id: legacyPage
                // 已迁移的新页面都在上面按顺序堆着，隐藏时高度为 0，所以这里累加即可
                y: basicPage.height + displayPage.height
                width: parent.width
                category: settingsPage.category

                onLanguageChanged: settingsPage.languageChanged()
            }

            EcosystemSettingsPage {
                id: ecosystemPage
                y: basicPage.height + displayPage.height + legacyPage.height
                width: parent.width
                readonly property string settingsCategory: "ecosystem"
                visible: settingsPage.category === settingsCategory
                height: visible ? implicitHeight : 0
                onAboutRequested: {
                    settingsPage.category = "about"
                    rail.focusCurrent()
                    scrollArea.contentY = 0
                }
            }

            AboutSettingsPage {
                id: aboutPage
                y: basicPage.height + displayPage.height + legacyPage.height + ecosystemPage.height
                width: parent.width
                readonly property string settingsCategory: "about"
                visible: settingsPage.category === settingsCategory
                height: visible ? implicitHeight : 0
                onScrollToEndRequested: scrollArea.scrollToEnd()
            }

            PeripheralsSettingsPage {
                id: peripheralsPage
                y: basicPage.height + displayPage.height + legacyPage.height
                   + ecosystemPage.height + aboutPage.height
                width: parent.width
                readonly property string settingsCategory: "peripherals"
                visible: settingsPage.category === settingsCategory
                height: visible ? implicitHeight : 0
            }
        }

        SettingsSearchResults {
            id: searchResults
            anchors.fill: scrollArea
            controller: search
            searchField: searchField
        }
    }

    Component.onCompleted: {
        // 语言切换需要重建若干下拉的模型
        settingsPage.languageChanged.connect(basicPage.languageChanged)
        settingsPage.languageChanged.connect(displayPage.languageChanged)
    }
}
