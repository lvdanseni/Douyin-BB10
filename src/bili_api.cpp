#include "bili_api.hpp"

#include "qrcodegen.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QStringList>
#include <QTimer>
#include <cstdio>
#include <curl/curl.h>
#include <bb/data/JsonDataAccess>

using namespace bb::data;

static const int MIXIN_TAB[] = {
    46,47,18,2,53,8,23,32,15,50,10,31,58,3,45,35,27,43,5,49,33,9,42,19,29,28,14,39,12,38,41,13,37,48,7,16,24,
    55,40,61,26,17,0,1,60,51,30,4,22,25,54,21,56,59,6,63,57,62,11,36,20,34,44,52
};

static const char* BILI_UA =
    "Mozilla/5.0 (BB10; BiliClient) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/60.0 Safari/537.36";

/**
 * libcurl 后台工作线程（无 Q_OBJECT，免 moc）：在线程内执行 curl_easy_perform，
 * run() 返回后 finished() 信号跨线程回到主线程，由 BiliApi 读取结果并 emit netReply(tag, json, ok)。
 * tag 用于在多个并发请求的回调中区分调用者（FeedModel / VideoStore 按 tag 路由到各自槽）。
 *
 * 注意：诊断 tag 用 const char* 而非全局 QString——共享库里的全局 QString 需要
 * 静态构造器，在 BB10 加载期存在风险（且会被 -Os 优化成不可预期的形态）。
 */
static const char* const kDiagTag = "__diag__";
static const char* const kNavTag  = "__nav__";
static const char* const kLoginQrTag   = "__loginqr__";
static const char* const kLoginPollTag = "__loginpoll__";

class CurlWorker : public QThread {
public:
    /**
     * cookieJar=true 时开启 curl 自己的 cookie 引擎（CURLOPT_COOKIEFILE 传空串 = 只开引擎
     * 不读文件），请求结束后用 CURLINFO_COOKIELIST 把服务端 Set-Cookie 下来的 cookie
     * 原样 dump 出来。扫码登录只能这样拿 —— 从响应体的 data.url 里抠 SESSDATA 要猜
     * 它有没有被二次 URL 编码，猜错就是「登录成功但所有接口还是未登录」。
     */
    explicit CurlWorker(const QByteArray& url, bool isDiag, bool follow = true,
                        const QString& tag = QString(), const QByteArray& ua = QByteArray(),
                        const QByteArray& cookie = QByteArray(), bool cookieJar = false,
                        const QByteArray& body = QByteArray())
        : m_url(url), m_diag(isDiag), m_follow(follow), m_tag(tag), m_ua(ua),
          m_cookie(cookie), m_jar(cookieJar), m_body(body), m_code(-99) {}
    QString result() const { return m_result; }
    int code() const { return m_code; }
    QString tag() const { return m_tag; }
    /** 仅 cookieJar=true 时有效："name=value; name=value"。 */
    QString cookies() const { return m_cookies; }

protected:
    void run() {
        CURL* c = curl_easy_init();
        if (!c) { m_code = -100; return; }
        curl_easy_setopt(c, CURLOPT_URL, m_url.constData());
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, m_follow ? 1L : 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, m_diag ? 8L : 15L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, m_ua.isEmpty() ? BILI_UA : m_ua.constData());
        curl_easy_setopt(c, CURLOPT_REFERER, "https://www.bilibili.com/");
        curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
        if (!m_cookie.isEmpty()) {
            curl_easy_setopt(c, CURLOPT_COOKIE, m_cookie.constData());
        }
        if (m_jar) {
            curl_easy_setopt(c, CURLOPT_COOKIEFILE, "");  // 空串 = 只启用引擎
        }
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &m_result);
        // 【v85】POST：点赞/投币/收藏/关注都是 application/x-www-form-urlencoded 的 POST。
        // curl 不会复制 HTTPHEADER 里的字符串，所以 slist 必须活到 perform 之后。
        struct curl_slist* hdrs = 0;
        if (!m_body.isEmpty()) {
            curl_easy_setopt(c, CURLOPT_POST, 1L);
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, m_body.constData());
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)m_body.size());
            hdrs = curl_slist_append(hdrs, "Content-Type: application/x-www-form-urlencoded");
            curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
        }
        m_code = curl_easy_perform(c);
        if (hdrs) curl_slist_free_all(hdrs);
        if (m_jar) {
            dumpCookies(c);
        }
        curl_easy_cleanup(c);
    }

private:
    /** 把 curl 引擎里的 cookie 拼成 Cookie 头格式。 */
    void dumpCookies(CURL* c) {
        struct curl_slist* sl = 0;
        if (curl_easy_getinfo(c, CURLINFO_COOKIELIST, &sl) != CURLE_OK || !sl) return;
        QStringList parts;
        for (struct curl_slist* p = sl; p; p = p->next) {
            if (!p->data) continue;
            // Netscape 格式：domain \t includeSub \t path \t secure \t expiry \t name \t value
            // 域名可能带 "#HttpOnly_" 前缀，但我们只取后两列，不受影响。
            const QStringList f = QString::fromUtf8(p->data).split(QLatin1Char('\t'));
            if (f.size() < 7) continue;
            const QString name = f.at(5).trimmed();
            // 【坑】QStringList 没有重写 mid()：f.mid(6) 返回的是 QList<QString>，
            // 上面没有 join()。必须自己拼。
            QString val;
            for (int i = 6; i < f.size(); ++i) {
                if (i > 6) val += QLatin1Char('\t');
                val += f.at(i);
            }
            if (name.isEmpty() || val.isEmpty()) continue;
            parts << name + QLatin1Char('=') + val;
        }
        curl_slist_free_all(sl);
        m_cookies = parts.join(QString::fromLatin1("; "));
    }

    static size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
        QString* out = static_cast<QString*>(userdata);
        out->append(QString::fromUtf8(ptr, size * nmemb));
        return size * nmemb;
    }

    QByteArray m_url;
    bool m_diag;
    bool m_follow;
    QString m_tag;
    QByteArray m_ua;
    QByteArray m_cookie;
    bool m_jar;
    /** 【v85】非空则用 POST 发这个 body（application/x-www-form-urlencoded）。 */
    QByteArray m_body;
    QString m_result;
    QString m_cookies;
    int m_code;
};

/**
 * 二进制下载 worker：把 bilibili 的 MP4 直链落盘到应用沙盒。
 *
 * 为什么要先下载：bb::multimedia::MediaPlayer 走 QNX mmrenderer 的 http 客户端，
 * 无法设置 User-Agent / Referer，而 bilibili CDN 两个都要校验（实测缺 UA 直接 403）。
 * 用 libcurl（能带完整请求头）下载到本地，再把本地路径交给 MediaPlayer 播，是最稳的路径。
 *
 * 进度不用 curl 的进度回调（BB10 自带 curl 版本较老，CURLOPT_XFERINFOFUNCTION 可能不存在），
 * 而是在写回调里累加已写字节，由 BiliApi::downloadPercent() 轮询换算百分比。
 */
