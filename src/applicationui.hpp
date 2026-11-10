#ifndef APPLICATIONUI_HPP
#define APPLICATIONUI_HPP

#include <QObject>

class QTimer;

namespace bb { namespace cascades { class Page; class ImageTracker; } }

class NoteModel;
class NoteStore;
class CollectStore;
class HistoryStore;
class FollowStore;
class LocalDb;
class VideoPlayer;
class DouyinFeedModel;
class DouyinPlayerStore;
class DouyinCommentModel;
class DouyinHistoryModel;
class DouyinFavoriteModel;
class DouyinDownloadStore;

/**
 * QML 侧 `ui.du(...)` 的实现。
 *
 * 【为什么需要它】Cascades QML 里所有尺寸属性（preferredWidth / padding / margin …）
 * 本来就以 du（design unit）为单位，框架自己会按屏幕换算，所以 du(n) 就应该返回 n。
 *
 * 但工程里所有 QML 都写了 `ui.du(...)`，而 `ui` 从来没有被定义过（既没有 QML 单例，
 * 也没有 setContextProperty）。结果是每个 `ui.du(...)` 都在抛
 *   ReferenceError: Can't find variable: ui
 * 属性绑定因此得到 undefined，静默塌陷成 0 或"按内容自适应"——
 * 这正是详情页播放区只有一条细缝、文字贴着屏幕左边缘、评论列表高度不对的总根因。
 *
 * 之前把它误判成 ListItemComponent 的作用域问题（v15），其实两者无关。
 */
class UiHelper : public QObject {
    Q_OBJECT

public:
    explicit UiHelper(QObject* parent = 0) : QObject(parent) {}

    Q_INVOKABLE double du(double value) const { return value; }
};


/**
 * 应用入口：创建主题、各笔记模型/Store，注入 QML 上下文并加载 main.qml。
 * 对应 BBTube 的 applicationui（C++ 核心装配 + QML 场景）。
 *
 * 【从 BerryLi 到 Xiaohongshu 的主体变更】
 *   - FeedModel*（rcmd/popular/ranking/region/search/follow/history） → NoteModel*
 *     统一笔记流模型，按 Kind 区分推荐/发现/搜索/关注/历史。
 *   - FavStore* / DownloadStore* → CollectStore* / HistoryStore*
 *     收藏与历史改为独立 Store，由 LocalDb 持久化。
 *   - 新增 LocalDb*（SQLite 本地数据库 xiaohongshu.db，沙箱根目录）。
 *   - 新增 NoteStore*（笔记详情 + 播放流）。
 */
class ApplicationUI : public QObject {
    Q_OBJECT

    /**
     * 【v90】版本号给 QML 用（设置页显示 "Xiaohongshu 1.0.0.N"）。
     *
     * 为什么不用 setContextProperty("appVersion", QString)：
     * BB10 的 QmlDocument::setContextProperty 只有 QObject* 一个重载，
     * 传 QVariant/QString 直接编译失败（no matching function）。
     * 而 documentContext()->setContextProperty(...) 用的是另一个 context 对象，
     * 与 createRootObject 实际使用的那个是否一致无法确认 —— 不确定的路径不用。
     * 做成 Q_PROPERTY 最稳：`app` 这个 context property 早就在了，
     * 属性绑定在 QML 引擎内部驱动，是本工程唯一验证过 100% 可靠的 C++→QML 通道。
     *
     * 值来自 build.sh 生成的 app_version.hpp，源头是 bar-descriptor.xml，
     * 所以显示的和安装包里的是同一个号。
     */
    Q_PROPERTY(QString version READ version CONSTANT)

    /**
     * 【1.0.0.33 测试命令通道】BB10 无 root 触注入/截屏通道（INPUT_PROVIDER /
     * DISPLAY_MANAGER 特权上下文 devuser 一律 EPERM），四大功能的真机验证改用
     * 「命令文件 → C++ 轮询 → Q_PROPERTY → QML 分发」：
     *   blackberry-deploy -putFile 把 dy_test_cmd.txt 写进沙箱（tmp 或 data），
     *   onTestCmdTick() 每秒读到即自删，经本属性（值带自增序号，保证同串命令
     *   重复注入也能触发 NOTIFY）推给 QML onAppTestCmdChanged 分发执行，
     *   执行体与按钮 onClicked 是同一个函数（refactor 抽出，非替身）。
     * 走 Q_PROPERTY+NOTIFY —— 本项目唯一验证过 100% 可靠的 C++→QML 通道
     * （铁律 4：信号 .connect() 在 QML 侧静默不连）。
     */
    Q_PROPERTY(QString testCmd READ testCmd NOTIFY testCmdChanged)

public:
    explicit ApplicationUI(QObject* parent = 0);
    virtual ~ApplicationUI();

