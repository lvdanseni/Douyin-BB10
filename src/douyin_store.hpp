#ifndef DOUYIN_STORE_HPP
#define DOUYIN_STORE_HPP

#include <bb/cascades/databinding/arraydatamodel.h>
#include <QHash>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>
#include <QString>
#include <QTimer>
#include <QDebug>

class DouyinApi;
class VideoProxy;
class VideoPrefetcher;

/**
 * 抖音视频流分页模型。
 *
 * 为什么单独一个类而不是复用 BiliBar 的 FeedModel：
 *  FeedModel 的 JSON 解析与 B 站接口结构强绑定（data.item / data.list / data.medias
 *  三种不同字段名 + 每种一套字段映射），而抖音是 aweme_list + 嵌套的
 *  video/author/statistics。硬塞进同一个 switch 会变成两边互相拖累的大杂烩。
 *  抖音的结构更扁平（所有接口都是 aweme_list），单独一个类反而更短。
 *
 * 行字段（QML 侧通过 ListItemData.<key> 读）：
 *   awemeId / title / picUrl / durationText / metaText / api
 *
 * 【封面机制照旧】模型里 picUrl **永远存远端 URL，永不变化**；
 * 本地路径由 QML 用 api.coverUrlFor(picUrl, api.coverVersion) 现场解析。
 * 原因见 MEMORY「v102/v103 定论」：BB10 上模型行更新 → ListView 重绘不可靠，
 * Q_PROPERTY + NOTIFY 才是验证过 100% 可靠的通道。
 */
class DouyinFeedModel : public bb::cascades::ArrayDataModel {
    Q_OBJECT

    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool loadingMore READ loadingMore NOTIFY loadingMoreChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    /** 封面进度诊断（"已就绪/总数"），设置页可见。 */
    Q_PROPERTY(QString coverStats READ coverStats NOTIFY coverStatsChanged)

public:
    enum Kind {
        Recommend,   // 首页推荐流（/aweme/v1/feed/，免签名直连）
        Follow,      // 关注流（/aweme/v1/follow/feed/，匿名恒空）
        City,        // 同城流（/aweme/v1/nearby/feed/，host 探针实证可用）
        UserPost     // 某用户发布的视频（同 feed 路径 + sec_user_id）
    };

    explicit DouyinFeedModel(Kind kind = Recommend, QObject* parent = 0);

    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool loadingMore READ loadingMore NOTIFY loadingMoreChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString coverStats READ coverStats NOTIFY coverStatsChanged)

    bool loading() const { return m_loading; }
    bool loadingMore() const { return m_loadingMore; }
    QString error() const { return m_error; }
    int count() const { return m_items.size(); }
    QString coverStats() const {
        if (m_imgTotal == 0) return QString::fromUtf8("未请求");
        return QString("%1/%2").arg(m_imgOk).arg(m_imgTotal);
    }

    /** 拉第一页。 */
    Q_INVOKABLE void load();
    /** 触底拉下一页（游标由接口给，不自己算）。 */
    Q_INVOKABLE void loadMore();
    /** 切到本页时调用：登录态变了或列表为空才重拉。 */
    Q_INVOKABLE void reloadIfStale();
    /** 清空（退出登录、切换来源时用）。 */
    Q_INVOKABLE void clearFeed();
    /** 第 row 行的视频 id。 */
    Q_INVOKABLE QString awemeIdAt(int row) const;
    /** 第 row 行的完整数据（含 picUrl / playUrl / durationSec / authorName…）。
     *  点视频时把整行扔给 DouyinPlayerStore::load(row)，播放免二次请求。 */
    Q_INVOKABLE QVariantMap rowAt(int row) const;
    /** 设置 UserPost 类型的 sec_uid。 */
    Q_INVOKABLE void setSecUid(const QString& secUid);
    /** 诊断文本。 */
    Q_INVOKABLE QString diagLast() const { return m_diagLast; }

public slots:
    /** DouyinApi::netReply 的接收端（编译期 signal/slot 连接）。 */
    void onNetReply(const QString& tag, const QString& json, bool ok);

