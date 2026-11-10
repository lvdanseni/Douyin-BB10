#include "douyin_api.hpp"

#include "qrcodegen.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMultiMap>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
#include <cstdio>
#include <curl/curl.h>
#include <bb/data/JsonDataAccess>

using namespace bb::data;

static const char* const kDiagTag  = "__diag__";
static const char* const kLoginQrTag   = "__dyloginqr__";
static const char* const kLoginPollTag = "__dyloginpoll__";
static const char* const kLoginVerifyTag = "__dyloginverify__";

/** self 接口的固定公共参数（与 feed 同族，2026-11-06 curl 实测 200 JSON）。 */
static const char* const kSelfQuery =
    "device_platform=android&version_code=290200&aid=1233&app_name=aweme"
    "&os=android&os_api=28&device_type=SM-G981B";

/**
 * 抖音移动端默认 UA（Android WebView，抖音极速版常用）。
 * 抓包后若发现抖音要求不同 UA，直接改这里。
 */
/* 2026-11-06 curl 实证：aweme.snssdk.com /aweme/v1/feed/ 用这个 App UA +
 * Accept: application/json 稳定返回 200 JSON（免签名）。控制变量实验显示
 * Chrome UA 也能 200，但统一用 App 协议值，避免服务端按 UA 协商成 protobuf。 */
static const char* DOUYIN_UA =
    "com.ss.android.ugc.aweme/290200 (Linux; U; Android 9; zh_CN; SM-G981B; "
    "Build/G981BZSU2ATA5; Cronet/TTNetVersion:7a5b1a1b 2023-03-20 "
    "QuicVersion:00424bb5 2022-10-11)";

class DyCurlWorker : public QThread {
public:
    explicit DyCurlWorker(const QByteArray& url, bool isDiag, bool follow = true,
                        const QString& tag = QString(), const QByteArray& ua = QByteArray(),
                        const QByteArray& cookie = QByteArray(), bool cookieJar = false,
                        const QByteArray& body = QByteArray(),
                        const QByteArray& contentType = QByteArray())
        : m_url(url), m_diag(isDiag), m_follow(follow), m_tag(tag), m_ua(ua),
          m_cookie(cookie), m_jar(cookieJar), m_body(body), m_contentType(contentType),
          m_code(-99), m_httpCode(-99) {}
    QString result() const { return m_result; }
    int code() const { return m_code; }
    int httpCode() const { return m_httpCode; }
    QString tag() const { return m_tag; }
    QString cookies() const { return m_cookies; }

protected:
    void run() {
        // 【1.0.0.30 诊断】真机 .30 首现 code=-99（构造初值，perform 未执行的铁证），
        // 三点埋点定位：enter / perform-enter / perform-exit(带 errbuf)。
        dyNetLog(QString("net worker enter tag=%1 diag=%2 url=%3")
                     .arg(m_tag).arg(m_diag ? 1 : 0)
                     .arg(QString::fromUtf8(m_url.left(80))));
        CURL* c = curl_easy_init();
        if (!c) {
            m_code = -100;
            dyNetLog(QString("net worker curl_init_fail tag=%1").arg(m_tag));
            return;
        }
        curl_easy_setopt(c, CURLOPT_URL, m_url.constData());
        // 【1.0.0.37】强制 IPv4 解析：BB10 网络栈没有 IPv6 出口，抖音 CDN 的
        // 部分节点 302 跳 IPv6 字面量地址 —— FOLLOWLOCATION 自动跟随后
        // curl 报 7（CONN_FAIL），feed 整页空、有的视频整条拉不到。
        // 业务 GET 与封面 GET 都走这里，一行解决一类"加载不出来"。
        curl_easy_setopt(c, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, m_follow ? 1L : 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
        // v1/feed count=15 实测 280KB / host 1.5s；BB10 上 JSON 解析更慢，
        // 15s 曾在弱网下贴近临界，放宽到 25s（加载看门狗同步放到 45s）。
        curl_easy_setopt(c, CURLOPT_TIMEOUT, m_diag ? 8L : 25L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, m_ua.isEmpty() ? DOUYIN_UA : m_ua.constData());
        curl_easy_setopt(c, CURLOPT_REFERER, "https://www.douyin.com/");
        curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
        if (!m_cookie.isEmpty()) {
            curl_easy_setopt(c, CURLOPT_COOKIE, m_cookie.constData());
        }
        if (m_jar) {
            curl_easy_setopt(c, CURLOPT_COOKIEFILE, "");
        }
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &m_result);
        // 【1.0.0.30 诊断】错误缓冲：curl 失败原因的人话（DNS/connect/SSL）。
        char errbuf[CURL_ERROR_SIZE];
        errbuf[0] = '\0';
        curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
        struct curl_slist* hdrs = 0;
        // 【协议实证】v2/feed 只返 protobuf，v1/feed 要显式声明 JSON。
        // 不加也可能 200 JSON，但显式声明可避免服务端按 UA 协商格式。
        hdrs = curl_slist_append(hdrs, "Accept: application/json");
        if (!m_body.isEmpty()) {
            curl_easy_setopt(c, CURLOPT_POST, 1L);
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, m_body.constData());
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)m_body.size());
            // 【1.0.0.29】Content-Type 可配：默认 form-urlencoded（旧行为不变）；
            // 传 application/json 走评论发表（POST /aweme/v1/comment/publish/
            // 只认 JSON body —— curl 实证：form 编码到达不了鉴权层之后的解析）。
            hdrs = curl_slist_append(hdrs, m_contentType.isEmpty()
                                             ? "Content-Type: application/x-www-form-urlencoded"
                                             : (QByteArray("Content-Type: ") + m_contentType).constData());
        }
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
        dyNetLog(QString("net worker perform-enter tag=%1").arg(m_tag));
        m_code = curl_easy_perform(c);
        dyNetLog(QString("net worker perform-exit tag=%1 code=%2 err=%3")
                     .arg(m_tag).arg(m_code)
                     .arg(errbuf[0] ? QString::fromUtf8(errbuf).left(80)
                                    : QString("<none>")));
        // 【1.0.0.30】HTTP 状态码单独取：curl code==OK 也可能是 403/429，
        // 只有 code+http+bytes 三个值同时在日志里才能一次定性。
        long rc = 0;
        if (curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &rc) == CURLE_OK) {
            m_httpCode = (int)rc;
        }
        if (hdrs) curl_slist_free_all(hdrs);
        if (m_jar) {
            dumpCookies(c);
        }
        curl_easy_cleanup(c);
    }

