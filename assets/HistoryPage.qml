import bb.cascades 1.1

import "components"
// 【v89】观看历史 —— 对应小红书「历史」板块。
//
// 数据源：LocalDb 的 history 表（本地 SQLite，由 NoteStore 在观看笔记时写入）。
// HistoryStore 负责从数据库读取并填充 ArrayDataModel，每行字段：
//   bvid / title / coverUrl / ownerName / viewAt
// NoteRow 复用：title / meta(ownerName) / picUrl(coverUrl) / api(biliApi)。
//
// 点击条目 → host.pushNote(nav, noteId) 进入笔记详情页。
// 顶部"清空"按钮 → historyStore.clearAll()。
//
// 切 Tab 时由 main.qml 的 onActiveTabChanged 调用 historyStore.reload()，
// 这里不在 onCreationCompleted 里 load（避免未登录时空拉）。
Page {
    id: historyPage

    property variant nav: null
    property variant host: null

    titleBar: TitleBar {
        title: "历史"
        acceptAction: ActionItem {
            title: "刷新"
            onTriggered: {
                historyStore.reload()
            }
        }
    }

    Container {
        id: historyRoot
        horizontalAlignment: HorizontalAlignment.Fill
        verticalAlignment: VerticalAlignment.Fill
        layout: DockLayout {}

        // 历史记录列表
        ListView {
            id: historyList
            dataModel: historyStore.model
            // ListView 不写 layout（StackLayout 是 Container 的，类型不匹配）
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Fill

            listItemComponents: [
                ListItemComponent {
                    Container {
                        id: historyRowContainer
                        preferredWidth: 720
                        bottomPadding: 16
                        layout: StackLayout {}

                        // 封面（16:9，宽度 = 行宽）
                        ImageView {
                            id: historyCover
                            imageSource: biliApi.coverUrlFor(ListItemData.coverUrl, biliApi.coverVersion)
                            preferredWidth: 720
                            preferredHeight: 720 * 9 / 16
                            scalingMethod: ScalingMethod.AspectFill
                            horizontalAlignment: HorizontalAlignment.Center
                        }

                        // 标题
                        Label {
                            text: ListItemData.title
                            textStyle.color: Color.create("#F2F3F5")
                            textStyle.fontSize: FontSize.Default
                            textStyle.fontWeight: FontWeight.Bold
                            multiline: true
                            topPadding: 6
                            leftPadding: 2
                            rightPadding: 2
                            horizontalAlignment: HorizontalAlignment.Left
                        }

                        // 元数据（作者名）
                        Label {
                            text: ListItemData.ownerName
                            textStyle.color: Color.create("#9CA0A6")
                            textStyle.fontSize: FontSize.Small
                            multiline: true
                            topPadding: 2
                            leftPadding: 2
                            rightPadding: 2
                            horizontalAlignment: HorizontalAlignment.Left
                        }

                        // Cascades 1.1 无 TouchBehaviour，改用 Control.onTouch 信号
                        onTouch: {
                            if (event.touchType === TouchType.Down) {
                                var noteId = ListItemData.bvid
                                if (!noteId || noteId.length === 0) {
                                    noteId = historyStore.noteIdAt(indexPath[0])
                                }
                                if (noteId && noteId.length > 0 && historyPage.host) {
                                    historyPage.host.pushNote(historyPage.nav, noteId)
                                }
                            }
                        }
                    }
                }
            ]

            onTriggered: {
                var row = historyList.dataModel ? historyList.dataModel.data(indexPath) : null
                var noteId = (row && row.bvid) ? row.bvid : ""
                if (!noteId || noteId.length === 0) {
                    noteId = historyStore.noteIdAt(indexPath[0])
                }
                if (noteId && noteId.length > 0 && historyPage.host) {
                    historyPage.host.pushNote(historyPage.nav, noteId)
                }
            }
        }

        // 空状态（HistoryStore 是同步读本地数据库，无 loading / error 状态）
        Container {
            id: historyEmpty
            horizontalAlignment: HorizontalAlignment.Center
            verticalAlignment: VerticalAlignment.Center
            visible: historyStore.count === 0
            layout: StackLayout {}

            Label {
                text: "暂无观看历史"
                textStyle.color: theme.textSecondary
                textStyle.fontSize: FontSize.Medium
            }
            Label {
                text: "观看过的笔记会出现在这里"
                textStyle.color: theme.textMuted
                textStyle.fontSize: FontSize.Small
                topPadding: 4
            }
            Button {
                text: "清空历史"
                horizontalAlignment: HorizontalAlignment.Center
                topMargin: 12
                onClicked: {
                    historyStore.clearAll()
                }
            }
        }
    }
}
