#ifndef DOUYIN_API_HPP
#define DOUYIN_API_HPP

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <QThread>
#include <QHash>
#include <QStringList>

class DyCurlWorker;
class DyCurlDownloadWorker;
class QTimer;

/**
 * 【1.0.0.30】网络层日志：与 ApplicationUI 的 bootLog 同一格式、同两个目录
 *（应用 tmp/ + 沙箱 data/，双写防崩溃清空），但允许网络层直接调用。
 * 用途：把每次 curl 结束的 code/http/bytes 写进 douyin_boot.log ——
 * 1.0.0.28/.29 真机连续空 feed 时只有请求行没有回流行，无法区分
 *「设备断网」「服务端限流」「解析空」，这是让不可见变可见的关键一行。
 */
void dyNetLog(const QString& s);

/**
 * 抖音 Web API 客户端（对应 BiliBar 的 BiliApi）：
 * 统一 UA/Referer/Cookie、抖音签名（_signature / X-Bogus / msToken 等，需按真实抓包填入）。
 *
 * 网络层说明：与 BiliBar  identical —— BB10 的 QtNetwork(QSslSocket) HTTPS 握手会永久挂起，
 * 因此所有请求改为调用系统 curl（/usr/bin/curl，curl+OpenSSL），通过 QProcess/QThread
 * 异步回传 JSON。receiver 的槽签名为 slot(const QString& json, bool ok)。
 *
 * 【登录态给 QML 的方式】只用 Q_PROPERTY 绑定，绝不 emit 信号让 QML 去 .connect()。
 * BB10 / Qt 4.8 上「C++ 信号 → QML JS 处理函数」的 .connect() 会静默不连接，
 * 而属性绑定的 NOTIFY 走 QML 引擎内部通道，可靠。
 *
 * =========================== ⚠️ 签名算法（必须填入真实值） ===========================
 * 抖音当前的登录/视频流接口需要签名参数（常见为 _signature、X-Bogus、msToken、as/cp 等）。
 * 本文件只预留了 signRequest() 插槽，返回带签名的完整 URL。**需要你在真机/模拟器上抓包
 * 抖音 App，把签名生成逻辑（JS 或加密算法）用 C++/OpenSSL/QCryptographicHash 复现，
 * 填进 signRequest() 内部。** 签名算法不正确时，所有接口会返回 403/登录失败/视频流为空。
 *
 * 建议抓包工具：mitmproxy / Charles / Fiddler，抓抖音极速版/抖音 App 的登录、feed、
 * iteminfo 接口，记录请求头（User-Agent、Referer、X-Bogus、_signature 等）与 Query 参数。
 * =================================================================================
 */
class DouyinApi : public QObject {
    Q_OBJECT

    // ---- 登录态（QML 侧读这些属性即可，不要 connect）----
    Q_PROPERTY(bool loggedIn READ loggedIn NOTIFY userChanged)
    Q_PROPERTY(QString userName READ userName NOTIFY userChanged)
    Q_PROPERTY(QString userFace READ userFace NOTIFY userChanged)
    Q_PROPERTY(QString userMid READ userMid NOTIFY userChanged)
    /** 0=空闲 1=等待扫码 2=已扫码待确认 3=登录成功 4=二维码失效 5=出错 */
    Q_PROPERTY(int loginState READ loginState NOTIFY loginChanged)
    /** 给用户看的一行提示（可直接显示） */
    Q_PROPERTY(QString loginTip READ loginTip NOTIFY loginChanged)
    /** 二维码图片的 file:// URL；未生成时为空串 */
    Q_PROPERTY(QString qrImageUrl READ qrImageUrl NOTIFY loginChanged)

    /** 【v102】封面就绪版本号，让 QML 的封面绑定能重算。 */
    Q_PROPERTY(int coverVersion READ coverVersion NOTIFY coverVersionChanged)

