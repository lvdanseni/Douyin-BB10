#include "history_store.hpp"
#include "local_db.hpp"

#include <bb/cascades/ArrayDataModel>
#include <QDebug>

HistoryStore::HistoryStore(QObject* parent)
    : QObject(parent)
    , m_count(0)
    , m_localDb(0)
    , m_diagLast()
{
    m_model = new bb::cascades::ArrayDataModel(this);
    m_localDb = new LocalDb(this);
}

QObject* HistoryStore::model() const
{
    return m_model;
}

void HistoryStore::reload()
{
    if (!m_localDb) return;

    m_model->clear();
    QVariantList list = m_localDb->history(100);
    for (int i = 0; i < list.size(); ++i) {
        m_model->append(list.at(i));
    }
    setCount(list.size());
}

QString HistoryStore::noteIdAt(int row) const
{
    if (row < 0 || row >= m_model->size()) return QString();
    QVariant v = m_model->value(row);
    if (v.canConvert<QVariantMap>()) {
        return v.toMap().value("noteId").toString();
    }
    return QString();
}

void HistoryStore::clearAll()
{
    if (!m_localDb) return;
    m_localDb->clearHistory();
    m_model->clear();
    setCount(0);
}

void HistoryStore::setCount(int n)
{
    if (m_count == n) return;
    m_count = n;
    emit countChanged();
}
