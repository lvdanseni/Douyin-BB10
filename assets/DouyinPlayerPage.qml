import bb.cascades 1.1

// 抖音风格播放页。
//
// 【与 BiliBar VideoPage 的关系】
//   布局抄的是 VideoPage.qml 的 v76 版本——ForeignWindowControl **内联在这里**，
//   不走 ComponentDefinition 间接（那层间接从没带来收益，只多一个 null 失败点）。
//   windowId / windowGroup / updatedProperties 三个属性必须全部写全，否则
//   Cascades 不会把位置尺寸同步给屏幕窗口，表现为视频贴在左上角、大小不对。
//
// 【播放触发链路（零信号中转，铁律 4）】
//   main.qml pushDyVideo(nav, row) 前先把 page.videoRow = row，
//   页面创建后 attachRow() 同步调用 dyPlayerStore.load(row)。
//   C++ 拿到 CDN 直链后 emit videoReadyToPlay(QString) → ApplicationUI 用
//   编译期 connect 转给 videoPlayer.play() —— 中间不经过任何 QML 转发
//   （Qt 4.8 上 C++ 信号 → QML 处理函数会静默不连接）。
//
// 【时长来源】DouyinPlayerStore::load 会 emit apiDurationReady(秒)，
//   同样由 ApplicationUI 编译期 connect 进播放器，不依赖 libbbplayer 自带的
//   duration 字段（那个经常为 0，进度条会满格跳）。
// 【根节点必须是 Page】Cascades 1.1 的 NDK 里只有 page.h / abstractpane.h /
// navigationpane.h / tabbedpane.h，没有 pane.h，QML 也没有注册 `Pane` 类型。
// 写成 Pane 会让本页 Type unavailable（createObject 时静默失败，点视频无反应）。
// ground truth：真机可运行的 BiliClient.bar / BiliBar 的 VideoPage.qml 根节点都是 Page。
Page {
    id: videoPage
    // C++ 的 ApplicationUI::onPagePopped 靠 objectName 认页面（系统返回手势
    // 不经过下面的按钮），pop 时统一停播放器 + 断流。别改这个字符串。
    objectName: "DouyinPlayerPage"

    // 宿主导航（main.qml 在 createObject 后设置）
    property variant nav: null
    // FeedModel 的整行数据（含 awemeId / title / picUrl / playUrl / durationSec…）
    property variant videoRow: null

    // 【1.0.0.15】目标设备 BlackBerry Q20 (SQC100-4) 是 1:1 方屏，物理 720x720，
    // Cascades 的 du 与 px 1:1。旧值 1280 抄自 Z10 (720x1280)，在方屏上
    // videoH = min(720/0.5625, 1280) = 1280，播放面被拉成 720x1280，画面比例全错。
    // 9:16 视频在 1:1 屏上唯一正确的画法：高 720、宽 405，左右黑边。
    // （换其他设备部署时改这一个数即可。）
    property double screenW: 720
    property double screenH: 720
    // 竖屏视频 9:16 = 0.5625；横屏 16:9 = 0.5625 的倒数。
    // dyPlayerStore::portrait 来自 feed 行里 video.width/height 的比较。
    property double videoAspect: dyPlayerStore.portrait ? 0.5625 : 1.7777
    // 播放面高度：方屏竖屏视频 = min(720/0.5625, 720) = 720（高撑满，宽自动收窄）。
    property double videoH: Math.min(videoPage.screenW / videoPage.videoAspect, videoPage.screenH)
    // 宽度 = 高度 × 比例，最后夹一次屏宽：比例异常也不允许播放面宽于屏幕。
    // ⚠️ 不能写成块绑定（Qt 4.8 把 { 当对象字面量），三元/|| 一律压成单行。
    property double videoW: Math.min(videoPage.videoH * videoPage.videoAspect, videoPage.screenW)

    // 【Page 没有 background 属性】1.1 的 page.h 里没有 background Q_PROPERTY
    // （只有 Container 有：container.h:99）。底色放在下面那个铺满的 Container 上。

    // 布局完成 → 入数据。放 onCreationCompleted 而不是在 nav.push 前调用，
    // 保证 load() 时 QML 侧属性（videoAspect 等）已经能读到。
    onCreationCompleted: {
        videoPage.attachRow();
    }

    // 【1.0.0.15 真机实证的第二个坑】createObject() 返回时 onCreationCompleted
    // 就已经触发完，而 main.qml 的 pushDyVideo() 是在 createObject **之后**才
    // page.videoRow = row —— 老代码在 onCreationCompleted 里 attachRow() 拿到的
    // videoRow 恒为 null，播放页被 push 了却从不 load()。
    // 证据：1.0.0.15 设备日志 pushPage ("14","381") 之后没有任何 startStream。
    // 所以必须在 videoRow 属性变化时再补一次 attachRow（main.qml 现在是先 push
    // 再赋值，赋值那一刻页面已经在导航栈里，ForeignWindowControl 也能认领窗口）。
    // attachedId 守卫保证同一行只 load 一次（creation 路径 + 属性路径不重复拉流）。
    property string attachedId: ""
    onVideoRowChanged: {
        videoPage.attachRow();
    }

    Container {
        horizontalAlignment: HorizontalAlignment.Fill
        verticalAlignment: VerticalAlignment.Fill
        background: Color.Black
        layout: DockLayout {}

        // ---- 播放面（v76：内联 FWC）----
        Container {
            id: playerBox
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Fill
            background: Color.Black
            layout: DockLayout {}

            Container {
                id: stage
                horizontalAlignment: HorizontalAlignment.Fill
                verticalAlignment: VerticalAlignment.Fill
                layout: DockLayout {}

                ForeignWindowControl {
                    id: surface
                    horizontalAlignment: HorizontalAlignment.Center
                    // 【1.0.0.15】按算好的播放面尺寸居中：方屏上 9:16 视频 = 405x720，
                    // 左右自然留黑边；横屏视频则上下留黑边。不再用 Fill（Fill 会把
                    // 播放面拉满整屏，9:16 源被硬拉变形 —— 用户实测比例不对的根因之一）。
                    verticalAlignment: VerticalAlignment.Center
                    // 宽度按视频真实比例算：竖屏收窄居中，横屏铺满。
                    preferredWidth: videoPage.videoW
                    preferredHeight: videoPage.videoH
                    // windowId 为空串时 FWC 不认领。videoPlayer.windowId 构造期已算好。
                    windowId: videoPlayer.windowId.length > 0 ? videoPlayer.windowId : ""
                    // 必须与 libbbplayer join 的那个组是同一字符串，否则认领不到。
                    windowGroup: videoPlayer.groupId.length > 0 ? videoPlayer.groupId : ""
                    // 三个属性必须写全，否则位置尺寸不会同步给屏幕窗口。
                    updatedProperties: WindowProperty.Size | WindowProperty.Position | WindowProperty.Visible
                    // 必须保持可见才能去认领/绑定屏幕窗口。写成 boundToWindow 会永远绑不上。
                    visible: true
                }
            }
        }

        // ---- 顶部条：返回 + 标题 ----
        Container {
            id: topbar
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Top
            preferredHeight: 108
            topPadding: 24
            leftPadding: 16
            rightPadding: 16
            background: Color.create("#66000000")
            layout: DockLayout {}

            // 返回：显式按钮，不用系统返回手势 —— 系统 pop 时拿不到通知
            // （Page.onPopTransitionEnded 在 Cascades 1.1 不可用），
            // 所以必须在这里先 stopStream() 再 pop()，否则播放器还在拉流。
            Button {
                horizontalAlignment: HorizontalAlignment.Left
                verticalAlignment: VerticalAlignment.Center
                preferredWidth: 72
                preferredHeight: 72
                text: "\u00D7"
                onClicked: {
                    // 同步直调，不经信号中转（铁律 4）。main.qml 的 pushDyVideo
                    // 不再挂 page.back.connect —— 本页没声明那个 signal。
                    if (dyPlayerStore && dyPlayerStore.stopStream) dyPlayerStore.stopStream();
                    if (videoPlayer && videoPlayer.stop) videoPlayer.stop();
                    if (videoPage.nav) videoPage.nav.pop();
                }
            }

            Label {
                horizontalAlignment: HorizontalAlignment.Center
                verticalAlignment: VerticalAlignment.Center
                text: dyPlayerStore.title
                textStyle.color: Color.White
                textStyle.fontSize: FontSize.Small
                textStyle.fontWeight: FontWeight.Bold
                multiline: true
                leftMargin: 80
                rightMargin: 80
            }

            Label {
                horizontalAlignment: HorizontalAlignment.Right
                verticalAlignment: VerticalAlignment.Center
                text: dyPlayerStore.durationSec > 0 ? Math.floor(dyPlayerStore.durationSec) + "s" : ""
                textStyle.color: Color.create("#CCFFFFFF")
                textStyle.fontSize: FontSize.XSmall
            }
        }

        // ---- 底部信息条：作者 + 标题 + 数据 ----
        Container {
            id: bottombar
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Bottom
            preferredHeight: 260
            bottomPadding: 30
            leftPadding: 20
            rightPadding: 20
            background: Color.create("#88000000")
            layout: DockLayout {}

            Container {
                horizontalAlignment: HorizontalAlignment.Left
                verticalAlignment: VerticalAlignment.Fill
                preferredWidth: 480
                layout: StackLayout {}

                Label {
                    text: "@" + (dyPlayerStore.authorName.length > 0 ? dyPlayerStore.authorName : "抖音用户")
                    textStyle.color: Color.White
                    textStyle.fontSize: FontSize.Medium
                    textStyle.fontWeight: FontWeight.Bold
                    bottomMargin: 6
                }
                Label {
                    text: dyPlayerStore.title
                    textStyle.color: Color.create("#E8E8E8")
                    textStyle.fontSize: FontSize.Small
                    multiline: true
                    bottomMargin: 8
                }
                Label {
                    text: videoPage.htmlcount2(dyPlayerStore.likeCount) + "赞  " +
                          videoPage.htmlcount2(dyPlayerStore.commentCount) + "评论"
                    textStyle.color: Color.create("#C0C0C6")
                    textStyle.fontSize: FontSize.XSmall
                    multiline: true
                }
            }

            // 右侧竖排操作（播放页里 Button 的 clicked 是可靠信号）
            Container {
                horizontalAlignment: HorizontalAlignment.Right
                verticalAlignment: VerticalAlignment.Fill
                preferredWidth: 200
                rightMargin: 4
                layout: StackLayout {
                    orientation: LayoutOrientation.LeftToRight
                }

                Button {
                    preferredWidth: 56
                    preferredHeight: 56
                    text: dyPlayerStore.liked ? "\u2665" : "\u2661"
                    onClicked: {
                        dyPlayerStore.toggleLike();
                    }
                    horizontalAlignment: HorizontalAlignment.Right
                }
                Button {
                    preferredWidth: 56
                    preferredHeight: 56
                    text: dyPlayerStore.collected ? "\u2605" : "\u2606"
                    onClicked: {
                        dyPlayerStore.toggleCollect();
                    }
                    horizontalAlignment: HorizontalAlignment.Right
                }
                Button {
                    preferredWidth: 56
                    preferredHeight: 56
                    text: dyPlayerStore.authorFollowed ? "已关注" : "关注"
                    onClicked: {
                        dyPlayerStore.toggleFollow();
                    }
                    horizontalAlignment: HorizontalAlignment.Right
                }
            }
        }

        // ---- 状态层：加载中 / 错误 ----
        Container {
            id: hud
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Center
            leftPadding: 60
            rightPadding: 60
            topPadding: 40
            bottomPadding: 40
            // 只在有信息时占布局：空提示会让画面"糊着一层透明块"（v74 教训）。
            visible: (dyPlayerStore.loading || dyPlayerStore.error.length > 0 || dyPlayerStore.tip.length > 0)
            background: Color.create("#CC000000")
            layout: StackLayout {}

            ActivityIndicator {
                id: hudSpin
                horizontalAlignment: HorizontalAlignment.Center
                preferredWidth: 120
                preferredHeight: 120
                running: dyPlayerStore.loading
                // 没有加载任务时整个隐藏：空 ActivityIndicator 仍然参与布局。
                visible: dyPlayerStore.loading
            }

            Label {
                id: hudErr
                horizontalAlignment: HorizontalAlignment.Center
                text: dyPlayerStore.error.length > 0 ? dyPlayerStore.error : dyPlayerStore.tip
                textStyle.color: Color.create("#F2F3F5")
                textStyle.fontSize: FontSize.Small
                multiline: true
                // 与上方的转圈不重复露面：加载中转圈，出错看文字。
                visible: (!dyPlayerStore.loading) && (dyPlayerStore.error.length > 0 || dyPlayerStore.tip.length > 0)
            }

            Button {
                horizontalAlignment: HorizontalAlignment.Center
                text: "重试播放"
                visible: (!dyPlayerStore.loading) && (dyPlayerStore.error.length > 0)
                onClicked: {
                    // 有原始行数据就整行重入（走 feed→play_addr 的捷径）；
                    // 否则只按 awemeId 重新拉详情。
                    videoPage.attachRow();
                }
            }
        }
    }

    /**
     * 把 feed 行数据灌进 DouyinPlayerStore。
     * 整行给：C++ 侧优先直接用行里的 playUrl 播放（免 iteminfo 二次请求）。
     * attachedId 守卫：onCreationCompleted 与 onVideoRowChanged 两条路径都可能
     * 进来，同一 awemeId 只拉一次流。
     */
    function attachRow() {
        if (!videoRow || !dyPlayerStore || !dyPlayerStore.load) {
            return;
        }
        var id = videoRow.awemeId;
        if (!id || id.length === 0) {
            return;
        }
        if (id === videoPage.attachedId) {
            return;
        }
        videoPage.attachedId = id;
        dyPlayerStore.load(videoRow);
    }

    /**
     * 大数字缩写：12345 → 1.2万。文案里直接调用，不进绑定表达式
     * （同样是为了避开块绑定）。
     */
    function htmlcount2(n) {
        if (n < 0) return "";
        if (n < 10000) return "" + n;
        var w = n / 10000.0;
        if (w < 10) return (Math.round(w * 10) / 10.0) + "万";
        return Math.round(w) + "万";
    }
}
