#include "video_prefetch.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMutexLocker>
#include <QMultiMap>
#include <QStringList>
#include <QUrl>
#include <curl/curl.h>

// 【1.0.0.28】换片秒开的预取器。设计定论见 video_prefetch.hpp 头注释
// （官方抖音 FeedCacheConfig.asyncPrepareVideo 的 BB10 移植）。
//
// 与播放流的关系：预取是**纯增量** —— 失败了播放照旧走 CDN，绝不让
// "预取没成功"变成"播不出来"。因此这里所有失败路径都只留痕 + emit
// prefetchFailed，由 DouyinPlayerStore 转成 dyplay.log 的一行 PREFETCH。

namespace {

/** 每次从 curl 写回调搬往磁盘的块大小阈值以上则续写；节流睡眠在写回调里。 */
const int kPfWriteChunk = 256 * 1024;
/** 节流：每写完一块睡这么久（≈3.2MB/s），给正在播的那条流留带宽。 */
const int kPfThrottleMs = 80;
/** 缓存封顶：只保留最新的几个（= 接下来会刷到的几个）。 */
const int kPfMaxCache = 6;
/** 与 douyin_store.cpp 的 DY_UA **逐字节一致**（同一批 play_addr 直链：
 *  播放代理用 Android UA 拿到过 206，预取换 UA 有被 CDN 403 的风险）。 */
const char* const kPfUA =
    "Mozilla/5.0 (Linux; Android 10; Pixel 4) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/86.0.4240.185 Mobile Safari/537.36";

}  // namespace

VideoPrefetchWorker::VideoPrefetchWorker(const QString& url,
                                         const QString& partPath,
                                         const QString& finalPath)
    : m_url(url)
    , m_partPath(partPath)
    , m_finalPath(finalPath)
    , m_file(0)
    , m_written(0)
    , m_elapsedMs(0)
    , m_abort(false)
    , m_done(false)
    , m_failed(false)
    , m_newResponse(true)
    , m_bodyDiscard(false)
{
}

VideoPrefetchWorker::~VideoPrefetchWorker()
{
    abortNow();
    QThread::wait(3000);   // 显式限定：QNX <sys/wait.h> 的 wait(int*) 会抢裸 wait()
    // .part 一律不残留：失败/中止时留下的半成品只会让下次 hasLocal 之外
    // 的清理逻辑变复杂。真正落地靠 rename（原子换名）。
    if (m_file) {
        m_file->close();
        delete m_file;
        m_file = 0;
        QFile::remove(m_partPath);
    }
}

void VideoPrefetchWorker::abortNow()
{
    QMutexLocker lk(&m_mutex);
    m_abort = true;
    m_cond.wakeAll();
}

QString VideoPrefetchWorker::failReason() const
{
    return m_err;
}

qint64 VideoPrefetchWorker::bytesWritten() const
{
    return m_written;
}

int VideoPrefetchWorker::elapsedMs() const
{
    return m_elapsedMs;
}

size_t VideoPrefetchWorker::writeCb(char* ptr, size_t size, size_t nmemb, void* ud)
{
    VideoPrefetchWorker* w = static_cast<VideoPrefetchWorker*>(ud);
    const size_t n = size * nmemb;

    QMutexLocker lk(&w->m_mutex);
    if (w->m_abort) return 0;   // 返回 0 = 让 curl 中止

    if (w->m_file) {
        w->m_file->write(ptr, (qint64)n);
        w->m_written += (qint64)n;
    }
    // 节流：播放中的流要带宽，预取不抢满速（抖音 videoDownloadOption 同思路）。
    w->m_cond.wait(&w->m_mutex, kPfThrottleMs);
    return n;
}

size_t VideoPrefetchWorker::headerCb(char* ptr, size_t size, size_t nmemb, void* ud)
{
    VideoPrefetchWorker* w = static_cast<VideoPrefetchWorker*>(ud);
    const size_t n = size * nmemb;
    if (!w) return n;

    w->m_hdrLine.append(ptr, (int)n);
    int cut;
    while ((cut = w->m_hdrLine.indexOf('\n')) >= 0) {
        QByteArray line = w->m_hdrLine.left(cut).trimmed();
        w->m_hdrLine.remove(0, cut + 1);
        if (line.isEmpty()) continue;
        if (w->m_newResponse) {
            w->m_newResponse = false;
            if (line.startsWith("HTTP/")) {
                const QList<QByteArray> parts = line.split(' ');
                if (parts.size() >= 2) {
                    const int code = parts.at(1).toInt();
                    w->m_bodyDiscard = (code >= 300 && code < 400);
                }
            }
            continue;
        }
        if (line.toLower().startsWith("location:")) {
            w->m_curLocation = QString::fromUtf8(line.mid(9).trimmed());
        }
    }
    return n;
}

