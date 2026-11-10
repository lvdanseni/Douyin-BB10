#ifndef BILI_API_HPP
#define BILI_API_HPP

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QThread>
#include <QHash>

class CurlWorker;
class CurlDownloadWorker;
class QTimer;

/**
 * bilibili Web API 客户端（对应 BBTube 的 InnerTube 客户端层）：
 * 统一 UA/Referer/Cookie、WBI 签名（nav 取密钥 → mixinKey → md5 w_rid）。
 *
 * 网络层说明：BB10 的 QtNetwork(QSslSocket) HTTPS 握手会永久挂起（设备浏览器经
 * WebKit 自己的网络栈可正常访问，QtNetwork 的 https 重定向后无响应），因此所有
 * 请求改为调用系统 curl（/usr/bin/curl，curl+OpenSSL，与 BBTube 的 libbbnet 同源
 * 技术栈，已在 BB10 上证明可用）。curl 输出（stdout JSON）经 QProcess 异步回传，
 * receiver 的槽签名为 slot(const QString& json, bool ok)。
 *
 * 【登录态给 QML 的方式】只用 Q_PROPERTY 绑定，绝不 emit 信号让 QML 去 .connect()。
 * BB10 / Qt 4.8 上「C++ 信号 → QML JS 处理函数」的 .connect() 会静默不连接（v29 实测），
 * 而属性绑定的 NOTIFY 走 QML 引擎内部通道，可靠。
 */
class BiliApi : public QObject {
    Q_OBJECT

    // ---- 登录态（QML 侧读这些属性即可，不要 connect）----
    Q_PROPERTY(bool loggedIn READ loggedIn NOTIFY userChanged)
    Q_PROPERTY(QString userName READ userName NOTIFY userChanged)
    Q_PROPERTY(QString userFace READ userFace NOTIFY userChanged)
    Q_PROPERTY(QString userMid READ userMid NOTIFY userChanged)
    /** 0=空闲 1=等待扫码 2=已扫码待确认 3=登录成功 4=二维码失效 5=出错 */
    Q_PROPERTY(int loginState READ loginState NOTIFY loginChanged)
    /** 给用户看的一行提示（已本地化，可直接显示） */
    Q_PROPERTY(QString loginTip READ loginTip NOTIFY loginChanged)
    /** 二维码图片的 file:// URL；未生成时为空串 */
    Q_PROPERTY(QString qrImageUrl READ qrImageUrl NOTIFY loginChanged)

    /**
     * 【v102】封面就绪版本号 —— 让 QML 的封面绑定能"被通知"重算。
     *
     * 为什么需要它：BB10 上「模型行数据变了 → ListView 重绘」这条路不可靠
     * （replace() 只发 itemChanged，实测封面永远不出现；removeAt+insert 也未必）。
     * 而本工程里**已经验证可靠**的通道是 Q_PROPERTY + NOTIFY ——
     * 播放页 UP 主头像（videoStore.detail.ownerFace）就是这么显示的，一直正常。
     *
     * 所以封面改成和头像同一套机制：模型里只放**远端 URL（永不变化）**，
     * QML 用 `biliApi.coverUrlFor(url, biliApi.coverVersion)` 去换本地路径。
     * 绑定表达式里读了 coverVersion，所以每张封面下完时 version++ 就能让
     * 所有封面绑定重算 —— 不依赖任何模型刷新。
     */
    Q_PROPERTY(int coverVersion READ coverVersion NOTIFY coverVersionChanged)

public:
    int coverVersion() const { return m_coverVersion; }
    static BiliApi* instance();
    static void create(QObject* parent);

    /**
     * 【v102】把远端封面 URL 换成本地 file:// 路径；还没下好返回空串。
     *
     * ⚠️ 必须是 public（Q_INVOKABLE 落在 private 区时 QML 调不到，静默失败）。
     *
     * @param raw 远端 URL（可带 @672w_378h_1c.jpg 后缀，coverUrl 会归一化）
     * @param ver **必须传 biliApi.coverVersion** —— 这个参数本身没用到，
     *            它的唯一作用是把 coverVersion 拉进 QML 的依赖图，
     *            否则绑定不会重算（QML 只跟踪属性读取，不跟踪函数返回值）。
     */
    Q_INVOKABLE QString coverUrlFor(const QString& raw, int ver);