signals:
    void loadingChanged();
    void loadingMoreChanged();
    void errorChanged();
    void countChanged();
    void coverStatsChanged();

private:
    /** 待回填封面：tag → 该行在 m_items 里的绝对索引。 */
    struct ImgPending {
        int row;
        QString url;
        QString path;
        ImgPending() : row(-1) {}
    };

    void fetch(bool more);
    void onReply(const QString& json, bool ok, bool more);
    void appendItems(const QVariantList& items);
    void reset();
    void connectNet();
    void connectImageNet();
    QString imageCachePath(const QString& url) const;
    /** prio=0 视频封面（优先），prio=1 作者头像（排封面后面，避免抢 TLS 并发）。 */
    void startDownloadCover(const QString& url, const QString& local, int absRow, int prio = 0);

private slots:
    void onImageDownloadFinished(const QString& tag, const QString& path, bool ok);
    void onLoadTimeout();

private:
    Kind m_kind;
    QString m_secUid;
    /** 抖音翻页游标（接口给的 max_cursor，不透明值，原样回传）。 */
    QString m_maxCursor;
    bool m_hasMore;
    bool m_loading;
    bool m_loadingMore;
    QString m_error;
    QVariantList m_items;
    /** 【1.0.0.22】已入列表的 awemeId 集合。
     *  抖音匿名推荐流服务端会返重复 aweme（insert_ids 上限 60、max_cursor 常返 0），
     *  不去重的话同一部视频在列表里出现三五次，表现就是「刷过去的视频又回来」。 */
    QSet<QString> m_seenIds;
    /** 被去重丢弃的重复项累计数（写进 diagLast 供真机取证）。 */
    int m_dupSkipped;
    bool m_netConnected;
    bool m_imgNetConnected;
    QString m_tagPrefix;
    QHash<QString, ImgPending> m_pendingImg;
    /** 封面请求的单调递增序号（永不清零），只用于拼出永不重复的 tag。 */
    quint32 m_imgSeq;
    int m_imgOk;
    int m_imgTotal;
    /** 上次 load 时的登录态，用于判断登录/退出后要不要重拉。 */
    bool m_loadedLoggedIn;
    QString m_diagLast;
    QTimer* m_loadTimer;
};


/**
 * 抖音视频详情 + 播放地址解析 Store。
 *
 * 职责：
 *  1. 调 iteminfo 接口拿单条视频详情（标题/作者/统计/播放地址）
 *  2. 从 play_addr 里挑一条可用直链（去水印：playwm → play）
 *  3. 交给 VideoProxy 在 127.0.0.1 起本地代理，边拉 CDN 数据边喂播放器
 *  4. emit videoReadyToPlay(url) → 由 ApplicationUI 用编译期 connect 转给 VideoPlayer
 *
 * 【为什么用 VideoProxy 而不是直接喂 CDN 直链】
 *  bb::multimedia::MediaPlayer 走 QNX mmrenderer，无法设置 UA/Referer，
 *  而抖音 CDN 两个都要校验（缺 UA 直接 403）。VideoProxy 用 libcurl 带完整
 *  请求头拉流，再把字节喂给本地 socket，播放器连的是 127.0.0.1 不受校验影响。
 */
class DouyinPlayerStore : public QObject {
    Q_OBJECT