    /**
     * 【1.0.0.15】首屏自动播放触发器。
     * feed 就绪后由 ApplicationUI 置位；QML 侧 `biliApi.autoPlayTrigger`
     * 绑定重算 → maybeAutoPlay() 推入播放页。走 Q_PROPERTY+NOTIFY（铁律 4），
     * 不用 C++ 信号 .connect()。
     */
    Q_PROPERTY(bool autoPlayTrigger READ autoPlayTrigger NOTIFY autoPlayTriggerChanged)

public:
    int coverVersion() const { return m_coverVersion; }
    bool autoPlayTrigger() const { return m_autoPlayTrigger; }
    /** feed 就绪后由 ApplicationUI 调用：置位 + NOTIFY（QML 才能重算绑定）。 */
    void fireAutoPlay();
    static DouyinApi* instance();
    static void create(QObject* parent);

    /** 把远端封面 URL 换成本地 file:// 路径；还没下好返回空串。ver 必须传 coverVersion。 */
    Q_INVOKABLE QString coverUrlFor(const QString& raw, int ver);

    /** 把远端头像 URL 换成本地 file:// 路径；还没下好返回空串。ver 必须传 coverVersion。 */
    Q_INVOKABLE QString faceUrlFor(const QString& raw, int ver);

    /** 发起 GET（经系统 curl），完成时发出 netReply(tag, json, ok)。 */
    void get(const QString& url, const QString& tag, const QString& ua = QString());

    /** POST 表单请求（application/x-www-form-urlencoded），用于写操作。 */
    void post(const QString& url, const QString& body, const QString& tag);

    /**
     * 【1.0.0.29】POST JSON 请求（Content-Type: application/json）。
     * 评论发表 /aweme/v1/comment/publish/ 只认 JSON body（curl 实证：form 编码
     * 返回 "Url does not match"，JSON 到达鉴权层返 status_code=8 用户未登录）。
     * 完成时同样广播 netReply(tag, json, ok)。
     */
    void postJson(const QString& url, const QString& json, const QString& tag);

    /** 抖音写操作的 csrf/验证字段（cookie 里的对应值）。 */
    Q_INVOKABLE QString csrf() const { return m_csrf; }

    /** 下载二进制文件（带 UA/Referer/Cookie），完成时 emit downloadFinished(tag, path, ok)。 */
    void download(const QString& url, const QString& destPath, const QString& tag,
                  qint64 expectedBytes, qint64 rangeStart = -1, qint64 rangeEnd = -1,
                  bool queued = false, int prio = 0);

    /** 指定 tag 的下载进度（0-100）。供 UI 定时轮询。 */
    Q_INVOKABLE int downloadPercent(const QString& tag) const;
    qint64 downloadWritten(const QString& tag) const;

    /**
     * 对给定路径+参数做抖音签名后发起 GET（签名未就绪时 fallback 为无签名请求，会 403）。
     * @param path API 路径（如 /aweme/v2/feed/）
     * @param params 已 percentEncode 的查询参数
     * @param tag 请求标签
     */
    void getSigned(const QString& path, const QVariantMap& params, const QString& tag);

    // ---- 登录（Cookie 粘贴）----
    /**
     * 【抖音登录实证结论】扫码登录在本网络实测 error_code 16（接口已废弃/需签名），
     * 不可用。可行路径是 Cookie 登录：用户从浏览器/手机导出抖音 Cookie 粘贴进来，
     * 客户端用 /aweme/v1/user/profile/self/ 校验，status_code=0 且 user 非空才算成功。
     *
     * raw 支持两种形态：
     *   - 完整 Cookie 串："sessionid=xxx; sid_tt=xxx; csrf_session_id=xxx; ..."
     *   - 裸 sessionid 值：只给 32 位十六进制串时自动补 "sessionid="
     */
    Q_INVOKABLE void loginWithCookie(const QString& raw);
    /** 用当前已保存的 cookie 重新校验登录态（冷启动时调用）。 */
    Q_INVOKABLE void refreshUser();
    /** 兼容旧入口：指向 Cookie 登录说明，不再触发扫码。 */
    Q_INVOKABLE void loginStart();
    /** 中止轮询（离开登录页时调用）。不影响已建立的登录态。 */
    Q_INVOKABLE void loginCancel();
    /** 退出登录：清内存 cookie、删盘上 cookie 文件、清用户信息。 */
    Q_INVOKABLE void logout();
    /** 当前 cookie 串（诊断用）。 */
    Q_INVOKABLE QString cookie() const { return m_cookie; }
    /** cookie 持久化通道自检：路径、是否存在、可写性、长度、上次校验结果。 */
    Q_INVOKABLE QString cookieDiag() const;
    /**
     * 登录态 JSON 解析器不变量自检：用「真机抓包的匿名响应」+「构造的成功响应」
     * 两段固定输入跑一遍 parseSelfProfile，返回可打印的结论串。
     * 设备日志里能直接看到，用于自证解析逻辑正确（不是空桩）。
     */
    Q_INVOKABLE QString selfParserCheck() const;

