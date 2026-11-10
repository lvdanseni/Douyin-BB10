import bb.cascades 1.1

// 观看历史页（1.0.0.24 新增）。
//
// 【入口】DouyinFeedPage 抽屉菜单「历史」→ main.qml pushHistory()：
//   createObject() → push（historyDef 与 pushHistory 同在 main.qml 根节点作用域）。
//
// 【数据】dyHistoryModel（DouyinHistoryModel）：每次换片 playInline 记一行
//   （同 id 去重置顶、cap 100），原子落盘 data/douyin_history.json，重启不丢。
//   行内 api 指针由 C++ add()/loadFile() 塞好 → ListItemComponent 用
//   ListItemData.api.coverUrlFor 解析封面本地路径（**context property 在
//   delegate 里不可见**，DouyinCommentPage 同款结论）。
//
// 【点击复播】ListView.onTriggered → host.playHistoryItem(row)：
//   main.qml 先 pop 回推荐页，再 feedHome.playRow(row)（stop→load 复用
//   换片序列；CDN 直链过期时 HUD 报错 + 重试按钮兜住，不假装一定能播）。
//
// 【铁律】不要写 type: "item"；自定义 list item 必须给 preferredWidth；
//   绑定不能写成块 { ... }；Label 没有 maxLines，多行用 multiline。
Page {
    id: historyPage

    /** 上一级 NavigationPane（main.qml push 时注入）。 */
    property variant nav: null
    /** 宿主 NavigationPane（main.qml）：playHistoryItem 回调入口。 */
    property variant host: null

    // DockLayout：顶栏吸顶、列表填充、清空按钮与空态沉底/居中。
    content: Container {
        id: histRoot
        horizontalAlignment: HorizontalAlignment.Fill
        verticalAlignment: VerticalAlignment.Fill
        background: theme.bg
        layout: DockLayout {}

        // ===== 历史列表（先声明，z 序在顶栏之下） =====
        ListView {
            id: histList
            objectName: "dyHistoryList"
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Fill
            topMargin: 84
            bottomMargin: 76
            dataModel: dyHistoryModel

            listItemComponents: [
                ListItemComponent {
                    Container {
                        // 自定义 list item 必须给确定宽度（铁律 3）。
                        preferredWidth: 720
                        horizontalAlignment: HorizontalAlignment.Fill
                        background: Color.create("#16161C")
                        topPadding: 10
                        bottomPadding: 10
                        leftPadding: 14
                        rightPadding: 14
                        layout: StackLayout {
                            orientation: LayoutOrientation.LeftToRight
                        }

                        // 封面缩略图（远端 URL 永在行里，本地路径由 api 现场解析）。
                        Container {
                            preferredWidth: 128
                            preferredHeight: 96
                            background: Color.create("#000000")
                            layout: DockLayout {}

                            ImageView {
                                horizontalAlignment: HorizontalAlignment.Fill
                                verticalAlignment: VerticalAlignment.Fill
                                scalingMethod: ScalingMethod.AspectFill
                                imageSource: (ListItemData.api && ListItemData.picUrl && ListItemData.picUrl.length > 0)
                                    ? ListItemData.api.coverUrlFor(ListItemData.picUrl, ListItemData.api.coverVersion)
                                    : ""
                            }

                            // 右下角时长角标（黑底白字）。
                            Label {
                                horizontalAlignment: HorizontalAlignment.Right
                                verticalAlignment: VerticalAlignment.Bottom
                                rightMargin: 4
                                bottomMargin: 4
                                text: (ListItemData.durationSec > 0)
                                    ? (Math.floor(ListItemData.durationSec / 60) + ":" +
                                       ((ListItemData.durationSec % 60 < 10) ? ("0" + ListItemData.durationSec % 60)
                                                                             : ("" + ListItemData.durationSec % 60)))
                                    : ""
                                textStyle.color: Color.White
                                textStyle.fontSize: FontSize.XXSmall
                            }
                        }

                        // 右侧文字列（吃掉剩余宽度）。
                        Container {
                            layoutProperties: StackLayoutProperties {
                                spaceQuota: 1
                            }
                            leftMargin: 16
                            layout: StackLayout {}

                            Label {
                                text: "@" + (ListItemData.authorName ? ListItemData.authorName : "抖音用户")
                                textStyle.color: Color.create("#8E9095")
                                textStyle.fontSize: FontSize.XSmall
                            }

                            Label {
                                text: ListItemData.title ? ListItemData.title : ""
                                textStyle.color: Color.create("#F2F3F5")
                                textStyle.fontSize: FontSize.Small
                                multiline: true
                                topMargin: 4
                            }

                            Label {
                                text: ListItemData.timeText ? ListItemData.timeText : ""
                                textStyle.color: Color.create("#8E9095")
                                textStyle.fontSize: FontSize.XSmall
                                topMargin: 8
                            }
                        }
                    }
                }
            ]

            onTriggered: {
                var row = dyHistoryModel.rowAt(indexPath);
                if (!row) {
                    return;
                }
                // 【1.0.0.29】try/catch 兜底：复播失败（pop 中/playRow 抛异常）也要落日志。
                var rh = "no-host";
                try {
                    if (historyPage.host && historyPage.host.playHistoryItem) {
                        rh = historyPage.host.playHistoryItem(row);
                    }
                } catch (e) {
                    rh = "EX:" + e;
                }
                if (app && app.swipeLog) {
                    app.swipeLog("HIST-ITEM ret=" + rh);
                }
            }
        }

        // ===== 顶部条：返回 + 标题（条数） =====
        Container {
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Top
            preferredHeight: 84
            background: theme.surfaceSemi
            leftPadding: 16
            rightPadding: 16
            layout: StackLayout {
                orientation: LayoutOrientation.LeftToRight
            }

            Button {
                text: "< 返回"
                horizontalAlignment: HorizontalAlignment.Left
                verticalAlignment: VerticalAlignment.Center
                preferredWidth: 130
                preferredHeight: 56
                onClicked: {
                    if (historyPage.nav && historyPage.nav.pop) {
                        historyPage.nav.pop();
                    }
                }
            }

            Label {
                text: dyHistoryModel.count > 0 ? ("历史 " + dyHistoryModel.count) : "历史"
                horizontalAlignment: HorizontalAlignment.Fill
                verticalAlignment: VerticalAlignment.Center
                textStyle.color: theme.textPrimary
                textStyle.fontSize: FontSize.Medium
                textStyle.fontWeight: FontWeight.Bold
            }
        }

        // ===== 底部：清空历史 =====
        Button {
            horizontalAlignment: HorizontalAlignment.Center
            verticalAlignment: VerticalAlignment.Bottom
            preferredWidth: 220
            preferredHeight: 56
            bottomMargin: 14
            text: "清空历史"
            visible: dyHistoryModel.count > 0
            onClicked: {
                if (dyHistoryModel && dyHistoryModel.clear) {
                    dyHistoryModel.clear();
                }
            }
        }

        // ===== 空态 =====
        Container {
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Center
            visible: dyHistoryModel.count == 0
            leftPadding: 60
            rightPadding: 60
            layout: StackLayout {}

            Label {
                text: "还没有观看记录\n刷几个视频后回来看看"
                horizontalAlignment: HorizontalAlignment.Center
                textStyle.color: Color.create("#8E9095")
                textStyle.fontSize: FontSize.Small
                multiline: true
            }
        }
    }
}