    Q_PROPERTY(QString awemeId READ awemeId NOTIFY detailChanged)
    Q_PROPERTY(QString title READ title NOTIFY detailChanged)
    Q_PROPERTY(QString authorName READ authorName NOTIFY detailChanged)
    Q_PROPERTY(QString authorFace READ authorFace NOTIFY detailChanged)
    Q_PROPERTY(QString authorId READ authorId NOTIFY detailChanged)
    Q_PROPERTY(int likeCount READ likeCount NOTIFY detailChanged)
    Q_PROPERTY(int commentCount READ commentCount NOTIFY detailChanged)
    Q_PROPERTY(int playCount READ playCount NOTIFY detailChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    /** 视频时长（秒）。 */
    Q_PROPERTY(int durationSec READ durationSec NOTIFY detailChanged)
    /** 竖屏视频（height > width）时为 true，播放页据此决定画面比例。 */
    Q_PROPERTY(bool portrait READ portrait NOTIFY detailChanged)
    /** 一次性操作提示（"已点赞"等），C++ 定时清空。 */
    Q_PROPERTY(QString tip READ tip NOTIFY tipChanged)
    /** 右侧三个按钮的本地点状态（接口不通时 UI 也要有反馈）。 */
    Q_PROPERTY(bool liked READ liked NOTIFY detailChanged)
    Q_PROPERTY(bool collected READ collected NOTIFY detailChanged)
    Q_PROPERTY(bool authorFollowed READ authorFollowed NOTIFY detailChanged)
    /** 诊断行：播放链路走到了哪一步（NetworkStore 的 diagLast 用）。 */
    Q_PROPERTY(QString diagState READ diagLast NOTIFY detailChanged)

public:
    explicit DouyinPlayerStore(QObject* parent = 0);

    QString awemeId() const { return m_awemeId; }
    QString title() const { return m_title; }
    QString authorName() const { return m_authorName; }
    QString authorFace() const { return m_authorFace; }
    QString authorId() const { return m_authorId; }
    int likeCount() const { return m_likeCount; }
    int commentCount() const { return m_commentCount; }
    int playCount() const { return m_playCount; }
    bool loading() const { return m_loading; }
    QString error() const { return m_error; }
    int durationSec() const { return m_durationSec; }
    bool portrait() const { return m_portrait; }
    QString tip() const { return m_tip; }
    /** 右侧三个按钮的本地点状态。 */
    bool liked() const { return m_liked; }
    bool collected() const { return m_collected; }
    bool authorFollowed() const { return m_authorFollowed; }

    /**
     * 加载指定视频。
     *
     * @param row  FeedModel 的整行数据（QVariantMap），至少含 awemeId。
     *              行里已内嵌 playUrl 时（FeedModel 从 feed 响应直接解析得到）
     *              **直接播放，不再请求 iteminfo** —— 抖音 feed 返回的 aweme
     *              是完整对象，play_addr 就在 video 里，二次请求纯属浪费。
     *              行里没有 playUrl（老缓存/精简行）时才回落 iteminfo。
     */
    Q_INVOKABLE void load(const QVariantMap& row);

    /** 仅取视频详情（用 awemeId 请求 iteminfo）。 */
    Q_INVOKABLE void loadDetail(const QString& awemeId);

    /** 停止流式代理（离开播放页必须调用，否则后台继续吃流量）。 */
    Q_INVOKABLE void stopStream();

    /** 点赞 / 收藏 / 关注（需登录）。 */
    Q_INVOKABLE void toggleLike();
    Q_INVOKABLE void toggleCollect();
    Q_INVOKABLE void toggleFollow();

    /** 诊断文本。 */
    Q_INVOKABLE QString diagLast() const { return m_diagLast; }
    /** 自检用：本地代理已送达播放器的字节数（>0 证明真的在拉流）。 */
    Q_INVOKABLE qint64 proxyBytesSent() const;
    /** 【1.0.0.24】QML 菜单/开关的轻提示（3s 自消，复用 setTip 通道）。 */
    Q_INVOKABLE void showTip(const QString& text) { setTip(text); }
    /**
     * 【1.0.0.28】预取下一片（官方抖音 asyncPrepareVideo 同款）。
     * 换片成功后由 QML playInline 调：后台整段下载下一片 play_addr 到沙盒，
     * 下次换片时 startStream 直接 file:// 秒开。失败只留痕不甩锅播放。
     */
    Q_INVOKABLE void prefetchNext(const QVariantMap& row);

public slots:
    void onNetReply(const QString& tag, const QString& json, bool ok);

signals:
    void detailChanged();
    void loadingChanged();
    void errorChanged();
    void tipChanged();
    /** 播放地址就绪（本地代理 URL）。ApplicationUI 用编译期 connect 转给 VideoPlayer。 */
    void videoReadyToPlay(const QString& url);
    /** 视频时长（秒），播放器 duration 为 0 时作兜底。 */
    void apiDurationReady(int sec);
    /**
     * 【1.0.0.24】整段视频字节已全部送达播放器（代理侧 EOF 事实）。
     * 循环播放的结束判定用它兜底：进度/时长可能因墙钟兜底失真，
     * 但"代理把 totalSize 字节都交给播放器了"是硬事实。
     */
    void streamComplete();

private slots:
    void onTipTimeout();
    void onProxyStreamFailed(const QString& reason);
    /** 【1.0.0.28】预取结果留痕（dyplay.log 的 PREFETCH 行，协议证据用）。 */
    void onPrefetchDone(const QString& url, const QString& path, qint64 bytes, int ms);
    void onPrefetchFailed(const QString& url, const QString& why);

private:
    void setTip(const QString& text);
    /**
     * @param totalSize 该 CDN 直链的真实字节数（play_addr.data_size）。
     *  必须传：VideoProxy 靠它回 206 + Content-Range，libbbplayer 检测到
     *  「server does not serve byte ranges (HTTP 200)」会直接放弃播放。
     */
    void startStream(const QString& cdnUrl, qint64 totalSize);
    /** 【1.0.0.23】带备用 CDN 候选的播放启动（单节点 302 到 IPv6 字面量时自动切换）。 */
    void startStream(const QString& cdnUrl, qint64 totalSize, const QStringList& alts);
    /** 从 feed 行数据里解出 CDN 直链（优先最高清的 bit_rate），并带回它的 data_size。 */
    QString pickPlayUrl(const QVariantMap& videoMap, qint64* sizeOut);
    /** 【1.0.0.23】同上，另带回同一 play_addr 的备用 CDN 节点列表。 */
    QString pickPlayUrl(const QVariantMap& videoMap, qint64* sizeOut, QStringList* altsOut);
    /** 从 iteminfo 响应里解析播放直链。 */
    bool parsePlayUrl(const QString& json);

private:
    QString m_awemeId;
    QString m_title;
    QString m_authorName;
    QString m_authorFace;
    QString m_authorId;
    int m_likeCount;
    int m_commentCount;
    int m_playCount;
    int m_durationSec;
    bool m_portrait;
    bool m_loading;
    QString m_error;
    QString m_tip;
    QTimer* m_tipTimer;
    /** 本地点赞/收藏/关注状态（接口不通时 UI 也要有反馈）。 */
    bool m_liked;
    bool m_collected;
    bool m_authorFollowed;
    VideoProxy* m_proxy;
    /** 【1.0.0.28】下一片预取器（本地化换片的 source）。 */
    VideoPrefetcher* m_prefetch;
    /** 【1.0.0.37】起播耗时分层：load()/loadDetail() 记时刻，startStream 算差值。
     *  证据链 = load issued → STREAM(url picked) → READY(proxy up) → PLAY → 帧推进，
     *  dyplay.log 一次重启即可定位"卡在哪一跳"（加载不出/慢的定性工具）。 */
    qint64 m_loadIssueMs;
    QString m_tagPrefix;
    QString m_diagLast;
};

/**
 * 【1.0.0.23】评论列表模型（DouyinCommentModel）。
 *
 * 接口：www.iesdouyin.com/web/api/v2/comment/list/（DouyinApi::commentListUrl），
 * 2026-11-06 curl 实证：设备同款 App UA + Referer 返 200 JSON，匿名可读，
 * comments[] 每页固定 10 条，cursor 参数被服务端忽略。
 *
 * 翻页策略（诚实的降级，不是假装有下一页）：
 *   loadMore 用「最后一条 cid」作 cursor 再拉一次；服务端若哪一天开始支持
 *   cursor，新评论直接追加；若仍返旧数据（当前行为），客户端按 cid 去重后发现
 *   0 条新增 → hasMore=false + 提示「匿名接口仅提供前 10 条」。
 *   绝不把同一批评论重复 append 进列表（那会是自欺欺人的假分页）。
 *
 * 行字段（QML 经 ListItemData.<key> 读）：
 *   cid / text / nick / face(远端 URL，永不变化) / digg / ipLabel /
 *   createTime / replyTotal / api(DouyinApi 指针，见 v102 定论)
 * 头像解析与其他页完全同链：pickJpegUrl → faceCachePath → 队列下载 →
 * DouyinApi 就绪表 + coverVersion++ → QML faceUrlFor(face, coverVersion)。
 */
class DouyinCommentModel : public bb::cascades::ArrayDataModel {
    Q_OBJECT

    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool loadingMore READ loadingMore NOTIFY loadingMoreChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY hasMoreChanged)
    /** 诊断行（拉了多少条、去重丢了多少、头像就绪数）。 */
    Q_PROPERTY(QString diagLast READ diagLast NOTIFY diagChanged)
    /** 当前视频 id（QML 可绑定显示）。 */
    Q_PROPERTY(QString awemeId READ awemeId NOTIFY awemeIdChanged)
    /**
     * 【1.0.0.29】发表评论结果提示（QML 底部输入条上方显示）。
     * 取值示例："发表成功" / "用户未登录：请先粘贴 Cookie 登录" /
     *           "发表失败：<status_msg>" / "评论内容不能为空"。
     * 3 秒后自消（m_tipTimer）。
     */
    Q_PROPERTY(QString publishTip READ publishTip NOTIFY publishTipChanged)
    /** 发表请求在飞（单飞；输入条按钮转圈用）。 */
    Q_PROPERTY(bool publishing READ publishing NOTIFY publishingChanged)

public:
    explicit DouyinCommentModel(QObject* parent = 0);

