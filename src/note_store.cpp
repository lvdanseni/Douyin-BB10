#include "note_store.hpp"
#include "note_model.hpp"
#include "douyin_api.hpp"
#include "bbplayer_wrapper.hpp"
#include "video_store.hpp"
#include "local_db.hpp"

#include <bb/cascades/Application>
#include <bb/data/JsonDataAccess>
#include <QDateTime>
#include <QDir>
#include <QDebug>

using namespace bb::cascades;
using namespace bb::data;

NoteStore::NoteStore(QObject* parent)
    : QObject(parent)
    , m_loading(false)
    , m_netConnected(false)
    , m_liked(false)
    , m_collectCount(0)
    , m_collected(false)
    , m_followed(false)
    , m_likeCount(0)
    , m_collectTotal(0)
    , m_dlState(0)
    , m_dlGen(0)
    , m_dlForPlayback(false)
    , m_dlWanted(false)
    , m_dlProgressMs(0)
    , m_dlBestGot(0)
    , m_dlPending(0)
    , m_dlSegmented(false)
    , m_dlLastBytes(0)
    , m_dlLastMs(0)
    , m_dlTimer(0)
    , m_detailRetry(0)
    , m_proxy(0)
    , m_streamSize(0)
    , m_usingProxy(false)
    , m_commentCount(0)
    , m_localDb(0)
    , m_diagLast()
{
    m_tagPrefix = QString("note");
    m_commentModel = new bb::cascades::ArrayDataModel(this);
    m_relatedModel = new NoteModel(NoteModel::Rcmd, this);
    m_localDb = new LocalDb(this);

    connectNet();
}

void NoteStore::load(const QString& noteId)
{
    if (m_loading || noteId.isEmpty()) return;
    m_noteId = noteId;
    m_loading = true;
    m_error.clear();
    emit loadingChanged();
    emit errorChanged();

    m_detail.clear();
    m_commentModel->clear();
    m_liked = m_collected = m_followed = false;
    emit detailChanged();
    emit interactChanged();

    m_detailRetry = 0;
    DouyinApi::instance()->getSigned(DouyinApi::noteDetailApi(noteId), QVariantMap(), "__detail__");
}

QObject* NoteStore::relatedModel() const
{
    return m_relatedModel;
}

void NoteStore::toggleLike()
{
    if (!DouyinApi::instance()->loggedIn()) {
        setTip(QString::fromUtf8("请先登录"));
        return;
    }
    QString url = DouyinApi::noteLikeApi(m_noteId);
    QVariantMap params;
    params.insert("note_id", m_noteId);
    DouyinApi::instance()->post(url, QString(), "__like__");
}

void NoteStore::toggleCollect()
{
    if (!DouyinApi::instance()->loggedIn()) {
        setTip(QString::fromUtf8("请先登录"));
        return;
    }
    QString url = DouyinApi::noteCollectApi(m_noteId);
    QVariantMap params;
    params.insert("note_id", m_noteId);
    DouyinApi::instance()->post(url, QString(), "__collect__");
}

void NoteStore::toggleFollow()
{
    if (!DouyinApi::instance()->loggedIn()) {
        setTip(QString::fromUtf8("请先登录"));
        return;
    }
    QString url = DouyinApi::userFollowApi(m_ownerMid);
    QVariantMap params;
    params.insert("user_id", m_ownerMid);
    DouyinApi::instance()->post(url, QString(), "__follow__");
}

void NoteStore::shareNote()
{
    QString link = QString("https://www.xiaohongshu.com/explore/%1").arg(m_noteId);
    emit detailReady(m_detail);
}

void NoteStore::downloadCurrent()
{
    if (m_streamUrl.isEmpty()) {
        setTip(QString::fromUtf8("暂无可下载的媒体"));
        return;
    }
    downloadMedia(m_streamUrl, m_streamSize, false);
}

void NoteStore::downloadMedia(const QString& url, qint64 expectedBytes,
                              bool forPlayback)
{
    if (url.isEmpty()) return;
    m_dlForPlayback = forPlayback;
    m_dlWanted = true;
    m_dlState = 1;
    emit dlStateChanged();

    QDir dir(QDir::homePath());
    QString finalPath = dir.absoluteFilePath(
        QString("videos/%1.mp4").arg(m_noteId));
    if (!dir.exists("videos")) dir.mkpath("videos");

    startSingle(url, finalPath, expectedBytes);
}