int VideoPrefetchWorker::progressCb(void* ud, double, double, double, double)
{
    VideoPrefetchWorker* w = static_cast<VideoPrefetchWorker*>(ud);
    QMutexLocker lk(&w->m_mutex);
    return w->m_abort ? 1 : 0;
}

void VideoPrefetchWorker::run()
{
    QElapsedTimer t;
    t.start();

    m_file = new QFile(m_partPath);
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMutexLocker lk(&m_mutex);
        m_failed = true;
        m_err = "open .part";
        m_done = true;
        m_cond.wakeAll();
        return;
    }

    CURL* h = curl_easy_init();
    if (!h) {
        QMutexLocker lk(&m_mutex);
        m_failed = true;
        m_err = "curl init";
        m_done = true;
        m_cond.wakeAll();
        return;
    }

    struct curl_slist* hs = 0;
    hs = curl_slist_append(hs, (QString("User-Agent: ") + QString(kPfUA)).toUtf8().constData());
    hs = curl_slist_append(hs, "Referer: https://www.douyin.com/");
    hs = curl_slist_append(hs, "Origin: https://www.douyin.com/");
    hs = curl_slist_append(hs, "Accept: */*");
    // 【1.0.0.44】Range: 0- —— 真机实证：无 Range 的整文件 GET 被 CDN 302 跳
    // IPv6 字面量（BB10 无 IPv6 出口）；带 Range 与播放代理同路径，直接 206。
    hs = curl_slist_append(hs, "Range: 0-");

    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hs);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, &VideoPrefetchWorker::writeCb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, &VideoPrefetchWorker::headerCb);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, this);
    // 【1.0.0.44】同 DyCurlWorker：强制 IPv4 解析（BB10 网络栈无 IPv6 出口）。
    curl_easy_setopt(h, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 12L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_PROGRESSFUNCTION, &VideoPrefetchWorker::progressCb);
    curl_easy_setopt(h, CURLOPT_PROGRESSDATA, this);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);

    const int kMaxHops = 6;
    bool got = false;
    QString why = "cdn-unavailable";
    QStringList urls;
    if (!m_url.isEmpty()) urls << m_url;

    for (int ci = 0; ci < urls.size() && !m_abort && !got; ++ci) {
        QByteArray cur = urls.at(ci).toUtf8();
        for (int hop = 0; hop < kMaxHops && !m_abort && !got; ++hop) {
            m_hdrLine.clear();
            m_curLocation.clear();
            m_newResponse = true;
            m_bodyDiscard = false;
            curl_easy_setopt(h, CURLOPT_URL, cur.constData());

            const CURLcode rc = curl_easy_perform(h);

            long httpCode = 0;
            curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &httpCode);

            if (m_abort) break;

            if (rc != CURLE_OK) {
                QMutexLocker lk(&m_mutex);
                m_failed = true;
                m_err = QString("curl %1").arg((int)rc);
                m_done = true;
                m_cond.wakeAll();
                curl_easy_setopt(h, CURLOPT_HTTPHEADER, 0);
                curl_slist_free_all(hs);
                curl_easy_cleanup(h);
                return;
            }

            if (httpCode == 200 || httpCode == 206) {
                got = true;
                break;
            }

            if (httpCode >= 300 && httpCode < 400 && !m_curLocation.isEmpty()) {
                const QString loc = m_curLocation;
                // 与播放 worker 同规则：IPv6 字面量跳转 BB10 无通路，直接否决。
                if (loc.contains("://[") || loc.startsWith("//[")) {
                    why = "cdn-ipv6-hop";
                    break;
                }
                QUrl base = QUrl::fromEncoded(cur);
                QUrl next = base.resolved(QUrl(loc));
                cur = next.toEncoded();
                continue;
            }

            why = QString("http %1").arg(httpCode);
            break;
        }
    }

    curl_easy_setopt(h, CURLOPT_HTTPHEADER, 0);
    curl_slist_free_all(hs);
    curl_easy_cleanup(h);

    {
        QMutexLocker lk(&m_mutex);
        m_elapsedMs = (int)t.elapsed();
    }

    if (!got) {
        // 失败：只留痕。.part 由析构清掉。
        QMutexLocker lk(&m_mutex);
        if (!m_abort) {
            m_failed = true;
            m_err = why;
        }
        m_done = true;
        m_cond.wakeAll();
        qDebug("BiliClient prefetch FAILED url=%d why=%s written=%lld",
               m_url.size(), qPrintable(why), (long long)m_written);
        return;
    }

    // 成功：flush → close → remove 旧文件 → rename 原子落地。
    // 【关键】QFile::rename 不覆盖已存在目标（1.0.0.25 历史落盘静默失败教训：
    // 不先 remove 的话，第二次预取同一 URL 时 rename 静默失败，磁盘永远停
    // 在半成品上，而 hasLocal 只会看到旧的完整文件 —— 表现为"预取永远不更新"）。
    m_file->flush();
    m_file->close();
    delete m_file;
    m_file = 0;

    bool renamed = false;
    if (QFile::exists(m_finalPath)) {
        QFile::remove(m_finalPath);
    }
    renamed = QFile::rename(m_partPath, m_finalPath);

    QMutexLocker lk(&m_mutex);
    if (!renamed) {
        m_failed = true;
        m_err = "rename";
    }
    m_done = true;
    m_cond.wakeAll();
    qDebug("BiliClient prefetch DONE url=%d bytes=%lld ms=%d renamed=%d",
           m_url.size(), (long long)m_written, m_elapsedMs, renamed ? 1 : 0);
}