    bool loading() const { return m_loading; }
    bool loadingMore() const { return m_loadingMore; }
    QString error() const { return m_error; }
    int count() const { return m_items.size(); }
    bool hasMore() const { return m_hasMore; }
    QString diagLast() const { return m_diagLast; }
    QString awemeId() const { return m_awemeId; }
    QString publishTip() const { return m_publishTip; }
    bool publishing() const { return m_publishing; }

    /** 拉某个视频的第一页评论（换视频时整体重拉）。 */
    Q_INVOKABLE void load(const QString& awemeId);
    /** 触底拉更多（去重后无新增即自动置 hasMore=false）。 */
    Q_INVOKABLE void loadMore();
    /** 清空（离开评论页时调，避免下次进来先看到旧数据）。 */
    Q_INVOKABLE void clear();

    /**
     * 【1.0.0.29】发表评论到当前 awemeId。
     * POST /aweme/v1/comment/publish/（JSON，tag=dycmt:pub，后缀路由进
     * onPublishReply）。成功（status_code=0）后自动重载评论列表 —— 服务端
     * cursor 被忽略，重拉即拿到含新评论的前 10 条。结果一律进 publishTip。
     */
    Q_INVOKABLE void publish(const QString& text);

public slots:
    /** DouyinApi::netReply 的接收端（编译期 signal/slot 连接）。 */
    void onNetReply(const QString& tag, const QString& json, bool ok);

signals:
    void loadingChanged();
    void loadingMoreChanged();
    void errorChanged();
    void countChanged();
    void hasMoreChanged();
    void diagChanged();
    void awemeIdChanged();
    void publishTipChanged();
    void publishingChanged();

private:
    /** 待回填头像：tag → 本地路径（只用于诊断，行数据永不动）。 */
    struct ImgPending {
        QString path;
        ImgPending() {}
    };