void NoteStore::stopStream()
{
    m_usingProxy = false;
    if (m_proxy) {
        m_proxy->stop();
    }
}

void NoteStore::fallbackToDownload()
{
    if (!m_streamUrl.isEmpty()) {
        downloadMedia(m_streamUrl, m_streamSize, true);
    }
}

void NoteStore::connectNet()
{
    if (m_netConnected) return;
    QObject::connect(DouyinApi::instance(), SIGNAL(netReply(QString,QString,bool)),
                     this, SLOT(onNetReply(QString,QString,bool)));
    QObject::connect(DouyinApi::instance(), SIGNAL(downloadFinished(QString,QString,bool)),
                     this, SLOT(onDownloadFinished(QString,QString,bool)));
    m_netConnected = true;
}

void NoteStore::onNetReply(const QString& tag, const QString& json, bool ok)
{
    if (!tag.startsWith("__") || !tag.contains("__")) return;

    if (tag == "__detail__") {
        onDetailReply(json, ok);
    } else if (tag == "__media__") {
        onMediaReply(json, ok);
    } else if (tag == "__comment__") {
        onCommentReply(json, ok);
    } else if (tag == "__like__") {
        onInteractReply("like", json, ok);
    } else if (tag == "__collect__") {
        onInteractReply("collect", json, ok);
    } else if (tag == "__follow__") {
        onInteractReply("follow", json, ok);
    }
}

void NoteStore::onDetailReply(const QString& json, bool ok)
{
    m_loading = false;
    emit loadingChanged();

    if (!ok || json.isEmpty()) {
        m_error = QString::fromUtf8("网络请求失败");
        emit errorChanged();
        emit loadFailed(m_error);
        return;
    }

    JsonDataAccess jda;
    QVariant v = jda.loadFromBuffer(json.toUtf8());
    if (!v.isValid()) {
        m_error = QString::fromUtf8("JSON 解析失败");
        emit errorChanged();
        emit loadFailed(m_error);
        return;
    }

    QVariantMap root = v.toMap();
    int code = root.value("code").toInt(0);
    if (code != 0) {
        m_error = root.value("message").toString();
        emit errorChanged();
        emit loadFailed(m_error);
        return;
    }

    QVariantMap data = root.value("data").toMap();
    QVariantMap detail;
    detail["noteId"] = data.value("noteId").toString();
    detail["title"] = data.value("title").toString();
    detail["desc"] = data.value("desc").toString();
    detail["coverUrl"] = data.value("coverUrl").toString();
    detail["ownerName"] = data.value("ownerName").toString();
    detail["ownerMid"] = data.value("ownerMid").toString();
    detail["ownerFace"] = data.value("ownerFace").toString();
    detail["likeCount"] = data.value("likeCount").toDouble();
    detail["collectCount"] = data.value("collectCount").toDouble();
    detail["commentCount"] = data.value("commentCount").toDouble();
    detail["duration"] = data.value("duration").toDouble();
    detail["createTime"] = data.value("createTime").toDouble();

    // 图片列表
    QVariantList images;
    QVariantList imgArray = data.value("images").toList();
    for (int i = 0; i < imgArray.size(); ++i) {
        images.append(imgArray.at(i).toString());
    }
    detail["images"] = images;

    // 视频 URL（如有）
    detail["videoUrl"] = data.value("videoUrl").toString();

    m_detail = detail;
    m_ownerMid = detail.value("ownerMid").toString();

    // 互动状态
    m_liked = data.value("liked").toBool();
    m_collected = data.value("collected").toBool();
    m_followed = data.value("followed").toBool();
    m_likeCount = detail.value("likeCount").toLongLong();
    m_collectTotal = detail.value("collectCount").toLongLong();

    m_commentCount = data.value("commentCount").toInt(0);

    emit detailChanged();
    emit interactChanged();
    emit commentCountChanged();
    emit detailReady(detail);

    // 缓存到 LocalDb
    if (m_localDb) {
        m_localDb->upsertNote(m_noteId, detail, json);
    }

    // 记录观看历史
    if (m_localDb) {
        QVariantMap meta;
        meta["title"] = detail.value("title");
        meta["coverUrl"] = detail.value("coverUrl");
        meta["ownerName"] = detail.value("ownerName");
        m_localDb->addHistory(m_noteId, meta);
    }

    // 拉取媒体直链（如有视频）
    if (!detail.value("videoUrl").toString().isEmpty()) {
        DouyinApi::instance()->getSigned(DouyinApi::noteDetailApi(m_noteId) + "/media",
                                       QVariantMap(), "__media__");
    }

    // 拉取相关推荐
    QVariantMap relParams;
    relParams.insert("note_id", m_noteId);
    DouyinApi::instance()->getSigned(DouyinApi::noteFeedApi(), relParams, "__related__");

    // 拉取评论
    QVariantMap cParams;
    cParams.insert("note_id", m_noteId);
    cParams.insert("page", 1);
    cParams.insert("page_size", 20);
    DouyinApi::instance()->getSigned("/api/sns/v10/comments", cParams, "__comment__");
}

