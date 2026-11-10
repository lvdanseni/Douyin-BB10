#include "note_model.hpp"
#include "douyin_api.hpp"

#include <bb/cascades/Application>
#include <QNetworkAccessManager>
#include <bb/data/JsonDataAccess>
#include <QDateTime>
#include <QDir>

using namespace bb::cascades;
using namespace bb::data;

NoteModel::NoteModel(Kind kind, QObject* parent)
    : ArrayDataModel(parent)
    , m_kind(kind)
    , m_loadedLoggedIn(false)
    , m_page(0)
    , m_hasMore(true)
    , m_loading(false)
    , m_loadingMore(false)
    , m_navConnected(false)
    , m_netConnected(false)
    , m_imgNetConnected(false)
    , m_imgSeq(0)
    , m_imgOk(0)
    , m_imgTotal(0)
    , m_loadTimer(0)
{
    switch (m_kind) {
    case Rcmd:     m_tagPrefix = QString("rcmd"); break;
    case Discover: m_tagPrefix = QString("discover"); break;
    case Search:   m_tagPrefix = QString("search"); break;
    case Follow:   m_tagPrefix = QString("follow"); break;
    case History:  m_tagPrefix = QString("history"); break;
    case Collect:  m_tagPrefix = QString("collect"); break;
    }

    m_loadTimer = new QTimer(this);
    m_loadTimer->setSingleShot(true);
    m_loadTimer->setInterval(20000);
    connect(m_loadTimer, SIGNAL(timeout()), this, SLOT(onLoadTimeout()));
}

void NoteModel::load()
{
    if (m_loading) return;
    reset();
    m_page = 0;
    m_hasMore = true;
    m_error.clear();
    emit errorChanged();
    fetch(false);
}

void NoteModel::loadMore()
{
    if (m_loading || m_loadingMore || !m_hasMore) return;
    fetch(true);
}

void NoteModel::reloadIfStale()
{
    // 只在"列表为空"或"登录态变了"时才重新拉
    if (m_items.isEmpty() || m_loadedLoggedIn != DouyinApi::instance()->loggedIn()) {
        load();
    }
}

void NoteModel::setKeyword(const QString& keyword)
{
    if (m_keyword == keyword) return;
    m_keyword = keyword;
    load();
}

void NoteModel::setUserId(const QString& userId)
{
    if (m_userId == userId) return;
    m_userId = userId;
    load();
}

void NoteModel::setCollectId(const QString& collectId)
{
    if (m_collectId == collectId) return;
    m_collectId = collectId;
    load();
}

void NoteModel::clearFeed()
{
    reset();
}

QString NoteModel::noteIdAt(int row) const
{
    if (row < 0 || row >= m_items.size()) return QString();
    return m_items.at(row).toMap().value("noteId").toString();
}

void NoteModel::fetch(bool more)
{
    if (!DouyinApi::instance()) return;

    m_loading = !more;
    m_loadingMore = more;
    emit loadingChanged();
    emit loadingMoreChanged();

    // 组装请求 URL（简化：实际接口路径在 T03 中由 DouyinApi::getSigned 统一构造）
    QString path;
    QVariantMap params;
    QString tag;

    switch (m_kind) {
    case Rcmd:
        path = "/api/sns/v10/recommend";
        params.insert("page", m_page);
        params.insert("pageSize", 20);
        tag = m_tagPrefix + ":feed";
        break;
    case Discover:
        path = "/api/sns/v10/discover";
        params.insert("page", m_page);
        params.insert("pageSize", 20);
        tag = m_tagPrefix + ":feed";
        break;
    case Search:
        path = "/api/sns/v10/search";
        params.insert("keyword", m_keyword);
        params.insert("page", m_page);
        params.insert("pageSize", 20);
        tag = m_tagPrefix + ":search";
        break;
    case Follow:
        path = "/api/sns/v10/follow/feed";
        params.insert("offset", m_followOffset);
        params.insert("pageSize", 20);
        tag = m_tagPrefix + ":feed";
        break;
    case History:
        path = "/api/sns/v10/history";
        params.insert("max", m_page);
        params.insert("count", 20);
        tag = m_tagPrefix + ":feed";
        break;
    case Collect:
        path = "/api/sns/v10/collection/notes";
        params.insert("collectId", m_collectId);
        params.insert("page", m_page);
        params.insert("pageSize", 20);
        tag = m_tagPrefix + ":feed";
        break;
    }

    m_loadTimer->start();
    DouyinApi::instance()->getSigned(path, params, tag);
}