class CurlDownloadWorker : public QThread {
public:
    CurlDownloadWorker(const QByteArray& url, const QString& path, const QString& tag,
                       qint64 rangeStart = -1, qint64 rangeEnd = -1,
                       const QByteArray& cookie = QByteArray(), bool queued = false)
        : m_url(url), m_path(path), m_tag(tag), m_code(-99), m_written(0), m_file(0),
          m_rangeStart(rangeStart), m_rangeEnd(rangeEnd), m_cookie(cookie), m_queued(queued) {}
    QString tag() const { return m_tag; }
    QString path() const { return m_path; }
    int code() const { return m_code; }
    qint64 written() const { return m_written; }
    /** 【v84】是否由封面下载队列派发（决定完成时要不要归还并发额度）。 */
    bool queued() const { return m_queued; }

protected:
    void run() {
        // 【v99】原子落盘：先写 <path>.part，成功后才 rename 到最终路径。
        //
        // 原来是直接 fopen(m_path,"wb") 写最终路径。两个后果：
        //   1) 写的过程中（可能持续几秒）最终路径上躺着一个半截文件，
        //      任何在此时做缓存校验的地方都会把它当成"有效缓存"；
        //   2) 同一路径被并发请求时（推荐/热门/排行都有同一个视频），
        //      两个 FILE* 同时截断 + 交错写入 → 文件头合法、正文乱码，
        //      解码失败但文件头检查通过 → 这张封面永久空白。
        // rename 在 POSIX 上是原子的：最终路径要么不存在，要么是完整文件。
        const QString tmpPath = m_path + QLatin1String(".part");
        m_file = fopen(tmpPath.toUtf8().constData(), "wb");
        if (!m_file) { m_code = -201; return; }
        CURL* c = curl_easy_init();
        if (!c) { fclose(m_file); m_file = 0; QFile::remove(tmpPath); m_code = -100; return; }
        curl_easy_setopt(c, CURLOPT_URL, m_url.constData());
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
        // 【v93】超时按用途分开设。原来一律 600 秒，是"整页封面全空白"的元凶：
        //
        // 封面走 3 并发队列（见 kMaxCoverConcurrent）。只要 3 个连接同时卡在
        // "已建连但服务器不返回响应头"这种状态，LOW_SPEED 不会触发（压根没有
        // 数据在流），只能等 CURLOPT_TIMEOUT —— 600 秒。三个槽位被占满 10 分钟，
        // 队列里的其余几十张封面一张都派不出去，用户看到的就是整页灰块。
        //
        // 封面就是一张 672x378 的 jpeg，正常 1~3 秒，20 秒足够；
        // 视频要保持长超时（几十分钟的大文件很正常）。
        if (m_queued) {
            curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
        } else {
            curl_easy_setopt(c, CURLOPT_TIMEOUT, 600L);
        }
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, BILI_UA);
        curl_easy_setopt(c, CURLOPT_REFERER, "https://www.bilibili.com/");
        if (!m_cookie.isEmpty()) {
            curl_easy_setopt(c, CURLOPT_COOKIE, m_cookie.constData());
        }
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, this);
        // 分段：让 curl 自己发 Range 头。CDN 支持 206（实测 -r 0-2047 返回 206）。
        QByteArray range;
        if (m_rangeStart >= 0) {
            range = QString("%1-%2").arg(m_rangeStart)
                        .arg(m_rangeEnd >= 0 ? QString::number(m_rangeEnd) : QString()).toLatin1();
            curl_easy_setopt(c, CURLOPT_RANGE, range.constData());
        }
        m_code = curl_easy_perform(c);
        curl_easy_cleanup(c);
        fclose(m_file);
        m_file = 0;
        // 【v99】只有 curl 完全成功才把临时文件转正；失败一律删掉。
        // 失败时留着 .part 没意义（下次会重下），留着只会占空间。
        if (m_code == CURLE_OK) {
            QFile::remove(m_path);              // QFile::rename 不覆盖已存在的目标
            if (!QFile::rename(tmpPath, m_path)) {
                // 兜底：某些沙盒/文件系统策略下 rename 会失败。复制也失败才
                // 算这次下载失败 —— 不能因为 rename 不支持就让**所有**封面都出不来。
                QFile::remove(m_path);
                if (QFile::copy(tmpPath, m_path)) {
                    QFile::remove(tmpPath);
                } else {
                    m_code = -202;
                }
            }
        } else {
            QFile::remove(tmpPath);
        }
    }

private:
    static size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
        CurlDownloadWorker* self = static_cast<CurlDownloadWorker*>(userdata);
        if (!self->m_file) return 0;
        const size_t n = fwrite(ptr, 1, size * nmemb, self->m_file);
        self->m_written += (qint64)n;
        return n;
    }

    QByteArray m_url;
    QString m_path;
    QString m_tag;
    int m_code;
    volatile qint64 m_written;
    FILE* m_file;
    qint64 m_rangeStart;
    qint64 m_rangeEnd;
    QByteArray m_cookie;
    bool m_queued;
};



/** curl 错误码 → 简要英文（ASCII，避免设备缺中文字体乱码） */
static QString curlErrText(int c)
{
    switch (c) {
    case 0: return "OK";
    case 1: return "UNSUPPORTED_PROTO";
    case 6: return "NO_HOST";
    case 7: return "CONN_FAIL";
    case 18: return "PARTIAL_FILE";
    case 26: return "READ_ERR";
    case 28: return "TIMEOUT";
    case 33: return "RANGE_ERR";
    case 35: return "SSL_HANDSHAKE";
    case 38: return "SSL_CACERT?";
    case 51: return "PEER_CERT";
    case 53: return "SSL_ENGINE";
    case 56: return "RECV_ERR";
    case 60: return "CERT_PROBLEM";
    case 67: return "LOGIN_DENIED";
    case 77: return "CACERT_ACCESS";
    case 80: return "SSL_INIT";
    case 82: return "SSL_CERTPROBLEM";
    case -100: return "NO_CURL_INIT";
    default: return QString::number(c);
    }
}

BiliApi* BiliApi::instance()
{
    static BiliApi* s = 0;
    if (!s) {
        s = new BiliApi();
    }
    return s;
}

void BiliApi::create(QObject* parent)
{
    // 关键：必须复用 instance() 的同一个单例。
    // 若这里另用一个 static 指针，会出现"create 造一个、instance 造另一个"的双实例，
    // 导致 netReply 信号发在 A 实例上、接收方 connect 的是 B 实例 → 回调永远收不到。
    BiliApi* s = instance();
    if (parent && s->parent() != parent) {
        s->setParent(parent);
    }
}

BiliApi::BiliApi(QObject* parent)
    : QObject(parent)
    , m_keysReady(false)
    , m_networkStarted(false)
    , m_diagHttpsCode(-99)
    , m_diagHttpCode(-99)
    , m_bizReqCount(0)
    , m_bizFinCount(0)
    , m_coverVersion(0)     // 【v102】声明顺序：m_coverVersion 在 m_dlStallMs 之前
    , m_dlStallMs(0)
    , m_dlWatchdog(0)
    , m_imgOk(0)
    , m_imgFail(0)
    , m_loggedIn(false)
    , m_userFaceAltTried(false)
    , m_loginState(0)
    , m_pollBusy(false)
    , m_pollTimer(0)
{
    // 构造期刻意不做任何网络动作（不起线程、不初始化 curl）。
    // 原因：BB10 启动看门狗对首帧渲染敏感，构造期启动 TLS 握手线程会让首帧延迟，
    // 严重时应用直接被系统杀掉，表现为"点图标无法启动"。网络由 startNetwork() 触发。
    // 读 cookie 是纯本地文件操作，放在这里没问题 —— 而且必须早于第一次 nav 请求。
    m_diagResult = "diag:idle";
    loadCookie();
}

