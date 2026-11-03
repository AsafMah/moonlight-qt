import QtQuick
import QtQuick.Controls
import "../../app/gui/settings"
import "../../app/gui/theme"
import "../../app/gui"
import "../../app/gui/settings/SettingsSearchIndex.js" as SearchIndex

// Stand-in for SettingsView: same stacking pattern (pages with a
// searchCategory, zero height when hidden, LegacySettingsPage-style cards that
// carry their own category), the real SettingsScrollArea with its focus-follow
// auto-scroll, and the same key routing as SettingsView. Real pages can't load
// here because they need the app's C++ singletons. The owner API mirrors
// SettingsView; tst_upstream_shell also exercises that actual owner source.
FocusScope {
    id: shell

    width: 900
    height: 560

    property string category: "basic"
    property int escapesReachedShell: 0

    property var categories: [
        { key: "basic", title: "Streaming" },
        { key: "display", title: "Display Settings" },
        { key: "audio", title: "Audio Settings" },
        { key: "input", title: "Input Settings" },
        { key: "about", title: "About" },
        { key: "clipboard", title: "Clipboard" }
    ]

    property alias results: searchControl.resultsView
    property alias field: searchControl.field
    property alias searchControl: searchControl
    property alias scrollArea: scrollArea
    property alias railStandIn: railStandIn
    property alias vrrRow: vrrRow
    property alias resolutionRow: resolutionRow
    property alias resolutionFocusTarget: resolutionFocusTarget
    property alias exclusiveMode: modeRepeater
    property alias aboutCard: aboutCard
    property alias nextCategoryRow: audioRow
    property alias clipboardPage: clipboardPage
    property alias pyrowavePanel: pyrowavePanel

    property string searchQuery: ""
    property bool searchExpanded: false
    property var searchIndex: []
    property var searchResults: []
    property var uncategorized: []
    property Item pendingTarget: null
    property int settleTicks: 0
    property real settleLastY: NaN
    signal searchFocusRequested()

    function rebuildSearchIndex() {
        var context = { categories: categories, currentCategory: category, uncategorized: [] }
        searchIndex = SearchIndex.collect(scrollArea.contentItem, context)
        uncategorized = context.uncategorized
        searchResults = SearchIndex.search(SearchIndex.prepare(searchIndex), searchQuery, 50)
    }
    function openSearch() {
        searchExpanded = true
        searchFocusRequested()
    }
    function clearSearch() { searchQuery = "" }
    function closeSearch() {
        searchExpanded = false
        searchQuery = ""
        pendingTarget = null
    }
    function openSearchResult(result) { SearchIndex.beginOpen(shell, result) }
    onSearchQueryChanged: rebuildSearchIndex()
    onCategoriesChanged: rebuildSearchIndex()

    Timer {
        interval: 16
        repeat: true
        running: shell.pendingTarget !== null
        onTriggered: SearchIndex.settleOpen(shell, scrollArea)
    }

    // Same routing as SettingsView.qml
    Keys.onEscapePressed: function(event) {
        escapesReachedShell++
        railStandIn.forceActiveFocus()
    }
    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_PageUp || event.key === Qt.Key_PageDown) {
            closeSearch()
            var keys = categories.map(function(c) { return c.key })
            var i = keys.indexOf(category) + (event.key === Qt.Key_PageDown ? 1 : -1)
            category = keys[(i + keys.length) % keys.length]
            railStandIn.forceActiveFocus()
            event.accepted = true
        }
        else if (SearchIndex.isTypeToSearch(event)) {
            searchQuery += event.text
            openSearch()
            event.accepted = true
        }
    }

    Column {
        id: railColumn
        width: 220
        spacing: 8

        SettingsSearchControl {
            id: searchControl
            width: parent.width
            page: shell
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
        opacity: SearchIndex.normalize(shell.searchQuery) !== "" ? 0 : 1
        enabled: opacity === 1

        Column {
            id: basicPage
            readonly property string searchCategory: "basic"
            width: parent.width
            visible: shell.category === searchCategory
            height: visible ? implicitHeight : 0

            SettingsCard {
                title: "Video"

                SettingsRow {
                    id: resolutionRow
                    title: "Resolution and FPS"
                    description: "Increasing resolution and frame rate requires more bandwidth"
                    searchKeywords: "fps framerate"
                    searchFocusTarget: resolutionFocusTarget
                    HardSwitch {}
                    HardSwitch { id: resolutionFocusTarget }
                }

                PyrowaveCodecPanel {
                    id: pyrowavePanel
                    applicable: false
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
            readonly property string searchCategory: "display"
            y: basicPage.height
            width: parent.width
            visible: shell.category === searchCategory
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
                    searchKeywords: "vrr gsync freesync adaptive sync"
                }
            }

            SettingsCard {
                title: "Performance overlay"
                searchKeywords: "osd stats"

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
                searchCategory: "audio"
                visible: shell.category === searchCategory && hasVisibleContent
                title: "Audio Settings"

                ToggleRow {
                    id: audioRow
                    title: "Mute host PC speakers while streaming"
                    searchKeywords: "sound"
                }
            }

            SettingsCard {
                searchCategory: "input"
                visible: shell.category === searchCategory && hasVisibleContent
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
                searchCategory: "about"
                visible: shell.category === searchCategory && hasVisibleContent
                title: "About"
                subtitle: "Version and license information"

                Text { text: "Moonlight"; color: "white" }
            }
        }
        ClipboardSettingsPage {
            id: clipboardPage
            readonly property string searchCategory: "clipboard"
            y: basicPage.height + displayPage.height + legacyPage.height
            width: parent.width
            visible: shell.category === searchCategory
            height: visible ? implicitHeight : 0
        }
    }
}
