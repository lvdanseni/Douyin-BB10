#ifndef FEED_MODEL_HPP
#define FEED_MODEL_HPP

#include <bb/cascades/databinding/arraydatamodel.h>
#include <QHash>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>
#include <QString>
#include <QTimer>
#include <QDebug>

/**
 * 分页 Feed 列表模型 —— 移植 BBTube FeedStore 的模式：
 * 推荐/热门/排行榜/分区/搜索共用，首页加载、触底加载更多、错误+重试。
 *
 * 继承 bb::cascades::ArrayDataModel（Cascades ListView 需要 DataModel 子类；
 * QAbstractListModel 无法直接驱动 Cascades ListView），
 * 每行数据为 QVariantMap：bvid/title/picUrl/durationText/metaText/typeName，
 * QML 侧经 ListItemData.<key> 读取。
 */

/**
 * 【v96】封面待回填项。
 *
 * 为什么不能只存行号：下载失败要重试，重试需要原始 URL 和落盘路径 ——
 * 而失败后行数据里的 picUrl 是空字符串（正是"还没图"的状态），
 * 拿不回来 URL。所以入队时就把 url/path/retry 一起记下来。
 *
 * 必须是文件作用域的结构体：BB10 的 qcc 是 C++03，
 * 局部类型不能当 QHash 的模板参数（v89 在 DownloadStore 上踩过）。
 */
struct ImgPending {
    int row;        // 绝对行号
    QString url;    // 下载用的完整 URL（已带 @672w 后缀）
    QString path;   // 落盘路径
    int retry;      // 已重试次数
    ImgPending() : row(-1), retry(0) {}
};

class FeedModel : public bb::cascades::ArrayDataModel {
    Q_OBJECT

    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool loadingMore READ loadingMore NOTIFY loadingMoreChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Kind {
        Rcmd,       // 首页推荐
        Popular,    // 热门
        Ranking,    // 排行榜（单页）
        Region,     // 分区
        Search,     // 搜索
        Related,    // 播放页下方的"相关推荐"（/x/web-interface/archive/related）
        Follow,     // 【v83】关注动态（/x/polymer/web-dynamic/v1/feed/all?type=video）
        Fav,        // 【v84】某个收藏夹里的视频（/x/v3/fav/resource/list）
        History     // 【v89】观看历史（/x/web-interface/history/cursor）
    };

    FeedModel(Kind kind, int regionRid, QObject* parent = 0);

    bool loading() const { return m_loading; }
    bool loadingMore() const { return m_loadingMore; }
    QString error() const { return m_error; }
    int count() const { return m_items.size(); }

    Q_INVOKABLE void load();
    Q_INVOKABLE void loadMore();
    /**
     * 【v83】切到本 Tab 时调用：只在"列表为空"或"登录态变了"时才重新拉。
     * 关注动态依赖登录态 —— 用户在设置页登录/退出后，这个列表必须跟着变，
     * 否则会出现"登录了但还是提示未登录"的死状态。
     */
    Q_INVOKABLE void reloadIfStale();
    Q_INVOKABLE void setKeyword(const QString& keyword);
    /**
     * 【v72】Related 类型专用：设置要查相关推荐的 bvid。
     * 必须在 load() 之前调用 —— fetch() 会把它塞进请求参数。
     */
    Q_INVOKABLE void setRelatedBvid(const QString& bvid);
    Q_INVOKABLE void setRegion(int rid);
    /**
     * 【v84】Fav 类型专用：设置要拉取的收藏夹 media_id。
     * 和 setRegion 一样，id 变了会 reset()，调用方随后再 load()。
     */
    Q_INVOKABLE void setFavMediaId(qint64 mediaId);
    /**
     * 【v84】彻底清空（等价于 reset()）。
     * VideoStore 原来直接调 ArrayDataModel::clear()，那只会清界面上的行，
     * m_items / m_pendingImg 还留着旧数据 —— 在途的封面回调回来时会拿旧索引
     * 去 replace 一个空模型。这里统一走 reset()。
     */
    Q_INVOKABLE void clearFeed();
    Q_INVOKABLE QString bvidAt(int row) const;
    /**
     * 【v98】除了接口结果，还动态带上封面下载进度。
     * 排查"封面只显示一部分"时这一项最直接：
     *   img3/20  → 只成功 3 张，是下载问题
     *   img20/20 → 全下好了，那就是界面刷新问题
     * 动态拼是因为封面回调在接口返回之后才陆续到达。
     */
    Q_INVOKABLE QString diagLast() const {
        return m_diagLast + QString(" img%1/%2").arg(m_imgOk).arg(m_imgTotal);
    }

