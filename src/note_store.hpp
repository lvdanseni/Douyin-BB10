#ifndef NOTE_STORE_HPP
#define NOTE_STORE_HPP

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVariantList>

#include <bb/cascades/ArrayDataModel>

class QTimer;
class QTcpServer;
class QTcpSocket;
class ProxyStreamWorker;
class BbPlayer;
class LocalDb;
class NoteModel;
class VideoProxy;

/**
 * 笔记详情 + 播放流 Store —— 对应 BBTube 的 VideoStore：
 * 拉取 note detail（标题/正文/图集/UP主/统计）→ 获取媒体直链 → 流式播放（不落盘）→ 评论。
 *
 * 播放为什么现在是"直接把直链交给播放器"：
 * 早期用 bb::multimedia::MediaPlayer（mmrenderer）走 QNX http 客户端，不能设
 * User-Agent / Referer，B 站 CDN 一律 403，全部失败。逆向 BBTube 后发现它
 * 用自带 libbbplayer.so 直接吃 CDN 直链、字节逐块喂解码器、全程不写沙盒。所以这里
 * 拿到媒体直链后直接 emit videoReadyToPlay(url)，由 VideoPlayer 交给 libbbplayer
 * 流式播放；UA/Referer 由 VideoPlayer 通过 bbplayer_net_config 注入。
 *
 * 【从 BerryLi 到 Xiaohongshu 的核心变更】
 *   - bvid/aid/cid → noteId/noteId/mediaId（小红书笔记 ID 体系）
 *   - 视频流 → 笔记图文/视频混合流（笔记可含多张图或单个视频）
 *   - 新增 LocalDb 持久化：笔记详情缓存、观看历史
 */
class NoteStore : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    /** 笔记详情（title/desc/images/videoUrl/ownerName/ownerFace/stats）。QML 直接绑定字段。 */
    Q_PROPERTY(QVariantMap detail READ detail NOTIFY detailChanged)
    /** 加载/播放失败原因（空字符串表示无错误）。 */
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    /** 下载/缓存进度 0-100（流式代理阶段恒为 0）。 */
    Q_PROPERTY(int dlPercent READ dlPercent NOTIFY dlPercentChanged)
    /** 下载速度/剩余时间文本。 */
    Q_PROPERTY(QString dlSpeed READ dlSpeed NOTIFY dlSpeedChanged)
    /** 评论列表模型（ArrayDataModel），ListView 直接绑定。 */
    Q_PROPERTY(QObject* commentModel READ commentModel CONSTANT)
    /** 评论条数（供标题显示）。 */
    Q_PROPERTY(int commentCount READ commentCount NOTIFY commentCountChanged)
    /** 相关推荐列表（NoteModel），ListView 直接绑定。 */
    Q_PROPERTY(QObject* relatedModel READ relatedModel CONSTANT)

    // ---- 互动状态（点赞/收藏/关注）----
    Q_PROPERTY(bool liked READ liked NOTIFY interactChanged)
    Q_PROPERTY(bool collected READ collected NOTIFY interactChanged)
    Q_PROPERTY(bool followed READ followed NOTIFY interactChanged)
    /** 操作反馈（"已点赞" / "收藏失败（-111）"），2.5 秒后自动清空。 */
    Q_PROPERTY(QString interactTip READ interactTip NOTIFY interactChanged)
    /** 下载状态。0 = 没下过，1 = 下载中，2 = 已下载。 */
    Q_PROPERTY(int dlState READ dlState NOTIFY dlStateChanged)

public:
    explicit NoteStore(QObject* parent = 0);

    bool loading() const { return m_loading; }
    QVariantMap detail() const { return m_detail; }
    QString error() const { return m_error; }
    int dlPercent() const { return m_dlPercent; }
    QString dlSpeed() const { return m_dlSpeed; }
    QObject* commentModel() const { return m_commentModel; }
    QObject* relatedModel() const;
    int commentCount() const { return m_commentCount; }
    bool liked() const { return m_liked; }
    bool collected() const { return m_collected; }
    bool followed() const { return m_followed; }
    QString interactTip() const { return m_interactTip; }
    int dlState() const { return m_dlState; }

    /* ---- 笔记加载 ---- */
    Q_INVOKABLE void load(const QString& noteId);

    /* ---- 互动动作（写操作，需要登录 + csrf）---- */
    Q_INVOKABLE void toggleLike();
    Q_INVOKABLE void toggleCollect();
    Q_INVOKABLE void toggleFollow();
    Q_INVOKABLE void shareNote();

    /* ---- 下载（图文/视频）---- */
    Q_INVOKABLE void downloadCurrent();
    Q_INVOKABLE void downloadMedia(const QString& url, qint64 expectedBytes,
                                   bool forPlayback = false);
    Q_INVOKABLE bool usingProxy() const { return m_usingProxy; }
    Q_INVOKABLE void stopStream();
    Q_INVOKABLE void fallbackToDownload();

    /** 诊断摘要。 */
    Q_INVOKABLE QString diagLast() const { return m_diagLast; }