void NoteModel::onReply(const QString& json, bool ok, bool more)
{
    m_loadTimer->stop();

    m_loading = false;
    m_loadingMore = false;
    emit loadingChanged();
    emit loadingMoreChanged();

    if (!ok) {
        m_error = QString("网络请求失败");
        emit errorChanged();
        return;
    }

    JsonDataAccess jda;
    QVariant v = jda.loadFromBuffer(json.toUtf8());
    if (!v.isValid()) {
        m_error = QString("JSON 解析失败");
        emit errorChanged();
        return;
    }

    QVariantMap root = v.toMap();
    int code = root.value("code").toInt(0);
    if (code != 0) {
        m_error = root.value("message").toString();
        emit errorChanged();
        return;
    }

    QVariantMap data = root.value("data").toMap();
    QVariantList items = data.value("items").toList();

    QVariantList variantItems;
    for (int i = 0; i < items.size(); ++i) {
        QVariantMap obj = items.at(i).toMap();
        QVariantMap item;
        item["noteId"] = obj.value("noteId").toString();
        item["title"] = obj.value("title").toString();
        item["coverUrl"] = obj.value("coverUrl").toString();
        item["durationText"] = obj.value("durationText").toString();
        item["metaText"] = obj.value("metaText").toString();
        item["typeName"] = obj.value("typeName").toString();
        item["likeCount"] = obj.value("likeCount").toDouble();
        item["collectCount"] = obj.value("collectCount").toDouble();
        item["commentCount"] = obj.value("commentCount").toDouble();
        item["api"] = QVariant::fromValue(static_cast<QObject*>(DouyinApi::instance()));
        variantItems.append(item);
    }

    if (more) {
        appendItems(variantItems);
    } else {
        reset();
        appendItems(variantItems);
    }

    m_hasMore = data.value("hasMore").toBool();
    m_page++;

    // 封面预取
    m_imgTotal += variantItems.size();
    for (int i = 0; i < variantItems.size(); ++i) {
        QVariantMap item = variantItems.at(i).toMap();
        QString coverUrl = item.value("coverUrl").toString();
        if (!coverUrl.isEmpty()) {
            QString absUrl = DouyinApi::coverUrl(coverUrl);
            QString path = DouyinApi::coverCachePath(absUrl);
            QString tag = m_tagPrefix + ":img" + QString::number(m_imgSeq++);
            NoteImgPending pending;
            pending.row = m_items.size() - variantItems.size() + i;
            pending.url = absUrl;
            pending.path = path;
            pending.retry = 0;
            m_pendingImg.insert(tag, pending);
            DouyinApi::instance()->download(absUrl, path, tag, 0, -1, -1, true, 0);
        }
    }

    connectImageNet();
    emit countChanged();
    emit coverStatsChanged();
}

void NoteModel::appendItems(const QVariantList& items)
{
    int base = m_items.size();
    for (int i = 0; i < items.size(); ++i) {
        m_items.append(items.at(i));
    }
    // ArrayDataModel 会自动通过 modelReset / itemsChanged 通知 QML
    emit countChanged();
}

void NoteModel::reset()
{
    m_items.clear();
    m_pendingImg.clear();
    clear();
    m_imgOk = 0;
    m_imgTotal = 0;
    emit countChanged();
    emit coverStatsChanged();
}

void NoteModel::connectNet()
{
    if (m_netConnected) return;
    QObject::connect(DouyinApi::instance(), SIGNAL(netReply(QString,QString,bool)),
                     this, SLOT(onNetReply(QString,QString,bool)));
    m_netConnected = true;
}

void NoteModel::connectImageNet()
{
    if (m_imgNetConnected) return;
    QObject::connect(DouyinApi::instance(), SIGNAL(downloadFinished(QString,QString,bool)),
                     this, SLOT(onImageDownloadFinished(QString,QString,bool)));
    m_imgNetConnected = true;
}

void NoteModel::onNetReply(const QString& tag, const QString& json, bool ok)
{
    // 路由：只有 tag 匹配本模型前缀的才处理
    if (!tag.startsWith(m_tagPrefix + ":")) return;

    bool more = tag.endsWith(":more");
    onReply(json, ok, more);
}

void NoteModel::onNavFailed()
{
    m_error = QString("网络不可用");
    emit errorChanged();
    m_loading = false;
    m_loadingMore = false;
    emit loadingChanged();
    emit loadingMoreChanged();
}

void NoteModel::onLoadTimeout()
{
    if (m_loading || m_loadingMore) {
        m_error = QString("请求超时");
        emit errorChanged();
        m_loading = false;
        m_loadingMore = false;
        emit loadingChanged();
        emit loadingMoreChanged();
    }
}

QString NoteModel::imageCachePath(const QString& url) const
{
    return DouyinApi::coverCachePath(url);
}

void NoteModel::onImageDownloadFinished(const QString& tag, const QString& path, bool ok)
{
    if (!m_pendingImg.contains(tag)) return;
    NoteImgPending pending = m_pendingImg.take(tag);

    if (ok && DouyinApi::isImageFile(path)) {
        m_imgOk++;
        // 封面已就绪，bump coverVersion 让 QML 绑定重算（v102 机制）
        DouyinApi::instance()->coverVersion(); // 触发 coverVersionChanged 通过 BiliApi 内部
        emit coverStatsChanged();
    } else if (pending.retry < 2) {
        pending.retry++;
        m_pendingImg.insert(tag, pending);
        DouyinApi::instance()->download(pending.url, pending.path, tag, 0, -1, -1, true, 0);
    }

    emit coverStatsChanged();
}