// ---------------------------------------------------------------------------

VideoPrefetcher::VideoPrefetcher(QObject* parent)
    : QObject(parent)
    , m_doneCount(0)
    , m_failCount(0)
{
}

QString VideoPrefetcher::cacheDir()
{
    static QString s_dir;
    if (s_dir.isEmpty()) {
        QDir d(QDir::homePath());
        // BB10 的 homePath 本身就是沙盒 data 目录（勿再拼 /data/ —— 1.0.0.8
        // data/data/ 双目录教训）。
        if (!d.exists("dy_prefetch")) d.mkpath("dy_prefetch");
        s_dir = d.absoluteFilePath("dy_prefetch");
    }
    return s_dir;
}

QString VideoPrefetcher::localPathFor(const QString& url)
{
    if (url.isEmpty()) return QString();
    const QByteArray h = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Md5).toHex();
    return cacheDir() + QLatin1Char('/') + QString::fromLatin1(h) + QLatin1String(".mp4");
}

QString VideoPrefetcher::localFor(const QString& url) const
{
    const QString p = localPathFor(url);
    if (p.isEmpty()) return QString();
    const QFileInfo fi(p);
    if (fi.exists() && fi.size() > 0) {
        return QString("file://") + p;
    }
    return QString();
}

bool VideoPrefetcher::hasLocal(const QString& url) const
{
    return !localFor(url).isEmpty();
}

void VideoPrefetcher::prefetch(const QString& url, qint64 totalSize)
{
    // 【1.0.0.50】入口顺手收尸（60s 限速），stale .part 不再无限堆积。
    sweepStaleParts(300);
    if (url.isEmpty() || url.startsWith("file://")) return;
    if (hasLocal(url)) {
        // 已在本地：不重复下载（用户来回刷同一批视频时最常见）。
        return;
    }
    if (m_inflight.contains(url)) return;   // 单飞
    if (m_inflight.size() >= 3) return;     // 最多同时预取 3 个，别和播放抢

    const QString finalPath = localPathFor(url);
    if (finalPath.isEmpty()) return;
    const QString partPath = finalPath + QString::fromLatin1(".part");

    m_inflight << url;
    VideoPrefetchWorker* w = new VideoPrefetchWorker(url, partPath, finalPath);
    QObject::connect(w, SIGNAL(finished()), this, SLOT(onWorkerFinished()));
    qDebug("BiliClient prefetch ISSUE url=%d size=%lld inFlight=%d",
           url.size(), (long long)totalSize, m_inflight.size());
    w->start();
}

