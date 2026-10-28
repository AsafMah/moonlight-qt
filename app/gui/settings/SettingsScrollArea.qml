import QtQuick 2.9
import QtQuick.Controls
import QtQuick.Window 2.2
import "../theme"

// 内容滚动区。搬运了旧 SettingsView 的焦点自动滚动逻辑，
// 手柄按 Tab 走到屏幕外的控件时把它滚进视野。
Flickable {
    id: area

    default property alias areaContent: contentHolder.data

    boundsBehavior: Flickable.OvershootBounds
    contentWidth: width
    contentHeight: contentHolder.childrenRect.height + Theme.spaceXl
    clip: true

    ScrollBar.vertical: ScrollBar {
        policy: area.contentHeight > area.height ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
    }

    Item {
        id: contentHolder
        width: area.width
        height: childrenRect.height
    }

    NumberAnimation on contentY {
        id: autoScrollAnimation
        duration: 100
    }

    function isChildOfFlickable(item) {
        while (item) {
            if (item.parent === contentItem) {
                return true
            }

            item = item.parent
        }
        return false
    }

    // 供页面内的快捷入口直接定位到内容末尾。目标位置由滚动区统一计算，
    // 页面组件不需要知道外层 Flickable 的可滚动范围。
    function scrollToEnd() {
        autoScrollAnimation.stop()
        autoScrollAnimation.from = contentY
        autoScrollAnimation.to = Math.max(0, contentHeight - height)
        autoScrollAnimation.start()
    }

    // Set while a caller positions the view itself (settings search), so the
    // focus-follow scroll below doesn't start a competing animation.
    property bool suppressAutoScroll: false

    // Scrolls `item` into view without animation: centered when it fits,
    // otherwise top-aligned. If `focusItem` would still be off-screen, center
    // on that instead.
    function revealItem(item, focusItem) {
        autoScrollAnimation.stop()
        var margin = height > 100 ? 50 : 0
        var pos = item.mapToItem(contentItem, 0, 0)
        var to = item.height + margin * 2 <= height
                ? pos.y - (height - item.height) / 2
                : pos.y - margin
        if (focusItem && focusItem !== item) {
            var fpos = focusItem.mapToItem(contentItem, 0, 0)
            if (fpos.y < to + margin || fpos.y + focusItem.height > to + height - margin) {
                to = fpos.y - (height - focusItem.height) / 2
            }
        }
        contentY = Math.max(0, Math.min(to, Math.max(0, contentHeight - height)))
    }

    readonly property Item highlightTarget: searchHighlight.target
    readonly property real highlightOpacity: searchHighlight.opacity

    // Briefly outlines `item`, e.g. a setting reached from search.
    function flashItem(item) {
        var pos = item.mapToItem(contentItem, 0, 0)
        searchHighlight.target = item
        searchHighlight.x = pos.x
        searchHighlight.y = pos.y
        searchHighlight.width = item.width
        searchHighlight.height = item.height
        searchHighlightAnimation.restart()
    }

    Rectangle {
        id: searchHighlight

        property Item target: null

        z: 10
        opacity: 0
        visible: opacity > 0
        radius: 0
        color: Theme.accentSoft
        border.width: 2
        border.color: Theme.accent

        SequentialAnimation {
            id: searchHighlightAnimation
            PropertyAction { target: searchHighlight; property: "opacity"; value: 1 }
            PauseAnimation { duration: 700 }
            NumberAnimation {
                target: searchHighlight
                property: "opacity"
                to: 0
                duration: 900
                easing.type: Easing.InQuad
            }
            ScriptAction { script: searchHighlight.target = null }
        }
    }

    Window.onActiveFocusItemChanged: {
        var item = Window.activeFocusItem
        if (suppressAutoScroll || !item || !isChildOfFlickable(item)) {
            return
        }

        var pos = item.mapToItem(contentItem, 0, 0)
        var scrollMargin = height > 100 ? 50 : 0

        if (pos.y - scrollMargin < contentY) {
            autoScrollAnimation.from = contentY
            autoScrollAnimation.to = Math.max(pos.y - scrollMargin, 0)
            autoScrollAnimation.start()
        }
        else if (pos.y + item.height + scrollMargin > contentY + height) {
            autoScrollAnimation.from = contentY
            autoScrollAnimation.to = Math.min(pos.y + item.height + scrollMargin - height,
                                              Math.max(0, contentHeight - height))
            autoScrollAnimation.start()
        }
    }
}