private:
    void dumpCookies(CURL* c) {
        struct curl_slist* sl = 0;
        if (curl_easy_getinfo(c, CURLINFO_COOKIELIST, &sl) != CURLE_OK || !sl) return;
        QStringList parts;
        for (struct curl_slist* p = sl; p; p = p->next) {
            if (!p->data) continue;
            const QStringList f = QString::fromUtf8(p->data).split(QLatin1Char('\t'));
            if (f.size() < 7) continue;
            const QString name = f.at(5).trimmed();
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
    QByteArray m_body;
    QByteArray m_contentType;
    QString m_result;
    QString m_cookies;
    int m_code;
    int m_httpCode;
};

/**
 * 二进制下载 worker：把抖音 MP4 直链落盘到应用沙盒。
 * 抖音 CDN 同样校验 UA/Referer，MediaPlayer 无法带请求头，故先用 curl 落盘再播放。
 */
class DyCurlDownloadWorker : public QThread {
public:
    DyCurlDownloadWorker(const QByteArray& url, const QString& path, const QString& tag,
                       qint64 rangeStart = -1, qint64 rangeEnd = -1,
                       const QByteArray& cookie = QByteArray(), bool queued = false)
        : m_url(url), m_path(path), m_tag(tag), m_code(-99), m_httpCode(-99), m_written(0),
          m_file(0), m_rangeStart(rangeStart), m_rangeEnd(rangeEnd), m_cookie(cookie),
          m_queued(queued) {}
    QString tag() const { return m_tag; }
    QString path() const { return m_path; }
    int code() const { return m_code; }
    int httpCode() const { return m_httpCode; }
    qint64 written() const { return m_written; }
    bool queued() const { return m_queued; }

protected:
    void run() {
        const QString tmpPath = m_path + QLatin1String(".part");
        m_file = fopen(tmpPath.toUtf8().constData(), "wb");
        if (!m_file) { m_code = -201; return; }
        CURL* c = curl_easy_init();
        if (!c) { fclose(m_file); m_file = 0; QFile::remove(tmpPath); m_code = -100; return; }
        curl_easy_setopt(c, CURLOPT_URL, m_url.constData());
        // 【1.0.0.37】同 DyCurlWorker：强制 IPv4（BB10 无 IPv6 出口，
        // CDN 跳 IPv6 字面量 = curl 7，下载/封面整条失败）。
        curl_easy_setopt(c, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
        if (m_queued) {
            curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
        } else {
            curl_easy_setopt(c, CURLOPT_TIMEOUT, 600L);
        }
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, DOUYIN_UA);
        curl_easy_setopt(c, CURLOPT_REFERER, "https://www.douyin.com/");
        if (!m_cookie.isEmpty()) {
            curl_easy_setopt(c, CURLOPT_COOKIE, m_cookie.constData());
        }
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, this);
        QByteArray range;
        if (m_rangeStart >= 0) {
            range = QString("%1-%2").arg(m_rangeStart)
                        .arg(m_rangeEnd >= 0 ? QString::number(m_rangeEnd) : QString()).toLatin1();
        } else if (m_tag.startsWith("__dyvid__")) {
            // 【1.0.0.44】视频下载补 Range: 0-。真机实证（1.0.0.43 终验）：
            // 无 Range 的整文件 GET 被 CDN 302 跳到 IPv6 字面量（BB10 无 IPv6
            // 出口 → curl code=7）；同一 URL 带 Range（播放代理同款）直接 206。
            // "有的视频下载不下来"的根因，按播放路径修。
            range = QByteArray("0-");
        }
        if (!range.isEmpty()) {
            curl_easy_setopt(c, CURLOPT_RANGE, range.constData());
        }
        m_code = curl_easy_perform(c);
        // 【1.0.0.30】HTTP 码单独取（Range 206 才算真命中 CDN）。
        {
            long rc = 0;
            if (curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &rc) == CURLE_OK) {
                m_httpCode = (int)rc;
            }
        }
        curl_easy_cleanup(c);
        fclose(m_file);
        m_file = 0;
        if (m_code == CURLE_OK) {
            QFile::remove(m_path);
            if (!QFile::rename(tmpPath, m_path)) {
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
        DyCurlDownloadWorker* self = static_cast<DyCurlDownloadWorker*>(userdata);
        if (!self->m_file) return 0;
        const size_t n = fwrite(ptr, 1, size * nmemb, self->m_file);
        self->m_written += (qint64)n;
        return n;
    }

    QByteArray m_url;
    QString m_path;
    QString m_tag;
    int m_code;
    int m_httpCode;
    volatile qint64 m_written;
    FILE* m_file;
    qint64 m_rangeStart;
    qint64 m_rangeEnd;
    QByteArray m_cookie;
    bool m_queued;
};

static QString curlErrText(int c)
{
    switch (c) {
    case 0: return "OK";
    case 6: return "NO_HOST";
    case 7: return "CONN_FAIL";
    case 18: return "PARTIAL_FILE";
    case 28: return "TIMEOUT";
    case 35: return "SSL_HANDSHAKE";
    case 60: return "CERT_PROBLEM";
    case -100: return "NO_CURL_INIT";
    default: return QString::number(c);
    }
}

/**
 * 【1.0.0.30】网络层日志（格式/目录与 ApplicationUI::bootLog 完全一致）。
 * 1.0.0.28 真机「feed 三连空」只有 dyFeedModel.load() issued 一行、23 秒零回复，
 * 既没有 curl 错误码也没有 HTTP 码，无法区分断网/限流/解析空 —— 空等 4 次重启后
 * 补上这一层：每次请求结束必留一行，一次重启即可定性。
 */
void dyNetLog(const QString& s)
{
    const QString line = QDateTime::currentDateTime().toString("hh:mm:ss.zzz") + "  " + s;
    QStringList dirs;
    dirs << QDir::tempPath()   // 应用私有 tmp（ terminate 后会被清空）
         << QDir::homePath();  // 沙箱根，持久
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

DouyinApi* DouyinApi::instance()
{
    static DouyinApi* s = 0;
    if (!s) {
        s = new DouyinApi();
    }
    return s;
}

void DouyinApi::create(QObject* parent)
{
    DouyinApi* s = instance();
    if (parent && s->parent() != parent) {
        s->setParent(parent);
    }
}

DouyinApi::DouyinApi(QObject* parent)
    : QObject(parent)
    , m_keysReady(false)
    , m_networkStarted(false)
    , m_diagHttpsCode(-99)
    , m_diagHttpCode(-99)
    , m_bizReqCount(0)
    , m_bizFinCount(0)
    , m_coverVersion(0)
    , m_autoPlayTrigger(false)
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
    m_diagResult = "diag:idle";
    m_lastLoginVerify = "verify:idle";
    loadCookie();
}

void DouyinApi::startNetwork()
{
    if (m_networkStarted) return;
    m_networkStarted = true;

    CURLcode ginit = curl_global_init(CURL_GLOBAL_ALL);
    if (ginit != CURLE_OK) {
        m_diagResult = QString("diag:curl_init_fail(%1)").arg((int)ginit);
        qWarning("DouyinBar DouyinApi::startNetwork curl_global_init failed %d", (int)ginit);
        emit navFailed();
        return;
    }

    m_diagResult = "diag:run";
    // 【1.0.0.30 诊断】curl 版本人话：证明 curl 符号真的解析到了真库
    //（.so 无 libcurl 依赖，靠进程全局域解析；若这里打出非空版本串，
    // 符号解析就没问题，-99 只能是库内行为）。
    dyNetLog(QString("net curl ver=%1").arg(QString::fromUtf8(curl_version())));
    // 【2026-11-06 设备实证】api.douyin.com 不可达，diag 恒报 SSL_HANDSHAKE FAIL。
    // 改成 curl 实测可达的 v1/feed（200 JSON，免签名），diag OK 就等于业务通道可用。
    DyCurlWorker* d1 = new DyCurlWorker(
        "https://aweme.snssdk.com/aweme/v1/feed/?count=1&feed_style=0"
        "&device_platform=android&version_code=290200",
        true, true, QString::fromLatin1(kDiagTag));
    m_diagJobs.insert(d1, 1);
    QObject::connect(d1, SIGNAL(finished()), this, SLOT(onDiagFinished()));
    // 【1.0.0.30 诊断】start() 返回值：false = 线程根本没起（run() 不会执行）。
    d1->start();

    // 抖音签名密钥/配预取（若有对应的 nav/device 接口，这里填入；目前保留占位）
    // qDebug("DouyinBar DouyinApi::startNetwork done cookie=%d", m_cookie.size());
}

QString DouyinApi::diagResult() const
{
    return m_diagResult;
}

QString DouyinApi::bizDiag() const
{
    return QString("req%1 fin%2 imgok%3 imgfail%4 last=%5")
            .arg(m_bizReqCount).arg(m_bizFinCount)
            .arg(m_imgOk).arg(m_imgFail).arg(m_lastBiz);
}

void DouyinApi::onDiagFinished()
{
    DyCurlWorker* w = static_cast<DyCurlWorker*>(sender());
    if (!w) return;
    int tag = m_diagJobs.value(w, 0);
    m_diagJobs.remove(w);
    if (tag == 1) {
        m_diagHttpsCode = w->code();
        m_diagHttpsOut = w->result();
    }
    w->deleteLater();

    if (m_diagJobs.isEmpty()) {
        QString https = (m_diagHttpsCode == CURLE_OK && !m_diagHttpsOut.trimmed().isEmpty())
                        ? "HTTPS OK"
                        : QString("HTTPS FAIL %1").arg(curlErrText(m_diagHttpsCode));
        m_diagResult = "diag " + https;
        qDebug("DouyinBar DouyinApi::onDiagFinished -> %s", qPrintable(m_diagResult));
        // 【1.0.0.30】设备取不到 slog2，qDebug 等于不存在；verdict 必须进 boot log。
        // http 码同样单独取：curl code==OK 也可能是 403/429（限流），
        // code+http+bytes 三个值同时在日志里才能一次定性。
        dyNetLog(QString("net diag verdict %1 code=%2 http=%3 bytes=%4")
                     .arg(m_diagResult)
                     .arg(m_diagHttpsCode)
                     .arg(w ? w->httpCode() : -99)
                     .arg(m_diagHttpsOut.size()));
    }
}

void DouyinApi::get(const QString& url, const QString& tag, const QString& ua)
{
    request(url, tag, ua);
}

void DouyinApi::post(const QString& url, const QString& body, const QString& tag)
{
    if (!m_networkStarted) {
        startNetwork();
    }
    qDebug("DouyinBar DouyinApi::post tag=%s body=%s", qPrintable(tag), qPrintable(body));
    m_bizReqCount++;
    DyCurlWorker* w = new DyCurlWorker(url.toUtf8(), false, true, tag, QByteArray(),
                                   m_cookie.toUtf8(), false, body.toUtf8());
    QObject::connect(w, SIGNAL(finished()), this, SLOT(onCurlFinished()));
    w->start();
}

/**
 * 【1.0.0.29】JSON POST（Content-Type: application/json）。
 * 评论发表 /aweme/v1/comment/publish/ 只认 JSON body —— curl 实证：
 * form-urlencoded 形态返回 "Url does not match"（走不到鉴权层），
 * JSON 形态到达鉴权层返回 {"status_code":8,"status_msg":"用户未登录"}。
 * 行为与 post() 完全一致（同一条 DyCurlWorker 线程链 + netReply 广播）。
 */
void DouyinApi::postJson(const QString& url, const QString& json, const QString& tag)
{
    if (!m_networkStarted) {
        startNetwork();
    }
    qDebug("DouyinBar DouyinApi::postJson tag=%s json=%s", qPrintable(tag), qPrintable(json));
    m_bizReqCount++;
    DyCurlWorker* w = new DyCurlWorker(url.toUtf8(), false, true, tag, QByteArray(),
                                   m_cookie.toUtf8(), false, json.toUtf8(),
                                   QByteArray("application/json"));
    QObject::connect(w, SIGNAL(finished()), this, SLOT(onCurlFinished()));
    w->start();
}

void DouyinApi::launchCurl(const QString& url, const QString& tag, const QString& ua, bool cookieJar)
{
    if (!m_networkStarted) {
        startNetwork();
    }
    qDebug("DouyinBar DouyinApi::launchCurl tag=%s jar=%d %s", qPrintable(tag), cookieJar ? 1 : 0,
           qPrintable(url));
    m_bizReqCount++;
    const QByteArray ck = cookieJar ? QByteArray() : m_cookie.toUtf8();
    DyCurlWorker* w = new DyCurlWorker(url.toUtf8(), false, true, tag, ua.toUtf8(), ck, cookieJar);
    QObject::connect(w, SIGNAL(finished()), this, SLOT(onCurlFinished()));
    w->start();
}

/**
 * 统一的业务 GET 入口。
 *
 * 声明在 douyin_api.hpp 里、曾经**没有实现** —— 共享库是懒绑定：头文件有声明、
 * .cpp 漏定义的函数链接期不报错，设备上 dlopen 时动态链接器解析失败，
 * 表现是**应用开屏即闪退**（BiliBar 第 18 轮在同一工程栽过一次：DouyinApi::request、
 * downloadPercent、downloadWritten 三个符号缺失，装机后完全打不开）。
 * 现在按铁律在自检里加 nm 扫描。
 */
void DouyinApi::request(const QString& url, const QString& tag, const QString& ua,
                        bool cookieJar)
{
    launchCurl(url, tag, ua, cookieJar);
}

int DouyinApi::downloadPercent(const QString& tag) const
{
    // 100 只由 finished 给出，避免进度条假满（与 BiliBar 同款语义）。
    DyCurlDownloadWorker* w = m_dlJobs.value(tag, 0);
    if (!w) return 100;
    const qint64 total = m_dlTotal.value(tag, 0);
    if (total <= 0) return 0;
    const qint64 done = w->written();
    int p = (int)(done * 100 / total);
    if (p < 0) p = 0;
    if (p > 99) p = 99;   // 100 只由 finished 给出
    return p;
}

qint64 DouyinApi::downloadWritten(const QString& tag) const
{
    DyCurlDownloadWorker* w = m_dlJobs.value(tag, 0);
    return w ? w->written() : 0;
}

void DouyinApi::download(const QString& url, const QString& destPath, const QString& tag,
                         qint64 expectedBytes, qint64 rangeStart, qint64 rangeEnd, bool queued,
                         int prio)
{
    if (!m_networkStarted) {
        startNetwork();
    }
    if (queued) {
        if (m_dlPathTags.contains(destPath)) {
            QStringList& tags = m_dlPathTags[destPath];
            if (!tags.contains(tag)) tags.append(tag);
            qDebug("DouyinBar cover dedup path=%s tag=%s", qPrintable(destPath), qPrintable(tag));
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
        p.row = -1;
        p.retry = 0;
        m_dlQueue.append(p);
        pumpDownloads();
        return;
    }
    startDownload(url, destPath, tag, expectedBytes, rangeStart, rangeEnd, false);
}

/** =========================== 抖音签名插槽（⚠️ 必须填入真实签名算法） ===========================
 *
 * 抖音的登录/视频流/播放地址接口都需要签名（_signature / X-Bogus / msToken 等）。
 * 这里只返回原 URL（未签名），所有带签名的接口会返回 403。
 *
 * 你需要：
 *  1. 在真机/模拟器上抓包抖音 App，拿到完整的签名请求（URL + 请求头 + 查询参数）。
 *  2. 把签名生成逻辑（通常是 JS 加密，也可能是本地 so 调用）用 C++/OpenSSL/QCryptographicHash
 *     复现，填进本函数。
 *  3. 把签名参数拼进 URL 返回。
 *
 * 签名算法不正确时，现象为：登录接口返回错误码、视频流返回空列表、播放地址为空。
 * ========================================================================================== */
QString DouyinApi::signRequest(const QString& url, const QVariantMap& params)
{
    Q_UNUSED(params)
    // ⚠️ TODO：在此处实现抖音签名（_signature / X-Bogus / msToken / as / cp 等）。
    // 示例（伪代码，不可直接用）：
    //   QString query = buildQuery(params);
    //   QString sig = computeXBogus(url + "?" + query, m_deviceParams);
    //   return url + "?" + query + "&_signature=" + percentEncode(sig);
    return url;
}

void DouyinApi::getSigned(const QString& path, const QVariantMap& params, const QString& tag)
{
    if (!m_networkStarted) {
        startNetwork();
    }
    QString base = apiBase();
    if (!base.endsWith("/")) base += "/";
    QString p = path;
    if (p.startsWith("/")) p = p.mid(1);
    QString url = base + p;

    // 拼接查询参数
    QStringList kv;
    QVariantMap::const_iterator it;
    for (it = params.constBegin(); it != params.constEnd(); ++it) {
        kv << percentEncode(it.key()) + QLatin1String("=") + percentEncode(it.value().toString());
    }
    if (!kv.isEmpty()) {
        url += QLatin1String("?") + kv.join(QLatin1String("&"));
    }

    // ⚠️ 签名：签名算法实现后，这里会在 URL 上追加 _signature / X-Bogus 等。
    url = signRequest(url, params);

    qDebug("DouyinBar DouyinApi::getSigned tag=%s url=%s", qPrintable(tag), qPrintable(url));
    m_bizReqCount++;
    launchCurl(url, tag, QString(), false);
}

// ============================== 登录（Cookie 粘贴 + self 校验） ==============================
//
// 【为什么不是扫码】历史实测抖音扫码接口返回 error_code 16，且需要完整签名才能取到
// 二维码。本工程的 v1/feed 家族免签名可用，self 接口同样免签名，因此按 curl 实证
// 结论走「粘贴 Cookie → /aweme/v1/user/profile/self/ 校验」这条真正能闭环的路径。
//
// 【协议证据（2026-11-06 host curl）】
//   GET https://aweme.snssdk.com/aweme/v1/user/profile/self/?<公共参数>
//   匿名           -> 200 application/json  {"status_code":8,"user":null,...}  (227 B)
//   Cookie: sessionid=fake... -> 同上，status_code=8
//   判定：status_code==0 且 user 非空 => 登录有效；否则一律视为未登录。
// =========================================================================================

void DouyinApi::loginStart()
{
    // 兼容旧 UI 入口：明确告诉用户扫码不可用，引导到 Cookie 输入框。
    setLoginState(0, QString("抖音扫码登录不可用（实测 error_code 16）。请在下方粘贴 Cookie 后点「验证并登录」。"));
}

void DouyinApi::loginCancel()
{
    stopPoll();
}

void DouyinApi::logout()
{
    stopPoll();
    m_cookie.clear();
    m_csrf.clear();
    m_loggedIn = false;
    m_userName.clear();
    m_userFace.clear();
    m_userFaceRemote.clear();
    m_userMid.clear();
    m_loginState = 0;
    m_loginTip = QString("已退出登录");
    m_qrUrl.clear();
    m_qrKey.clear();
    m_qrPath.clear();
    m_lastLoginVerify = "verify:logged_out";
    saveCookie();
    emit userChanged();
    emit loginChanged();
}

void DouyinApi::onLoginQrReply(const QString& json, bool ok)
{
    Q_UNUSED(json)
    Q_UNUSED(ok)
    // 扫码通道已停用，保留函数体避免头文件声明变成懒绑定缺失符号。
}

void DouyinApi::onLoginPollReply(const QString& json, bool ok, const QString& cookies)
{
    Q_UNUSED(json)
    Q_UNUSED(ok)
    Q_UNUSED(cookies)
    // 扫码通道已停用，保留函数体避免头文件声明变成懒绑定缺失符号。
}

void DouyinApi::setLoginState(int st, const QString& tip)
{
    m_loginState = st;
    m_loginTip = tip;
    emit loginChanged();
}

void DouyinApi::stopPoll()
{
    if (m_pollTimer) {
        m_pollTimer->stop();
        m_pollBusy = false;
    }
}

QString DouyinApi::normalizeCookie(const QString& raw)
{
    QString s = raw.trimmed();
    if (s.startsWith(QLatin1String("Cookie:"), Qt::CaseInsensitive)) {
        s = s.mid(7).trimmed();
    }
    s.remove(QLatin1Char('\r'));
    s.remove(QLatin1Char('\n'));
    s = s.trimmed();
    if (s.isEmpty()) return s;
    // 用户只粘了裸 sessionid（32 位十六进制）时自动补全键名。
    if (!s.contains(QLatin1Char('='))) {
        s = QLatin1String("sessionid=") + s;
    }
    return s;
}

int DouyinApi::parseSelfProfile(const QString& json, bool* valid,
                                QString* nick, QString* uid, QString* face)
{
    if (valid) *valid = false;
    if (nick) nick->clear();
    if (uid) uid->clear();
    if (face) face->clear();
    if (json.trimmed().isEmpty()) return -1;

    JsonDataAccess jda;
    const QVariant v = jda.loadFromBuffer(json.toUtf8());
    if (!v.isValid()) return -2;
    const QVariantMap root = v.toMap();
    if (root.isEmpty() || !root.contains(QLatin1String("status_code"))) return -2;

    const int status = root.value(QLatin1String("status_code")).toInt();
    const QVariantMap user = root.value(QLatin1String("user")).toMap();
    if (status != 0 || user.isEmpty()) return status;

    if (valid) *valid = true;
    if (nick) *nick = user.value(QLatin1String("nickname")).toString();
    if (uid) {
        *uid = user.value(QLatin1String("uid")).toString();
        if (uid->isEmpty()) *uid = user.value(QLatin1String("short_id")).toString();
    }
    if (face) {
        const char* const keys[] = {"avatar_larger", "avatar_medium", "avatar_thumb"};
        for (int i = 0; i < 3; ++i) {
            const QVariantMap a = user.value(QLatin1String(keys[i])).toMap();
            const QVariantList lst = a.value(QLatin1String("url_list")).toList();
            if (!lst.isEmpty() && !lst.at(0).toString().isEmpty()) {
                *face = lst.at(0).toString();
                break;
            }
        }
    }
    return status;
}

QString DouyinApi::selfParserCheck() const
{
    // anon 是设备/host 实测抓回来的真实响应；ok 是按抖音 self 结构构造的成功样本。
    // 两段固定输入跑同一个解析器，任何解析回归都会在启动日志里立刻显形。
    static const char* const kAnon =
        "{\"extra\":{\"logid\":\"20261106121307E20BF21E9C996D4D454A\"},"
        "\"status_code\":8,\"status_msg\":null,\"user\":null}";
    static const char* const kOk =
        "{\"status_code\":0,\"user\":{\"nickname\":\"BB10Test\",\"uid\":\"123456789\","
        "\"avatar_larger\":{\"url_list\":[\"https://p.douyin.com/a.webp\"]}}}";
    bool v1 = true, v2 = false;
    QString n1, u1, f1, n2, u2, f2;
    const int s1 = parseSelfProfile(QString::fromLatin1(kAnon), &v1, &n1, &u1, &f1);
    const int s2 = parseSelfProfile(QString::fromLatin1(kOk), &v2, &n2, &u2, &f2);
    const bool pass = (s1 == 8 && !v1 && s2 == 0 && v2 &&
                       n2 == QLatin1String("BB10Test") &&
                       u2 == QLatin1String("123456789") &&
                       f2.endsWith(QLatin1String(".webp")));
    return QString("selfParser %1 [anon status=%2 valid=%3 | ok status=%4 valid=%5 nick=%6 uid=%7 face=%8]")
            .arg(pass ? QLatin1String("PASS") : QLatin1String("FAIL"))
            .arg(s1).arg(v1 ? 1 : 0).arg(s2).arg(v2 ? 1 : 0)
            .arg(n2).arg(u2).arg(f2.isEmpty() ? QLatin1String("empty") : QLatin1String("ok"));
}

void DouyinApi::verifyCookie(const QString& cookie, bool userInitiated)
{
    if (cookie.isEmpty()) {
        setLoginState(5, QString("Cookie 为空，请先粘贴抖音 Cookie。"));
        return;
    }
    if (!m_networkStarted) {
        startNetwork();
    }
    setLoginState(1, QString("正在校验登录态（Cookie %1 字节）…").arg(cookie.size()));

    const QString url = apiBase() + userProfileApi() + QLatin1String("?")
                        + QLatin1String(kSelfQuery);
    m_bizReqCount++;
    // 独立 slot 回调：不经过 emit netReply 广播，避免 __dyloginverify__ 这个未知 tag
    // 落进各 Store 的 onNetReply 里被误判成业务响应。
    DyCurlWorker* w = new DyCurlWorker(url.toUtf8(), false, true,
                                   QString::fromLatin1(kLoginVerifyTag),
                                   QByteArray(), cookie.toUtf8(), false);
    m_loginJobs.insert(w, userInitiated);
    QObject::connect(w, SIGNAL(finished()), this, SLOT(onLoginVerifyFinished()));
    qDebug("DouyinBar login verify issued len=%d userInitiated=%d url=%s",
           cookie.size(), userInitiated ? 1 : 0, qPrintable(url));
    w->start();
}

void DouyinApi::loginWithCookie(const QString& raw)
{
    const QString c = normalizeCookie(raw);
    if (c.isEmpty()) {
        setLoginState(5, QString("Cookie 为空，请先粘贴抖音 Cookie。"));
        return;
    }
    // 先放进内存供 curl 使用；只有 self 校验通过才 saveCookie() 落盘。
    m_cookie = c;
    verifyCookie(c, true);
}

void DouyinApi::refreshUser()
{
    if (m_cookie.isEmpty()) return;
    verifyCookie(m_cookie, false);
}

void DouyinApi::onLoginVerifyFinished()
{
    DyCurlWorker* w = static_cast<DyCurlWorker*>(sender());
    if (!w) return;
    const bool userInitiated = m_loginJobs.value(w, false);
    m_loginJobs.remove(w);
    const int code = w->code();
    const QString json = w->result();
    w->deleteLater();

    if (code != CURLE_OK) {
        m_lastLoginVerify = QString("verify:NET_FAIL(%1)").arg(code);
        qWarning("DouyinBar login verify net fail %s", qPrintable(m_lastLoginVerify));
        // 网络故障不等于 Cookie 失效：保留凭证，下次冷启动会自动重试。
        setLoginState(5, QString("网络不可达，未能校验（curl %1）。Cookie 已暂存，可稍后重试。").arg(code));
        return;
    }

    bool valid = false;
    QString nick, uid, face;
    const int status = parseSelfProfile(json, &valid, &nick, &uid, &face);
    m_lastLoginVerify = QString("verify:status=%1 valid=%2 bytes=%3")
                            .arg(status).arg(valid ? 1 : 0).arg(json.size());
    qDebug("DouyinBar login verify result %s", qPrintable(m_lastLoginVerify));

    if (valid) {
        m_loggedIn = true;
        m_userName = nick;
        m_userMid = uid;
        m_userFaceRemote = face;
        m_userFace.clear();
        m_csrf.clear();
        const QStringList parts = m_cookie.split(QLatin1Char(';'));
        for (int i = 0; i < parts.size(); ++i) {
            const QString p = parts.at(i).trimmed();
            if (p.startsWith(QLatin1String("csrf_session_id="))) m_csrf = p.mid(16);
            else if (p.startsWith(QLatin1String("csrftoken="))) m_csrf = p.mid(10);
        }
        saveCookie();
        setLoginState(3, QString("登录成功：%1").arg(nick.isEmpty() ? uid : nick));
        emit userChanged();
        emit loginChanged();
        qDebug("DouyinBar login OK nick=%s uid=%s face=%d",
               qPrintable(nick), qPrintable(uid), face.size());
        if (!face.isEmpty()) {
            const QString local = faceCachePath(face);
            if (!isUsableImage(local, 512)) {
                download(face, local, QString::fromLatin1("__dyface__"), 0, -1, -1, false, 0);
            }
        }
    } else {
        // 服务端已明确判定凭证无效：清掉，绝不把失效 Cookie 当成登录态显示。
        m_loggedIn = false;
        m_cookie.clear();
        m_csrf.clear();
        m_userName.clear();
        m_userMid.clear();
        m_userFace.clear();
        m_userFaceRemote.clear();
        saveCookie();
        QString tip;
        if (status == 8) {
            tip = userInitiated
                  ? QString("Cookie 无效或已过期（status_code=8），请重新从抖音获取。")
                  : QString("已保存的登录态失效（status_code=8），请重新粘贴 Cookie。");
        } else if (status < 0) {
            tip = QString("登录校验响应无法解析（parse=%1），请检查网络后重试。").arg(status);
        } else {
            tip = QString("服务端拒绝（status_code=%1），请重新获取 Cookie。").arg(status);
        }
        setLoginState(5, tip);
        emit userChanged();
        emit loginChanged();
    }
}

void DouyinApi::loadCookie()
{
    const QString p = cookiePath();
    // 兼容历史路径做一次性迁移：1.0.0.8 写在 data/data/，更早的写在 app/native/。
    QStringList legacyPaths;
    legacyPaths << (QDir::homePath() + QLatin1String("/data/douyin_cookie.txt"))
                << (QDir::homePath() + QLatin1String("/app/native/douyin_cookie.txt"));
    QFile f(p);
    if (!f.exists()) {
        for (int i = 0; i < legacyPaths.size(); ++i) {
            QFile old(legacyPaths.at(i));
            if (!old.exists()) continue;
            if (old.open(QIODevice::ReadOnly) &&
                f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(old.readAll());
                f.close();
                old.close();
                qDebug("DouyinBar cookie migrated from %s",
                       qPrintable(legacyPaths.at(i)));
                break;
            }
            old.close();
        }
    }
    if (!f.open(QIODevice::ReadOnly)) return;
    m_cookie = QString::fromUtf8(f.readAll()).trimmed();
    f.close();
    const QStringList parts = m_cookie.split(QLatin1Char(';'));
    for (int i = 0; i < parts.size(); ++i) {
        const QString kv = parts.at(i).trimmed();
        if (kv.startsWith(QLatin1String("csrf_session_id="))) m_csrf = kv.mid(16);
        else if (kv.startsWith(QLatin1String("csrftoken="))) m_csrf = kv.mid(10);
    }
    if (!m_cookie.isEmpty()) {
        // 【关键】有本地 cookie 不等于登录有效。构造函数阶段 curl 还没初始化，
        // 这里只置「待校验」，真正的 self 校验由 ApplicationUI::startNetworkDeferred()
        // 在场景 set 完、startNetwork() 之后触发。
        m_loginState = 1;
        m_loginTip = QString("正在校验已保存的登录态…");
        qDebug("DouyinBar cookie loaded len=%d path=%s", m_cookie.size(), qPrintable(p));
    }
}

void DouyinApi::saveCookie()
{
    const QString p = cookiePath();
    QFile f(p);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(m_cookie.toUtf8());
        f.close();
        qDebug("DouyinBar cookie saved bytes=%d path=%s", m_cookie.size(), qPrintable(p));
    } else {
        qWarning("DouyinBar cookie SAVE FAILED path=%s", qPrintable(p));
    }
}

void DouyinApi::setCookie(const QString& c)
{
    m_cookie = c;
    saveCookie();
    m_loggedIn = !c.isEmpty();
    emit userChanged();
    emit loginChanged();
}

QString DouyinApi::cookieDiag() const
{
    const QString p = cookiePath();
    const QFileInfo fi(p);
    // 只读检查：1.0.0.8 用 open(Append) 探测可写性会凭空创建 0 字节文件，
    // 让启动日志出现 exists=1 size=0，是诊断函数自身的副作用，已消除。
    const bool writable = QFileInfo(fi.absolutePath()).isWritable();
    return QString("path=%1 exists=%2 size=%3 writable=%4 cookieLen=%5 %6")
            .arg(p).arg(fi.exists() ? 1 : 0).arg(fi.size())
            .arg(writable ? 1 : 0).arg(m_cookie.size()).arg(m_lastLoginVerify);
}

// ============================== 封面/头像缓存（与 BiliBar 同一套机制） ==============================

void DouyinApi::fireAutoPlay()
{
    if (m_autoPlayTrigger) return;
    m_autoPlayTrigger = true;
    emit autoPlayTriggerChanged();
    qDebug("DouyinBar autoPlayTrigger fired");
}

QString DouyinApi::coverUrlFor(const QString& raw, int ver)
{
    Q_UNUSED(ver)
    QString local = coverCachePath(raw);
    return readyImageFor(local, 1024);
}

QString DouyinApi::faceUrlFor(const QString& raw, int ver)
{
    Q_UNUSED(ver)
    QString local = faceCachePath(raw);
    return readyImageFor(local, 512);
}

QString DouyinApi::percentEncode(const QString& s)
{
    QByteArray ba = s.toUtf8();
    QByteArray out;
    out.reserve(ba.size());
    for (int i = 0; i < ba.size(); ++i) {
        char ch = ba.at(i);
        if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out.append(ch);
        } else {
            out += QString::fromLatin1("%%1").arg((uchar)ch, 2, 16, QLatin1Char('0')).toUpper().toUtf8();
        }
    }
    return QString::fromUtf8(out);
}