public slots:
    /**
     * DouyinApi::netReply 统一接收端（编译期 signal/slot 连接）。
     * tag 路由：__detail__ / __media__ / __comment__ / __like__ / __collect__ / __follow__
     */
    void onNetReply(const QString& tag, const QString& json, bool ok);
    /** DouyinApi::downloadFinished 接收端。 */
    void onDownloadFinished(const QString& tag, const QString& path, bool ok);
    /** 下载进度轮询（每 500ms 读一次已写字节数）。 */
    void onDownloadTick();
    /** 操作反馈文案 2.5 秒后自动清空。 */
    void clearTip();
    /** 流式代理失败 → 自动退回整段下载。 */
    void onStreamFailed(const QString& reason);

signals:
    void loadingChanged();
    void detailChanged();
    void errorChanged();
    void dlPercentChanged();
    void dlSpeedChanged();
    void commentCountChanged();
    /** 互动状态/反馈变化。 */
    void interactChanged();
    /** 下载状态 0/1/2 变化（进度本身走 dlPercentChanged）。 */
    void dlStateChanged();
    void detailReady(const QVariantMap& detail);
    /** 接口解析出的总时长（秒）。applicationui 用编译期 connect
     *  转给 VideoPlayer::setApiDurationSec。 */
    void apiDurationReady(int sec);
    void mediaUrlReady(const QString& mediaUrl);
    void commentsReady(const QVariantList& comments);
    void loadFailed(const QString& message);
    /** 下载进度 0-100。 */
    void downloadProgress(int percent);
    /** 下载完成，path 为本地路径，可直接交给播放器。 */
    void localVideoReady(const QString& path);
    /** 下载失败。 */
    void downloadFailed(const QString& message);
    /** 下载速度/剩余时间文本，供界面显示。 */
    void downloadSpeed(const QString& text);
    /** 流式代理就绪，url 可直接交给播放器。 */
    void streamReady(const QString& url);
    /** 笔记已就绪、可直接播放/展示。 */
    void videoReadyToPlay(const QString& url);

private:
    void finishLoading();
    void onDetailReply(const QString& json, bool ok);
    void onMediaReply(const QString& json, bool ok);
    void onCommentReply(const QString& json, bool ok);
    /** 点赞/收藏/关注的统一回复处理。 */
    void onInteractReply(const QString& suffix, const QString& json, bool ok);
    /** 下载落盘完成：写 sidecar 元数据 + 把 dlState 置为"已下载"。 */
    void onLocalVideoReady(const QString& path);
    /** 写本地元数据 + 记录观看历史（LocalDb）。 */
    void writeDownloadMeta();
    /** 下载失败：把 dlState 从"下载中"退回"没下过"。 */
    void onDownloadFailedReset(const QString& reason);
    void connectNet();
    void startSegmented(const QString& url, const QString& finalPath, qint64 total);
    void startSingle(const QString& url, const QString& finalPath, qint64 total);
    void finishSegmented();
    void abortDownload(const QString& reason);
    /** 起本地代理，成功则发 streamReady，失败直接转 downloadMedia。 */
    void startStream(const QString& url, qint64 total);

    /* ---- 互动内部实现 ---- */
    void setTip(const QString& t);
    void setCountText(const QString& key, qint64 n);
    void fetchCollectFolders();
    void doCollectDeal(bool add);

    QString m_tagPrefix;
    QString m_noteId;
    QString m_mediaId;
    bool m_loading;
    bool m_netConnected;

    // ---- 互动状态 ----
    bool m_liked;
    int m_collectCount;      // 已收藏数（同一笔记最多收藏到多个夹）
    bool m_collected;
    bool m_followed;
    qint64 m_likeCount;
    qint64 m_collectTotal;
    QString m_ownerMid;      // 笔记作者 mid（关注接口的 fid）
    QStringList m_collectFolderIds;  // 收藏夹 id 列表（第一个一般是默认夹）
    QString m_pendingCollect;  // "add"/"del"：等收藏夹目录回来后再执行
    QString m_interactTip;
    QTimer* m_tipTimer;

    // ---- 下载状态 ----
    /** 0 = 没下过 / 1 = 下载中 / 2 = 已下载。 */
    int m_dlState;
    QString m_dlNoteId;
    QVariantMap m_dlMeta;
    int m_dlGen;
    bool m_dlForPlayback;
    bool m_dlWanted;
    qint64 m_dlProgressMs;
    qint64 m_dlBestGot;

    QStringList m_dlTags;
    QStringList m_dlParts;
    QString m_dlFinal;
    QString m_dlUrl;
    qint64 m_dlExpected;
    int m_dlPending;
    bool m_dlSegmented;
    qint64 m_dlLastBytes;
    qint64 m_dlLastMs;
    QTimer* m_dlTimer;
    int m_detailRetry;

    // ---- 流式播放 ----
    VideoProxy* m_proxy;
    QString m_streamUrl;
    qint64 m_streamSize;
    bool m_usingProxy;

    // ---- 暴露给 QML 的状态（属性绑定，规避 QML .connect() 不可靠） ----
    QVariantMap m_detail;
    QString m_error;
    int m_dlPercent;
    QString m_dlSpeed;
    bb::cascades::ArrayDataModel* m_commentModel;
    NoteModel* m_relatedModel;
    int m_commentCount;

    LocalDb* m_localDb;
    QString m_diagLast;
};

#endif // NOTE_STORE_HPP
