import bb.cascades 1.1

// 笔记列表行 —— 大封面样式，照 BBTube 的 components/VideoRow.qml 做。
//
// 【布局】
//   ┌───────────────────────────┐
//   │                           │
//   │      全宽 16:9 封面        │  ← 高度 = 行宽 * 9 / 16
//   │                   [12:34] │  ← 右下角时长徽章
//   ├───────────────────────────┤
//   │ 标题（可换行）             │
//   │ 作者 · 23万点赞 · 12天前    │
//   └───────────────────────────┘
//
// 【作用域约束（老坑，别踩）】
// ListItemComponent 是独立的组件作用域，取不到文档级 id，也取不到全局 ui 对象
// （运行时会抛 ReferenceError: Can't find variable: ui）。所以这里一律写字面量：
// 尺寸直接用数字（Cascades 的尺寸属性本来就是 du 单位，du(n) == n），颜色用
// Color.create(...)。不要试图引用 theme / ui / 父组件 id。
Container {
    id: noteRow

    property string title: ""
    property string meta: ""
    /**
     * 封面地址。两种形态都支持：
     *   - "file:///…"  直接当本地图片用（已下载列表走这条）
     *   - "https://…"  远端 URL，交给 biliApi.coverUrlFor 换成本地路径
     *
     * 【v102】为什么远端 URL 也放在这里：BB10 上「模型行数据变了 → ListView 重绘」
     * 不可靠（replace() 实测封面永远不出现），而 Q_PROPERTY + NOTIFY 是可靠的
     * （播放页 UP 主头像同款机制）。所以列表里存的是**永不变化的远端 URL**，
     * 本地路径由下面 ImageView 的绑定现场解析 —— 解析结果依赖 coverVersion，
     * 每张封面下完 version++，所有绑定自动重算。BBTube 也是这个架构。
     */
    property string picUrl: ""
    property string durationText: ""
    /** BiliApi 对象，由宿主页面注入（ListItemComponent 作用域看不到 context property）。 */
    property variant api: null

    /**
     * 把 picUrl 解析成 ImageView 能用的地址。
     * 独立成函数而不是内联块绑定的原因：Qt 4.8 的 QML 里
     * `prop: { ... }` 会被当成 JS 对象字面量（铁律 5），块绑定直接失效。
     *
     * @param url 远端 URL 或 file:// 本地路径
     * @param ver coverVersion，只用于在调用处建立绑定依赖
     */
    function picSource(url, ver) {
        if (!url || url.length === 0) {
            return "";
        }
        if (url.indexOf("file://") === 0) {
            return url;
        }
        if (api) {
            return api.coverUrlFor(url, ver);
        }
        return "";
    }

    // 行宽。默认整屏宽（720du = 屏幕宽，与 ui.du(720) 一致）。
    property int rowWidth: 720

    // ListView 会把 item 拉伸到全屏宽；显式给一个确定宽度，避免"自定义 list item
    // 宽度塌陷"（Cascades 自定义 item 不给宽度时会缩到内容宽）。
    preferredWidth: noteRow.rowWidth

    // 卡片之间留缝，让页面底色透出来当分隔 —— 不用 Divider，
    // BBTube 也是靠留白分卡，加横线反而更碎。
    bottomPadding: 18

    layout: StackLayout {}

    // 封面：按行宽算出 16:9 高度（或按笔记真实比例，这里先用 16:9 兜底）
    ImageView {
        id: coverView
        imageSource: noteRow.picSource(noteRow.picUrl, biliApi.coverVersion)
        preferredWidth: noteRow.rowWidth
        preferredHeight: noteRow.rowWidth * 9 / 16
        scalingMethod: ScalingMethod.AspectFill
        horizontalAlignment: HorizontalAlignment.Center
    }

    // 时长徽章（右下角）
    Container {
        id: durationBadge
        horizontalAlignment: HorizontalAlignment.Right
        verticalAlignment: VerticalAlignment.Bottom
        rightPadding: ui.du(4)
        bottomPadding: ui.du(4)
        background: Color.create("#88000000")

        Label {
            text: noteRow.durationText
            textStyle.color: Color.White
            textStyle.fontSize: FontSize.XSmall
            leftPadding: ui.du(2)
            rightPadding: ui.du(2)
            topPadding: ui.du(1)
            bottomPadding: ui.du(1)
        }
    }

    // 标题
    Label {
        text: noteRow.title
        textStyle.color: Color.create("#F2F3F5")
        textStyle.fontSize: FontSize.Default
        textStyle.fontWeight: FontWeight.Bold
        multiline: true
        topPadding: ui.du(6)
        leftPadding: ui.du(2)
        rightPadding: ui.du(2)
        horizontalAlignment: HorizontalAlignment.Left
    }

    // 元数据
    Label {
        text: noteRow.meta
        textStyle.color: Color.create("#9CA0A6")
        textStyle.fontSize: FontSize.Small
        multiline: true
        topPadding: ui.du(2)
        leftPadding: ui.du(2)
        rightPadding: ui.du(2)
        horizontalAlignment: HorizontalAlignment.Left
    }
}