    QString version() const;
    QString testCmd() const { return m_testCmd; }

    Q_INVOKABLE void openVideo(const QString& bvid);

    /**
     * 返回启动面包屑日志内容（沙箱 douyin_boot.log）。
     * 设置页直接展示，方便在设备上肉眼看到启动卡在哪一步，无需取文件。
     */
    Q_INVOKABLE QString bootLogText() const;

    /** 网络层 + 各模型诊断汇总（curl 自检 / 请求计数 / connect 结果 / 最后一次回复）。 */
    Q_INVOKABLE QString diagText() const;

    /**
     * 返回进程内捕获到的 QML 告警/报错文本。
     * 用途：ComponentDefinition 的 createObject() 返回 null 时，QML 文件里
     * 到底是哪一行、哪个元素不被支持，全在这段文字里。
     */
    Q_INVOKABLE QString qmlLogText() const;

    /**
     * 返回 libbbplayer 的 stderr 日志尾部（含 "ERROR: ..." 等真实错误）。
     * 【为什么需要】v46 真机确认 raw=0/0、state=5（=库内部 ERROR），但错误原文只写进
     * stderr/slog2，设备上取不到。构造期已把 fd1/fd2 dup2 到沙箱 bbplayer.log，
     * libbbplayer 链接了 stdio（fprintf/vfprintf/write），其错误日志因此可被读到并显示。
     */
    Q_INVOKABLE QString playerLogText() const;

    /**
     * 代理相关环境变量的当前值（no_proxy / http_proxy / all_proxy）。
     * 【用途】v50 在 main() 里设了 no_proxy=* 来绕开 libbbnet 写死的默认代理，
     * 这个接口把实际生效的值显示到播放页叠加层，一眼确认修复是否真的落地
     * （若显示 np=(null) 说明 setenv 没生效，就得换别的路子）。
     */
    Q_INVOKABLE QString proxyEnvDiag() const;

    /**
     * 【播放链路自检】设备上没有可用的截屏 / 触摸注入通道（snapshot 是 viewport
     * 演示程序，devmode HTTP 无 /screenshot 端点，/pps/services/input/control 无注入
     * 能力），所以把「点视频能不能真的播放」改造成 C++ 主动跑一遍真实链路并用日志自证：
     *   DouyinFeedModel::rowAt(0) → DouyinPlayerStore::load(row) → VideoProxy::start
     *   → videoReadyToPlay → VideoPlayer::play → libbbplayer 连 127.0.0.1 拉流
     * 每跳都写 qDebug（落 data/bbplayer.log），14 秒后按 state / position 出结论。
     * 触发：设置页按钮，或沙箱 tmp/playback_selftest.flag 冷启动自动跑一次（自删）。
     */
    Q_INVOKABLE QString playbackSelfTest();
    /** 自检逐行轨迹，供设置页「刷新诊断信息」显示。 */
    Q_INVOKABLE QString playbackSelfTestLog() const { return m_selfTestLog; }

    /**
     * 【手势链路埋点】QML 在 onTouch / ListScrollStateHandler / playInline 各处
     * 调用 app.swipeLog(tag)，逐跳写进沙箱 data/douyin_swipe.log。
     * 为什么单独一个文件：boot.log 里自检/封面/播放日志混在一起，上滑问题要在
     * 注入手势后按时间顺序精确读到「事件有没有到、ListView 有没有滚、换流有没有起」，
     * 独立日志才能一眼定位断点。QML→C++ 的 Q_INVOKABLE 是本项目验证过的可靠通道。
     */
    Q_INVOKABLE void swipeLog(const QString& tag);

