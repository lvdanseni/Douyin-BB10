import bb.cascades 1.1

// 收藏页（1.0.0.37 新增）—— 用户要求"菜单里添加收藏"。
//
// 【入口】DouyinFeedPage 抽屉菜单「收藏」→ main.qml pushFavorites()：
//   createObject() → push（favDef 与 pushFavorites 同在 main.qml 根节点作用域）。
//
// 【数据】dyFavModel（DouyinFavoriteModel）：douyin_favorite.json 持久化，
//   feed 页右侧操作列「☆ 收藏」按钮 toggle（favVersion 驱动按钮文字实时变），
//   行内 api 指针由 C++ add()/loadFile() 塞好 → ListItemComponent 用
//   ListItemData.api.coverUrlFor 解析封面本地路径（context property 在
//   delegate 里不可见，DouyinHistoryPage 同款结论）。
//
// 【点击复播】ListView.onTriggered → host.playFeedRow(row)：main.qml 先 pop
//   回推荐页，再 feedHome.playRow(row)。收藏行可能存着已过期的 CDN 直链，
//   失败时 HUD 报错 + 「重试播放」（换新地址，1.0.0.37）兜住。
//
// 【行内删除】不用 contextActions 长按菜单：ListItemComponent 是独立作用域，
//   长按菜单的 ActionItem 里跨作用域调用不可靠（BiliBar v91 教训）。改用
//   delegate 内的「取消收藏」Button —— Button.clicked 是验证过的可靠信号，
//   只引用 delegate 根节点 id（编译期解析，不受作用域影响）。
//
// 【铁律】ItemType 默认空串才匹配，切勿显式写 item 类型；自定义 list item 必须给 preferredWidth；
//   绑定不能写成块 { ... }；Label 没有 maxLines，多行用 multiline。
Page {
    id: favPage

    /** 上一级 NavigationPane（main.qml push 时注入）。 */
    property variant nav: null
    /** 宿主 NavigationPane（main.qml）：playFeedRow 回调入口。 */
    property variant host: null

    content: Container {
        id: favRoot
        horizontalAlignment: HorizontalAlignment.Fill
        verticalAlignment: VerticalAlignment.Fill
        background: theme.bg
        layout: DockLayout {}

        // ===== 收藏列表（先声明，z 序在顶栏之下） =====
        ListView {
            id: favList
            objectName: "dyFavList"
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Fill
            topMargin: 84
            bottomMargin: 76
            dataModel: dyFavModel

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

                        // 行内「取消收藏」按钮（id 编译期解析，不受作用域影响）。
                        Container {
                            id: favRow
                            preferredWidth: 116
                            verticalAlignment: VerticalAlignment.Center
                            layout: DockLayout {}

                            property string rowId: (ListItemData.awemeId) ? ListItemData.awemeId : ""
                            // favStore 指针由 C++ 随行塞（和 api 同款原因）。
                            property variant rowStore: (ListItemData.favStore) ? ListItemData.favStore : null

                            Button {
                                horizontalAlignment: HorizontalAlignment.Center
                                verticalAlignment: VerticalAlignment.Center
                                preferredWidth: 108
                                preferredHeight: 48
                                text: "取消收藏"
                                onClicked: {
                                    if (favRow.rowStore && favRow.rowId.length > 0) {
                                        favRow.rowStore.remove(favRow.rowId);
                                    }
                                }
                            }
                        }
                    }
                }
            ]

            onTriggered: {
                var row = dyFavModel.rowAt(indexPath);
                if (!row) {
                    return;
                }
                // try/catch 兜底：复播失败（pop 中/playRow 抛异常）也要落日志。
                var rf = "no-host";
                try {
                    if (favPage.host && favPage.host.playFeedRow) {
                        rf = favPage.host.playFeedRow(row);
                    }
                } catch (e) {
                    rf = "EX:" + e;
                }
                if (app && app.swipeLog) {
                    app.swipeLog("FAV-ITEM ret=" + rf);
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
                    if (favPage.nav && favPage.nav.pop) {
                        favPage.nav.pop();
                    }
                }
            }

            Label {
                text: dyFavModel.count > 0 ? ("收藏 " + dyFavModel.count) : "收藏"
                horizontalAlignment: HorizontalAlignment.Fill
                verticalAlignment: VerticalAlignment.Center
                textStyle.color: theme.textPrimary
                textStyle.fontSize: FontSize.Medium
                textStyle.fontWeight: FontWeight.Bold
            }
        }

        // ===== 空态 =====
        Container {
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Center
            visible: dyFavModel.count == 0
            leftPadding: 60
            rightPadding: 60
            layout: StackLayout {}

            Label {
                text: "还没有收藏\n刷视频时点右侧「☆ 收藏」，这里就能找回它"
                horizontalAlignment: HorizontalAlignment.Center
                textStyle.color: Color.create("#8E9095")
                textStyle.fontSize: FontSize.Small
                multiline: true
            }
        }
    }
}