void NoteStore::onMediaReply(const QString& json, bool ok)
{
    if (!ok || json.isEmpty()) {
        m_error = QString::fromUtf8("获取媒体直链失败");
        emit errorChanged();
        return;
    }

    JsonDataAccess jda;
    QVariant v = jda.loadFromBuffer(json.toUtf8());
    if (!v.isValid()) return;
    QVariantMap root = v.toMap();
    if (root.value("code").toInt(0) != 0) return;

    QVariantMap data = root.value("data").toMap();
    QString mediaUrl = data.value("url").toString();
    if (!mediaUrl.isEmpty()) {
        m_streamUrl = mediaUrl;
        m_streamSize = data.value("size").toDouble(0);
        emit mediaUrlReady(mediaUrl);
        emit videoReadyToPlay(mediaUrl);
    }
}

void NoteStore::onCommentReply(const QString& json, bool ok)
{
    if (!ok || json.isEmpty()) return;

    JsonDataAccess jda;
    QVariant v = jda.loadFromBuffer(json.toUtf8());
    if (!v.isValid()) return;
    QVariantMap root = v.toMap();
    if (root.value("code").toInt(0) != 0) return;

    QVariantList arr = root.value("data").toList();
    QVariantList comments;
    for (int i = 0; i < arr.size(); ++i) {
        QVariantMap obj = arr.at(i).toMap();
        QVariantMap c;
        c["content"] = obj.value("content").toString();
        c["userName"] = obj.value("userName").toString();
        c["userFace"] = obj.value("userFace").toString();
        c["likeCount"] = obj.value("likeCount").toDouble();
        c["createTime"] = obj.value("createTime").toDouble();
        comments.append(c);
    }
    m_commentModel->clear();
    for (int i = 0; i < comments.size(); ++i) {
        m_commentModel->append(comments.at(i));
    }
    emit commentsReady(comments);
}

void NoteStore::onInteractReply(const QString& suffix, const QString& json, bool ok)
{
    bool success = ok && !json.isEmpty();
    if (success) {
        JsonDataAccess jda;
        QVariant v = jda.loadFromBuffer(json.toUtf8());
        success = v.isValid() && v.toMap().value("code").toInt(0) == 0;
    }

    if (suffix == "like") {
        if (success) {
            m_liked = !m_liked;
            m_likeCount += m_liked ? 1 : -1;
            setTip(m_liked ? QString::fromUtf8("已点赞") : QString::fromUtf8("已取消点赞"));
        } else {
            setTip(QString::fromUtf8("点赞失败"));
        }
    } else if (suffix == "collect") {
        if (success) {
            m_collected = !m_collected;
            setTip(m_collected ? QString::fromUtf8("已收藏") : QString::fromUtf8("已取消收藏"));
        } else {
            setTip(QString::fromUtf8("收藏失败"));
        }
    } else if (suffix == "follow") {
        if (success) {
            m_followed = !m_followed;
            setTip(m_followed ? QString::fromUtf8("已关注") : QString::fromUtf8("已取关"));
        } else {
            setTip(QString::fromUtf8("关注失败"));
        }
    }
    emit interactChanged();
}