    bool loggedIn() const { return m_loggedIn; }
    QString userName() const { return m_userName; }
    QString userFace() const { return m_userFace; }
    QString userMid() const { return m_userMid; }
    int loginState() const { return m_loginState; }
    QString loginTip() const { return m_loginTip; }
    QString qrImageUrl() const { return m_qrUrl; }

    // ---- 工具函数（QML 也可用）----
    Q_INVOKABLE static QString percentEncode(const QString& s);
    Q_INVOKABLE static QString countText(qint64 n);
    Q_INVOKABLE static QString durationText(long long sec);
    Q_INVOKABLE static QString timeAgo(long long unix);

    /** 封面 URL 规范化：砍掉已有尺寸后缀，统一加上抖音常用缩略图后缀。 */
    static QString coverUrl(const QString& raw);

    /** 封面缓存文件的绝对路径（md5(url)+".jpg"）。 */
    static QString coverCachePath(const QString& raw);
    /**
     * 【1.0.0.49】封面/头像磁盘缓存清扫（只增不减缺陷的修复）。
     * md5 命名的 jpg 随每次 feed 刷新累积（12 张/次，无回收路径），
     * 长期运行可堆到几十 MB。启动期跑一次（非热路径）：超过上限时按
     * mtime 删最旧的。上限宽松（正常用量远摸不到），只为兜底防呆。
     * @return 实际删除的文件数。
     */
    Q_INVOKABLE static int sweepImageCaches(int maxFiles, qint64 maxBytes);

    /** 头像缓存文件的绝对路径。 */
    static QString faceCachePath(const QString& raw);

    /**
     * 【1.0.0.15 真机实证】从抖音的 url_list 里挑一个"真 JPEG"地址。
     *
     * feed 里 cover.url_list 的前 3 个都是 .webp，第 4 个才是 .jpeg：
     *   .../oQptAx...AA.webp?lk3s=...&x-signature=...
     *   .../oQptAx...AA.jpeg?lk3s=...&x-signature=...
     * 旧代码无脑取 [0]，下载下来的是 RIFF....WEBP，却按 md5+".jpg" 落盘。
     * Cascades 1.1 的 ImageView 对「.jpg 后缀 + WebP 内容」不认，主页整屏空白。
     * 这里只认 ? 之前的 path 以 .jpeg/.jpg 结尾的候选，并在并列时优先 https。
     * 没有 JPEG 候选就返回空串 —— 宁可不显示，也不要造一个骗过自己的坏文件。
     */
    static QString pickJpegUrl(const QVariantList& urls);

    /** cover → origin_cover → dynamic_cover 依次挑 JPEG；都不行返回空串。 */
    static QString pickCoverUrl(const QVariantMap& video);

    /** 落盘文件的真实格式："JPEG" / "PNG" / "WEBP" / "GIF" / "OTHER" / "MISSING"。 */
    static QString imageFormatOf(const QString& path);

    static bool isImageFile(const QString& path);
    /**
     * 图片可用性校验：头 + 尾双标记，且**内容必须与 .jpg 缓存扩展名一致**。
     * WebP/GIF 内容落在 .jpg 文件里一律判 false（v1.0.0.15 修复点）。
     */
    static bool isUsableImage(const QString& path, qint64 minSize = 1024);

    /** curl HTTPS 自检结果（诊断用）。 */
    Q_INVOKABLE QString diagResult() const;
    /** 业务请求计数诊断。 */
    Q_INVOKABLE QString bizDiag() const;

    /** 抖音 API 基础主机。 */
    Q_INVOKABLE static QString apiBase();

