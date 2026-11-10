import bb.cascades 1.1

import "components"
// 收藏页 —— 对应 BBTube FavPage（收藏夹目录 + 夹内笔记）
// Xiaohongshu 版：使用 collectStore + noteModel (Kind=Collect)
Page {
    id: collectPage

    property variant nav: null
    property variant host: null

    titleBar: TitleBar {
        title: "收藏"
        acceptAction: ActionItem {
            title: "刷新"
            onTriggered: {
                collectStore.reload();
            }
        }
    }

    Container {
        id: collectRoot
        horizontalAlignment: HorizontalAlignment.Fill
        verticalAlignment: VerticalAlignment.Fill
        layout: DockLayout {}

        // 上方：收藏夹目录（横向滚动）
        Container {
            id: foldersBar
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Top
            preferredHeight: ui.du(70)
            background: theme.cardBg
            leftPadding: ui.du(12)
            rightPadding: ui.du(12)
            layout: StackLayout {}

            Label {
                text: "收藏夹"
                textStyle.color: theme.textPrimary
                textStyle.fontSize: FontSize.Medium
                verticalAlignment: VerticalAlignment.Center
            }

            ListView {
                id: folderList
                dataModel: collectStore.noteModel
                // 【不要写 layout: StackLayout {}】ListView.layout 的类型是
                // bb::cascades::ListLayout*（listview.h:315），塞 Container 的
                // StackLayout 会报 "Cannot assign object to property"，
                // 并让整个 CollectPage Type unavailable。默认就是竖排线性列表。
                horizontalAlignment: HorizontalAlignment.Fill
                verticalAlignment: VerticalAlignment.Center
                preferredHeight: ui.du(50)

                listItemComponents: [
                    ListItemComponent {
                        Container {
                            leftPadding: ui.du(8)
                            rightPadding: ui.du(8)
                            verticalAlignment: VerticalAlignment.Center
                            background: ListItemData.selected ? theme.accent : theme.cardBg
                            layout: StackLayout {}

                            Label {
                                text: ListItemData.title
                                textStyle.color: ListItemData.selected ? Color.White : theme.textPrimary
                                textStyle.fontSize: FontSize.Small
                                verticalAlignment: VerticalAlignment.Center
                            }
                            // Cascades 1.1 无 TouchBehaviour，改用 Control.onTouch 信号
                            onTouch: {
                                if (event.touchType === TouchType.Down) {
                                    collectStore.selectFolder(indexPath[0]);
                                }
                            }
                        }
                    }
                ]
            }
        }

        // 下方：选中收藏夹内的笔记
        Container {
            id: notesArea
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Fill
            topPadding: ui.du(70)

            NoteFeedPage {
                model: collectStore.noteModel
                nav: collectPage.nav
                host: collectPage.host
            }
        }

        // 空状态
        Container {
            horizontalAlignment: HorizontalAlignment.Center
            verticalAlignment: VerticalAlignment.Center
            visible: collectStore.count === 0 && !collectStore.loading
            layout: StackLayout {}

            Label {
                text: "暂无收藏夹"
                textStyle.color: theme.textSecondary
                textStyle.fontSize: FontSize.Medium
            }
            Label {
                text: "收藏感兴趣的笔记，它们会出现在这里"
                textStyle.color: theme.textMuted
                textStyle.fontSize: FontSize.Small
                topPadding: ui.du(4)
            }
        }

        // 加载中
        ActivityIndicator {
            id: collectSpinner
            horizontalAlignment: HorizontalAlignment.Center
            verticalAlignment: VerticalAlignment.Center
            running: collectStore.loading
            visible: collectStore.loading
        }
    }

    onCreationCompleted: {
        collectStore.reload();
    }
}