void NoteStore::onDownloadFinished(const QString& tag, const QString& path, bool ok)
{
    Q_UNUSED(tag);
    if (ok) {
        m_dlState = 2;
        emit dlStateChanged();
        onLocalVideoReady(path);
    } else {
        onDownloadFailedReset(QString::fromUtf8("下载失败"));
    }
}

void NoteStore::onDownloadTick()
{
    Q_UNUSED(this);
}

void NoteStore::clearTip()
{
    setTip(QString());
}

void NoteStore::onStreamFailed(const QString& reason)
{
    Q_UNUSED(reason);
    m_usingProxy = false;
    fallbackToDownload();
}

void NoteStore::finishLoading()
{
    m_loading = false;
    emit loadingChanged();
}

void NoteStore::setTip(const QString& t)
{
    m_interactTip = t;
    emit interactChanged();
    if (m_tipTimer) {
        m_tipTimer->stop();
        m_tipTimer->start();
    }
}

void NoteStore::setCountText(const QString& key, qint64 n)
{
    Q_UNUSED(key);
    Q_UNUSED(n);
}

void NoteStore::fetchCollectFolders()
{
    // 收藏夹目录获取（用于收藏操作）
    DouyinApi::instance()->getSigned("/api/sns/v10/collection/folders", QVariantMap(), "__folders__");
}

void NoteStore::doCollectDeal(bool add)
{
    Q_UNUSED(add);
}

void NoteStore::onLocalVideoReady(const QString& path)
{
    if (m_localDb) {
        m_localDb->upsertNote(m_dlNoteId.isEmpty() ? m_noteId : m_dlNoteId, m_detail, QString());
    }
    emit localVideoReady(path);
}

/** 把 QVariant 简单序列化为 JSON 字符串（BB10 无 QtJson 模块，手写兜底）。 */
static QString variantToJson(const QVariant& v)
{
    switch (v.type()) {
    case QVariant::String:
        return QString::fromLatin1("\"") + v.toString() + QString::fromLatin1("\"");
    case QVariant::Double:
        return QString::number(v.toDouble(), 'f', 2);
    case QVariant::Int:
    case QVariant::UInt:
    case QVariant::LongLong:
    case QVariant::ULongLong:
        return QString::number(v.toLongLong());
    case QVariant::Bool:
        return v.toBool() ? QString::fromLatin1("true") : QString::fromLatin1("false");
    case QVariant::Map: {
        QVariantMap m = v.toMap();
        QStringList parts;
        QMapIterator<QString, QVariant> it(m);
        while (it.hasNext()) {
            it.next();
            parts << QString::fromLatin1("\"") + it.key() + QString::fromLatin1("\":") + variantToJson(it.value());
        }
        return QString::fromLatin1("{") + parts.join(QString::fromLatin1(",")) + QString::fromLatin1("}");
    }
    case QVariant::List: {
        QVariantList l = v.toList();
        QStringList parts;
        for (int i = 0; i < l.size(); ++i)
            parts << variantToJson(l.at(i));
        return QString::fromLatin1("[") + parts.join(QString::fromLatin1(",")) + QString::fromLatin1("]");
    }
    default:
        return QString::fromLatin1("\"") + v.toString() + QString::fromLatin1("\"");
    }
}

void NoteStore::writeDownloadMeta()
{
    QDir dir(QDir::homePath());
    if (!dir.exists("videos")) dir.mkpath("videos");
    QString metaPath = dir.absoluteFilePath(
        QString("videos/%1.json").arg(m_dlNoteId.isEmpty() ? m_noteId : m_dlNoteId));
    QFile f(metaPath);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(variantToJson(m_dlMeta).toUtf8());
        f.close();
    }
}

void NoteStore::onDownloadFailedReset(const QString& reason)
{
    m_dlState = 0;
    emit dlStateChanged();
    m_error = reason;
    emit errorChanged();
    emit downloadFailed(reason);
}

void NoteStore::startSegmented(const QString&, const QString&, qint64) {}
void NoteStore::startSingle(const QString&, const QString&, qint64) {}
void NoteStore::finishSegmented() {}
void NoteStore::abortDownload(const QString&) {}
void NoteStore::startStream(const QString&, qint64) {}
