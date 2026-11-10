import bb.cascades 1.1

// 通用笔记流卡片页 —— 对应 BBTube FeedPage：同一视图复用不同 NoteModel
// Xiaohongshu 版：使用 noteModel 替代原 FeedModel
Container {
    id: noteFeedPage
    property variant model: null
    property string pageTitle: ""

    // 本页所在的 NavigationPane，由父页面显式传入（main.qml → XxxPage → NoteFeedPage）。
    // 【不要】用 tabs.activeTab.content 去猜当前栈：那样多个 Tab 会互相串台。
    property variant nav: null

    // 行宽 = 本容器实际布局宽度（作为 Page 的 content，会被拉伸到全屏宽）。
    property int screenWidth: noteFeedPage.width > 0 ? noteFeedPage.width : 720

    // 根节点（TabbedPane）引用：建页与 push 都由 main.qml 的 pushNote() 完成
    property variant host: null

    // 探针结果（由 main.qml 的 probe() 返回后缓存）
    property string probeState: "unset"

    /**
     * 统一入口：拿一整行数据 push 笔记详情页。
     * 全程同步 JS 调用 —— 不经 C++ 信号、不经 Connections、不做多跳 QML 信号转发
     * （Qt 4.8 的 QML 里信号命名参数注入不可靠，链路会静默断掉）。
     *
     * @param row 形如 { noteId: "..." } 的数据项
     * @param src 触发来源标记，只用于诊断显示（triggered / gesture）
     */
    function pushNoteByRow(row, src) {
        var now = new Date().getTime();
        if (now - noteFeedPage.lastTapMs < 900) {
            return; // 两条触发路径都命中时只认第一次，避免 push 两层
        }
        var noteId = (row && row.noteId) ? row.noteId : "";
        var diag = src + " noteId=" + noteId;
        if (noteId.length == 0) {
            noteFeedPage.tapDiag = diag + " | row-null";
            return;
        }
        if (!noteFeedPage.nav) {
            noteFeedPage.tapDiag = diag + " | nav-null";
            return;
        }
        // 建页 + push 全部交给 main.qml 根节点的 pushNote()：
        // ComponentDefinition 与调用 createObject 的函数必须在同一个文档作用域里
        if (!noteFeedPage.host) {
            noteFeedPage.tapDiag = diag + " | host-null";
            return;
        }
        var r = noteFeedPage.host.pushNote(noteFeedPage.nav, noteId);
        if (r == "pushed") {
            noteFeedPage.lastTapMs = now;
            noteFeedPage.tapDiag = "";
        } else {
            var lg = app.qmlLogText();
            if (lg.length > 400) {
                lg = lg.substring(lg.length - 400);
            }
            noteFeedPage.tapDiag = diag + " | " + r + "/" + noteFeedPage.host.probe() + "\n" + lg;
        }
    }

    // 防重复触发
    property double lastTapMs: 0
    // 临时诊断
    property string tapDiag: ""

    /**
     * ListView.onTriggered 的入口：用标准 DataModel API 取整行。
     * 注意 data() 要收整条 indexPath（QVariantList），不是 indexPath[0]。
     */
    function openVideoAt(indexPath) {
        var row = list.dataModel ? list.dataModel.data(indexPath) : null;
        if (!row && model) {
            // 兜底：dataModel 未就绪时用 Q_INVOKABLE 的 noteIdAt 造一个最小行
            row = {
                "noteId": model.noteIdAt(indexPath[0])
            };
        }
        noteFeedPage.pushNoteByRow(row, "triggered");
    }

    ListView {
        id: list
        dataModel: model
        scrollRole: ScrollRole.Main

        listItemComponents: [
            // 【关键】不要写 type: "item"。
            ListItemComponent {
                // 行内容必须放在独立文件 components/NoteRow.qml 里：
                // ListItemComponent 是独立组件作用域，内联内容取不到文档级 id 和 ui。
                NoteRow {
                    title: ListItemData.title
                    meta: ListItemData.metaText
                    // 【v102】picUrl 里存的是**远端 URL**（永不变化），
                    // 本地路径由 NoteRow 内部用 biliApi.coverUrlFor 解析。
                    picUrl: ListItemData.picUrl
                    durationText: ListItemData.durationText
                    // ListItemComponent 是独立作用域，看不到 context property，
                    // 所以 api 由 C++ 塞进行数据（ListItemData.api），不是 biliApi。
                    api: ListItemData.api
                }
            }
        ]

        onTriggered: {
            noteFeedPage.openVideoAt(indexPath);
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

    // 首次加载转圈
    ActivityIndicator {
        id: bigSpinner
        horizontalAlignment: HorizontalAlignment.Center
        verticalAlignment: VerticalAlignment.Center
        running: model.loading
        visible: model.loading
    }

    // 触底加载更多
    ActivityIndicator {
        id: moreSpinner
        horizontalAlignment: HorizontalAlignment.Center
        verticalAlignment: VerticalAlignment.Bottom
        bottomMargin: ui.du(10)
        running: model.loadingMore
        visible: model.loadingMore
    }

    // 错误 + 重试
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

    // 【临时诊断】放在文件的最后一个子节点 —— DockLayout 里后声明的在上层
    Container {
        id: tapDiagBox
        horizontalAlignment: HorizontalAlignment.Left
        verticalAlignment: VerticalAlignment.Top
        background: Color.create("#EE000000")
        visible: noteFeedPage.tapDiag.length > 0
        topPadding: ui.du(5)
        bottomPadding: ui.du(5)
        leftPadding: ui.du(10)
        rightPadding: ui.du(10)

        Label {
            text: noteFeedPage.tapDiag
            textStyle.color: Color.Yellow
            textStyle.fontSize: FontSize.XSmall
            multiline: true
        }
    }
}
