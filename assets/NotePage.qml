import bb.cascades 1.1

import "components"

// 笔记详情页 —— 对应 BBTube VideoPage（笔记详情 + 播放）
//
// 【数据绑定原则（关键，BB10/Qt4.8 可靠性）】
// 完全基于 Q_PROPERTY 的绑定：NoteStore 把 detail/error/dlPercent/commentModel
// 等暴露成属性，本页用属性绑定（titleLabel.text: noteStore.detail.title 等）消费，
// 属性 NOTIFY 在 QML 引擎内部驱动绑定，可靠。播放触发也移到了 C++（applicationui
// 里 videoReadyToPlay → videoPlayer.play 的编译期连接），QML 侧只负责显示与控制。
Page {
    id: notePage

    property string noteId: ""

    // 【必须声明】main.qml 的 Tab 里会赋 nav / host，本页 346 行也会读
    // notePage.nav / notePage.host 传给子页面。Cascades 1.1 对自定义组件
    // 赋未声明属性会直接报 "Cannot assign to non-existent property"，
    // 并让整个页面 Type unavailable（规则 17 拦截的正是这类）。
    property variant nav: null
    property variant host: null

    // 【空值兜底】1.0.0.6/1.0.0.7 设备日志里 noteStore.detail.images 为
    // undefined，直接取 .length / [0] 连抛 6 次 TypeError。绑成数组属性后
    // 绑定仍会追踪 noteStore.detail 的 NOTIFY，只是把 undefined 归一成 []。
    property variant imgs: noteStore.detail.images ? noteStore.detail.images : []

    // ---- 播放区尺寸 ----
    // 【v69】完全照 BBTube 的算法：
    //     playerBoxH = min(pageW * aspectH / aspectW, pageH)
    // 也就是"先按屏宽和视频比例算高度，超过屏高就夹到屏高"。
    property double screenW: ui.du(720)
    property double screenH: ui.du(720)
    property double videoAspect: notePage.safeAspect()
    property double videoH: Math.min(notePage.screenW / notePage.videoAspect, notePage.screenH)
    property double videoW: Math.min(notePage.videoH * notePage.videoAspect, notePage.screenW)

    // ---- 进度条状态 ----
    property double trackW: (trackArea && trackArea.width > 0) ? trackArea.width : ui.du(716)
    property bool scrubbing: false
    property int scrubMs: 0
    property int shownMs: notePage.scrubbing ? notePage.scrubMs : videoPlayer.positionMs
    property double progress: videoPlayer.durationMs > 0 ? Math.max(0, Math.min(1, notePage.shownMs / videoPlayer.durationMs)) : 0

    function msAt(x) {
        if (notePage.trackW <= 0 || videoPlayer.durationMs <= 0) return 0;
        var r = x / notePage.trackW;
        if (r < 0) r = 0;
        if (r > 1) r = 1;
        return Math.round(r * videoPlayer.durationMs);
    }

    function scrubTouch(event) {
        if (!event) return;
        if (videoPlayer.durationMs <= 0) return;
        if (event.touchType === TouchType.Down || event.touchType === TouchType.Move) {
            if (event.touchType === TouchType.Down) {
                videoPlayer.showControls();
            }
            notePage.scrubbing = true;
            notePage.scrubMs = notePage.msAt(event.localX);
        }
        if (event.touchType === TouchType.Up) {
            notePage.scrubbing = false;
            videoPlayer.seekTo(notePage.scrubMs);
        }
    }

    function safeAspect() {
        var a = noteStore.detail.aspect;
        if (a && a > 0.5 && a < 3) return a;
        return 16 / 9;
    }

    // 根容器：Cascades 1.1 Page 的 content 是单数属性，只能有一个直接子元素。
    // 把所有 UI 子元素包裹在一个 DockLayout 容器里，避免 "Cannot assign multiple values to a singular property"。
    Container {
        id: notePageRoot
        layout: DockLayout {}
        horizontalAlignment: HorizontalAlignment.Fill
        verticalAlignment: VerticalAlignment.Fill

        // ---- 播放面：ForeignWindowControl 承载 libbbplayer 的屏幕窗口 ----
    // windowId/windowGroup 必须与 C++ 侧一致（applicationui.cpp 注入的 videoPlayer）。
    // bbplayer_set_window 参数顺序是 (groupId, windowId)——组在前 ID 在后。
    Container {
        id: playerBox
        preferredWidth: notePage.videoW
        preferredHeight: notePage.videoH
        horizontalAlignment: HorizontalAlignment.Center
        verticalAlignment: VerticalAlignment.Top
        topPadding: ui.du(4)

        ForeignWindowControl {
            id: playerSurface
            windowId: videoPlayer.windowId
            windowGroup: videoPlayer.groupId
            preferredWidth: notePage.videoW
            preferredHeight: notePage.videoH
            horizontalAlignment: HorizontalAlignment.Center
            verticalAlignment: VerticalAlignment.Center
        }
    }

    // ---- 控制叠加层（播放按钮 / 进度条 / 时长标签） ----
    Container {
        id: controlLayer
        horizontalAlignment: HorizontalAlignment.Fill
        verticalAlignment: VerticalAlignment.Fill
        visible: videoPlayer.playing || videoPlayer.ctrlShown
        layout: DockLayout {}

        // 播放/暂停按钮（居中大图标）
        ImageView {
            id: playBtn
            horizontalAlignment: HorizontalAlignment.Center
            verticalAlignment: VerticalAlignment.Center
            preferredWidth: ui.du(80)
            preferredHeight: ui.du(80)
            imageSource: videoPlayer.playing ? "asset:///images/pause.png" : "asset:///images/play.png"
            scalingMethod: ScalingMethod.AspectFit
            onTouch: {
                if (event.touchType === TouchType.Down) {
                    videoPlayer.togglePlay();
                }
            }
        }

        // 进度条轨道
        Container {
            id: trackArea
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Bottom
            bottomPadding: ui.du(14)
            leftPadding: ui.du(8)
            rightPadding: ui.du(8)
            preferredHeight: ui.du(8)
            background: Color.create("#33FFFFFF")

            // 已播放进度填充
            Container {
                id: progressFill
                preferredWidth: notePage.progress * trackArea.width
                preferredHeight: ui.du(8)
                background: Color.create("#FB7299")
                horizontalAlignment: HorizontalAlignment.Left
                verticalAlignment: VerticalAlignment.Center
            }

            // 触摸拖动（Cascades 1.1 无 TouchBehaviour，改用 Control.onTouch 信号）
            onTouch: notePage.scrubTouch(event)
        }

        // 时间标签（当前 / 总时长）
        Container {
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Bottom
            bottomPadding: ui.du(32)
            leftPadding: ui.du(12)
            rightPadding: ui.du(12)
            layout: StackLayout {}

            Label {
                text: videoPlayer.positionMs > 0
                    ? (Math.floor(notePage.shownMs / 60000) + ":" +
                       ("0" + Math.floor((notePage.shownMs % 60000) / 1000)).slice(-2))
                    : "0:00"
                textStyle.color: Color.White
                textStyle.fontSize: FontSize.Small
                horizontalAlignment: HorizontalAlignment.Left
            }
            Label {
                text: videoPlayer.durationMs > 0
                    ? (Math.floor(videoPlayer.durationMs / 60000) + ":" +
                       ("0" + Math.floor((videoPlayer.durationMs % 60000) / 1000)).slice(-2))
                    : "0:00"
                textStyle.color: Color.White
                textStyle.fontSize: FontSize.Small
                horizontalAlignment: HorizontalAlignment.Right
            }
        }
    }

    // ---- 笔记内容区（标题 / 正文 / 作者 / 统计 / 图集） ----
    ScrollView {
        id: contentScroll
        scrollRole: ScrollRole.Main
        horizontalAlignment: HorizontalAlignment.Fill
        verticalAlignment: VerticalAlignment.Fill

        Container {
            topPadding: ui.du(16)
            bottomPadding: ui.du(16)
            leftPadding: ui.du(16)
            rightPadding: ui.du(16)
            layout: StackLayout {}

            // 标题
            Label {
                text: noteStore.detail.title
                textStyle.color: theme.textPrimary
                textStyle.fontSize: FontSize.Large
                textStyle.fontWeight: FontWeight.Bold
                multiline: true
                horizontalAlignment: HorizontalAlignment.Left
            }

            // 作者行
            Container {
                layout: DockLayout {}
                topPadding: ui.du(12)

                ImageView {
                    imageSource: noteStore.detail.ownerFace
                    preferredWidth: ui.du(24)
                    preferredHeight: ui.du(24)
                    scalingMethod: ScalingMethod.AspectFit
                    horizontalAlignment: HorizontalAlignment.Left
                    verticalAlignment: VerticalAlignment.Center
                }
                Label {
                    text: noteStore.detail.ownerName
                    textStyle.color: theme.textPrimary
                    textStyle.fontSize: FontSize.Small
                    horizontalAlignment: HorizontalAlignment.Left
                    verticalAlignment: VerticalAlignment.Center
                    leftPadding: ui.du(8)
                }
            }

            // 正文
            Label {
                text: noteStore.detail.desc
                textStyle.color: theme.textSecondary
                textStyle.fontSize: FontSize.Default
                multiline: true
                topPadding: ui.du(8)
                horizontalAlignment: HorizontalAlignment.Left
            }

            // 统计行
            Container {
                layout: DockLayout {}
                topPadding: ui.du(12)
                horizontalAlignment: HorizontalAlignment.Fill

                Label {
                    text: "赞 " + biliApi.countText(noteStore.detail.likeCount)
                    textStyle.color: theme.textSecondary
                    textStyle.fontSize: FontSize.Small
                    horizontalAlignment: HorizontalAlignment.Left
                }
                Label {
                    text: "收藏 " + biliApi.countText(noteStore.detail.collectCount)
                    textStyle.color: theme.textSecondary
                    textStyle.fontSize: FontSize.Small
                    horizontalAlignment: HorizontalAlignment.Center
                }
                Label {
                    text: "评论 " + biliApi.countText(noteStore.detail.commentCount)
                    textStyle.color: theme.textSecondary
                    textStyle.fontSize: FontSize.Small
                    horizontalAlignment: HorizontalAlignment.Right
                }
            }

            // 图集（如有）
            Container {
                id: imagesContainer
                topPadding: ui.du(12)
                visible: notePage.imgs && notePage.imgs.length > 0
                layout: StackLayout {}

                // 图片由 Repeater 渲染（Cascades 1.1 没有 Repeater，改用固定最多 9 张）
                ImageView {
                    imageSource: notePage.imgs.length > 0
                        ? biliApi.coverUrlFor(notePage.imgs[0], biliApi.coverVersion)
                        : ""
                    preferredWidth: notePage.screenW - ui.du(32)
                    scalingMethod: ScalingMethod.AspectFit
                    horizontalAlignment: HorizontalAlignment.Center
                    visible: notePage.imgs.length > 0
                }
                ImageView {
                    imageSource: notePage.imgs.length > 1
                        ? biliApi.coverUrlFor(notePage.imgs[1], biliApi.coverVersion)
                        : ""
                    preferredWidth: notePage.screenW - ui.du(32)
                    scalingMethod: ScalingMethod.AspectFit
                    horizontalAlignment: HorizontalAlignment.Center
                    topPadding: ui.du(8)
                    visible: notePage.imgs.length > 1
                }
                ImageView {
                    imageSource: notePage.imgs.length > 2
                        ? biliApi.coverUrlFor(notePage.imgs[2], biliApi.coverVersion)
                        : ""
                    preferredWidth: notePage.screenW - ui.du(32)
                    scalingMethod: ScalingMethod.AspectFit
                    horizontalAlignment: HorizontalAlignment.Center
                    topPadding: ui.du(8)
                    visible: notePage.imgs.length > 2
                }
            }

            // 互动按钮栏
            Container {
                layout: DockLayout {}
                topPadding: ui.du(20)
                horizontalAlignment: HorizontalAlignment.Fill

                Button {
                    text: noteStore.liked ? "已点赞" : "点赞"
                    horizontalAlignment: HorizontalAlignment.Left
                    onClicked: noteStore.toggleLike()
                }
                Button {
                    text: noteStore.collected ? "已收藏" : "收藏"
                    horizontalAlignment: HorizontalAlignment.Center
                    onClicked: noteStore.toggleCollect()
                }
                Button {
                    text: noteStore.followed ? "已关注" : "关注"
                    horizontalAlignment: HorizontalAlignment.Right
                    onClicked: noteStore.toggleFollow()
                }
            }

            // 操作反馈提示
            Label {
                text: noteStore.interactTip
                textStyle.color: theme.accent
                textStyle.fontSize: FontSize.Small
                topPadding: ui.du(8)
                horizontalAlignment: HorizontalAlignment.Center
                visible: noteStore.interactTip.length > 0
            }

            // 分隔线 + 相关推荐标题
            Container {
                topPadding: ui.du(24)
                horizontalAlignment: HorizontalAlignment.Fill
                background: theme.border
                preferredHeight: 1
            }
            Label {
                text: "相关推荐"
                textStyle.color: theme.textPrimary
                textStyle.fontSize: FontSize.Medium
                topPadding: ui.du(12)
                horizontalAlignment: HorizontalAlignment.Left
            }

            // 相关推荐列表（复用 NoteFeedPage）
            NoteFeedPage {
                model: noteStore.relatedModel
                nav: notePage.nav
                host: notePage.host
                preferredHeight: ui.du(300)
            }
        }
    }

    // 底部浮层：加载中 / 错误
    Container {
        horizontalAlignment: HorizontalAlignment.Fill
        verticalAlignment: VerticalAlignment.Fill
        background: Color.create("#88000000")
        visible: noteStore.loading || noteStore.error.length > 0
        layout: DockLayout {}

        Container {
            horizontalAlignment: HorizontalAlignment.Center
            verticalAlignment: VerticalAlignment.Center
            layout: StackLayout {}

            ActivityIndicator {
                running: noteStore.loading
                visible: noteStore.loading
            }
            Label {
                text: noteStore.error
                textStyle.color: Color.White
                textStyle.fontSize: FontSize.Small
                multiline: true
                topPadding: ui.du(8)
                visible: noteStore.error.length > 0
            }
        }
    }
    }  // 关闭根容器 notePageRoot

    // 返回信号：由 main.qml 的 pushNote 监听，pop 页面时停播放
    signal back

    onCreationCompleted: {
        if (noteId && noteId.length > 0) {
            noteStore.load(noteId);
        }
    }
}
