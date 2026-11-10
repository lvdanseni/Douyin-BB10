#include "applicationui.hpp"
#include "app_version.hpp"     // build.sh 从 bar-descriptor.xml 自动生成
#include "douyin_api.hpp"
#include "note_model.hpp"
#include "note_store.hpp"
#include "collect_store.hpp"
#include "history_store.hpp"
#include "follow_store.hpp"
#include "local_db.hpp"
#include "douyin_store.hpp"
#include "video_store.hpp"

#include <bb/cascades/Application>
#include <bb/cascades/QmlDocument>
#include <bb/cascades/AbstractPane>
#include <bb/cascades/Page>
#include <bb/cascades/NavigationPane>
#include <bb/cascades/Container>
#include <bb/cascades/Label>
#include <bb/cascades/Color>
#include <bb/cascades/ImageTracker>
#include <bb/cascades/ResourceState>
#include <bb/cascades/ListView>
#include <bb/cascades/ScrollAnimation>
#include <QtDeclarative/QDeclarativeError>
#include <QDebug>
#include <QTimer>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTextStream>
#include <QDateTime>
#include <QCoreApplication>
#include <QUrl>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

using namespace bb::cascades;

/**
 * 【无人值守播放自检的一次性开关】
 * 用 blackberry-deploy -putFile 往沙箱 tmp/ 放这个文件，再 -launchApp，
 * 应用会在 startNetworkDeferred() 里自检一次并立刻删掉它（不会每次启动都播）。
 * 同时兼容 data/ 下的同名文件，避免 tmp 与 homePath 映射差异导致漏触发。
 */
static const char* const kSelfTestFlag = "playback_selftest.flag";
/** 【1.0.0.14】导航栈 pop 端到端自检 flag：push 标记页 → pop → 观察 popTransitionEnded。 */
static const char* const kNavSelfTestFlag = "nav_selftest.flag";
/**
 * 【1.0.0.33】测试命令文件：blackberry-deploy -putFile 写一行命令进沙箱
 * （tmp/ 或 data/ 均可），onTestCmdTick() 读到即自删并推 Q_PROPERTY testCmd。
 * 与两个 .flag 一样是一次性消费，不重放、不常驻。
 */
static const char* const kTestCmdFile = "dy_test_cmd.txt";

/**
 * 启动面包屑日志：只写应用自己的沙箱（tmp / 沙箱根）。
 *
 * 重要：不要写 /accounts/1000/shared —— 那需要 access_shared 权限，BB10 会在首次
 * 启动时弹出权限授权窗口，而 devMode 未签名包在授权后进程会被重启、重启再申请，
 * 形成"弹权限窗 → 闪退"的循环。诊断信息宁可难取，也不能妨碍应用启动。
 */
static void bootLog(const QString& s)
{
    const QString line = QDateTime::currentDateTime().toString("hh:mm:ss.zzz") + "  " + s;
    QStringList dirs;
    dirs << QDir::tempPath()   // 应用私有 tmp
         << QDir::homePath();  // 沙箱根 /accounts/1000/appdata/<pkg>
    // 【1.0.0.12】两个目录都写，不要在第一个成功后 return。
    // tmp/ 在应用 terminate / SIGSEGV 后会被系统清空：1.0.0.11 崩溃后
    // `-getFile tmp/douyin_boot.log` 直接 No such file，自检轨迹全丢。
    // data/（QDir::homePath()）是持久的，务必留一份，否则崩溃现场无法复盘。
    for (int i = 0; i < dirs.size(); ++i) {
        if (dirs.at(i).isEmpty()) continue;
        QDir d(dirs.at(i));
        QFile f(d.filePath("douyin_boot.log"));
        if (f.open(QIODevice::Append | QIODevice::Text)) {
            QTextStream ts(&f);
            ts << line << "\n";
            f.flush();
            f.close();
        }
    }
}

// ---- QML 报错捕获 ----------------------------------------------------------
// 设备上取不到 slog2 日志，所以把 Qt 的告警捕获到内存，由 qmlLogText() 暴露给 QML 显示。
// 用途：ComponentDefinition 的 createObject() 返回 null 时（组件加载失败），
// Qt 一定会打出具体的 QML 错误行（哪个文件、第几行、什么错误），
// 这段文字就是唯一可靠的定位依据，比反复猜"哪个元素不被支持"高效得多。
static QString g_qmlLog;
static const int kQmlLogMax = 6000;

static void qmlMsgHandler(QtMsgType type, const char* msg)
{
    Q_UNUSED(type)
    if (!msg) return;
    const QString s = QString::fromUtf8(msg);
    // 只留 QML 相关的行，避免被 BiliApi 的 qDebug 冲掉（那些每秒都有）
    if (s.contains("QDeclarative") || s.contains("Cannot assign")
        || s.contains("Unable to assign") || s.contains("is not a type")
        || s.contains("Component is not ready") || s.contains(".qml")
        || s.contains("Unknown") || s.contains("Unable to create")) {
        g_qmlLog += s + "\n";
        if (g_qmlLog.size() > kQmlLogMax) {
            g_qmlLog = g_qmlLog.right(kQmlLogMax);
        }
    }
    // 同时仍输出到 stderr（= slog2），有 SSH 时照旧可见
    fprintf(stderr, "%s\n", msg);
}

