import bb.cascades 1.1


import "components"
// 【v89】已下载 —— 沙盒 videos/ 目录里的 mp4。
// 【v91】长按一行 → 系统菜单"删除" → 页面弹二次确认 → C++ 删文件并重扫目录。
//
// 【为什么不复用 FeedPage】FeedPage 绑的是一堆 HTTP 模型接口
// （loading / loadMore / error / bvidAt），而这里的数据源在本地文件系统，
// DownloadStore 只有 model / count / reload。硬套会为了迁就接口写一堆空实现，
// 不如单独一个 ListView —— 反正 delegate 还是同一个 VideoRow。
//
// 点一行直接进播放页：视频已在本地，VideoStore 的 playurl 之后会命中
// videos/<bvid>.mp4 缓存（downloadVideo 里那个"大小吻合就跳过下载"的分支），
// 所以是秒开，不再走流式。
Page {
    id: dlPage

    property variant nav: null
    property variant host: null

    titleBar: TitleBar {
        title: "已下载"
        acceptAction: ActionItem {
            title: "刷新"
            onTriggered: {
                downloadStore.reload();
            }
        }
    }

    content: Container {
        layout: DockLayout {}

        ListView {
            id: dlList
            dataModel: downloadStore.model
            scrollRole: ScrollRole.Main

            listItemComponents: [
                // 【不要写 type: "item"】ArrayDataModel 的 itemType 默认是空字符串，
                // 写了 type 就匹配不上，ListView 会退回默认 delegate，
                // 整页只剩一列 BV 号（详见 components/FeedPage.qml 的说明）。
                ListItemComponent {
                    VideoRow {
                        // 【v92】这个 id 是整条删除链路的关键。
                        // 长按菜单的 ActionItem 在 JS 函数体里只引用 dlRow，
                        // 而 id 解析是编译期完成的 —— 不受组件作用域影响。
                        id: dlRow

                        title: ListItemData.title
                        meta: ListItemData.metaText
                        // 【v103】picUrl 是远端 URL（永不变化），本地路径由
                        // VideoRow 内部用 api.coverUrlFor 解析 —— 补下载完成后
                        // coverVersion 一变，绑定自己重算，不依赖模型刷新。
                        picUrl: ListItemData.picUrl
                        durationText: ListItemData.durationText
                        api: ListItemData.api

                        // 这两个走属性绑定：绑定一定求值，而且上下文就是 delegate
                        // 的数据上下文，ListItemData 在这里 100% 可用。
                        store: ListItemData.store
                        rowBvid: ListItemData.bvid

                        // 【v91/v92】长按菜单。
                        //
                        // 用系统 contextActions 而不是自己在 onTouch 里算长按时长：
                        // 长按手势由 ListView 原生识别，不会和滚动手势打架；
                        // 自己实现的话，手指稍微一动就被判定成滚动，长按永远触发不了。
                        //
                        // contextActions 的类型是 QDeclarativeListProperty<ActionSet>
                        // （见 control.h:506），所以必须包一层 ActionSet，不能直接放 ActionItem。
                        contextActions: [
                            ActionSet {
                                actions: [
                                    ActionItem {
                                        title: "删除"
                                        onTriggered: {
                                            // 只登记待删项，真删等确认层。
                                            // 删除不可逆，不能一次长按就没了。
                                            //
                                            // 【不要在这里写 downloadStore.xxx 或
                                            // ListItemData.bvid】—— v91 就是这么写的，
                                            // 长按菜单能弹、点删除完全没反应，
                                            // 因为这两者在 ActionItem 的函数作用域里
                                            // 都不保证可见。改用根节点 id + 属性绑定。
                                            if (dlRow.store && dlRow.rowBvid.length > 0) {
                                                dlRow.store.requestDelete(dlRow.rowBvid);
                                                return;
                                            }
                                            // 兜底：万一 QVariant 里的 QObject* 没被 QML
                                            // 包装成可调用对象，再走一次 context property。
                                            // （v91 实测这条不可靠，只作保险，不指望它。）
                                            try {
                                                downloadStore.requestDelete(dlRow.rowBvid);
                                            } catch (e) {
                                                console.log("BerryLi delete: " + e);
                                            }
                                        }
                                    }
                                ]
                            }
                        ]
                    }
                }
            ]

            onTriggered: {
                var row = dlList.dataModel ? dlList.dataModel.data(indexPath) : null;
                var bvid = (row && row.bvid) ? row.bvid : "";
                if (bvid.length == 0) return;
                if (!dlPage.host || !dlPage.nav) return;
                // 和 FeedPage 一样：建页 + push 都交给 main.qml 根节点的函数
                // （ComponentDefinition 与 createObject 必须在同一文档作用域）。
                dlPage.host.pushVideo(dlPage.nav, bvid);
            }
        }

        // 空状态。必须放在 ListView 之后（DockLayout 里后声明的在上层），
        // 否则会被列表盖住。
        Container {
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Fill
            background: Color.create("#17181A")
            visible: downloadStore.empty
            layout: DockLayout {}

            Container {
                horizontalAlignment: HorizontalAlignment.Fill
                verticalAlignment: VerticalAlignment.Center
                leftPadding: ui.du(40)
                rightPadding: ui.du(40)

                Label {
                    text: "还没有下载任何视频"
                    horizontalAlignment: HorizontalAlignment.Center
                    textStyle.color: Color.create("#F2F3F5")
                    textStyle.fontSize: FontSize.Large
                }
                Label {
                    text: "在播放页点一下「下载」，下好的视频会出现在这里，离线也能看。"
                    horizontalAlignment: HorizontalAlignment.Center
                    multiline: true
                    topMargin: ui.du(8)
                    textStyle.color: Color.create("#9CA0A6")
                }
            }
        }

        // ================= 删除确认层 =================
        //
        // 【为什么状态在 C++】长按菜单声明在 ListItemComponent 里，那是独立组件作用域，
        // 拿不到本页的 id（dlPage），没法回调这里的函数来弹层。
        // 所以 DownloadStore 用 pendingBvid / pendingTitle 两个属性描述"待删的是谁"，
        // 本页只做属性绑定 —— 不需要任何跨作用域调用。
        //
        // 必须是 DockLayout 里最后声明的几个之一：后声明 = 在上层，否则会被列表盖住。
        Container {
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Fill
            background: Color.create("#CC000000")
            visible: downloadStore.pendingBvid !== ""
            layout: DockLayout {}

            Container {
                horizontalAlignment: HorizontalAlignment.Center
                verticalAlignment: VerticalAlignment.Center
                preferredWidth: ui.du(580)
                background: Color.create("#26272B")
                leftPadding: ui.du(26)
                rightPadding: ui.du(26)
                topPadding: ui.du(24)
                bottomPadding: ui.du(18)

                Label {
                    text: "删除下载？"
                    textStyle.color: Color.create("#F2F3F5")
                    textStyle.fontSize: FontSize.Large
                }
                Label {
                    // 只显示 BV 号的话用户认不出是哪个视频，所以标题由 C++ 反查后给出。
                    text: "「" + downloadStore.pendingTitle + "」"
                    topMargin: ui.du(10)
                    multiline: true
                    textFormat: TextFormat.Plain
                    textStyle.color: Color.create("#F2F3F5")
                }
                Label {
                    text: "视频文件会从本机移除，之后可以重新下载。"
                    topMargin: ui.du(8)
                    multiline: true
                    textStyle.color: Color.create("#9CA0A6")
                    textStyle.fontSize: FontSize.Small
                }

                Container {
                    horizontalAlignment: HorizontalAlignment.Fill
                    topMargin: ui.du(20)
                    layout: StackLayout {
                        orientation: LayoutOrientation.LeftToRight
                    }

                    // 用 Button 而不是自绘 Container：onClicked 是同文档信号，
                    // 在 BB10 上可靠；自绘的话要自己处理 touch 的 up/cancel，
                    // 手指滑出按钮范围就会漏掉。
                    Button {
                        text: "取消"
                        layoutProperties: StackLayoutProperties {
                            spaceQuota: 1
                        }
                        onClicked: {
                            downloadStore.cancelDelete();
                        }
                    }
                    Button {
                        text: "删除"
                        layoutProperties: StackLayoutProperties {
                            spaceQuota: 1
                        }
                        onClicked: {
                            downloadStore.confirmDelete();
                        }
                    }
                }
            }
        }

        // ================= 操作提示 =================
        // 删除完成后列表会少一行，但用户不一定注意到，所以再给一条短提示。
        // 文本和自动消失都由 C++ 控制（QML 里没有 Timer 类型）。
        Container {
            horizontalAlignment: HorizontalAlignment.Center
            verticalAlignment: VerticalAlignment.Bottom
            bottomMargin: ui.du(30)
            visible: downloadStore.tip !== ""
            background: Color.create("#E626272B")
            leftPadding: ui.du(18)
            rightPadding: ui.du(18)
            topPadding: ui.du(9)
            bottomPadding: ui.du(9)

            Label {
                text: downloadStore.tip
                textFormat: TextFormat.Plain
                textStyle.color: Color.create("#F2F3F5")
                textStyle.fontSize: FontSize.Small
            }
        }
    }
}