    /**
     * 【v103】把远端**头像** URL 换成本地 file:// 路径；还没下好返回空串。
     *
     * 和 coverUrlFor 是同一套机制、同一个就绪表（m_coverReady 按本地路径登记，
     * BiliApi::onDownloadFinished 对所有进队列的下载都会登记 + bump 版本号），
     * 唯一的区别是路径算法用 faceCachePath 而不是 coverCachePath。
     *
     * 为什么需要它：评论列表原来在 onFaceDownloaded 里走
     * `m_commentModel->replace(row, item)` 回填头像 —— 和 v102 封面那次是同一个坑，
     * BB10 上「模型行变化 → ListView 重绘」不可靠，首次进视频页评论头像永远不出现。
     * 改成模型里只存远端 URL + 这里现场解析，靠 coverVersion 通知重算。
     *
     * @param ver 必须传 biliApi.coverVersion（作用同 coverUrlFor）
     */
    Q_INVOKABLE QString faceUrlFor(const QString& raw, int ver);

    /** 发起 GET（经系统 curl），完成时发出 netReply(tag, json, ok)，接收方用命名槽连接。
     *  ua 为空时用默认 BILI_UA；个别接口（评论 reply/main）必须用标准浏览器 UA，
     *  否则 bilibili 风控返回 -352（实测：BB10 UA → -352，Chrome UA → 正常返回评论）。 */
    void get(const QString& url, const QString& tag, const QString& ua = QString());

    /**
     * 【v85】POST 表单请求（application/x-www-form-urlencoded）。
     * body 由调用方拼好并 percentEncode。用于点赞/投币/收藏/关注这些写操作。
     */
    void post(const QString& url, const QString& body, const QString& tag);

    /** 【v85】cookie 里的 csrf（= bili_jct）。写操作必须带，缺了会 -111。 */
    Q_INVOKABLE QString csrf() const;

    /**
     * 下载二进制文件（带 UA/Referer/Cookie），完成时 emit downloadFinished(tag, path, ok)。
     * 用于把需要 Referer/UA 的 bilibili MP4 直链先落到本地沙盒，
     * 再交给 bb::multimedia::MediaPlayer 播放（MediaPlayer 无法自定义请求头，
     * 直连 CDN 会 403 —— 实测无 UA/Referer 时 upos 返回 403）。
     *
     * rangeStart/rangeEnd 用于分段并行下载（-1 表示整段）。bilibili 的 CDN 单连接
     * 往往被限速，切成几段并发抓能明显缩短等待。
     *
     * 【v84】queued=true 表示走"封面下载队列"：最多同时 3 个。
     * 列表一次要 20~40 张封面，全并发在 BB10 上握不完 TLS，会整页无图。
     */
    void download(const QString& url, const QString& destPath, const QString& tag,
                  qint64 expectedBytes, qint64 rangeStart = -1, qint64 rangeEnd = -1,
                  bool queued = false, int prio = 0);

    /** 指定 tag 的下载进度（0-100）。供 UI 定时轮询，避免跨线程信号。 */
    Q_INVOKABLE int downloadPercent(const QString& tag) const;

    /** 指定 tag 已写入的字节数（分段下载时由 VideoStore 汇总计算总进度与速度）。 */
    qint64 downloadWritten(const QString& tag) const;

    /** 对给定路径+参数做 WBI 签名后发起 GET（密钥未就绪时 fallback 为无签名请求）。 */
    void getSigned(const QString& path, const QVariantMap& params, const QString& tag);