    void fetch(bool more);
    void onReply(const QString& json, bool ok, bool more);
    void appendComments(const QVariantList& comments, bool more);
    void onPublishReply(const QString& json, bool ok);
    /** 发布结果提示 3 秒自消（懒起定时器）。 */
    void setPublishTip(const QString& tip);
    void resetState();
    void connectNet();
    void connectImageNet();
    void startFaceDownload(const QString& url, const QString& local);

private slots:
    void onImageDownloadFinished(const QString& tag, const QString& path, bool ok);
    void onLoadTimeout();
    /** publishTip 自消到时。 */
    void onPublishTipTimeout();

private:
    QString m_awemeId;
    /** 翻页游标：首页 0；之后是最后一条评论的 cid（数值）。 */
    qint64 m_cursor;
    bool m_hasMore;
    bool m_loading;
    bool m_loadingMore;
    QString m_error;
    QVariantList m_items;
    /** 已入列表的 cid 集合：服务端忽略 cursor 时的去重兜底。 */
    QSet<QString> m_seenCids;
    bool m_netConnected;
    bool m_imgNetConnected;
    QString m_tagPrefix;
    QHash<QString, ImgPending> m_pendingImg;
    quint32 m_imgSeq;
    int m_imgOk;
    int m_imgTotal;
    int m_dupSkipped;
    QString m_diagLast;
    QTimer* m_loadTimer;
    /** 【1.0.0.29】发表评论：结果提示 + 在飞标志 + 自消定时器。 */
    QString m_publishTip;
    bool m_publishing;
    QTimer* m_publishTipTimer;
};

