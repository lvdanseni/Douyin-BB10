#include "follow_store.hpp"
#include "douyin_api.hpp"
#include "local_db.hpp"

#include <bb/cascades/ArrayDataModel>
#include <bb/data/JsonDataAccess>
#include <QDebug>

using namespace bb::data;

FollowStore::FollowStore(QObject* parent)
    : QObject(parent)
    , m_loading(false)
    , m_loadedLoggedIn(false)
    , m_count(0)
    , m_localDb(0)
{
    m_tagPrefix = QString("follow");
    m_model = new bb::cascades::ArrayDataModel(this);
    m_localDb = new LocalDb(this);
    connectNet();
}

void FollowStore::connectNet()
{
    QObject::connect(DouyinApi::instance(), SIGNAL(netReply(QString,QString,bool)),
                     this, SLOT(onNetReply(QString,QString,bool)));
}

void FollowStore::reloadIfStale()
{
    if (m_model->size() == 0 || m_loadedLoggedIn != DouyinApi::instance()->loggedIn()) {
        load();
    }
}

void FollowStore::load()
{
    if (m_loading) return;
    if (!DouyinApi::instance()->loggedIn()) {
        m_error = QString::fromUtf8("请先登录");
        emit errorChanged();
        return;
    }

    m_loading = true;
    m_error.clear();
    emit loadingChanged();
    emit errorChanged();

    QVariantMap params;
    params.insert("offset", m_offset);
    params.insert("page_size", 20);
    DouyinApi::instance()->getSigned(DouyinApi::followFeedApi(), params, m_tagPrefix + ":feed");
}

QString FollowStore::noteIdAt(int row) const
{
    if (row < 0 || row >= m_model->size()) return QString();
    QVariant v = m_model->value(row);
    if (v.canConvert<QVariantMap>()) {
        return v.toMap().value("noteId").toString();
    }
    return QString();
}

void FollowStore::setCount(int n)
{
    if (m_count == n) return;
    m_count = n;
    emit countChanged();
}

void FollowStore::onNetReply(const QString& tag, const QString& json, bool ok)
{
    m_loading = false;
    emit loadingChanged();

    if (!ok) {
        m_error = QString::fromUtf8("网络请求失败");
        emit errorChanged();
        return;
    }

    if (tag != m_tagPrefix + ":feed") {
        return;
    }

    JsonDataAccess jda;
    QVariant v = jda.loadFromBuffer(json.toUtf8());
    if (!v.isValid()) {
        m_error = QString::fromUtf8("JSON 解析失败");
        emit errorChanged();
        return;
    }

    QVariantMap root = v.toMap();
    int code = root.value("code").toInt();
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
        variantItems.append(item);
    }

    if (variantItems.isEmpty()) {
        m_hasMore = false;
    } else {
        m_model->append(variantItems);
        m_hasMore = data.value("hasMore").toBool();
        m_offset += variantItems.size();
    }

    setCount(m_model->size());
    emit countChanged();
}