    /**
     * 【v100】封面进度，给设置页当**可见**的诊断行用。
     *
     * 之前 diagLast() 只是个 Q_INVOKABLE，QML 里根本没人调用 —— 排查
     * "封面不显示"时我一直在猜，因为用户看不到 imgN/M 这个数字。
     * 做成 Q_PROPERTY + NOTIFY 后，QML 里 `rcmdModel.coverStats` 会随封面
     * 下载实时更新，一眼就能分清是"没下到"还是"下了但没画出来"。
     */
    Q_PROPERTY(QString coverStats READ coverStats NOTIFY coverStatsChanged)
    QString coverStats() const {
        if (m_imgTotal == 0) return QString::fromUtf8("未请求");
        return QString("%1/%2").arg(m_imgOk).arg(m_imgTotal);
    }

public slots:
    /**
     * DouyinApi::netReply 的接收端（编译期 signal/slot 连接，非 invokeMethod 运行时派发）。
     * 仿 BBTube SearchStore::searchResults / FeedStore 的命名槽模式。
     *
     * 关键：5 个 FeedModel 都 connect 到同一个 netReply 信号，因此 tag 必须带
     * 每个实例唯一的前缀（m_tagPrefix），否则一个模型的响应会被全部模型同时解析。
     */
    void onNetReply(const QString& tag, const QString& json, bool ok);

signals:
    void loadingChanged();
    void loadingMoreChanged();
    void errorChanged();
    void countChanged();
    /** 【v100】封面进度变化，见 coverStats()。 */
    void coverStatsChanged();

private:
    void fetch(bool more);
    void onReply(const QString& json, bool ok, bool more);
    void appendItems(const QVariantList& items);
    void reset();
    void connectNet();
    /** 连接 downloadFinished，用于预取封面图片。 */
    void connectImageNet();
    /** 按 URL 哈希命名的图片缓存文件路径。 */
    QString imageCachePath(const QString& url) const;

private slots:
    void onNavFailed();
    void onLoadTimeout();
    /** 封面图片预取完成：将 m_items[row] 的 picUrl 换成 file:// 本地路径。 */
    void onImageDownloadFinished(const QString& tag, const QString& path, bool ok);

private:
    Kind m_kind;
    int m_regionRid;
    QString m_keyword;
    /** 【v72】Related 类型要查的 bvid。 */
    QString m_relatedBvid;
    /** 【v83】关注动态的翻页游标（接口给的不透明字符串，回传即可，不要自己算）。 */
    QString m_followOffset;
    /** 【v83】上一次 load 时的登录态，用于判断登录/退出后要不要重拉。 */
    bool m_loadedLoggedIn;
    /** 【v84】Fav 类型要拉的收藏夹 id。 */
    qint64 m_favMediaId;
    /** 【v89】History 的翻页游标。接口给的是不透明值，原样回传即可 ——
     *  不要自己按页码算（和 Follow 的 offset 是同一个道理）。 */
    QString m_histMax;
    QString m_histViewAt;
    int m_page;
    bool m_hasMore;
    bool m_loading;
    bool m_loadingMore;
    QString m_error;
    QVariantList m_items;
    bool m_navConnected;
    bool m_netConnected;
    bool m_imgNetConnected;  // downloadFinished 是否已连接
    QString m_tagPrefix;   // 实例唯一前缀，避免多模型串台
    /**
     * 【v78】待回填的封面：tag → 该行在 m_items 里的**绝对索引**。
     *
     * 为什么需要这张表：appendItems 里算出来的 rowIndex 只是"本批次内"的序号，
     * 而 downloadFinished 回调可能在 loadMore 之后才到 —— 直接拿 rowIndex 当
     * 模型索引会更新错行。更要命的是 clear() 之后索引会从头开始，旧回调
     * 带着旧索引回来会污染新列表。用表 + clear() 清表，两个问题一起解决。
     */
    QHash<QString, ImgPending> m_pendingImg;
    /**
     * 【v99】封面请求的单调递增序号，只用来拼出**永不重复**的 tag。
     *
     * 原来 tag 里放的是绝对行号（":img%1" % absRow）。clear() 之后行号从 0 重来，
     * 于是刷新前那个还没下完的 ":img0" 和刷新后新发的 ":img0" 撞成同一个 tag：
     * 旧 worker 回来时 take() 走了新请求的表项，把**旧视频的封面**贴到新列表第 0 行。
     * 行号只该存在 ImgPending 里（它已经是绝对索引了），tag 只需要唯一。
     * 这个计数器**永远不清零** —— 清零就又撞回去了。
     */
    quint32 m_imgSeq;
    /** 【v98】封面下载成功数 / 入队总数，只用于诊断输出。 */
    int m_imgOk;
    int m_imgTotal;
    QString m_connectDiag; // connect 结果诊断（connect:ok / connect:FAILED）
    QTimer* m_loadTimer;
    QString m_diagLast;
};


