import bb.cascades 1.1

import "components"
// 【v89】关注动态 —— 对应小红书「关注」板块。
//
// 数据源：FollowStore（需登录态，拉 /api/sns/v10/follow/feed）。
// 未登录时显示"去登录"引导，已登录时显示关注的人发布的笔记列表。
//
// 点击条目 → host.pushNote(nav, noteId) 进入笔记详情页。
//
// 切 Tab 时由 main.qml 的 onActiveTabChanged 调用 followStore.reloadIfStale()，
// 这里不在 onCreationCompleted 里 load（避免未登录时空拉）。
Page {
    id: followPage

    property variant nav: null
    property variant host: null

    titleBar: TitleBar {
        title: "关注"
        acceptAction: ActionItem {
            title: "刷新"
            onTriggered: {
                followStore.load()
            }
        }
    }

    Container {
        id: followRoot
        horizontalAlignment: HorizontalAlignment.Fill
        verticalAlignment: VerticalAlignment.Fill
        layout: DockLayout {}

        // 未登录引导
        Container {
            id: followLoginGuide
            horizontalAlignment: HorizontalAlignment.Center
            verticalAlignment: VerticalAlignment.Center
            visible: !biliApi.loggedIn
            layout: StackLayout {}

            Label {
                text: "未登录"
                textStyle.color: theme.textSecondary
                textStyle.fontSize: FontSize.Medium
            }
            Label {
                text: "登录后可查看关注的人发布的笔记"
                textStyle.color: theme.textMuted
                textStyle.fontSize: FontSize.Small
                topPadding: 4
            }
            Button {
                text: "去登录"
                horizontalAlignment: HorizontalAlignment.Center
                topMargin: 12
                onClicked: {
                    host.openSettings()
                }
            }
        }

        // 关注动态列表
        ListView {
            id: followList
            dataModel: followStore.model
            // ListView 不写 layout（StackLayout 是 Container 的，类型不匹配）
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Fill
            visible: biliApi.loggedIn

            listItemComponents: [
                ListItemComponent {
                    Container {
                        id: followRowContainer
                        preferredWidth: 720
                        bottomPadding: 16
                        layout: StackLayout {}

                        // 封面（16:9，宽度 = 行宽）
                        ImageView {
                            id: followCover
                            imageSource: biliApi.coverUrlFor(ListItemData.coverUrl, biliApi.coverVersion)
                            preferredWidth: 720
                            preferredHeight: 720 * 9 / 16
                            scalingMethod: ScalingMethod.AspectFill
                            horizontalAlignment: HorizontalAlignment.Center
                        }

                        // 时长徽章（右下角）
                        Container {
                            id: followDurationBadge
                            horizontalAlignment: HorizontalAlignment.Right
                            verticalAlignment: VerticalAlignment.Bottom
                            rightPadding: 4
                            bottomPadding: 4
                            background: Color.create("#88000000")

                            Label {
                                text: ListItemData.durationText
                                textStyle.color: Color.White
                                textStyle.fontSize: FontSize.XSmall
                                leftPadding: 2
                                rightPadding: 2
                                topPadding: 1
                                bottomPadding: 1
                            }
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

                        // 元数据
                        Label {
                            text: ListItemData.metaText
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
                                var noteId = ListItemData.noteId
                                if (!noteId || noteId.length === 0) {
                                    noteId = followStore.noteIdAt(indexPath[0])
                                }
                                if (noteId && noteId.length > 0 && followPage.host) {
                                    followPage.host.pushNote(followPage.nav, noteId)
                                }
                            }
                        }
                    }
                }
            ]

            onTriggered: {
                var row = followList.dataModel ? followList.dataModel.data(indexPath) : null
                var noteId = (row && row.noteId) ? row.noteId : ""
                if (!noteId || noteId.length === 0) {
                    noteId = followStore.noteIdAt(indexPath[0])
                }
                if (noteId && noteId.length > 0 && followPage.host) {
                    followPage.host.pushNote(followPage.nav, noteId)
                }
            }
        }

        // 空状态（已登录但无数据）
        Container {
            id: followEmpty
            horizontalAlignment: HorizontalAlignment.Center
            verticalAlignment: VerticalAlignment.Center
            visible: biliApi.loggedIn && followStore.count === 0 && !followStore.loading
            layout: StackLayout {}

            Label {
                text: "暂无关注动态"
                textStyle.color: theme.textSecondary
                textStyle.fontSize: FontSize.Medium
            }
            Label {
                text: "去关注感兴趣的作者，他们的笔记会出现在这里"
                textStyle.color: theme.textMuted
                textStyle.fontSize: FontSize.Small
                topPadding: 4
            }
        }

        // 加载中
        ActivityIndicator {
            id: followSpinner
            horizontalAlignment: HorizontalAlignment.Center
            verticalAlignment: VerticalAlignment.Center
            running: followStore.loading
            visible: followStore.loading
        }

        // 错误 + 重试
        Container {
            id: followError
            horizontalAlignment: HorizontalAlignment.Center
            verticalAlignment: VerticalAlignment.Center
            visible: followStore.error.length > 0
            topPadding: 30
            bottomPadding: 30
            leftPadding: 40
            rightPadding: 40

            Label {
                text: followStore.error
                textStyle.color: theme.textSecondary
                multiline: true
                horizontalAlignment: HorizontalAlignment.Center
            }
            Button {
                text: "重试"
                horizontalAlignment: HorizontalAlignment.Center
                onClicked: {
                    followStore.load()
                }
            }
        }
    }
}
