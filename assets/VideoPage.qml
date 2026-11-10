import bb.cascades 1.1

// 复用首页的大封面卡片（components/VideoRow.qml）做"相关推荐"列表。
// VideoPage.qml 在 assets 根目录，components 是它的子目录。
import "components"


// 视频详情 + 播放页 —— 对应 BBTube VideoPage。
//
// 【数据绑定原则（关键，BB10/Qt4.8 可靠性）】
// 早期版本在 onCreationCompleted 里用 videoStore.detailReady.connect(handler) 等
// "C++ 信号 → QML 处理函数" 的连接方式，而 BB10 的 Qt 4.8 上这种跨语言 .connect()
// 会静默失效（信号发了但 JS 处理函数永不触发），表现为"详情/视频都出不来、整页卡死"。
//
// 现改为完全基于 Q_PROPERTY 的绑定：VideoStore 把 detail/error/dlPercent/commentModel
// 等暴露成属性，本页用属性绑定（titleLabel.text: videoStore.detail.title 等）消费，
// 属性 NOTIFY 在 QML 引擎内部驱动绑定，可靠。播放触发也移到了 C++（applicationui
// 里 videoReadyToPlay → videoPlayer.play 的编译期连接），QML 侧只负责显示与控制。

Page {
    id: videoPage
    property string bvid: ""
    // 【v76】原来的 `property variant player` 已删除 —— 播放面改成内联的
    // ForeignWindowControl，不再由 main.qml 创建后注入（原因见 stage 处的注释）。
    // 下载速度文本（"2.4 MB/s · 剩 12s"），由 downloadSpeed 信号更新
    property string dlSpeed: ""

    // ---- 【v63】认领重挂逻辑已全部移除 ----
    // v57~v61 那一整套"按轮询 remove/add 重挂播放面，逼 ForeignWindowControl 重新扫描"
    // 的代码，是为了绕开一个错误诊断（真因是 set_window 的组/ID 参数顺序反了，
    // 屏幕窗口压根没建出来）。v62 真机结果显示 claim=0 —— 一次重挂都没发生，
    // FWC 在窗口出现时自己就绑上了。所以这套逻辑连同它的计数属性一起删掉，
    // 顺便消除 remove/add 在转场期间可能造成的布局抖动。

    // ---- 播放区尺寸 ----
    // 【v69】完全照 BBTube 的算法：
    //     playerBoxH = min(pageW * aspectH / aspectW, pageH)
    // 也就是"先按屏宽和视频比例算高度，超过屏高就夹到屏高"。
    // 旧版用 0.62 * 屏高 封顶，竖屏视频只占 446du，跟 BBTube 差得很远；
    // BBTube 对 9:16 竖屏是直接占满一屏（720du），详情靠滚动去看。
    // Q10 是 720x720 方屏，去掉 TitleBar 后内容区就是整屏。
    property double screenW: ui.du(720)
    property double screenH: ui.du(720)
    // 【v66】宽高比来源改为 view 接口的 dimension（见 VideoStore::onViewReply）。
    // 之前读 videoPlayer.videoAspect，而那个 C++ 属性从未被赋值 → 永远返回
    // 兜底 16:9 → 9:16 竖屏视频按 16:9 算尺寸 → 画面被左右拉伸。
    property double videoAspect: videoPage.safeAspect()
    // 16:9 → 405du；9:16 竖屏 → 夹到 720du（占满一屏）
    property double videoH: Math.min(videoPage.screenW / videoPage.videoAspect, videoPage.screenH)
    // 播放面宽度 = 高度 × 宽高比。横屏时正好等于屏宽；竖屏时远小于屏宽，
    // 由 PlayerSurface 的 horizontalAlignment: Center 居中，左右自然露出黑底。
    // 末尾再夹一次屏宽：任何比例异常都不允许播放面宽于屏幕（那必然意味着拉伸）。
    //
    // ⚠️ 这里**不能**写成块绑定：
    //     property double videoW: {
    //         var w = ...; return ...;
    //     }
    // Qt 4.8 的 QML 解析器会把 `{` 当成对象字面量，块里的 `}` 收尾后遇到下一个
    // token（`function`）直接报 Syntax error，真机表现是**整个播放页打不开**
    // （QmlDocument: error when loading QML from VideoPage.qml ... 50:13）。
    // 单行表达式 + Math.min 才是安全的写法。
    property double videoW: Math.min(videoPage.videoH * videoPage.videoAspect, videoPage.screenW)

    // ---- 【v67】进度条状态（复刻 BBTube VideoControls 的拖动逻辑） ----
    // 进度条可用宽度 = 触摸区（trackArea）的实际宽度。
    // 【v85】原来写死 ui.du(716)（假设屏宽 720 减去左右各 2du padding）。
    // 一旦真实宽度和它不一致，msAt() 的分母就错，拖到哪儿都整体偏移 ——
    // 这也是"拖动不准"的来源之一。改成绑定真实宽度，算不出来时才回落 716。
    property double trackW: (trackArea && trackArea.width > 0) ? trackArea.width : ui.du(716)
    // 手指按在进度条上（拖动中）
    property bool scrubbing: false
    // 拖动中的目标位置（毫秒）
    property int scrubMs: 0
    // 时间文本显示哪个值：拖动时显示手指位置，否则显示播放器真实位置
    property int shownMs: videoPage.scrubbing ? videoPage.scrubMs : videoPlayer.positionMs
    property double progress: videoPlayer.durationMs > 0 ? Math.max(0, Math.min(1, videoPage.shownMs / videoPlayer.durationMs)) : 0

    // 手指的 x 坐标 → 毫秒
    function msAt(x) {
        if (videoPage.trackW <= 0 || videoPlayer.durationMs <= 0) return 0;
        var r = x / videoPage.trackW;
        if (r < 0) r = 0;
        if (r > 1) r = 1;
        return Math.round(r * videoPlayer.durationMs);
    }

    // 进度条拖动。BBTube 在拖动中会每 250ms 做一次关键帧预览 seek，
    // 我们不做：libbbplayer 的 seek 要重开解码器，边拖边 seek 会卡成幻灯片。
    // 只在手指抬起时跳一次，位置用抬起那一刻记录的 scrubMs
    // （touch Up 的 localX 在拖动后不可信，这是 BB10 的已知行为）。
    function scrubTouch(event) {
        if (!event) return;
        if (videoPlayer.durationMs <= 0) return;
        if (event.touchType === TouchType.Down || event.touchType === TouchType.Move) {
            if (event.touchType === TouchType.Down) {
                // 重置 4 秒自动隐藏的计时器，别让进度条在拖到一半时消失
                videoPlayer.showControls();
            }
            videoPage.scrubbing = true;
            videoPage.scrubMs = videoPage.msAt(event.localX);
        } else if (event.touchType === TouchType.Up) {
            if (!videoPage.scrubbing) return;
            videoPage.scrubbing = false;
            // 【v85】改成绝对拖动 seekTo(目标毫秒)。
            // 原来传的是 `scrubMs - positionMs` 这个差值，而 positionMs 在手指抬起
            // 那一刻可能刚被 250ms 的轮询改过，差值就算偏了 —— 表现是"拖到 50%
            // 却跳到了别处"。绝对目标没有中间量，不会有这个问题。
            videoPlayer.seekTo(videoPage.scrubMs);
        } else if (event.touchType === TouchType.Cancel) {
            videoPage.scrubbing = false;
        }
    }

    // 手柄的水平偏移：让它中心对准进度点，并且不越出两端
    function knobX() {
        var k = videoPage.scrubbing ? ui.du(9) : ui.du(7);
        var x = videoPage.trackW * videoPage.progress - k / 2;
        var maxX = videoPage.trackW - k;
        if (x < 0) x = 0;
        if (x > maxX) x = maxX;
        return x;
    }

    // ---- 【v85】药丸按钮（点赞/投币/收藏/分享/关注）的统一点击判定 ----
    //
    // 为什么不在 TouchType.Down 上直接执行：这几个按钮只有 28du 高，
    // 用户想上下滚动页面时手指很可能正好落在它们上面。按下即触发的话，
    // 一滑动就会误点赞/误投币 —— 而这两个动作在 B 站上是有代价的。
    //
    // 所以：按下只记坐标，抬起时位移 < 15du 才算"点了一下"。
    // 同时保持默认的 overlapTouchPolicy（Deny），触摸不会漏给外层 ScrollView，
    // 滚动就不会在按钮上"半途接管"。
    property double pillDownX: 0
    property double pillDownY: 0

    function pillTap(event, what) {
        if (!event) return;
        if (event.touchType === TouchType.Down) {
            videoPage.pillDownX = event.localX;
            videoPage.pillDownY = event.localY;
        } else if (event.touchType === TouchType.Up) {
            var dx = event.localX - videoPage.pillDownX;
            var dy = event.localY - videoPage.pillDownY;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx >= 15 || dy >= 15) return;
            // 用字符串派发而不是把函数当值传：Qt 4.8 的 V4 引擎里
            // 把 QML 方法取出来再调用，this 绑定不可靠，容易静默失败。
            if (what === "like") {
                videoStore.toggleLike();
            } else if (what === "coin") {
                videoStore.addCoin();
            } else if (what === "fav") {
                videoStore.toggleFav();
            } else if (what === "share") {
                videoStore.shareVideo();
            } else if (what === "follow") {
                videoStore.toggleFollow();
            } else if (what === "download") {
                videoStore.downloadCurrent();
            }
        }
    }

    // 时间格式化："1:05"，超过一小时 "1:02:09"（照 BBTube 的 clock()）
    function clock(ms) {
        var t = Math.floor(Math.max(0, ms) / 1000);
        var h = Math.floor(t / 3600);
        var m = Math.floor(t / 60) % 60;
        var s = t % 60;
        var ss = (s < 10 ? "0" : "") + s;
        if (h > 0 || videoPlayer.durationMs >= 3600000) {
            return h + ":" + (m < 10 ? "0" : "") + m + ":" + ss;
        }
        return m + ":" + ss;
    }

    function safeAspect() {
        // 1) 首选：view 接口给的 dimension。竖屏 1080x1920 → 0.5625，正是我们要的。
        var dw = videoStore.detail.dimW;
        var dh = videoStore.detail.dimH;
        if (dw > 0 && dh > 0) {
            var a = dw / dh;
            // 0.3 ~ 3.0 之外的几乎一定是脏数据（超宽贴片/长截图），退兜底
            if (a >= 0.3 && a <= 3.0) return a;
        }
        // 2) 次选：播放器自报的宽高比（目前恒为 0，留给以后从解码器拿真实值）
        var b = videoPlayer.videoAspect;
        if (b >= 0.3 && b <= 3.0) return b;
        // 3) 兜底：16:9。尺寸算不出来时宁可退化成默认比例，也不能让播放区塌成一条缝。
        return 1.7777778;
    }

    signal back()

    // 【v70】清理播放的钩子已经移到 C++（ApplicationUI::onPagePopped）。
    //
    // 这里连续踩了两个坑，都记一下：
    //   1) Page 上没有 popTransitionEnded —— 那是 NavigationPane 的信号
    //      （navigationpane.h:103）。写了 → Cannot assign to non-existent property。
    //   2) Page 上也没有 visible 的 QML 暴露 —— 虽然 VisualNode 里确实有
    //      Q_PROPERTY(bool visible ... NOTIFY visibleChanged FINAL)，但 Page 的
    //      可见性完全由 NavigationPane 管理，QML 层拿不到这个属性，
    //      写 onVisibleChanged 同样报 Cannot assign to non-existent property。
    //      （教训：C++ 头文件里有这个属性，不等于 QML 层能用。）
    //
    // 现在由 applicationui.cpp 里 connect(nav, popTransitionEnded, this, onPagePopped)
    // 处理 —— 编译期连接，可靠。下面这个 objectName 就是给它认页面用的。
    objectName: "VideoPage"

    content: ScrollView {
        id: pageScroll
        horizontalAlignment: HorizontalAlignment.Fill
        verticalAlignment: VerticalAlignment.Fill
        // 不写 scrollRole：那是给 NavigationPane 的 action bar 做"滚动自动隐藏"用的，
        // 本页没有 action bar。而 ScrollRole 枚举在 QML 1.1 是否注册拿不准，
        // 少一个拿不准的符号就少一次"整页打不开"。

        // 照 BBTube：整页是一个纵向 StackLayout，视频在最上面、跟着一起滚。
        // 内容容器只设 horizontalAlignment: Fill，**不设 verticalAlignment: Fill** ——
        // BBTube 的注释里写明了：垂直 ScrollView 的内容必须按自身内容定高，
        // 钉成 Fill 会被夹到视口高度，结果整个页面滚不动。
        Container {
            horizontalAlignment: HorizontalAlignment.Fill
            layout: StackLayout {}


            // ---- 播放区（顶部，全宽，高度 = 画面比例） ----
            //
            // 【v67】结构照搬 BBTube 的 bb-shared/media/VideoView：
            //   playerBox（黑框）
            //     ├── stage      ← 播放面由 main.qml 动态 add 到这里
            //     ├── 状态层      ← 提示 / spinner / 错误（声明式，在 stage 之上）
            //     └── ctrlLayer  ← 控件（最后声明，最上层）
            //
            // 为什么必须有 stage 这一层：动态 add 进来的子节点会排在声明式子节点的
            // 【后面】（更上层）。如果直接 playerBox.add(player)，播放面就会盖住控件层，
            // 又回到"看不到按钮"。BBTube 的 VideoView 也是把画面和 overlay 分成两个
            // 兄弟容器，控件放 overlay 里。
            //
            // 顺带纠正 v65 的一个错误判断：当时认为"原生屏幕窗口在合成器层面绘制在
            // Cascades UI 之上，任何重叠控件都会被遮住"。BBTube 的 VideoControls 就
            // 明明白白叠在 ForeignWindowControl 上，所以那个结论是错的 —— 真正的原因
            // 只是布局（DockLayout 覆盖 + 按钮总宽溢出屏幕）。
            Container {
                id: playerBox
                horizontalAlignment: HorizontalAlignment.Fill
                preferredHeight: videoPage.videoH
                background: Color.Black
                layout: DockLayout {}

                // ---- 播放面 ----
                //
                // 【v76】ForeignWindowControl 直接内联在这里，不再走 main.qml 的
                // ComponentDefinition + `page.player` 注入那一套。
                //
                // 为什么改掉：原来 pushVideo 里是 `page.player = tabs.newPlayer()`，
                // 而 newPlayer() 一旦返回 null（ComponentDefinition 加载失败），播放页
                // 就只剩一行"播放面加载失败 + (未捕获到 QML 错误)"——错误信息还拿不到，
                // 完全无法定位。而那个 ComponentDefinition 存在的唯一理由，是注释里写的
                // "PlayerSurface.qml 需要 import bb.multimedia，单独成文件可以让模块缺失时
                // 只失败它一个"——这个理由**根本不成立**：那个文件里只有
                // ForeignWindowControl，它属于 bb.cascades，整个文件没有一行
                // bb.multimedia。也就是说这层间接从没带来任何收益，只多了一个失败点。
                //
                // 内联之后：少一个文件、少一次 createObject、少一个 null 分支，
                // 而 FWC 本来就是事件驱动的（窗口出现后自己会重新匹配），
                // 放在哪个文档里创建都一样。
                Container {
                    id: stage
                    horizontalAlignment: HorizontalAlignment.Fill
                    verticalAlignment: VerticalAlignment.Fill
                    layout: DockLayout {}

                    ForeignWindowControl {
                        id: surface
                        horizontalAlignment: HorizontalAlignment.Center
                        verticalAlignment: VerticalAlignment.Fill
                        // 宽度按视频真实比例算：横屏铺满，竖屏/方形收窄居中留黑边。
                        preferredWidth: videoPage.videoW
                        // windowId 为空串时 FWC 不认领。videoPlayer.windowId 构造期就已算好，
                        // 所以这里一开始就是真实 ID；真正的屏幕窗口稍后由 openUrl 建出来。
                        windowId: videoPlayer.windowId.length > 0 ? videoPlayer.windowId : ""
                        // 必须与 libbbplayer join 的那个组是【同一个字符串】，否则认领不到。
                        windowGroup: videoPlayer.groupId.length > 0 ? videoPlayer.groupId : ""
                        // 三个属性必须写全，否则 Cascades 不会把位置尺寸同步给屏幕窗口，
                        // 表现为视频贴在左上角、大小不对。
                        updatedProperties: WindowProperty.Size | WindowProperty.Position | WindowProperty.Visible
                        // ⚠️ 必须保持可见才能去认领/绑定屏幕窗口。写成 visible: boundToWindow
                        // 会让控件在不可见期间不去认领，导致永远绑不上、永远黑屏。
                        visible: true
                    }
                }

                // ---- 状态层 ----
                Container {
                    horizontalAlignment: HorizontalAlignment.Fill
                    verticalAlignment: VerticalAlignment.Fill
                    layout: DockLayout {}

                    Label {
                        id: playHint
                        horizontalAlignment: HorizontalAlignment.Center
                        verticalAlignment: VerticalAlignment.Center
                        text: videoPage.uiState()
                        // 【v74】没有提示文字时必须整个隐藏，不能只留空字符串 ——
                        // 空 Label 仍然参与布局，会让人感觉"画面上糊着一层字"。
                        visible: text.length > 0
                        textStyle.color: Color.create("#6E7278")
                        textStyle.fontSize: FontSize.XSmall
                        multiline: true
                    }

                    ActivityIndicator {
                        id: playSpinner
                        horizontalAlignment: HorizontalAlignment.Center
                        verticalAlignment: VerticalAlignment.Center
                        visible: (videoStore.error.length === 0 && videoStore.loading)
                        running: (videoStore.error.length === 0 && videoStore.loading)
                    }

                    Label {
                        id: playError
                        horizontalAlignment: HorizontalAlignment.Center
                        verticalAlignment: VerticalAlignment.Center
                        visible: (videoStore.error.length > 0)
                        text: videoStore.error
                        textStyle.color: Color.create("#9CA0A6")
                        multiline: true
                    }
                }

                // ---- 控件层（复刻 BBTube VideoControls） ----
                //
                // 和 BBTube 的差异，都是刻意的：
                //   1) 不做"播放中 3 秒后自动隐藏"。用户上一轮的诉求就是"看不到暂停/播放
                //      按键"，自动隐藏会让它再次消失。宁可多占一点画面。
                //   2) 不做全屏。BB10 的横屏要靠 OrientationSupport，本工程的 bar-descriptor
                //      没有声明 landscape 方向，强行切会让整个页面重排。右下角改成倍速。
                //   3) 不用 TapHandler（那是 Cascades 1.2 才有的，本工程 import 1.1），
                //      一律用 Container 的 onTouch 判 TouchType.Down。
                Container {
                    id: ctrlLayer
                    horizontalAlignment: HorizontalAlignment.Fill
                    verticalAlignment: VerticalAlignment.Fill
                    layout: DockLayout {}

                    // 【v74】原来这里是 `durationMs > 0 || playing`，而流式播放时
                    // durationMs 恒为 0（libbbplayer 给不出容器总时长），playing 又因为
                    // 状态判定问题常常是 false —— 两个条件同时不成立，整个中央按钮
                    // 就永远不显示。用户反馈的"没有播放/暂停按钮"就是这个。
                    // 现在只要没报错就显示：加载中点一下无效（C++ 里 m_player 为空直接
                    // return，不会崩），但至少按钮"看得见、摸得到"。
                    property bool active: videoStore.error.length === 0

                    // ---------- 【v86】长按扫动的方向提示 ----------
                    //
                    // 只在扫动期间出现。作用是"确认手势被认出来了" ——
                    // 底层没有倍速接口，画面是 600ms 一段地跳，头 600ms 内
                    // 用户看不到任何反应，没有这个提示会以为手势没生效。
                    //
                    // ⚠️ 必须声明在全屏触摸层【之前】（= 下层）。放在后面它会盖在
                    // 触摸层上方，手指正好按在提示上时触摸就被它吃掉，长按松手的
                    // Up 事件收不到 → 扫动停不下来。它是纯展示节点，放在触摸层
                    // 下面既不影响观感（触摸层没有背景），也不会抢事件。
                    Container {
                        horizontalAlignment: HorizontalAlignment.Center
                        verticalAlignment: VerticalAlignment.Top
                        topMargin: ui.du(18)
                        visible: videoPlayer.scanning
                        background: Color.create("#B3101214")
                        leftPadding: ui.du(18)
                        rightPadding: ui.du(18)
                        topPadding: ui.du(8)
                        bottomPadding: ui.du(8)
                        Label {
                            text: videoPlayer.scanDir > 0 ? "3× 快进中" : "3× 后退中"
                            textStyle.color: Color.create("#FB7299")
                            textStyle.fontSize: FontSize.Small
                        }
                    }

                    // ---------- 全屏触摸层：点画面任意处播放/暂停 ----------
                    //
                    // 【v76】必须放在最前面（= 最底层）。因为中央圆盘按钮在播放时
                    // 会隐藏，隐藏之后画面上若没有别的接收者，用户就没有暂停入口了。
                    // 放在底层的好处：进度条、中央按钮都声明在它之后（更上层），
                    // 各自优先收到触摸，点空白处才落到这一层。
                    //
                    // ⚠️ 两个必须做的设置，缺一个都会出问题：
                    //
                    // 1) overlapTouchPolicy: OverlapTouchPolicy.Allow
                    //    默认是 Deny —— 上层节点会"独占"触摸，下层（这里是外层
                    //    ScrollView）收不到事件，页面就滚不动了。Allow 表示自己照常
                    //    处理，同时允许被覆盖的节点也收到（visualnode.h:228 的说明）。
                    //
                    // 2) 只在"按下和抬起几乎在同一个点"时才切换播放状态。
                    //    否则手指在视频上滑动翻页，按下瞬间就会误触发暂停。
                    Container {
                        horizontalAlignment: HorizontalAlignment.Fill
                        verticalAlignment: VerticalAlignment.Fill
                        visible: ctrlLayer.active
                        overlapTouchPolicy: OverlapTouchPolicy.Allow
                        // 按下点坐标，用来区分"点击"和"滑动"
                        property double downX: 0
                        property double downY: 0
                        onTouch: {
                            if (!event) return;
                            if (event.touchType === TouchType.Down) {
                                downX = event.localX;
                                downY = event.localY;
                                // 【v86】按下就把"长按判定"交给 C++。
                                // QML 里没有 Timer（Cascades 1.1 不带 Timer 类型），
                                // 判"按住 500ms 不放"只能靠 C++ 的 QTimer。
                                videoPlayer.pressAt(event.localX, width);
                            } else if (event.touchType === TouchType.Move) {
                                // 手指明显移动 = 用户在滑动页面，不是长按 → 取消长按判定。
                                // 已经在扫动时不做这个判断：扫动期间手指轻微漂移很正常，
                                // 一漂就停会让"按住不放"变得很难维持。
                                var mdx = event.localX - downX;
                                var mdy = event.localY - downY;
                                if (mdx < 0) mdx = -mdx;
                                if (mdy < 0) mdy = -mdy;
                                if (!videoPlayer.scanning) {
                                    if (mdx > 40 || mdy > 40) videoPlayer.cancelPress();
                                } else if (mdy > 80) {
                                    // 已经在扫动，但手指往下/上拉了很远 = 其实是想翻页。
                                    // 80du 远大于正常抖动，只拦真正的纵向滑动。
                                    videoPlayer.cancelPress();
                                }
                            } else if (event.touchType === TouchType.Up) {
                                // 【v86】releasePress() 返回"这次是不是长按"。
                                // 是长按就只结束扫动，不再当成点击去切控件 ——
                                // 否则每次长按松手都会把进度条收掉/唤出，很突兀。
                                var wasLong = videoPlayer.releasePress();
                                if (wasLong) return;
                                // 20du 以内算点击。手指滑动几十像素很正常，不会误判。
                                var dx = event.localX - downX;
                                var dy = event.localY - downY;
                                if (dx < 0) dx = -dx;
                                if (dy < 0) dy = -dy;
                                if (dx < 20 && dy < 20) {
                                    // 【v85】点画面 = 唤出/收起控件层（进度条 + 暂停按钮）。
                                    // 不再直接切换播放/暂停 —— 用户要求"播放过程中隐藏进度条，
                                    // 只有点击视频窗口时才出现"，所以点击的第一语义是"叫出控件"。
                                    // 播放/暂停交给中央那个按钮。
                                    videoPlayer.toggleControls();
                                }
                            } else if (event.touchType === TouchType.Cancel) {
                                videoPlayer.releasePress();
                            }
                        }
                    }

                    // ---------- 中央：播放 / 暂停 ----------
                    // disc.png 是 45% 黑的圆盘，ic_play / ic_pause 是纯白图形，叠出来就是
                    // BBTube 那个半透明圆形按钮（都不需要 filterColor，图标本身是白的）。
                    //
                    // 【v76】两处改动：
                    //   1) 尺寸 15du → 30du。15du 在 720 宽的屏上只有约 27px，
                    //      手指按上去很勉强，用户反馈"太小了"。
                    //   2) 播放时隐藏，只在暂停/未播时显示 —— 正在播的时候画面上
                    //      不要压着一个按钮挡画面。暂停入口交给上面那层全屏触摸区。
                    // 【v85】尺寸再放大 4 倍：30du → 120du（图标 22du → 88du）。
                    // 用户明确要求"在当前大小下等比例放大4倍"。720du 宽的屏上
                    // 120du ≈ 屏幕宽的 1/6，单手拇指能稳稳按到。
                    //
                    // 显隐规则（v85 调整）：
                    //   暂停 → 一直显示（否则用户找不到"继续播放"）；
                    //   播放 → 只在控件层亮起时显示（点画面唤出，4 秒后自动隐藏）。
                    Container {
                        horizontalAlignment: HorizontalAlignment.Center
                        verticalAlignment: VerticalAlignment.Center
                        preferredWidth: ui.du(120)
                        preferredHeight: ui.du(120)
                        layout: DockLayout {}
                        visible: ctrlLayer.active && (videoPlayer.ctrlShown || !videoPlayer.playing)

                        ImageView {
                            horizontalAlignment: HorizontalAlignment.Fill
                            verticalAlignment: VerticalAlignment.Fill
                            imageSource: "asset:///images/disc.png"
                        }

                        ImageView {
                            horizontalAlignment: HorizontalAlignment.Center
                            verticalAlignment: VerticalAlignment.Center
                            preferredWidth: ui.du(88)
                            preferredHeight: ui.du(88)
                            imageSource: videoPlayer.playing ? "asset:///images/ic_pause.png" : "asset:///images/ic_play.png"
                        }

                        // 触摸层必须最后声明（最上层）：ImageView 会把触摸事件吃掉，
                        // onTouch 挂在它下面的容器上就永远收不到。
                        Container {
                            horizontalAlignment: HorizontalAlignment.Fill
                            verticalAlignment: VerticalAlignment.Fill
                            onTouch: {
                                if (!event) return;
                                if (event.touchType === TouchType.Down) {
                                    videoPlayer.togglePlay();
                                }
                            }
                        }
                    }

                    // ---------- 底部：进度条 + 时间 + 倍速 ----------
                    Container {
                        horizontalAlignment: HorizontalAlignment.Fill
                        verticalAlignment: VerticalAlignment.Bottom
                        background: Color.create("#99000000")
                        leftPadding: ui.du(2)
                        rightPadding: ui.du(2)
                        // 底部条只认时长：没有时长就没有进度可画，0:00 / 0:00 只会让人困惑
                        // 【v85】显隐：播放中默认隐藏，点画面才出来（4 秒后自动收起）；
                        // 暂停时一直显示。ctrlShown 的定时器在 C++ 里（QML 1.1 没有 Timer）。
                        visible: videoPlayer.durationMs > 0 && (videoPlayer.ctrlShown || !videoPlayer.playing)
                        layout: StackLayout {}

                        // 进度条。
                        // 【v85】触摸区 7du → 30du。7du 在 720 宽的屏上只有 12 像素左右，
                        // 手指根本压不准 —— 压不中就落到下面的"点画面切控件"层，
                        // 表现就是"拖不动/时灵时不灵"。可见的线仍然只有 2du。
                        Container {
                            horizontalAlignment: HorizontalAlignment.Fill
                            preferredHeight: ui.du(30)
                            layout: DockLayout {}

                            // 槽（未播部分）
                            Container {
                                horizontalAlignment: HorizontalAlignment.Fill
                                verticalAlignment: VerticalAlignment.Center
                                preferredHeight: ui.du(2)
                                background: Color.create("#4CFFFFFF")
                            }

                            // 已播部分
                            Container {
                                horizontalAlignment: HorizontalAlignment.Left
                                verticalAlignment: VerticalAlignment.Center
                                preferredHeight: ui.du(2)
                                preferredWidth: Math.max(ui.du(2), videoPage.trackW * videoPage.progress)
                                background: Color.create("#FB7299")
                            }

                            // 手柄（knob.png 是纯白，直接当白色手柄用）
                            ImageView {
                                horizontalAlignment: HorizontalAlignment.Left
                                verticalAlignment: VerticalAlignment.Center
                                preferredWidth: videoPage.scrubbing ? ui.du(9) : ui.du(7)
                                preferredHeight: videoPage.scrubbing ? ui.du(9) : ui.du(7)
                                translationX: videoPage.knobX()
                                imageSource: "asset:///images/knob.png"
                            }

                            // 触摸层：最后声明，接管整条进度条的按下/移动/抬起
                            Container {
                                id: trackArea
                                horizontalAlignment: HorizontalAlignment.Fill
                                verticalAlignment: VerticalAlignment.Fill
                                onTouch: {
                                    videoPage.scrubTouch(event);
                                }
                            }
                        }

                        // 时间 / 倍速
                        Container {
                            horizontalAlignment: HorizontalAlignment.Fill
                            topPadding: ui.du(0.5)
                            bottomPadding: ui.du(0.5)
                            layout: StackLayout {}

                            Label {
                                id: timeLabel
                                verticalAlignment: VerticalAlignment.Center
                                text: videoPage.clock(videoPage.shownMs) + " / " + videoPage.clock(videoPlayer.durationMs)
                                textStyle.color: Color.White
                                textStyle.fontSize: FontSize.XSmall
                            }

                            // 撑开中间（BBTube 用 spaceQuota:1 的同款手法）。
                            // 【v75】原来右边缘还有一个倍速标签，已移除 ——
                            // libbbplayer 的导出符号里没有任何 rate/speed 接口
                            // （见 video_store.cpp setSpeed 的注释），点了也不会生效。
                            // 留一个"按下去没反应"的按钮比没有更糟，用户会以为是坏的。
                            // 想恢复真倍速只能换播放后端（MediaPlayer + 本地代理注入
                            // UA/Referer），那是另一个量级的改动，等确定要做再说。
                            Container {
                                layoutProperties: StackLayoutProperties {}
                            }
                        }
                    }
                }
            }

            Container {
                horizontalAlignment: HorizontalAlignment.Fill
                layout: StackLayout {}

                Container {
                    leftPadding: ui.du(18)
                    rightPadding: ui.du(18)
                    topPadding: ui.du(12)

                    // 【v69】照 BBTube：标题用 SystemDefaults.TextStyles.TitleText
                    // （大字号、多行），下面紧跟 SubtitleText 的 meta 行。
                    // 旧版是 FontSize.Medium，比 BBTube 小一大截，这是"UI 差距明显"
                    // 最直观的一处。
                    Label {
                        id: titleLabel
                        text: videoStore.detail.title || ""
                        textStyle.base: SystemDefaults.TextStyles.TitleText
                        textStyle.color: theme.textPrimary
                        multiline: true
                        bottomMargin: 0
                    }
                    Label {
                        id: statLabel
                        text: videoStore.detail.statText || ""
                        textStyle.base: SystemDefaults.TextStyles.SubtitleText
                        textStyle.color: theme.textSecondary
                        topMargin: 0
                        multiline: true
                    }

                    // ---- 药丸按钮行（照 BBTube 的 PillButton 行） ----
                    //
                    // 【v72】圆角背景用 assets/images/pill_dark.png.amd。
                    // BBTube 的 RoundedContainer 依赖它自己的 C++ RoundedImageProvider
                    // 动态生成圆角图，我们没有那个，改用 Cascades 自带的九宫格机制：
                    //   pill_dark.png（48x48、圆角 12）+ pill_dark.png.amd（sliceMargins: 12 12 12 12）
                    // 缺 .amd 的话圆角图会被当普通图整张拉伸，圆角就变成"被拉扁的圆角"。
                    //
                    // 结构必须是「ImageView 打底 + 内容层」—— ImageView 继承 Control，
                    // 没有 background 属性（这是之前踩过的坑），不能反过来用。
                    //
                    // 点赞/投币/收藏的数字来自 VideoStore::onDetailReply 里的
                    // likeText / coinText / favText（statText 是拼好的长串，按钮要单独取）。
                    Container {
                        horizontalAlignment: HorizontalAlignment.Fill
                        leftPadding: ui.du(16)
                        rightPadding: ui.du(16)
                        topMargin: ui.du(10)
                        layout: StackLayout {}

                        // 点赞
                        //
                        // 【v85】这四个药丸从"摆设"变成真按钮：每个都加了一层
                        // 透明触摸层（DockLayout 里最后声明 = 最上层），
                        // 按下就调 VideoStore 的对应方法。状态（已赞/已投币/已收藏）
                        // 走 Q_PROPERTY 绑定，颜色和文案跟着变。
                        Container {
                            preferredHeight: ui.du(28)
                            rightMargin: ui.du(8)
                            layout: DockLayout {}
                            ImageView {
                                horizontalAlignment: HorizontalAlignment.Fill
                                verticalAlignment: VerticalAlignment.Fill
                                imageSource: "asset:///images/pill_dark.png.amd"
                            }
                            Container {
                                horizontalAlignment: HorizontalAlignment.Fill
                                verticalAlignment: VerticalAlignment.Fill
                                leftPadding: ui.du(12)
                                rightPadding: ui.du(12)
                                Label {
                                    verticalAlignment: VerticalAlignment.Center
                                    text: (videoStore.liked ? "已赞 " : "点赞 ") + (videoStore.detail.likeText || "0")
                                    textStyle.base: SystemDefaults.TextStyles.SmallText
                                    textStyle.color: videoStore.liked ? Color.create("#FB7299") : Color.create("#F2F3F5")
                                }
                            }
                            Container {
                                horizontalAlignment: HorizontalAlignment.Fill
                                verticalAlignment: VerticalAlignment.Fill
                                onTouch: {
                                    videoPage.pillTap(event, "like");
                                }
                            }
                        }

                        // 投币
                        Container {
                            preferredHeight: ui.du(28)
                            rightMargin: ui.du(8)
                            layout: DockLayout {}
                            ImageView {
                                horizontalAlignment: HorizontalAlignment.Fill
                                verticalAlignment: VerticalAlignment.Fill
                                imageSource: "asset:///images/pill_dark.png.amd"
                            }
                            Container {
                                horizontalAlignment: HorizontalAlignment.Fill
                                verticalAlignment: VerticalAlignment.Fill
                                leftPadding: ui.du(12)
                                rightPadding: ui.du(12)
                                Label {
                                    verticalAlignment: VerticalAlignment.Center
                                    text: (videoStore.coined ? "已投 " : "投币 ") + (videoStore.detail.coinText || "0")
                                    textStyle.base: SystemDefaults.TextStyles.SmallText
                                    textStyle.color: videoStore.coined ? Color.create("#FB7299") : Color.create("#F2F3F5")
                                }
                            }
                            Container {
                                horizontalAlignment: HorizontalAlignment.Fill
                                verticalAlignment: VerticalAlignment.Fill
                                onTouch: {
                                    videoPage.pillTap(event, "coin");
                                }
                            }
                        }

                        // 收藏
                        Container {
                            preferredHeight: ui.du(28)
                            rightMargin: ui.du(8)
                            layout: DockLayout {}
                            ImageView {
                                horizontalAlignment: HorizontalAlignment.Fill
                                verticalAlignment: VerticalAlignment.Fill
                                imageSource: "asset:///images/pill_dark.png.amd"
                            }
                            Container {
                                horizontalAlignment: HorizontalAlignment.Fill
                                verticalAlignment: VerticalAlignment.Fill
                                leftPadding: ui.du(12)
                                rightPadding: ui.du(12)
                                Label {
                                    verticalAlignment: VerticalAlignment.Center
                                    text: (videoStore.favored ? "已收藏 " : "收藏 ") + (videoStore.detail.favText || "0")
                                    textStyle.base: SystemDefaults.TextStyles.SmallText
                                    textStyle.color: videoStore.favored ? Color.create("#FB7299") : Color.create("#F2F3F5")
                                }
                            }
                            Container {
                                horizontalAlignment: HorizontalAlignment.Fill
                                verticalAlignment: VerticalAlignment.Fill
                                onTouch: {
                                    videoPage.pillTap(event, "fav");
                                }
                            }
                        }

                        // 分享
                        Container {
                            preferredHeight: ui.du(28)
                            rightMargin: ui.du(8)
                            layout: DockLayout {}
                            ImageView {
                                horizontalAlignment: HorizontalAlignment.Fill
                                verticalAlignment: VerticalAlignment.Fill
                                imageSource: "asset:///images/pill_dark.png.amd"
                            }
                            Container {
                                horizontalAlignment: HorizontalAlignment.Fill
                                verticalAlignment: VerticalAlignment.Fill
                                leftPadding: ui.du(12)
                                rightPadding: ui.du(12)
                                Label {
                                    verticalAlignment: VerticalAlignment.Center
                                    text: "分享"
                                    textStyle.base: SystemDefaults.TextStyles.SmallText
                                    textStyle.color: Color.create("#F2F3F5")
                                }
                            }
                            Container {
                                horizontalAlignment: HorizontalAlignment.Fill
                                verticalAlignment: VerticalAlignment.Fill
                                onTouch: {
                                    videoPage.pillTap(event, "share");
                                }
                            }
                        }

                        // 下载（【v89】）
                        //
                        // 三态文案直接绑 videoStore.dlState / dlPercent：
                        //   0 = 下载  1 = 下载中 42%  2 = 已下载（白底深字）
                        // 状态、进度、落盘全在 C++（下载线程 + 定时器都在那边），
                        // QML 只做属性绑定 —— 铁律 4。
                        Container {
                            preferredHeight: ui.du(28)
                            layout: DockLayout {}
                            ImageView {
                                horizontalAlignment: HorizontalAlignment.Fill
                                verticalAlignment: VerticalAlignment.Fill
                                imageSource: videoStore.dlState === 2
                                             ? "asset:///images/pill_white.png.amd"
                                             : "asset:///images/pill_dark.png.amd"
                            }
                            Container {
                                horizontalAlignment: HorizontalAlignment.Fill
                                verticalAlignment: VerticalAlignment.Fill
                                leftPadding: ui.du(12)
                                rightPadding: ui.du(12)
                                Label {
                                    verticalAlignment: VerticalAlignment.Center
                                    // dlPercent 为 -1 表示还没拿到总大小（不知道百分比），
                                    // 这时只显示"下载中"，别一直挂着"下载中 0%"。
                                    text: videoStore.dlState === 2
                                          ? "已下载"
                                          : (videoStore.dlState === 1
                                             ? (videoStore.dlPercent >= 0
                                                ? ("下载中 " + videoStore.dlPercent + "%")
                                                : "下载中")
                                             : "下载")
                                    textStyle.base: SystemDefaults.TextStyles.SmallText
                                    // 已下载 = 白底深字，和"关注"按钮同样的处理：
                                    // 完成态退成浅色胶囊，不再抢注意力。
                                    textStyle.color: videoStore.dlState === 2
                                                      ? Color.create("#101214")
                                                      : Color.create("#F2F3F5")
                                }
                            }
                            Container {
                                horizontalAlignment: HorizontalAlignment.Fill
                                verticalAlignment: VerticalAlignment.Fill
                                onTouch: {
                                    videoPage.pillTap(event, "download");
                                }
                            }
                        }
                    }

                    // 【v85】操作反馈（"已点赞" / "投币失败（-111）"）。
                    // 2.5 秒后由 C++ 的 QTimer 清空 —— QML 1.1 没有 Timer 可用。
                    Label {
                        text: videoStore.interactTip || ""
                        visible: text.length > 0
                        leftPadding: ui.du(16)
                        rightPadding: ui.du(16)
                        topMargin: ui.du(6)
                        textStyle.color: Color.create("#FB7299")
                        textStyle.fontSize: FontSize.XSmall
                    }

                    // ---- 频道信息卡（照 BBTube 的 VideoInfo） ----
                    // 头像 + UP主名 + 关注按钮。BBTube 的关注按钮是"白底黑字"，
                    // 和周围深色胶囊区分开，作为唯一的 call-to-action。
                    // 【v85】关注按钮已接上 /x/relation/modify（act=1 关注 / 2 取关），
                    // 未登录时点它会提示"请先到设置页登录"。
                    Container {
                        horizontalAlignment: HorizontalAlignment.Fill
                        leftPadding: ui.du(16)
                        rightPadding: ui.du(16)
                        topMargin: ui.du(14)
                        bottomMargin: ui.du(14)
                        layout: StackLayout {}

                        // 头像。ImageView 没有 background，所以外面套一层 Container 垫底色。
                        Container {
                            preferredWidth: ui.du(40)
                            preferredHeight: ui.du(40)
                            verticalAlignment: VerticalAlignment.Center
                            background: Color.create("#26272B")

                            ImageView {
                                id: ownerFace
                                imageSource: videoStore.detail.ownerFace || ""
                                preferredWidth: ui.du(40)
                                preferredHeight: ui.du(40)
                                scalingMethod: ScalingMethod.AspectFill
                            }
                        }

                        Container {
                            verticalAlignment: VerticalAlignment.Center
                            leftPadding: ui.du(10)
                            layoutProperties: StackLayoutProperties {}
                            Label {
                                id: ownerName
                                text: videoStore.detail.ownerName || ""
                                textStyle.base: SystemDefaults.TextStyles.BodyText
                                textStyle.color: theme.textPrimary
                            }
                        }

                        // 关注（【v85】可点击，已关注时变成深色胶囊 + 灰字）
                        Container {
                            verticalAlignment: VerticalAlignment.Center
                            preferredHeight: ui.du(28)
                            layout: DockLayout {}
                            ImageView {
                                horizontalAlignment: HorizontalAlignment.Fill
                                verticalAlignment: VerticalAlignment.Fill
                                // 未关注 = 白底黑字（唯一 call-to-action）；
                                // 已关注 = 深色胶囊，视觉上退到背景里，符合 BB10 习惯
                                imageSource: videoStore.followed ? "asset:///images/pill_dark.png.amd" : "asset:///images/pill_white.png.amd"
                            }
                            Label {
                                horizontalAlignment: HorizontalAlignment.Center
                                verticalAlignment: VerticalAlignment.Center
                                text: videoStore.followed ? "已关注" : "关注"
                                leftPadding: ui.du(14)
                                rightPadding: ui.du(14)
                                textStyle.base: SystemDefaults.TextStyles.SmallText
                                textStyle.color: videoStore.followed ? Color.create("#9CA0A6") : Color.create("#101214")
                            }
                            Container {
                                horizontalAlignment: HorizontalAlignment.Fill
                                verticalAlignment: VerticalAlignment.Fill
                                onTouch: {
                                    videoPage.pillTap(event, "follow");
                                }
                            }
                        }
                    }

                    Label {
                        id: descLabel
                        text: videoStore.detail.desc || ""
                        textStyle.color: theme.textSecondary
                        textStyle.fontSize: FontSize.XSmall
                        multiline: true
                        topMargin: ui.du(12)
                    }

                    Divider {
                    }

                    Label {
                        id: commentHeader
                        text: "评论 (" + videoStore.commentCount + ")"
                        textStyle.color: theme.textPrimary
                        textStyle.fontSize: FontSize.Small
                        textStyle.fontWeight: FontWeight.Normal
                    }

                    // ---- 评论区 ----
                    ListView {
                        id: commentList
                        dataModel: videoStore.commentModel
                        // du 基准：屏幕宽 = 720du。原来写 ui.du(24) 只有 24px 高，
                        // 一条评论都放不下，详情页会看起来"没有评论"。
                        // 【v80】180 → 260：新的评论行是"头像 + 三行内容"（作者行/正文/点赞），
                        // 比原来高，180 只能露出两条半。
                        preferredHeight: ui.du(260)

                        listItemComponents: [
                            ListItemComponent {
                                // 同 FeedPage：ArrayDataModel 的 itemType 默认为空字符串，
                                // 这里若写 type: "item" 会匹配失败而退回默认 delegate，
                                // 只显示 QVariantMap 里字母序第一个键（这里是 face 头像 URL）。
                                //
                                // ⚠️ ListItemComponent 是【独立文档作用域】，访问不到外层注入的
                                // ui / theme / videoPage —— v47 日志里成片的
                                // "VideoPage.qml:189: ReferenceError: Can't find variable: ui"
                                // 就是这里用了 ui.du()。ui.du() 本身是恒等函数（直接返回入参），
                                // 所以这里一律写字面量，效果完全相同。
                                //
                                // 【v80】改成 BBTube CommentRow 的排法：
                                //   [圆头像] 作者 • 时间
                                //            正文
                                //            ♡ 点赞数
                                // 原来是"头像 + 名字/时间"单独一行、正文另起一行全宽，
                                // 正文和名字对不齐，读起来像两条互不相干的东西。
                                Container {
                                    layout: StackLayout {}
                                    // 必须给不透明底色：ListView 的条目在按下/选中时会用主题色
                                    // （我们这里是粉色 #FB7299）铺一层高亮，条目自己没底色时
                                    // 那层高亮就会整块透出来，看起来像"评论条目是红底的"。
                                    background: Color.create("#17181A")
                                    horizontalAlignment: HorizontalAlignment.Fill
                                    leftPadding: 16
                                    rightPadding: 16
                                    topPadding: 10
                                    bottomPadding: 10

                                    // ---- 圆形头像 ----
                                    // Cascades 没有 clip，ImageView 做不出圆形。做法是：
                                    // 方头像铺满，上面再盖一张"中心透明、四周填页面底色"的遮罩图
                                    // （images/avatar_mask.png，40x40 生成，四角被底色吃掉）。
                                    // BBTube 那边是 C++ 的 AdvancedImageView.circle，我们没有。
                                    Container {
                                        preferredWidth: 38
                                        preferredHeight: 38
                                        verticalAlignment: VerticalAlignment.Top
                                        layout: DockLayout {}
                                        // 【v87】垫一层深灰：头像没下完时是个灰色圆位，
                                        // 而不是和行背景融为一体的洞（用户看到的就是"不显示"）。
                                        background: Color.create("#26272B")

                                        ImageView {
                                            horizontalAlignment: HorizontalAlignment.Fill
                                            verticalAlignment: VerticalAlignment.Fill
                                            // 【v103】不能再写 ListItemData.face —— 模型里存的是
                                            // 远端 URL，要现场换成 file://。必须写成单表达式：
                                            // 写成 `{ ... }` 会被 Qt4.8 当对象字面量，绑定直接失效。
                                            // 第二参数把 coverVersion 拉进依赖图，每张头像下完就重算。
                                            imageSource: ListItemData.api
                                                         ? ListItemData.api.faceUrlFor(ListItemData.face,
                                                                                       ListItemData.api.coverVersion)
                                                         : ""
                                            scalingMethod: ScalingMethod.AspectFill
                                        }

                                        // 遮罩必须后声明（在上层）才能盖住四个角
                                        ImageView {
                                            horizontalAlignment: HorizontalAlignment.Fill
                                            verticalAlignment: VerticalAlignment.Fill
                                            imageSource: "asset:///images/avatar_mask.png"
                                        }
                                    }

                                    // ---- 右侧：作者行 / 正文 / 点赞 ----
                                    Container {
                                        leftMargin: 10
                                        verticalAlignment: VerticalAlignment.Top
                                        layoutProperties: StackLayoutProperties {}

                                        layout: StackLayout {}

                                        // 作者 • 时间（同一行，作者亮一点、时间更暗）
                                        Container {
                                            layout: StackLayout {}

                                            Label {
                                                text: ListItemData.name
                                                textStyle.color: Color.create("#9CA0A6")
                                                textStyle.fontSize: FontSize.XSmall
                                                textStyle.fontWeight: FontWeight.Normal
                                            }

                                            Label {
                                                text: "•  " + ListItemData.time
                                                leftMargin: 6
                                                textStyle.color: Color.create("#6E7278")
                                                textStyle.fontSize: FontSize.XSmall
                                            }
                                        }

                                        // 正文
                                        Label {
                                            text: ListItemData.message
                                            topMargin: 4
                                            textStyle.color: Color.create("#F2F3F5")
                                            textStyle.fontSize: FontSize.Small
                                            multiline: true
                                        }

                                        // 点赞：图标 + 数量
                                        Container {
                                            topMargin: 6
                                            layout: StackLayout {}

                                            ImageView {
                                                verticalAlignment: VerticalAlignment.Center
                                                // ⚠️ 不能用 filterColor —— 那是 bb.cascades 1.4
                                                // 才有的属性，10.1.0.1020 的头文件里全局搜不到，
                                                // 写了会直接 "Cannot assign to non-existent
                                                // property"，整页打不开。所以这里用一张预先染好
                                                // 灰色的图标（ic_like.png 原图是纯白 96x96，
                                                // 用 PIL 把 RGB 换成 #6E7278、alpha 原样保留）。
                                                imageSource: "asset:///images/ic_like_gray.png"
                                                preferredWidth: 14
                                                preferredHeight: 14
                                            }

                                            Label {
                                                verticalAlignment: VerticalAlignment.Center
                                                leftMargin: 5
                                                text: ListItemData.like
                                                textStyle.color: Color.create("#6E7278")
                                                textStyle.fontSize: FontSize.XSmall
                                            }
                                        }
                                    }
                                }
                            }
                        ]
                    }

                    // ---- 相关推荐（照 BBTube 的 RelatedRow 列表） ----
                    //
                    // 【v72】数据来自 VideoStore::relatedModel（FeedModel::Related），
                    // 详情加载完就并行请求 /x/web-interface/archive/related。
                    //
                    // ⚠️ ListView 嵌在 ScrollView 里有两个硬约束：
                    //   1) 必须给固定 preferredHeight，否则它会向上索取无限空间，
                    //      整个页面滚不动；
                    //   2) 必须禁用它自己的 flick 手势，否则手指在列表上滑只会滚
                    //      这个列表，外层页面不动。
                    // 高度只按前 5 条算，多出来的由 ListView 自己裁掉。
                    Label {
                        text: "相关推荐"
                        leftPadding: ui.du(16)
                        rightPadding: ui.du(16)
                        topMargin: ui.du(14)
                        textStyle.base: SystemDefaults.TextStyles.SubtitleText
                        textStyle.color: theme.textPrimary
                    }

                    ListView {
                        id: relatedList
                        dataModel: videoStore.relatedModel
                        // 【v80】大封面后每行高度 ≈ 封面 405 + 上边距 8 + 标题两行 56
                        // + 间距 4 + meta 24 + 下边距 18 = 515。标题只有一行时实际会矮
                        // 30du，多出来的空间留在列表底部（不影响滚动）。
                        // 只显示前 5 条：再多的内容往下翻也没人看，而每多一条就多一个
                        // 405 高的大图要解码，BB10 的内存经不起。
                        preferredHeight: Math.min(videoStore.relatedModel.count, 5) * ui.du(515)
                        // 【v73】禁掉 ListView 自己的滚动。
                        //   ❌ scrollViewProperties { scrollMode: ... } —— 那是 ScrollView
                        //      的属性（scrollview.h:155），ListView 上没有，写了直接
                        //      Cannot assign to non-existent property，整页打不开。
                        //   ✅ ListView 上真实存在的是 flickMode（listview.h:385，
                        //      @since 10.0.0），FlickMode.None 明确表示"禁用 flick 手势"
                        //      （flickmode.h:47）。
                        //   scrollIndicatorMode 顺手设 None，免得列表里露出一条内层滚动条。
                        flickMode: FlickMode.None
                        scrollIndicatorMode: ScrollIndicatorMode.None

                        // 【v74】点击相关视频 → 原地换片。
                        //
                        // 不新建页面、不 nav.push：从播放页再压一个播放页会让栈里
                        // 叠两层同一个页面，而播放器/VideoStore 都是单例 —— 新页一
                        // load 就把上一部的数据覆盖了，返回时旧页显示的是新视频，
                        // 还会重复占住一条 CDN 连接。直接改 bvid 最干净：
                        // onBvidChanged → videoStore.load(bvid) 本来就是这个页面的
                        // 正常加载路径，播放、详情、相关推荐会整套换掉。
                        onTriggered: {
                            videoPage.openRelated(indexPath);
                        }

                        listItemComponents: [
                            ListItemComponent {
                                // 【v80】直接复用首页那张大封面卡片。
                                // BBTube 的 RelatedRow 也只是包了一层 VideoRow（多加了点击），
                                // 所以两边的相关推荐和首页长得一模一样。
                                //
                                // ⚠️ 字段名是 metaText，不是 meta —— 原来这里写的
                                // ListItemData.meta 一直取不到值（FeedModel 里的键叫
                                // metaText），所以那行 meta 一直是空的。顺手修掉。
                                VideoRow {
                                    title: ListItemData.title
                                    meta: ListItemData.metaText
                                    // 【v102】picUrl 是远端 URL，本地路径由 VideoRow 内部解析
                                    picUrl: ListItemData.picUrl
                                    durationText: ListItemData.durationText
                                    api: ListItemData.api
                                }
                            }
                        ]
                    }

                    Label {
                        id: bottomPad
                        text: ""
                        preferredHeight: ui.du(20)
                    }
                }
            }
        }
    }


    // 【v76】onPlayerChanged / onVideoAspectChanged 已删除。
    // 播放面现在是内联在 stage 里的 ForeignWindowControl，宽度直接绑定
    // videoPage.videoW，不再需要"注入对象 + 手动写 aspectWidth"这套流程。
    // 原来那个分支最大的害处是：player 为 null 时会给 playError.text 赋值，
    // 而 playError 本来绑定着 videoStore.error —— 一旦手动赋值，绑定就永久断开，
    // 之后真正的错误反而显示不出来了。

    onBvidChanged: {
        if (bvid.length > 0) {
            videoStore.load(bvid);
        }
    }

    // 【v74】点击相关视频 → 原地换片。
    //
    // 为什么不用 nav.push 新建页面：VideoStore / VideoPlayer 都是单例，
    // 从播放页再压一个播放页，新页一 load 就把上一部的数据覆盖了；用户按返回时
    // 看到的旧页面里其实已经是新视频，同时还会多占一条到 CDN 的连接。
    // 直接改 bvid 走本页正常的加载路径最干净。
    //
    // 取 bvid 用 dataModel.data(indexPath)：ArrayDataModel::data 是 Q_INVOKABLE
    // （arraydatamodel.h:108），QML 里能直接调，返回的就是 QVariantMap。
    function openRelated(indexPath) {
        if (!indexPath || indexPath.length === 0) return;
        var it = videoStore.relatedModel.data(indexPath);
        if (!it) return;
        var b = it.bvid;
        if (!b || b.length === 0) return;
        if (b === videoPage.bvid) return;
        // 先滚回顶部：新视频如果比旧的短，停在原来的滚动位置会看到一片空白。
        // scrollToPoint 是 ScrollView 的 Q_INVOKABLE（scrollview.h:289），
        // 三个参数都显式传 —— QML 不会套用 C++ 的默认参数。
        pageScroll.scrollToPoint(0, 0, ScrollAnimation.None);
        videoPage.bvid = b;
    }

    // 【v75】fmtSpeed / cycleSpeed 已随倍速按钮一起删除。
    // 底层 libbbplayer 没有 rate/speed 接口，这两个函数留着也没有调用方。

    function fmtMs(ms) {
        var total = Math.floor(ms / 1000);
        var m = Math.floor(total / 60);
        var s = total % 60;
        return (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s;
    }

    // 中央提示行：根据 加载态/错误/播放态 计算。绑定里读取的 property 变化会自动重算。
    // 【v63】去掉了所有调试信息与认领重挂调用 —— 只保留用户真正需要看到的状态。
    // 【v64】去掉开头的 `videoPlayer.pollSeq;` 强制重算：它让这个绑定每秒重算 4 次，
    // 而下面的 stateText / playing / loading / error 本身都有 NOTIFY，状态一变就会重算。
    function uiState() {
        if (videoStore.error.length > 0) return "";
        var st = videoPlayer.stateText;
        // 把所有可能的失败态都显式翻译出来，避免笼统显示"正在连接视频"掩盖真相：
        //   player-unavailable —— libbbplayer.so 没加载成功（dlopen 失败）
        //   open-fail        —— bbplayer_open_url 返回非 0（CDN 403 / 网络不通）
        //   play-fail        —— bbplayer_play 返回非 0
        if (st === "player-unavailable") return "播放器不可用：libbbplayer 加载失败";
        if (st.indexOf("error") === 0) {
            // 错误原文来自事件回调（type=7），比 state 码有用得多
            var pe = videoPlayer.playerError;
            return "播放失败：" + (pe.length > 0 ? pe : st.substring(6));
        }
        if (st.indexOf("fail") >= 0) {
            return "播放失败：" + st;
        }
        if (videoStore.loading) return "正在加载视频信息…";
        // 播放中 / 已暂停 / 已结束 / 已停止：都不要在画面中央压文字。
        // 进度与状态在下方控制条的 timeLabel 里，那里一直在更新。
        // 【v74】加 st === "playing" 判定：playing 属性偶尔滞后于 stateText，
        // 只认 playing 会让"已经在播了但字还压在画面上"。
        if (st === "playing") return "";
        if (videoPlayer.playing) return "";
        if (st === "paused" || st === "ended" || st === "stopped") return "";
        // 流式播放没有本地下载阶段（dlPercent 恒为 0），这里保留分支仅作兼容。
        if (videoStore.dlPercent > 0 && videoStore.dlPercent < 100) {
            return "正在缓存视频 " + videoStore.dlPercent + "%";
        }
        return "正在连接视频…";
    }
}
