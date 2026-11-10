#ifndef LOCAL_DB_HPP
#define LOCAL_DB_HPP

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVariantList>

class QSqlDatabase;
class QSqlQuery;

/**
 * SQLite 本地数据库封装 —— 对应 BBTube 的本地存储层。
 *
 * 【数据库位置】沙箱根目录 xiaohongshu.db（QDir::homePath() 下）。
 * 【为什么不在 /accounts/1000/shared】那需要 access_shared 权限，devMode
 * 未签名包会在首次启动时弹权限窗，授权后进程重启，形成"弹窗→闪退"循环。
 * 所有持久化数据都留在应用私有沙箱里。
 *
 * 【表结构】
 *   notes(bvid PRIMARY KEY, title, desc, coverUrl, ownerName, ownerMid,
 *         duration, likeCount, collectCount, commentCount, createTime,
 *         rawJson, updatedAt)
 *   collections(cid PRIMARY KEY, title, cover, mediaCount, privacy, updatedAt)
 *   history(bvid, viewAt, title, coverUrl, ownerName, PRIMARY KEY(bvid, viewAt))
 *   follows(mid PRIMARY KEY, name, faceUrl, followedAt)
 *   settings(key PRIMARY KEY, value)
 *
 * 所有公开方法都是 Q_INVOKABLE / Q_PROPERTY，QML 可直接调用。
 * 内部用 QSqlDatabase/QSqlQuery，线程安全：所有操作都在主线程完成，
 * 不跨线程访问数据库（避免 BB10 上 SQLite 多线程锁问题）。
 */
class LocalDb : public QObject {
    Q_OBJECT

public:
    explicit LocalDb(QObject* parent = 0);
    ~LocalDb();

    /** 打开/创建数据库，建表。失败时 qDebug 输出错误，后续操作静默无效。 */
    Q_INVOKABLE bool open();

    /** 数据库是否已成功打开。 */
    Q_INVOKABLE bool isOpen() const;

    /** 关闭数据库。 */
    Q_INVOKABLE void close();

    /* ================= 笔记缓存 ================= */

    /** 把一条笔记的完整快照写入缓存（已存在则替换）。rawJson 为接口返回的原始 JSON。 */
    Q_INVOKABLE void upsertNote(const QString& bvid, const QVariantMap& note,
                                const QString& rawJson);

    /** 根据 bvid 读取缓存的笔记快照；未缓存时返回空 QVariantMap。 */
    Q_INVOKABLE QVariantMap getNote(const QString& bvid) const;

    /** 读取最近 N 条缓存的笔记（按 updatedAt 降序），供"最近浏览"用。 */
    Q_INVOKABLE QVariantList recentNotes(int limit = 20) const;

    /* ================= 收藏夹 ================= */

    Q_INVOKABLE void upsertCollection(const QString& cid, const QVariantMap& coll);
    Q_INVOKABLE QVariantMap getCollection(const QString& cid) const;
    Q_INVOKABLE QVariantList collections() const;
    Q_INVOKABLE void removeCollection(const QString& cid);

    /* ================= 观看历史 ================= */

    /** 记录一次观看（bvid + 元数据），同一 bvid 重复观看会更新 viewAt。 */
    Q_INVOKABLE void addHistory(const QString& bvid, const QVariantMap& meta);
    Q_INVOKABLE QVariantList history(int limit = 50) const;
    Q_INVOKABLE void clearHistory();

    /* ================= 关注 ================= */

    Q_INVOKABLE void upsertFollow(const QString& mid, const QVariantMap& user);
    Q_INVOKABLE QVariantMap getFollow(const QString& mid) const;
    Q_INVOKABLE QVariantList follows() const;
    Q_INVOKABLE void removeFollow(const QString& mid);

    /* ================= 设置键值 ================= */

    Q_INVOKABLE void setSetting(const QString& key, const QString& value);
    Q_INVOKABLE QString getSetting(const QString& key,
                                   const QString& defaultValue = QString()) const;

signals:
    void opened(bool ok);

private:
    bool ensureOpen();
    void createTables();

    QSqlDatabase* m_db;
    bool m_opened;
};

#endif // LOCAL_DB_HPP