QString DouyinApi::countText(qint64 n)
{
    if (n >= 100000000) return QString::number((double)n / 100000000, 'f', 1) + QLatin1String("亿");
    if (n >= 10000) return QString::number((double)n / 10000, 'f', 1) + QLatin1String("万");
    return QString::number(n);
}

QString DouyinApi::durationText(long long sec)
{
    if (sec < 0) return QString();
    long long m = sec / 60, s = sec % 60;
    return QString("%1:%2").arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
}

QString DouyinApi::timeAgo(long long unix)
{
    if (unix <= 0) return QString();
    const qint64 now = QDateTime::currentDateTime().toTime_t();
    qint64 diff = now - unix;
    if (diff < 0) diff = 0;
    if (diff < 60) return QString::fromUtf8("刚刚");
    if (diff < 3600) return QString::fromUtf8("%1 分钟前").arg(diff / 60);
    if (diff < 86400) return QString::fromUtf8("%1 小时前").arg(diff / 3600);
    if (diff < 30 * 86400LL) return QString::fromUtf8("%1 天前").arg(diff / 86400);
    return QString::fromUtf8("%1 个月前").arg(diff / (30 * 86400LL));
}

QString DouyinApi::coverUrl(const QString& raw)
{
    // 抖音封面通常带 ?images/... 或无后缀，这里统一返回原 URL（缓存路径由 md5 决定）
    return raw;
}