ApplicationUI::ApplicationUI(QObject* parent)
    : QObject(parent)
    , m_noteModel(0)
    , m_noteStore(0)
    , m_collectStore(0)
    , m_historyStore(0)
    , m_followStore(0)
    , m_localDb(0)
    , m_dyFeedModel(0)
    , m_dyPlayerStore(0)
    , m_dyCommentModel(0)
    // 【1.0.0.24】菜单数据源
    , m_dyFollowModel(0)
    , m_dyCityModel(0)
    , m_dyHistoryModel(0)
    , m_dyFavModel(0)
    , m_dyDlStore(0)
    , m_player(0)
    , m_ui(0)
    , m_theme(0)
{
    // 【v47 诊断】最早把 stdout/stderr 重定向到沙箱文件，捕获 libbbplayer 的 "ERROR: ..." 日志。
    // libbbplayer 链接 stdio（fprintf/vfprintf/fputs/fwrite/write），其内部错误经 fd2 输出；
    // BB10 设备上取不到 slog2，dup2 到文件后由 playerLogText() 读出显示，定位流错误真因。
    // 必须在任何 libbbplayer 调用（play）之前完成——它在本 ctor 里只是惰性创建，play 更晚。
    {
        m_playerLogPath = QDir(QDir::homePath()).filePath("bbplayer.log");
        int fd = ::open(m_playerLogPath.toUtf8().constData(),
                        O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            ::dup2(fd, 1);
            ::dup2(fd, 2);
            if (fd > 2) ::close(fd);
        }
    }
    // 必须最早安装：QML 组件加载失败的告警要在 createScene 之前就能被捕获
    qInstallMsgHandler(qmlMsgHandler);
    // 播放自检默认全关：只有设置页按钮或沙箱 flag 显式触发才实例化定时器。
    m_selfTestTimer = 0;
    m_selfTestTick = 0;
    m_selfTestStartTick = -1;
    m_selfTestRunning = false;
    m_selfTestMaxRawPos = 0;
    m_selfTestSawError = false;
    // 【1.0.0.33】测试命令通道：序号从 0 起，定时器在 startNetworkDeferred() 里armed。
    m_testCmdTimer = 0;
    m_testCmdSeq = 0;
    bootLog("=== ApplicationUI ctor begin ===");

    DouyinApi::create(this);
    bootLog("DouyinApi created");

    // 本地数据库：先创建，各 Store 可能在构造期就要用到它（例如历史记录的读取）。
    m_localDb = new LocalDb(this);
    bootLog("LocalDb created");

    // 笔记流模型（替代原多 FeedModel，按 Kind 分推荐/发现/搜索/关注/历史）。
    m_noteModel = new NoteModel(NoteModel::Rcmd, this);
    bootLog("NoteModel created");

    // 搜索笔记流模型（Kind=Search），注入 QML 为 searchModel
    m_searchModel = new NoteModel(NoteModel::Search, this);
    bootLog("Search NoteModel created");

    // 笔记详情 + 播放流 Store。
    m_noteStore = new NoteStore(this);
    bootLog("NoteStore created");

    // 收藏 Store（需登录态）。
    m_collectStore = new CollectStore(this);
    bootLog("CollectStore created");

    // 历史 Store（需登录态，持久化到 LocalDb）。
    m_historyStore = new HistoryStore(this);
    bootLog("HistoryStore created");

    // 关注动态 Store（需登录态）。
    m_followStore = new FollowStore(this);
    bootLog("FollowStore created");

    m_player = new VideoPlayer(this);
    bootLog("VideoPlayer created");

    // 播放触发走编译期信号槽，可靠（不依赖 QML .connect()）。
    // videoReadyToPlay 在 note_store 内部拿到 CDN 直链时发射，这里统一转给播放器。
    // ⚠️ 关键：connect 必须在 m_player 创建【之后】。v29~v35 这里顺序写反了——
    // 当时 m_player 还是构造初始值 0（null），Qt 对空接收者 connect 静默失败，
    // videoReadyToPlay 永远到不了 play()，播放页因此永远卡在"正在连接视频…"。
    QObject::connect(m_noteStore, SIGNAL(videoReadyToPlay(QString)),
                     m_player, SLOT(play(QString)));

    // 【v74】总时长：view 接口解析出 data.duration（秒）后直接喂给播放器，
    // 作为 libbbplayer duration 字段为 0 时的兜底。同样是编译期 connect ——
    // 不能走 QML 转发（Qt 4.8 上 C++ 信号 → QML 处理函数会静默不连接）。
    QObject::connect(m_noteStore, SIGNAL(apiDurationReady(int)),
                     m_player, SLOT(setApiDurationSec(int)));

    // ---- 抖音客户端（DouyinBar）专用 Store ----
    // 推荐视频流：解析抖音 feed 的 aweme_list，封面走与 BiliBar 完全相同的
    // md5 缓存 + 队列下载 + Q_PROPERTY(coverVersion) 绑定机制。
    m_dyFeedModel = new DouyinFeedModel(DouyinFeedModel::Recommend, this);
    bootLog("DouyinFeedModel created");

    // 播放详情：优先直接用 feed 行里已经解析好的 play_addr 播放（免 iteminfo 二次请求），
    // 行里没有才回落 iteminfo。CDN 直链经 VideoProxy 本地代理后交播放器（CDN 校验
    // Referer/UA，mmrenderer 无法设置，必须由 libcurl 侧带上）。
    m_dyPlayerStore = new DouyinPlayerStore(this);
    bootLog("DouyinPlayerStore created");

    // 【1.0.0.23】评论模型：web v2 免签端点，匿名可读；行内 api 指针供
    // ListItemComponent 解析头像（ListItemComponent 看不到 context property）。
    m_dyCommentModel = new DouyinCommentModel(this);
    bootLog("DouyinCommentModel created");

    // 【1.0.0.24】关注流 / 同城流 / 观看历史。
    // 三者都是进程内独立实例（tag 前缀 dyfollow/dycity 各自收自己的 netReply），
    // 惰性加载：切到哪个菜单项才 load()，不拖慢首屏。
    m_dyFollowModel = new DouyinFeedModel(DouyinFeedModel::Follow, this);
    bootLog("DouyinFollowModel created");
    m_dyCityModel = new DouyinFeedModel(DouyinFeedModel::City, this);
    bootLog("DouyinCityModel created");
    m_dyHistoryModel = new DouyinHistoryModel(this);
    bootLog(QString("DouyinHistoryModel created n=%1").arg(m_dyHistoryModel->count()));

    // 【1.0.0.37】本地收藏 + 视频下载（JSON 索引、videos/ 目录落盘）。
    m_dyFavModel = new DouyinFavoriteModel(this);
    bootLog(QString("DouyinFavoriteModel created n=%1").arg(m_dyFavModel->count()));
    m_dyDlStore = new DouyinDownloadStore(this);
    bootLog(QString("DouyinDownloadStore created n=%1 dir=%2")
                .arg(m_dyDlStore->count()).arg(m_dyDlStore->cacheDir()));

    // 同样走编译期 signal/slot：必须在播放器创建【之后】connect（同 noteStore 的教训）。
    QObject::connect(m_dyPlayerStore, SIGNAL(videoReadyToPlay(QString)),
                     m_player, SLOT(play(QString)));
    QObject::connect(m_dyPlayerStore, SIGNAL(apiDurationReady(int)),
                     m_player, SLOT(setApiDurationSec(int)));
    // 【1.0.0.24】代理 EOF 硬事实 → 播放器（循环播放的结束判定通道之一）。
    QObject::connect(m_dyPlayerStore, SIGNAL(streamComplete()),
                     m_player, SLOT(setEofLikely()));

    // QML 里到处在用 ui.du(...)，必须注入，否则每个尺寸绑定都抛 ReferenceError
    m_ui = new UiHelper(this);
    bootLog("UiHelper created");

    m_theme = new QObject(this);
    // 【1.0.0.46 色板收敛（UI 评审 P1-2）】旧值 bg/cardBg/textSecondary/accent
    // 与抖音系页面实际硬编码色有 drift（accent 还是 BiliBar 的哔哩粉），页面级
    // 硬编码已全部改为 theme.* 引用，此处统一对齐实际用量；overlay* 系列替代
    // feed 页散落的 #66000000/#99000000/#CC000000 遮罩；surfaceSemi/toastBg
    // 替代四页各自为政的半透明面板/toast 底色。Qt4.8 QColor 支持 #AARRGGBB。
    m_theme->setProperty("bg", QColor("#101014"));
    m_theme->setProperty("cardBg", QColor("#16161C"));
    m_theme->setProperty("border", QColor("#2A2C30"));
    m_theme->setProperty("textPrimary", QColor("#F2F3F5"));
    m_theme->setProperty("textSecondary", QColor("#8E9095"));
    m_theme->setProperty("textMuted", QColor("#6E7278"));
    // 【1.0.0.55 / UI-AUDIT P1-3】视频/页面叠加文字用的亮灰，原先 5+2 处直接
    // 写死 #C9C9CF / #C0C0C6（非 token）。收敛为 textOnVideo，GATE-DS 比对。
    m_theme->setProperty("textOnVideo", QColor("#C9C9CF"));
    m_theme->setProperty("accent", QColor("#FE2C55"));
    m_theme->setProperty("accentDark", QColor("#D81E4A"));
    m_theme->setProperty("thumbBg", QColor("#26272B"));
    m_theme->setProperty("overlayDim", QColor("#66000000"));
    m_theme->setProperty("overlayStrong", QColor("#99000000"));
    m_theme->setProperty("overlayScrim", QColor("#CC000000"));
    m_theme->setProperty("surfaceSemi", QColor("#CC0F0F14"));
    m_theme->setProperty("toastBg", QColor("#E626272B"));
    bootLog("theme set");

    // 【1.0.0.49】封面/头像磁盘缓存启动期清扫（md5 jpg 只增不减的兜底，
    // 见 DouyinApi::sweepImageCaches 注释）。上限 600 文件 / 128MB —— 正常
    // 用量（每次 feed 刷 12 张、约 1MB）远摸不到，只为防长期呆跑堆盘。
    {
        const int swept = DouyinApi::sweepImageCaches(600, 128 * 1024 * 1024);
        bootLog(QString("imgcache sweep removed=%1").arg(swept));
    }

    createScene();
    bootLog("=== ApplicationUI ctor end ===");
}

