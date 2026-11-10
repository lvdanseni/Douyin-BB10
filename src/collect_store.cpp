#include "collect_store.hpp"
#include "note_model.hpp"
#include "douyin_api.hpp"

#include <bb/data/JsonDataAccess>
#include <QDebug>

using namespace bb::data;

CollectStore::CollectStore(QObject* parent)
    : QObject(parent)
    , m_loading(false)
    , m_current(-1)
    , m_loadedLoggedIn(false)
{
    m_tagPrefix = QString("collect");
    m_notes = new NoteModel(NoteModel::Collect, this);
    connectNet();
}

QString CollectStore::currentTitle() const
{
    if (m_current < 0 || m_current >= m_folders.size()) return QString();
    return m_folders.at(m_current).title;
}

QObject* CollectStore::noteModel() const
{
    return m_notes;
}

void CollectStore::connectNet()
{
    QObject::connect(DouyinApi::instance(), SIGNAL(netReply(QString,QString,bool)),
                     this, SLOT(onNetReply(QString,QString,bool)));
}

void CollectStore::reload()
{
    if (DouyinApi::instance()->loggedIn() != m_loadedLoggedIn) {
        loadFolders();
    } else if (m_current >= 0) {
        // 重拉当前夹子的笔记
        QString cid = m_folders.at(m_current).id;
        QVariantMap params;
        params.insert("collect_id", cid);
        params.insert("page", 1);
        params.insert("page_size", 20);
        DouyinApi::instance()->getSigned(DouyinApi::collectNotesApi(), params, "__collect_notes__");
    }
}

void CollectStore::loadFolders()
{
    m_loading = true;
    m_error.clear();
    emit foldersChanged();
    emit loadingChanged();

    DouyinApi::instance()->getSigned("/api/sns/v10/collection/folders",
                                   QVariantMap(), "__folders__");
}

void CollectStore::selectFolder(int index)
{
    if (index < 0 || index >= m_folders.size()) return;
    m_current = index;
    m_notes->clearFeed();
    QString cid = m_folders.at(m_current).id;
    QVariantMap params;
    params.insert("collect_id", cid);
    params.insert("page", 1);
    params.insert("page_size", 20);
    DouyinApi::instance()->getSigned(DouyinApi::collectNotesApi(), params, "__collect_notes__");
    emit currentChanged();
    emit foldersChanged();
}

QString CollectStore::folderName(int index) const
{
    if (index < 0 || index >= m_folders.size()) return QString();
    return m_folders.at(index).title;
}

int CollectStore::folderNoteCount(int index) const
{
    if (index < 0 || index >= m_folders.size()) return 0;
    return m_folders.at(index).noteCount;
}

void CollectStore::onNetReply(const QString& tag, const QString& json, bool ok)
{
    if (!tag.startsWith("__")) return;

    if (tag == "__folders__") {
        onFoldersReply(json, ok);
    } else if (tag == "__collect_notes__") {
        onCollectNotesReply(json, ok);
    }
}

void CollectStore::onFoldersReply(const QString& json, bool ok)
{
    m_loading = false;
    emit loadingChanged();

    if (!ok || json.isEmpty()) {
        m_error = QString::fromUtf8("获取收藏夹失败");
        emit errorChanged();
        emit foldersChanged();
        return;
    }

    JsonDataAccess jda;
    QVariant v = jda.loadFromBuffer(json.toUtf8());
    if (!v.isValid()) {
        m_error = QString::fromUtf8("JSON 解析失败");
        emit errorChanged();
        emit foldersChanged();
        return;
    }

    QVariantMap root = v.toMap();
    if (root.value("code").toInt(0) != 0) {
        m_error = root.value("message").toString();
        emit errorChanged();
        emit foldersChanged();
        return;
    }

    QVariantList arr = root.value("data").toList();
    m_folders.clear();
    for (int i = 0; i < arr.size(); ++i) {
        QVariantMap obj = arr.at(i).toMap();
        Folder f;
        f.id = obj.value("id").toString();
        f.title = obj.value("title").toString();
        f.noteCount = obj.value("note_count").toInt(0);
        m_folders.append(f);
    }

    m_loadedLoggedIn = DouyinApi::instance()->loggedIn();

    if (!m_folders.isEmpty() && m_current < 0) {
        m_current = 0;
        emit currentChanged();
        selectFolder(0);
    }

    emit foldersChanged();
}

void CollectStore::onCollectNotesReply(const QString& json, bool ok)
{
    Q_UNUSED(ok);
    Q_UNUSED(json);
    // NoteModel 会通过自己的 netReply 路由处理，这里不需要额外处理
}