namespace {
/** 只看 ? 之前的 path 部分判断扩展名（抖音 URL 的 query 里带 .webp 之类的干扰很少，但必须隔离）。 */
bool isJpegImageUrl(const QString& u)
{
    QString path = u;
    const int q = path.indexOf(QLatin1Char('?'));
    if (q >= 0) path.truncate(q);
    const int sharp = path.indexOf(QLatin1Char('#'));
    if (sharp >= 0) path.truncate(sharp);
    const QString p = path.toLower();
    return p.endsWith(QLatin1String(".jpeg")) || p.endsWith(QLatin1String(".jpg"));
}
} // namespace

QString DouyinApi::pickJpegUrl(const QVariantList& urls)
{
    // 【1.0.0.15 真机取证】feed_now.json 里 6/6 条 cover.url_list 都是
    // [.webp, .webp, .webp, .jpeg]，第 4 个才是真 JPEG；.jpeg 变体 curl 实测
    // HTTP=200 Content-Type=image/jpeg，magic=FFD8FFE0...JFIF，尾部 FFD9。
    // 并列候选中优先 https（CDN 签名与协议绑定，不能自己改后缀拼 URL）。
    QString httpsHit, httpHit;
    for (int i = 0; i < urls.size(); ++i) {
        const QString u = urls.at(i).toString();
        if (u.isEmpty() || !isJpegImageUrl(u)) continue;
        if (u.startsWith(QLatin1String("https://"))) {
            if (httpsHit.isEmpty()) httpsHit = u;
        } else if (u.startsWith(QLatin1String("http://"))) {
            if (httpHit.isEmpty()) httpHit = u;
        }
    }
    if (!httpsHit.isEmpty()) return httpsHit;
    return httpHit;
}