ApplicationUI::~ApplicationUI()
{
}

void ApplicationUI::openVideo(const QString& bvid)
{
    // 由 QML 侧监听本信号并 push VideoPage
    emit videoRequested(bvid);
}

QString ApplicationUI::version() const
{
    // APP_VERSION_STRING 由 build.sh 从 bar-descriptor.xml 的
    // <versionNumber> + <buildId> 拼出来，形如 "1.0.0.104"。
    return QString::fromLatin1(APP_VERSION_STRING);
}

QString ApplicationUI::bootLogText() const
{
    QStringList dirs;
    dirs << QDir::tempPath() << QDir::homePath();
    for (int i = 0; i < dirs.size(); ++i) {
        if (dirs.at(i).isEmpty()) continue;
        QFile f(QDir(dirs.at(i)).filePath("douyin_boot.log"));
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QString all = QString::fromUtf8(f.readAll());
            f.close();
            // 只保留最后 30 行，避免撑爆界面
            QStringList lines = all.split('\n', QString::SkipEmptyParts);
            if (lines.size() > 30) lines = lines.mid(lines.size() - 30);
            return lines.join("\n");
        }
    }
    return QString::fromUtf8("(无启动日志)");
}

void ApplicationUI::swipeLog(const QString& tag)
{
    // 与 bootLog 一样双写 tmp/ + home/，但独立文件，便于按时间序列分析手势。
    const QString line = QDateTime::currentDateTime().toString("hh:mm:ss.zzz") + "  " + tag;
    QStringList dirs;
    dirs << QDir::tempPath() << QDir::homePath();
    for (int i = 0; i < dirs.size(); ++i) {
        if (dirs.at(i).isEmpty()) continue;
        QFile f(QDir(dirs.at(i)).filePath("douyin_swipe.log"));
        if (f.open(QIODevice::Append | QIODevice::Text)) {
            QTextStream ts(&f);
            ts << line << "\n";
            f.flush();
            f.close();
        }
    }
}

void ApplicationUI::scrollFeedTo(int idx)
{
    swipeLog(QString("C++ scrollFeedTo ENTER idx=%1").arg(idx));
    AbstractPane* pane = Application::instance()->scene();
    if (!pane) {
        swipeLog("C++ scrollFeedTo FAIL no scene");
        return;
    }
    ListView* lv = pane->findChild<ListView*>(QLatin1String("dyFeedList"));
    if (!lv) {
        swipeLog("C++ scrollFeedTo FAIL no ListView(dyFeedList)");
        return;
    }
    QVariantList path;
    path << idx;
    lv->scrollToItem(path, ScrollAnimation::Smooth);
    swipeLog("C++ scrollFeedTo OK Smooth");
}

QString ApplicationUI::qmlLogText() const
{
    if (g_qmlLog.isEmpty()) {
        return QString::fromUtf8("(未捕获到 QML 错误)");
    }
    return g_qmlLog;
}

QString ApplicationUI::playerLogText() const
{
    if (m_playerLogPath.isEmpty()) return QString::fromUtf8("(无日志路径)");
    QFile f(m_playerLogPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString::fromUtf8("(打不开 bbplayer.log)");
    }
    QByteArray all = f.readAll();
    f.close();
    if (all.isEmpty()) return QString::fromUtf8("(bbplayer.log 为空)");
    // 只取尾部，避免文件很大时卡。放宽到 32KB：QML 报错会把日志刷得很大，
    // 真正的库错误往往被推到很后面（v47 就是因为只取 4KB + 尾部 8 行，被 ReferenceError 淹了）。
    if (all.size() > 32768) all = all.right(32768);
    QString s = QString::fromUtf8(all);
    QStringList lines = s.split('\n', QString::SkipEmptyParts);

    // v48：先剔除 QML 噪声，否则成片的
    //   asset:///VideoPage.qml:189: ReferenceError: Can't find variable: ui
    // 会把 libbbplayer / FFmpeg 的真实输出彻底盖住。
    QStringList clean;
    for (int i = 0; i < lines.size(); ++i) {
        QString l = lines.at(i).trimmed();
        if (l.isEmpty()) continue;
        if (l.contains("asset:///") || l.contains("ReferenceError")
            || l.contains("TypeError") || l.contains("is not a type")
            || l.contains("Cannot assign to non-existent property")
            || l.contains("Image loading aborted") || l.contains("Unsupported scheme")
            || l.startsWith("qml:") || l.startsWith("BiliClient")) {
            continue;
        }
        clean << l;
    }
    if (clean.isEmpty()) return QString::fromUtf8("(无库日志)");

    // 优先保留含错误/播放关键字的行（libbbplayer 与 FFmpeg 的标记），否则退回尾部几行。
    QStringList keep;
    for (int i = 0; i < clean.size(); ++i) {
        const QString& l = clean.at(i);
        if (l.contains("ERROR") || l.contains("error") || l.contains("fail")
            || l.contains("mux") || l.contains("open") || l.contains("http")
            || l.contains("scheme") || l.contains("codec") || l.contains("demux")
            || l.contains("av_") || l.contains("segment")) {
            keep << l;
        }
    }
    if (keep.isEmpty()) {
        int from = clean.size() - 5; if (from < 0) from = 0;
        for (int i = from; i < clean.size(); ++i) keep << clean.at(i);
    }
    // 最多 4 行，避免撑爆叠加层
    if (keep.size() > 4) keep = keep.mid(keep.size() - 4);
    QString out = keep.join("\n");
    if (out.size() > 400) out = out.right(400);
    return out;
}

QString ApplicationUI::proxyEnvDiag() const
{
    const char* np = ::getenv("no_proxy");
    const char* hp = ::getenv("http_proxy");
    const char* ap = ::getenv("all_proxy");
    return QString("np=%1 hp=%2 ap=%3")
        .arg(np ? QString::fromLatin1(np) : QString("(null)"))
        .arg(hp ? QString::fromLatin1(hp) : QString("(null)"))
        .arg(ap ? QString::fromLatin1(ap) : QString("(null)"));
}