void BiliApi::startNetwork()
{
    if (m_networkStarted) return;
    m_networkStarted = true;

    // curl_global_init 非线程安全，必须在任何线程发起 curl_easy 调用前，
    // 在主线程一次性初始化（否则多个并发 worker 会竞态，导致部分请求永久卡住）。
    CURLcode ginit = curl_global_init(CURL_GLOBAL_ALL);
    if (ginit != CURLE_OK) {
        m_diagResult = QString("diag:curl_init_fail(%1)").arg((int)ginit);
        qWarning("BiliClient BiliApi::startNetwork curl_global_init failed %d", (int)ginit);
        emit navFailed();
        return;
    }

    // 网络层采用 libcurl（curl+OpenSSL，与 BBTube 的 libbbnet 同源，BB10 系统自带运行库）：
    // BB10 的 QtNetwork(QSslSocket) HTTPS 握手永久挂起，已弃用。
    // 启动时双通道自检：HTTPS（主路径）+ HTTP 明文对照（区分 SSL 栈 vs 网络层）。
    m_diagResult = "diag:run";
    CurlWorker* d1 = new CurlWorker("https://api.bilibili.com/x/web-interface/nav", true, true,
                                    QString::fromLatin1(kDiagTag));
    m_diagJobs.insert(d1, 1);
    QObject::connect(d1, SIGNAL(finished()), this, SLOT(onDiagFinished()));
    d1->start();
    CurlWorker* d2 = new CurlWorker("http://api.bilibili.com/x/web-interface/nav", true, false,
                                    QString::fromLatin1(kDiagTag));
    m_diagJobs.insert(d2, 2);
    QObject::connect(d2, SIGNAL(finished()), this, SLOT(onDiagFinished()));
    d2->start();

    // 预取 WBI 签名密钥（非阻塞）：密钥就绪后 getSigned 自动带 w_rid，未就绪则走 fallback。
    // 顺带拿登录用户信息 —— nav 的返回里同时有 wbi_img 和 uname/face/mid/isLogin，
    // 一次请求两用。cookie 已在构造函数里 loadCookie() 读回，这里直接带上。
    CurlWorker* navW = new CurlWorker("https://api.bilibili.com/x/web-interface/nav",
                                      false, true, QString::fromLatin1(kNavTag),
                                      QByteArray(), m_cookie.toUtf8());
    QObject::connect(navW, SIGNAL(finished()), this, SLOT(onCurlFinished()));
    navW->start();

    qDebug("BiliClient BiliApi::startNetwork done cookie=%d", m_cookie.size());
}

QString BiliApi::diagResult() const
{
    return m_diagResult;
}

QString BiliApi::bizDiag() const
{
    // img ok/fail 是封面队列的累计计数：ok 远大于 fail 说明网络没问题，
    // fail 占多数说明是超时/劫持/CDN 拒绝，能直接区分"没下完"和"下坏了"。
    return QString("req%1 fin%2 imgok%3 imgfail%4 last=%5")
            .arg(m_bizReqCount).arg(m_bizFinCount)
            .arg(m_imgOk).arg(m_imgFail).arg(m_lastBiz);
}

void BiliApi::onDiagFinished()
{
    CurlWorker* w = static_cast<CurlWorker*>(sender());
    if (!w) return;
    int tag = m_diagJobs.value(w, 0);
    m_diagJobs.remove(w);
    if (tag == 1) {
        m_diagHttpsCode = w->code();
        m_diagHttpsOut = w->result();
    } else if (tag == 2) {
        m_diagHttpCode = w->code();
        m_diagHttpOut = w->result();
    }
    w->deleteLater();

    if (m_diagJobs.isEmpty()) {
        QString https = (m_diagHttpsCode == CURLE_OK && !m_diagHttpsOut.trimmed().isEmpty())
                        ? "HTTPS OK"
                        : QString("HTTPS FAIL %1").arg(curlErrText(m_diagHttpsCode));
        // HTTP 明文：期望 307（bilibili 将明文重定向到 https）
        bool httpOk = (m_diagHttpCode == CURLE_OK && m_diagHttpOut.contains("30"));
        QString http = httpOk
                       ? QString("HTTP %1").arg(m_diagHttpOut.trimmed().left(8))
                       : QString("HTTP FAIL %1").arg(curlErrText(m_diagHttpCode));
        m_diagResult = "diag " + https + " / " + http;
        qDebug("BiliClient BiliApi::onDiagFinished -> %s", qPrintable(m_diagResult));
    }
}

void BiliApi::get(const QString& url, const QString& tag, const QString& ua)
{
    request(url, tag, ua);
}

/**
 * 【v85】POST 表单请求。点赞/投币/收藏/关注全走这里。
 *
 * 这几个接口都不需要 WBI 签名（只有 csrf 校验），所以不走 getSigned，
 * 直接 POST + cookie 即可。body 必须是已经 percentEncode 过的 key=value&...
 */
void BiliApi::post(const QString& url, const QString& body, const QString& tag)
{
    if (!m_networkStarted) {
        startNetwork();
    }
    qDebug("BiliClient BiliApi::post tag=%s body=%s", qPrintable(tag), qPrintable(body));
    m_bizReqCount++;
    CurlWorker* w = new CurlWorker(url.toUtf8(), false, true, tag, QByteArray(),
                                   m_cookie.toUtf8(), false, body.toUtf8());
    QObject::connect(w, SIGNAL(finished()), this, SLOT(onCurlFinished()));
    w->start();
}

/**
 * 【v85】从 cookie 里取 csrf（= bili_jct）。
 *
 * B 站所有写操作（点赞/投币/收藏/关注）都要带 csrf，值就是 cookie 里的 bili_jct。
 * 少了它一律返回 -111「csrf 校验失败」。
 */
QString BiliApi::csrf() const
{
    const QStringList parts = m_cookie.split(QString::fromLatin1("; "));
    for (int i = 0; i < parts.size(); ++i) {
        const QString& p = parts.at(i);
        if (p.startsWith(QString::fromLatin1("bili_jct="))) {
            return p.mid(9);
        }
    }
    return QString();
}

void BiliApi::launchCurl(const QString& url, const QString& tag, const QString& ua, bool cookieJar)
{
    // 兜底：QML 的 onCreationCompleted 可能在 startNetwork() 之前就调 load()，
    // 此时 curl_global_init 尚未执行，直接起 worker 会崩。这里惰性确保已初始化。
    if (!m_networkStarted) {
        startNetwork();
    }

    qDebug("BiliClient BiliApi::launchCurl tag=%s jar=%d %s", qPrintable(tag), cookieJar ? 1 : 0,
           qPrintable(url));
    m_bizReqCount++;
    // 所有业务请求都自动带上登录 cookie（未登录时为空串，curl 不会发 Cookie 头）。
    // 例外：cookieJar 请求（扫码轮询）本身不需要凭证，而且带上会和引擎收集到的
    // Set-Cookie 拼出两条重复的 SESSDATA，所以这类请求不发已有 cookie。
    const QByteArray ck = cookieJar ? QByteArray() : m_cookie.toUtf8();
    CurlWorker* w = new CurlWorker(url.toUtf8(), false, true, tag, ua.toUtf8(), ck, cookieJar);
    QObject::connect(w, SIGNAL(finished()), this, SLOT(onCurlFinished()));
    w->start();
}

void BiliApi::download(const QString& url, const QString& destPath, const QString& tag,
                       qint64 expectedBytes, qint64 rangeStart, qint64 rangeEnd, bool queued,
                       int prio)
{
    if (!m_networkStarted) {
        startNetwork();
    }

    // 【v84】封面走队列，限制同时进行的下载数。
    //
    // 为什么要排队：一次列表加载会一口气请求 20~40 张封面，每张都开一个
    // QThread + 一次完整 TLS 握手。BB10（2013 年的机器）根本握不完这么多并发
    // 连接，CONNECTTIMEOUT 10 秒一到全部失败 —— 表现就是"整页封面一个都不显示"。
    // 首页因为图早就缓存过所以看不出来，相关推荐每次都是新视频，必挂。
    //
    // 视频分段下载不走队列（只有 2~4 个），保持原来的即时启动。
    if (queued) {
        // 【v99】one fetch per path —— 照 BBTube AdvancedImageView 的规则。
        //
        // 缓存路径由 URL 决定，所以"同一个视频出现在推荐和热门里"就是同一个
        // 目标路径。以前这里无条件入队：两个 FeedModel 各带一个 tag 请求同一路径，
        // 队列里堆两条、并发槽位同时派出去 → 两个 worker 同时写同一个文件。
        // 就算加了原子落盘（.part + rename），重复下载也是纯浪费 —— BB10 上
        // 一次 TLS 握手 3~6 秒，省下的这一份能直接换成另一张封面早点出来。
        //
        // 已经在跑/在排队的路径：只把 tag 记进 m_dlPathTags，完成时统一回调。
        if (m_dlPathTags.contains(destPath)) {
            QStringList& tags = m_dlPathTags[destPath];
            if (!tags.contains(tag)) tags.append(tag);
            qDebug("BiliClient cover dedup path=%s tag=%s", qPrintable(destPath), qPrintable(tag));
            return;
        }
        m_dlPathTags.insert(destPath, QStringList() << tag);

        PendingDl p;
        p.url = url;
        p.path = destPath;
        p.tag = tag;
        p.expected = expectedBytes;
        p.rangeStart = rangeStart;
        p.rangeEnd = rangeEnd;
        p.prio = prio;
        m_dlQueue.append(p);
        pumpDownloads();
        return;
    }

    startDownload(url, destPath, tag, expectedBytes, rangeStart, rangeEnd, false);
}