/**
 * 【1.0.0.24】抖音观看历史模型。
 *
 * 为什么不用 LocalDb/SQLite：历史就是「最近看过谁」的薄列表，一个 JSON 数组
 * 文件足够，且与 BiliBar 的 HistoryStore（小红书笔记历史）完全隔离，互不污染。
 *
 * 记录时机：feed 页 playInline 换片（SWITCH）与历史项复访时各写一次；
 * 同一 awemeId 去重并置顶，上限 100 条（超出丢尾）。
 * 行字段：awemeId/title/authorName/authorFace/picUrl/playUrl/videoSize/
 *         durationSec/videoAspect/watchedAt（epoch ms）/timeText（hh:mm）。
 */
class DouyinHistoryModel : public bb::cascades::ArrayDataModel {
    Q_OBJECT

    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString diagLast READ diagLast NOTIFY diagChanged)

public:
    explicit DouyinHistoryModel(QObject* parent = 0);

    int count() const { return m_items.size(); }
    QString diagLast() const { return m_diagLast; }

    /** 记录一次观看（换片/复访）。同一 awemeId 置顶去重，落盘。 */
    Q_INVOKABLE void add(const QVariantMap& row);
    /** 清空全部历史（含磁盘文件）。 */
    Q_INVOKABLE void clear();
    /** 第 row 行的完整数据（含 playUrl，点击历史项直接复播）。 */
    Q_INVOKABLE QVariantMap rowAt(int row) const;

signals:
    void countChanged();
    void diagChanged();

private:
    void loadFile();
    void saveFile() const;

    QVariantList m_items;
    QString m_path;
    QString m_diagLast;
};

/**
 * 【1.0.0.37】抖音本地收藏模型（用户要求"菜单里添加收藏"）。
 *
 * 与 DouyinHistoryModel 同构（JSON 文件、去重、api 指针随行塞），差别：
 *  - 收藏是用户主动行为，上限 200，按收藏动作置顶；
 *  - 多一个 favVersion Q_PROPERTY + favText()：QML 按钮文字
 *    "★ 已收藏"/"☆ 收藏" 要随 toggle 实时变 —— 模型行更新不可靠
 *    （v102 定论），用 NOTIFY 属性把版本号拉进依赖图（coverVersion 同款）。
 *
 * 行字段：awemeId/title/authorName/authorFace/picUrl/playUrl/videoSize/
 *         durationSec/videoAspect/favedAt/timeText/api/favStore。
 * 播放回落：收藏行可能存着已过期的 CDN 直链，点击复播失败时 HUD 报错 +
 * 重试兜住（与历史页同一机制，不假装一定播得出来）。
 */