QString ApplicationUI::diagText() const
{
    QStringList out;
    out << "net: " + DouyinApi::instance()->diagResult();
    out << "biz: " + DouyinApi::instance()->bizDiag();
    if (m_noteModel)    out << "noteModel: " + m_noteModel->diagLast();
    if (m_noteStore)    out << "noteStore: " + m_noteStore->diagLast();
    if (m_collectStore) out << "collectStore: " + m_collectStore->diagLast();
    if (m_historyStore) out << "historyStore: " + m_historyStore->diagLast();
    if (m_followStore)  out << "followStore: " + m_followStore->diagLast();

    // 【v96】封面缓存实况。排查"封面只显示一部分"时，这一行最关键：
    //   files 少 + bad=0  → 根本没下下来（网络/队列问题）
    //   bad 不为 0        → 缓存里躺着一堆 HTML 劫持页，解码必然失败
    //   files 多但界面还空 → QML 绑定/刷新问题，不是下载问题
    {
        QDir imgDir(QDir::homePath() + "/images");
        int total = 0, bad = 0;
        qint64 bytes = 0;
        if (imgDir.exists()) {
            const QStringList fs = imgDir.entryList(QStringList() << "*.jpg", QDir::Files);
            total = fs.size();
            for (int i = 0; i < fs.size(); ++i) {
                const QString p = imgDir.absoluteFilePath(fs.at(i));
                bytes += QFileInfo(p).size();
                if (!DouyinApi::isImageFile(p)) bad++;
            }
        }
        out << QString("cover: %1 files, %2 bad, %3 KB")
                   .arg(total).arg(bad).arg(bytes / 1024);
    }
    return out.join("\n");
}

void ApplicationUI::createScene()
{
    QmlDocument* qml = QmlDocument::create("asset:///main.qml").parent(this);
    if (!qml) {
        bootLog("QmlDocument::create FAILED");
        qDebug() << "Douyin: QmlDocument::create failed";
        setFallbackScene("QML DOC LOAD FAILED: asset:///main.qml");
        return;
    }
    if (qml->hasErrors()) {
        QString detail;
        QList<QDeclarativeError> errs = qml->errors();
        qDebug() << "Douyin: QML errors:" << errs.size();
        for (int i = 0; i < errs.size(); ++i) {
            const QDeclarativeError& e = errs.at(i);
            qDebug() << "  " << e.url().toString() << "line" << e.line() << "col" << e.column()
                     << e.description();
            if (i < 5) {
                detail += QString::number(e.line()) + ":" + QString::number(e.column()) + " " +
                          e.description() + "\n";
            }
        }
        bootLog("QML PARSE ERROR: " + detail);
        setFallbackScene("QML PARSE ERROR (main.qml):\n" + detail);
        return;
    }
    qml->setContextProperty("theme", m_theme);
    qml->setContextProperty("noteModel", m_noteModel);
    qml->setContextProperty("searchModel", m_searchModel);
    qml->setContextProperty("noteStore", m_noteStore);
    qml->setContextProperty("collectStore", m_collectStore);
    qml->setContextProperty("historyStore", m_historyStore);
    qml->setContextProperty("followStore", m_followStore);
    qml->setContextProperty("localDb", m_localDb);
    qml->setContextProperty("videoStore", m_noteStore);
    qml->setContextProperty("videoPlayer", m_player);
    // 抖音视频流模型 + 播放详情 Store（QML 侧变量名 dyFeedModel / dyPlayerStore）。
    qml->setContextProperty("dyFeedModel", m_dyFeedModel);
    qml->setContextProperty("dyPlayerStore", m_dyPlayerStore);
    qml->setContextProperty("dyCommentModel", m_dyCommentModel);
    // 【1.0.0.24】菜单数据源：关注流 / 同城流 / 观看历史。
    qml->setContextProperty("dyFollowModel", m_dyFollowModel);
    qml->setContextProperty("dyCityModel", m_dyCityModel);
    qml->setContextProperty("dyHistoryModel", m_dyHistoryModel);
    // 【1.0.0.37】收藏 / 已下载（菜单两项 + 视频页两个按钮）。
    qml->setContextProperty("dyFavModel", m_dyFavModel);
    qml->setContextProperty("dyDownloadStore", m_dyDlStore);
    qml->setContextProperty("ui", m_ui);
    // 登录态/扫码状态全部是 Q_PROPERTY，QML 侧直接读属性绑定即可。
    // 不要把 BiliApi 的信号 connect 到 QML 函数上 —— Qt 4.8 上会静默不连接。
    qml->setContextProperty("biliApi", DouyinApi::instance());
    qml->setContextProperty("app", this);

    AbstractPane* root = qml->createRootObject<AbstractPane>();
    if (!root) {
        bootLog("createRootObject returned NULL");
        qDebug() << "Douyin: createRootObject returned NULL";
        setFallbackScene("QML ROOT CREATE FAILED (main.qml)");
        return;
    }
    qDebug() << "Douyin: root created OK, scene set";
    Application::instance()->setScene(root);
    bootLog("scene set OK");

    // 【1.0.0.13 修复】播放页没有 TitleBar，返回只剩系统边缘手势，那条路径不触发
    // QML 的 back 信号，所以必须在 C++ 侧把每个 NavigationPane 的
    // popTransitionEnded 连到 onPagePopped 停流。
    //
    // 旧写法只对 root 做 qobject_cast<NavigationPane*>，而 main.qml 的根节点是
    // TabbedPane（AbstractPane 的直接派生，不是 NavigationPane），cast 永远失败 →
    // onPagePopped 是彻底的死代码：真机 1.0.0.10~1.0.0.12 的所有 boot log 里
    // 从来没有 "nav popTransitionEnded connected"。dyNav / settingsNav 是 Tab
    // 的子节点，必须遍历 QObject 树才能拿到。
    QList<NavigationPane*> panes = root->findChildren<NavigationPane*>();
    // 【1.0.0.22】main.qml 根节点由 TabbedPane 改为 NavigationPane（objectName
    // "dyNav"），root 自身就是要连的导航栈。findChildren/findChild 都不含自身，
    // 所以先把 root 自身加进来，再保留子层 objectName 兜底。
    if (NavigationPane* rootNav = qobject_cast<NavigationPane*>(root)) {
        if (!panes.contains(rootNav)) panes.append(rootNav);
    }
    if (NavigationPane* dyNav = root->findChild<NavigationPane*>(QLatin1String("dyNav"))) {
        if (!panes.contains(dyNav)) panes.append(dyNav);
    }
    for (int i = 0; i < panes.size(); ++i) {
        QObject::connect(panes.at(i), SIGNAL(popTransitionEnded(bb::cascades::Page*)),
                         this, SLOT(onPagePopped(bb::cascades::Page*)));
    }
    bootLog(QString("nav popTransitionEnded connected count=%1").arg(panes.size()));
    if (panes.isEmpty()) {
        bootLog("WARN no NavigationPane found - system back gesture will NOT stop stream");
    }

    // 应用退出时清理资源：停止播放器和流式代理，避免 curl 线程 /
    // MediaPlayer / QTcpServer 残留导致进程无法正常结束，下次启动失败。
    QObject::connect(QCoreApplication::instance(), SIGNAL(aboutToQuit()),
                     this, SLOT(cleanup()));

    // 网络层延迟到场景就绪后启动：先让首帧渲染出来，再起 TLS 线程。
    // 这样即使网络层有问题，应用也能正常启动并显示错误，而不是"点图标没反应"。
    QTimer::singleShot(0, this, SLOT(startNetworkDeferred()));
}

