import bb.cascades 1.1

import "components"
// 抖音风格视频流主页 —— 上滑直接播放（1.0.0.17 起）。
//
// 【内嵌播放架构（单 FWC，勿改成每行一个）】
//   ForeignWindowControl 作为**固定层**放在 ListView 之后声明（z 序更高），
//   按当前行 videoAspect 做 letterbox（竖屏 405x720 居中、横屏 720x405）。
//   【1.0.0.19 上滑分页定论】上滑手势由**根容器 onTouch** 判定（一次上滑=一个视频）：
//   gestureUp() 算 dy → swipe(dir) → playInline(target) 换流 + app.scrollFeedTo(target)
//   平滑滚动封面。不再依赖 ListView 的 flick/snap：真机注入手势证实全屏透明
//   FWC 层默认 OverlapTouchPolicy.Deny 会独占触摸，ListView 根本收不到手势
//   （ROOT/VIDEOLAYER 都有 Down/Up，但 SCROLL/FIRSTVISIBLE/换流一次都没有）。
//   ListView 因此只作显示载体（flickMode: None），换流仍走已验证过的
//   stop() → load() 序列。
//
//   为什么不把 FWC 放进 ListItemComponent：每行一个原生窗口在 BB10 上没有
//   先例，且窗口销毁/认领时序不可控；单个 FWC + 换流是可验证的稳态。
//
// 【信息层为什么搬到页面固定层】
//   原生视频窗口只遮挡它自己的矩形；卡片行里底部左侧的文字区（x 20~460）
//   与竖屏视频区（x 157~562）重叠，会被视频盖掉。所以作者/标题/数据上移到
//   页面固定层（半透明条，声明在视频层之后 → z 序更高），右侧操作列本就
//   在视频区外（x > 562），也一并固定化并绑到 dyPlayerStore（当前行）。
//   好处：按钮点击可靠性同播放页（Button.clicked 是验证过的可靠信号）。
//
// 【点击行为】点视频区域 = 暂停/继续（videoPlayer.togglePlay），不再跳页 ——
//   避免首页 FWC 与播放页 FWC 争抢同一个原生窗口（双 FWC 未验证）。
//   DouyinPlayerPage.qml 保留在磁盘（不再被 push），GATE3 的导航自检用
//   C++ 自造标记页，不受影响。
//
// 【根节点必须是 Page】main.qml 把本组件直接放进 NavigationPane。
Page {
    id: feedPage
    property variant model: null
    property string pageTitle: "推荐"
    property variant nav: null
    property variant host: null

    // 本页是否要求登录态（关注流 = true）。
    property bool requireLogin: false

    // ---- 首屏自动播放（1.0.0.15 通道，Q_PROPERTY+NOTIFY 是唯一可靠 C++→QML 通道）----
    property bool autoPlayed: false
    property int feedCount: model ? model.count : 0
    property bool autoPlayGo: biliApi ? biliApi.autoPlayTrigger : false
    onFeedCountChanged: {
        feedPage.maybeAutoPlay();
    }
    onAutoPlayGoChanged: {
        feedPage.maybeAutoPlay();
    }

    // ---- 内嵌播放状态 ----
    /** 当前正在展示/播放的 feed 行（驱动 letterbox 尺寸与信息层）。 */
    property variant curRow: null
    /** 正在播放的 awemeId：换片去重，防止 firstVisibleItemChanged 抖动重复拉流。 */
    property string playingId: ""
    /**
     * 【1.0.0.37】换流窗口标志（修"上一个视频最后一帧残留"）。
     * 用户原话："划到下一个视频后，会残留上一个视频的最后一帧在这个视频的后面"。
     * 根因：VideoPlayer::stop() 调 m_player->close() 后屏幕窗口仍在合成器里
     * 留着旧片的最后一帧，而 FWC 硬编码 visible:true —— 新片首帧渲染前，
     * 旧帧一直在 letterbox 区可见（CDN 慢时能停好几秒）。
     * 方案：stop() 前一律置 switching=true，遮罩层（第 2 层内的 switchMask，
     * 黑底 + 新片封面 = 抖音式"封面→视频"）盖住整个视频矩形；新片首帧就绪
     * （vplaying 变真）才撤罩。不碰 FWC 的 visible：BB10 上 FWC 加入场景时
     * 只扫一次匹配窗口（v56 真机结论），动 visible 有窗口不再显示的回归风险，
     * 遮罩是零风险的等价方案。
     */
    property bool switching: false
    /** videoPlayer.playing 的影子属性：C++ 信号不能直连 QML 函数（铁律 4），
     *  NOTIFY 属性把它引进依赖图；playing 变真 = 新片首帧已出，撤换流遮罩。 */
    property bool vplaying: videoPlayer ? videoPlayer.playing : false
    onVplayingChanged: {
        if (feedPage.vplaying && feedPage.switching) {
            feedPage.switching = false;
            feedPage.logSwipe("FWC mask OFF first-frame id=" + feedPage.playingId);
        }
    }
    /** 当前页序号：上滑/下滑都基于它算目标行（不依赖 ListView 的滚动推断）。 */
    property int curIdx: 0
    /** 手势起点的窗口坐标（event.windowX/Y 是绝对屏幕坐标，跨层稳定）。 */
    property double downX: 0
    property double downY: 0
    /** 手势是否被接管：去重（冒泡会让多个容器都收到同一个 Down/Up）。 */
    property bool gestureActive: false
    /** 最近一次 Down 的时间戳（ms）：兜底复位用，防丢失结束事件导致永久失效。 */
    property double gestureDownAt: 0
    /** 最近一次 Move 的坐标：ListView 抢手势时只给 Cancel 不给 Up，靠它算位移。 */
    property double lastX: 0
    property double lastY: 0
    /** 判定为一次翻页的最小位移；低于它且位移很小 = 点按（暂停/继续）。 */
    property int minSwipe: 90

    // ---- 【1.0.0.24】左上角菜单 ----
    /** 抽屉菜单是否展开。 */
    property bool menuOpen: false
    /** 当前流来源：recommend / follow / city（菜单切换用）。 */
    property string feedKind: "recommend"

    // ---- 【1.0.0.24】循环播放 ----
    /** 播完自动重播（用户要求的行为，控制条 🔁 可关）。 */
    property bool loopOn: true
    /** videoPlayer.stateText 的影子属性：C++ 信号不能直接连 QML 函数（铁律 4），
     *  用 NOTIFY 属性把 "ended" 状态引进 QML 依赖图。
     *  【1.0.0.49】这里【只写日志，不再调 replay()】：1.0.0.24~48 由本处理器
     *  同步调用 videoPlayer.replay()，而 replay() 内部 stop()/play() 又同步
     *  setState → 在本绑定的求值链里重入自己 → Qt4 QML 每次循环重播刷 3 条
     *  "Binding loop detected for property pstate"（真机 bbplayer.log 实证）。
     *  循环重播决策已移入 VideoPlayer 状态机（onPoll ENDED 分支末尾），
     *  详见 video_store.hpp 的 loopOn Q_PROPERTY 注释。 */
    property string pstate: videoPlayer ? videoPlayer.stateText : ""
    onPstateChanged: {
        if (feedPage.pstate === "ended" && feedPage.loopOn && feedPage.playingId.length > 0) {
            feedPage.logSwipe("LOOP ended id=" + feedPage.playingId);
        }
    }

    // ---- 【1.0.0.24】进度条拖动 ----
    /** 手指是否正在拖进度条（拖的时候 value 跟随手指，不与播放器位置打架）。 */
    property bool seeking: false
    /** 拖动中的目标毫秒。 */
    property int seekUiMs: 0
    /** 上次拖动 seek 的时间戳（节流：拖动中每 400ms 才真 seek 一次）。 */
    property double lastDragSeekMs: 0
    // 与播放页同一套公式：Q20 方屏 720x720，竖屏视频 405x720 居中、横屏 720x405。
    // ⚠️ 单表达式绑定，不能写成块（铁律 5）。
    // 【1.0.0.22 改用真实比例】旧写法 portrait→固定 0.5625/1.7777，遇到非标比例
    // 视频（4:3 / 1:1 / 1080×1260 / 横屏各档）FWC letterbox 比例就错，封面比例又
    // 对不上 → 截图那种"视频叠在封面上、边缘露封面"的错位。curRow.videoAspect 由
    // douyin_store 用 video.width/height 算好，这里直接用。
    property double videoAspect: (feedPage.curRow && feedPage.curRow.videoAspect > 0) ? feedPage.curRow.videoAspect : 0.5625
    property double videoH: Math.min(720 / feedPage.videoAspect, 720)
    property double videoW: Math.min(feedPage.videoH * feedPage.videoAspect, 720)
    // 【1.0.0.27】视频矩形几何（Q20 方屏 720x720，视频 letterbox 居中）：
    // rectX/rectY = 视频左上角的屏幕坐标。控件层（videoUiLayer）用同一组值
    // 贴住视频矩形 —— 竖屏 405x720 / 横屏 720x405 都自适应
    // （用户："将进度条和播放/暂停放到视频中去跟随视频自动适配"）。
    property double rectX: (720 - feedPage.videoW) / 2
    property double rectY: (720 - feedPage.videoH) / 2
    // 视频矩形内底部"控件区"的上沿：起手点在矩形内且低于它 = 用户在够
    // 播放/暂停/进度条/循环钮，不翻页也不轻点暂停；从黑边区起手仍可翻页。
    // 104 = 控件条(50) + 作者名(40+6) + 底边距(6) + 余量。
    property double ctrlZoneTop: feedPage.rectY + feedPage.videoH - 104

    /**
     * 内嵌播放第 idx 行。上滑落定与首屏自动播放共用这一个入口。
     * 换片序列 = videoPlayer.stop() + dyPlayerStore.load(row)：
     *   stop() 拆旧片状态/窗口 → load() 起新代理 → videoReadyToPlay →
     *   play() 里 ensurePlayer 以同一 windowId 重建窗口、FWC 重新认领。
     * playingId 去重：同一行绝不重复拉流（含 firstVisibleItemChanged 抖动）。
     */
    function playInline(idx) {
        feedPage.logSwipe("playInline ENTER idx=" + idx + " playing=" + feedPage.playingId);
        if (idx < 0) return;
        if (!feedPage.model || !feedPage.model.rowAt) return;
        var row = feedPage.model.rowAt(idx);
        if (!row) return;
        var id = row.awemeId;
        if (!id || id.length === 0) return;
        feedPage.curRow = row;
        if (id === feedPage.playingId) return;
        feedPage.playingId = id;
        feedPage.logSwipe("playInline SWITCH idx=" + idx + " id=" + id);
        // 拖动终止：换片后进度条回到跟随播放位置（防旧片的 seeking 状态残留）。
        feedPage.seeking = false;
        // 【1.0.0.37】换流窗口开始：stop() 会 close 播放器但屏幕窗口留着旧片
        // 最后一帧（FWC 恒 visible），遮罩先盖上再拆流，用户看不到残留帧。
        feedPage.switching = true;
        feedPage.logSwipe("FWC mask ON switch id=" + id);
        // 【1.0.0.24】观看历史：每次换片记一行（同 id 去重置顶，cap 100）。
        // 只在 SWITCH 记 —— 循环重播不刷历史，否则循环一部视频会把列表撑爆。
        if (dyHistoryModel && dyHistoryModel.add) dyHistoryModel.add(row);
        if (videoPlayer && videoPlayer.stop) videoPlayer.stop();
        if (dyPlayerStore && dyPlayerStore.load) dyPlayerStore.load(row);
        // 【1.0.0.28】下一片预取（官方抖音 asyncPrepareVideo 同款）：
        // 当前片起播的同一时刻，后台整段下载 idx+1 的 play_addr；下次上滑
        // startStream 命中本地文件（PREFETCH hit）→ file:// 秒开，换片不等 CDN。
        // rowAt 越界返回空 map、playUrl 空串时 C++ 侧只记 PREFETCH skip，
        // 绝不为了预取再打一次网络。在飞上限 2 个，不和播放抢带宽。
        if (dyPlayerStore && dyPlayerStore.prefetchNext && feedPage.model && feedPage.model.rowAt) {
            var nxtRow = feedPage.model.rowAt(idx + 1);
            if (nxtRow && nxtRow.playUrl && nxtRow.playUrl.length > 0) dyPlayerStore.prefetchNext(nxtRow);
        }
    }

    /**
     * 【抖音式分页：一次上滑 = 一个视频】
     * 不依赖 ListView 的 flick/snap 猜测：顶层固定覆盖层会独占触摸
     * （OverlapTouchPolicy 默认 Deny，见 VideoPage.qml 的注释），ListView 根本
     * 收不到手势 —— 1.0.0.18 真机注入手势的日志已实证：ROOT/VIDEOLAYER 收到
     * Down/Up，但 SCROLL / FIRSTVISIBLE / playInline SWITCH 一次都没有。
     * 所以改为：根容器（所有触摸都会沿父链冒泡到这里）自己判定上下滑 →
     * 直接换流 + C++ 桥平滑滚动封面。ListView 只做「显示载体」。
     */
    function swipe(dir) {
        var total = (feedPage.model && feedPage.model.count) ? feedPage.model.count : 0;
        var target = feedPage.curIdx + dir;
        if (target < 0) target = 0;
        if (target >= total) {
            feedPage.logSwipe("SWIPE at end target=" + target + " total=" + total + " -> loadMore");
            if (feedPage.model && feedPage.model.loadMore) feedPage.model.loadMore();
            if (dyPlayerStore && dyPlayerStore.showTip) dyPlayerStore.showTip("正在加载更多…");
            return;
        }
        feedPage.logSwipe("SWIPE dir=" + dir + " from=" + feedPage.curIdx + " to=" + target);
        feedPage.curIdx = target;
        // 先换流（stop→load，playInline 内 playingId 去重），封面随后平滑跟上。
        feedPage.playInline(target);
        if (app && app.scrollFeedTo) app.scrollFeedTo(target);
        // 提前一屏续流，避免滑到底才加载。
        if (total - target <= 2 && feedPage.model && feedPage.model.loadMore) {
            feedPage.model.loadMore();
        }
    }

    /** 根容器触摸入口：Down 记起点，Up 判方向；轻点中央视频区 = 暂停/继续。 */
    function gestureDown(x, y) {
        feedPage.downX = x;
        feedPage.downY = y;
        feedPage.lastX = x;
        feedPage.lastY = y;
        feedPage.gestureDownAt = (new Date()).getTime();
        feedPage.logSwipe("ROOT DOWN x=" + Math.round(x) + " y=" + Math.round(y));
    }

    /**
     * 统一手势入口。三条真机定论都在这里兜住：
     *  1) 全屏覆盖层默认 Deny 会独占触摸 → 用最顶层手势层保证拿到 Down；
     *  2) 冒泡会让多个容器重复收到 Down/Up → gestureActive 去重；
     *  3) ListView 接管交互会发 TouchType.Cancel 而不是 Up（touchtype.h 注释），
     *     所以 Move 持续记末点，Cancel 也按末点结算，手势不会凭空丢失。
     */
    function gestureTouch(event) {
        if (!event) return;
        if (event.touchType === TouchType.Down) {
            // 【1.0.0.26】抽屉展开时，抽屉区域内（x<304 且 y>=74，即 menuDrawer
            // 的 preferredWidth 300 + topMargin 74）的触摸整段交给抽屉自己的
            // 按钮：这里只要一起手记了 gestureActive，系统随后发来的 Cancel
            // （按钮抢占手势）或 Up 就会走 gestureUp 的 menuOpen 分支把抽屉
            // 藏起来 —— 按钮的 onClicked 还没跑就没了。真机两代版本连击实证
            // （1.0.0.24 21:53 五次 / 1.0.0.25 22:42 五次）：日志只有 MENU
            // toggle open 与 MENU close by gesture，零 MENU switch / MENU
            // history；而菜单关闭时同层的 CTRL / CMT-BTN 按钮都正常可点。
            // 抽屉内按钮自己会关菜单（switchFeed / 设置 / 历史均带 menuOpen=false）。
            if (feedPage.menuOpen && event.windowX < 304 && event.windowY >= 74) return;
            var now = (new Date()).getTime();
            if (feedPage.gestureActive) {
                // 上一次 Down 的结束事件可能被系统手势/来电吞掉，gestureActive 卡在 true。
                // 新的 Down 应当总是启动新手势：刷新时间戳并继续，不再阻断后续滑动。
                // 冒泡重复 Down 的 downX/downY 相同，gestureDown/gestureUp 自幂，不会重复触发。
                feedPage.gestureDownAt = now;
                feedPage.logSwipe("ROOT RESET stale gestureActive downAt=" + Math.round(feedPage.gestureDownAt) + " now=" + Math.round(now));
            }
            feedPage.gestureActive = true;
            feedPage.gestureDown(event.windowX, event.windowY);
        } else if (event.touchType === TouchType.Move) {
            if (!feedPage.gestureActive) return;
            feedPage.lastX = event.windowX;
            feedPage.lastY = event.windowY;
        } else if (event.touchType === TouchType.Up || event.touchType === TouchType.Cancel) {
            if (!feedPage.gestureActive) return;
            feedPage.gestureActive = false;
            if (event.touchType === TouchType.Cancel) {
                feedPage.logSwipe("ROOT CANCEL lastY=" + Math.round(feedPage.lastY));
            }
            // Up 事件自身坐标最准；Cancel 没有有效坐标，用 Move 末点。
            var x = (event.touchType === TouchType.Up) ? event.windowX : feedPage.lastX;
            var y = (event.touchType === TouchType.Up) ? event.windowY : feedPage.lastY;
            feedPage.gestureUp(x, y);
        }
    }

    function gestureUp(x, y) {
        var dx = x - feedPage.downX;
        var dy = y - feedPage.downY;
        feedPage.logSwipe("ROOT UP dx=" + Math.round(dx) + " dy=" + Math.round(dy));
        // 菜单展开时：任何手势结束都先关菜单（标准抽屉语义，点/滑都不穿透）。
        if (feedPage.menuOpen) {
            feedPage.menuOpen = false;
            feedPage.logSwipe("MENU close by gesture");
            return;
        }
        // 【1.0.0.27】控件区跟随视频矩形（进度条/播放暂停已移入视频内）：
        // 起手点在**视频矩形内**且低于 ctrlZoneTop = 用户在够控件，不翻页也不
        // 轻点暂停；从黑边区（横屏上下 / 竖屏左右）起手仍可翻页，不被误伤。
        // 旧判据是写死的 downY >= 645（控件在屏幕底部那会儿），.27 起控件贴
        // 视频底边，横屏视频只有 405 高（rectY≈157）——写死值会把整个视频区
        // 都判成控件区，翻页全废。
        if (feedPage.downX >= feedPage.rectX && feedPage.downX <= feedPage.rectX + feedPage.videoW
                && feedPage.downY >= feedPage.ctrlZoneTop && feedPage.downY <= feedPage.rectY + feedPage.videoH) {
            feedPage.logSwipe("ROOT UP in controlzone, ignore");
            return;
        }
        if (Math.abs(dy) >= feedPage.minSwipe && Math.abs(dy) > Math.abs(dx)) {
            feedPage.swipe(dy < 0 ? 1 : -1);
            return;
        }
        // 轻点：只认视频矩形内的画面区。坐标从写死的 157~562 / <490 改为按
        // videoW/videoH 实算（竖屏 405x720、横屏 720x405 都贴视频）；顶栏区
        // （y<76）不触发 —— 修今早 09:41:33.242 实证：点顶栏标签把视频暂停了；
        // 右上循环钮区域同样不触发（按钮自己会响）。
        if (Math.abs(dx) < 24 && Math.abs(dy) < 24
                && x > feedPage.rectX + 2 && x < feedPage.rectX + feedPage.videoW - 2
                && y > Math.max(76, feedPage.rectY + 2) && y < feedPage.ctrlZoneTop
                && !(y < feedPage.rectY + 56 && x > feedPage.rectX + feedPage.videoW - 76)) {
            feedPage.logSwipe("TAP togglePlay");
            // 点画面先把控件层亮出来（暂停恢复时也要能看到按钮，v85 语义）。
            if (videoPlayer && videoPlayer.showControls) videoPlayer.showControls();
            if (videoPlayer && videoPlayer.togglePlay) videoPlayer.togglePlay();
        }
    }

    /**
     * 【1.0.0.24】菜单切换流来源。
     * kind = recommend / follow / city。
     * 序列：停当前播放 → 复位翻页状态 → 换模型 → 空则现拉。
     * 停播放必须在换模型之前：旧视频窗口盖在新（空）列表上很难看。
     */
    function switchFeed(kind) {
        feedPage.menuOpen = false;
        if (kind === feedPage.feedKind && feedPage.model && feedPage.model.count > 0) {
            // 【1.0.0.27 修「点了没反应」】同项不再静默早退：回顶部 + 重播第一部
            // + HUD 提示，每一次点击必有可见变化。真机实证（今早 08:18:33.630
            // "MENU same kind=recommend keep"）：用户点「推荐」时本就在推荐流，
            // 旧代码直接 return，画面零变化、抽屉随即关闭 → 体感"点击无反应"。
            feedPage.logSwipe("MENU same kind=" + kind + " refresh");
            feedPage.curIdx = 0;
            feedPage.playingId = "";
            feedPage.seeking = false;
            if (app && app.scrollFeedTo) app.scrollFeedTo(0);
            feedPage.playInline(0);
            if (dyPlayerStore && dyPlayerStore.showTip) {
                dyPlayerStore.showTip("已刷新「" + feedPage.pageTitle + "」，回到顶部重播");
            }
            return;
        }
        feedPage.logSwipe("MENU switch kind=" + kind + " from=" + feedPage.feedKind);
        // 【1.0.0.37】停流前先置换流窗口：旧片停播后屏幕窗口仍留着最后一帧，
        // 遮罩盖上直到新流首帧就绪（playInline 内 vplaying 复位）。
        feedPage.switching = true;
        if (videoPlayer && videoPlayer.stop) videoPlayer.stop();
        if (dyPlayerStore && dyPlayerStore.stopStream) dyPlayerStore.stopStream();
        feedPage.playingId = "";
        feedPage.curIdx = 0;
        feedPage.autoPlayed = false;   // 新流落定后允许自动播第一部
        feedPage.feedKind = kind;
        if (kind === "follow") {
            if (!(biliApi && biliApi.loggedIn)) {
                // 关注流必须登录（host 探针实证匿名恒空）：诚实提示，不假装有内容。
                dyPlayerStore.showTip("关注流需要登录：设置 > 粘贴 Cookie");
                feedPage.model = dyFollowModel;
                feedPage.pageTitle = "关注";
                return;
            }
            feedPage.model = dyFollowModel;
            feedPage.pageTitle = "关注";
        } else if (kind === "city") {
            feedPage.model = dyCityModel;
            feedPage.pageTitle = "同城";
        } else {
            feedPage.model = dyFeedModel;
            feedPage.pageTitle = "推荐";
            feedPage.feedKind = "recommend";
        }
        if (feedPage.model) {
            if (feedPage.model.count === 0) {
                if (feedPage.model.reloadIfStale) feedPage.model.reloadIfStale();
                else if (feedPage.model.load) feedPage.model.load();
            } else {
                feedPage.playInline(0);
            }
        }
        // 【1.0.0.27】切换落地必报一行 HUD：用户点完菜单立刻看到"切过去了"，
        // 不必等列表刷出来才知道有没有反应（设置/历史按钮同样带结果反馈）。
        if (dyPlayerStore && dyPlayerStore.showTip) {
            dyPlayerStore.showTip("已切换到「" + feedPage.pageTitle + "」");
        }
    }

    /**
     * 【1.0.0.29】顶条「去登录/设置 >」按钮的处理函数（refactor 抽出，
     * 点击与测试命令 login 走同一函数体）。双 try/catch：main.qml 侧已保证
     * 返回 EX: 串不再抛，这里再兜底一次 —— 日志必落、失败必 HUD，
     * 杜绝"点了没反应"。
     */
    function doTopbarLogin() {
        var r0 = "no-host";
        try {
            if (feedPage.host && feedPage.host.openLogin) {
                r0 = feedPage.host.openLogin();
            }
        } catch (e) {
            r0 = "EX:" + e;
        }
        feedPage.logSwipe("TOPBAR openLogin ret=" + r0);
        if (r0 !== "pushed" && dyPlayerStore && dyPlayerStore.showTip) {
            dyPlayerStore.showTip("设置页打开失败：" + r0);
        }
    }

    /**
     * 【1.0.0.29】抽屉「设置」按钮的处理函数（refactor 抽出，点击与测试命令
     * menu-settings 同一函数体）。先关抽屉再 push，失败同样落日志 + HUD。
     */
    function doMenuSettings() {
        feedPage.menuOpen = false;
        var r1 = "no-host";
        try {
            if (feedPage.host && feedPage.host.openLogin) {
                r1 = feedPage.host.openLogin();
            }
        } catch (e) {
            r1 = "EX:" + e;
        }
        feedPage.logSwipe("MENU settings ret=" + r1);
        if (r1 !== "pushed" && dyPlayerStore && dyPlayerStore.showTip) {
            dyPlayerStore.showTip("设置页打开失败：" + r1);
        }
    }

    /**
     * 【1.0.0.38】收藏当前行（右侧「收藏」按钮与测试命令 fav/unfav 同一函数体，
     * 注入=真功能）。toggle 后 C++ favVersion++ 驱动按钮文字绑定重算。
     */
    function favCurRow() {
        var fid = (feedPage.curRow && feedPage.curRow.awemeId) ? feedPage.curRow.awemeId : "";
        feedPage.logSwipe("FAV-BTN id=" + fid);
        if (fid.length === 0 || !dyFavModel || !dyFavModel.toggle) return "no-fav-model";
        var on = dyFavModel.toggle(feedPage.curRow);
        feedPage.logSwipe("FAV-BTN on=" + on + " total=" + dyFavModel.count);
        if (dyPlayerStore && dyPlayerStore.showTip) {
            dyPlayerStore.showTip(on ? "已收藏，菜单「收藏」里可找回" : "已取消收藏");
        }
        return on ? "faved" : "unfaved";
    }

    /**
     * 【1.0.0.38】下载当前行（右侧「下载」按钮与测试命令 dl/dlcancel 同一函数体）。
     * 三种态一个入口：下载→开始 / 取消 / 已下载→提示。
     */
    function dlCurRow() {
        var did = (feedPage.curRow && feedPage.curRow.awemeId) ? feedPage.curRow.awemeId : "";
        feedPage.logSwipe("DL-BTN id=" + did);
        if (did.length === 0 || !dyDownloadStore) return "no-dl-store";
        var st = dyDownloadStore.dlText(did, dyDownloadStore.dlVersion);
        if (st === "下载") {
            dyDownloadStore.start(feedPage.curRow);
            return "dl-start";
        } else if (st === "取消") {
            dyDownloadStore.cancel(did);
            return "dl-cancel";
        }
        if (dyPlayerStore && dyPlayerStore.showTip) {
            dyPlayerStore.showTip("已在「已下载」里，离线也能播");
        }
        return "dl-have";
    }

    /**
     * 【1.0.0.33】测试命令分发（main.qml 的 appTestCmd 绑定驱动，C++ 轮询
     * dy_test_cmd.txt 触发）。命令经 app.swipeLog 落 douyin_swipe.log：
     *   login          → doTopbarLogin()   （顶条「去登录」按钮同一函数）
     *   menu-settings  → doMenuSettings()  （抽屉「设置」按钮同一函数）
     *   refresh        → refreshFeed()     （右上角「刷新」按钮同一函数）
     *   comment        → nav.pushComments  （右侧「评论」按钮同一路径，awemeId 取首行）
     *   publish <文本> → dyCommentModel.publish（评论页「发表」按钮同一 C++ 路径）
     *   togglePlay     → videoPlayer.togglePlay（画面播放/暂停按钮同一 C++ 路径，
     *                   用于无触屏环境驱动图标态转换，onImageSourceChanged 留痕）
     *   next           → 程序化翻页（gestureUp 同款换流路径，验残留帧遮罩）
     *   fav / unfav    → favCurRow()（右侧「收藏」按钮同一函数体）
     *   dl / dlcancel  → dlCurRow()（右侧「下载」按钮同一函数体）
     *   favorites      → host.pushFavorites()（抽屉「收藏」菜单同一函数体）
     *   downloads      → host.pushDownloads()（抽屉「已下载」菜单同一函数体）
     *   pop            → nav.pop()         （推开页面后复位导航栈）
     */
    function runTestCmd(raw) {
        var idx = raw.indexOf("|");
        var cmd = idx >= 0 ? raw.substring(idx + 1) : raw;
        app.swipeLog("TESTCMD exec " + cmd);
        try {
            if (cmd === "login") {
                feedPage.doTopbarLogin();
            } else if (cmd === "menu-settings") {
                feedPage.doMenuSettings();
            } else if (cmd === "refresh") {
                feedPage.refreshFeed();
            } else if (cmd === "comment") {
                var row0 = (feedPage.model && feedPage.model.rowAt) ? feedPage.model.rowAt(0) : null;
                var aid = (row0 && row0.awemeId) ? row0.awemeId : "";
                if (aid.length === 0) {
                    app.swipeLog("TESTCMD comment skip no-feed-row");
                } else if (feedPage.nav && feedPage.nav.pushComments) {
                    app.swipeLog("TESTCMD comment push ret=" + feedPage.nav.pushComments(aid));
                } else {
                    app.swipeLog("TESTCMD comment no-nav");
                }
            } else if (cmd === "publish" || cmd.substring(0, 8) === "publish ") {
                var sp = cmd.indexOf(" ");
                var text = (sp >= 0) ? cmd.substring(sp + 1) : "";
                if (text.length === 0) {
                    app.swipeLog("TESTCMD publish skip empty-text");
                } else if (dyCommentModel && dyCommentModel.publish) {
                    dyCommentModel.publish(text);
                    app.swipeLog("TESTCMD publish sent text=" + text);
                } else {
                    app.swipeLog("TESTCMD publish no-model");
                }
            } else if (cmd === "togglePlay") {
                if (videoPlayer && videoPlayer.togglePlay) {
                    videoPlayer.togglePlay();
                    app.swipeLog("TESTCMD togglePlay ok");
                } else {
                    app.swipeLog("TESTCMD togglePlay no-player");
                }
            } else if (cmd === "pop") {
                if (feedPage.nav && feedPage.nav.pop) {
                    feedPage.nav.pop();
                    app.swipeLog("TESTCMD pop ok");
                } else {
                    app.swipeLog("TESTCMD pop no-nav");
                }
            } else if (cmd === "next") {
                // 1.0.0.38 无触屏环境驱动翻页（gestureUp 同款换流路径）；
                // 验证残留帧遮罩：switching 置位→FWC mask ON→load→首帧 OFF。
                if (feedPage.model && feedPage.model.count) {
                    var t = feedPage.curIdx + 1;
                    if (t >= feedPage.model.count) t = 0;
                    feedPage.logSwipe("TESTCMD next target=" + t);
                    feedPage.curIdx = t;
                    if (app && app.scrollFeedTo) app.scrollFeedTo(t);
                    feedPage.playInline(t);
                } else {
                    app.swipeLog("TESTCMD next no-model");
                }
            } else if (cmd === "fav" || cmd === "unfav") {
                var fr = feedPage.favCurRow();
                app.swipeLog("TESTCMD fav ret=" + fr);
            } else if (cmd === "dl" || cmd === "dlcancel") {
                var dr = feedPage.dlCurRow();
                app.swipeLog("TESTCMD dl ret=" + dr);
            } else if (cmd === "favorites") {
                if (feedPage.host && feedPage.host.pushFavorites) {
                    app.swipeLog("TESTCMD favorites ret=" + feedPage.host.pushFavorites());
                } else {
                    app.swipeLog("TESTCMD favorites no-host");
                }
            } else if (cmd === "downloads") {
                if (feedPage.host && feedPage.host.pushDownloads) {
                    app.swipeLog("TESTCMD downloads ret=" + feedPage.host.pushDownloads());
                } else {
                    app.swipeLog("TESTCMD downloads no-host");
                }
            } else if (cmd === "dlplay") {
                // 1.0.0.40 = 已下载页点第一行（onTriggered 同路径）：playUrl 换
                // file:// 本地路径 → playFeedRow → pop 回推荐页 → playRow 本地播。
                // 先注入 downloads 打开页面，pop 才有东西可退（真实流程同款）。
                var dlprobe = "store=" + (dyDownloadStore ? "y" : "n");
                try {
                    dlprobe += " count=" + dyDownloadStore.count
                             + " rowAt=" + (dyDownloadStore.rowAt ? "y" : "n");
                } catch (e2) {
                    dlprobe += " EX:" + e2;
                }
                app.swipeLog("TESTCMD dlplay probe " + dlprobe);
                if (dyDownloadStore && dyDownloadStore.count > 0) {
                    var drow = dyDownloadStore.rowAt(0);
                    if (drow && drow.awemeId && drow.state !== "downloading") {
                        drow.playUrl = "file://" + drow.localPath;
                        drow.videoSize = drow.fileBytes;
                        feedPage.logSwipe("TESTCMD dlplay id=" + drow.awemeId + " local=" + drow.localPath);
                        if (feedPage.host && feedPage.host.playFeedRow) {
                            app.swipeLog("TESTCMD dlplay ret=" + feedPage.host.playFeedRow(drow));
                        } else {
                            app.swipeLog("TESTCMD dlplay no-host");
                        }
                    } else {
                        app.swipeLog("TESTCMD dlplay skip state=" + (drow ? drow.state : "null"));
                    }
                } else {
                    app.swipeLog("TESTCMD dlplay empty");
                }
            } else if (cmd === "dlrm") {
                // 1.0.0.42 = 已下载页「删除」按钮同路径：remove(id) 删文件 + 索引。
                if (dyDownloadStore && dyDownloadStore.count > 0) {
                    var rrow = dyDownloadStore.rowAt(0);
                    if (rrow && rrow.awemeId) {
                        feedPage.logSwipe("TESTCMD dlrm id=" + rrow.awemeId);
                        dyDownloadStore.remove(rrow.awemeId);
                        app.swipeLog("TESTCMD dlrm ret=removed count=" + dyDownloadStore.count
                                     + " has=" + (dyDownloadStore.has(rrow.awemeId) ? "y" : "n"));
                    } else {
                        app.swipeLog("TESTCMD dlrm no-row");
                    }
                } else {
                    app.swipeLog("TESTCMD dlrm empty");
                }
            } else if (cmd === "favplay") {                // 1.0.0.40 = 收藏页点第一行（onTriggered 同路径）→ 复播。
                if (dyFavModel && dyFavModel.count > 0 && dyFavModel.rowAt) {
                    var frow = dyFavModel.rowAt(0);
                    if (frow && frow.awemeId) {
                        feedPage.logSwipe("TESTCMD favplay id=" + frow.awemeId);
                        if (feedPage.host && feedPage.host.playFeedRow) {
                            app.swipeLog("TESTCMD favplay ret=" + feedPage.host.playFeedRow(frow));
                        } else {
                            app.swipeLog("TESTCMD favplay no-host");
                        }
                    } else {
                        app.swipeLog("TESTCMD favplay skip");
                    }
                } else {
                    app.swipeLog("TESTCMD favplay empty");
                }
            } else {
                app.swipeLog("TESTCMD unknown cmd=" + cmd);
            }
        } catch (e) {
            app.swipeLog("TESTCMD EX " + e);
        }
    }

    /**
     * 【1.0.0.29】右上角「刷新」按钮：重新拉当前流 + 回顶部 + 自动播第一部。
     * 与 switchFeed 同 kind 分支的区别：那条只重播缓存第一片（菜单同项点击的
     * 兜底反馈），这里走真网络重拉（model.load() = reset + fetch），拉到新数据
     * 后 feedCount 变化 → maybeAutoPlay() 自动播第一部（冷启动同款链路）。
     */
    function refreshFeed() {
        feedPage.logSwipe("TOPBAR refresh kind=" + feedPage.feedKind);
        feedPage.switching = true;   // 1.0.0.37 换流窗口：盖住旧片残留帧
        if (videoPlayer && videoPlayer.stop) videoPlayer.stop();
        if (dyPlayerStore && dyPlayerStore.stopStream) dyPlayerStore.stopStream();
        feedPage.playingId = "";
        feedPage.curIdx = 0;
        feedPage.autoPlayed = false;   // 新流落定后允许自动播第一部
        if (app && app.scrollFeedTo) app.scrollFeedTo(0);
        if (feedPage.model && feedPage.model.load) {
            feedPage.model.load();
        }
        if (dyPlayerStore && dyPlayerStore.showTip) {
            dyPlayerStore.showTip("正在刷新「" + feedPage.pageTitle + "」…");
        }
    }

    /**
     * 【1.0.0.24】播放一整行数据（历史页点击项用）。
     * 行来自 dyHistoryModel.rowAt()：含 playUrl/videoSize/alts 由 inset 时存好；
     * CDN 直链过期时 HUD 会报错并可重试（与推荐流同一机制，不假装一定能播）。
     */
    function playRow(row) {
        if (!row) return;
        feedPage.logSwipe("HIST-PLAY id=" + row.awemeId);
        feedPage.curRow = row;
        // playingId 先落再 load：首帧就绪时 ON/OFF 埋点能对上同一个 id
        // （1.0.0.38 修复：原来先清空，mask OFF 日志 id= 为空，无法自证）。
        feedPage.playingId = row.awemeId ? row.awemeId : "";
        feedPage.switching = true;   // 1.0.0.37 换流窗口：盖住旧片残留帧
        feedPage.logSwipe("FWC mask ON switch id=" + feedPage.playingId);
        if (videoPlayer && videoPlayer.stop) videoPlayer.stop();
        if (dyHistoryModel && dyHistoryModel.add) dyHistoryModel.add(row);
        if (dyPlayerStore && dyPlayerStore.load) dyPlayerStore.load(row);
    }

    function maybeAutoPlay() {
        if (feedPage.autoPlayed) return;
        if (feedPage.feedCount <= 0) return;
        if (!feedPage.autoPlayGo) return;      // 等 C++ 确认首屏数据稳定
        feedPage.autoPlayed = true;
        feedPage.playInline(0);
    }

    /** 头像解析：md5+"_face.jpg" 缓存，必须走 faceUrlFor（1.0.0.15 修复）。 */
    function faceSourceOf(url) {
        if (!url || url.length === 0) return "";
        if (url.indexOf("file://") === 0) return url;
        if (biliApi) return biliApi.faceUrlFor(url, biliApi.coverVersion);
        return "";
    }

    /** 大数字缩写：12345 → 1.2万。 */
    function htmlcount(n) {
        // 【1.0.0.22 顺带修】NaN/undefined 计数会渲染成 "NaN万"（真机截图里的收藏计数）。
        if (isNaN(n) || n < 0) return "";
        if (n < 10000) return "" + n;
        var w = n / 10000.0;
        if (w < 10) return (Math.round(w * 10) / 10.0) + "万";
        return Math.round(w) + "万";
    }

    /** 毫秒 → "m:ss"（进度条时间标签用；>=1h 补 "h:"）。 */
    function fmtTime(ms) {
        if (isNaN(ms) || ms < 0) ms = 0;
        var t = Math.floor(ms / 1000);
        var h = Math.floor(t / 3600);
        var m = Math.floor((t % 3600) / 60);
        var s = t % 60;
        var ss = (s < 10) ? ("0" + s) : ("" + s);
        if (h > 0) return ("" + h) + ":" + ((m < 10) ? ("0" + m) : ("" + m)) + ":" + ss;
        return ("" + m) + ":" + ss;
    }

    /** 手势链路埋点：app 是 ApplicationUI（context property），落盘 douyin_swipe.log。 */
    function logSwipe(tag) {
        if (app && app.swipeLog) app.swipeLog(tag);
    }

    content: Container {
        id: feedRoot
        horizontalAlignment: HorizontalAlignment.Fill
        verticalAlignment: VerticalAlignment.Fill
        layout: DockLayout {}
        background: Color.Black

        // 根容器保留一个入口做兜底；真正保证全屏覆盖的手势层在最后（第 7 层）。
        onTouch: feedPage.gestureTouch(event)

        // ===== 第 1 层：滑动/对齐载体（封面卡片） =====
        ListView {
            id: list
            objectName: "dyFeedList"     // C++ scrollFeedTo() 靠它 findChild
            dataModel: model
            scrollRole: ScrollRole.Main
            // 手势分页由根容器负责（见 gestureUp/swipe），这里关掉原生 flick，
            // 避免覆盖层改为 Allow 后 ListView 又自己滚一次导致跳两行。
            flickMode: FlickMode.None
            snapMode: SnapMode.LeadingEdge

            listItemComponents: [
                // 【关键】不要写 type: "item"：ArrayDataModel 的 itemType 默认空字符串。
                ListItemComponent {
                    DouyinFeedRow {
                        picUrl: ListItemData.picUrl
                        api: ListItemData.api
                    }
                }
            ]

            // 点按暂停由根容器 gestureUp() 统一处理（带坐标守卫，避开按钮区）。
            // 这里不再接 onTriggered：覆盖层 Allow 后 ListView 也会收到触摸，
            // 两处都 togglePlay 会互相抵消。

            attachedObjects: [
                ListScrollStateHandler {
                    id: scrollState
                    onScrollingChanged: {
                        feedPage.logSwipe("SCROLL scrolling=" + scrolling);
                    }
                    onFirstVisibleItemChanged: {
                        // 程序化 scrollToItem 落定后同步页序号；playInline 有去重保护。
                        var idx = (firstVisibleItem && firstVisibleItem.length > 0) ? firstVisibleItem[0] : -1;
                        feedPage.logSwipe("FIRSTVISIBLE idx=" + idx + " len=" +
                                          (firstVisibleItem ? firstVisibleItem.length : -1));
                        if (idx >= 0) {
                            feedPage.curIdx = idx;
                            feedPage.playInline(idx);
                        }
                    }
                    onAtEndChanged: {
                        feedPage.logSwipe("ATEND=" + atEnd);
                        if (atEnd && model && model.loadMore) {
                            model.loadMore();
                        }
                    }
                }
            ]
        }

        // ===== 第 2 层：内嵌视频（固定层，盖住卡片中央） =====
        Container {
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Fill
            // 透明层只做 FWC 的定位容器，不遮住下面的封面海报。
            background: Color.Transparent
            layout: DockLayout {}

            // 【诊断】视频层触摸：1.0.0.18 真机注入实证，全屏透明层会收到手势，
            // 且它默认 Deny 会让下面的 ListView 完全收不到触摸。
            // 因此这层保持默认 Deny（不让 ListView 自己滚），分页交给根容器。
            onTouch: {
                if (event.touchType === TouchType.Down || event.touchType === TouchType.Up)
                    feedPage.logSwipe("VIDEOLAYER touch=" + event.touchType + " y=" + Math.round(event.windowY));
            }

            ForeignWindowControl {
                id: surface
                horizontalAlignment: HorizontalAlignment.Center
                verticalAlignment: VerticalAlignment.Center
                preferredWidth: feedPage.videoW
                preferredHeight: feedPage.videoH
                // windowId 为空串时 FWC 不认领。videoPlayer.windowId 构造期已算好。
                windowId: videoPlayer.windowId.length > 0 ? videoPlayer.windowId : ""
                // 必须与 libbbplayer join 的那个组是同一字符串，否则认领不到。
                windowGroup: videoPlayer.groupId.length > 0 ? videoPlayer.groupId : ""
                // 三个属性必须写全，否则位置尺寸不会同步给屏幕窗口。
                updatedProperties: WindowProperty.Size | WindowProperty.Position | WindowProperty.Visible
                visible: true
            }

            // 【1.0.0.37】换流遮罩：盖住上一个视频残留的最后一帧。
            // 与 FWC 共用同一个 letterbox 矩形（videoW/videoH）——封面矩形 ==
            // 视频窗口矩形（DouyinFeedRow 同款公式），边界严格对齐。
            // 内容 = 黑底 + 新片封面（抖音式"封面→视频"过渡）：封面没下下来
            // 也只是一片黑，绝不是旧片的残帧。switching 由 vplaying 首帧就绪
            // 复位（见 feedPage.switching 注释）。
            Container {
                id: switchMask
                objectName: "switchMask"
                horizontalAlignment: HorizontalAlignment.Center
                verticalAlignment: VerticalAlignment.Center
                preferredWidth: feedPage.videoW
                preferredHeight: feedPage.videoH
                visible: feedPage.switching
                background: Color.Black
                layout: DockLayout {}

                ImageView {
                    horizontalAlignment: HorizontalAlignment.Fill
                    verticalAlignment: VerticalAlignment.Fill
                    scalingMethod: ScalingMethod.AspectFill
                    imageSource: (feedPage.curRow && feedPage.curRow.picUrl && feedPage.curRow.picUrl.length > 0)
                                ? biliApi.coverUrlFor(feedPage.curRow.picUrl, biliApi.coverVersion)
                                : ""
                }
            }
        }

        // ===== 【1.0.0.27】控件入画层：作者名 + 播放控制条贴视频矩形自适应 =====
        // 用户原话："将进度条和'播放/暂停'放到视频中去跟随视频自动适配"。
        // 本层 preferredWidth/Height = videoW/videoH 且 Center/Center —— 与 FWC
        // 共用同一个 letterbox 后的矩形（竖屏 405x720 / 横屏 720x405），控件永远
        // 在视频里，不会漂到黑边上。声明在视频层之后 = z 序高于 FWC。
        // 手势层（第 7 层，Allow）保证按钮仍可点；gestureUp 的 controlzone 判据
        // （起手在矩形内且低于 ctrlZoneTop）让控件区手势不翻页/不轻点暂停。
        Container {
            id: videoUiLayer
            objectName: "videoUiLayer"
            horizontalAlignment: HorizontalAlignment.Center
            verticalAlignment: VerticalAlignment.Center
            preferredWidth: feedPage.videoW
            preferredHeight: feedPage.videoH
            visible: feedPage.playingId.length > 0
            layout: DockLayout {}

            // 中央暂停提示（抖音式：暂停时画面中央亮一个纯白播放键，无底无框，
            // 恢复即隐）。【1.0.0.49 用户原话"去掉播放按钮的背景"】旧版这里挂了
            // theme.overlayDim 的 96x96 半透明黑方块，官方抖音暂停态只有一个
            // 白色三角，没有任何底衬 —— 去掉 background，Container 仅作居中
            // 定位用（透明容器不影响点按穿透到视频层 togglePlay）。
            // 【1.0.0.29】中央符号换图标：旧 Label text "\u25B6" 在 BB10 上是
            // 文字字形（可能豆腐块），换成 PIL 生成的 play.png（白 + 透明底）。
            Container {
                horizontalAlignment: HorizontalAlignment.Center
                verticalAlignment: VerticalAlignment.Center
                preferredWidth: 96
                preferredHeight: 96
                // 【1.0.0.37】换流窗口不亮中央暂停符：那一刻 playing=false 是
                // stop() 的中间态，不是用户真的暂停了，画个大播放键在封面上会误导。
                visible: videoPlayer && !videoPlayer.playing && !feedPage.switching
                layout: DockLayout {}

                ImageView {
                    horizontalAlignment: HorizontalAlignment.Center
                    verticalAlignment: VerticalAlignment.Center
                    preferredWidth: 56
                    preferredHeight: 56
                    scalingMethod: ScalingMethod.AspectFit
                    imageSource: "asset:///images/play.png"
                }
            }

            // 循环开关：移到视频右上角（原在控制条行内，入画后行宽不够：
            // 竖屏视频只有 405 宽，⏯+时间+进度条+循环挤一行滑块只剩 70px）。
            // 【1.0.0.55 / UI-AUDIT P1-1】Button → ImageButton 44×44：旧控件
            // preferredHeight:40 < minTouch 44、带 Button 默认 chrome、且
            // 「循环 / 不循环」文字切换会改按钮宽度，三个契约同时破坏。现在状态
            // 只改图形颜色（白描边 → 品牌红），宽度恒 44；按下态按状态取 _sp。
            // onClicked 函数体一字未动（GATE7 的 videoPlayer.loopOn 同步锚点保留）。
            ImageButton {
                horizontalAlignment: HorizontalAlignment.Right
                verticalAlignment: VerticalAlignment.Top
                topMargin: 8
                rightMargin: 8
                preferredWidth: 44
                preferredHeight: 44
                defaultImageSource: feedPage.loopOn
                            ? "asset:///images/ic_loop_on_s.png"
                            : "asset:///images/ic_loop_s.png"
                pressedImageSource: feedPage.loopOn
                            ? "asset:///images/ic_loop_on_sp.png"
                            : "asset:///images/ic_loop_sp.png"
                onDefaultImageSourceChanged: feedPage.logSwipe("ICON loop=" + (feedPage.loopOn ? "on" : "off"))
                onClicked: {
                    feedPage.loopOn = !feedPage.loopOn;
                    feedPage.logSwipe("LOOP toggle on=" + feedPage.loopOn);
                    // 【1.0.0.49】循环重播决策已移入播放器状态机（见 pstate 注释），
                    // 开关必须同步给 C++，否则 QML 显示"已开启"而播放器不重播。
                    if (videoPlayer) videoPlayer.loopOn = feedPage.loopOn;
                    if (dyPlayerStore && dyPlayerStore.showTip) {
                        dyPlayerStore.showTip(feedPage.loopOn ? "已开启循环播放" : "已关闭循环播放");
                    }
                }
            }

            // 视频底部：作者名 + 控制条（⏯ + 时间 + 进度条）。
            // rightPadding 横屏(720 全宽)时让位右侧操作列（96 宽 + 12 边距），
            // 竖屏视频本身就在操作列左侧，不需要让位。
            Container {
                horizontalAlignment: HorizontalAlignment.Fill
                verticalAlignment: VerticalAlignment.Bottom
                bottomMargin: 6
                leftPadding: 8
                rightPadding: feedPage.videoW >= 720 ? 100 : 8
                layout: StackLayout {}

                // 作者 + 标题 + 计数（1.0.0.46 信息层回归，UI 评审 P0-1）。
                // 1.0.0.22 隐藏整条 230px 底部信息条换竖屏视频全高，但没做
                // overlay 替代 → 用户在刷什么视频说不清（只剩作者名）。现按
                // 抖音式把标题/计数并入作者名条同一半透明底衬（左侧信息块）：
                // 三行总高约 96px，整体仍在 ctrlZoneTop 的 104px 豁免区内，
                // gestureUp 判据一行不改；旧实体条（原第 4 层）随之删除。
                Container {
                    horizontalAlignment: HorizontalAlignment.Left
                    bottomMargin: 6
                    leftPadding: 12
                    rightPadding: 12
                    topPadding: 8
                    bottomPadding: 8
                    background: theme.overlayDim
                    visible: dyPlayerStore.authorName.length > 0 || dyPlayerStore.title.length > 0
                    layout: StackLayout {}

                    Label {
                        text: "@" + (dyPlayerStore.authorName.length > 0 ? dyPlayerStore.authorName : "抖音用户")
                        textStyle.color: Color.White
                        textStyle.fontSize: FontSize.Small
                    }

                    // 标题单行：竖屏视频宽 405，超出部分 Label 默认省略号截断
                    // （抖音同款行为；Label 无 maxLines，天然单行）。
                    Label {
                        text: dyPlayerStore.title
                        textStyle.color: Color.White
                        textStyle.fontSize: FontSize.Small
                        topMargin: 8
                    }

                    // 计数行：赞 / 评论 / 播放（htmlcount 已有万级缩写）。
                    Label {
                        text: (dyPlayerStore.likeCount > 0 ? feedPage.htmlcount(dyPlayerStore.likeCount) + "赞" : "")
                              + (dyPlayerStore.commentCount > 0 ? " · " + feedPage.htmlcount(dyPlayerStore.commentCount) + "评论" : "")
                              + (dyPlayerStore.playCount > 0 ? " · " + feedPage.htmlcount(dyPlayerStore.playCount) + "播放" : "")
                        textStyle.color: theme.textOnVideo
                        textStyle.fontSize: FontSize.XSmall
                        topMargin: 8
                        bottomMargin: 4
                    }
                }

                // 控制条：⏸/▶ + 时间 + 进度条（滑块宽度按视频宽实算，单行三元）。
                Container {
                    horizontalAlignment: HorizontalAlignment.Fill
                    preferredHeight: 50
                    background: theme.overlayStrong
                    layout: StackLayout {
                        orientation: LayoutOrientation.LeftToRight
                    }

                    // 播放/暂停。
                    // 【1.0.0.29 图标美化】旧代码 text: "\u23F8"/"\u25B6" 文字字形，
                    // BB10 上 Button.text 渲染 Unicode 媒体符号常出豆腐块/字形丑陋
                    // （用户原话"图标太丑了"），换成 PIL 生成的纯白透明底 PNG
                    // （assets/images/play.png / pause.png，96x96 4x 抗锯齿）。
                    // 【1.0.0.49 用户原话"去掉播放按钮的背景"】Cascades 1.1 的 Button
                    // 没有 background Q_PROPERTY（button.h 实证只有 text/image/
                    // imageSource 三项），它的粉色圆角 chrome 来自 CASCADES_THEME
                    // primaryColor（旧值 0xFB7299 是 BiliBar 哔哩粉；1.0.0.54 起
                    // 已按 design/tokens.json 改为 0xFE2C55）—— 只要还是 Button
                    // 就去不掉底。ImageButton 天生无 chrome（右侧六钮同款，已验证），
                    // 仅渲染图片本身，官方抖音控制条就是一个裸图标。切换埋点改挂
                    // defaultImageSource 的 NOTIFY（imagebutton.h:106 实证），
                    // ICON play/pause 埋点一行不缺（GATE10-B3 同步改锚点）。
                    ImageButton {
                        id: playBtn
                        verticalAlignment: VerticalAlignment.Center
                        preferredWidth: 44
                        preferredHeight: 44
                        defaultImageSource: (videoPlayer && videoPlayer.playing)
                                            ? "asset:///images/pause.png"
                                            : "asset:///images/play.png"
                        // 【1.0.0.55 / UI-AUDIT P1-3】旧写法按下给同图 → 零反馈。
                        // 播放控制条用的是 96 主图，不适用 44px _s/_sp 矩阵，
                        // 故生成器另出 _p（RGB×0.60）按压主图。
                        pressedImageSource: (videoPlayer && videoPlayer.playing)
                                            ? "asset:///images/pause_p.png"
                                            : "asset:///images/play_p.png"
                        // 禁用态沿用 default，视觉降级交给 QML opacity，不另出资产。
                        disabledImageSource: (videoPlayer && videoPlayer.playing)
                                            ? "asset:///images/pause.png"
                                            : "asset:///images/play.png"
                        // 【1.0.0.35 图标美化运行时证据】无 root 截屏通道下，用
                        // defaultImageSourceChanged 的 NOTIFY 回调记录图标真实切换
                        // （ICON pause = 播放中显示暂停图标；ICON play = 暂停态）。
                        // 按钮本身不跳号：GATE10-B3 锁住这行必须存在。
                        onDefaultImageSourceChanged: feedPage.logSwipe("ICON " + (videoPlayer && videoPlayer.playing ? "pause" : "play"))
                        onClicked: {
                            feedPage.logSwipe("CTRL togglePlay");
                            if (videoPlayer && videoPlayer.togglePlay) videoPlayer.togglePlay();
                        }
                    }

                    // 时长：当前位置 / 总时长。
                    Label {
                        verticalAlignment: VerticalAlignment.Center
                        preferredWidth: 110
                        text: feedPage.fmtTime(feedPage.seeking ? feedPage.seekUiMs : videoPlayer.positionMs) +
                              " / " + ((videoPlayer.durationMs > 0) ? feedPage.fmtTime(videoPlayer.durationMs) : "--:--")
                        textStyle.color: Color.White
                        textStyle.fontSize: FontSize.XSmall
                    }

                    // 进度条。
                    // 【1.0.0.24 修 seek 反馈风暴（真机 1346 条 SEEK drag 实证）】
                    // 程序化 value 回写（positionMs 绑定每秒推一次）同样会触发
                    // immediateValueChanged/valueChanged —— 首版把这两个信号当成
                    // "用户在拖"，于是每秒自动 seekTo 两次，解码器被反复重启。
                    // 判据改成**触摸**：只有 Slider 真的收到 Down/Up 才算拖动；
                    // 松手时补最终 seek。
                    Slider {
                        id: seekBar
                        verticalAlignment: VerticalAlignment.Center
                        preferredWidth: Math.max(120, feedPage.videoW - (feedPage.videoW >= 720 ? 108 : 16) - 160)
                        preferredHeight: 44
                        fromValue: 0
                        toValue: (videoPlayer.durationMs > 0) ? videoPlayer.durationMs : 1
                        value: feedPage.seeking ? feedPage.seekUiMs : videoPlayer.positionMs
                        onTouch: {
                            if (event.touchType === TouchType.Down) {
                                // 起拖基准 = 当前位置，别让条子先跳 0。
                                feedPage.seekUiMs = videoPlayer.positionMs;
                                feedPage.seeking = true;
                                feedPage.logSwipe("SEEK touch down=" + feedPage.seekUiMs);
                            } else if (event.touchType === TouchType.Up) {
                                feedPage.seeking = false;
                                if (videoPlayer && videoPlayer.seekTo) videoPlayer.seekTo(seekBar.value);
                                feedPage.logSwipe("SEEK release to=" + Math.round(seekBar.value));
                            }
                        }
                        onImmediateValueChanged: {
                            // 手指拖动才跟手 + 400ms 节流 seek；程序化回写直接忽略。
                            if (!feedPage.seeking) return;
                            feedPage.seekUiMs = immediateValue;
                            var nowMs2 = (new Date()).getTime();
                            if (nowMs2 - feedPage.lastDragSeekMs >= 400) {
                                feedPage.lastDragSeekMs = nowMs2;
                                if (videoPlayer && videoPlayer.seekTo) videoPlayer.seekTo(immediateValue);
                                feedPage.logSwipe("SEEK drag to=" + Math.round(immediateValue));
                            }
                        }
                    }
                }
            }
        }

        // ===== 第 3 层：右侧操作列（固定，绑当前行；Button.clicked 可靠） =====
        // 【1.0.0.46 图标化重构（UI 评审 P0-2）】原列 ♡♥★☆ Unicode 字形、
        //   「评论/关注/收藏/下载」中文按钮、计数小字三代同列且无分组；
        //   Unicode 媒体符号在 BB10 上缺字形即豆腐块（1.0.0.29 播放键已换
        //   PNG，此列一直没换）。现全列统一为 Button.imageSource 图标
        //   （播放键同款写法：text:"" + imageSource + onImageSourceChanged
        //   留痕），三条 Divider 按「互动 / 关系 / 离线」分三组。
        //   纪律：所有绑定保持单表达式；**onClicked 函数体一字未动**
        //   （toggle / toggleCollect / toggleFollow / CMT-BTN / favCurRow /
        //   dlCurRow 全部原样，测试命令链路零影响）。
        // 【1.0.0.47 紧凑化 + 流畅度（用户反馈「按钮太宽、排列不够紧凑」）】
        //   ① 按钮宽 84→48：原 84 是按钮 chrome 宽，图标只有约 44，两侧各
        //      20px 空 chrome 显得"按钮比图标大一排"；收到图标大小后 chrome
        //      贴图标（Control 无 background 属性，按钮底衬去不掉，只能缩尺寸）。
        //   ② 排列收紧：头像 64→56、Divider 边距 16/3→12/2 且高度 2→1、
        //      计数 Label 间距 8→4、列 bottomMargin 60→48。
        //   ③ 流畅度：全列定宽定高无 Fill，计数 Label 定宽 60 —— 计数从
        //      "999" 变 "1.2万" 时文字变宽不再撑挤整列；下载进度 Label 去掉
        //      visible 显隐（原来状态出现时整列跳变），改常驻占位。
        Container {
            horizontalAlignment: HorizontalAlignment.Right
            verticalAlignment: VerticalAlignment.Center
            rightMargin: 10
            bottomMargin: 48
            preferredWidth: 96
            // 允许覆盖层下面的节点也参与触摸传播：手势从右侧按钮区起手时，
            // 根容器仍能收到 Down/Up（默认 Deny 会被 Button 吃掉）。
            layout: StackLayout {}

            // ---- 组 0：作者头像（56px + 白色描边，从黑底分离）----
            Container {
                horizontalAlignment: HorizontalAlignment.Center
                preferredWidth: 56
                preferredHeight: 56
                background: theme.textPrimary
                leftPadding: 2
                rightPadding: 2
                topPadding: 2
                bottomPadding: 2
                layout: DockLayout {}
                bottomMargin: 8

                ImageView {
                    horizontalAlignment: HorizontalAlignment.Fill
                    verticalAlignment: VerticalAlignment.Fill
                    imageSource: feedPage.faceSourceOf(feedPage.curRow ? feedPage.curRow.authorFace : "")
                    scalingMethod: ScalingMethod.AspectFill
                }
            }

            Divider {
                leftMargin: 12
                rightMargin: 12
                topMargin: 2
                bottomMargin: 2
                preferredHeight: 1
            }

            // ---- 组 1 互动：赞 / 评论 / 藏 ----
            // 赞（icon: ic_like / ic_like_gray 红灰双态，仓库既有素材；
            // 红心是唯一彩色图标 = 点赞的产品强调色，其余全白统一）
            // 【1.0.0.48】Button → ImageButton（用户反馈「按钮还是太宽」定论）：
            // 真机实证竖排 StackLayout 里的 Button 声明 preferredWidth 48 无效，
            // 实际渲染 108x56（96px 图标 + 默认 chrome 驱动最小宽），比图标
            // （20~38px）大一截；ImageButton 无 chrome、无 text，尺寸=图片，
            // 配 44x44 新素材后渲染即 44x44（preferred 44 双保险）。
            // pressedImageSource 必配（NDK 实证：不配按下时图标透明消失）。
            // 纪律：onClicked 函数体一字未动；ICON 埋点改挂
            // onDefaultImageSourceChanged，文案一字不动。
            ImageButton {
                horizontalAlignment: HorizontalAlignment.Center
                preferredWidth: 44
                preferredHeight: 44
                defaultImageSource: dyPlayerStore.liked
                            ? "asset:///images/ic_like_s.png"
                            : "asset:///images/ic_like_gray_s.png"
                // 【1.0.0.55 / UI-AUDIT P1-2】按压态必须与 default 状态分支同构：
                // 旧写法写死 ic_like_sp，未点赞时按下会跳回红心。
                pressedImageSource: dyPlayerStore.liked
                            ? "asset:///images/ic_like_sp.png"
                            : "asset:///images/ic_like_gray_sp.png"
                onDefaultImageSourceChanged: feedPage.logSwipe("ICON like=" + (dyPlayerStore.liked ? "on" : "off"))
                onClicked: {
                    dyPlayerStore.toggleLike();
                }
            }
            Label {
                horizontalAlignment: HorizontalAlignment.Center
                preferredWidth: 60
                text: feedPage.htmlcount(dyPlayerStore.likeCount)
                textStyle.color: theme.textOnVideo
                textStyle.fontSize: FontSize.XSmall
                bottomMargin: 4
            }

            // 评论（1.0.0.23：push 评论页看真实评论；onClicked 逻辑未动）
            // 【1.0.0.48】Button → ImageButton 44x44，同赞按钮注释。
            ImageButton {
                horizontalAlignment: HorizontalAlignment.Center
                preferredWidth: 44
                preferredHeight: 44
                defaultImageSource: "asset:///images/ic_comment_s.png"
                pressedImageSource: "asset:///images/ic_comment_sp.png"
                onClicked: {
                    var id = (feedPage.curRow && feedPage.curRow.awemeId) ? feedPage.curRow.awemeId : "";
                    feedPage.logSwipe("CMT-BTN id=" + id);
                    if (id.length > 0 && feedPage.host && feedPage.host.pushComments) {
                        // 【1.0.0.29】try/catch 兜底：push 失败也要可见。
                        var rc = "no-host";
                        try {
                            rc = feedPage.host.pushComments(id);
                        } catch (e) {
                            rc = "EX:" + e;
                        }
                        feedPage.logSwipe("CMT-BTN push ret=" + rc);
                        if (rc !== "pushed" && dyPlayerStore && dyPlayerStore.showTip) {
                            dyPlayerStore.showTip("评论页打开失败：" + rc);
                        }
                    }
                }
            }
            Label {
                horizontalAlignment: HorizontalAlignment.Center
                preferredWidth: 60
                text: feedPage.htmlcount(dyPlayerStore.commentCount)
                textStyle.color: theme.textOnVideo
                textStyle.fontSize: FontSize.XSmall
                bottomMargin: 4
            }

            // 藏（服务端收藏；icon: 星形描边/实心双态，collected NOTIFY 驱动）
            // 【1.0.0.48】Button → ImageButton 44x44。
            ImageButton {
                horizontalAlignment: HorizontalAlignment.Center
                preferredWidth: 44
                preferredHeight: 44
                defaultImageSource: dyPlayerStore.collected
                            ? "asset:///images/ic_star_on_s.png"
                            : "asset:///images/ic_star_s.png"
                pressedImageSource: dyPlayerStore.collected
                            ? "asset:///images/ic_star_on_sp.png"
                            : "asset:///images/ic_star_sp.png"
                onDefaultImageSourceChanged: feedPage.logSwipe("ICON collect=" + (dyPlayerStore.collected ? "on" : "off"))
                onClicked: {
                    dyPlayerStore.toggleCollect();
                }
            }
            Label {
                horizontalAlignment: HorizontalAlignment.Center
                preferredWidth: 60
                text: feedPage.htmlcount(dyPlayerStore.collectCount)
                textStyle.color: theme.textOnVideo
                textStyle.fontSize: FontSize.XSmall
                bottomMargin: 4
            }

            Divider {
                leftMargin: 12
                rightMargin: 12
                topMargin: 2
                bottomMargin: 2
                preferredHeight: 1
            }

            // ---- 组 2 关系：关注（人形描边/实心双态）----
            // 【1.0.0.48】Button → ImageButton 44x44。
            ImageButton {
                horizontalAlignment: HorizontalAlignment.Center
                preferredWidth: 44
                preferredHeight: 44
                defaultImageSource: dyPlayerStore.authorFollowed
                            ? "asset:///images/ic_person_on_s.png"
                            : "asset:///images/ic_person_s.png"
                pressedImageSource: dyPlayerStore.authorFollowed
                            ? "asset:///images/ic_person_on_sp.png"
                            : "asset:///images/ic_person_sp.png"
                onDefaultImageSourceChanged: feedPage.logSwipe("ICON follow=" + (dyPlayerStore.authorFollowed ? "on" : "off"))
                onClicked: {
                    dyPlayerStore.toggleFollow();
                }
            }

            Divider {
                leftMargin: 12
                rightMargin: 12
                topMargin: 2
                bottomMargin: 2
                preferredHeight: 1
            }

            // ---- 组 3 离线：收藏（本地收藏夹）/ 下载 ----
            // 【1.0.0.37】本地收藏夹（与「藏」的服务端收藏区分：用文件夹图标）。
            // favVersion 锚点保留：toggle 后 C++ favVersion++ → 绑定重算
            // （v102 同款机制），已收藏态 = 实心文件夹 ic_folder_on。
            // 【1.0.0.48】Button → ImageButton 44x44。
            ImageButton {
                horizontalAlignment: HorizontalAlignment.Center
                preferredWidth: 44
                preferredHeight: 44
                defaultImageSource: (feedPage.curRow && feedPage.curRow.awemeId
                              && dyFavModel.favText(feedPage.curRow.awemeId,
                                                    dyFavModel.favVersion) === "已收藏")
                            ? "asset:///images/ic_folder_on_s.png"
                            : "asset:///images/ic_folder_s.png"
                pressedImageSource: (feedPage.curRow && feedPage.curRow.awemeId
                              && dyFavModel.favText(feedPage.curRow.awemeId,
                                                    dyFavModel.favVersion) === "已收藏")
                            ? "asset:///images/ic_folder_on_sp.png"
                            : "asset:///images/ic_folder_sp.png"
                onClicked: {
                    feedPage.favCurRow();
                }
            }

            // 【1.0.0.37】下载（mp4 落盘 videos/，菜单「已下载」里本地播）。
            // 三态一个入口：下载→开始 / 下载中=取消 / 已下载→提示（对勾图标）。
            // 状态判定带 dlVersion 锚点（下载中变"取消"，完成变"已下载"）。
            // 【1.0.0.48】Button → ImageButton 44x44。
            ImageButton {
                horizontalAlignment: HorizontalAlignment.Center
                preferredWidth: 44
                preferredHeight: 44
                defaultImageSource: (feedPage.curRow && feedPage.curRow.awemeId
                              && dyDownloadStore.dlText(feedPage.curRow.awemeId,
                                                       dyDownloadStore.dlVersion) === "已下载")
                            ? "asset:///images/ic_dl_ok_s.png"
                            : "asset:///images/ic_download_s.png"
                pressedImageSource: (feedPage.curRow && feedPage.curRow.awemeId
                              && dyDownloadStore.dlText(feedPage.curRow.awemeId,
                                                       dyDownloadStore.dlVersion) === "已下载")
                            ? "asset:///images/ic_dl_ok_sp.png"
                            : "asset:///images/ic_download_sp.png"
                onClicked: {
                    feedPage.dlCurRow();
                }
            }
            // 下载状态行（进度/提示，激活下载时才有字）。
            // 【1.0.0.47 流畅度】去掉 visible: text.length > 0 的显隐切换：
            // 状态出现/消失时 StackLayout 整列重排，下方两个按钮会跳位；
            // 改为常驻占位（定宽定高），文字变化只重绘不重排。
            Label {
                horizontalAlignment: HorizontalAlignment.Center
                preferredWidth: 96
                preferredHeight: 12
                text: (feedPage.curRow && feedPage.curRow.awemeId)
                      ? dyDownloadStore.progressFor(feedPage.curRow.awemeId, dyDownloadStore.dlVersion)
                      : ""
                textStyle.color: theme.accent
                textStyle.fontSize: FontSize.XXSmall
                bottomMargin: 6
            }
        }

        // 【1.0.0.46】原「第 4 层：底部信息条」整体删除（UI 评审 P0-1）。
        // 1.0.0.22 起它一直 visible:false（为竖屏视频全高而隐藏），但既没做
        // overlay 替代也没删，成为死结构；信息层回归放在控件层左侧信息块
        // （见上方 videoUiLayer 内的 作者+标题+计数 块），这里不再重复声明。

        // ===== 第 5 层：播放错误 HUD（只承担真错误 + 重试） =====
        // 【1.0.0.46】轻提示不再走这里：原来 tip 与 error 共用 #CC000000 全屏
        // 遮罩，「已切换到「推荐」」这类切换反馈会把画面挡 3 秒，与真错误
        // （播不了）混为同一视觉等级 → 用户对错误的敏感度被稀释。
        // 轻提示改走下方的 feedToast（第 5.5 层，DouyinCommentPage pubToast
        // 同款：560px 居中深色小条，不遮视频）；本层只在 error 出现。
        Container {
            id: hud
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Center
            leftPadding: 60
            rightPadding: 60
            topPadding: 30
            bottomPadding: 30
            visible: dyPlayerStore.error.length > 0 && !dyPlayerStore.loading
            background: theme.overlayScrim
            layout: StackLayout {}

            Label {
                horizontalAlignment: HorizontalAlignment.Center
                text: dyPlayerStore.error
                textStyle.color: theme.textPrimary
                textStyle.fontSize: FontSize.Small
                multiline: true
                bottomMargin: 14
            }
            Button {
                horizontalAlignment: HorizontalAlignment.Center
                text: "重试播放"
                visible: dyPlayerStore.error.length > 0
                onClicked: {
                    // 【1.0.0.37】优先「换新地址」重试：feed 行里的 play_addr 是
                    // 拉列表时拿的，CDN 直链几小时就过期（403 → open-fail）。
                    // 旧代码 load(同一行) 只会拿同一个过期 URL 再失败一次，
                    // 用户体感"重试也没用"。loadDetail(id) 重新请求 iteminfo
                    // 拿新鲜链路（一次 TLS 握手 3~6s），失败才回落原行。
                    var id = (feedPage.curRow && feedPage.curRow.awemeId) ? feedPage.curRow.awemeId : "";
                    feedPage.logSwipe("HUD retry id=" + id);
                    feedPage.playingId = "";   // 允许同一行重新拉流
                    if (id.length > 0 && dyPlayerStore && dyPlayerStore.loadDetail) {
                        dyPlayerStore.loadDetail(id);
                    } else if (feedPage.curRow && dyPlayerStore && dyPlayerStore.load) {
                        dyPlayerStore.load(feedPage.curRow);
                    }
                }
            }
        }

        // ===== 第 6 层：首屏加载转圈 / 触底加载 / feed 错误 / 未登录提示 =====
        ActivityIndicator {
            id: bigSpinner
            horizontalAlignment: HorizontalAlignment.Center
            verticalAlignment: VerticalAlignment.Center
            preferredWidth: 96
            preferredHeight: 96
            running: model.loading
            visible: model.loading
        }

        ActivityIndicator {
            id: moreSpinner
            horizontalAlignment: HorizontalAlignment.Center
            verticalAlignment: VerticalAlignment.Bottom
            preferredWidth: 120
            preferredHeight: 120
            bottomMargin: 100
            running: model.loadingMore
            visible: model.loadingMore
        }

        Container {
            id: errorBox
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Center
            visible: model.error.length > 0 && !model.loading
            leftPadding: 60
            rightPadding: 60
            topPadding: 40
            bottomPadding: 40
            layout: StackLayout {}

            Label {
                text: model.error
                horizontalAlignment: HorizontalAlignment.Center
                textStyle.color: theme.textPrimary
                textStyle.fontSize: FontSize.Small
                multiline: true
                bottomMargin: 20
            }
            Button {
                text: "重试"
                horizontalAlignment: HorizontalAlignment.Center
                onClicked: {
                    model.load();
                }
            }
        }

        // ===== 【1.0.0.46】第 5.5 层：轻提示 toast（showTip 专用） =====
        // 与 DouyinCommentPage 的 pubToast（1.0.0.37）同款范式：居中深色
        // 小条、560px 定宽、3 秒自消由 C++ setTip 定时器驱动（零 C++ 改动）。
        // 声明在 HUD/转圈/顶条之后、抽屉与手势层之前：z 序压住视频层与
        // 操作列，但不抢手势层的触摸接力（背景透明层 Allow 继续分发）。
        Container {
            objectName: "feedToast"
            horizontalAlignment: HorizontalAlignment.Center
            verticalAlignment: VerticalAlignment.Center
            visible: dyPlayerStore.tip.length > 0
            background: theme.toastBg
            preferredWidth: 560
            leftPadding: 20
            rightPadding: 20
            topPadding: 12
            bottomPadding: 12
            layout: DockLayout {}

            Label {
                horizontalAlignment: HorizontalAlignment.Fill
                verticalAlignment: VerticalAlignment.Center
                text: dyPlayerStore.tip
                textStyle.color: theme.textPrimary
                textStyle.fontSize: FontSize.Small
                multiline: true
            }
        }

        // ===== 顶部常驻条：登录态 + 菜单按钮 + 设置入口 =====
        // 【1.0.0.22 用户反馈】左下角三条杠（TabbedPane）隐藏后，右侧滑手势呼不出
        //   侧边栏，"推荐和设置"两个入口一起消失了 → 做一条常驻顶条。
        // 【1.0.0.24 用户反馈】左上角加菜单按钮，抽屉里放「同城/推荐/关注/设置/历史」。
        Container {
            verticalAlignment: VerticalAlignment.Top
            horizontalAlignment: HorizontalAlignment.Fill
            preferredHeight: 72
            background: theme.surfaceSemi
            leftPadding: 12
            rightPadding: 16

            layout: StackLayout {
                orientation: LayoutOrientation.LeftToRight
            }

            // 左上角菜单按钮（汉堡）。点击展开/收起抽屉（drawerAnim）。
            Button {
                verticalAlignment: VerticalAlignment.Center
                preferredWidth: 64
                preferredHeight: 52
                text: "\u2630"
                onClicked: {
                    feedPage.menuOpen = !feedPage.menuOpen;
                    feedPage.logSwipe("MENU toggle open=" + feedPage.menuOpen);
                }
            }

            Label {
                text: (biliApi && biliApi.loggedIn)
                      ? ("已登录 · " + (biliApi.userName.length > 0 ? biliApi.userName : "抖音用户"))
                      : "未登录 · 匿名推荐流"
                textStyle.color: theme.textOnVideo
                textStyle.fontSize: FontSize.XSmall
                verticalAlignment: VerticalAlignment.Center
                leftMargin: 6
            }

            // 【1.0.0.29】右上角「刷新」按钮（用户要求）：图标按钮，圆形回旋箭头
            // （refresh.png，PIL 纯白透明底）。语义 = 重拉当前流 + 回顶部重播，
            // 与菜单同项分支（只重播缓存）区分：这里走真网络 load()。
            Button {
                horizontalAlignment: HorizontalAlignment.Right
                verticalAlignment: VerticalAlignment.Center
                preferredWidth: 52
                preferredHeight: 48
                text: ""
                imageSource: "asset:///images/refresh.png"
                onClicked: {
                    feedPage.refreshFeed();
                }
            }

            Button {
                horizontalAlignment: HorizontalAlignment.Right
                verticalAlignment: VerticalAlignment.Center
                preferredWidth: 112
                preferredHeight: 48
                text: (biliApi && biliApi.loggedIn) ? "设置 >" : "去登录 >"
                onClicked: feedPage.doTopbarLogin()
            }
        }

        // ===== 抽屉菜单（同城/推荐/关注/设置/历史，1.0.0.24）=====
        // 声明在视频层之后 = z 序更高，盖在视频上；左对齐 300px 宽，右侧留出的
        // 区域由手势层收"轻点关闭"（gestureUp 的 menuOpen 分支）。
        Container {
            id: menuDrawer
            objectName: "menuDrawer"
            horizontalAlignment: HorizontalAlignment.Left
            verticalAlignment: VerticalAlignment.Fill
            preferredWidth: 300
            visible: feedPage.menuOpen
            // 开关各带一次位移动画（Cascades 1.1 的隐式动画挂法）。
            translationX: feedPage.menuOpen ? 0 : -320
            background: theme.surfaceSemi
            topMargin: 74
            bottomMargin: 0
            layout: StackLayout {}
            animations: [
                TranslateTransition {
                    duration: 200
                    easingCurve: StockCurve.CubicOut
                }
            ]

            Label {
                text: "菜单"
                textStyle.color: theme.textSecondary
                textStyle.fontSize: FontSize.XSmall
                horizontalAlignment: HorizontalAlignment.Center
                topMargin: 10
                bottomMargin: 8
            }

            // 同城（/aweme/v1/nearby/feed/，host 探针实证匿名可用）
            Button {
                horizontalAlignment: HorizontalAlignment.Fill
                preferredHeight: 64
                text: feedPage.feedKind === "city" ? "● 同城" : "同城"
                onClicked: {
                    feedPage.switchFeed("city");
                }
            }
            // 推荐（当前默认流）
            Button {
                horizontalAlignment: HorizontalAlignment.Fill
                preferredHeight: 64
                text: feedPage.feedKind === "recommend" ? "● 推荐" : "推荐"
                onClicked: {
                    feedPage.switchFeed("recommend");
                }
            }
            // 关注（需登录；匿名直给提示，不假装有内容）
            Button {
                horizontalAlignment: HorizontalAlignment.Fill
                preferredHeight: 64
                text: feedPage.feedKind === "follow" ? "● 关注" : "关注"
                onClicked: {
                    feedPage.switchFeed("follow");
                }
            }
            // 设置（Cookie 登录 / 运行诊断，与顶条右侧按钮同栈）
            // 【1.0.0.27 修「点了没反应」】旧代码只 menuOpen=false + openLogin()，
            // 不记日志不报结果：真机今早两次静默关闭（08:18:31 / 09:41:30 附近
            // 无任何 MENU 日志而抽屉消失），若 push 失败用户完全无从感知。
            // 现在把 main.qml 的返回值（pushed / settings-null / no-host）落盘 +
            // 失败时 HUD 明示。
            Button {
                horizontalAlignment: HorizontalAlignment.Fill
                preferredHeight: 64
                text: "设置"
                onClicked: feedPage.doMenuSettings()
            }
            // 历史（观看记录，点击记录复播）
            Button {
                horizontalAlignment: HorizontalAlignment.Fill
                preferredHeight: 64
                text: "历史"
                onClicked: {
                    feedPage.menuOpen = false;
                    var r2 = "no-host";
                    try {
                        if (feedPage.host && feedPage.host.pushHistory) {
                            r2 = feedPage.host.pushHistory();
                        }
                    } catch (e) {
                        r2 = "EX:" + e;
                    }
                    feedPage.logSwipe("MENU history ret=" + r2);
                    if (r2 !== "pushed" && dyPlayerStore && dyPlayerStore.showTip) {
                        dyPlayerStore.showTip("历史页打开失败：" + r2);
                    }
                }
            }
            // 【1.0.0.37】收藏（本地收藏夹，视频页「☆ 收藏」按钮管理）
            Button {
                horizontalAlignment: HorizontalAlignment.Fill
                preferredHeight: 64
                text: "收藏"
                onClicked: {
                    feedPage.menuOpen = false;
                    var r3 = "no-host";
                    try {
                        if (feedPage.host && feedPage.host.pushFavorites) {
                            r3 = feedPage.host.pushFavorites();
                        }
                    } catch (e) {
                        r3 = "EX:" + e;
                    }
                    feedPage.logSwipe("MENU favorites ret=" + r3);
                    if (r3 !== "pushed" && dyPlayerStore && dyPlayerStore.showTip) {
                        dyPlayerStore.showTip("收藏页打开失败：" + r3);
                    }
                }
            }
            // 【1.0.0.37】已下载（videos/ 目录里的本地 mp4，离线可播）
            Button {
                horizontalAlignment: HorizontalAlignment.Fill
                preferredHeight: 64
                text: "已下载"
                onClicked: {
                    feedPage.menuOpen = false;
                    var r4 = "no-host";
                    try {
                        if (feedPage.host && feedPage.host.pushDownloads) {
                            r4 = feedPage.host.pushDownloads();
                        }
                    } catch (e) {
                        r4 = "EX:" + e;
                    }
                    feedPage.logSwipe("MENU downloads ret=" + r4);
                    if (r4 !== "pushed" && dyPlayerStore && dyPlayerStore.showTip) {
                        dyPlayerStore.showTip("已下载页打开失败：" + r4);
                    }
                }
            }
        }

        // ===== 第 7 层：全局手势接收层（声明在最后 = z 序最高） =====
        // 1.0.0.19 真机注入实测：只靠根容器冒泡时，从底部信息条起手的上滑
        // 5 次里 0 次能到根容器（被 ListView 抢走并改发 Cancel），而从中部
        // 视频区起手 3/3 成功。最顶层这一层保证**任何位置**起手都先收到
        // Down/Move/Up；Allow 让下面的按钮仍能被点到，gestureTouch() 里的
        // gestureActive 去重 + Cancel 结算负责把被抢的手势也算完整。
        Container {
            objectName: "dyGestureLayer"
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Fill
            background: Color.Transparent
            overlapTouchPolicy: OverlapTouchPolicy.Allow
            onTouch: feedPage.gestureTouch(event)
        }
    }
}