    /** 首页推荐视频流（抖音 feed，返回 aweme_list）。 */
    Q_INVOKABLE static QString feedApi();
    /** 登录态校验：匿名/失效 cookie 返回 status_code=8 + user=null。 */
    Q_INVOKABLE static QString userProfileApi();
    /** 单视频详情（含播放地址 play_addr）。 */
    Q_INVOKABLE static QString itemInfoApi(const QString& awemeId);
    /** 用户发布视频列表。 */
    Q_INVOKABLE static QString postListApi(const QString& secUid);

    /**
     * 【1.0.0.23】评论列表完整 URL（curl 实证可用的免签 web 端点）。
     *
     * 主机是 www.iesdouyin.com（**不是** apiBase() 的 aweme.snssdk.com），
     * 所以这里直接返回完整 URL，调用方不要再拼 apiBase()。
     *   2026-11-06 实证：设备同款 App UA + Referer 返 200 JSON，comments[]
     *   字段 = cid / text / createTime(秒) / digg_count / reply_comment_total /
     *   ip_label / user.nickname / user.avatar_medium.url_list。
     *   两个坑：① status_code 是**对象** {StatusCode:0,...}，不能用
     *   root.status_code.toInt() 判成败（判空会恒得 0，掩耳盗铃）；② cursor
     *   参数被服务端忽略，每页固定返前 10 条 —— 翻页靠 DouyinCommentModel
     *   按 cid 客户端去重，拉不到新评论即置 hasMore=false。
     */
    Q_INVOKABLE static QString commentListUrl(const QString& awemeId, int cursor);

    /**
     * 【1.0.0.29】评论发表完整 URL（curl 实证的 JSON 端点）。
     * 2026-11-07 host 复核：POST https://www.iesdouyin.com/aweme/v1/comment/publish/
     * + Content-Type: application/json + body {"aweme_id":"...","text":"..."} →
     * HTTP 200 {"status_code":8,"status_msg":"用户未登录"}（匿名到达鉴权层；
     * 带合法 sessionid Cookie 应返 status_code=0 + comment 对象）。
     * 对照：/web/api/v2/comment/create/ → "Url doesn't match"（需签名）；
     * aweme.snssdk.com 的 /aweme/v1/comment/create/ → "Url does not match"（a_bogus）。
     */
    Q_INVOKABLE static QString commentPublishUrl();

    // ---- 与 note_store 兼容的端点别名（⚠️ 抖音真实路径待抓包确认）----
    Q_INVOKABLE static QString noteDetailApi(const QString& noteId);
    Q_INVOKABLE static QString noteLikeApi(const QString& noteId);
    Q_INVOKABLE static QString noteCollectApi(const QString& noteId);
    Q_INVOKABLE static QString userFollowApi(const QString& userId);
    Q_INVOKABLE static QString noteFeedApi();
    Q_INVOKABLE static QString collectNotesApi();
    Q_INVOKABLE static QString followFeedApi();
    Q_INVOKABLE static QString nearbyFeedApi();

    void startNetwork();

private:
    explicit DouyinApi(QObject* parent = 0);
    Q_DISABLE_COPY(DouyinApi)

    void request(const QString& url, const QString& tag, const QString& ua = QString(),
                 bool cookieJar = false);
    void launchCurl(const QString& url, const QString& tag, const QString& ua = QString(),
                    bool cookieJar = false);
    void onNavReply(const QString& json, bool ok);

    // ---- 抖音签名插槽（⚠️ 这里填入真实签名算法）----
    /**
     * 给请求 URL 加上抖音签名，返回签名后的完整 URL。
     * 若签名算法未实现，返回原 URL（接口会 403，供你验证"签名缺失"的现象）。
     */
    QString signRequest(const QString& url, const QVariantMap& params);

    // 【v84】封面下载队列（限并发）
    struct PendingDl {
        QString url;
        QString path;
        QString tag;
        qint64 expected;
        qint64 rangeStart;
        qint64 rangeEnd;
        int prio;
        int row;
        int retry;
        PendingDl() : expected(0), rangeStart(-1), rangeEnd(-1), prio(0), row(-1), retry(0) {}
    };
    void startDownload(const QString& url, const QString& destPath, const QString& tag,
                       qint64 expectedBytes, qint64 rangeStart, qint64 rangeEnd, bool queued);
    void pumpDownloads();
    void dispatchDownloadResult(const QString& tag, const QString& path, bool ok);