/** 【v84】真正起线程下载。queued=true 表示由队列派发，完成时要归还并发额度。 */
void BiliApi::startDownload(const QString& url, const QString& destPath, const QString& tag,
                            qint64 expectedBytes, qint64 rangeStart, qint64 rangeEnd, bool queued)
{
    qDebug("BiliClient BiliApi::download tag=%s range=%lld-%lld -> %s", qPrintable(tag),
           (long long)rangeStart, (long long)rangeEnd, qPrintable(destPath));
    m_dlTotal.insert(tag, expectedBytes);
    CurlDownloadWorker* w = new CurlDownloadWorker(url.toUtf8(), destPath, tag, rangeStart, rangeEnd,
                                                   m_cookie.toUtf8(), queued);
    m_dlJobs.insert(tag, w);
    QObject::connect(w, SIGNAL(finished()), this, SLOT(onDownloadFinished()));
    w->start();
}

int BiliApi::activeCoverJobs() const
{
    int n = 0;
    for (QHash<QString, CurlDownloadWorker*>::const_iterator it = m_dlJobs.constBegin();
         it != m_dlJobs.constEnd(); ++it) {
        if (it.value() && it.value()->queued()) n++;
    }
    return n;
}

void BiliApi::pumpDownloads()
{
    // 【v94】第一次派封面时才开始看门狗。
    // 不在构造函数里建：构造期刻意零动作（见构造函数注释，BB10 启动看门狗
    // 对首帧渲染很敏感），而且没有下载任务时定时器也没意义。
    if (!m_dlWatchdog) {
        m_dlWatchdog = new QTimer(this);
        m_dlWatchdog->setInterval(15000);
        connect(m_dlWatchdog, SIGNAL(timeout()), this, SLOT(onDlWatchdog()));
        m_dlWatchdog->start();
    }
    if (m_dlStallMs <= 0) {
        m_dlStallMs = QDateTime::currentMSecsSinceEpoch();
    }

    // 【v90】优先派 prio=0 的（视频封面），头像（prio=1）排后面。
    //
    // 不加这个优先级的话：播放页会同时入队 8 张相关推荐封面 + 20 个评论头像，
    // 3 并发 + 每次 TLS 3~6 秒 —— 如果头像先到，封面就要干等二三十秒，
    // 用户看到的就是"封面显示不出来"。
    while (activeCoverJobs() < kMaxCoverConcurrent && !m_dlQueue.isEmpty()) {
        int pick = 0;
        for (int i = 0; i < m_dlQueue.size(); ++i) {
            if (m_dlQueue.at(i).prio <= 0) { pick = i; break; }
        }
        const PendingDl p = m_dlQueue.takeAt(pick);
        startDownload(p.url, p.path, p.tag, p.expected, p.rangeStart, p.rangeEnd, true);
    }
}

qint64 BiliApi::downloadWritten(const QString& tag) const
{
    CurlDownloadWorker* w = m_dlJobs.value(tag, 0);
    return w ? w->written() : 0;
}

int BiliApi::downloadPercent(const QString& tag) const
{
    CurlDownloadWorker* w = m_dlJobs.value(tag, 0);
    if (!w) return 100;
    const qint64 total = m_dlTotal.value(tag, 0);
    if (total <= 0) return 0;
    const qint64 done = w->written();
    int p = (int)(done * 100 / total);
    if (p < 0) p = 0;
    if (p > 99) p = 99;   // 100 只由 finished 给出，避免进度条假满
    return p;
}

void BiliApi::onDlWatchdog()
{
    if (m_dlQueue.isEmpty()) return;

    // 【v98】实时统计后这里只剩"补派"作用：槽位有空却没派出去 = 某次 pump 漏了。
    // 以前还要处理"计数器泄漏导致永久停摆"，现在计数是从 m_dlJobs 现数的，
    // 不存在泄漏，所以不需要强制归零了。
    if (activeCoverJobs() < kMaxCoverConcurrent) {
        pumpDownloads();
    }
}

void BiliApi::onDownloadFinished()
{
    CurlDownloadWorker* w = static_cast<CurlDownloadWorker*>(sender());
    if (!w) return;
    const QString tag = w->tag();
    const QString path = w->path();
    const bool ok = (w->code() == CURLE_OK);
    qDebug("BiliClient BiliApi::onDownloadFinished tag=%s code=%d bytes=%lld",
           qPrintable(tag), w->code(), (long long)w->written());
    m_dlJobs.remove(tag);
    m_dlTotal.remove(tag);
    const bool wasQueued = w->queued();
    w->deleteLater();

    // 【v94】记下"上一次有下载结束"的时刻，看门狗靠它判断队列是否卡死
    m_dlStallMs = QDateTime::currentMSecsSinceEpoch();
    if (wasQueued) {
        if (ok) m_imgOk++;
        else m_imgFail++;
    }

    // 【v102】封面下载完成且**字节校验通过** → 登记进就绪表 + 版本号 +1。
    // 版本号一变，QML 里所有 `coverUrlFor(url, biliApi.coverVersion)` 绑定重算，
    // 封面立刻出现 —— 完全不依赖 ListView 对模型行变化的处理。
    // 这是本工程里唯一被反复验证可靠的 C++→QML 通道（播放页 UP 主头像同款机制）。
    if (wasQueued && ok && isUsableImage(path)) {
        m_coverReady.insert(path, QLatin1String("file://") + path);
        ++m_coverVersion;
        emit coverVersionChanged();
    }

    // 【v84】归还并发额度并立刻派下一张封面。
    // 必须在 emit 之前 pump：emit 里 UI 可能马上又入队新的封面。
    if (wasQueued) {
        // 【v98】不再维护 m_dlActive 计数器。worker 已经从这里上面 remove 掉了，
        // activeCoverJobs() 自然少算一个，不会漏。
        pumpDownloads();
    }

    // 【v99】这个路径可能被多个 tag 同时请求（见 m_dlPathTags）。
    // 取出全部关心者，逐个回调 —— 只回调发起下载的那个 tag 的话，
    // 后请求的列表（比如切过去的"热门"页）永远等不到结果，封面一直是灰块。
    QStringList waiters = m_dlPathTags.take(path);
    if (waiters.isEmpty()) waiters << tag;   // 兜底：非封面队列的下载不会登记
    if (!waiters.contains(tag)) waiters.prepend(tag);

    for (int i = 0; i < waiters.size(); ++i) {
        dispatchDownloadResult(waiters.at(i), path, ok);
    }
}

void BiliApi::dispatchDownloadResult(const QString& tag, const QString& path, bool ok)
{
    // 用户头像：Cascades 的 ImageView 不能直连 https（走 QtNetwork 会永久挂起），
    // 必须和视频封面一样先用 curl 落到本地，再喂 file:// 路径。
    if (tag == QLatin1String("userface") || tag == QLatin1String("userface_alt")) {
        QFileInfo fi(path);
        if (ok && fi.exists() && fi.size() > 512) {
            m_userFace = "file://" + path;
            qDebug("BiliClient user face ready %s", qPrintable(m_userFace));
            emit userChanged();
        } else if (tag == QLatin1String("userface") && !m_userFaceAltTried
                   && m_userFaceRemote.startsWith("https:")) {
            // 【v87】https 失败就换 http 再试一次。反过来的情况也真实存在：
            // 某些网络里 hdslb 的 TLS 会被中间设备掐掉，而 http 反而能通。
            // 只回退一次，避免两个 scheme 来回打转。
            m_userFaceAltTried = true;
            QFile::remove(path);
            QString alt = m_userFaceRemote;
            alt.replace(0, 6, QString::fromLatin1("http:"));
            qDebug("BiliClient user face https failed, retry http");
            download(alt, path, "userface_alt", 0);
        } else {
            // 【v87】失败时把远端 URL 清掉，让下一次 nav 刷新能重试。
            // 原来不清：一次失败之后 m_userFaceRemote 已经等于该 URL，
            // 于是直到重启应用都不会再下 —— 用户看到的永远是空白头像。
            m_userFaceRemote.clear();
            qDebug("BiliClient user face download failed ok=%d", ok ? 1 : 0);
        }
        return;
    }

    emit downloadFinished(tag, path, ok);
}