void ApplicationUI::startNetworkDeferred()
{
    bootLog("startNetwork begin");
    DouyinApi* api = DouyinApi::instance();
    api->startNetwork();
    bootLog("startNetwork done -> " + api->diagResult());

    // 【登录层自检直写启动日志】没有真实用户 Cookie 时也要能自证解析逻辑不是空桩：
    // selfParserCheck 用「实测匿名响应」+「构造成功响应」跑固定输入，结论写进 boot log；
    // cookieDiag 证明持久化目录确实可写。设备取回日志即可核对。
    bootLog("selfParserCheck -> " + api->selfParserCheck());
    bootLog("cookieDiag -> " + api->cookieDiag());
    // 冷启动时有保存的凭证必须重新校验：loadCookie() 只置「待校验」，
    // 旧实现直接 m_loggedIn=true 会把过期 Cookie 当成已登录。
    if (!api->cookie().isEmpty()) {
        bootLog("saved cookie present -> refreshUser");
        api->refreshUser();
    }

    // 【抖音 Tab 首屏数据】TabbedPane 会延迟创建未激活的 Tab 内容，而 QML 侧
    // 只写了 model: dyFeedModel，从没有任何地方调用 load() —— 实测 1.0.0.6
    // 设备日志里完全没有 dyrcmd:first 请求，抖音 Tab 永远空白。
    // 场景已经 set 完（本函数由 singleShot(0) 触发），这里同步发起首次拉取。
    if (m_dyFeedModel) {
        bootLog("dyFeedModel.load() begin");
        m_dyFeedModel->load();
        bootLog("dyFeedModel.load() issued");
    }

    // 【1.0.0.15 封面自检】冷启动 25 秒后自动跑一次：feed 行 → file:// →
    // 真实 JPEG 字节 → Cascades ImageTracker 解码。写进 boot log / bbplayer.log，
    // 用设备自己的框架解码器证明"主页确实有图可显示"，而不是靠 code=0 猜。
    QTimer::singleShot(25000, this, SLOT(onCoverSelfTest()));

    // 【1.0.0.15 首屏自动播放】feed 出行后置位 autoPlayTrigger，QML 侧推入播放页。
    // 8 秒起试（feed 在本机实测 1~3 秒返回，留足 TLS 余量），没出行每 3 秒再试。
    m_autoPlayAttempts = 0;
    QTimer::singleShot(8000, this, SLOT(onAutoPlayTick()));

    // 【无人值守播放自检】flag 由 blackberry-deploy -putFile 放进沙箱，这里一次性消费掉。
    // 放在 load() 之后：自检自己的 1 秒定时器会等首行数据到位再开跑。
    {
        const QString f1 = QDir(QDir::tempPath()).filePath(QLatin1String(kSelfTestFlag));
        const QString f2 = QDir(QDir::homePath()).filePath(QLatin1String(kSelfTestFlag));
        const QString flag = QFile::exists(f1) ? f1 : (QFile::exists(f2) ? f2 : QString());
        if (!flag.isEmpty()) {
            QFile::remove(flag);
            bootLog("playback selftest flag present -> auto run");
            playbackSelfTest();
        }
    }

    // 【1.0.0.14 导航栈自检】设备没有可用的触摸注入 / 截屏通道，连上
    // popTransitionEnded 之后也要能证明信号真的会触发。这里用一个 flag 让 C++
    // 在 18 秒后往 dyNav push 一个标记页、1.5 秒后 pop，全程写 boot log。
    // 与播放自检错开（播放自检 14 秒出结论），两者可以同时注入。
    {
        const QString f1 = QDir(QDir::tempPath()).filePath(QLatin1String(kNavSelfTestFlag));
        const QString f2 = QDir(QDir::homePath()).filePath(QLatin1String(kNavSelfTestFlag));
        const QString flag = QFile::exists(f1) ? f1 : (QFile::exists(f2) ? f2 : QString());
        if (!flag.isEmpty()) {
            QFile::remove(flag);
            bootLog("nav selftest flag present -> schedule push+pop at t+18s");
            QTimer::singleShot(18000, this, SLOT(onNavSelfTestPush()));
        }
    }

    // 【1.0.0.33 测试命令通道】1 秒轮询沙箱 dy_test_cmd.txt。为什么常驻而不是
    // 一次性 flag：四大功能（去登录/设置/刷新/发表）需要按顺序逐条触发、每条
    // 之间要留出网络/构建时间，flag 一次性模型做不到"运行中随时注入"。
    // 空转成本 = 每秒两次 QFile::exists，可忽略；命令到达即自删，重启不重放。
    m_testCmdTimer = new QTimer(this);
    m_testCmdTimer->setInterval(1000);
    QObject::connect(m_testCmdTimer, SIGNAL(timeout()),
                     this, SLOT(onTestCmdTick()));
    m_testCmdTimer->start();
    bootLog("test cmd channel armed (poll dy_test_cmd.txt 1s)");

    // 【1.0.0.50】解码停滞看护（零依赖自愈网）。
    // 背景：1.0.0.49/50 曾疑"熄屏后 FWC 输出面冻结"，试过 screen API
    // 查 SCREEN_PROPERTY_POWER_MODE 跳变触发 replay —— 实测
    // screen_get_context_property_iv 在 Cascades app 进程内 SIGSEGV
    // （设备 bbplayer.log：fltno=11 ip=…@screen_get_context_property_iv+0x3），
    // 整方案已撤，勿再引回。
    // 1.0.0.51 定性修正：熄屏/循环"有声无画"的真根因是 replay() 走
    // stop()→bbplayer_close() 销毁屏幕窗口、FWC 不重扫（见 video_store.cpp
    // replay() 长注释），position 在冻结期照走 —— 所以本看护对那个缺陷
    // 无效，它只兜"解码链真卡死"（rawPosition 15s 不推进 = 缓冲/网络死）。
    // 判据：每 3s 比对播放器 rawPosition（真实解码推进值），playing 态
    // 连续 5 tick（15s）不推进 → replay() 原地重开媒体源（不拆窗）。
    m_stallWatchTimer = new QTimer(this);
    m_stallWatchTimer->setInterval(3000);
    QObject::connect(m_stallWatchTimer, SIGNAL(timeout()),
                     this, SLOT(onStallWatchTick()));
    m_stallWatchTimer->start();
    m_lastRawPos = -1;
    m_stallTicks = 0;
    bootLog("stall watch armed (rawPosition 3s poll, 15s no-progress kick)");
}