    // 登录内部流程
    void onLoginQrReply(const QString& json, bool ok);
    void onLoginPollReply(const QString& json, bool ok, const QString& cookies);
    void setLoginState(int st, const QString& tip);
    void stopPoll();

    /** 规范化粘贴进来的 Cookie 串：去 Cookie: 前缀/空白/换行，裸 sessionid 自动补全。 */
    static QString normalizeCookie(const QString& raw);
    /** 发 self 校验请求；userInitiated 决定失败时是否清空已保存 cookie。 */
    void verifyCookie(const QString& cookie, bool userInitiated);
    /**
     * 解析 /aweme/v1/user/profile/self/ 响应。
     * 返回 status_code；valid 仅在 status_code==0 且 user 非空时为真。
     */
    static int parseSelfProfile(const QString& json, bool* valid,
                                QString* nick, QString* uid, QString* face);

    // cookie 持久化（沙盒 data/douyin_cookie.txt，app/native 是安装镜像不保证可写）
    void loadCookie();
    void saveCookie();
    void setCookie(const QString& c);

signals:
    void netReply(const QString& tag, const QString& json, bool ok);
    void navFailed();
    void downloadFinished(const QString& tag, const QString& path, bool ok);
    void userChanged();
    void loginChanged();
    void coverVersionChanged();
    /** 【1.0.0.15】首屏自动播放触发器置位通知。 */
    void autoPlayTriggerChanged();

private slots:
    void onCurlFinished();
    void onDiagFinished();
    void onDownloadFinished();
    void onLoginPollTick();
    void onDlWatchdog();
    /** self 校验请求的独立回调：不经过 netReply 广播，避免污染各 Store 的 tag 路由。 */
    void onLoginVerifyFinished();

private:
    int activeCoverJobs() const;
    QString cookiePath() const;

    QString m_imgKey;    QString m_subKey;
    bool m_keysReady;
    bool m_networkStarted;
    QString m_diagResult;
    QHash<DyCurlWorker*, int> m_diagJobs;
    int m_diagHttpsCode;
    QString m_diagHttpsOut;
    int m_diagHttpCode;
    QString m_diagHttpOut;
    int m_bizReqCount;
    int m_bizFinCount;
    QString m_lastBiz;
    QHash<QString, DyCurlDownloadWorker*> m_dlJobs;
    QHash<QString, qint64> m_dlTotal;
    QHash<QString, QStringList> m_dlPathTags;
    QHash<QString, QString> m_coverReady;
    int m_coverVersion;
    /** 【1.0.0.15】首屏自动播放触发器（feed 就绪后由 ApplicationUI 置位）。 */
    bool m_autoPlayTrigger;
    QString readyImageFor(const QString& local, qint64 minSize);
    QList<PendingDl> m_dlQueue;
    static const int kMaxCoverConcurrent = 5;
    qint64 m_dlStallMs;
    QTimer* m_dlWatchdog;
    int m_imgOk;
    int m_imgFail;

    // ---- cookie / 登录态 ----
    QString m_cookie;          // "sessionid=..; csrftoken=..; ..."（抖音字段名待抓包确认）
    bool m_loggedIn;
    QString m_userName;
    QString m_userFace;        // 已是 file:// 本地路径
    QString m_userFaceRemote;  // 远端原始 URL
    bool m_userFaceAltTried;
    QString m_userMid;
    QString m_csrf;            // 抖音写操作验证字段（cookie 里的对应值）
    // 登录校验
    QHash<DyCurlWorker*, bool> m_loginJobs;   // worker -> userInitiated
    QString m_lastLoginVerify;              // 上次 self 校验结论（诊断用）
    // 扫码（已停用，保留成员避免大改）
    int m_loginState;
    QString m_loginTip;
    QString m_qrKey;           // 扫码凭证，轮询用
    QString m_qrUrl;           // 本地 PNG 的 file:// URL
    QString m_qrPath;          // 本地 PNG 绝对路径
    bool m_pollBusy;
    QTimer* m_pollTimer;       // 扫码轮询定时器（2s），惰性创建
};

#endif // DOUYIN_API_HPP