    // ---- 登录（扫码）----
    /** 开始扫码登录：取二维码 → 落盘 PNG → 每 2 秒轮询一次结果。可重复调用（会重置）。 */
    Q_INVOKABLE void loginStart();
    /** 中止轮询（离开设置页时调用）。不影响已建立的登录态。 */
    Q_INVOKABLE void loginCancel();
    /** 退出登录：清内存 cookie、删盘上 cookie 文件、清用户信息。 */
    Q_INVOKABLE void logout();
    /** 当前 cookie 串（诊断用）。 */
    Q_INVOKABLE QString cookie() const { return m_cookie; }

    bool loggedIn() const { return m_loggedIn; }
    QString userName() const { return m_userName; }
    QString userFace() const { return m_userFace; }
    QString userMid() const { return m_userMid; }
    int loginState() const { return m_loginState; }
    QString loginTip() const { return m_loginTip; }
    QString qrImageUrl() const { return m_qrUrl; }

    // ---- 工具函数（QML 也可用） ----
    Q_INVOKABLE static QString percentEncode(const QString& s);
    Q_INVOKABLE static QString countText(qint64 n);
    Q_INVOKABLE static QString durationText(long long sec);
    Q_INVOKABLE static QString timeAgo(long long unix);

    /**
     * 【v93】封面 URL 规范化：砍掉已有的 @尺寸后缀，再统一加上 672w_378h_1c。
     *
     * 为什么要统一：列表页按这个后缀请求（体积降到 1/10），而下载 sidecar 里
     * 存的是接口返回的原始 URL。两边各自算缓存文件名 → 同一张封面被存成两份，
     * 而且"已下载"列表查的那个名字永远不存在，列表里全是灰块。
     *
     * 幂等：传原始 URL 或已带后缀的 URL，结果都一样。
     */
    static QString coverUrl(const QString& raw);

    /**
     * 【v93】封面缓存文件的绝对路径（顺带确保 images/ 目录存在）。
     *
     * 全工程**只有这一处**能算封面缓存路径。FeedModel（列表）、VideoStore
     * （下载时补封面）、DownloadStore（已下载列表）三处以前各写一遍，
     * 已经因为 60 字符截断不一致出过一次"已下载没封面"的 bug。
     *
     * 【v101】文件名改成 URL 的 md5（照 BBTube 的 advimages 做法）。
     * 原来的"替换非法字符 + 截断 60 字符"有两个隐患：`@` 没被替换（URL 带
     * `@672w_378h_1c.jpg` 后缀，短路径下会留在文件名里），以及长 host 会把
     * hash 也截掉一截、极端情况下两张封面算出同一个名字互相覆盖。
     * md5 定长 32 字符、纯十六进制、零特殊字符，两个问题一起消失。
     */
    static QString coverCachePath(const QString& raw);

    /**
     * 【v103】头像缓存文件的绝对路径。
     *
     * 原来只在 VideoStore 里有一份（VideoStore::faceCachePath），现在挪到这里，
     * 因为 faceUrlFor() 在 QML 绑定里被高频调用，必须和封面走同一个函数体、
     * 同一套目录缓存，不能再各写一份 —— 两处算法一旦不一致，就是"头像永远空白"。
     *
     * 和封面不同的两点（都是历史结论，别改）：
     *   1) **不用 md5**：头像 URL 里带 @120w_120h_1c.jpg 后缀，但前缀本身就是唯一的
     *      32 位 hex，直接换非法字符已经够用，没必要为此作废全部老缓存；
     *   2) **不截断**：截到 60 字符会把 hex 尾巴砍掉，两个用户撞同一个文件。
     */
    static QString faceCachePath(const QString& raw);

    /**
     * 【v96】只看文件头判断是不是图片（jpeg/png/webp/gif）。
     *
     * 运营商劫持、CDN 错误页、登录墙返回的 HTML 都远超 1KB。
     * 只按大小判断，这些垃圾会被当成有效缓存命中，ImageView 解码失败 ——
     * "文件明明在，封面就是不出来"，而且命中缓存后永远不会重下。
     */
    static bool isImageFile(const QString& path);

