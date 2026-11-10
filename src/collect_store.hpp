#ifndef COLLECT_STORE_HPP
#define COLLECT_STORE_HPP

#include <QObject>
#include <QVariantMap>
#include <QVariantList>
#include <QString>
#include <QList>

class BiliApi;
class NoteModel;

/**
 * 收藏夹 Store —— 对应 BBTube 的 FavStore：
 * 收藏是**两级**数据 —— 先拿收藏夹列表（默认收藏夹/自定义夹…），
 * 再按 collect_id 拉某个夹子里的笔记。这里把两级串起来：目录到货后自动选中第一个夹子
 * 并开始拉笔记，QML 侧只需要渲染，不用做任何"拿到目录再选第一个"的时序协调。
 *
 * 【为什么不放 QML 做】QML 里把 C++ 信号 connect 到 JS 函数在 BB10/Qt4.8 上
 * 静默不连接（项目铁律 4）。这里的自动选中全在 C++ 里完成，
 * QML 只通过 Q_PROPERTY 读 count / current / error。
 */
class CollectStore : public QObject {
    Q_OBJECT

    Q_PROPERTY(int count READ count NOTIFY foldersChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY foldersChanged)
    Q_PROPERTY(QString error READ error NOTIFY foldersChanged)
    /** 当前选中的收藏夹下标（-1 = 未选） */
    Q_PROPERTY(int current READ current NOTIFY currentChanged)
    Q_PROPERTY(QString currentTitle READ currentTitle NOTIFY currentChanged)
    /** 收藏夹内容的 NoteModel，QML 直接喂给 NotePage/ListView */
    Q_PROPERTY(QObject* noteModel READ noteModel CONSTANT)

public:
    explicit CollectStore(QObject* parent = 0);

    int count() const { return m_folders.size(); }
    bool loading() const { return m_loading; }
    QString error() const { return m_error; }
    int current() const { return m_current; }
    QString currentTitle() const;
    QObject* noteModel() const;

    /** 切到收藏 tab 时调用：按登录态决定"清空 / 重拉目录 / 重拉当前夹子"。 */
    Q_INVOKABLE void reload();
    /** 强制重拉收藏夹目录（TitleBar 的刷新按钮）。 */
    Q_INVOKABLE void loadFolders();
    Q_INVOKABLE void selectFolder(int index);
    /** 第 i 个收藏夹的名字；越界返回空串。 */
    Q_INVOKABLE QString folderName(int index) const;
    Q_INVOKABLE int folderNoteCount(int index) const;
    Q_INVOKABLE QString diagLast() const { return m_diagLast; }

public slots:
    void onNetReply(const QString& tag, const QString& json, bool ok);

signals:
    void foldersChanged();
    void currentChanged();
    void loadingChanged();
    void errorChanged();

private:
    struct Folder {
        QString id;
        QString title;
        int noteCount;
    };

    QList<Folder> m_folders;
    NoteModel* m_notes;
    bool m_loading;
    QString m_error;
    int m_current;
    bool m_loadedLoggedIn;
    QString m_tagPrefix;
    QString m_diagLast;

    void connectNet();

    void onFoldersReply(const QString& json, bool ok);
    void onCollectNotesReply(const QString& json, bool ok);
};

#endif // COLLECT_STORE_HPP
