#include "local_db.hpp"

#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlQuery>
#include <QtSql/QSqlError>
#include <QDir>
#include <QFile>
#include <QDateTime>
#include <QDebug>

LocalDb::LocalDb(QObject* parent)
    : QObject(parent)
    , m_db(0)
    , m_opened(false)
{
}

LocalDb::~LocalDb()
{
    close();
    delete m_db;
    m_db = 0;
}

bool LocalDb::open()
{
    if (m_opened) return true;

    m_db = new QSqlDatabase(QSqlDatabase::addDatabase("QSQLITE", "douyin_conn"));
    QDir dir(QDir::homePath());
    QString dbPath = dir.absoluteFilePath("douyin.db");
    m_db->setDatabaseName(dbPath);

    if (!m_db->open()) {
        qDebug("LocalDb: 打开数据库失败: %s",
               qPrintable(m_db->lastError().text()));
        m_opened = false;
        emit opened(false);
        return false;
    }

    createTables();
    m_opened = true;
    emit opened(true);
    qDebug("LocalDb: 数据库打开成功 %s", qPrintable(dbPath));
    return true;
}

bool LocalDb::isOpen() const
{
    return m_opened;
}

void LocalDb::close()
{
    if (m_db && m_db->isOpen()) {
        m_db->close();
    }
    m_opened = false;
}

bool LocalDb::ensureOpen()
{
    if (!m_opened) {
        return open();
    }
    return true;
}

void LocalDb::createTables()
{
    if (!m_db || !m_db->isOpen()) return;

    QSqlQuery q(*m_db);

    // 笔记缓存表
    q.exec("CREATE TABLE IF NOT EXISTS notes ("
           "bvid TEXT PRIMARY KEY,"
           "title TEXT,"
           "desc TEXT,"
           "coverUrl TEXT,"
           "ownerName TEXT,"
           "ownerMid TEXT,"
           "duration INTEGER DEFAULT 0,"
           "likeCount INTEGER DEFAULT 0,"
           "collectCount INTEGER DEFAULT 0,"
           "commentCount INTEGER DEFAULT 0,"
           "createTime INTEGER DEFAULT 0,"
           "rawJson TEXT,"
           "updatedAt INTEGER DEFAULT 0)");

    // 收藏夹表
    q.exec("CREATE TABLE IF NOT EXISTS collections ("
           "cid TEXT PRIMARY KEY,"
           "title TEXT,"
           "cover TEXT,"
           "mediaCount INTEGER DEFAULT 0,"
           "privacy INTEGER DEFAULT 0,"
           "updatedAt INTEGER DEFAULT 0)");

    // 观看历史表（同一 bvid 可多次观看，按 viewAt 区分）
    q.exec("CREATE TABLE IF NOT EXISTS history ("
           "bvid TEXT,"
           "viewAt INTEGER,"
           "title TEXT,"
           "coverUrl TEXT,"
           "ownerName TEXT,"
           "PRIMARY KEY(bvid, viewAt))");

    // 关注用户表
    q.exec("CREATE TABLE IF NOT EXISTS follows ("
           "mid TEXT PRIMARY KEY,"
           "name TEXT,"
           "faceUrl TEXT,"
           "followedAt INTEGER DEFAULT 0)");

    // 设置键值表
    q.exec("CREATE TABLE IF NOT EXISTS settings ("
           "key TEXT PRIMARY KEY,"
           "value TEXT)");

    if (q.lastError().isValid()) {
        qDebug("LocalDb: 建表错误: %s", qPrintable(q.lastError().text()));
    }
}

/* ---------------- 笔记 ---------------- */

void LocalDb::upsertNote(const QString& bvid, const QVariantMap& note,
                         const QString& rawJson)
{
    if (!ensureOpen()) return;

    QSqlQuery q(*m_db);
    q.prepare("INSERT OR REPLACE INTO notes (bvid, title, desc, coverUrl, ownerName, "
              "ownerMid, duration, likeCount, collectCount, commentCount, createTime, "
              "rawJson, updatedAt) "
              "VALUES (:bvid, :title, :desc, :coverUrl, :ownerName, :ownerMid, "
              ":duration, :likeCount, :collectCount, :commentCount, :createTime, "
              ":rawJson, :updatedAt)");
    q.bindValue(":bvid", bvid);
    q.bindValue(":title", note.value("title").toString());
    q.bindValue(":desc", note.value("desc").toString());
    q.bindValue(":coverUrl", note.value("coverUrl").toString());
    q.bindValue(":ownerName", note.value("ownerName").toString());
    q.bindValue(":ownerMid", note.value("ownerMid").toString());
    q.bindValue(":duration", note.value("duration").toLongLong());
    q.bindValue(":likeCount", note.value("likeCount").toLongLong());
    q.bindValue(":collectCount", note.value("collectCount").toLongLong());
    q.bindValue(":commentCount", note.value("commentCount").toLongLong());
    q.bindValue(":createTime", note.value("createTime").toLongLong());
    q.bindValue(":rawJson", rawJson);
    q.bindValue(":updatedAt", (qint64)QDateTime::currentMSecsSinceEpoch());
    if (!q.exec()) {
        qDebug("LocalDb: upsertNote 失败: %s", qPrintable(q.lastError().text()));
    }
}