QString DouyinApi::pickCoverUrl(const QVariantMap& video)
{
    static const char* const kKeys[] = { "cover", "origin_cover", "dynamic_cover" };
    for (int k = 0; k < 3; ++k) {
        const QVariantList lst = video.value(QLatin1String(kKeys[k])).toMap()
                                     .value(QLatin1String("url_list")).toList();
        const QString u = pickJpegUrl(lst);
        if (!u.isEmpty()) return u;
    }
    // 一条都没有就返回空：宁可这行没封面，也不要把 WebP 伪装成 .jpg 塞给 ImageView。
    return QString();
}

QString DouyinApi::imageFormatOf(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QLatin1String("MISSING");
    const QByteArray h = f.read(16);
    f.close();
    if (h.size() >= 2 && (uchar)h.at(0) == 0xFF && (uchar)h.at(1) == 0xD8)
        return QLatin1String("JPEG");
    if (h.startsWith("\x89PNG\r\n\x1a\n")) return QLatin1String("PNG");
    if (h.startsWith("RIFF") && h.size() >= 12 && h.mid(8, 4) == "WEBP")
        return QLatin1String("WEBP");
    if (h.startsWith("GIF87a") || h.startsWith("GIF89a")) return QLatin1String("GIF");
    return QLatin1String("OTHER");
}

