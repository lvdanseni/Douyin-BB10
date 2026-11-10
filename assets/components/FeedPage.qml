import bb.cascades 1.1

// 通用分页卡片页 —— 对应 BBTube HomePage：同一视图复用不同 Feed 模型
Container {
    id: feedPage
    property variant model: null
    property string pageTitle: ""

    // 本页所在的 NavigationPane，由父页面显式传入（main.qml → XxxPage → FeedPage）。
    // 【不要】用 tabs.activeTab.content 去猜当前栈：那样 5 个 Tab 会互相串台。
    property variant nav: null

    // 行宽 = 本容器实际布局宽度（作为 Page 的 content，会被拉伸到全屏宽）。
    // 布局完成前 width 为 0，此时回落 720，避免封面高度塌成 0。
    //
    // 【不要】从 C++ 用 bb::device::DisplayInfo 注入屏幕尺寸：本工程 Makefile 的
    // LIBS 里没有 -lbbdevice，那三个符号会保持未定义，设备上 dlopen 解析失败，
    // 表现是应用完全打不开（v4 就栽在这里）。纯 QML 取布局宽度，零外部依赖。
    property int screenWidth: feedPage.width > 0 ? feedPage.width : 720

    // 【临时诊断】点击链路的可见反馈。确认跳转正常后连同 tapDiagBox 一起删掉。
    property string tapDiag: ""
    // 防重复触发：一次点击只允许 push 一层（避免快速连点叠出多层详情页）
    property double lastTapMs: 0
    // 根节点（TabbedPane）引用：建页与 push 都由 main.qml 的 pushVideo() 完成
    property variant host: null

    // 探针结果（由 main.qml 的 probe() 返回后缓存）
    property string probeState: "unset"

    /**
     * 【v83】本页数据是否要求登录态（关注流 = true）。
     * 未登录时用一层提示盖住列表 —— 否则用户只会看到一行 "API code -101"。
     */
    property bool requireLogin: false

    layout: DockLayout {}

    /**
     * 统一入口：拿一整行数据 push 详情页。
     * 全程同步 JS 调用 —— 不经 C++ 信号、不经 Connections、不做多跳 QML 信号转发
     * （Qt 4.8 的 QML 里信号命名参数注入不可靠，链路会静默断掉）。
     *
     * @param row 形如 { bvid: "BV..." } 的数据项
     * @param src 触发来源标记，只用于诊断显示（triggered / gesture）
     */
    function pushVideoByRow(row, src) {
        var now = new Date().getTime();
        if (now - feedPage.lastTapMs < 900) {
            return; // 两条触发路径都命中时只认第一次，避免 push 两层
        }
        var bvid = (row && row.bvid) ? row.bvid : "";
        var diag = src + " bvid=" + bvid;
        if (bvid.length == 0) {
            feedPage.tapDiag = diag + " | row-null";
            return;
        }
        if (!feedPage.nav) {
            feedPage.tapDiag = diag + " | nav-null";
            return;
        }
        // 建页 + push 全部交给 main.qml 根节点的 pushVideo()：
        // ComponentDefinition 与调用 createObject 的函数必须在同一个文档作用域里
        // （BBTube 就是这么做的，也是它唯一验证过的位置）。
        if (!feedPage.host) {
            feedPage.tapDiag = diag + " | host-null";
            return;
        }
        var r = feedPage.host.pushVideo(feedPage.nav, bvid);
        if (r == "pushed") {
            feedPage.lastTapMs = now;
            // 【v78】成功时清空诊断，不要留一行黄字糊在列表顶部。
            // 原来这里写的是 "triggered bvid=BV... | pushed"，每次点完视频
            // 推荐页顶上就多一条黑底黄字，看起来像报错。
            feedPage.tapDiag = "";
        } else {
            // 附带探针结果 + Qt 捕获到的 QML 报错原文（最后 400 字），
            // 报错原文会直接指出是哪个文件、哪一行、哪个元素不被支持。
            var lg = app.qmlLogText();
            if (lg.length > 400) {
                lg = lg.substring(lg.length - 400);
            }
            feedPage.tapDiag = diag + " | " + r + "/" + feedPage.host.probe() + "\n" + lg;
        }
    }

    /**
     * ListView.onTriggered 的入口：用标准 DataModel API 取整行。
     * 注意 data() 要收整条 indexPath（QVariantList），不是 indexPath[0]。
     */
    function openVideoAt(indexPath) {
        var row = list.dataModel ? list.dataModel.data(indexPath) : null;
        if (!row && model) {
            // 兜底：dataModel 未就绪时用 Q_INVOKABLE 的 bvidAt 造一个最小行
            row = {
                "bvid": model.bvidAt(indexPath[0])
            };
        }
        feedPage.pushVideoByRow(row, "triggered");
    }

    ListView {
        id: list
        dataModel: model
        scrollRole: ScrollRole.Main

        listItemComponents: [
            // 【关键】不要写 type: "item"。
            // ArrayDataModel 的 itemType 默认是空字符串，ListItemComponent 的 type
            // 也必须为空才会匹配。一旦写成 type: "item"，ListView 找不到匹配项，
            // 就会退回到默认 delegate —— 它只显示 QVariantMap 里字母序第一个键的值
            // （我们这里是 bvid），于是整页只剩一串 BV 号，没有封面、没有标题。
            // BBTube 的 HomePage.qml 同样不写 type，这是设备上验证过的做法。
            ListItemComponent {
                // 行内容必须放在独立文件 components/VideoRow.qml 里：
                // ListItemComponent 是独立组件作用域，内联内容取不到文档级 id 和 ui。
                // 这里只做「把 ListItemData 的字段传给行组件」，与 BBTube 写法一致。
                VideoRow {
                    title: ListItemData.title
                    meta: ListItemData.metaText
                    // 【v102】picUrl 里存的是**远端 URL**（永不变化），
                    // 本地路径由 VideoRow 内部用 biliApi.coverUrlFor 解析。
                    picUrl: ListItemData.picUrl
                    durationText: ListItemData.durationText
                    // ListItemComponent 是独立作用域，看不到 context property，
                    // 所以 api 由 C++ 塞进行数据（ListItemData.api），不是 biliApi。
                    api: ListItemData.api
                }
            }
        ]

        onTriggered: {
            // 一行调用，全部逻辑在 openVideoAt 里（取行 → 建页 → push）
            feedPage.openVideoAt(indexPath);
        }

        attachedObjects: [
            ListScrollStateHandler {
                onAtEndChanged: {
                    if (atEnd) {
                        model.loadMore();
                    }
                }
            }
        ]
    }

    // 首次加载转圈（对应 HomePage 的 first-load spinner）
    ActivityIndicator {
        id: bigSpinner
        horizontalAlignment: HorizontalAlignment.Center
        verticalAlignment: VerticalAlignment.Center
        running: model.loading
        visible: model.loading
    }

    // 触底加载更多（对应 HomePage 的 footer spinner）
    ActivityIndicator {
        id: moreSpinner
        horizontalAlignment: HorizontalAlignment.Center
        verticalAlignment: VerticalAlignment.Bottom
        bottomMargin: ui.du(10)
        running: model.loadingMore
        visible: model.loadingMore
    }

    // 错误 + 重试（对应 HomePage 的 error retry）
    Container {
        id: errorBox
        horizontalAlignment: HorizontalAlignment.Center
        verticalAlignment: VerticalAlignment.Center
        visible: model.error.length > 0
        topPadding: ui.du(30)
        bottomPadding: ui.du(30)
        leftPadding: ui.du(40)
        rightPadding: ui.du(40)

        Label {
            text: model.error
            textStyle.color: theme.textSecondary
            multiline: true
            horizontalAlignment: HorizontalAlignment.Center
        }
        Button {
            text: "重试"
            horizontalAlignment: HorizontalAlignment.Center
            onClicked: {
                model.load();
            }
        }
    }

    // 【临时诊断】放在文件的最后一个子节点 —— DockLayout 里后声明的在上层，
    // 放前面会被 ListView 的卡片背景盖住，导致"看起来没有黄字"。
    Container {
        id: tapDiagBox
        horizontalAlignment: HorizontalAlignment.Left
        verticalAlignment: VerticalAlignment.Top
        background: Color.create("#EE000000")
        visible: feedPage.tapDiag.length > 0
        topPadding: ui.du(5)
        bottomPadding: ui.du(5)
        leftPadding: ui.du(10)
        rightPadding: ui.du(10)

        Label {
            text: feedPage.tapDiag
            textStyle.color: Color.Yellow
            textStyle.fontSize: FontSize.XSmall
            multiline: true
        }
    }

    // 【v83】登录提示层 —— 必须是 DockLayout 里最后一个子节点（后声明的在上层），
    // 否则会被 ListView 盖住。仅 requireLogin 的页面（关注流）在未登录时显示。
    Container {
        horizontalAlignment: HorizontalAlignment.Fill
        verticalAlignment: VerticalAlignment.Fill
        background: Color.create("#17181A")
        visible: feedPage.requireLogin && !biliApi.loggedIn
        layout: DockLayout {}

        Container {
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Center
            leftPadding: ui.du(40)
            rightPadding: ui.du(40)

            Label {
                text: "登录后查看关注动态"
                horizontalAlignment: HorizontalAlignment.Center
                textStyle.color: Color.create("#F2F3F5")
                textStyle.fontSize: FontSize.Large
            }
            Label {
                text: "在设置页扫码登录，这里会显示你关注的 UP 主发布的最新视频。"
                horizontalAlignment: HorizontalAlignment.Center
                multiline: true
                topMargin: ui.du(8)
                textStyle.color: Color.create("#9CA0A6")
            }
            Button {
                text: "去登录"
                horizontalAlignment: HorizontalAlignment.Center
                topMargin: ui.du(24)
                onClicked: {
                    // host = main.qml 的 TabbedPane，openSettings() 是它的 JS 函数。
                    // 直接函数调用可靠；不要走信号转发（Qt 4.8 会静默断链）。
                    if (feedPage.host) {
                        feedPage.host.openSettings();
                    }
                }
            }
        }
    }
}