/**
 * 【v84】收藏夹目录 + 收藏夹内容。
 *
 * 为什么单独一个类：收藏是**两级**数据 —— 先拿收藏夹列表（默认收藏夹/自定义夹…），
 * 再按 media_id 拉某个夹子里的视频。FeedModel 只能表达"一个列表"，
 * 塞不进去。这里把两级串起来：目录到货后自动选中第一个夹子并开始拉视频，
 * QML 侧只需要渲染，不用做任何"拿到目录再选第一个"的时序协调。
 *
 * 【为什么不放 QML 做】QML 里把 C++ 信号 connect 到 JS 函数在 BB10/Qt4.8 上
 * 静默不连接（项目铁律 4）。这里的自动选中全在 C++ 里完成，
 * QML 只通过 Q_PROPERTY 读 count / current / error。
 */
class FavStore : public QObject {
    Q_OBJECT

    Q_PROPERTY(int count READ count NOTIFY foldersChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY foldersChanged)
    Q_PROPERTY(QString error READ error NOTIFY foldersChanged)
    /** 当前选中的收藏夹下标（-1 = 未选） */
    Q_PROPERTY(int current READ current NOTIFY currentChanged)
    Q_PROPERTY(QString currentTitle READ currentTitle NOTIFY currentChanged)
    /** 收藏夹内容的 FeedModel（Kind=Fav），QML 直接喂给 FeedPage */
    Q_PROPERTY(QObject* videoModel READ videoModel CONSTANT)

public:
    explicit FavStore(QObject* parent = 0);

    int count() const { return m_folders.size(); }
    bool loading() const { return m_loading; }
    QString error() const { return m_error; }
    int current() const { return m_current; }
    QString currentTitle() const;
    QObject* videoModel() const;

    /** 切到收藏 tab 时调用：按登录态决定"清空 / 重拉目录 / 重拉当前夹子"。 */
    Q_INVOKABLE void reload();
    /** 强制重拉收藏夹目录（TitleBar 的刷新按钮）。 */
    Q_INVOKABLE void loadFolders();
    Q_INVOKABLE void selectFolder(int index);
    /** 第 i 个收藏夹的名字；越界返回空串（QML 的 6 个按钮共用）。 */
    Q_INVOKABLE QString folderName(int index) const;
    Q_INVOKABLE int folderMediaCount(int index) const;
    Q_INVOKABLE QString diagLast() const { return m_diagLast; }

public slots:
    void onNetReply(const QString& tag, const QString& json, bool ok);

signals:
    void foldersChanged();
    void currentChanged();

private:
    struct Folder {
        qint64 id;
        QString title;
        int mediaCount;
    };

    QList<Folder> m_folders;
    FeedModel* m_videos;
    bool m_loading;
    QString m_error;
    int m_current;
    bool m_loadedLoggedIn;
    QString m_tagPrefix;
    QString m_diagLast;
};

/**
 * 【v89】"已下载"板块的数据源。
 *
 * 【为什么不用 FeedModel】它的数据全靠 HTTP 拉，而这里的数据在**本地文件系统**里：
 * 沙盒 `videos` 目录下的 mp4 就是全部内容。硬塞进 FeedModel 只会让 fetch() 多一个
 * "不发请求"的特例，还得绕开分页/游标那一套。
 *
 * 【视频标题从哪来】下载完成时 VideoStore 会写一份同名 sidecar
 * `videos/<bvid>.json`（title/cover/duration/owner/ts）。没有 sidecar 的老文件
 * 就退化成显示 bvid —— 至少不会空白。
 *
 * 列表行字段和 FeedModel 保持一致（bvid/title/picUrl/durationText/metaText），
 * 这样 VideoRow 和 pushVideo() 都能原样复用。
 */
