import bb.cascades 1.1

// 列表行 —— 大封面样式，照 BBTube 的 components/VideoRow.qml 做。
//
// 【布局】
//   ┌───────────────────────────┐
//   │                           │
//   │      全宽 16:9 封面        │  ← 高度 = 行宽 * 9 / 16
//   │                   [12:34] │  ← 右下角时长徽章
//   ├───────────────────────────┤
//   │ 标题（可换行）             │
//   │ UP主 · 23万播放 · 12天前   │
//   └───────────────────────────┘
//
// 为什么不做"左标题 + 右小图"的紧凑行：BBTube 用的是 YouTube 那套
// full-width thumbnail + 下方文字，标题在全宽下读起来舒服得多。
// 用户明确要求对齐这个样式。推荐/热门/排行/分区四个页面共用本组件，
// 所以只改这一个文件，四个 tab 一起生效。
//
// 【作用域约束（老坑，别踩）】
// ListItemComponent 是独立的组件作用域，取不到文档级 id，也取不到全局 ui 对象
// （运行时会抛 ReferenceError: Can't find variable: ui）。所以这里一律写字面量：
// 尺寸直接用数字（Cascades 的尺寸属性本来就是 du 单位，du(n) == n），颜色用
// Color.create(...)。不要试图引用 theme / ui / 父组件 id。
Container {
    id: videoRow

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
     * 【v102】把 picUrl 解析成 ImageView 能用的地址。
     *
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

    /**
     * 【v92】给"已下载"页的长按删除用。
     *
     * store    —— DownloadStore 对象指针，由 C++ 在 reload() 里塞进每行数据，
     *              QML 侧通过属性绑定 `store: ListItemData.store` 拿到。
     * rowBvid  —— 这一行对应的 bvid，同样走属性绑定。
     *
     * 为什么不在长按菜单的 onTriggered 里直接读 ListItemData / downloadStore：
     * ListItemComponent 是独立组件作用域，contextActions 里的 ActionItem 更是
     * 挂在列表属性上，它的 JS 函数体里 `ListItemData` 和全局 context property
     * **都不保证可见**（v91 实测：菜单能弹，点了完全没反应）。
     * 属性绑定则一定求值 —— 所以取值全部挪到绑定里，JS 里只引用根节点的 id。
     */
    property variant store: null
    property string rowBvid: ""

    // 行宽。默认整屏宽（720du = 屏幕宽，与 ui.du(720) 一致）。
    // 播放页的相关推荐列表用的是同一个组件、同样的宽度 —— BBTube 的 RelatedRow
    // 也是直接包一层 VideoRow（只是多了点击），所以两边看起来是一套东西。
    //
    // 之所以要做成属性而不是写死：自定义 list item 不给确定宽度时会缩到内容宽，
    // 而不同宿主（首页 ListView / 播放页内嵌 ListView）需要能覆盖它。
    property int rowWidth: 720

    // ListView 会把 item 拉伸到全屏宽；显式给一个确定宽度，避免"自定义 list item
    // 宽度塌陷"（Cascades 自定义 item 不给宽度时会缩到内容宽）。
    preferredWidth: videoRow.rowWidth

    // 卡片之间留缝，让页面底色透出来当分隔 —— 不用 Divider，
    // BBTube 也是靠留白分卡，加横线反而更碎。
    bottomPadding: 18

    layout: StackLayout {}

    // ---- 封面：全宽 16:9 ----
    Container {
        horizontalAlignment: HorizontalAlignment.Fill
        // 按行宽算 16:9。布局完成前 width 为 0，用 720*9/16 兜底，
        // 否则高度会先塌成 0，ListView 算错行高。
        preferredHeight: videoRow.width > 0 ? videoRow.width * 9 / 16 : videoRow.rowWidth * 9 / 16
        background: Color.create("#26272B")

        layout: DockLayout {}

        ImageView {
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Fill
            // 【v102】封面解析。三种情况：
            //   1) 已经是 file:// → 直接用（已下载列表）
            //   2) 远端 URL → coverUrlFor 换本地路径
            //   3) 还没下好 → 空串，露出下面的 #26272B 占位色
            //
            // ⚠️ 必须是**单表达式**，不能写成块 `{ if(...) return ...; }` ——
            // Qt 4.8 会把开头的 `{` 当成 JS 对象字面量，绑定直接失效（铁律 5）。
            // 所以逻辑放进 picSource() 函数，绑定这里只做一次调用。
            //
            // ⚠️ 第二个实参 `videoRow.api.coverVersion` 是**依赖锚点**：
            // QML 只跟踪绑定表达式里读到的属性，不跟踪函数返回值。把 coverVersion
            // 写在实参里，才会在每张封面下完时触发这一行重算。
            imageSource: videoRow.picSource(videoRow.picUrl,
                                            videoRow.api ? videoRow.api.coverVersion : -1)
            // AspectFill：图片比例与 16:9 不完全一致时裁切而不是留白。
            scalingMethod: ScalingMethod.AspectFill
        }

        // 时长角标（右下）
        Container {
            horizontalAlignment: HorizontalAlignment.Right
            verticalAlignment: VerticalAlignment.Bottom
            rightMargin: 8
            bottomMargin: 8
            background: Color.create("#CC000000")
            leftPadding: 6
            rightPadding: 6
            topPadding: 2
            bottomPadding: 2
            visible: videoRow.durationText !== ""

            Label {
                text: videoRow.durationText
                textStyle.color: Color.White
                textStyle.fontSize: FontSize.XSmall
            }
        }
    }

    // ---- 文字：标题 + 元数据 ----
    Container {
        horizontalAlignment: HorizontalAlignment.Fill
        leftPadding: 12
        rightPadding: 12
        topPadding: 8

        layout: StackLayout {}

        Label {
            text: videoRow.title
            textStyle.color: Color.create("#F2F3F5")
            textStyle.fontSize: FontSize.Small
            // 标题里可能带 <em> 之类的富文本标记，一律按纯文本渲染
            textFormat: TextFormat.Plain
            multiline: true
        }

        Label {
            text: videoRow.meta
            textStyle.color: Color.create("#9CA0A6")
            textStyle.fontSize: FontSize.XSmall
            topMargin: 4
            multiline: true
            visible: videoRow.meta !== ""
        }
    }
}
