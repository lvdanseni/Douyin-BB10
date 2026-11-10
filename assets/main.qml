import bb.cascades 1.1

import "."
// 全局导航：NavigationPane 单栈（抖音原生客户端；1.0.0.22 由 TabbedPane 改造）
//
// 【为什么去掉 TabbedPane（用户 1.0.0.21 反馈）】
//   截图里用户看到：① 屏幕最底部一条深蓝灰"横条"；② 左下角"三条杠"；③ 向
//   左滑屏幕会滑出 tab 切换选项。这三个全是 BB10 **TabbedPane 平台自带**的，
//   没有属性能彻底隐藏（TabbedPane 必须保留 tab 才能切换）。用户要的是抖音式
//   纯净全屏：只有视频 + 右侧操作列。因此根节点改用 NavigationPane：
//   - 无底部 tab 横条、无左侧三条杠、无侧滑切 tab；
//   - 设置页（Cookie 登录 / 运行诊断）不再常驻一个 Tab，改为从推荐页
//     "去登录"按钮 push 进同一导航栈，返回走系统返回手势 / QML pop。
//
// 【点击跳转的实现方式（沿用】不用 C++ 信号、不用 Connections（Cascades 1.1
//   没有 Connections）、也不做多跳 QML 信号转发。NavigationPane 通过 nav 属性
//   直接注入给页面，由页面在 onTriggered 里同步调用根节点 createObject + push。
//
// 【GATE3 依赖】C++ createScene() 用 root->findChild("dyNav") 兜底连接
//   popTransitionEnded（系统返回手势停流）。objectName "dyNav" 必须保留。
NavigationPane {
    id: dyNav
    objectName: "dyNav"

    // 推荐视频流页 = 唯一直驻页（启动首屏）。
    // id: feedHome —— 历史页点击复播要回到这一页调 playRow()（同步直调，不走信号）。
    DouyinFeedPage {
        id: feedHome
        nav: dyNav
        host: dyNav
        model: dyFeedModel
    }

    // 页面工厂全部放在根节点完成 —— BBTube 唯一验证过的位置
    // （ComponentDefinition 与调用 createObject 的函数必须在同一文档作用域内）。
    // source 用 assets 根目录下的平级文件名，不要写 ../，也不要用 asset:///。
    attachedObjects: [
        ComponentDefinition {
            id: notePageDef
            source: "NotePage.qml"
        },
        ComponentDefinition {
            id: probePageDef
            source: "ProbePage.qml"
        },
        // 抖音视频播放页（当前无 Tab push 它，保留工厂供旧 QML 的
        // host.pushDyVideo() 兜底；零运行时成本）。
        ComponentDefinition {
            id: dyPageDef
            source: "DouyinPlayerPage.qml"
        },
        // 设置 / 登录页：推荐页"去登录"按钮 push 进栈（替代原 Tab 切换）。
        ComponentDefinition {
            id: settingsDef
            source: "SettingsPage.qml"
        },
        // 【1.0.0.23】评论页：推荐页右侧操作列「评论」按钮 push 进栈。
        ComponentDefinition {
            id: commentDef
            source: "DouyinCommentPage.qml"
        },
        // 【1.0.0.24】观看历史页：抽屉菜单「历史」push 进栈，点击记录复播。
        ComponentDefinition {
            id: historyDef
            source: "DouyinHistoryPage.qml"
        },
        // 【1.0.0.37】收藏页 / 已下载页：抽屉菜单两项，click 播放或本地播放。
        ComponentDefinition {
            id: favDef
            source: "DouyinFavoritePage.qml"
        },
        ComponentDefinition {
            id: downloadDef
            source: "DouyinDownloadPage.qml"
        }
    ]

    /**
     * 【1.0.0.33 测试命令通道】无 root 触注入/截屏环境的功能验证桥。
     * C++ onTestCmdTick() 读到沙箱 dy_test_cmd.txt 后置 app.testCmd（"seq|cmd"），
     * 本单表达式绑定重算 → feedHome.runTestCmd() 分发。各命令执行体与按钮
     * onClicked 是同一个函数（login/menu-settings/refresh 已 refactor 抽出；
     * comment/publish 走同一 push/publish 路径），日志落 douyin_swipe.log。
     */
    property string appTestCmd: app.testCmd
    onAppTestCmdChanged: feedHome.runTestCmd(dyNav.appTestCmd)

    /**
     * 用户点"去登录" → push 设置页进同一导航栈。
     * 【1.0.0.29 修「点设置/去登录没反应」】旧代码无守卫无 try/catch：settingsDef
     * 为 null 时 createObject() 抛 TypeError，调用方的 logSwipe/showTip 全被跳过
     * → 真机零日志零反馈（1.0.0.28 实证：ROOT DOWN 有坐标 + CANCEL 模式 + 无 MENU
     * 日志 = 按钮被点但调用内抛异常）。现在失败一律折成返回值：
     *   pushed / def-missing / settings-null / EX:<异常>
     * 调用点（DouyinFeedPage）用 try/catch 兜底，保证日志必落、失败必提示。
     */
    function openLogin() {
        if (!settingsDef) {
            return "def-missing";
        }
        try {
            var page = settingsDef.createObject();
            if (!page) {
                return "settings-null";
            }
            page.nav = dyNav;
            dyNav.push(page);
            return "pushed";
        } catch (e) {
            return "EX:" + e;
        }
    }

    /** 旧组件遗留调用点兜底：行为同 openLogin。 */
    function openSettings() {
        return dyNav.openLogin();
    }

    /**
     * 【1.0.0.23】push 评论页并加载指定视频的评论。
     * 顺序守 GATE4 铁律：先 push 再赋 awemeId —— 赋值那一刻页面必须已在
     * 导航栈里（onAwemeIdChanged → dyCommentModel.load 才会正常走完）。
     */
    function pushComments(awemeId) {
        if (!awemeId || awemeId.length === 0) {
            return "bad-args";
        }
        if (!commentDef) {
            return "def-obj-null";
        }
        try {
            var page = commentDef.createObject();
            if (!page) {
                return "def-null";
            }
            page.nav = dyNav;
            dyNav.push(page);
            page.awemeId = awemeId;
            return "pushed";
        } catch (e) {
            return "EX:" + e;
        }
    }

    /**
     * 【1.0.0.24】push 观看历史页。
     * 顺序同 pushComments：createObject → 赋 nav/host → push。
     * host 传 dyNav 自身：历史行点击时历史页调 host.playHistoryItem(row)。
     */
    function pushHistory() {
        if (!historyDef) {
            return "def-obj-null";
        }
        try {
            var page = historyDef.createObject();
            if (!page) {
                return "def-null";
            }
            page.nav = dyNav;
            page.host = dyNav;
            dyNav.push(page);
            return "pushed";
        } catch (e) {
            return "EX:" + e;
        }
    }

    /**
     * 【1.0.0.24】历史页点击一条记录 → 复播。
     * 先 pop 回推荐页（首页是导航栈根，pop 后立即可见），再同步直调
     * feedHome.playRow(row) —— 不走 C++ 信号中转（铁律 4）。
     */
    function playHistoryItem(row) {
        if (!row || !row.awemeId || row.awemeId.length === 0) {
            return "bad-args";
        }
        try {
            return dyNav.playFeedRow(row);
        } catch (e) {
            return "EX:" + e;
        }
    }

    /**
     * 【1.0.0.37】统一的"播放这一行"回调（历史/收藏/已下载三个页面共用）。
     * 已下载页会把行的 playUrl 换成 file:// 本地路径后传进来，
     * DouyinPlayerStore::load → startStream 对 file:// 走本地直服分支。
     */
    function playFeedRow(row) {
        if (!row || !row.awemeId || row.awemeId.length === 0) {
            return "bad-args";
        }
        try {
            dyNav.pop();
            if (feedHome && feedHome.playRow) {
                feedHome.playRow(row);
            }
            return "playing";
        } catch (e) {
            return "EX:" + e;
        }
    }

    /** 【1.0.0.37】push 收藏页（顺序同 pushComments：createObject → push）。 */
    function pushFavorites() {
        if (!favDef) {
            return "def-obj-null";
        }
        try {
            var page = favDef.createObject();
            if (!page) {
                return "def-null";
            }
            page.nav = dyNav;
            page.host = dyNav;
            dyNav.push(page);
            return "pushed";
        } catch (e) {
            return "EX:" + e;
        }
    }

    /** 【1.0.0.37】push 已下载页。 */
    function pushDownloads() {
        if (!downloadDef) {
            return "def-obj-null";
        }
        try {
            var page = downloadDef.createObject();
            if (!page) {
                return "def-null";
            }
            page.nav = dyNav;
            page.host = dyNav;
            dyNav.push(page);
            return "pushed";
        } catch (e) {
            return "EX:" + e;
        }
    }

    // 遗留：小红书笔记详情页工厂。当前无页面调用，保留函数体避免旧组件引用失效。
    function pushNote(nav, noteId) {
        if (!nav || !noteId || noteId.length == 0) {
            return "bad-args";
        }
        if (!notePageDef) {
            return "def-obj-null";
        }
        try {
            var page = notePageDef.createObject();
            if (!page) {
                return "def-null";
            }
            page.noteId = noteId;
            page.back.connect(function() {
                videoPlayer.stop();
                noteStore.stopStream();
                nav.pop();
            });
            nav.push(page);
            return "pushed";
        } catch (e) {
            return "EX:" + e;
        }
    }

    function probe() {
        if (!probePageDef) {
            return "probe-nodef";
        }
        try {
            var p = probePageDef.createObject();
            return p ? "probe-ok" : "probe-fail";
        } catch (e) {
            return "EX:" + e;
        }
    }

    /**
     * 在指定 NavigationPane 上 push 抖音播放页，并把整行数据灌给 C++ Store。
     * 保留：GATE4 检查该函数"先 push 再赋 videoRow"顺序；首页内嵌播放后不再调用。
     */
    function pushDyVideo(nav, row) {
        if (!nav || !row) {
            return "bad-args";
        }
        if (!dyPageDef) {
            return "def-obj-null";
        }
        try {
            var page = dyPageDef.createObject();
            if (!page) {
                return "def-null";
            }
            // 【1.0.0.15 关键顺序】必须先 push 再赋 videoRow：
            // createObject() 返回时 onCreationCompleted 已经跑完（videoRow 还是 null，
            // attachRow 什么都不做），真正加载靠 onVideoRowChanged —— 只有先 push，
            // 赋值那一刻页面才在导航栈里，ForeignWindowControl 也已进入场景可认领窗口。
            page.nav = nav;
            nav.push(page);
            page.videoRow = row;
            return "pushed";
        } catch (e) {
            return "EX:" + e;
        }
    }
}