// ---------------------------------------------------------------------------
// 【1.0.0.33】测试命令通道（无 root 触注入环境的功能验证桥）
//
// 背景：BB10 的 screen 输入注入（SCREEN_INPUT_PROVIDER_CONTEXT）与抓帧
// （SCREEN_DISPLAY_MANAGER_CONTEXT）都要 root 特权，devuser 一律 EPERM
// （真机实证：errno=1 Operation not permitted）。设备当前只剩 devuser SSH
// （root authorized_keys 丢失，/etc/ssh/authorized_keys2 里虽有 key 但运行中
// sshd 不读该文件——AllowUsers=root 的配置与 devuser 能登入矛盾，实际是另一套
// 配置在服务）。因此"点按钮"改由命令文件驱动：与按钮 onClicked 同一函数体
// （refactor 抽出，非替身），命令逐条落 boot log 可核对。
// ---------------------------------------------------------------------------
void ApplicationUI::onTestCmdTick()
{
    const QString f1 = QDir(QDir::tempPath()).filePath(QLatin1String(kTestCmdFile));
    const QString f2 = QDir(QDir::homePath()).filePath(QLatin1String(kTestCmdFile));
    const QString path = QFile::exists(f1) ? f1 : (QFile::exists(f2) ? f2 : QString());
    if (path.isEmpty()) {
        return;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        bootLog("TESTCMD recv open-fail path=" + path);
        return;
    }
    const QString content = QString::fromUtf8(f.readAll());
    f.close();
    // 一次性消费：先删再派发，避免读失败/崩在派发中途时重启重放。
    QFile::remove(path);
    const QStringList lines = content.split('\n');
    int n = 0;
    for (int i = 0; i < lines.size(); ++i) {
        const QString cmd = lines.at(i).trimmed();
        if (cmd.isEmpty()) {
            continue;
        }
        ++n;
        bootLog("TESTCMD recv " + cmd);
        pushTestCmd(cmd);
    }
    if (n == 0) {
        bootLog("TESTCMD recv empty-file path=" + path);
    }
}

void ApplicationUI::onStallWatchTick()
{
    // 【1.0.0.50/51】解码停滞看护：playing 态下 rawPosition 连续 15s 不
    // 推进 = 解码链真卡死（缓冲死/网络死）→ replay() 原地重开媒体源。
    // 注意边界：循环播放"有声无画"的缺陷 position 照走（真机 ENDED
    // pos=38000 40s/轮），本看护探不到它 —— 那个缺陷已由 1.0.0.51 的
    // "replay 不拆窗"修复（见 video_store.cpp replay() 长注释）。
    // 判据选择理由：
    //   - rawPosition 是播放器真实解码推进值（position 受 seek/UI 层加工），
    //     只有解码器真的吐帧才前进；
    //   - 比对 pos > 0 && pos == 基线：pos == 0 视为"刚起播还没出数"，
    //     不计数，等它先动起来；
    //   - 只在 playing 且 state 前缀为 "play" 时判定：暂停/stopped/error
    //     态 rawPosition 本就不该动，误 kick 会打断用户操作；
    //   - kick 后重置基线并放行计数：replay() 触发 onPoll 复位，
    //     下一 tick 拿到的是新链路的 rawPosition，不会连续误 kick。
    if (!m_player) {
        return;
    }
    const QString st = m_player->stateText();
    if (!m_player->playing() || !st.startsWith("play")) {
        m_lastRawPos = -1;
        m_stallTicks = 0;
        return;   // 非播放态：重置基线，下次起播从头计时
    }
    const qint64 pos = m_player->rawPosition();
    if (pos > 0 && pos == m_lastRawPos) {
        if (++m_stallTicks >= 5) {
            bootLog(QString("STALL watch kick replay raw=%1 ticks=%2 state=%3")
                    .arg(pos).arg(m_stallTicks).arg(st));
            m_stallTicks = 0;
            m_lastRawPos = -1;
            // replay() 原地 openUrl+play 重开媒体源（不拆窗，FWC 认领不变）。
            m_player->replay();
        }
    } else {
        m_lastRawPos = pos;
        m_stallTicks = 0;
    }
}

void ApplicationUI::pushTestCmd(const QString& cmd)
{
    ++m_testCmdSeq;
    // 带自增序号：重复注入同一条命令也要能触发 NOTIFY（属性值不变则信号不发射，
    // QML onAppTestCmdChanged 不会跑——这是 Q_PROPERTY 通道的固有语义）。
    m_testCmd = QString::number(m_testCmdSeq) + "|" + cmd;
    emit testCmdChanged();
    bootLog("TESTCMD dispatch " + m_testCmd);
}

// ---------------------------------------------------------------------------
// 播放链路自检
//
// 【为什么要有这段】BB10 真机上没有可用的截屏 / 触摸注入通道，无法用「看界面」
// 来证明点开视频真的能播。这里改成在 C++ 侧主动跑一遍与用户点击完全相同的链路，
// 让每一跳都在 data/bbplayer.log 里留下事实：
//   VideoProxy 打印 listening / headers set / GET range / FIRST video bytes
//   VideoPlayer 打印 scheme 降级 / state=playing / libbbplayer 的 ERROR 原文
// 再用轮询读到的 state + position 是否推进作为最终判据。position 只在解码器真的
// 吐帧后才会前进，所以它比「openUrl 返回 true」强得多。
// ---------------------------------------------------------------------------
void ApplicationUI::selfTestTrace(const QString& s)
{
    m_selfTestLog += s + "\n";
    if (m_selfTestLog.size() > 8000) m_selfTestLog = m_selfTestLog.right(8000);
    bootLog("[playback-selftest] " + s);
    qDebug("DouyinBar selftest: %s", qPrintable(s));
}

QString ApplicationUI::playbackSelfTest()
{
    if (m_selfTestRunning) return QString::fromUtf8("已在运行");
    if (!m_dyFeedModel || !m_dyPlayerStore || !m_player)
        return QString::fromUtf8("组件未就绪");

    m_selfTestLog.clear();
    m_selfTestRunning = true;
    m_selfTestTick = 0;
    m_selfTestStartTick = -1;
    m_selfTestMaxRawPos = 0;
    m_selfTestSawError = false;
    if (!m_selfTestTimer) {
        m_selfTestTimer = new QTimer(this);
        m_selfTestTimer->setInterval(1000);
        QObject::connect(m_selfTestTimer, SIGNAL(timeout()),
                         this, SLOT(onPlaybackSelfTestTick()));
    }
    selfTestTrace("START feed row -> CDN -> VideoProxy -> libbbplayer");
    m_selfTestTimer->start();
    return QString::fromUtf8("已启动（约 14 秒出结论）");
}