    /**
     * 【上滑分页的程序化翻页桥】QML 根容器自己判定上滑/下滑手势，然后调这里
     * 让 ListView 平滑滚到目标行。为什么不让 QML 直接调 ListView::scrollToItem：
     * 头文件里 scrollToItem 有两个重载（枚举版 + int 版，后者是 Qt 4.8 枚举
     * 参数在 QML 里不可用的 workaround），QML 解析重载容易出运行时错误。
     * C++ 侧用 objectName="dyFeedList" 找到 ListView，类型明确、可打日志。
     */
    Q_INVOKABLE void scrollFeedTo(int idx);

signals:
    void videoRequested(const QString& bvid);
    void testCmdChanged();

private slots:
    /** 场景就绪后延迟启动网络层（QTimer::singleShot(0) 触发），保证首帧先渲染。 */
    void startNetworkDeferred();
    /** 应用退出时清理资源：停止播放器、流式代理，避免进程残留导致下次启动失败。 */
    void cleanup();

    /**
     * 【v70】NavigationPane 弹出页面完成时调用。
     *
     * 为什么需要它：播放页现在照 BBTube 去掉了 TitleBar，返回只剩 BB10 的
     * 系统边缘手势。那条路径**不会**触发 QML 里的 back 信号，所以必须在这里
     * 停掉播放、释放 CDN 连接，否则退出播放页后视频还在后台播、流也不断。
     *
     * 用 C++ 侧的编译期 connect（不是 QML 的 .connect()）—— 后者对 C++ 信号
     * 在本工程不可靠（见项目铁律 4）。
     */
    void onPagePopped(bb::cascades::Page* page);
    /** 【1.0.0.14】导航栈端到端自检：push 标记页 / 延时 pop，验证 popTransitionEnded 真触发。 */
    void onNavSelfTestPush();
    void onNavSelfTestPop();
    /** 播放自检的 1 秒心跳：先等 feed 行，再采样播放器状态，最后出结论并清理。 */
    void onPlaybackSelfTestTick();
    /**
     * 【1.0.0.15】封面链路自检：feed 行 → coverUrlFor → 落盘真实格式 →
     * Cascades ImageTracker 解码结果。设备日志里给出可核对的 PASS/FAIL 判据，
     * 不再靠"下载 code=0"推断"界面上应该有图"。
     */
    void onCoverSelfTest();
    void onCoverDecodeReport();
    /**
     * 【1.0.0.15】首屏自动播放调度：feed 出行后置位 autoPlayTrigger，
     * 由 QML 绑定重算推入播放页（不在本栈内 push，避免与模型 append 重入）。
     */
    void onAutoPlayTick();
    /**
     * 【1.0.0.33】测试命令通道的 1 秒心跳：读沙箱 dy_test_cmd.txt（tmp + data），
     * 命中即自删并逐条 pushTestCmd()。见 testCmd Q_PROPERTY 注释。
     */
    void onTestCmdTick();
    /**
     * 【1.0.0.50】解码停滞看护心跳：playing 态下 rawPosition 连续 15s 不
     * 推进 → replay() 重建 FWC 输出面（灭屏冻结修复，见 m_stallWatchTimer）。
     */
    void onStallWatchTick();

private:
    void createScene();
    void setFallbackScene(const QString& message);
    /** 自检日志同时写进程内存、douyin_boot.log 和 stderr(=bbplayer.log)。 */
    void selfTestTrace(const QString& s);
    /** 【1.0.0.33】置测试命令属性 + NOTIFY；值形如 "seq|cmd"（序号保证重复可触发）。 */
    void pushTestCmd(const QString& cmd);

