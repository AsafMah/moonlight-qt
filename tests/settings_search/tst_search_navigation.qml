import QtQuick
import QtTest

// End-to-end UX of the settings search against real settings components
// (SettingsRow/ToggleRow/SettingsCard/SettingsScrollArea) in SearchHarness.qml:
// index collection, keyboard & gamepad-key flows, category switch, scroll,
// focus, highlight and Esc semantics.
TestCase {
    id: testCase
    name: "SettingsSearchNavigation"
    width: 900
    height: 560
    // TestCase is invisible by default; the harness needs real visibility
    visible: true
    when: windowShown

    FontLoader { source: "../../app/res/fonts/Manrope-Regular.ttf" }

    Component {
        id: harnessComponent
        SearchHarness {}
    }

    property var shell: null

    function init() {
        shell = createTemporaryObject(harnessComponent, testCase)
        verify(shell)
        shell.forceActiveFocus()
        waitForRendering(shell)
    }

    function typeText(text) {
        for (var i = 0; i < text.length; i++) {
            keyClick(text[i])
        }
    }

    function resultTitles() {
        return shell.searchResults.map(function(r) { return r.title })
    }

    function isInside(item, ancestor) {
        while (item) {
            if (item === ancestor) {
                return true
            }
            item = item.parent
        }
        return false
    }

    function inViewport(item) {
        var area = shell.scrollArea
        var pos = item.mapToItem(area.contentItem, 0, 0)
        return pos.y >= area.contentY - 1 && pos.y + item.height <= area.contentY + area.height + 1
    }

    function focusedItem() {
        return testCase.Window.window.activeFocusItem
    }

    // ---- index collection ----------------------------------------------

    function test_collectsRowsRepeaterRowsAndCustomMarkers() {
        shell.searchQuery = "frame"
        var titles = resultTitles()
        verify(titles.indexOf("Frame time") >= 0, "Repeater delegate rows are indexed")
        verify(titles.indexOf("Hidden frame option") < 0, "non-applicable rows are excluded")

        var frameTime = shell.searchResults[titles.indexOf("Frame time")]
        compare(frameTime.categoryKey, "display")
        compare(frameTime.categoryTitle, "Display Settings")
        compare(frameTime.section, "Performance overlay")

        shell.searchQuery = "exclusive"
        compare(resultTitles()[0], "Exclusive display streaming mode")
        compare(shell.searchResults[0].categoryKey, "input")
        compare(shell.searchResults[0].section, "Screen Combination Mode")
        compare(shell.uncategorized.length, 0)
    }

    function test_cardIsOwnResultOnlyWithoutSearchableRows() {
        shell.searchQuery = "about"
        verify(resultTitles().indexOf("About") >= 0, "card without rows is a result")
        shell.searchQuery = "audio settings"
        verify(resultTitles().indexOf("Audio Settings") < 0, "card with rows is only a section")
        verify(resultTitles().indexOf("Mute host PC speakers while streaming") >= 0)
    }

    function test_cardKeywordsApplyToTheirRows() {
        shell.searchQuery = "osd"
        compare(resultTitles().sort(), ["Decoder latency", "Frame time"])
    }

    function test_unknownCategoryIsNotSearchable() {
        // Category filtered out of the rail (like Peripherals without USB/IP)
        shell.categories = shell.categories.filter(function(c) { return c.key !== "audio" })
        shell.searchQuery = "mute"
        compare(shell.searchResults.length, 0)
    }

    // ---- keyboard flow: type, Down, Enter --------------------------------

    function test_typeArrowEnterNavigatesScrollsFocusesHighlights() {
        shell.field.focusField()
        typeText("vrr")
        compare(shell.searchQuery, "vrr")
        tryCompare(shell.results, "visible", true)
        compare(shell.scrollArea.opacity, 0)
        compare(resultTitles()[0], "Variable refresh rate (G-Sync / FreeSync)")

        keyClick(Qt.Key_Down)
        verify(shell.results.listFocused, "Down moves into the results")
        compare(shell.results.currentIndex, 0)

        keyClick(Qt.Key_Return)
        compare(shell.category, "display")
        compare(shell.searchQuery, "", "query cleared after opening")
        compare(shell.results.visible, false)

        tryVerify(function() { return isInside(focusedItem(), shell.vrrRow) }, 2000,
                  "focus lands on the target row's control")
        verify(focusedItem() !== shell.vrrRow, "the switch itself has focus, not just the row scope")
        verify(shell.scrollArea.contentY > 0, "target needed a scroll")
        verify(inViewport(shell.vrrRow), "target scrolled into view")
        compare(shell.scrollArea.highlightTarget, shell.vrrRow)
        verify(shell.scrollArea.highlightOpacity > 0, "target highlighted")

        // The highlight is brief, and the scroll position survives the focus
        // change (no competing auto-scroll pulls the row away)
        var y = shell.scrollArea.contentY
        tryCompare(shell.scrollArea, "highlightOpacity", 0, 3000)
        compare(shell.scrollArea.contentY, y)
        verify(inViewport(shell.vrrRow))
    }

    function test_enterInFieldOpensTopResult() {
        shell.field.focusField()
        typeText("exclusive")
        keyClick(Qt.Key_Return)
        compare(shell.category, "input")
        var option = shell.exclusiveMode.itemAt(1)
        tryCompare(option, "activeFocus", true)
        tryVerify(function() { return shell.scrollArea.highlightTarget === option })
    }

    function test_rowWithoutControlFocusesFirstFocusableOrRow() {
        shell.field.focusField()
        typeText("about")
        keyClick(Qt.Key_Return)
        compare(shell.category, "about")
        tryVerify(function() { return isInside(focusedItem(), shell.aboutCard) }, 2000)
        tryVerify(function() { return shell.scrollArea.highlightTarget === shell.aboutCard })
    }

    function test_openInCurrentCategoryStillScrolls() {
        shell.category = "display"
        waitForRendering(shell)
        compare(shell.scrollArea.contentY, 0)
        shell.field.focusField()
        typeText("freesync")
        keyClick(Qt.Key_Return)
        tryVerify(function() { return isInside(focusedItem(), shell.vrrRow) }, 2000)
        verify(inViewport(shell.vrrRow))
    }

    function test_mouseClickOpensResult() {
        shell.field.focusField()
        typeText("fps")
        var delegate = shell.results.itemAtIndex(0)
        verify(delegate)
        mouseClick(delegate)
        compare(shell.category, "basic")
        tryVerify(function() { return isInside(focusedItem(), shell.resolutionRow) }, 2000)
        verify(focusedItem() === shell.resolutionFocusTarget, "upstream explicit focus metadata wins over tree order")
    }

    // ---- Esc semantics ---------------------------------------------------

    function test_escInFieldClearsThenPropagates() {
        shell.field.focusField()
        typeText("fps")
        keyClick(Qt.Key_Escape)
        compare(shell.searchQuery, "")
        compare(shell.escapesReachedShell, 0, "first Esc only clears")
        verify(shell.field.activeFocus)

        keyClick(Qt.Key_Escape)
        compare(shell.escapesReachedShell, 1, "Esc on an empty field goes back like before")
        verify(shell.railStandIn.activeFocus)
    }

    function test_escInResultsClearsAndReturnsToField() {
        shell.field.focusField()
        typeText("frame")
        keyClick(Qt.Key_Down)
        verify(shell.results.listFocused)
        keyClick(Qt.Key_Escape)
        compare(shell.searchQuery, "")
        compare(shell.escapesReachedShell, 0)
        tryVerify(function() { return shell.field.activeFocus })
    }

    // ---- gamepad (UI-nav mode sends Tab/Backtab, Space for A) -------------

    function test_gamepadTabBacktabAndA() {
        shell.field.focusField()
        typeText("frame")
        verify(shell.searchResults.length >= 2)
        keyClick(Qt.Key_Tab)
        verify(shell.results.listFocused)
        compare(shell.results.currentIndex, 0)
        keyClick(Qt.Key_Tab)
        compare(shell.results.currentIndex, 1)
        keyClick(Qt.Key_Backtab, Qt.ShiftModifier)
        compare(shell.results.currentIndex, 0)
        keyClick(Qt.Key_Backtab, Qt.ShiftModifier)
        verify(shell.field.activeFocus, "up from the first result returns to the field")

        keyClick(Qt.Key_Tab)
        keyClick(Qt.Key_Tab)
        var expected = shell.searchResults[1].source.item
        keyClick(Qt.Key_Space)
        tryVerify(function() { return isInside(focusedItem(), expected) }, 2000)
    }

    function test_shoulderButtonsLeaveSearch() {
        shell.field.focusField()
        typeText("vrr")
        keyClick(Qt.Key_Down)
        keyClick(Qt.Key_PageDown)
        compare(shell.searchQuery, "")
        compare(shell.category, "display")
        compare(shell.scrollArea.opacity, 1)
    }

    // ---- type to search, empty state --------------------------------------

    function test_typingElsewhereStartsSearch() {
        shell.railStandIn.forceActiveFocus()
        keyClick("h")
        keyClick("d")
        compare(shell.searchQuery, "hd")
        verify(shell.field.activeFocus)
    }

    function test_noMatchesShowsEmptyState() {
        shell.field.focusField()
        typeText("zzzq")
        compare(shell.searchResults.length, 0)
        verify(shell.results.visible)
        verify(shell.results.emptyStateVisible)
        keyClick(Qt.Key_Down)
        verify(shell.field.activeFocus, "nothing to move to")
    }

    function test_upstreamControlOwnsOnlySearchUi() {
        compare(shell.searchControl.page, shell)
        compare(shell.searchControl.field, shell.field)
        compare(shell.field.objectName, "settingsSearchField")
        compare(shell.searchControl.resultsView, shell.results)
        shell.searchControl.focusField()
        verify(shell.searchExpanded)
        typeText("frame time")
        compare(shell.searchQuery, "frame time")
        compare(resultTitles()[0], "Frame time")
        compare(shell.searchResults.length, 1, "all query tokens must match")
    }

    function test_realForkClipboardAndPyrowaveCoverageAndApplicability() {
        shell.field.focusField()
        typeText("clipboard")
        compare(shell.searchResults.length, 6, "every real clipboard row inherits card keywords")
        var result = shell.searchResults.filter(function(r) { return r.title === "Drop files onto the stream" })[0]
        verify(result)
        compare(result.categoryKey, "clipboard")
        shell.openSearchResult(result)
        tryVerify(function() { return isInside(focusedItem(), result.source.item) }, 2000)
        verify(inViewport(result.source.item))
        compare(shell.scrollArea.highlightTarget, result.source.item)

        shell.category = "display"
        shell.searchQuery = "wavelet"
        compare(shell.searchResults.length, 0, "non-applicable real panel is excluded on another page")
        shell.pyrowavePanel.applicable = true
        shell.rebuildSearchIndex()
        compare(shell.searchResults.length, 1)
        compare(shell.searchResults[0].title, "PyroWave")
        compare(shell.searchResults[0].categoryKey, "basic")
        shell.openSearchResult(shell.searchResults[0])
        tryVerify(function() { return isInside(focusedItem(), shell.pyrowavePanel) }, 2000)
        compare(shell.scrollArea.highlightTarget, shell.pyrowavePanel)
        shell.pyrowavePanel.applicable = false
        shell.searchQuery = "wavelet"
        compare(shell.searchResults.length, 0, "applicability updates are read live")
    }
}
