#ifndef NOTE_MODEL_HPP
#define NOTE_MODEL_HPP

#include <bb/cascades/databinding/arraydatamodel.h>
#include <QHash>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>
#include <QString>
#include <QTimer>
#include <QDebug>

class BiliApi;

/**
 * 笔记流列表模型 —— 移植 BBTube FeedStore 的模式，替代原 FeedModel。
 * 推荐/发现/搜索/关注/历史/收藏共用，首页加载、触底加载更多、错误+重试。
 *
 * 继承 bb::cascades::ArrayDataModel（Cascades ListView 需要 DataModel 子类；
 * QAbstractListModel 无法直接驱动 Cascades ListView），
 * 每行数据为 QVariantMap：noteId/title/coverUrl/durationText/metaText/typeName，
 * QML 侧经 ListItemData.<key> 读取。
 *
 * 【从 BerryLi 到 Xiaohongshu 的核心变更】
 *   - 统一用 "noteId" 替代 "bvid"（小红书笔记 ID）
 *   - Kind 枚举对应小红书的分类：Rcmd/Discover/Search/Follow/History/Collect
 *   - 封面 URL 只存远端，QML 用 biliApi.coverUrlFor(url, coverVersion) 解析
 *     （照 v102 封面机制，不再用 replace()/removeAt+insert 回填）
 */
struct NoteImgPending {
    int row;        // 绝对行号
    QString url;    // 下载用的完整 URL（已带尺寸后缀）
    QString path;   // 落盘路径
    int retry;      // 已重试次数
    NoteImgPending() : row(-1), retry(0) {}
};

class NoteModel : public bb::cascades::ArrayDataModel {
    Q_OBJECT

    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool loadingMore READ loadingMore NOTIFY loadingMoreChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Kind {
        Rcmd,       // 首页推荐
        Discover,   // 发现页
        Search,     // 搜索
        Follow,     // 关注动态（需要登录态）
        History,    // 观看历史（需要登录态）
        Collect     // 收藏夹内笔记
    };

    NoteModel(Kind kind, QObject* parent = 0);

    bool loading() const { return m_loading; }
    bool loadingMore() const { return m_loadingMore; }
    QString error() const { return m_error; }
    int count() const { return m_items.size(); }

    Q_INVOKABLE void load();
    Q_INVOKABLE void loadMore();
    /**
     * 切到本 Tab 时调用：只在"列表为空"或"登录态变了"时才重新拉。
     * 关注动态依赖登录态 —— 用户在设置页登录/退出后，这个列表必须跟着变。
     */
    Q_INVOKABLE void reloadIfStale();
    Q_INVOKABLE void setKeyword(const QString& keyword);
    Q_INVOKABLE void setUserId(const QString& userId);
    Q_INVOKABLE void setCollectId(const QString& collectId);
    Q_INVOKABLE void clearFeed();
    Q_INVOKABLE QString noteIdAt(int row) const;
    Q_INVOKABLE QString diagLast() const {
        return m_diagLast + QString(" img%1/%2").arg(m_imgOk).arg(m_imgTotal);
    }

    Q_PROPERTY(QString coverStats READ coverStats NOTIFY coverStatsChanged)
    QString coverStats() const {
        if (m_imgTotal == 0) return QString::fromUtf8("未请求");
        return QString("%1/%2").arg(m_imgOk).arg(m_imgTotal);
    }

public slots:
    /**
     * DouyinApi::netReply 的接收端（编译期 signal/slot 连接）。
     * 多个 NoteModel 实例 connect 到同一个 netReply 信号，tag 必须带
     * 每个实例唯一的前缀（m_tagPrefix），否则串台。
     */
    void onNetReply(const QString& tag, const QString& json, bool ok);

signals:
    void loadingChanged();
    void loadingMoreChanged();
    void errorChanged();
    void countChanged();
    /** 封面进度变化，见 coverStats()。 */
    void coverStatsChanged();

private:
    void fetch(bool more);
    void onReply(const QString& json, bool ok, bool more);
    void appendItems(const QVariantList& items);
    void reset();
    void connectNet();
    void connectImageNet();
    QString imageCachePath(const QString& url) const;

private slots:
    void onNavFailed();
    void onLoadTimeout();
    void onImageDownloadFinished(const QString& tag, const QString& path, bool ok);

private:
    Kind m_kind;
    QString m_keyword;
    QString m_userId;
    QString m_collectId;
    QString m_followOffset;
    bool m_loadedLoggedIn;
    int m_page;
    bool m_hasMore;
    bool m_loading;
    bool m_loadingMore;
    QString m_error;
    QVariantList m_items;
    bool m_navConnected;
    bool m_netConnected;
    bool m_imgNetConnected;
    QString m_tagPrefix;
    QHash<QString, NoteImgPending> m_pendingImg;
    quint32 m_imgSeq;
    int m_imgOk;
    int m_imgTotal;
    QString m_connectDiag;
    QTimer* m_loadTimer;
    QString m_diagLast;
};

#endif // NOTE_MODEL_HPP