void ApplicationUI::onPlaybackSelfTestTick()
{
    if (!m_selfTestRunning) {
        if (m_selfTestTimer) m_selfTestTimer->stop();
        return;
    }
    ++m_selfTestTick;

    // ---- 阶段 1：等首屏 feed 行，且必须带 playUrl ----
    if (m_selfTestStartTick < 0) {
        const int n = m_dyFeedModel ? m_dyFeedModel->count() : 0;
        if (n <= 0) {
            if (m_selfTestTick >= 45) {
                selfTestTrace("FAIL 45s 内 feed 没有返回任何行");
                m_selfTestRunning = false;
                m_selfTestTimer->stop();
            }
            return;
        }
        QVariantMap row;
        QString playUrl;
        for (int i = 0; i < n && i < 8; ++i) {
            const QVariantMap r = m_dyFeedModel->rowAt(i);
            const QString u = r.value("playUrl").toString();
            if (!u.isEmpty()) {
                row = r;
                playUrl = u;
                break;
            }
        }
        if (playUrl.isEmpty()) {
            selfTestTrace(QString("FAIL feed 有 %1 行但都没有 playUrl").arg(n));
            m_selfTestRunning = false;
            m_selfTestTimer->stop();
            return;
        }
        selfTestTrace(QString("row ok awemeId=%1 playUrlLen=%2 urlHead=%3 dur=%4s size=%5B")
                      .arg(row.value("awemeId").toString())
                      .arg(playUrl.size())
                      .arg(playUrl.left(64))
                      .arg(row.value("durationSec").toInt())
                      .arg(row.value("videoSize").toLongLong()));
        m_selfTestStartTick = m_selfTestTick;
        selfTestTrace("DouyinPlayerStore::load(row) issued");
        m_dyPlayerStore->load(row);
        return;
    }

    // ---- 阶段 2：采样播放器状态，让代理有时间把字节喂给 libbbplayer ----
    //
    // 【1.0.0.10 自检自身的坑】绝不能用 positionMs() 当判据：那是 v74 的墙钟兜底，
    // 解码器报错时它照样按真实时间往前走。上一版因此把
    //   rawPos=0 rawState=5 playerError="does not serve byte ranges (HTTP 200)"
    // 误判成 VERDICT PASS。现在只认 rawPosition（解码器真实推进）+ 错误标志
    // + proxyBytesSent()（代理真的写出过视频字节）三者同时成立。
    const int elapsed = m_selfTestTick - m_selfTestStartTick;
    if (elapsed == 3 || elapsed == 7 || elapsed == 11 || elapsed == 14) {
        if (m_player->rawPosition() > m_selfTestMaxRawPos)
            m_selfTestMaxRawPos = m_player->rawPosition();
        if (!m_player->playerError().isEmpty() || m_player->rawState() == 5)
            m_selfTestSawError = true;
        selfTestTrace(QString("t+%1s state=%2 playing=%3 posMs=%4 durMs=%5 rawPos=%6 rawState=%7 proxyBytes=%8 err=%9")
                      .arg(elapsed)
                      .arg(m_player->stateText())
                      .arg(m_player->playing() ? 1 : 0)
                      .arg(m_player->positionMs())
                      .arg(m_player->durationMs())
                      .arg(m_player->rawPosition())
                      .arg(m_player->rawState())
                      .arg(m_dyPlayerStore->proxyBytesSent())
                      .arg(m_player->playerError()));
    }
    if (elapsed >= 14) {
        const QString st = m_player->stateText();
        const qint64 proxyBytes = m_dyPlayerStore->proxyBytesSent();
        const bool bytesFlowed = (proxyBytes > 0);
        const bool decoded = (m_selfTestMaxRawPos > 0);
        const bool healthy = !m_selfTestSawError
                             && m_player->playerError().isEmpty()
                             && !st.startsWith(QLatin1String("error"))
                             && st != QLatin1String("open-fail")
                             && st != QLatin1String("play-fail")
                             && st != QLatin1String("player-unavailable")
                             && st != QLatin1String("idle");
        const QString verdict = (bytesFlowed && decoded && healthy) ? QString("PASS")
                                : (bytesFlowed ? QString("PARTIAL") : QString("FAIL"));
        selfTestTrace(QString("VERDICT %1 [state=%2 maxRawPos=%3 proxyBytes=%4 sawError=%5 err=%6]")
                      .arg(verdict)
                      .arg(st)
                      .arg(m_selfTestMaxRawPos)
                      .arg(proxyBytes)
                      .arg(m_selfTestSawError ? 1 : 0)
                      .arg(m_player->playerError()));
        // 清理：不把 CDN 流和解码器留在后台
        m_dyPlayerStore->stopStream();
        m_player->stop();
        selfTestTrace("cleanup proxy stopped + player stopped");
        m_selfTestRunning = false;
        m_selfTestTimer->stop();
    }
}

void ApplicationUI::onNavSelfTestPush()
{
    AbstractPane* root = Application::instance()->scene();
    // 根即 NavigationPane（1.0.0.22）时 findChild 不含自身，先试自身再兜底子层。
    NavigationPane* nav = root ? qobject_cast<NavigationPane*>(root) : 0;
    if (!nav) nav = root ? root->findChild<NavigationPane*>(QLatin1String("dyNav")) : 0;
    if (!nav) {
        bootLog("nav selftest FAIL dyNav not found");
        return;
    }
    // objectName 故意用 DouyinPlayerPage：这样 pop 时 onPagePopped 会走完整的
    // "停播放器 + 断两条流" 分支，能一次验证信号转发和业务清理两件事。
    Page* marker = new Page();
    marker->setObjectName(QLatin1String("DouyinPlayerPage"));
    nav->push(marker);
    bootLog("nav selftest marker pushed");
    // push 有转场动画，立刻 pop 可能被吞；等 1.5 秒再 pop。
    QTimer::singleShot(1500, this, SLOT(onNavSelfTestPop()));
}

void ApplicationUI::onNavSelfTestPop()
{
    AbstractPane* root = Application::instance()->scene();
    // 根即 NavigationPane（1.0.0.22）时 findChild 不含自身，先试自身再兜底子层。
    NavigationPane* nav = root ? qobject_cast<NavigationPane*>(root) : 0;
    if (!nav) nav = root ? root->findChild<NavigationPane*>(QLatin1String("dyNav")) : 0;
    if (!nav) {
        bootLog("nav selftest FAIL dyNav not found at pop");
        return;
    }
    bootLog("nav selftest pop requested");
    nav->pop();
}

void ApplicationUI::onPagePopped(bb::cascades::Page* page)
{
    // 只对播放页生效。QML 侧给 VideoPage / DouyinPlayerPage 的根节点写了
    // objectName，因为 C++ 拿不到 main.qml 里 createObject 出来的那个引用。
    // 系统返回手势（peek / 返回键）不会经过页面里的按钮，所以抖音流的
    // 断流清理必须在这里兜底，否则后台会继续拉 CDN。
    if (!page) {
        bootLog("nav page popped: (null)");
        return;
    }
    const QString name = page->objectName();
    // 【1.0.0.13】每个 pop 都留痕。以前只有"是播放页"才写日志，连接本身失效时
    // 完全看不出来；现在 boot log 里能直接证明 signal/slot 真的被触发过。
    bootLog("nav page popped: " + (name.isEmpty() ? QString("(unnamed)") : name));
    const bool isVideoPage = (name == QLatin1String("VideoPage") ||
                              name == QLatin1String("DouyinPlayerPage"));
    if (!isVideoPage) {
        return;
    }
    bootLog("video page popped -> stop playback: " + name);
    if (m_player)        m_player->stop();
    if (m_noteStore)     m_noteStore->stopStream();
    if (m_dyPlayerStore) m_dyPlayerStore->stopStream();
}