QString DouyinApi::coverCachePath(const QString& raw)
{
    QString dir = QDir::homePath() + QLatin1String("/app/native/images/");
    QDir().mkpath(dir);
    QString hash = QString(QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Md5).toHex());
    return dir + hash + QLatin1String(".jpg");
}

QString DouyinApi::faceCachePath(const QString& raw)
{
    QString dir = QDir::homePath() + QLatin1String("/app/native/images/");
    QDir().mkpath(dir);
    QString hash = QString(QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Md5).toHex());
    return dir + hash + QLatin1String("_face.jpg");
}

/**
 * 【1.0.0.49】封面/头像缓存清扫：md5 jpg 只增不减（1.0.0.15 封面管线起
 * 就没有回收路径）。启动期调用一次，按 mtime 删到上限内。
 * 只扫 *.jpg（封面 md5.jpg + 头像 md5_face.jpg），不碰别的资产。
 */
int DouyinApi::sweepImageCaches(int maxFiles, qint64 maxBytes)
{
    const QString dir = QDir::homePath() + QLatin1String("/app/native/images/");
    QDir d(dir);
    if (!d.exists()) return 0;
    const QStringList files = d.entryList(QStringList() << "*.jpg", QDir::Files);
    // 按 mtime 升序收集（NDK C++98：QMultiMap 免 lambda/std::sort 依赖）。
    QMultiMap<QDateTime, QFileInfo> byTime;
    qint64 total = 0;
    for (int i = 0; i < files.size(); ++i) {
        const QFileInfo fi(d.absoluteFilePath(files.at(i)));
        byTime.insert(fi.lastModified(), fi);
        total += fi.size();
    }
    if (files.size() <= maxFiles && total <= maxBytes) return 0;

    int removed = 0;
    int kept = files.size();
    QMultiMap<QDateTime, QFileInfo>::const_iterator it = byTime.constBegin();
    while (it != byTime.constEnd() && (kept > maxFiles || total > maxBytes)) {
        if (QFile::remove(it.value().absoluteFilePath())) {
            total -= it.value().size();
            ++removed;
            --kept;
        }
        ++it;
    }
    if (removed > 0) {
        qDebug("DouyinBar imgcache sweep removed=%d kept=%d bytes=%lld",
               removed, kept, (long long)total);
    }
    return removed;
}

QString DouyinApi::cookiePath() const
{
    // 【设备实证 1.0.0.8】QDir::homePath() 本身就已经是沙盒可写目录
    // /accounts/1000/appdata/<pkg>/data（LocalDb 的 douyin.db 就在这层）。
    // 1.0.0.8 又拼了一层 /data/，得到 .../data/data/douyin_cookie.txt，
    // 虽然可写且验证通过，但路径语义重复，这里归一到 homePath() 根下。
    const QString dir = QDir::homePath();
    QDir().mkpath(dir);
    return dir + QLatin1String("/douyin_cookie.txt");
}

bool DouyinApi::isImageFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QByteArray head = f.read(16);
    f.close();
    if (head.size() < 2) return false;
    if (head.at(0) == (char)0xFF && head.at(1) == (char)0xD8) return true;   // JPEG
    if (head.startsWith("\x89PNG\r\n\x1a\n")) return true;                    // PNG
    if (head.startsWith("RIFF") && head.size() >= 12 &&
        head.mid(8, 4) == "WEBP") return true;                                // WEBP
    if (head.startsWith("GIF87a") || head.startsWith("GIF89a")) return true;  // GIF
    return false;
}