void BiliApi::onCurlFinished()
{
    CurlWorker* w = static_cast<CurlWorker*>(sender());
    if (!w) return;
    QString out = w->result();
    int code = w->code();
    bool ok = (code == CURLE_OK);
    QString tag = w->tag();

    // 诊断通道走原有双通道自检逻辑（d1/d2 在 startNetwork 中已直接 connect 到
    // onDiagFinished，不会进入 onCurlFinished；此分支保留作为防御）。
    if (tag == QLatin1String(kDiagTag)) {
        w->deleteLater();
        return;
    }

    if (tag == QLatin1String(kNavTag)) {
        // nav 密钥预取通道：解析 WBI img_key/sub_key，写入 m_keysReady；
        // 同时解析登录用户信息（uname/face/mid/isLogin）。
        // 失败时 emit navFailed()，让正在等待的 FeedModel 结束加载态。
        onNavReply(out, ok);
        w->deleteLater();
        return;
    }

    if (tag == QLatin1String(kLoginQrTag)) {
        onLoginQrReply(out, ok);
        w->deleteLater();
        return;
    }

    if (tag == QLatin1String(kLoginPollTag)) {
        // 轮询请求开了 cookie 引擎，Set-Cookie 下来的登录凭证在 w->cookies() 里
        onLoginPollReply(out, ok, w->cookies());
        w->deleteLater();
        return;
    }

    // 业务请求通道：emit netReply 给已 connect 的接收方
    // m_bizReqCount 已在 launchCurl 中累加（get/getSigned 入口），此处只累加 fin。
    m_bizFinCount++;
    m_lastBiz = QString("%1/%2").arg(code).arg(out.size());
    qDebug("BiliClient BiliApi::onCurlFinished tag=%s code=%d bytes=%d", qPrintable(tag), code, out.size());
    w->deleteLater();
    emit netReply(tag, out, ok);
}

/** cookie 落盘位置：沙盒根目录。不要写 /accounts/1000/shared（需 access_shared 权限）。 */
static QString cookieFilePath()
{
    return QDir(QDir::homePath()).filePath("bili_cookie.txt");
}

static QString keyOf(const QString& url)
{
    QString name = url.mid(url.lastIndexOf('/') + 1);
    int dot = name.indexOf('.');
    return dot > 0 ? name.left(dot) : name;
}

static QString mixinKey(const QString& imgKey, const QString& subKey)
{
    QString s = imgKey + subKey;
    QString r;
    for (int i = 0; i < 32 && i < 64; ++i) {
        int idx = MIXIN_TAB[i];
        if (idx < s.length()) {
            r.append(s.at(idx));
        }
    }
    return r;
}

void BiliApi::request(const QString& url, const QString& tag, const QString& ua, bool cookieJar)
{
    launchCurl(url, tag, ua, cookieJar);
}

void BiliApi::getSigned(const QString& path, const QVariantMap& params, const QString& tag)
{
    QVariantMap p = params;

    // 2026-10-31 修复：bilibili 已移除 w_rid 签名校验（实测全接口无签名均 code=0），
    // 之前的"nav 取密钥 → 挂起等 nav 返回 → 再签名发请求"链路在设备端因 nav 响应
    // 解析异常而永久挂起，导致推荐/搜索页 30s 超时。
    // 新策略：w_rid 仍随请求携带（若密钥已就绪则参与签名，否则传空占位），
    // 请求不再依赖密钥，直接发出，bilibili 无校验时行为与有校验时完全一致。
    if (m_keysReady) {
        p.insert("wts", QString::number(QDateTime::currentMSecsSinceEpoch() / 1000));

        QStringList keys;
        QMap<QString, QString> flat;
        QMapIterator<QString, QVariant> it(p);
        while (it.hasNext()) {
            it.next();
            flat.insert(it.key(), it.value().toString());
            keys << it.key();
        }
        keys.sort(); // ASCII 字典序，与 JS Object.keys().sort() 一致

        QStringList pairs;
        for (int i = 0; i < keys.size(); ++i) {
            pairs << percentEncode(keys.at(i)) + "=" + percentEncode(flat.value(keys.at(i)));
        }
        QString query = pairs.join("&");
        QString wrid = QString(QCryptographicHash::hash((query + mixinKey(m_imgKey, m_subKey)).toUtf8(),
                                                        QCryptographicHash::Md5).toHex());
        QString url = "https://api.bilibili.com" + path + "?" + query + "&w_rid=" + wrid;
        request(url, tag);
    } else {
        // 无密钥 fallback：仅拼业务参数（不含 wts/w_rid），bilibili 当前版本直接接受
        QStringList pairs;
        QStringList keys;
        QVariantMap::const_iterator it = p.constBegin();
        for (; it != p.constEnd(); ++it) {
            keys << it.key();
        }
        keys.sort();
        for (int i = 0; i < keys.size(); ++i) {
            pairs << percentEncode(keys.at(i)) + "=" + percentEncode(p.value(keys.at(i)).toString());
        }
        QString url = "https://api.bilibili.com" + path;
        if (!pairs.isEmpty()) {
            url += "?" + pairs.join("&");
        }
        request(url, tag);
    }
}

void BiliApi::onNavReply(const QString& json, bool ok)
{
    bool navOk = ok && !json.isEmpty();
    qDebug("BiliClient BiliApi::onNavReply ok=%d bytes=%d", navOk, json.size());
    if (navOk) {
        JsonDataAccess jda;
        QVariant v = jda.loadFromBuffer(json.toUtf8());
        QVariantMap root = v.toMap();
        QVariantMap nav = root.value("data").toMap();
        QVariantMap wbi = nav.value("wbi_img").toMap();
        m_imgKey = keyOf(wbi.value("img_url").toString());
        m_subKey = keyOf(wbi.value("sub_url").toString());
        m_keysReady = !m_imgKey.isEmpty() && !m_subKey.isEmpty();

        // ---- 登录用户信息（同一份返回里，不额外发请求）----
        const int navCode = root.value("code").toInt();
        const bool isLogin = nav.value("isLogin").toBool();
        const QString uname = nav.value("uname").toString();
        const QString mid = nav.value("mid").toString();
        QString face = nav.value("face").toString();
        // 【v87】原来这里把 //i0.hdslb.com/... 补成 "http:"。改成 https：
        // 视频封面一路都是 https 下载（v78 起稳定），头像是唯一还在用裸 http 的，
        // 而 hdslb 的 CDN 对 http 经常直接 403/重定向 —— 下载失败就是头像空白。
        // curl 自带 OpenSSL，https 握手没问题（走 QtNetwork 才有问题）。
        if (face.startsWith("//")) face = "https:" + face;

        // -101 = 未登录。带着 cookie 还被判未登录，说明凭证已过期（SESSDATA 有效期
        // 约半年），清掉免得后续每个请求都白发一个无效 Cookie 头。
        if (navCode == -101 && !m_cookie.isEmpty()) {
            qDebug("BiliClient cookie expired (nav -101), clearing");
            m_cookie = "";
            QFile::remove(cookieFilePath());
        }

        bool changed = (isLogin != m_loggedIn) || (uname != m_userName) || (mid != m_userMid);
        m_loggedIn = isLogin;
        m_userName = uname;
        m_userMid = mid;

        // 头像走 curl 落地：Cascades ImageView 直连 https 会永久挂起。
        // 只在 URL 变了才重下，避免每次 nav 都拉一遍。
        if (isLogin && !face.isEmpty() && face != m_userFaceRemote) {
            m_userFaceRemote = face;
            m_userFaceAltTried = false;   // 【v87】换账号/换 URL → 重新允许一次 http 回退
            QDir dir(QDir::homePath());
            if (!dir.exists("images")) dir.mkpath("images");
            // 【v87】文件名带时间戳，每次下载都换一个新路径。
            //
            // 原因：Cascades 的 ImageView 按 source URL 缓存解码结果。如果第一次
            // 加载失败（比如旧版本还没修 https 直连的那个 bug），同一个 URL 之后
            // 可能一直渲染成空白，即使文件已经换成了真图。换文件名 = 换 URL，
            // 一定重新解码。用户换了头像时 nav 给的 URL 会变、这里也会重下，
            // 同样受益。
            //
            // 代价是老文件会累积，所以先清掉同前缀的旧头像。
            const QStringList olds = dir.entryList(QStringList() << "userface*.jpg", QDir::Files);
            for (int i = 0; i < olds.size(); ++i) {
                QFile::remove(dir.absoluteFilePath(olds.at(i)));
            }
            const QString dst = dir.absoluteFilePath(
                QString("userface_%1_%2.jpg")
                    .arg(mid.isEmpty() ? QString::fromLatin1("0") : mid)
                    .arg((uint)QDateTime::currentDateTime().toTime_t()));
            // 先删旧图，避免下载失败时显示上一个账号的头像
            QFile::remove(dst);
            m_userFace = "";
            download(face, dst, "userface", 0);
            changed = true;
        } else if (!isLogin) {
            m_userFaceRemote = "";
            m_userFaceAltTried = false;
            m_userFace = "";
        }

        if (changed) {
            qDebug("BiliClient user isLogin=%d uname=%s mid=%s", isLogin ? 1 : 0,
                   qPrintable(uname), qPrintable(mid));
            emit userChanged();
        }
    }
    if (!m_keysReady) {
        // 密钥获取失败：广播通知所有等待中的 signed 请求结束加载态（避免无限转圈）
        emit navFailed();
    }
}