QVariantMap LocalDb::getNote(const QString& bvid) const
{
    QVariantMap empty;
    if (!m_db || !m_db->isOpen()) return empty;

    QSqlQuery q(*m_db);
    q.prepare("SELECT title, desc, coverUrl, ownerName, ownerMid, duration, "
              "likeCount, collectCount, commentCount, createTime, rawJson "
              "FROM notes WHERE bvid = :bvid");
    q.bindValue(":bvid", bvid);
    if (!q.exec() || !q.next()) return empty;

    QVariantMap note;
    note["title"] = q.value(0).toString();
    note["desc"] = q.value(1).toString();
    note["coverUrl"] = q.value(2).toString();
    note["ownerName"] = q.value(3).toString();
    note["ownerMid"] = q.value(4).toString();
    note["duration"] = q.value(5).toLongLong();
    note["likeCount"] = q.value(6).toLongLong();
    note["collectCount"] = q.value(7).toLongLong();
    note["commentCount"] = q.value(8).toLongLong();
    note["createTime"] = q.value(9).toLongLong();
    note["rawJson"] = q.value(10).toString();
    return note;
}

QVariantList LocalDb::recentNotes(int limit) const
{
    QVariantList list;
    if (!m_db || !m_db->isOpen()) return list;

    QSqlQuery q(*m_db);
    q.prepare("SELECT bvid, title, coverUrl, ownerName, likeCount, updatedAt "
              "FROM notes ORDER BY updatedAt DESC LIMIT :limit");
    q.bindValue(":limit", limit);
    if (!q.exec()) return list;

    while (q.next()) {
        QVariantMap item;
        item["bvid"] = q.value(0).toString();
        item["title"] = q.value(1).toString();
        item["coverUrl"] = q.value(2).toString();
        item["ownerName"] = q.value(3).toString();
        item["likeCount"] = q.value(4).toLongLong();
        item["updatedAt"] = q.value(5).toLongLong();
        list.append(item);
    }
    return list;
}

/* ---------------- 收藏夹 ---------------- */

void LocalDb::upsertCollection(const QString& cid, const QVariantMap& coll)
{
    if (!ensureOpen()) return;

    QSqlQuery q(*m_db);
    q.prepare("INSERT OR REPLACE INTO collections (cid, title, cover, mediaCount, privacy, updatedAt) "
              "VALUES (:cid, :title, :cover, :mediaCount, :privacy, :updatedAt)");
    q.bindValue(":cid", cid);
    q.bindValue(":title", coll.value("title").toString());
    q.bindValue(":cover", coll.value("cover").toString());
    q.bindValue(":mediaCount", coll.value("mediaCount").toInt());
    q.bindValue(":privacy", coll.value("privacy").toInt());
    q.bindValue(":updatedAt", (qint64)QDateTime::currentMSecsSinceEpoch());
    if (!q.exec()) {
        qDebug("LocalDb: upsertCollection 失败: %s", qPrintable(q.lastError().text()));
    }
}

QVariantMap LocalDb::getCollection(const QString& cid) const
{
    QVariantMap empty;
    if (!m_db || !m_db->isOpen()) return empty;

    QSqlQuery q(*m_db);
    q.prepare("SELECT title, cover, mediaCount, privacy, updatedAt "
              "FROM collections WHERE cid = :cid");
    q.bindValue(":cid", cid);
    if (!q.exec() || !q.next()) return empty;

    QVariantMap coll;
    coll["title"] = q.value(0).toString();
    coll["cover"] = q.value(1).toString();
    coll["mediaCount"] = q.value(2).toInt();
    coll["privacy"] = q.value(3).toInt();
    coll["updatedAt"] = q.value(4).toLongLong();
    return coll;
}

QVariantList LocalDb::collections() const
{
    QVariantList list;
    if (!m_db || !m_db->isOpen()) return list;

    QSqlQuery q(*m_db);
    if (!q.exec("SELECT cid, title, cover, mediaCount, privacy, updatedAt "
                "FROM collections ORDER BY updatedAt DESC")) {
        return list;
    }

    while (q.next()) {
        QVariantMap item;
        item["cid"] = q.value(0).toString();
        item["title"] = q.value(1).toString();
        item["cover"] = q.value(2).toString();
        item["mediaCount"] = q.value(3).toInt();
        item["privacy"] = q.value(4).toInt();
        item["updatedAt"] = q.value(5).toLongLong();
        list.append(item);
    }
    return list;
}