    /**
     * 【v100】判断缓存文件是不是一张**字节完整**的图。
     *
     * 只用字节判定（头 + 尾双标记），**绝不依赖图片解码器**。
     * v99 曾用 `QImageReader::canRead()` 做校验，结果设备上一张封面都不显示：
     * BB10 的 Qt 图片格式插件在 Cascades 应用里不一定被正确加载，
     * canRead() 可能对所有 jpeg 返回 false → 每张刚下完就被判成坏图删掉。
     *
     * 为什么还要校验：v98 及以前封面是直接写最终路径的，同一个视频出现在
     * 推荐/热门/排行里时两个 worker 会同时截断同一个文件、交错写入，
     * 留下"文件头合法、正文乱码"的 jpeg —— 它过得了 isImageFile，
     * 但 ImageView 解不出来，于是这张封面永久空白且永不重下。
     * 尾标记（JPEG 的 FFD9）能识破这种文件和被截断的文件。
     *
     * 【v103】加 minSize 参数：封面用默认 1024（一张 672x378 的封面不可能更小），
     * 但头像只有 120x120，极简图案可能压到 1KB 以下 —— 拿 1024 去卡它会导致
     * "判坏图 → 删掉 → 重下 → 再判坏图"的死循环，头像永远出不来。
     * 头像那边传 512。
     */
    static bool isUsableImage(const QString& path, qint64 minSize = 1024);

    /** 返回 curl HTTPS 自检结果（诊断用），如 "diag HTTPS OK / HTTP 307"。 */
    Q_INVOKABLE QString diagResult() const;

    /** 业务请求计数诊断："req=已发起 fin=已回调 last=最近一次 curl code/bytes"。 */
    Q_INVOKABLE QString bizDiag() const;

    /* ================= 小红书笔记 API 入口 ================= */

    /** API 基础主机。 */
    Q_INVOKABLE static QString noteApiBase();

    /** 笔记详情接口：/api/sns/v10/note/{note_id} */
    Q_INVOKABLE static QString noteDetailApi(const QString& noteId);

    /** 笔记推荐流：/api/sns/v10/recommend */
    Q_INVOKABLE static QString noteFeedApi();

    /** 笔记点赞：/api/sns/v10/like/{note_id} */
    Q_INVOKABLE static QString noteLikeApi(const QString& noteId);

    /** 笔记收藏：/api/sns/v10/collect/{note_id} */
    Q_INVOKABLE static QString noteCollectApi(const QString& noteId);

    /** 用户关注：/api/sns/v10/follow/{user_id} */
    Q_INVOKABLE static QString userFollowApi(const QString& userId);

    /** 关注动态流：/api/sns/v10/follow/feed */
    Q_INVOKABLE static QString followFeedApi();

    /** 观看历史：/api/sns/v10/history */
    Q_INVOKABLE static QString historyApi();

    /** 收藏夹笔记：/api/sns/v10/collection/notes */
    Q_INVOKABLE static QString collectNotesApi();

    /**
     * 启动网络层（curl 全局初始化 + 自检 + WBI 密钥预取）。
     * 必须由 ApplicationUI 在 setScene() 之后调用（经 QTimer::singleShot(0) 延迟），
     * 原因：BB10 启动看门狗对首帧渲染时间敏感，在构造期就起 TLS 握手线程会导致
     * 首帧延迟甚至被系统杀掉（表现为"点图标无法启动"）。
     */
    void startNetwork();

private:
    explicit BiliApi(QObject* parent = 0);
    Q_DISABLE_COPY(BiliApi)

    void request(const QString& url, const QString& tag, const QString& ua = QString(),
                 bool cookieJar = false);
    void launchCurl(const QString& url, const QString& tag, const QString& ua = QString(),
                    bool cookieJar = false);
    void onNavReply(const QString& json, bool ok);