    /** 笔记流模型（替代原多 FeedModel，按 Kind 分推荐/发现/搜索/关注/历史）。 */
    NoteModel* m_noteModel;
    /** 搜索笔记流模型（Kind=Search）。 */
    NoteModel* m_searchModel;
    /** 笔记详情 + 播放流 Store。 */
    NoteStore* m_noteStore;
    /** 收藏夹 + 笔记收藏 Store（需登录态）。 */
    CollectStore* m_collectStore;
    /** 观看历史 Store（需登录态，持久化到 LocalDb）。 */
    HistoryStore* m_historyStore;
    /** 关注动态 Store（需登录态）。 */
    FollowStore* m_followStore;
    /** SQLite 本地数据库（xiaohongshu.db，沙箱根目录）。 */
    LocalDb* m_localDb;
    /** 抖音推荐视频流模型（解析抖音 feed 的 aweme_list）。 */
    DouyinFeedModel* m_dyFeedModel;
    /** 抖音视频详情 + 播放代理 Store。 */
    DouyinPlayerStore* m_dyPlayerStore;
    DouyinCommentModel* m_dyCommentModel;
    /** 【1.0.0.24】菜单「关注/同城」的流模型（与推荐流各自独立实例，tag 前缀错开）。 */
    DouyinFeedModel* m_dyFollowModel;
    DouyinFeedModel* m_dyCityModel;
    /** 【1.0.0.24】观看历史模型（JSON 文件，复播用）。 */
    DouyinHistoryModel* m_dyHistoryModel;
    /** 【1.0.0.37】本地收藏模型（菜单「收藏」+ 视频页收藏按钮）。 */
    DouyinFavoriteModel* m_dyFavModel;
    /** 【1.0.0.37】视频下载 Store（视频页「下载」+ 菜单「已下载」）。 */
    DouyinDownloadStore* m_dyDlStore;
    VideoPlayer* m_player;
    UiHelper* m_ui;
    QObject* m_theme;
    /** 构造期 dup2 的 stderr/stdout 日志文件路径（沙箱 bbplayer.log）。 */
    QString m_playerLogPath;
    /**
     * 【1.0.0.50】解码停滞看护（灭屏冻结修复，终版方案）。
     * 背景：BB10 auto-lock 熄屏后 libbbplayer 的 FWC 视图面被系统冻结，
     * 真机实证 idleState=1 期间 PROXY eof 100% 送达、LOOP 40s/轮照走，
     * 但亮屏后 3s 差分只剩时钟行 2.6%、视频区 0.00% —— 音频/状态机在跑，
     * 就是画面不输出。
     * 方案（零新依赖）：每 3s 比对 rawPosition（播放器真实解码推进值，
     * 非墙钟）。playing 态下 rawPosition 连续 5 个 tick（15s）不推进 =
     * 输出停滞（熄屏冻结/缓冲卡死/窗口失效皆是），调一次 replay() 强制
     * stop() 拆窗 + ensurePlayer() 重建、FWC 重新认领输出面。正常播放
     * 每 tick 必有推进；暂停态不进本分支；缓冲卡顿时 kick 也是合理自愈。
     * （screen API SCREEN_PROPERTY_POWER_MODE 方案实测首 tick 即崩，
     *  已撤：BB10 app 进程内 screen_create_context 不稳定，勿再引回。）
     */
    QTimer* m_stallWatchTimer;
    qint64 m_lastRawPos;    /**< 上 tick 的 rawPosition 基线。 */
    int m_stallTicks;       /**< 连续无推进的 tick 数。 */

    /** 播放自检状态（默认全关，只有显式触发才跑，不影响正常启动）。 */
    QTimer* m_selfTestTimer;
    int m_selfTestTick;
    int m_selfTestStartTick;
    bool m_selfTestRunning;
    QString m_selfTestLog;
    /** 自检判据：只看解码器真实推进的 rawPosition，不用墙钟兜底的 positionMs。 */
    qint64 m_selfTestMaxRawPos;
    bool m_selfTestSawError;
    /** 【1.0.0.15】首屏自动播放的轮询计数（最多 6 次 × 3 秒）。 */
    int m_autoPlayAttempts;
    /** 【1.0.0.33】测试命令通道：1 秒轮询定时器 + 当前命令值 + 自增序号。 */
    QTimer* m_testCmdTimer;
    QString m_testCmd;
    int m_testCmdSeq;
    /** 【1.0.0.49】封面 decode 自检是否已加宽过一次窗口。
     *  背景：真机 14:11 / 17:38 两次 4s 窗口内 0/3（三次 Tracker 全 0x0），
     *  而 cover selftest 同刻 PASS（文件在、真 JPEG）——ImageTracker 的
     *  异步资源加载在播放/下载负载下 4s 不够，属预言机误报。加宽一次到
     *  12s 再判，避免"狼来了"噪声侵蚀门禁可信度；不加宽多次，真失败
     *  （比如 1.0.0.14 那种全空白）12s 也足够暴露。 */
    bool m_coverDecodeRetried;
};

#endif // APPLICATIONUI_HPP