void ApplicationUI::cleanup()
{
    bootLog("cleanup: begin");
    if (m_player) {
        // 【1.0.0.52】退出时才真拆窗（close 销毁屏幕窗口 + 复位 initialized）。
        // stop() 已不再拆窗（窗口终身只建一次，换片靠 openUrl 复用）。
        m_player->releasePlayer();
        bootLog("cleanup: videoPlayer released");
    }
    if (m_noteStore) {
        m_noteStore->stopStream();
        bootLog("cleanup: noteStore stream stopped");
    }
    bootLog("cleanup: end");
}

// ---------------------------------------------------------------------------
// 【1.0.0.15】封面链路自检
//
// 事故复盘：1.0.0.14 真机上 6 张封面下载全部 code=0、缓存目录里文件都在，
// 但主页一张都不显示。取回文件才发现扩展名是 .jpg、内容却是 RIFF....WEBP。
// 也就是说「curl 成功 / 文件存在 / isUsableImage 放行」这三层证据全都靠不住，
// 必须再加一层：用 Cascades 自己的 ImageTracker（与 ImageView 同一个资源加载器）
// 去解一遍，width/height > 0 才算真的能显示。
// ---------------------------------------------------------------------------
void ApplicationUI::onCoverSelfTest()
{
    DouyinApi* api = DouyinApi::instance();
    // 【1.0.0.49】每轮自检重新计加宽次数（见 m_coverDecodeRetried 注释）。
    m_coverDecodeRetried = false;
    const int n = m_dyFeedModel ? m_dyFeedModel->count() : 0;
    const int check = qMin(n, 6);
    int jpegUrl = 0, ready = 0, missing = 0, jpeg = 0, webp = 0, other = 0;
    QStringList samples;

    for (int i = 0; i < check; ++i) {
        const QVariantMap row = m_dyFeedModel->rowAt(i);
        const QString url = row.value(QLatin1String("picUrl")).toString();

        QString pathPart = url;
        const int q = pathPart.indexOf(QLatin1Char('?'));
        if (q >= 0) pathPart.truncate(q);
        const QString lowerUrl = pathPart.toLower();
        if (lowerUrl.endsWith(QLatin1String(".jpeg")) || lowerUrl.endsWith(QLatin1String(".jpg")))
            ++jpegUrl;

        const QString local = api->coverUrlFor(url, api->coverVersion());
        if (local.isEmpty()) {
            ++missing;
            continue;
        }
        ++ready;
        QString path = local;
        if (path.startsWith(QLatin1String("file://"))) path.remove(0, 7);
        const QString fmt = DouyinApi::imageFormatOf(path);
        if (fmt == QLatin1String("JPEG")) ++jpeg;
        else if (fmt == QLatin1String("WEBP")) ++webp;
        else ++other;

        if (samples.size() < 3) {
            samples << QString("r%1=%2/%3B").arg(i).arg(fmt).arg(QFileInfo(path).size());
            // ImageTracker 用框架解码器当 oracle；4 秒后统一读 width/height。
            ImageTracker* tracker = new ImageTracker(QUrl(local), this);
            tracker->setObjectName(QString("coverTracker%1").arg(i));
        }
    }

    const bool pass = (n > 0 && ready == check && jpeg == check && webp == 0
                       && other == 0 && missing == 0 && jpegUrl == check);
    const QString verdict = QString("cover selftest VERDICT %1 rows=%2 checked=%3 "
                                    "ready=%4 missing=%5 jpeg=%6 webp=%7 other=%8 "
                                    "jpegUrl=%9 samples=[%10]")
            .arg(pass ? QLatin1String("PASS") : QLatin1String("FAIL"))
            .arg(n).arg(check).arg(ready).arg(missing).arg(jpeg).arg(webp)
            .arg(other).arg(jpegUrl).arg(samples.join(QLatin1String(" ")));
    bootLog(verdict);
    qDebug("DouyinBar %s", qPrintable(verdict));

    QTimer::singleShot(4000, this, SLOT(onCoverDecodeReport()));
}

void ApplicationUI::onCoverDecodeReport()
{
    const QList<ImageTracker*> trackers = findChildren<ImageTracker*>();
    int decoded = 0;
    QString details;
    for (int i = 0; i < trackers.size(); ++i) {
        ImageTracker* t = trackers.at(i);
        const int w = t->width();
        const int h = t->height();
        if (w > 0 && h > 0) ++decoded;
        if (details.size() < 240)
            details += QString(" %1:%2x%3").arg(t->objectName()).arg(w).arg(h);
    }
    const bool pass = !trackers.isEmpty() && decoded == trackers.size();
    // 【1.0.0.49】加宽窗口再判一次：4s 内 0/3 曾出现两次（14:11/17:38 真机），
    // 而 cover selftest 同刻 PASS（文件在、真 JPEG）——异步资源加载在负载下
    // 没跑完，不是 App 缺陷。8s 后再采样一次，仍不过才记 FAIL。
    if (!pass && !m_coverDecodeRetried) {
        m_coverDecodeRetried = true;
        bootLog("cover decode RETRY (4s window exhausted, re-sample in 8s)");
        QTimer::singleShot(8000, this, SLOT(onCoverDecodeReport()));
        return;
    }
    const QString verdict = QString("cover decode VERDICT %1 cascadesDecoded=%2/%3 [%4]")
            .arg(pass ? QLatin1String("PASS") : QLatin1String("FAIL"))
            .arg(decoded).arg(trackers.size()).arg(details.trimmed());
    bootLog(verdict);
    qDebug("DouyinBar %s", qPrintable(verdict));
    // 只做一次采样，别把 ImageTracker 常驻在对象树上。
    for (int i = 0; i < trackers.size(); ++i) trackers.at(i)->deleteLater();
}

void ApplicationUI::onAutoPlayTick()
{
    ++m_autoPlayAttempts;
    DouyinApi* api = DouyinApi::instance();
    const int n = m_dyFeedModel ? m_dyFeedModel->count() : 0;
    if (n > 0) {
        bootLog(QString("autoPlay trigger -> feed rows=%1 attempt=%2").arg(n).arg(m_autoPlayAttempts));
        api->fireAutoPlay();     // QML 绑定重算后由 maybeAutoPlay() 推入播放页
        return;
    }
    if (m_autoPlayAttempts >= 6) {
        bootLog(QString("autoPlay trigger ABORT feed empty after 6 attempts "
                        "diag=%1 biz=%2")
                    .arg(api->diagResult()).arg(api->bizDiag()));
        return;
    }
    QTimer::singleShot(3000, this, SLOT(onAutoPlayTick()));
}

void ApplicationUI::setFallbackScene(const QString& message)
{
    // 兜底页：任何 QML 失败都不再黑屏，直接显示错误信息
    Page* page = new Page();
    Container* root = new Container();
    root->setHorizontalAlignment(HorizontalAlignment::Fill);
    root->setVerticalAlignment(VerticalAlignment::Fill);
    root->setTopPadding(32);
    root->setLeftPadding(16);
    root->setRightPadding(16);
    Label* label = new Label();
    label->setText(message);
    label->setMultiline(true);
    label->textStyle()->setColor(Color::fromRGBA(0xF2, 0xF3, 0xF5, 0xFF));
    root->add(label);
    page->setContent(root);
    Application::instance()->setScene(page);
}