void VideoPrefetcher::clearAll()
{
    QDir d(cacheDir());
    const QStringList files = d.entryList(QStringList() << "*.mp4" << "*.part",
                                          QDir::Files);
    for (int i = 0; i < files.size(); ++i) {
        QFile::remove(d.absoluteFilePath(files.at(i)));
    }
    qDebug("BiliClient prefetch clearAll removed=%d", files.size());
}

/**
 * 【1.0.0.50】陈旧 .part 收尸（实现）。真机证据：1.0.0.49 dy_prefetch 躺着
 * 两个 13:57 的孤儿 .part（6.8MB/62.8MB，白占 70MB）—— worker 被杀后
 * fopen 的 .part 无人 rename/remove。trimCache 只扫 *.mp4、clearAll 没有
 * 调用方，这个泄漏没有任何既有路径能收。
 * 限速：两次清扫至少隔 60s，避免刷 feed 时每片都扫目录。
 */
void VideoPrefetcher::sweepStaleParts(int maxAgeSec)
{
    if (m_sweepClock.isNull()) {
        m_sweepClock.start();
        // 首次（DouyinStore 构造期）立即扫：启动时 inflight 为空，目录里的
        // .part 必然全是上一轮会话遗留的孤儿。误伤不存在。
    } else if (m_sweepClock.elapsed() < 60000) {
        return;   // 60s 限速：刷 feed 时每片都调也不扫
    } else {
        m_sweepClock.restart();
    }

    QDir d(cacheDir());
    if (!d.exists()) return;
    const QStringList files = d.entryList(QStringList() << "*.part", QDir::Files);
    if (files.isEmpty()) return;
    const QDateTime now = QDateTime::currentDateTime();
    int removed = 0;
    qint64 freed = 0;
    for (int i = 0; i < files.size(); ++i) {
        const QFileInfo fi(d.absoluteFilePath(files.at(i)));
        // 年龄判据：下载超时上限 600s，在飞的 .part mtime 会被 writeCb 持续
        // 刷新（年龄必小）；>= maxAgeSec 只可能是孤儿或卡死的 worker。
        if (fi.lastModified().secsTo(now) < maxAgeSec) continue;
        const qint64 sz = fi.size();
        if (QFile::remove(fi.absoluteFilePath())) {
            ++removed;
            freed += sz;
        }
    }
    if (removed > 0) {
        qDebug("BiliClient prefetch sweepStaleParts removed=%d freed=%lld",
               removed, (long long)freed);
    }
}

int VideoPrefetcher::inFlight() const
{
    return m_inflight.size();
}

void VideoPrefetcher::onWorkerFinished()
{
    if (!sender()) return;
    VideoPrefetchWorker* w = static_cast<VideoPrefetchWorker*>(sender());
    const QString url = w->url();
    const bool failed = !w->failReason().isEmpty();
    const QString why = w->failReason();
    const qint64 bytes = w->bytesWritten();
    const int ms = w->elapsedMs();
    const QString path = VideoPrefetcher::localPathFor(url);

    m_inflight.removeAll(url);
    if (failed) {
        ++m_failCount;
        emit prefetchFailed(url, why);
    } else {
        ++m_doneCount;
        emit prefetchDone(url, path, bytes, ms);
    }
    w->deleteLater();   // 先摘链（上面已从 m_inflight 移除）再删
    trimCache();
}

void VideoPrefetcher::trimCache()
{
    // 只保留最新的 kPfMaxCache 个 .mp4（.part 是在飞的半成品，不动）。
    // 按最后修改时间排序：预取按刷视频顺序写盘，新的几个 = 接下来会刷到的几个。
    // 【为什么用 QMultiMap 而不是 std::sort + lambda】NDK 默认 C++98，lambda
    // 编不过；QMultiMap 按键（时间）自动升序，取头部最旧的删即可。
    QDir d(cacheDir());
    const QStringList files = d.entryList(QStringList() << "*.mp4", QDir::Files);
    if (files.size() <= kPfMaxCache) return;

    QMultiMap<QDateTime, QString> byTime;
    for (int i = 0; i < files.size(); ++i) {
        const QFileInfo fi(d.absoluteFilePath(files.at(i)));
        byTime.insert(fi.lastModified(), fi.absoluteFilePath());
    }
    QMultiMap<QDateTime, QString>::const_iterator it = byTime.constBegin();
    for (int i = files.size(); i > kPfMaxCache && it != byTime.constEnd(); --i, ++it) {
        QFile::remove(it.value());
    }
}
