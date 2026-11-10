#ifndef HISTORY_STORE_HPP
#define HISTORY_STORE_HPP

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QString>
#include <QSet>

#include <bb/cascades/ArrayDataModel>

class LocalDb;

/**
 * "观看历史"板块的数据源。
 *
 * 【为什么不用 NoteModel】它的数据靠 HTTP 拉，而这里的数据在**本地数据库**里：
 * LocalDb 的 history 表才是全部内容。每次观看笔记时，NoteStore 会写入一条历史。
 *
 * 列表行字段和 NoteModel 保持一致（noteId/title/coverUrl/durationText/metaText），
 * 这样 NoteRow 和 pushNote() 都能原样复用。
 *
 * 【持久化位置】SQLite 沙箱根目录 xiaohongshu.db，不要写 /accounts/1000/shared。
 */
class HistoryStore : public QObject {
    Q_OBJECT

    Q_PROPERTY(QObject* model READ model CONSTANT)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    /** 历史记录里一条都没有时为 true，QML 用它显示空状态提示。 */
    Q_PROPERTY(bool empty READ isEmpty NOTIFY countChanged)

public:
    explicit HistoryStore(QObject* parent = 0);

    QObject* model() const;
    int count() const { return m_count; }
    bool isEmpty() const { return m_count == 0; }

    /** 重新扫一遍本地历史数据库（切到本 Tab 时调用）。 */
    Q_INVOKABLE void reload();
    Q_INVOKABLE QString noteIdAt(int row) const;
    Q_INVOKABLE void clearAll();
    Q_INVOKABLE QString diagLast() const { return m_diagLast; }

signals:
    void countChanged();

private:
    void setCount(int n);

    bb::cascades::ArrayDataModel* m_model;
    int m_count;
    LocalDb* m_localDb;
    QString m_diagLast;
};

#endif // HISTORY_STORE_HPP