// ============================================================================
//  登录（扫码）
// ============================================================================
//
// 流程（B 站 Web 端扫码登录，三个接口）：
//   1. GET /x/passport-login/web/qrcode/generate  → { data: { url, qrcode_key } }
//      url 就是二维码内容，qrcode_key 用于轮询。
//   2. 把 url 画成 PNG 落盘，QML 用 file:// 显示（Cascades 的 ImageView 直连
//      https 会走 QtNetwork，在 BB10 上永久挂起 —— 和封面图同一个坑）。
//   3. 每 2 秒 GET /x/passport-login/web/qrcode/poll?qrcode_key=..
//      业务码在 data.code 里：
//        86101 未扫码 / 86090 已扫码待确认 / 86038 已失效 / 0 成功
//   4. 成功后凭证有两处来源，优先用 curl cookie 引擎 dump 出来的 Set-Cookie
//      （原样、无编码歧义），拿不到再从 data.url 的 query 里抠，作为兜底。
//
// 为什么不用账号密码登录：BB10 上要处理密码加密（RSA/pubkey）+ 极验验证码，
// 而且极验是 JS 挑战，没有 WebView 就走不通。扫码只依赖一个 PNG 渲染。

/**
 * 从 poll 返回的 data.url 里抠登录字段（Set-Cookie 拿不到时的兜底）。
 * 【不要】做 URL 解码：B 站 SESSDATA 的值本身就含 %2C，解码成 ',' 反而认不出。
 */
static QString cookieFromLoginUrl(const QString& url)
{
    const int q = url.indexOf(QLatin1Char('?'));
    if (q < 0) return QString();
    static const char* const want[] = {
        "SESSDATA", "bili_jct", "DedeUserID", "DedeUserID__ckMd5", "sid"
    };
    const QStringList pairs = url.mid(q + 1).split(QLatin1Char('&'), QString::SkipEmptyParts);
    QStringList keep;
    for (int i = 0; i < pairs.size(); ++i) {
        const QString& kv = pairs.at(i);
        const int eq = kv.indexOf(QLatin1Char('='));
        if (eq <= 0) continue;
        const QString k = kv.left(eq);
        for (int w = 0; w < 5; ++w) {
            if (k == QLatin1String(want[w])) {
                keep << kv;
                break;
            }
        }
    }
    return keep.join(QString::fromLatin1("; "));
}

void BiliApi::loadCookie()
{
    QFile f(cookieFilePath());
    if (!f.exists()) return;
    if (!f.open(QIODevice::ReadOnly)) {
        qDebug("BiliClient loadCookie open fail");
        return;
    }
    m_cookie = QString::fromUtf8(f.readAll()).trimmed();
    f.close();
    qDebug("BiliClient cookie loaded bytes=%d", m_cookie.size());
}

void BiliApi::saveCookie()
{
    QFile f(cookieFilePath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qDebug("BiliClient saveCookie open fail");
        return;
    }
    f.write(m_cookie.toUtf8());
    f.close();
    qDebug("BiliClient cookie saved bytes=%d", m_cookie.size());
}

void BiliApi::setCookie(const QString& c)
{
    m_cookie = c;
    saveCookie();
}

void BiliApi::setLoginState(int st, const QString& tip)
{
    if (st == m_loginState && tip == m_loginTip) return;
    m_loginState = st;
    m_loginTip = tip;
    qDebug("BiliClient login state=%d tip=%s", st, qPrintable(tip));
    emit loginChanged();
}

void BiliApi::stopPoll()
{
    if (m_pollTimer) m_pollTimer->stop();
    m_pollBusy = false;
    m_qrKey = "";
}

void BiliApi::loginStart()
{
    if (!m_pollTimer) {
        m_pollTimer = new QTimer(this);
        m_pollTimer->setInterval(2000);
        QObject::connect(m_pollTimer, SIGNAL(timeout()), this, SLOT(onLoginPollTick()));
    }
    m_pollTimer->stop();
    m_pollBusy = false;
    m_qrKey = "";

    // 旧二维码图删掉（文件名带时间戳，不删会一直堆在沙盒里）
    if (!m_qrPath.isEmpty()) {
        QFile::remove(m_qrPath);
        m_qrPath = "";
    }
    m_qrUrl = "";

    setLoginState(1, QString::fromUtf8("正在获取二维码…"));
    request(QString::fromLatin1("https://passport.bilibili.com/x/passport-login/web/qrcode/generate"),
            QString::fromLatin1(kLoginQrTag));
}

void BiliApi::loginCancel()
{
    stopPoll();
}

void BiliApi::logout()
{
    stopPoll();
    m_cookie = "";
    QFile::remove(cookieFilePath());
    m_loggedIn = false;
    m_userName = "";
    m_userMid = "";
    m_userFace = "";
    m_userFaceRemote = "";
    m_userFaceAltTried = false;
    setLoginState(0, "");
    qDebug("BiliClient logout done");
    emit userChanged();
}

void BiliApi::refreshUser()
{
    // nav 一次拿两样：WBI 密钥 + 用户信息（见 onNavReply）
    request(QString::fromLatin1("https://api.bilibili.com/x/web-interface/nav"),
            QString::fromLatin1(kNavTag));
}

