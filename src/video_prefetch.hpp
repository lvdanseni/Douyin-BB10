#ifndef VIDEO_PREFETCH_HPP
#define VIDEO_PREFETCH_HPP

#include <QFile>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTime>
#include <QWaitCondition>

/**
 * 【1.0.0.28】下一片预取器 —— 官方抖音 asyncPrepareVideo 的 BB10 移植。
 *
 * 逆向依据（analysis/p26/rea_notes.md，官方 aweme APK 类名级实证）：
 * 官方 feed 缓存框架带 FeedCacheConfig.asyncPrepareVideo /
 * videoDownloadOption —— 当前片起播即后台预取下一片，换片时本地秒开；
 * Relax 分页框架另有"预渲染下一 panel"的开关（StopPrerenderWhenPageStopWhiteList
 * 反证其存在）。BB10 单 FWC + 本地 HTTP 代理架构下的等价实现：
 *
 *   1) 换片成功（DouyinPlayerStore::load）即预取**下一片** play_addr 整段下载
 *      到沙盒 dy_prefetch/<md5(url)>.mp4（.part → remove+rename 原子落地）；
 *   2) 下次换片时 VideoProxy 的源 URL 换成 file:// 本地文件
 *      （ProxyStreamWorker 的本地分支 seek/read 直服，不走 CDN）；
 *   3) 播放中的请求与预取请求共享 DNS 缓存（curl share），互不重复解析。
 *
 * 铁律落地：
 *   - worker 不许自毁：VideoPrefetcher 统一收尸，先摘链再 deleteLater
 *     （ProxyStreamWorker 1.0.0.11 SIGSEGV 同款教训）；
 *   - QFile::rename 不覆盖已存在目标：rename 前必须 remove（1.0.0.25
 *     历史落盘静默失败同款教训）；
 *   - 换片失败不甩锅给预取：预取失败只留痕，播放永远回落到 CDN。
 */

/** 全量下载 worker：curl 走链（IPv6 字面量否决同 ProxyStreamWorker）。 */
class VideoPrefetchWorker : public QThread {
public:
    VideoPrefetchWorker(const QString& url, const QString& partPath,
                        const QString& finalPath);
    ~VideoPrefetchWorker();

    /** 请求停止（可在任意线程调用）。 */
    void abortNow();

    /** 本 worker 负责的源 URL（主线程收尸时用，无需加锁：run() 里不变）。 */
    QString url() const { return m_url; }
    QString failReason() const;
    qint64 bytesWritten() const;
    /** 本次下载耗时（ms，完成时有效）。 */
    int elapsedMs() const;

protected:
    void run();

private:
    static size_t writeCb(char* ptr, size_t size, size_t nmemb, void* ud);
    static size_t headerCb(char* ptr, size_t size, size_t nmemb, void* ud);
    static int progressCb(void* ud, double dlt, double dln, double ult, double uln);

    QMutex m_mutex;
    QWaitCondition m_cond;
    QString m_url;
    QString m_partPath;
    QString m_finalPath;
    QString m_err;
    QFile* m_file;          /**< .part 文件句柄（仅 worker 线程触碰）。 */
    qint64 m_written;
    int m_elapsedMs;
    bool m_abort;
    bool m_done;
    bool m_failed;
    bool m_newResponse;     /**< 下一次头数据应是状态行。 */
    bool m_bodyDiscard;     /**< 3xx 跳转响应的 body 丢弃。 */
    QByteArray m_hdrLine;
    QString m_curLocation;
};

/**
 * 预取调度器（主线程对象）。
 * 单飞：同一 URL 在飞则跳过；已在本地则跳过；缓存封顶 kPfMaxCache 个文件
 * （Q20 沙盒空间有限，保留最新的几个 = 接下来会刷到的几个）。
 */
class VideoPrefetcher : public QObject {
    Q_OBJECT

public:
    explicit VideoPrefetcher(QObject* parent = 0);

    /** 缓存目录（沙盒 homePath()/dy_prefetch，幂等创建）。 */
    static QString cacheDir();
    /** md5(url)+".mp4" 的绝对路径（定长 32 十六进制，无特殊字符）。 */
    static QString localPathFor(const QString& url);

    /** 本地已缓存（存在且非空）→ "file://<绝对路径>"；否则空串。 */
    Q_INVOKABLE QString localFor(const QString& url) const;
    Q_INVOKABLE bool hasLocal(const QString& url) const;

    /**
     * 【1.0.0.50】陈旧 .part 收尸：dy_prefetch 里 worker 被换片/退出杀掉的
     * 半成品（fopen "wb" 后无人 rename/remove）只增不减 —— 真机 1.0.0.49
     * 发现 13:57 的 6.8MB+62.8MB 两个孤儿，白占 70MB 沙盒。
     * 判据 mtime 年龄 >= maxAgeSec：下载超时上限 600s（queued 20s），
     * 在飞的 .part mtime 会被持续刷新，年龄必小；卡死 30 分钟的 worker
     * 之类也该收。内部 60s 限速，挂 prefetch() 入口，运行中持续收尸。
     */
    void sweepStaleParts(int maxAgeSec = 1800);

    /**
     * 后台预取（QML 换片成功后调）。同 url 单飞；已在本地直接跳过。
     * @param totalSize 仅用于日志对照（feed 的 data_size）。
     */
    Q_INVOKABLE void prefetch(const QString& url, qint64 totalSize);

    /** 清空缓存（诊断/设置页用）。 */
    Q_INVOKABLE void clearAll();
    /** 在飞数量（自检/诊断）。 */
    Q_INVOKABLE int inFlight() const;

signals:
    /** 预取完成（path 不带 file:// 前缀）。 */
    void prefetchDone(const QString& url, const QString& path, qint64 bytes, int ms);
    /** 预取失败（只留痕，不影响播放回落 CDN）。 */
    void prefetchFailed(const QString& url, const QString& why);

private slots:
    void onWorkerFinished();

private:
    void trimCache();

    QStringList m_inflight;   /**< 在飞的 url（主线程专用，无需锁）。 */
    int m_doneCount;
    int m_failCount;
    QTime m_sweepClock;       /**< 【1.0.0.50】.part 收尸限速钟（60s）。 */
};

#endif // VIDEO_PREFETCH_HPP
