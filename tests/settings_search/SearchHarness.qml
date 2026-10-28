import QtQuick
import QtQuick.Controls
import "../../app/gui/settings"
import "../../app/gui/theme"

// Stand-in for SettingsView: same stacking pattern (pages with a
// settingsCategory, zero height when hidden, LegacySettingsPage-style cards that
// carry their own category), the real SettingsScrollArea with its focus-follow
// auto-scroll, and the same key routing as SettingsView. Real pages can't load
// here because they need the app's C++ singletons.
FocusScope {
    id: shell

    width: 900
    height: 560

    property string category: "basic"
    property int escapesReachedShell: 0

    readonly property var categories: [
        { key: "basic", title: "Streaming" },
        { key: "display", title: "Display Settings" },
        { key: "audio", title: "Audio Settings" },
        { key: "input", title: "Input Settings" },
        { key: "about", title: "About" }
    ]

    property alias controller: search
    property alias field: field
    property alias results: results
    property alias scrollArea: scrollArea
    property alias railStandIn: railStandIn
    property alias vrrRow: vrrRow
    property alias resolutionRow: resolutionRow
    property alias exclusiveMode: modeRepeater
    property alias aboutCard: aboutCard
    property alias nextCategoryRow: audioRow

    // Same routing as SettingsView.qml
    Keys.onEscapePressed: function(event) {
        escapesReachedShell++
        railStandIn.forceActiveFocus()
    }
    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_PageUp || event.key === Qt.Key_PageDown) {
            search.handlePageKey(event)
            var keys = categories.map(function(c) { return c.key })
            var i = keys.indexOf(category) + (event.key === Qt.Key_PageDown ? 1 : -1)
            category = keys[(i + keys.length) % keys.length]
            railStandIn.forceActiveFocus()
            event.accepted = true
        }
        else if (search.handleTypeToSearch(event)) {
            event.accepted = true
        }
    }

    SettingsSearchController {
        id: search
        categories: shell.categories
        currentCategory: shell.category
        indexRoot: scrollArea.contentItem
        scrollArea: scrollArea
        onCategoryRequested: function(key) { shell.category = key }
        onFocusFieldRequested: field.focusField()
    }

    Column {
        id: railColumn
        width: 220
        spacing: 8

        SettingsSearchField {
            id: field
            width: parent.width
            controller: search
            resultsView: results
        }

        Button {
            id: railStandIn
            text: "rail"
        }
    }

    SettingsScrollArea {
        id: scrollArea

        x: 240
        width: shell.width - x
        height: shell.height
        opacity: search.active ? 0 : 1
        enabled: !search.active

        Column {
            id: basicPage
            readonly property string settingsCategory: "basic"
            width: parent.width
            visible: shell.category === settingsCategory
            height: visible ? implicitHeight : 0

            SettingsCard {
                title: "Video"

                SettingsRow {
                    id: resolutionRow
                    title: "Resolution and FPS"
                    description: "Increasing resolution and frame rate requires more bandwidth"
                    settingsSearchKeywords: "fps framerate"
                    HardSwitch {}
                }

                Repeater {
                    model: 8
                    ToggleRow {
                        title: "Basic filler " + index
                        description: "A long wrapped description that makes this row tall enough to require "
                                     + "scrolling in the harness, the same way real settings descriptions wrap."
                    }
                }
            }
        }

        Column {
            id: displayPage
            readonly property string settingsCategory: "display"
            y: basicPage.height
            width: parent.width
            visible: shell.category === settingsCategory
            height: visible ? implicitHeight : 0

            SettingsCard {
                title: "Display"

                Repeater {
                    model: 10
                    ToggleRow {
                        title: "Display filler " + index
                        description: "Wrapped filler text that pushes the target well below the first screen, "
                                     + "so reaching it needs a real scroll after the category switch."
                    }
                }

                ToggleRow {
                    id: vrrRow
                    title: "Variable refresh rate (G-Sync / FreeSync)"
                    description: "Shows each frame as soon as it arrives."
                    settingsSearchKeywords: "vrr gsync freesync adaptive sync"
                }
            }

            SettingsCard {
                title: "Performance overlay"
                settingsSearchKeywords: "osd stats"

                Repeater {
                    model: ["Frame time", "Decoder latency"]
                    ToggleRow { title: modelData }
                }

                ToggleRow {
                    title: "Hidden frame option"
                    applicable: false
                }
            }
        }

        // LegacySettingsPage pattern: one page, cards of several categories
        Column {
            id: legacyPage
            y: basicPage.height + displayPage.height
            width: parent.width

            SettingsCard {
                settingsCategory: "audio"
                visible: shell.category === settingsCategory && hasVisibleContent
                title: "Audio Settings"

                ToggleRow {
                    id: audioRow
                    title: "Mute host PC speakers while streaming"
                    settingsSearchKeywords: "sound"
                }
            }

            SettingsCard {
                settingsCategory: "input"
                visible: shell.category === settingsCategory && hasVisibleContent
                title: "Screen Combination Mode"

                // Custom composite like ScreenCombinationModeSelector: each
                // option is its own searchable item
                Item {
                    width: parent.width
                    height: 60

                    Row {
                        spacing: 8
                        Repeater {
                            id: modeRepeater
                            model: ["Follow host", "Exclusive display streaming mode"]
                            AbstractButton {
                                property string settingsSearchTitle: modelData
                                property string settingsSearchDescription: "Display layout option"
                                activeFocusOnTab: true
                                width: 200
                                height: 50
                                background: Rectangle { color: parent.activeFocus ? "teal" : "gray" }
                            }
                        }
                    }
                }
            }

            SettingsCard {
                id: aboutCard
                settingsCategory: "about"
                visible: shell.category === settingsCategory && hasVisibleContent
                title: "About"
                subtitle: "Version and license information"

                Text { text: "Moonlight"; color: "white" }
            }
        }
    }

    SettingsSearchResults {
        id: results
        x: scrollArea.x
        width: scrollArea.width
        height: scrollArea.height
        controller: search
        searchField: field
    }
}
