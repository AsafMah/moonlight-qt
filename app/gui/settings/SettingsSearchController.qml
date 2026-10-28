import QtQuick 2.9
import "SettingsSearchIndex.js" as SearchIndex

// Non-visual core of the settings search: query, results, and the navigation
// transaction (switch category, wait for layout to settle, scroll, focus,
// highlight). SettingsSearchField and SettingsSearchResults are thin views on
// top of it; SettingsView owns one and routes page-level keys through it.
Item {
    id: controller

    // [{ key, title }] in rail order — only these categories produce results
    property var categories: []
    property string currentCategory: ""
    // Root of the live settings tree (the scroll area's contentItem)
    property Item indexRoot: null
    // SettingsScrollArea that hosts indexRoot
    property Item scrollArea: null
    property int limit: 50

    property string query: ""
    readonly property bool active: SearchIndex.normalize(query) !== ""
    property var results: []
    // Searchable items that have no category (should be empty; tests check it)
    property var uncategorized: []

    // Set while a result is being opened, until it is focused
    property Item pendingTarget: null

    signal categoryRequested(string key)
    signal focusFieldRequested()
    signal navigated(Item target, Item focusItem)

    visible: false

    function refresh() {
        // Not `active`: inside onQueryChanged that binding may not have
        // re-evaluated yet.
        if (SearchIndex.normalize(query) === "" || !indexRoot) {
            results = []
            return
        }
        // Rebuilt on every edit: a few hundred items, and titles/descriptions/
        // availability are always current (language, Repeater models, toggles).
        var context = {
            categories: categories,
            currentCategory: currentCategory,
            uncategorized: []
        }
        var entries = SearchIndex.collect(indexRoot, context)
        uncategorized = context.uncategorized
        results = SearchIndex.search(SearchIndex.prepare(entries), query, limit)
    }

    onQueryChanged: refresh()
    onCategoriesChanged: refresh()

    function clear() {
        query = ""
    }

    function open(result) {
        if (!result || !result.source || !result.source.item) {
            return
        }
        var target = result.source.item
        clear()
        pendingTarget = target
        settle.ticks = 0
        settle.lastY = NaN
        if (result.categoryKey !== currentCategory) {
            categoryRequested(result.categoryKey)
        }
        settle.restart()
    }

    function finishOpen(target) {
        pendingTarget = null
        var focusItem = SearchIndex.focusTargetFor(target)
        if (scrollArea) {
            scrollArea.revealItem(target, focusItem)
            scrollArea.suppressAutoScroll = true
        }
        if (focusItem) {
            focusItem.forceActiveFocus(Qt.TabFocusReason)
        }
        else {
            target.forceActiveFocus(Qt.TabFocusReason)
        }
        if (scrollArea) {
            scrollArea.suppressAutoScroll = false
            scrollArea.flashItem(target)
        }
        navigated(target, focusItem)
    }

    // Category switches change visible/height bindings at once, but Column
    // positioners, Layouts and wrapped text settle during polish. Wait until the
    // target is laid out and its position is stable across two frames.
    Timer {
        id: settle

        property int ticks: 0
        property real lastY: NaN

        interval: 16
        repeat: true
        onTriggered: {
            var target = controller.pendingTarget
            if (!target) {
                stop()
                return
            }
            ticks++
            if (target.visible && target.height > 0 && controller.scrollArea) {
                var y = target.mapToItem(controller.scrollArea.contentItem, 0, 0).y
                if (y === lastY) {
                    stop()
                    controller.finishOpen(target)
                    return
                }
                lastY = y
            }
            if (ticks >= 60) {
                // Never became visible (e.g. hidden by a condition we can't see
                // from search). Give the field back instead of leaving focus lost.
                stop()
                controller.pendingTarget = null
                controller.focusFieldRequested()
            }
        }
    }

    // LB/RB (PageUp/PageDown) leave search mode before switching category, so
    // the results overlay never sits on top of a different page.
    function handlePageKey(event) {
        if (active) {
            clear()
        }
    }

    // Typing a printable character while focus is elsewhere on the settings
    // page starts a search with it.
    function handleTypeToSearch(event) {
        if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)) {
            return false
        }
        var t = event.text
        if (!t || t.length !== 1 || t.trim() === "" || t.charCodeAt(0) < 32 || t.charCodeAt(0) === 127) {
            return false
        }
        query = query + t
        focusFieldRequested()
        return true
    }
}
