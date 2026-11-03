import QtQuick
import QtTest

// Run the actual SettingsView owner and CategoryRail. Only C++-dependent
// content pages are replaced; their real Clipboard/PyroWave controls have
// separate integration coverage in SearchHarness.
TestCase {
    id: testCase
    name: "UpstreamSettingsShell"
    visible: true
    width: 900
    height: 650
    when: windowShown
    property var shell
    property url backgroundImageUrl: ""

    function init() {
        var url = Qt.resolvedUrl("../../app/gui/SettingsView.qml")
        var request = new XMLHttpRequest()
        request.open("GET", url, false)
        request.send()
        var source = request.responseText
        verify(source.indexOf("SettingsSearchControl {") >= 0)
        source = source.replace(/\b(?:Basic|Display|Legacy|Ecosystem|About|Peripherals|Clipboard)SettingsPage\s*\{/g,
                                "FixturePage {")
        var fixture = `
            component FixturePage: Column {
                id: fixturePage
                property string category: ""
                signal languageChanged()
                signal aboutRequested()
                signal scrollToEndRequested()
                SettingsCard {
                    title: "Fixture section"
                    Repeater {
                        model: 12
                        ToggleRow { title: "Filler " + index }
                    }
                    ToggleRow {
                        objectName: "fixtureTarget"
                        title: "Fixture " + (fixturePage.searchCategory || fixturePage.category)
                        searchKeywords: "integration exact target"
                    }
                }
            }
        `
        var end = source.lastIndexOf("}")
        source = source.slice(0, end) + fixture + source.slice(end)
        shell = createTemporaryQmlObject(source, testCase, String(url))
        verify(shell)
        shell.width = width
        shell.height = height
        waitForRendering(shell)
    }

    function test_actualOwnerUsesUpstreamControlAndGenericTreeNavigation_data() {
        return [{ tag: "wide", viewportWidth: 900 }, { tag: "compact", viewportWidth: 520 }]
    }

    function test_actualOwnerUsesUpstreamControlAndGenericTreeNavigation(data) {
        shell.width = data.viewportWidth
        waitForRendering(shell)
        shell.openSearch()
        var field = findChild(shell, "settingsSearchField")
        verify(field)
        verify(field.activeFocus, "the upstream control receives owner focus requests")
        shell.searchQuery = "exact clipboard"
        compare(shell.searchResults.length, 1)
        var result = shell.searchResults[0]
        compare(result.categoryKey, "clipboard", "generic collector covers additional page roots")
        keyClick(Qt.Key_Return)
        compare(shell.category, "clipboard")
        compare(shell.searchQuery, "")
        compare(shell.searchExpanded, false)
        var target = result.source.item
        tryVerify(function() {
            var item = testCase.Window.window.activeFocusItem
            while (item) {
                if (item === target) return true
                item = item.parent
            }
            return false
        }, 2000)
        var area = target
        while (area && area.highlightTarget === undefined) area = area.parent
        verify(area)
        compare(area.highlightTarget, target)
        var y = target.mapToItem(area.contentItem, 0, 0).y
        verify(y >= area.contentY - 1 && y + target.height <= area.contentY + area.height + 1)
    }

    function test_actualOwnerTypeAnywhereEscapeAndShoulders() {
        shell.forceActiveFocus()
        keyClick("v")
        compare(shell.searchQuery, "v")
        var field = findChild(shell, "settingsSearchField")
        verify(field.activeFocus)
        keyClick(Qt.Key_Escape)
        compare(shell.searchQuery, "")
        verify(field.activeFocus)
        keyClick(Qt.Key_Escape)
        compare(shell.searchExpanded, false)
        verify(!field.activeFocus)
        shell.openSearch()
        shell.searchQuery = "fixture"
        keyClick(Qt.Key_PageDown)
        compare(shell.searchQuery, "")
        compare(shell.searchExpanded, false)
        compare(shell.category, "display")
    }
}