class DouyinFavoriteModel : public bb::cascades::ArrayDataModel {
    Q_OBJECT

    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString diagLast READ diagLast NOTIFY diagChanged)
    /** 版本号：每次增删 ++，QML 按钮文字绑它做依赖锚点（coverVersion 同款）。 */
    Q_PROPERTY(int favVersion READ favVersion NOTIFY favVersionChanged)

public:
    explicit DouyinFavoriteModel(QObject* parent = 0);

    int count() const { return m_items.size(); }
    QString diagLast() const { return m_diagLast; }
    int favVersion() const { return m_favVersion; }

    /** 收藏/取消收藏（同 id 去重）。已收藏则移除并返回 false。 */
    Q_INVOKABLE bool toggle(const QVariantMap& row);
    /** 仅加入（不切换）。 */
    Q_INVOKABLE void add(const QVariantMap& row);
    /** 取消一个收藏。 */
    Q_INVOKABLE void remove(const QString& awemeId);
    /** 是否已收藏（QML 按钮状态用）。 */
    Q_INVOKABLE bool has(const QString& awemeId) const;
    /**
     * 按钮文字（带版本锚点）："★ 已收藏" / "☆ 收藏"。
     * @param ver 依赖锚点，调用方传 favVersion；值本身不参与判断。
     */
    Q_INVOKABLE QString favText(const QString& awemeId, int ver) const;
    /** 清空全部收藏（含磁盘文件）。 */
    Q_INVOKABLE void clear();
    /** 第 row 行的完整数据。 */
    Q_INVOKABLE QVariantMap rowAt(int row) const;

signals:
    void countChanged();
    void diagChanged();
    void favVersionChanged();

private:
    void loadFile();
    void saveFile() const;
    void bumpVersion() { ++m_favVersion; emit favVersionChanged(); }

    QVariantList m_items;
    QString m_path;
    QString m_diagLast;
    int m_favVersion;
};

/**
 * 【1.0.0.37】抖音视频下载 Store（用户要求"视频页下载按钮 + 菜单已下载"）。
 *
 * 为什么单独一个 Store 而不是复用 DouyinApi 的封面下载：
 *   - 封面走队列限并发（kMaxCoverConcurrent=5）且会被 trimCache 清理，
 *     视频必须**直下**（queued=false，独占 worker）且**长期保留**；
 *   - 下载有完整生命周期：进行中/完成/失败 + 进度，需要持久索引
 *     （data/douyin_downloads.json），重启后"已下载"列表还在；
 *   - 落盘目录独立：videos/<awemeId>.mp4（不进 dy_prefetch 封顶缓存）。
 *
 * 协议复用：DyCurlDownloadWorker 已在 DouyinApi::download()（CDN UA/Referer
 * 齐备、.part→rename 原子落地、LOW_SPEED 保活），这里只做业务编排：
 *   进度 = DouyinApi::downloadWritten(tag)（500ms 定时采样）；
 *   完成 = DouyinApi::downloadFinished(tag, path, ok)（编译期 connect）。
 *
 * 播放回落：已下载行的 playUrl 换成 "file://<videos/abs>.mp4"，经
 * DouyinPlayerStore::startStream → VideoProxy::runLocal（seek/read 直服，
 * 不吃 CDN 直链过期）—— 与 1.0.0.28 预取命中同一机制。
 *
 * 行字段：awemeId/title/authorName/authorFace/picUrl/durationSec/videoAspect/
 *         api/playUrl(CDN 原文)/localPath/fileBytes/state/
 *         downloadedAt/timeText。
 */
class DouyinDownloadStore : public bb::cascades::ArrayDataModel {
    Q_OBJECT

    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString diagLast READ diagLast NOTIFY diagChanged)
    /** 一次性操作提示（"下载完成"等，3s 自消；QML 无 Timer，铁律）。 */
    Q_PROPERTY(QString tip READ tip NOTIFY tipChanged)
    /** 版本号：进度采样/状态变化时 ++，QML 行内文字绑它做依赖锚点。 */
    Q_PROPERTY(int dlVersion READ dlVersion NOTIFY dlVersionChanged)
    /** 是否有下载在进行中。 */
    Q_PROPERTY(bool downloading READ downloading NOTIFY dlVersionChanged)