void LocalDb::removeCollection(const QString& cid)
{
    if (!ensureOpen()) return;
    QSqlQuery q(*m_db);
    q.prepare("DELETE FROM collections WHERE cid = :cid");
    q.bindValue(":cid", cid);
    q.exec();
}

/* ---------------- 观看历史 ---------------- */

void LocalDb::addHistory(const QString& bvid, const QVariantMap& meta)
{
    if (!ensureOpen()) return;

    QSqlQuery q(*m_db);
    q.prepare("INSERT OR REPLACE INTO history (bvid, viewAt, title, coverUrl, ownerName) "
              "VALUES (:bvid, :viewAt, :title, :coverUrl, :ownerName)");
    q.bindValue(":bvid", bvid);
    q.bindValue(":viewAt", (qint64)QDateTime::currentMSecsSinceEpoch());
    q.bindValue(":title", meta.value("title").toString());
    q.bindValue(":coverUrl", meta.value("coverUrl").toString());
    q.bindValue(":ownerName", meta.value("ownerName").toString());
    q.exec();
}

QVariantList LocalDb::history(int limit) const
{
    QVariantList list;
    if (!m_db || !m_db->isOpen()) return list;

    QSqlQuery q(*m_db);
    q.prepare("SELECT bvid, viewAt, title, coverUrl, ownerName "
              "FROM history ORDER BY viewAt DESC LIMIT :limit");
    q.bindValue(":limit", limit);
    if (!q.exec()) return list;

    while (q.next()) {
        QVariantMap item;
        item["bvid"] = q.value(0).toString();
        item["viewAt"] = q.value(1).toLongLong();
        item["title"] = q.value(2).toString();
        item["coverUrl"] = q.value(3).toString();
        item["ownerName"] = q.value(4).toString();
        list.append(item);
    }
    return list;
}

void LocalDb::clearHistory()
{
    if (!ensureOpen()) return;
    QSqlQuery q(*m_db);
    q.exec("DELETE FROM history");
}

/* ---------------- 关注 ---------------- */

void LocalDb::upsertFollow(const QString& mid, const QVariantMap& user)
{
    if (!ensureOpen()) return;

    QSqlQuery q(*m_db);
    q.prepare("INSERT OR REPLACE INTO follows (mid, name, faceUrl, followedAt) "
              "VALUES (:mid, :name, :faceUrl, :followedAt)");
    q.bindValue(":mid", mid);
    q.bindValue(":name", user.value("name").toString());
    q.bindValue(":faceUrl", user.value("faceUrl").toString());
    q.bindValue(":followedAt", (qint64)QDateTime::currentMSecsSinceEpoch());
    q.exec();
}

QVariantMap LocalDb::getFollow(const QString& mid) const
{
    QVariantMap empty;
    if (!m_db || !m_db->isOpen()) return empty;

    QSqlQuery q(*m_db);
    q.prepare("SELECT name, faceUrl, followedAt FROM follows WHERE mid = :mid");
    q.bindValue(":mid", mid);
    if (!q.exec() || !q.next()) return empty;

    QVariantMap user;
    user["name"] = q.value(0).toString();
    user["faceUrl"] = q.value(1).toString();
    user["followedAt"] = q.value(2).toLongLong();
    return user;
}

QVariantList LocalDb::follows() const
{
    QVariantList list;
    if (!m_db || !m_db->isOpen()) return list;

    QSqlQuery q(*m_db);
    if (!q.exec("SELECT mid, name, faceUrl, followedAt FROM follows ORDER BY followedAt DESC")) {
        return list;
    }

    while (q.next()) {
        QVariantMap item;
        item["mid"] = q.value(0).toString();
        item["name"] = q.value(1).toString();
        item["faceUrl"] = q.value(2).toString();
        item["followedAt"] = q.value(3).toLongLong();
        list.append(item);
    }
    return list;
}

void LocalDb::removeFollow(const QString& mid)
{
    if (!ensureOpen()) return;
    QSqlQuery q(*m_db);
    q.prepare("DELETE FROM follows WHERE mid = :mid");
    q.bindValue(":mid", mid);
    q.exec();
}

/* ---------------- 设置 ---------------- */

void LocalDb::setSetting(const QString& key, const QString& value)
{
    if (!ensureOpen()) return;
    QSqlQuery q(*m_db);
    q.prepare("INSERT OR REPLACE INTO settings (key, value) VALUES (:key, :value)");
    q.bindValue(":key", key);
    q.bindValue(":value", value);
    q.exec();
}

QString LocalDb::getSetting(const QString& key, const QString& defaultValue) const
{
    if (!m_db || !m_db->isOpen()) return defaultValue;
    QSqlQuery q(*m_db);
    q.prepare("SELECT value FROM settings WHERE key = :key");
    q.bindValue(":key", key);
    if (!q.exec() || !q.next()) return defaultValue;
    return q.value(0).toString();
}