    // 【v84】封面下载队列（限并发）
    struct PendingDl {
        QString url;
        QString path;
        QString tag;
        qint64 expected;
        qint64 rangeStart;
        qint64 rangeEnd;
        /** 【v90】优先级：0 = 视频封面（用户一眼能看到的），1 = 头像（锦上添花）。
         *  pumpDownloads 永远先派 0。不加这个的话，一页 20 个评论头像会把
         *  相关推荐的 8 张封面堵在后面几十秒 —— 表现就是"封面不显示"。 */
        int prio;
    };
    void startDownload(const QString& url, const QString& destPath, const QString& tag,
                       qint64 expectedBytes, qint64 rangeStart, qint64 rangeEnd, bool queued);
    void pumpDownloads();
    /**
     * 【v99】把一次下载结果分发给所有关心它的 tag。
     *
     * 从 onDownloadFinished 里抽出来的：一个目标路径可能被多个 FeedModel 同时
     * 请求（见 m_dlPathTags），完成时要把 downloadFinished 发给每一个，
     * 否则后请求的那个列表永远等不到回调 —— 表现为"切到热门页，封面全是灰块"。
     */
    void dispatchDownloadResult(const QString& tag, const QString& path, bool ok);

    // 登录内部流程
    void onLoginQrReply(const QString& json, bool ok);
    void onLoginPollReply(const QString& json, bool ok, const QString& cookies);
    void setLoginState(int st, const QString& tip);
    void stopPoll();
    void refreshUser();

    // cookie 持久化（沙盒根目录 bili_cookie.txt）
    void loadCookie();
    void saveCookie();
    void setCookie(const QString& c);

signals:
    /** curl 完成时发出，接收方（FeedModel/VideoStore）用命名槽连接。tag 区分请求类型。 */
    void netReply(const QString& tag, const QString& json, bool ok);
    /** nav 签名密钥获取失败时发出（保留，当前 getSigned 不再依赖 nav，仅在密钥获取异常时发出）。 */
    void navFailed();
    /** 文件下载完成（path 为落盘路径，ok 表示 curl 是否成功）。 */
    void downloadFinished(const QString& tag, const QString& path, bool ok);
    /** 用户信息（登录态/昵称/头像）变化。QML 通过 Q_PROPERTY 消费，不要 connect。 */
    void userChanged();
    /** 扫码状态变化。QML 通过 Q_PROPERTY 消费，不要 connect。 */
    void loginChanged();
    /** 【v102】有封面下载完成（已校验通过），封面绑定需要重算。 */
    void coverVersionChanged();

private slots:
    void onCurlFinished();
    void onDiagFinished();
    void onDownloadFinished();
    void onLoginPollTick();
    /** 【v94】封面队列看门狗，见 onDlWatchdog 的实现注释。 */
    void onDlWatchdog();

private:
    /**
     * 【v98】当前真正在跑的封面下载数。
     *
     * 直接数 m_dlJobs 里 queued 的 worker，**不再依赖 m_dlActive 计数器**。
     * 计数器只要漏减一次（worker 的 finished 没触发、sender() 转换失败、
     * 线程卡在内核态），队列就永久停摆 —— 表现正是"封面只出来前几张，
     * 后面的再也不下"，而且重启才恢复。
     * 数一遍 map 是 O(几十)，每次派发算一次完全不心疼。
     */
    int activeCoverJobs() const;