void BiliApi::onLoginQrReply(const QString& json, bool ok)
{
    if (!ok || json.isEmpty()) {
        setLoginState(5, QString::fromUtf8("网络请求失败，请重试"));
        return;
    }
    JsonDataAccess jda;
    QVariantMap root = jda.loadFromBuffer(json.toUtf8()).toMap();
    const int code = root.value("code").toInt();
    QVariantMap data = root.value("data").toMap();
    const QString content = data.value("url").toString();
    const QString key = data.value("qrcode_key").toString();
    qDebug("BiliClient loginQr code=%d urlLen=%d keyLen=%d", code, content.size(), key.size());

    if (code != 0 || content.isEmpty() || key.isEmpty()) {
        setLoginState(5, QString::fromUtf8("获取二维码失败 (code=%1)").arg(code));
        return;
    }
    m_qrKey = key;

    // 文件名带毫秒时间戳：Cascades 的 ImageView 按 URL 缓存，同名覆盖不会重载，
    // 表现为"点了刷新二维码，图还是旧的"。
    QDir home(QDir::homePath());
    const QString path = home.absoluteFilePath(
        QString("qrcode_%1.png").arg(QDateTime::currentMSecsSinceEpoch()));

    // scale 传 0 = 自动（按模块数算到整图约 560px，保证每模块 ≥6px）。
    // 写死 8 的话 V8（49 模块）只有 456px，屏幕小、离得远时手机容易对不上焦。
    if (!QrCodeGen::generate(content, 'M', path, 0)) {
        qDebug("BiliClient qrcode generate failed -> %s", qPrintable(path));
        setLoginState(5, QString::fromUtf8("二维码生成失败"));
        return;
    }
    if (!m_qrPath.isEmpty()) QFile::remove(m_qrPath);
    m_qrPath = path;
    m_qrUrl = "file://" + path;

    setLoginState(1, QString::fromUtf8("请用「哔哩哔哩」App 扫码"));
    if (m_pollTimer) m_pollTimer->start();
}

void BiliApi::onLoginPollTick()
{
    if (m_qrKey.isEmpty()) {
        stopPoll();
        return;
    }
    if (m_pollBusy) return;   // 上一次还没回来，跳过这一轮（不叠加请求）
    m_pollBusy = true;
    const QString url =
        QString::fromLatin1("https://passport.bilibili.com/x/passport-login/web/qrcode/poll")
        + "?qrcode_key=" + percentEncode(m_qrKey)
        + QString::fromLatin1("&source=main-fe-header");
    // cookieJar=true：让 curl 引擎收集 Set-Cookie，成功后 dump 出登录凭证
    request(url, QString::fromLatin1(kLoginPollTag), QString(), true);
}

void BiliApi::onLoginPollReply(const QString& json, bool ok, const QString& cookies)
{
    m_pollBusy = false;
    // 网络抖动：不改状态，等下一轮（二维码有效期约 180 秒，够容错几次）
    if (!ok || json.isEmpty()) return;

    JsonDataAccess jda;
    QVariantMap root = jda.loadFromBuffer(json.toUtf8()).toMap();
    const int outer = root.value("code").toInt();
    if (outer != 0) {
        // 外层 code 非 0 = 接口层面出错（风控/参数不对），不是扫码进度
        stopPoll();
        setLoginState(5, QString::fromUtf8("登录接口错误 (code=%1)").arg(outer));
        return;
    }
    QVariantMap data = root.value("data").toMap();
    // 【坑】外层 code 恒为 0，真正的业务码在 data.code 里
    const int sc = data.value("code").toInt();

    if (sc == 86101) {
        setLoginState(1, QString::fromUtf8("请用「哔哩哔哩」App 扫码"));
        return;
    }
    if (sc == 86090) {
        setLoginState(2, QString::fromUtf8("已扫码，请在手机上确认登录"));
        return;
    }
    if (sc == 86038) {
        stopPoll();
        setLoginState(4, QString::fromUtf8("二维码已失效，请重新获取"));
        return;
    }
    if (sc != 0) {
        stopPoll();
        setLoginState(5, QString::fromUtf8("登录异常 (code=%1)").arg(sc));
        return;
    }

    // ---- 成功 ----
    stopPoll();
    QString ck = cookies;
    if (ck.isEmpty()) ck = cookieFromLoginUrl(data.value("url").toString());
    qDebug("BiliClient login OK cookieBytes=%d(fromJar=%d)", ck.size(), cookies.isEmpty() ? 0 : 1);
    if (ck.isEmpty()) {
        setLoginState(5, QString::fromUtf8("登录成功但没拿到凭证，请重试"));
        return;
    }
    setCookie(ck);
    setLoginState(3, QString::fromUtf8("登录成功"));
    refreshUser();
}

QString BiliApi::percentEncode(const QString& s)
{
    // 模拟 JS encodeURIComponent：保留 !~*'() 及 unreserved
    const QByteArray utf = s.toUtf8();
    QString out;
    for (int i = 0; i < utf.size(); ++i) {
        char c = utf.at(i);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' || c == ')') {
            out.append(QChar::fromLatin1(c));
        } else {
            out.append('%');
            out.append(QString("%1").arg((unsigned char)c, 2, 16, QLatin1Char('0')).toUpper());
        }
    }
    return out;
}

QString BiliApi::countText(qint64 n)
{
    if (n >= 100000000) return QString::number(n / 100000000.0, 'f', 1) + QString::fromUtf8("亿");
    if (n >= 10000) return QString::number(n / 10000.0, 'f', 1) + QString::fromUtf8("万");
    return QString::number(n);
}