class DownloadStore : public QObject {
    Q_OBJECT

    Q_PROPERTY(QObject* model READ model CONSTANT)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    /** 下载目录里一个文件都没有时为 true，QML 用它显示空状态提示。 */
    Q_PROPERTY(bool empty READ isEmpty NOTIFY countChanged)

    /**
     * 【v91】长按"删除"的待确认项。
     *
     * 为什么确认状态放在 C++ 而不是 QML：
     * 长按菜单（contextActions）声明在 ListItemComponent 里面，而 ListItemComponent
     * 是独立组件作用域 —— 取不到 DownloadPage 的文档级 id，没法回调页面的函数来弹确认层。
     * 把"待删除的是谁"存成 C++ 属性后，页面只要绑定 `downloadStore.pendingBvid`
     * 就能决定确认层显不显示，QML 侧不需要任何跨作用域调用。
     * （属性绑定是本工程唯一 100% 可靠的 C++→QML 通道，见 MEMORY 铁律 4。）
     */
    Q_PROPERTY(QString pendingBvid READ pendingBvid NOTIFY pendingDeleteChanged)
    Q_PROPERTY(QString pendingTitle READ pendingTitle NOTIFY pendingDeleteChanged)
    /** 一次性操作提示（"已删除 xxx" / "删除失败"），C++ 定时清空。 */
    Q_PROPERTY(QString tip READ tip NOTIFY tipChanged)

public:
    explicit DownloadStore(QObject* parent = 0);

    QObject* model() const;
    int count() const { return m_count; }
    bool isEmpty() const { return m_count == 0; }
    QString pendingBvid() const { return m_pendingBvid; }
    QString pendingTitle() const { return m_pendingTitle; }
    QString tip() const { return m_tip; }

    /** 重新扫一遍 videos 目录（切到本 Tab 时调用）。 */
    Q_INVOKABLE void reload();
    Q_INVOKABLE QString bvidAt(int row) const;

    /** 长按菜单点了"删除"：只记下待删项，真正的删除等用户确认。 */
    Q_INVOKABLE void requestDelete(const QString& bvid);
    /** 确认层点"删除"。 */
    Q_INVOKABLE void confirmDelete();
    /** 确认层点"取消"。 */
    Q_INVOKABLE void cancelDelete();

signals:
    void countChanged();
    void pendingDeleteChanged();
    void tipChanged();

private slots:
    void onTipTimeout();
    /**
     * 【v93】已下载列表补封面。
     *
     * 下载视频时 VideoStore 会顺手缓存封面，但老版本存的路径规则和列表页不一致
     * （少一个 @672w 尺寸后缀），所以历史下载的封面在列表里查不到。
     * 这里在 reload 时把缺失的封面重新入队，回来后就地 replace 那一行，
     * 不整体重建 model（否则 20 个视频补封面会闪 20 次）。
     */
    void onCoverFinished(const QString& tag, const QString& path, bool ok);
    /**
     * 【v93】给老下载补元数据（标题/封面/UP 主）。
     *
     * v89~v92 下载的视频可能没有 sidecar，或者 sidecar 里标题是空的
     * （下载时 detail 还没到）。那些行在列表里只剩 BV 号。
     * 这里在 reload 时对"标题还等于 bvid"的行发一次 /x/web-interface/view，
     * 拿回真标题后写回 sidecar 并就地更新那一行。离线时请求失败，保持原样。
     */
    void onMetaReply(const QString& tag, const QString& json, bool ok);

private:
    void setTip(const QString& text);

    bb::cascades::ArrayDataModel* m_model;
    int m_count;
    QString m_pendingBvid;
    QString m_pendingTitle;
    QString m_tip;
    QTimer* m_tipTimer;
    /** 补封面的 tag → 行号。reload 开头清空，回调里 take 掉。 */
    QHash<QString, int> m_coverRows;
    /** 已经发过元数据请求的 bvid。跨 reload 保留，避免每次切 tab 都重发一遍。 */
    QSet<QString> m_metaAsked;
};

#endif // FEED_MODEL_HPP