    QString m_imgKey;    QString m_subKey;
    bool m_keysReady;
    bool m_networkStarted;
    QString m_diagResult;
    // 双通道自检（HTTPS + HTTP 明文对照，区分 SSL 栈/网络层问题）
    QHash<CurlWorker*, int> m_diagJobs;
    int m_diagHttpsCode;
    QString m_diagHttpsOut;
    int m_diagHttpCode;
    QString m_diagHttpOut;
    // 业务请求计数（诊断：区分"curl 挂起" vs "回调链失效"）
    int m_bizReqCount;
    int m_bizFinCount;
    QString m_lastBiz;
    // 文件下载（tag -> worker），进度靠轮询 worker 已写入字节数，不依赖 curl 进度回调
    QHash<QString, CurlDownloadWorker*> m_dlJobs;
    QHash<QString, qint64> m_dlTotal;
    /**
     * 【v99】目标路径 → 关心这个文件的所有 tag（在跑的那个排第一，其余是等着的）。
     *
     * 这是照 BBTube 的 AdvancedImageView 抄的一条规则：**one fetch per address**。
     *
     * 为什么必须做：封面缓存路径是由 URL 决定的（BiliApi::coverCachePath），
     * 于是"同一个视频出现在推荐和热门里"= 同一个目标路径。以前 m_dlJobs 只按
     * tag 建索引，两个 FeedModel 用不同 tag 请求同一路径 → 两个 worker 各自
     * fopen(path,"wb") → 互相截断 + 交错写入 → 文件头合法、正文乱码。
     * ImageView 解码失败 → 封面空白；而 isImageFile 认这个文件头，
     * 于是它被当成有效缓存永远命中，重启也不恢复。
     *
     * 现在同一路径只跑一个 worker，其余 tag 挂在这张表里，完成时统一回调。
     */
    QHash<QString, QStringList> m_dlPathTags;
    /**
     * 【v102】已就绪的封面：本地绝对路径 → "file://…"。
     *
     * coverUrlFor() 每次绑定重算都会查它（纯哈希查找，零文件 IO）。
     * 下载完成且校验通过时由 onDownloadFinished 写入。
     */
    QHash<QString, QString> m_coverReady;
    /** 【v102】封面就绪版本号，见 Q_PROPERTY coverVersion。 */
    int m_coverVersion;
    /**
     * 【v103】就绪表查询 + 兜底现场校验，coverUrlFor / faceUrlFor 共用。
     *
     * 两者只差"本地路径怎么算"，查询逻辑必须共用 —— 一份代码两处抄，
     * 迟早有一处忘了校验、或忘了写进就绪表，表现就是"封面/头像时好时坏"。
     */
    QString readyImageFor(const QString& local, qint64 minSize);
    /** 【v84】封面下载队列与在跑数量（限并发，防止 BB10 上 TLS 握手挤爆）。 */
    QList<PendingDl> m_dlQueue;
    /**
     * 【v96】3 → 5。
     *
     * 3 并发在 BB10 上一次 TLS 握手 3~6 秒，一页 20 张封面要跑 7 轮 ≈ 半分钟，
     * 用户滑到第二屏时上半屏还有一堆灰块 —— 这就是"封面只显示一部分"。
     * v84 的教训是**别一次开 20~40 个**（BB10 握不完，全部 CONNECTTIMEOUT），
     * 但 5 个远在安全范围内。配合封面 20 秒超时 + 失败重试一次，
     * 既不会卡队列，又能把首屏铺满的速度提高近一倍。
     */
    static const int kMaxCoverConcurrent = 5;
    /** 【v94】上一次有封面下载完成的时刻（ms）。看门狗用它判断队列是不是卡死了。 */
    qint64 m_dlStallMs;
    QTimer* m_dlWatchdog;
    /** 【v96】封面下载成功/失败计数，只用于诊断输出（bizDiag）。 */
    int m_imgOk;
    int m_imgFail;

    // ---- cookie / 登录态 ----
    QString m_cookie;          // "SESSDATA=..; bili_jct=..; DedeUserID=.."
    bool m_loggedIn;
    QString m_userName;
    QString m_userFace;        // 已是 file:// 本地路径（Cascades ImageView 不能直连 https）
    QString m_userFaceRemote;  // 远端原始 URL，用于判断要不要重新下
    /** 【v87】https 失败后是否已经回退试过 http（只回退一次，避免来回打转）。 */
    bool m_userFaceAltTried;
    QString m_userMid;
    // 扫码
    int m_loginState;
    QString m_loginTip;
    QString m_qrKey;           // qrcode_key，轮询用
    QString m_qrUrl;           // 本地 PNG 的 file:// URL
    QString m_qrPath;          // 本地 PNG 绝对路径（换码时要删旧文件）
    bool m_pollBusy;           // 一次轮询未回来前不叠加下一次
    QTimer* m_pollTimer;       // 扫码轮询定时器（2s），惰性创建
};

#endif // BILI_API_HPP