public:
    explicit DouyinDownloadStore(QObject* parent = 0);

    int count() const { return m_items.size(); }
    QString diagLast() const { return m_diagLast; }
    QString tip() const { return m_tip; }
    int dlVersion() const { return m_dlVersion; }
    bool downloading() const { return !m_activeTag.isEmpty(); }

    /**
     * 开始下载一行（feed 页「下载」按钮入口）。
     * 已下载/在飞 → 提示并返回；否则插一行 state=downloading 并直下。
     */
    Q_INVOKABLE void start(const QVariantMap& row);
    /** 取消进行中的下载（按 awemeId）。 */
    Q_INVOKABLE void cancel(const QString& awemeId);
    /** 删除一个已下载视频（文件 + 索引）。 */
    Q_INVOKABLE void remove(const QString& awemeId);
    /** 是否已下载（按钮状态用）。 */
    Q_INVOKABLE bool has(const QString& awemeId) const;
    /**
     * 下载按钮/行内文字（带版本锚点，dlVersion 传进去做依赖）：
     * "下载" / "已下载" / "↓ 32%" / "取消"。
     */
    Q_INVOKABLE QString dlText(const QString& awemeId, int ver) const;
    /** 当前在飞下载的进度文字（"" = 无）。 */
    Q_INVOKABLE QString progressText() const;
    /**
     * 行内进度文字（带版本锚点）：只有"正在下载这一行"才给进度，
     * 其余返回 ""。ver 传 dlVersion —— QML 绑定只在表达式里读到
     * dlVersion 变化时才重算（coverVersion 同款机制）。
     */
    Q_INVOKABLE QString progressFor(const QString& awemeId, int ver) const;
    /** 重读索引 + 自愈（索引里文件丢了的行会被清掉）。 */
    Q_INVOKABLE void reload();
    /**
     * 第 row 行的完整数据（已下载页点击播放用）。
     * 【1.0.0.41】漏声明 = QML 里 rowAt 是 undefined：页面 onTriggered 每次
     * 点击都抛 TypeError 且无任何日志（DL-ITEM ret= 永不出现），页面看着正常、
     * 点进去没反应 —— 与"菜单点击没反应"同源的静默故障。
     */
    Q_INVOKABLE QVariantMap rowAt(int row) const;
    /** videos/ 目录绝对路径（诊断用）。 */
    Q_INVOKABLE QString cacheDir() const;
    /** 文件字节数可读化（"3.2MB"）。 */
    Q_INVOKABLE QString sizeText(qint64 bytes) const;

signals:
    void countChanged();
    void diagChanged();
    void tipChanged();
    void dlVersionChanged();

private slots:
    /** DouyinApi::downloadFinished 的接收端（tag 形如 __dyvid__<awemeId>）。 */
    void onDownloadFinished(const QString& tag, const QString& path, bool ok);
    /** 进度采样心跳（500ms；只在有在飞任务时跑）。 */
    void onProgressTick();
    /** tip 自消。 */
    void onTipTimeout();

private:
    void loadFile();
    void saveFile() const;
    void bumpVersion() { ++m_dlVersion; emit dlVersionChanged(); }
    void setTip(const QString& text);
    QString localPathFor(const QString& awemeId) const;
    QString tagFor(const QString& awemeId) const;
    void finishRow(const QString& awemeId, bool ok, qint64 bytes, const QString& why);
    void dropRow(int idx);

    QVariantList m_items;
    QString m_path;
    QString m_dir;
    QString m_diagLast;
    QString m_tip;
    QTimer* m_tipTimer;
    /** 进度采样定时器（在飞才跑，闲时零开销）。 */
    QTimer* m_progTimer;
    int m_dlVersion;
    /** 在飞任务的 tag（含 awemeId），空 = 无在飞。 */
    QString m_activeTag;
    bool m_netConnected;
};

#endif // DOUYIN_STORE_HPP