bool DouyinApi::isUsableImage(const QString& path, qint64 minSize)
{
    QFileInfo fi(path);
    if (!fi.exists() || fi.size() < minSize) return false;

    const QString fmt = imageFormatOf(path);

    // 【1.0.0.15 修复核心】缓存文件一律叫 *.jpg，内容却可能是 WebP。
    // Cascades 1.1 的 ImageView 走扩展名/MIME 挑解码器，这种"挂羊头卖狗肉"
    // 的文件不会报错、也不会显示 —— 表现就是主页整屏空白且下载日志全是 code=0。
    // 所以扩展名是 .jpg/.jpeg 时，内容必须是真 JPEG，否则一律判坏。
    const QString lower = path.toLower();
    if ((lower.endsWith(QLatin1String(".jpg")) || lower.endsWith(QLatin1String(".jpeg")))
            && fmt != QLatin1String("JPEG")) {
        return false;
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray all = f.readAll();
    f.close();
    if (all.size() < 4) return false;

    if (fmt == QLatin1String("JPEG")) {
        // 头 FFD8 + 尾 FFD9：只看头会放过"下载到一半"的残图。
        return all.at(all.size()-2) == (char)0xFF && all.at(all.size()-1) == (char)0xD9;
    }
    if (fmt == QLatin1String("PNG")) {
        return all.size() >= 8 &&
               all.at(all.size()-4) == (char)0x49 && all.at(all.size()-3) == (char)0x45 &&
               all.at(all.size()-2) == (char)0x4E && all.at(all.size()-1) == (char)0x44;
    }
    if (fmt == QLatin1String("WEBP")) {
        // RIFF 头里的 length 字段（小端）应等于 filesize-8，否则是被截断的 WebP。
        if (all.size() < 12) return false;
        const quint32 declared = (quint32)(uchar)all.at(4)
                               | ((quint32)(uchar)all.at(5) << 8)
                               | ((quint32)(uchar)all.at(6) << 16)
                               | ((quint32)(uchar)all.at(7) << 24);
        return declared == (quint32)(all.size() - 8);
    }
    if (fmt == QLatin1String("GIF")) return all.at(all.size()-1) == (char)0x3B;
    return false;   // 未知格式不再默认放行（旧实现这里 return true，是本次事故的帮凶）
}

QString DouyinApi::apiBase()
{
    // 【2026-11-06 curl 实证】api.douyin.com 在本网络不可达（502 / 设备 SSL_HANDSHAKE FAIL）。
    // aweme.snssdk.com 的 App 协议 v1 家族免签名直连，返回 JSON。
    return QString("https://aweme.snssdk.com");
}

QString DouyinApi::feedApi()
{
    // 200 JSON aweme_list（含 play_addr.url_list / cover.url_list / statistics）。
    // v2/feed 只返 application/x-protobuf，JsonDataAccess 解不了，不能用。
    return QString("/aweme/v1/feed/");
}

QString DouyinApi::userProfileApi()
{
    // 2026-11-06 curl 实证：匿名/失效 cookie 均 200 JSON status_code=8 user=null。
    // 免签名、免 _signature，是本工程唯一可闭环的登录态判定通道。
    return QString("/aweme/v1/user/profile/self/");
}

QString DouyinApi::itemInfoApi(const QString& awemeId)
{
    Q_UNUSED(awemeId)
    // 实测全路径 "Url does not match"，保留端点仅为兼容；播放走 feed 行内嵌的
    // play_addr.url_list[0]（DouyinPlayerStore::loadDirect），不依赖 iteminfo。
    return QString("/aweme/v1/aweme/iteminfo/");
}

QString DouyinApi::postListApi(const QString& secUid)
{
    Q_UNUSED(secUid)
    return QString("/aweme/v1/aweme/post/");   // 实测 200 JSON（匿名时 aweme_list 为空）
}

QString DouyinApi::commentListUrl(const QString& awemeId, int cursor)
{
    // 完整 URL：主机与 apiBase() 不同，调用方直接 get(url, tag)，不要拼 apiBase()。
    // cursor 首页传 0；之后传「最后一条评论的 cid 数值」——服务端目前忽略它，
    // 由 DouyinCommentModel 按 cid 去重兜底（见 douyin_store.hpp 注释）。
    return QString("https://www.iesdouyin.com/web/api/v2/comment/list/"
                   "?aweme_id=%1&cursor=%2&count=20")
            .arg(awemeId).arg(cursor);
}

QString DouyinApi::commentPublishUrl()
{
    // 【1.0.0.29】JSON 端点（host curl 实证，2026-11-07）：
    //   POST https://www.iesdouyin.com/aweme/v1/comment/publish/
    //   Content-Type: application/json
    //   body {"aweme_id":"<id>","text":"<内容>"}
    //   → 匿名 {"status_code":8,"status_msg":"用户未登录"}；带 Cookie 期望 status_code=0。
    return QString("https://www.iesdouyin.com/aweme/v1/comment/publish/");
}

QString DouyinApi::noteDetailApi(const QString& noteId)
{
    Q_UNUSED(noteId)
    return QString("/aweme/v1/aweme/iteminfo/");
}

QString DouyinApi::noteLikeApi(const QString& noteId)
{
    Q_UNUSED(noteId)
    return QString("/aweme/v1/commit/item/digg/");
}

QString DouyinApi::noteCollectApi(const QString& noteId)
{
    Q_UNUSED(noteId)
    return QString("/aweme/v1/aweme/collect/");
}

QString DouyinApi::userFollowApi(const QString& userId)
{
    Q_UNUSED(userId)
    return QString("/aweme/v1/user/follow/");
}

QString DouyinApi::noteFeedApi()
{
    return QString("/aweme/v1/feed/");
}

QString DouyinApi::followFeedApi()
{
    // 【1.0.0.24】同城/关注端点 host 探针实证（App UA + Referer 直连）：
    //   /aweme/v1/follow/feed/ → status_code=0 但匿名 aweme_list 恒空（关注流必须登录）
    // 旧值 /aweme/v1/feed/ 是推荐流路径，Follow 模型用它拿到的是推荐内容，已修正。
    return QString("/aweme/v1/follow/feed/");
}

QString DouyinApi::nearbyFeedApi()
{
    // 【1.0.0.24】同城端点 host 探针实证：status_code=0，aweme_list=6 条真实同城
    // 视频（含 play_addr/cover/statistics），翻页 pull_type=2 有效；current_city
    // 有 nearby_label_name="同城"。匿名可用，这是菜单「同城」的数据源。
    return QString("/aweme/v1/nearby/feed/");
}

QString DouyinApi::collectNotesApi()
{
    return QString("/aweme/v1/aweme/collect/");  // ⚠️ 抖音收藏夹内容，待抓包确认
}

void DouyinApi::startDownload(const QString& url, const QString& destPath, const QString& tag,
                              qint64 expectedBytes, qint64 rangeStart, qint64 rangeEnd, bool queued)
{
    // queued 必须透传给 worker：worker 的 queued() 是 activeCoverJobs() 唯一的计数来源
    // （限并发 5 全靠它数）。以前这里把 queued 丢掉，activeCoverJobs() 恒为 0，
    // pump 会把整个队列瞬间全部派出去 —— BB10 上表现成"封面一个都不出"。
    DyCurlDownloadWorker* w = new DyCurlDownloadWorker(url.toUtf8(), destPath, tag,
                                                   rangeStart, rangeEnd, m_cookie.toUtf8(), queued);
    m_dlJobs.insert(tag, w);
    m_dlTotal.insert(tag, expectedBytes);
    QObject::connect(w, SIGNAL(finished()), this, SLOT(onDownloadFinished()));
    w->start();
}

int DouyinApi::activeCoverJobs() const
{
    int n = 0;
    QHash<QString, DyCurlDownloadWorker*>::const_iterator it;
    for (it = m_dlJobs.constBegin(); it != m_dlJobs.constEnd(); ++it) {
        if (it.value()->queued()) ++n;
    }
    return n;
}

QString DouyinApi::readyImageFor(const QString& local, qint64 minSize)
{
    if (!m_coverReady.contains(local)) {
        if (isUsableImage(local, minSize)) {
            m_coverReady.insert(local, QLatin1String("file://") + local);
        } else {
            return QString();
        }
    }
    return m_coverReady.value(local);
}

void DouyinApi::pumpDownloads()
{
    // 【队列调度】与 BiliBar 的 BiliApi::pumpDownloads 同构：
    // 封面下载限并发（kMaxCoverConcurrent = 5），头像排封面后面。
    //
    // 为什么必须限并发：一屏 6~10 张封面如果同时各开一个线程做 TLS 握手，
    // BB10 的 curl 握不完（每次 3~6s，CONNECTTIMEOUT 一到全灭），
    // 结果是"整页一个封面都不显示"。排队 + 限并发是实测有效的唯一解。
    if (!m_dlWatchdog) {
        // 第一次派封面时才建看门狗：构造期刻意零动作（BB10 启动期对首帧渲染很敏感），
        // 没有下载任务时定时器也没有意义。
        m_dlWatchdog = new QTimer(this);
        m_dlWatchdog->setInterval(15000);
        connect(m_dlWatchdog, SIGNAL(timeout()), this, SLOT(onDlWatchdog()));
        m_dlWatchdog->start();
    }
    if (m_dlStallMs <= 0) {
        m_dlStallMs = QDateTime::currentMSecsSinceEpoch();
    }

    // 优先派 prio <= 0 的（视频封面），头像（prio=1）排后面。
    // 否则播放页会同时入队相关推荐封面 + 评论头像，头像先到的话封面要干等
    // 二三十秒，用户看到的就是"封面显示不出来"。
    while (activeCoverJobs() < kMaxCoverConcurrent && !m_dlQueue.isEmpty()) {
        int pick = 0;
        for (int i = 0; i < m_dlQueue.size(); ++i) {
            if (m_dlQueue.at(i).prio <= 0) { pick = i; break; }
        }
        const PendingDl p = m_dlQueue.takeAt(pick);
        startDownload(p.url, p.path, p.tag, p.expected, p.rangeStart, p.rangeEnd, true);
    }
}

void DouyinApi::dispatchDownloadResult(const QString& tag, const QString& path, bool ok)
{
    // 登录头像是一次性非队列下载，不走 coverReady 就绪表。必须在这里把本地
    // file:// 写回 Q_PROPERTY，否则 QML 的 imageSource: biliApi.userFace 恒为空。
    if (tag == QLatin1String("__dyface__")) {
        if (ok && isUsableImage(path, 512)) {
            m_userFace = QLatin1String("file://") + path;
            emit userChanged();
            qDebug("DouyinBar login face ready %s", qPrintable(m_userFace));
        }
        return;
    }
    // 一个路径可能被多个 tag 同时请求（见 m_dlPathTags 去重）：后请求的列表
    // （比如切过去的"关注"页）永远等不到结果，封面就会一直是灰块。
    Q_UNUSED(tag)
    Q_UNUSED(path)
    Q_UNUSED(ok)
}

void DouyinApi::onCurlFinished()
{
    DyCurlWorker* w = static_cast<DyCurlWorker*>(sender());
    if (!w) return;
    QString tag = w->tag();
    QString json = w->result();
    int code = w->code();
    bool ok = (code == CURLE_OK);
    m_lastBiz = tag;
    m_bizFinCount++;
    // 【1.0.0.30】每次业务请求结束必留一行：code（curl 错误）/http（状态码）/
    // bytes（响应大小）。1.0.0.28 空 feed 时缺的正是这一行。
    dyNetLog(QString("net reply tag=%1 code=%2 http=%3 bytes=%4")
                 .arg(tag).arg(code).arg(w->httpCode()).arg(json.size()));
    // ⚠️ TODO：按 tag 路由到登录/列表/详情等不同处理（类似 BiliApi::onCurlFinished 的 netReply 分发）。
    emit netReply(tag, json, ok);
    w->deleteLater();
}

void DouyinApi::onDownloadFinished()
{
    DyCurlDownloadWorker* w = static_cast<DyCurlDownloadWorker*>(sender());
    if (!w) return;
    QString tag = w->tag();
    QString path = w->path();
    int code = w->code();
    bool ok = (code == CURLE_OK);
    const bool wasQueued = w->queued();
    qDebug("DouyinBar DouyinApi::onDownloadFinished tag=%s code=%d bytes=%lld",
           qPrintable(tag), w->code(), (long long)w->written());
    // 【1.0.0.30】下载结束同样留痕：CDN Range 206 才算命中。
    dyNetLog(QString("net dl reply tag=%1 code=%2 http=%3 bytes=%4 path=%5")
                 .arg(tag).arg(w->code()).arg(w->httpCode())
                 .arg((long long)w->written()).arg(path));
    m_dlJobs.remove(tag);
    m_dlTotal.remove(tag);
    w->deleteLater();

    // 记下"上一次有下载结束"的时刻，看门狗靠它判断队列是否卡死。
    m_dlStallMs = QDateTime::currentMSecsSinceEpoch();
    if (wasQueued) {
        if (ok) m_imgOk++;
        else m_imgFail++;
    }

    // 【v102 同款】封面下载完成且**字节校验通过** → 登记进就绪表 + coverVersion++。
    // 版本号一变，QML 里所有 `coverUrlFor(url, api.coverVersion)` 绑定重算，
    // 封面立刻出现 —— 完全不依赖 ListView 对模型行变化的处理。
    // 这是本工程唯一被反复验证可靠的 C++→QML 通道（BBTube / BiliBar 同款机制）。
    if (wasQueued && ok) {
        const QString fmt = imageFormatOf(path);
        if (isUsableImage(path)) {
            m_coverReady.insert(path, QLatin1String("file://") + path);
            ++m_coverVersion;
            emit coverVersionChanged();
            qDebug("DouyinBar cover READY tag=%s fmt=%s bytes=%lld path=%s",
                   qPrintable(tag), qPrintable(fmt), (long long)w->written(), qPrintable(path));
        } else {
            // 内容与 .jpg 后缀不符 / 被截断：立刻删掉，不留坏缓存骗下一次启动。
            m_coverReady.remove(path);
            QFile::remove(path);
            qDebug("DouyinBar cover REJECT tag=%s fmt=%s path=%s",
                   qPrintable(tag), qPrintable(fmt), qPrintable(path));
        }
    }

    // 归还并发额度并立刻派下一张封面。
    // 必须在 emit downloadFinished 之前 pump：emit 里 UI 可能马上又入队新封面。
    if (wasQueued) {
        pumpDownloads();
    }

    // 这个路径可能被多个 tag 同时请求（见 m_dlPathTags）。
    // 取出全部关心者，逐个回调 —— 只回调发起下载的那个 tag 的话，
    // 后请求的列表永远等不到结果，封面一直是灰块。
    QStringList waiters = m_dlPathTags.take(path);
    if (waiters.isEmpty()) waiters << tag;   // 非封面队列的下载不会登记
    if (!waiters.contains(tag)) waiters.prepend(tag);

    for (int i = 0; i < waiters.size(); ++i) {
        dispatchDownloadResult(waiters.at(i), path, ok);
    }
    emit downloadFinished(tag, path, ok);
}

void DouyinApi::onLoginPollTick()
{
    Q_UNUSED(this)
    // ⚠️ TODO：定时轮询抖音登录结果。
}

void DouyinApi::onDlWatchdog()
{
    if (m_dlQueue.isEmpty()) {
        // 队列空不是问题：m_dlStallMs 一刀切（queue 空就重置），
        // 下面的 stall 判断只针对"队列非空但 60s 没动过"的情况。
        m_dlStallMs = 0;
        return;
    }
    // 槽位有空却没派出去 = 某次 pump 漏了，补派。
    // 计数现在是从 m_dlJobs 现数的（activeCoverJobs），不存在计数器泄漏，
    // 所以不需要强制归零。
    if (activeCoverJobs() < kMaxCoverConcurrent) {
        pumpDownloads();
        return;
    }
    // 队列非空且 60s 没有任何下载结束 → 视作 stalled，清掉最老的一张重来。
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_dlStallMs > 0 && now - m_dlStallMs > 60000) {
        m_dlStallMs = now;
        const PendingDl p = m_dlQueue.takeFirst();
        qDebug("DouyinBar watchdog drop stalled path=%s", qPrintable(p.path));
    }
}
