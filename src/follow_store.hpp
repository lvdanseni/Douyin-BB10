#ifndef FOLLOW_STORE_HPP
#define FOLLOW_STORE_HPP

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QString>

#include <bb/cascades/ArrayDataModel>

class LocalDb;

/**
 * 关注动态 Store —— 对应 BBTube 的 FollowModel（FeedModel::Follow）：
 * 拉取用户关注的人发布的笔记动态，需要登录态。
 */
class FollowStore : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QObject* model READ model CONSTANT)

public:
    explicit FollowStore(QObject* parent = 0);

    bool loading() const { return m_loading; }
    QString error() const { return m_error; }
    int count() const { return m_count; }
    QObject* model() const { return m_model; }

    /** 切到关注 tab 时调用：只在"列表为空"或"登录态变了"时才重新拉。 */
    Q_INVOKABLE void reloadIfStale();
    Q_INVOKABLE void load();
    Q_INVOKABLE QString noteIdAt(int row) const;
    Q_INVOKABLE QString diagLast() const { return m_diagLast; }
    void setCount(int n);

signals:
    void loadingChanged();
    void errorChanged();
    void countChanged();

public slots:
    void onNetReply(const QString& tag, const QString& json, bool ok);

private:
    void connectNet();

    bool m_loading;
    bool m_loadedLoggedIn;
    bool m_hasMore;
    QString m_error;
    int m_count;
    QString m_offset;
    QString m_tagPrefix;
    QString m_diagLast;
    bb::cascades::ArrayDataModel* m_model;
    LocalDb* m_localDb;
};

#endif // FOLLOW_STORE_HPP
