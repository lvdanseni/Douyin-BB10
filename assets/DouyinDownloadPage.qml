import bb.cascades 1.1

// 已下载页（1.0.0.37 新增）—— 用户要求"菜单栏里添加已下载"。
//
// 【入口】DouyinFeedPage 抽屉菜单「已下载」→ main.qml pushDownloads()。
//
// 【数据】dyDownloadStore（DouyinDownloadStore）：
//   - 索引 data/douyin_downloads.json + 文件 videos/<awemeId>.mp4，重启不丢；
//   - 行内 api 指针由 C++ 塞（封面解析同收藏/历史页）；
//   - 下载中行 state="downloading"，进度文字走 progressFor(id, dlVersion)
//     （版本锚点驱动绑定重算，QML 无 Timer，铁律）。
//
// 【点击播放】本地文件直播：把行的 playUrl 换成 "file://"+localPath、
//   videoSize 换成 fileBytes → host.playFeedRow(row) → feedHome.playRow →
//   DouyinPlayerStore::startStream(file://) → VideoProxy::runLocal
//   （seek/read 直服，不吃 CDN 直链过期，1.0.0.28 预取命中同机制）。
//
// 【行内删除】Button.clicked 可靠信号（contextActions 长按菜单在
//   ListItemComponent 独立作用域不可靠，BiliBar v91 教训），只引用
//   delegate 根节点 id（编译期解析）。
//
// 【铁律】ItemType 默认空串才匹配，切勿显式写 item 类型；自定义 list item 必须给 preferredWidth；
//   绑定不能写成块 { ... }；Label 没有 maxLines，多行用 multiline。
Page {
    id: dlPage

    /** 上一级 NavigationPane（main.qml push 时注入）。 */
    property variant nav: null
    /** 宿主 NavigationPane（main.qml）：playFeedRow 回调入口。 */
    property variant host: null

    content: Container {
        id: dlRoot
        horizontalAlignment: HorizontalAlignment.Fill
        verticalAlignment: VerticalAlignment.Fill
        background: theme.bg
        layout: DockLayout {}

        // ===== 已下载列表（先声明，z 序在顶栏之下） =====
        ListView {
            id: dlList
            objectName: "dyDlList"
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Fill
            topMargin: 84
            bottomMargin: 76
            dataModel: dyDownloadStore

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

                        // 封面缩略图（本地路径由 api 现场解析）。
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

                        // 右侧文字列（标题 + 作者 + 状态/大小/时间）。
                        Container {
                            layoutProperties: StackLayoutProperties {
                                spaceQuota: 1
                            }
                            leftMargin: 16
                            layout: StackLayout {}

                            Label {
                                text: ListItemData.title ? ListItemData.title : ""
                                textStyle.color: Color.create("#F2F3F5")
                                textStyle.fontSize: FontSize.Small
                                multiline: true
                            }

                            Label {
                                text: "@" + (ListItemData.authorName ? ListItemData.authorName : "抖音用户")
                                textStyle.color: Color.create("#8E9095")
                                textStyle.fontSize: FontSize.XSmall
                                topMargin: 4
                            }

                            // 状态行：下载中 = 实时进度（progressFor 带版本锚点）；
                            // 已完成 = 大小 + 时间。单表达式，禁块（铁律 5）。
                            Label {
                                text: (ListItemData.state === "downloading")
                                    ? dyDownloadStore.progressFor(ListItemData.awemeId, dyDownloadStore.dlVersion)
                                    : (dyDownloadStore.sizeText(ListItemData.fileBytes) + "  " +
                                       (ListItemData.timeText ? ListItemData.timeText : ""))
                                textStyle.color: (ListItemData.state === "downloading")
                                    ? Color.create("#FE2C55")
                                    : Color.create("#8E9095")
                                textStyle.fontSize: FontSize.XSmall
                                topMargin: 8
                            }
                        }

                        // 行内「删除」按钮（id 编译期解析，不受作用域影响）。
                        Container {
                            id: dlRow
                            preferredWidth: 116
                            verticalAlignment: VerticalAlignment.Center
                            layout: DockLayout {}

                            property string rowId: (ListItemData.awemeId) ? ListItemData.awemeId : ""

                            Button {
                                horizontalAlignment: HorizontalAlignment.Center
                                verticalAlignment: VerticalAlignment.Center
                                preferredWidth: 96
                                preferredHeight: 48
                                text: "删除"
                                onClicked: {
                                    if (dlRow.rowId.length > 0 && dyDownloadStore && dyDownloadStore.remove) {
                                        dyDownloadStore.remove(dlRow.rowId);
                                    }
                                }
                            }
                        }
                    }
                }
            ]

            onTriggered: {
                var row = dyDownloadStore.rowAt(indexPath);
                if (!row) {
                    return;
                }
                if (row.state === "downloading") {
                    if (dyDownloadStore && dyDownloadStore.cancel) dyDownloadStore.cancel(row.awemeId);
                    return;
                }
                // 本地文件直播：playUrl 换 file:// + videoSize 换真实字节。
                if (!row.localPath || row.localPath.length === 0) return;
                row.playUrl = "file://" + row.localPath;
                row.videoSize = row.fileBytes;
                var rp = "no-host";
                try {
                    if (dlPage.host && dlPage.host.playFeedRow) {
                        rp = dlPage.host.playFeedRow(row);
                    }
                } catch (e) {
                    rp = "EX:" + e;
                }
                if (app && app.swipeLog) {
                    app.swipeLog("DL-ITEM ret=" + rp);
                }
            }
        }

        // ===== 顶部条：返回 + 标题（条数 + 目录） =====
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
                    if (dlPage.nav && dlPage.nav.pop) {
                        dlPage.nav.pop();
                    }
                }
            }

            Label {
                text: dyDownloadStore.count > 0 ? ("已下载 " + dyDownloadStore.count) : "已下载"
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
            visible: dyDownloadStore.count == 0
            leftPadding: 60
            rightPadding: 60
            layout: StackLayout {}

            Label {
                text: "还没有下载视频\n刷视频时点右侧「下载」，下好的视频离线也能播"
                horizontalAlignment: HorizontalAlignment.Center
                textStyle.color: Color.create("#8E9095")
                textStyle.fontSize: FontSize.Small
                multiline: true
            }
        }

        // ===== 下载提示 toast（下载完成/失败/取消，3s 自消由 C++ 驱动）=====
        Container {
            horizontalAlignment: HorizontalAlignment.Center
            verticalAlignment: VerticalAlignment.Bottom
            bottomMargin: 120
            visible: (dyDownloadStore && dyDownloadStore.tip) ? dyDownloadStore.tip.length > 0 : false
            background: theme.toastBg
            leftPadding: 18
            rightPadding: 18
            topPadding: 10
            bottomPadding: 10
            preferredWidth: 560
            layout: DockLayout {}

            Label {
                horizontalAlignment: HorizontalAlignment.Fill
                verticalAlignment: VerticalAlignment.Center
                text: (dyDownloadStore && dyDownloadStore.tip) ? dyDownloadStore.tip : ""
                textStyle.color: theme.textPrimary
                textStyle.fontSize: FontSize.Small
                multiline: true
            }
        }
    }
}
