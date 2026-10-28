import QtQuick 2.9
import "../theme"

// 一张设置卡片：方角硬投影面板 + 左侧强调粗条 + 宽字距标题，内容走默认属性。
//
// 前身是 GlassCard（半透明表面 + 顶部高光渐变）。新风格里卡片是不透明的硬物件，
// 高光那层被删掉了 —— 它是 glassmorphism 的残留，和零模糊硬边的方向相反。
Item {
    id: card

    property string title: ""
    property string subtitle: ""
    default property alias cardContent: contentColumn.data

    // 业务上是否该显示这张卡（例如某功能在当前平台不可用）。和 SettingsRow 一样用
    // applicable 而不是直接绑 visible：设置搜索要在卡片所在分类没打开时也能判断它是否可用。
    property bool applicable: true

    // 这张卡属于哪个分类（SettingsView 的分类 key）。只有和其它分类的卡片混放在同一个
    // 页面里时才需要设（LegacySettingsPage）；独立页面由 SettingsView 在页面实例上声明。
    property string settingsCategory: ""

    // 设置搜索元数据，见 SettingsSearchIndex.js。卡片里有可搜索的行时，卡片标题只作为
    // 这些行的「分区」参与匹配；整张卡没有可搜索的行时，卡片自己才是一条结果。
    readonly property string settingsSearchKind: "card"
    property string settingsSearchTitle: title
    property string settingsSearchDescription: subtitle
    property string settingsSearchKeywords: ""
    property Item settingsSearchFocusTarget: null

    // 卡片内是否还有适用的行。全部被过滤掉时整张卡自动隐藏。
    //
    // 这里必须看 applicable 这类「显式意图」，绝对不能看 visible。visible 是实际可见性：
    // 设置页按分类切换时，父级一隐藏整页，所有子行的 visible 都会变成假，于是这里判定
    // 卡片是空的、把卡片自己也隐藏掉；而卡片一旦隐藏，子行就再也不可能变回可见 ——
    // 状态永久锁死，切回「基本设置」时整页就是空白的。
    readonly property bool hasVisibleContent: {
        for (var i = 0; i < contentColumn.children.length; i++) {
            var child = contentColumn.children[i]
            // 没声明 applicable 的（比如直接塞进来的 Item 容器）一律算作有内容
            if (child.applicable === undefined || child.applicable) {
                return true
            }
        }
        return false
    }

    width: parent ? parent.width : 0
    visible: applicable && hasVisibleContent
    height: visible ? implicitHeight : 0
    // 底部多留出投影的高度，否则硬投影会压在下一张卡片的上沿
    implicitHeight: layout.implicitHeight + Theme.spaceLg * 2 + Theme.shadowOffset

    Panel {
        fill: Theme.surfaceLayer
        anchors {
            fill: parent
            // 右下让出投影的位置，不然会溢出滚动区
            rightMargin: Theme.shadowOffset
            bottomMargin: Theme.shadowOffset
        }

        accentBarColor: Theme.accent
        accentBarWidth: Theme.accentBar

        Column {
            id: layout
            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
                margins: Theme.spaceLg
            }
            spacing: Theme.spaceMd

            Column {
                width: parent.width
                spacing: Theme.spaceXs
                visible: card.title !== ""

                Text {
                    text: card.title
                    color: Theme.accent
                    font.family: Theme.fontSans
                    font.pointSize: Theme.fontCardTitle
                    font.weight: Font.ExtraBold
                    font.capitalization: Font.AllUppercase
                    font.letterSpacing: Theme.tracking(Theme.fontCardTitle, 0.08)
                }

                Text {
                    width: parent.width
                    text: card.subtitle
                    visible: text !== ""
                    color: Theme.textSettingsSubtitle
                    font.family: Theme.fontSans
                    font.pointSize: Theme.fontSettingsSubtitle
                    font.weight: Font.Medium
                    wrapMode: Text.Wrap
                }
            }

            Column {
                id: contentColumn
                width: parent.width
                spacing: Theme.spaceSm
            }
        }
    }
}