QString BiliApi::durationText(long long sec)
{
    if (sec < 60) return QString::number(sec) + QString::fromUtf8("秒");
    long long m = sec / 60, s = sec % 60;
    if (m < 60) {
        return QString("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
    }
    long long h = m / 60;
    m %= 60;
    return QString("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
}

QString BiliApi::coverUrl(const QString& raw)
{
    if (raw.isEmpty()) return QString();
    QString u = raw;
    // 有些接口直接返回带 @ 参数的地址，先砍掉，避免叠加成 @672w...@672w...
    const int at = u.indexOf('@');
    if (at > 0) u = u.left(at);
    if (!u.startsWith("http", Qt::CaseInsensitive)) return u;
    // 【v96】统一升级到 https。
    // 明文 http 的封面在移动网络里很容易被运营商劫持成广告页/错误页 ——
    // 那种响应也是几 KB 的 HTML，只按大小判断会被当成有效图片缓存，
    // 于是 ImageView 解码失败、封面永远空白，而且命中缓存后不会再重下。
    // B 站所有封面 CDN 都支持 https（实测 http/https 均 200）。
    // 万一某个网络里 TLS 被中间设备掐掉，下载失败后重试会切回 http。
    if (u.startsWith("http://", Qt::CaseInsensitive)) {
        u.replace(0, 7, QString::fromLatin1("https://"));
    }
    return u + QLatin1String("@672w_378h_1c.jpg");
}

bool BiliApi::isImageFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray h = f.read(12);
    f.close();
    if (h.size() >= 2 && (quint8)h.at(0) == 0xFF && (quint8)h.at(1) == 0xD8) return true;   // JPEG
    if (h.size() >= 4 && (quint8)h.at(0) == 0x89 && h.mid(1, 3) == "PNG") return true;      // PNG
    if (h.size() >= 12 && h.startsWith("RIFF") && h.mid(8, 4) == "WEBP") return true;       // WEBP
    if (h.size() >= 3 && h.startsWith("GIF")) return true;                                  // GIF
    return false;
}

bool BiliApi::isUsableImage(const QString& path, qint64 minSize)
{
    // 【v100】只看字节，不依赖任何图片解码插件。
    //
    // v99 这里用了 QImageReader::canRead()/size() 做"真解码"校验，结果在设备上
    // 一张封面都不显示 —— BB10 的 Qt 图片格式插件在 Cascades 应用里不一定被
    // 正确加载，canRead() 可能对**所有** jpeg 返回 false。于是每张封面刚下完
    // 就被判成坏图删掉，重试一次还是失败，最后整页灰块。
    // 教训：缓存校验绝不能依赖"能不能解码"，只能依赖"字节是不是完整"。
    //
    // 完整性用头 + 尾双标记判定，纯字节比较，零依赖：
    //   JPEG 必须以 FFD9 收尾（被截断/被并发写坏的文件没有这个结束标记）
    //   PNG  以 IEND 收尾
    //   WEBP 的 RIFF 头里写了总长度，和实际大小对得上才算完整
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const qint64 sz = f.size();
    if (sz < minSize) { f.close(); return false; }   // 封面 1KB / 头像 512B，见头文件
    const QByteArray head = f.read(12);
    const int tailLen = (int)(sz < 64 ? sz : 64);
    f.seek(sz - tailLen);
    const QByteArray tail = f.read(tailLen);
    f.close();
    if (head.size() < 4 || tail.size() < 2) return false;

    const quint8 b0 = (quint8)head.at(0);
    const quint8 b1 = (quint8)head.at(1);

    if (b0 == 0xFF && b1 == 0xD8) {
        return tail.contains(QByteArray("\xff\xd9", 2));
    }
    if (b0 == 0x89 && head.mid(1, 3) == QByteArray("PNG")) {
        return tail.contains(QByteArray("IEND", 4));
    }
    if (head.startsWith(QByteArray("RIFF")) && head.mid(8, 4) == QByteArray("WEBP")) {
        const quint32 riffSize = (quint32)(quint8)head.at(4)
                               | ((quint32)(quint8)head.at(5) << 8)
                               | ((quint32)(quint8)head.at(6) << 16)
                               | ((quint32)(quint8)head.at(7) << 24);
        return (qint64)riffSize + 8 <= sz;
    }
    if (head.startsWith(QByteArray("GIF"))) {
        return (quint8)tail.at(tail.size() - 1) == 0x3B;
    }
    return false;
}

QString BiliApi::readyImageFor(const QString& local, qint64 minSize)
{
    if (local.isEmpty()) return QString();

    // 就绪表命中 = 纯哈希查找，零文件 IO（绑定重算会高频调用）
    const QHash<QString, QString>::const_iterator it = m_coverReady.constFind(local);
    if (it != m_coverReady.constEnd()) return it.value();

    // 未登记但文件已经在（例如上一版留下的缓存、或刚启动时）——现场验一次
    if (QFile::exists(local) && isUsableImage(local, minSize)) {
        const QString u = QLatin1String("file://") + local;
        m_coverReady.insert(local, u);
        return u;
    }
    return QString();
}

QString BiliApi::coverUrlFor(const QString& raw, int ver)
{
    Q_UNUSED(ver);   // 唯一作用是把 coverVersion 拉进 QML 的依赖图，见头文件说明
    if (raw.isEmpty()) return QString();
    // 已经是本地路径就直接用（已下载列表走这条）
    if (raw.startsWith(QLatin1String("file://"))) return raw;

    return readyImageFor(coverCachePath(raw), 1024);
}

QString BiliApi::faceUrlFor(const QString& raw, int ver)
{
    Q_UNUSED(ver);   // 同上：靠 coverVersion 进依赖图
    if (raw.isEmpty()) return QString();
    if (raw.startsWith(QLatin1String("file://"))) return raw;

    // 头像 URL 和封面 URL 的规范化规则不同（头像**不能**加 @672w_378h_1c 后缀，
    // 见 VideoStore::resolveFace 里的说明），所以这里不能走 coverUrl，
    // 否则算出来的路径和下载时用的路径不是同一个文件 → 永远查不到。
    QString u = raw;
    if (u.startsWith(QLatin1String("//"))) u = QLatin1String("https:") + u;
    if (!u.startsWith(QLatin1String("http"))) return QString();

    return readyImageFor(faceCachePath(u), 512);
}

QString BiliApi::faceCachePath(const QString& raw)
{
    QString name = raw;
    name.replace(QLatin1String("https://"), QLatin1String(""));
    name.replace(QLatin1String("http://"), QLatin1String(""));
    name.replace(QLatin1Char('/'), QLatin1Char('_'));
    name.replace(QLatin1Char('?'), QLatin1Char('_'));
    name.replace(QLatin1Char('&'), QLatin1Char('_'));
    name.replace(QLatin1Char('='), QLatin1Char('_'));
    name.replace(QLatin1Char(':'), QLatin1Char('_'));
    // 【为什么不截断】封面那边截到 60 字符是因为 CDN 文件名很长且前缀唯一；
    // 头像 URL 形如 i0.hdslb.com_bfs_face_<32位hex>.jpg@120w_120h_1c.jpg，
    // 恰好 60 上下，一截就把 hex 尾巴砍掉，两个用户会撞到同一个缓存文件。
    if (name.isEmpty()) return QString();

    static QString s_imgDir;
    if (s_imgDir.isEmpty()) {
        QDir dir(QDir::homePath());
        if (!dir.exists("images")) dir.mkpath("images");
        s_imgDir = dir.absoluteFilePath("images");
    }
    return s_imgDir + QLatin1Char('/') + name + QLatin1String(".jpg");
}

QString BiliApi::coverCachePath(const QString& raw)
{
    const QString u = coverUrl(raw);
    if (u.isEmpty()) return QString();

    // 【v101】文件名改成 URL 的 md5 —— 照 BBTube 的做法
    // （它的 advimages 缓存就是 QCryptographicHash + toHex）。
    //
    // 原来是把 URL 里的 / ? & = : 换成下划线再截断到 60 字符。两个隐患：
    //   1) **@ 没被替换**。URL 带 @672w_378h_1c.jpg 后缀，短路径下 @ 会留在文件名里，
    //      拼成 "file://" + path 后要过 QUrl 解析，属于不必要的风险；
    //   2) 60 字符截断对长 host（如 archive.biliimg.com）会把 hash 也切掉一截，
    //      极端情况下两张不同封面可能算出同一个文件名 → 互相覆盖。
    //
    // md5 是定长 32 字符、纯十六进制、零特殊字符，这两个问题一起消失。
    // 代价是老缓存全部作废、重下一遍 —— 反正 v99 已经清过一次，可以接受。
    const QByteArray h = QCryptographicHash::hash(u.toUtf8(), QCryptographicHash::Md5).toHex();

    // 【v102】images 目录路径缓存成静态量。
    // coverUrlFor() 现在会被 QML 绑定高频调用（每行每次重算），
    // 每次都 QDir::homePath() + exists() 是两个系统调用，没必要。
    static QString s_imgDir;
    if (s_imgDir.isEmpty()) {
        QDir dir(QDir::homePath());
        if (!dir.exists("images")) dir.mkpath("images");
        s_imgDir = dir.absoluteFilePath("images");
    }
    return s_imgDir + QLatin1Char('/') + QString::fromLatin1(h) + QLatin1String(".jpg");
}

/* ================= 小红书笔记 API 入口实现 ================= */

QString BiliApi::noteApiBase()
{
    return QString::fromLatin1("https://api.bilibili.com");
}

QString BiliApi::noteDetailApi(const QString& noteId)
{
    return QString::fromLatin1("/api/sns/v10/note/") + noteId;
}

QString BiliApi::noteFeedApi()
{
    return QString::fromLatin1("/api/sns/v10/recommend");
}

QString BiliApi::noteLikeApi(const QString& noteId)
{
    return QString::fromLatin1("/api/sns/v10/like/") + noteId;
}

QString BiliApi::noteCollectApi(const QString& noteId)
{
    return QString::fromLatin1("/api/sns/v10/collect/") + noteId;
}

QString BiliApi::userFollowApi(const QString& userId)
{
    return QString::fromLatin1("/api/sns/v10/follow/") + userId;
}

QString BiliApi::followFeedApi()
{
    return QString::fromLatin1("/api/sns/v10/follow/feed");
}

QString BiliApi::historyApi()
{
    return QString::fromLatin1("/api/sns/v10/history");
}

QString BiliApi::collectNotesApi()
{
    return QString::fromLatin1("/api/sns/v10/collection/notes");
}

QString BiliApi::timeAgo(long long unix)
{
    QDateTime dt = QDateTime::fromMSecsSinceEpoch(unix * 1000);
    qint64 secs = dt.secsTo(QDateTime::currentDateTime());
    if (secs < 60) return QString::fromUtf8("刚刚");
    if (secs < 3600) return QString::number(secs / 60) + QString::fromUtf8("分钟前");
    if (secs < 86400) return QString::number(secs / 3600) + QString::fromUtf8("小时前");
    if (secs < 86400 * 30) return QString::number(secs / 86400) + QString::fromUtf8("天前");
    if (secs < 86400 * 365) return QString::number(secs / (86400 * 30)) + QString::fromUtf8("个月前");
    return dt.toString("yyyy-MM-dd");
}
