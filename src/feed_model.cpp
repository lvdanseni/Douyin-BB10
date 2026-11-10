#include "feed_model.hpp"
#include "douyin_api.hpp"

#include <QVariantList>
#include <QSslSocket>
#include <QFile>
#include <QDir>
#include <QTextStream>
#include <QDateTime>
#include <bb/data/JsonDataAccess>

using namespace bb::data;

/** 追加到应用 tmp 目录的调试日志（BB10 沙箱 tmp 可写，SSH/Sachesi 可取） */
static void logf(const QString& s)
{
    QFile f(QDir::tempPath() + "/bili_log.txt");
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream ts(&f);
        ts << s << "\n";
        f.close();
    }
}

FeedModel::FeedModel(Kind kind, int regionRid, QObject* parent)
    : ArrayDataModel(parent)
    , m_kind(kind)
    , m_regionRid(regionRid)
    , m_loadedLoggedIn(false)
    , m_favMediaId(0)
    , m_page(0)
    , m_hasMore(true)
    , m_loading(false)
    , m_loadingMore(false)
    , m_navConnected(false)
    , m_netConnected(false)
    // 顺序必须和头文件里的声明顺序一致（m_imgSeq/m_imgOk/m_imgTotal 声明在
    // m_loadTimer 之前），否则 qcc 会报 -Wreorder
    , m_imgSeq(0)
    , m_imgOk(0)
    , m_imgTotal(0)
    , m_loadTimer(0)
{
    // 实例唯一 tag 前缀：kind + 对象地址，保证多模型并发时不串台
    m_tagPrefix = QString("feed%1_%2").arg((int)kind).arg((quintptr)this, 0, 16);
    connectNet();
    connectImageNet();
}

QString FeedModel::bvidAt(int row) const
{
    if (row < 0 || row >= m_items.size()) return QString();
    return m_items.at(row).toMap().value("bvid").toString();
}

void FeedModel::setRelatedBvid(const QString& bvid)
{
    m_relatedBvid = bvid;
}

void FeedModel::setKeyword(const QString& keyword)
{
    m_keyword = keyword;
    reset();
}

void FeedModel::setRegion(int rid)
{
    if (m_regionRid == rid) return;
    m_regionRid = rid;
    reset();
}

void FeedModel::setFavMediaId(qint64 mediaId)
{
    if (m_favMediaId == mediaId) return;
    m_favMediaId = mediaId;
    reset();
}

void FeedModel::clearFeed()
{
    // 走 reset() 而不是 ArrayDataModel::clear()：后者只清界面上的行，
    // m_items / m_pendingImg 还留着旧数据，在途的封面回调会用旧索引
    // 去 replace 一个空模型。
    reset();
    m_loading = false;
    m_loadingMore = false;
    emit loadingChanged();
    emit loadingMoreChanged();
}

void FeedModel::reset()
{
    ArrayDataModel::clear();
    m_items.clear();
    // 【v78】索引已经归零，在途的封面下载回调全部作废（否则会改到新列表的行）
    m_pendingImg.clear();
    m_imgOk = 0;
    m_imgTotal = 0;
    m_page = 0;
    m_hasMore = true;
    m_followOffset.clear();   // 【v83】游标跟着列表一起作废
    m_histMax.clear();        // 【v89】历史游标同理，重拉必须从第一页开始
    m_histViewAt.clear();
    m_error.clear();
    emit errorChanged();
    emit countChanged();
    emit coverStatsChanged();   // 【v100】计数器已归零，可见诊断要跟着清
}

void FeedModel::load()
{
    if (m_loading) return;

    // 【v83/v84】关注动态、收藏夹没有登录态就什么都拿不到（接口直接回 -101）。
    // 与其发一次注定失败的请求、再显示一行看不懂的 "API code -101"，
    // 不如直接结束加载，让 QML 的登录提示层接管（那边有"去登录"按钮）。
    if ((m_kind == Follow || m_kind == Fav || m_kind == History)
        && !DouyinApi::instance()->loggedIn()) {
        reset();
        m_loading = false;
        m_loadedLoggedIn = false;
        emit loadingChanged();
        return;
    }

    connectNet();
    reset();
    m_loading = true;
    m_loadedLoggedIn = DouyinApi::instance()->loggedIn();
    m_diagLast.clear();
    emit loadingChanged();
    logf(QString("load kind=%1 t=%2").arg((int)m_kind).arg(QDateTime::currentMSecsSinceEpoch()));
    if (!m_loadTimer) {
        m_loadTimer = new QTimer(this);
        m_loadTimer->setSingleShot(true);
        m_loadTimer->setInterval(30000); // 30s：BB10 每次 TLS 握手 3-6s；晚到成功仍会清错误显示列表
        connect(m_loadTimer, SIGNAL(timeout()), this, SLOT(onLoadTimeout()));
    }
    m_loadTimer->start();
    qDebug("BiliClient FeedModel::load kind=%d start", (int)m_kind);
    fetch(false);
}

void FeedModel::loadMore()
{
    if (m_loading || m_loadingMore || !m_hasMore) return;
    connectNet();
    m_loadingMore = true;
    emit loadingMoreChanged();
    fetch(true);
}

void FeedModel::reloadIfStale()
{
    if (m_loading) return;
    // 列表空 → 还没成功拉过；登录态变了 → 数据一定是旧的。两种情况都重拉。
    // 其余情况不动，避免每次切 Tab 都清空滚动位置。
    if (m_items.isEmpty() || m_loadedLoggedIn != DouyinApi::instance()->loggedIn()) {
        load();
    }
}

void FeedModel::fetch(bool more)
{
    QString path;
    QVariantMap params;

    switch (m_kind) {
    case Rcmd: {
        path = "/x/web-interface/wbi/index/top/rcmd";
        // fresh_type=3 在部分账号/时段被 bilibili 后端拒绝（code=-400），去掉后正常返回
        int idx = qMax(3, m_items.size());
        params.insert("ps", "10");
        params.insert("fresh_idx", QString::number(idx));
        params.insert("plat", "1");
        params.insert("web_location", "1430650");
        break;
    }
    case Popular:
        path = "/x/web-interface/popular";
        params.insert("pn", QString::number(++m_page));
        params.insert("ps", "30");
        break;
    case Ranking:
        path = "/x/web-interface/ranking/v2";
        break;
    case Region:
        path = "/x/web-interface/ranking/region";
        params.insert("rid", QString::number(m_regionRid));
        params.insert("pn", QString::number(++m_page));
        params.insert("ps", "30");
        break;
    case Search:
        path = "/x/web-interface/wbi/search/type";
        params.insert("search_type", "video");
        params.insert("keyword", m_keyword);
        params.insert("page", QString::number(++m_page));
        params.insert("page_size", "30");
        break;
    case Related:
        // 相关推荐：只要一个 bvid，返回固定一页（20 条），不支持翻页。
        // 这个接口**不需要** WBI 签名，走普通 get 即可。
        path = "/x/web-interface/archive/related";
        params.insert("bvid", m_relatedBvid);
        break;
    case Follow:
        // 关注动态（网页版"动态 → 视频"那个列表）。
        // 需要登录 cookie；type=video 只返回视频类动态（DYNAMIC_TYPE_AV），
        // 图文/转发/专栏一律不返回 —— 正好对应我们的大封面列表。
        //
        // 翻页用的是接口给的**不透明游标** offset，不是页码；第一次不带 offset。
        path = "/x/polymer/web-dynamic/v1/feed/all";
        params.insert("type", "video");
        params.insert("page", QString::number(++m_page));
        params.insert("timezone_offset", "-480");
        // web_location 是网页端固定带的风控标记，缺了偶尔会被判 -352；
        // 带上和浏览器请求一致，代价为零。
        params.insert("web_location", "333.999");
        if (!m_followOffset.isEmpty()) params.insert("offset", m_followOffset);
        break;
    case Fav:
        // 【v84】某个收藏夹里的视频。media_id 由 FavStore 选好再传进来。
        // order=mtime 按收藏时间倒序，和网页端默认一致。
        path = "/x/v3/fav/resource/list";
        params.insert("media_id", QString::number(m_favMediaId));
        params.insert("pn", QString::number(++m_page));
        params.insert("ps", "20");
        params.insert("platform", "web");
        params.insert("order", "mtime");
        params.insert("type", "0");
        params.insert("tid", "0");
        break;
    case History:
        // 【v89】观看历史。第一页 max=0 & view_at=0，之后必须原样回传
        // 上一轮 cursor 里的 max / view_at —— 它是"从哪一条往后翻"的游标，
        // 不是页码，自己算会拿到重复/跳段的数据。
        path = "/x/web-interface/history/cursor";
        params.insert("max", m_histMax.isEmpty() ? QString::fromLatin1("0") : m_histMax);
        params.insert("view_at", m_histViewAt.isEmpty() ? QString::fromLatin1("0") : m_histViewAt);
        params.insert("ps", "20");
        params.insert("type", "archive");   // 只要稿件，不要直播/番剧/专栏
        break;
    }

    // tag 带本实例唯一前缀：接收方 onNetReply 先比对前缀，再按 :first/:more 路由
    const QString tag = m_tagPrefix + (more ? ":more" : ":first");

    bool signed_ = (m_kind == Rcmd || m_kind == Search || m_kind == Follow);
    if (signed_) {
        DouyinApi::instance()->getSigned(path, params, tag);
    } else {
        QString url = "https://api.bilibili.com" + path;
        QVariantMap::const_iterator it = params.constBegin();
        QStringList pairs;
        for (; it != params.constEnd(); ++it) {
            pairs << DouyinApi::percentEncode(it.key()) + "=" + DouyinApi::percentEncode(it.value().toString());
        }
        if (!pairs.isEmpty()) {
            url += "?" + pairs.join("&");
        }
        DouyinApi::instance()->get(url, tag);
    }
}

void FeedModel::onNetReply(const QString& tag, const QString& json, bool ok)
{
    // 先过滤：不是发给本实例的请求直接忽略（避免多模型互相串台）
    if (!tag.startsWith(m_tagPrefix)) return;

    const QString suffix = tag.mid(m_tagPrefix.size());
    if (suffix == ":first") {
        onReply(json, ok, false);
    } else if (suffix == ":more") {
        onReply(json, ok, true);
    }
}

void FeedModel::connectNet()
{
    if (m_netConnected) return;
    m_netConnected = true;

    // 编译期 connect 确保 BB10 Qt4.8.5 信号槽机制可靠工作。
    // 记录 connect 返回值：false 表示签名不匹配（moc 元数据缺失），
    // 这是设备端"curl 成功但回调不执行"的最可能根因，必须能在日志里看到。
    bool okConnect = QObject::connect(DouyinApi::instance(),
                                      SIGNAL(netReply(QString, QString, bool)),
                                      this,
                                      SLOT(onNetReply(QString, QString, bool)));
    m_connectDiag = okConnect ? QString("connect:ok") : QString("connect:FAILED");
    logf(QString("connectNet kind=%1 prefix=%2 result=%3")
             .arg((int)m_kind).arg(m_tagPrefix).arg(okConnect));
    qDebug("BiliClient FeedModel::connectNet kind=%d result=%d", (int)m_kind, okConnect);

    // nav 密钥失败监听独立保留（与业务回调分离）
    if ((m_kind == Rcmd || m_kind == Search || m_kind == Follow) && !m_navConnected) {
        m_navConnected = true;
        QObject::connect(DouyinApi::instance(), SIGNAL(navFailed()),
                         this, SLOT(onNavFailed()));
    }
}

void FeedModel::onNavFailed()
{
    // 签名密钥（nav）获取失败：结束本模型正在进行的加载，显示错误，避免无限转圈
    if (!m_loading) return;
    m_loading = false;
    if (m_loadTimer) m_loadTimer->stop();
    m_error = "NET FAIL: no WBI keys (" + DouyinApi::instance()->diagResult() + ")";
    emit loadingChanged();
    emit errorChanged();
    qDebug("BiliClient FeedModel::onNavFailed kind=%d", (int)m_kind);
}

void FeedModel::onLoadTimeout()
{
    // 终极兜底：网络回复在 10 秒内未返回时强制结束加载态，避免无限转圈
    if (!m_loading) return;
    m_loading = false;
    m_error = "Load timeout 30s (" + DouyinApi::instance()->diagResult() + "; " +
              DouyinApi::instance()->bizDiag() + "; " + m_connectDiag + "; " + m_diagLast + ")";
    logf(QString("load timeout kind=%1 diag=%2 t=%3")
             .arg((int)m_kind)
             .arg(DouyinApi::instance()->diagResult())
             .arg(QDateTime::currentMSecsSinceEpoch()));
    emit loadingChanged();
    emit errorChanged();
    qDebug("BiliClient FeedModel::onLoadTimeout kind=%d ssl=%d", (int)m_kind, QSslSocket::supportsSsl());
}

/**
 * 【v83】把关注动态的嵌套结构拍平成 appendItems 认识的扁平字段。
 *
 * 接口原始结构（只列用到的部分）：
 *   data = {
 *     has_more: bool,
 *     offset:   "不透明游标",
 *     items: [{
 *       type: "DYNAMIC_TYPE_AV",
 *       modules: {
 *         module_author:  { name, face, pub_ts },
 *         module_dynamic: { major: { archive: {
 *                              bvid, title, cover, duration_text, stat: { play }
 *                          } } }
 *       }
 *     }]
 *   }
 *
 * appendItems 只认扁平字段（bvid/title/pic/duration/owner/stat.view/pubdate），
 * 在这里转一次，后面"封面缩略图 / 本地缓存 / 时长文案 / 时间文案"全部照旧复用，
 * 不用为关注流再写一套解析。
 */
static QVariantList flattenFollow(const QVariantMap& data, bool* hasMore, QString* offset)
{
    if (hasMore) *hasMore = data.value("has_more").toBool();
    if (offset)  *offset  = data.value("offset").toString();

    QVariantList out;
    const QVariantList items = data.value("items").toList();
    for (int i = 0; i < items.size(); ++i) {
        const QVariantMap modules = items.at(i).toMap().value("modules").toMap();
        const QVariantMap major = modules.value("module_dynamic").toMap().value("major").toMap();
        const QVariantMap arc = major.value("archive").toMap();
        const QString bvid = arc.value("bvid").toString();
        // 图文/转发/专栏没有 archive 段，直接跳过（type=video 理论上不会给，
        // 但接口偶尔会混进"动态里的视频卡片"，加个守卫不会错）
        if (bvid.isEmpty()) continue;

        const QVariantMap author = modules.value("module_author").toMap();

        QVariantMap row;
        row.insert("bvid", bvid);
        row.insert("title", arc.value("title"));
        row.insert("pic", arc.value("cover"));
        // duration_text 形如 "12:34" / "1:02:03"，parseDuration 认得
        row.insert("duration", arc.value("duration_text"));
        QVariantMap owner;
        owner.insert("name", author.value("name"));
        row.insert("owner", owner);
        QVariantMap stat;
        // 注意：这里是字符串形式的播放量，appendItems 用 toLongLong() 转，能转
        stat.insert("view", arc.value("stat").toMap().value("play"));
        row.insert("stat", stat);
        row.insert("pubdate", author.value("pub_ts"));
        out.append(row);
    }
    return out;
}

/**
 * 【v89】观看历史拍平。
 *
 * 响应形状（/x/web-interface/history/cursor）：
 *   { code: 0,
 *     data: {
 *       cursor: { max, view_at, ps, business },
 *       list: [{
 *         title, cover, covers,
 *         history: { oid, bvid, cid, page, business },
 *         author_name, duration, view_at, progress, is_finish
 *       }]
 *     } }
 *
 * 【翻页】cursor 是"从哪一条往后"的游标，不是页码 —— max / view_at 必须原样
 * 回传上一轮的值。自己按页码算会拿到重复段或直接跳段。
 * 接口不给 has_more，用"本页非空就当还有"来判断，下一页回空列表自然停。
 *
 * 【covers】部分条目（尤其是番剧/合集）没有单个 cover 字段，只有一个
 * covers 数组。两种都兜一下，否则那些行会没有封面。
 */
static QVariantList flattenHistory(const QVariantMap& data, bool* hasMore,
                                   QString* nextMax, QString* nextViewAt)
{
    QVariantList out;
    const QVariantList list = data.value("list").toList();
    for (int i = 0; i < list.size(); ++i) {
        const QVariantMap it = list.at(i).toMap();
        const QVariantMap hist = it.value("history").toMap();
        const QString bvid = hist.value("bvid").toString();
        // 直播/番剧/失效稿件没有 bvid，点进去也放不了，直接跳过
        if (bvid.isEmpty()) continue;

        QVariantMap row;
        row.insert("bvid", bvid);
        QString title = it.value("title").toString();
        if (title.isEmpty()) title = it.value("long_title").toString();
        row.insert("title", title);

        QString cover = it.value("cover").toString();
        if (cover.isEmpty()) {
            const QVariantList covers = it.value("covers").toList();
            if (!covers.isEmpty()) {
                const QVariantMap c0 = covers.at(0).toMap();
                cover = c0.value("url").toString();
                if (cover.isEmpty()) cover = c0.value("cover").toString();
            }
        }
        row.insert("pic", cover);

        row.insert("duration", it.value("duration"));
        QVariantMap owner;
        owner.insert("name", it.value("author_name"));
        row.insert("owner", owner);
        row.insert("pubdate", it.value("view_at"));
        out.append(row);
    }

    // 只有真拿到数据才推进游标；空页保持原值，避免下一轮又从头翻。
    if (!list.isEmpty()) {
        const QVariantMap cursor = data.value("cursor").toMap();
        if (nextMax)    *nextMax    = QString::number(cursor.value("max").toLongLong());
        if (nextViewAt) *nextViewAt = QString::number(cursor.value("view_at").toLongLong());
    }
    if (hasMore) *hasMore = !list.isEmpty();
    return out;
}

void FeedModel::onReply(const QString& json, bool ok, bool more)
{
    qDebug("BiliClient FeedModel::onReply kind=%d more=%d ok=%d bytes=%d",
           (int)m_kind, more, ok, json.size());
    m_diagLast = QString("reply:called ok=%1 b=%2").arg(ok).arg(json.size());
    logf(QString("reply called kind=%1 more=%2 ok=%3 bytes=%4 t=%5")
             .arg((int)m_kind).arg(more).arg(ok).arg(json.size())
             .arg(QDateTime::currentMSecsSinceEpoch()));
    if (m_loadTimer) m_loadTimer->stop();

    if (!ok) {
        if (more) {
            m_loadingMore = false;
            emit loadingMoreChanged();
        } else {
            m_loading = false;
            QString reason = json.isEmpty() ? QString("curl no out") : QString::fromUtf8(json.toUtf8()).left(40);
            m_error = "NET FAIL (" + reason + ")";
            emit loadingChanged();
            emit errorChanged();
        }
        return;
    }

    JsonDataAccess jda;
    QVariant v = jda.loadFromBuffer(json.toUtf8());
    QVariantMap root = v.toMap();
    int code = root.value("code").toInt();
    QVariantList items;
    if (code == 0) {
        // 成功即清空残留错误文案（避免先前的超时提示把列表遮死）
        m_error.clear();
        emit errorChanged();
        QVariantMap dataMap = root.value("data").toMap();
        switch (m_kind) {
        case Rcmd:
            items = dataMap.value("item").toList();
            break;
        case Popular:
        case Ranking:
            items = dataMap.value("list").toList();
            break;
        case Related:
            // 实测响应：data 直接是数组（不是 {list: [...]}），字段与 feed 一致
            items = root.value("data").toList();
            // 【v84】只留前 8 条。
            //
            // 接口固定回 40 条，而列表只显示 5 条。多出来的 35 条会各自触发一次
            // 封面下载 —— 40 个并发 TLS 握手在 BB10 上根本握不完，
            // CONNECTTIMEOUT 10s 一到全部失败，结果就是"相关视频一个封面都不显示"。
            // 首页看起来正常只是因为那些图早就缓存过了。
            while (items.size() > 8) items.removeLast();
            break;
        case Region: {
            // 实测响应：data 直接是列表（非 dict），字段为 play/author（非 stat.view/owner.name）
            items = root.value("data").toList();
            break;
        }
        case Search:
            items = dataMap.value("result").toList();
            m_hasMore = (m_items.size() + items.size()) < dataMap.value("numResults").toLongLong();
            break;
        case Follow:
            items = flattenFollow(dataMap, &m_hasMore, &m_followOffset);
            break;
        case Fav: {
            // 【v84】/x/v3/fav/resource/list 的 data 形如：
            //   { info: {id,title,media_count}, medias: [...], has_more: bool }
            // medias[] 的字段名和别的接口都不一样：cover / upper.name / cnt_info.play / pubtime。
            const QVariantList medias = dataMap.value("medias").toList();
            for (int i = 0; i < medias.size(); ++i) {
                const QVariantMap m = medias.at(i).toMap();
                // attr != 0 表示稿件已失效/私密，这类条目的 bvid 是空的，跳过
                const QString bv = m.value("bvid").toString();
                if (bv.isEmpty()) continue;
                QVariantMap row;
                row.insert("bvid", bv);
                row.insert("title", m.value("title"));
                row.insert("pic", m.value("cover"));
                row.insert("duration", m.value("duration"));
                QVariantMap owner;
                owner.insert("name", m.value("upper").toMap().value("name"));
                row.insert("owner", owner);
                QVariantMap stat;
                stat.insert("view", m.value("cnt_info").toMap().value("play"));
                row.insert("stat", stat);
                row.insert("pubdate", m.value("pubtime"));
                items.append(row);
            }
            m_hasMore = dataMap.value("has_more").toBool();
            break;
        }
        case History:
            items = flattenHistory(dataMap, &m_hasMore, &m_histMax, &m_histViewAt);
            break;
        }
        if (m_kind == Ranking) m_hasMore = false;
        m_diagLast += QString(" code=%1 items=%2").arg(code).arg(items.size());
    } else {
        // -101 = 账号未登录 / 凭证过期。关注流、播放记录这类接口最容易撞上，
        // 直接把原始错误码糊给用户看没有意义。
        m_error = (code == -101) ? QString::fromUtf8("登录已失效，请到设置页重新登录")
                                 : ("API code " + QString::number(code));
        m_diagLast += QString(" code=%1").arg(code);
        emit errorChanged();
    }

    if (more) {
        m_loadingMore = false;
        emit loadingMoreChanged();
    } else {
        m_loading = false;
        emit loadingChanged();
        if (code != 0) {
            ArrayDataModel::clear();
            m_items.clear();
            m_pendingImg.clear();   // 【v78】同上，索引失效
            emit countChanged();
            return;
        }
    }
    appendItems(items);
}

/**
 * 解析时长：不同接口格式不一致。
 *  - rcmd / popular / ranking：duration 为秒数（int）
 *  - ranking/region：duration 为字符串 "0:15" / "1:02:03"
 * 统一转成秒，交给 DouyinApi::durationText 输出。
 */
static long long parseDuration(const QVariant& v)
{
    if (v.type() == QVariant::String) {
        const QString s = v.toString().trimmed();
        if (s.isEmpty()) return 0;
        if (s.contains(':')) {
            long long total = 0;
            const QStringList parts = s.split(':');
            for (int i = 0; i < parts.size(); ++i) {
                total = total * 60 + parts.at(i).toLongLong();
            }
            return total;
        }
        return s.toLongLong();
    }
    return v.toLongLong();
}

void FeedModel::appendItems(const QVariantList& items)
{
    if (items.isEmpty()) {
        m_hasMore = false;
        return;
    }
    QVariantList normalized;
    for (int i = 0; i < items.size(); ++i) {
        QVariantMap raw = items.at(i).toMap();
        QVariantMap row;
        QString bvid = raw.value("bvid").toString();
        if (bvid.isEmpty()) continue;
        row.insert("bvid", bvid);
        row.insert("title", raw.value("title"));
        QString picUrl = raw.value("pic").toString();
        if (picUrl.startsWith("//")) picUrl = "https:" + picUrl;
        // 【v78】要 B 站 CDN 的缩略图，别拉原图。
        //
        // 改成大封面之后，一张封面要铺满 720du 宽；原图常常是 1280x720、几百 KB，
        // 一屏 10 张就是好几 MB。BB10 每次 TLS 握手 3~6 秒，拉原图会把首屏拖到
        // 不可用的程度。B 站 CDN 支持 `@宽w_高h_裁剪` 后缀，672 宽足够 720du 的屏用
        // （AspectFill 再裁一次），体积降到 1/10 左右。
        //
        // 加后缀会改变缓存文件名 —— 老缓存（原图）自然失效，一次性代价。
        if (picUrl.startsWith("http")) {
            // 【v93】后缀规则统一收进 DouyinApi::coverUrl（幂等：先砍 @ 参数再加），
            // 保证列表页、下载 sidecar、已下载页三处算出的缓存文件名是同一个。
            picUrl = DouyinApi::coverUrl(picUrl);
        }
        // 【v102】picUrl 现在**永远存远端 URL，永不变化**。
        //
        // 本地路径不写进模型了 —— 改由 QML 用 `biliApi.coverUrlFor(picUrl,
        // biliApi.coverVersion)` 现场换。原因是 BB10 上"模型行数据变了 →
        // ListView 重绘"这条路不可靠（replace() 实测封面永远不出现，
        // removeAt+insert 也未必），而 Q_PROPERTY + NOTIFY 是**已验证可靠**的通道
        // （播放页 UP 主头像 ownerFace 就是它，一直正常）。
        // 这也正是 BBTube 的架构：模型里只放远端 URL，图片由视图自己解析。
        if (!picUrl.isEmpty() && picUrl.startsWith("http")) {
            const QString local = imageCachePath(picUrl);
            QFileInfo fi(local);
            // 【v84】缓存有效性：size > 1024 且字节完整（见 isUsableImage）
            if (!(fi.exists() && fi.size() > 1024 && DouyinApi::isUsableImage(local))) {
                // 残骸/坏文件先删，避免再次命中
                if (fi.exists()) QFile::remove(local);
                int rowIndex = normalized.size();
                // 【v78】tag 里存**绝对索引**（模型里已有的行数 + 本批次序号）。
                const int absRow = m_items.size() + rowIndex;
                // 【v99】tag 用单调递增序号，不用行号 —— 行号在 clear() 后会重复，
                // 旧 worker 回来就会把旧视频的封面贴到新列表的同一行上。
                QString tag = m_tagPrefix + QString(":img%1").arg(++m_imgSeq);
                ImgPending pend;
                pend.row = absRow;
                pend.url = picUrl;
                pend.path = local;
                m_pendingImg.insert(tag, pend);
                m_imgTotal++;   // 诊断用：本实例一共入队多少张封面
                // 【v84】queued=true → 进封面下载队列（5 并发）。
                // 不能一次开 20~40 个线程各做一次 TLS 握手 —— BB10 握不完，
                // 全部 CONNECTTIMEOUT，整页一个封面都不显示。
                DouyinApi::instance()->download(picUrl, local, tag, -1, -1, -1, true);
            }
        }
        row.insert("picUrl", picUrl);
        // 【v102】把 BiliApi 指针塞进每一行。
        //
        // 为什么不直接在 QML 里写 `biliApi.coverUrlFor(...)`：ListItemComponent 是
        // 独立组件作用域，**看不到 context property**（v92 踩过：ui / theme 在
        // delegate 里都是 ReferenceError）。只能像长按删除那样，由 C++ 把对象指针
        // 塞进行数据，QML 通过 ListItemData 拿到。
        row.insert("api", QVariant::fromValue(static_cast<QObject*>(DouyinApi::instance())));
        QString owner = raw.value("owner").toMap().value("name").toString();
        if (owner.isEmpty()) owner = raw.value("author").toString();
        long long duration = parseDuration(raw.value("duration"));
        row.insert("durationText", DouyinApi::durationText(duration));
        qint64 view = raw.value("stat").toMap().value("view").toLongLong();
        if (view == 0) view = raw.value("play").toLongLong();
        QString meta = owner;
        if (view > 0) {
            QString v = DouyinApi::countText(view) + QString::fromUtf8("播放");
            meta = meta.isEmpty() ? v : meta + QString::fromUtf8(" · ") + v;
        }
        long long pub = raw.value("pubdate").toLongLong();
        if (pub <= 0) {
            // ranking/region 无 pubdate，改提供 create = "yyyy-MM-dd HH:mm"
            const QString cs = raw.value("create").toString().trimmed();
            if (!cs.isEmpty()) {
                QDateTime dt = QDateTime::fromString(cs, "yyyy-MM-dd HH:mm");
                if (dt.isValid()) pub = dt.toTime_t();
            }
        }
        if (pub > 0) {
            QString t = DouyinApi::timeAgo(pub);
            meta = meta.isEmpty() ? t : meta + QString::fromUtf8(" · ") + t;
        }
        row.insert("metaText", meta);
        row.insert("typeName", raw.value("typename"));
        normalized.append(row);
    }
    // 【v100】本批封面入队完毕，刷新一次可见诊断（设置页那行 imgN/M）。
    emit coverStatsChanged();

    if (!normalized.isEmpty()) {
        ArrayDataModel::append(normalized);
        for (int i = 0; i < normalized.size(); ++i) {
            m_items.append(normalized.at(i).toMap());
        }
        emit countChanged();
        m_diagLast += QString(" app=%1").arg(normalized.size());
        logf(QString("reply appended kind=%1 n=%2 t=%3")
                 .arg((int)m_kind).arg(normalized.size())
                 .arg(QDateTime::currentMSecsSinceEpoch()));
    } else {
        m_hasMore = false;
        m_diagLast += " app=0";
        logf("reply empty items");
    }
}

void FeedModel::connectImageNet()
{
    if (m_imgNetConnected) return;
    m_imgNetConnected = true;
    QObject::connect(DouyinApi::instance(),
                     SIGNAL(downloadFinished(QString, QString, bool)),
                     this,
                     SLOT(onImageDownloadFinished(QString, QString, bool)));
    qDebug("BiliClient FeedModel::connectImageNet kind=%d", (int)m_kind);
}

QString FeedModel::imageCachePath(const QString& url) const
{
    // 【v93】路径算法全工程只有一份（DouyinApi::coverCachePath）。
    // 以前这里和 DownloadStore、VideoStore 各写一遍，60 字符截断漏了一处，
    // 结果"已下载"列表永远查不到封面。
    return DouyinApi::coverCachePath(url);
}

void FeedModel::onImageDownloadFinished(const QString& tag, const QString& path, bool ok)
{
    if (!tag.startsWith(m_tagPrefix + ":img")) return;

    // 【v78】查表拿绝对行号。表里没有 = 这批数据已经被 clear() 掉了
    // （刷新/换 tab），直接丢弃，绝不能拿旧索引去改新列表的行。
    if (!m_pendingImg.contains(tag)) return;
    ImgPending pend = m_pendingImg.take(tag);
    // 【v102】行号已不参与回填（模型不再改），保留在 ImgPending 里只为诊断
    Q_UNUSED(pend.row);

    if (!ok) {
        // 【v84】失败的下载会留下 0 字节或半个文件的残骸，不删掉的话下次
        // 会被当成"已缓存"直接命中，这张封面就永远出不来了。
        QFile::remove(path);
        // 【v96】失败重试一次。
        //
        // 20 秒超时对慢速 CDN 偶尔不够，或者那一次正好撞上某个节点抽风。
        // 原来失败就彻底放弃 —— 那张封面在这次会话里再也不会出现，
        // 用户看到的就是"一页封面只出来一部分"。
        // 只重试一次：真正的 404/防盗链重试多少次都没用，不能浪费额度。
        if (pend.retry < 1 && !pend.url.isEmpty()) {
            pend.retry++;
            // 【v96】重试时切一次协议。
            // https 被中间设备掐掉、http 被劫持成广告页 —— 两种网络环境都真实存在
            // （v87 在头像上就踩过 https 不通要换 http 的情况）。换一个往往就通了。
            if (pend.url.startsWith(QLatin1String("https://"))) {
                pend.url.replace(0, 8, QString::fromLatin1("http://"));
            } else if (pend.url.startsWith(QLatin1String("http://"))) {
                pend.url.replace(0, 7, QString::fromLatin1("https://"));
            }
            m_pendingImg.insert(tag, pend);
            DouyinApi::instance()->download(pend.url, pend.path, tag, -1, -1, -1, true);
            qDebug("BiliClient image retry tag=%s", qPrintable(tag));
            return;
        }
        qDebug("BiliClient image download failed tag=%s", qPrintable(tag));
        return;
    }
    QFileInfo fi(path);
    // 【v96】除了大小还要验内容：劫持页/错误页是 HTML，能过 1KB 但解码不出来
    if (!fi.exists() || fi.size() <= 1024 || !DouyinApi::isUsableImage(path)) {
        QFile::remove(path);
        if (pend.retry < 1 && !pend.url.isEmpty()) {
            pend.retry++;
            m_pendingImg.insert(tag, pend);
            DouyinApi::instance()->download(pend.url, pend.path, tag, -1, -1, -1, true);
            qDebug("BiliClient image bad content, retry tag=%s", qPrintable(tag));
        }
        return;
    }
    // 【v102】不再改模型行。
    //
    // DouyinApi::onDownloadFinished 在下载完成且校验通过时，已经把这张封面登记进
    // 就绪表并 bump 了 coverVersion；QML 里 `coverUrlFor(url, biliApi.coverVersion)`
    // 的绑定会自己重算 —— 这是本工程唯一被反复验证可靠的 C++→QML 通道。
    //
    // 之前用 ArrayDataModel::replace()（v78~v100）和 removeAt+insert（v101）
    // 去改行数据，都依赖"模型行变化 → ListView 重绘"这条路，而它在 BB10 上不可靠：
    // 表现就是封面永远不出现，直到重新进这个 Tab。
    //
    // 对照 BBTube 就清楚了：它的行数据里只放**远端 URL（永不变化）**，
    // 图片由 C++ 的 AdvancedImageView 自己解析 —— 根本不需要模型刷新。
    m_imgOk++;
    emit coverStatsChanged();
}

// ============================================================================
// FavStore 【v84】收藏夹目录 + 收藏夹内容
// ============================================================================

FavStore::FavStore(QObject* parent)
    : QObject(parent)
    , m_videos(0)
    , m_loading(false)
    , m_current(-1)
    , m_loadedLoggedIn(false)
{
    // 实例唯一前缀。注意和内部 FeedModel 的前缀（feed7_xxx）不重叠，
    // 两者都连着同一个 netReply 信号，靠前缀区分，绝不能撞。
    m_tagPrefix = QString("fav_%1").arg((quintptr)this, 0, 16);
    m_videos = new FeedModel(FeedModel::Fav, 0, this);
    QObject::connect(DouyinApi::instance(),
                     SIGNAL(netReply(QString, QString, bool)),
                     this,
                     SLOT(onNetReply(QString, QString, bool)));
}

QObject* FavStore::videoModel() const
{
    return m_videos;
}

QString FavStore::currentTitle() const
{
    if (m_current < 0 || m_current >= m_folders.size()) return QString();
    return m_folders.at(m_current).title;
}

QString FavStore::folderName(int index) const
{
    if (index < 0 || index >= m_folders.size()) return QString();
    return m_folders.at(index).title;
}

int FavStore::folderMediaCount(int index) const
{
    if (index < 0 || index >= m_folders.size()) return 0;
    return m_folders.at(index).mediaCount;
}

void FavStore::reload()
{
    if (!DouyinApi::instance()->loggedIn()) {
        // 退出登录后必须把目录和列表一起清掉，否则界面还留着上一个账号的收藏
        if (!m_folders.isEmpty()) {
            m_folders.clear();
            emit foldersChanged();
        }
        if (m_current != -1) {
            m_current = -1;
            emit currentChanged();
        }
        m_error.clear();
        m_loading = false;
        m_loadedLoggedIn = false;
        if (m_videos) m_videos->clearFeed();
        return;
    }

    // 目录空，或者登录态刚从"未登录"变成"已登录"（mid 变了，旧目录作废）
    if (m_folders.isEmpty() || !m_loadedLoggedIn) {
        loadFolders();
    } else if (m_videos && m_videos->count() == 0) {
        // 目录在、列表空（上次拉失败了）→ 重拉当前夹子
        m_videos->load();
    }
}

void FavStore::loadFolders()
{
    if (m_loading) return;
    const QString mid = DouyinApi::instance()->userMid();
    if (mid.isEmpty()) {
        m_error = QString::fromUtf8("还没拿到用户信息，请稍后重试");
        emit foldersChanged();
        return;
    }
    m_loading = true;
    m_error.clear();
    emit foldersChanged();

    // up_mid 必须带：不带的话接口会按 cookie 猜，但 BB10 这边的 cookie 头是手工拼的，
    // 显式传 mid 最稳。
    const QString url = "https://api.bilibili.com/x/v3/fav/folder/created/list-all?up_mid="
                        + mid + "&web_location=333.1387";
    DouyinApi::instance()->get(url, m_tagPrefix + ":folders");
}

void FavStore::selectFolder(int index)
{
    if (index < 0 || index >= m_folders.size()) return;
    // 已经选中且已经有内容 → 不重复请求（避免每次点都清空重拉）
    if (m_current == index && m_videos && m_videos->count() > 0) return;

    m_current = index;
    emit currentChanged();
    if (m_videos) {
        m_videos->setFavMediaId(m_folders.at(index).id);
        m_videos->load();
    }
}

void FavStore::onNetReply(const QString& tag, const QString& json, bool ok)
{
    if (!tag.startsWith(m_tagPrefix)) return;
    if (!tag.endsWith(QLatin1String(":folders"))) return;

    m_loading = false;
    if (!ok) {
        m_error = QString::fromUtf8("网络请求失败，点刷新重试");
        m_diagLast = "net-fail";
        emit foldersChanged();
        return;
    }

    JsonDataAccess jda;
    const QVariantMap root = jda.loadFromBuffer(json.toUtf8()).toMap();
    const int code = root.value("code").toInt();
    if (code != 0) {
        m_error = (code == -101) ? QString::fromUtf8("登录已失效，请到设置页重新登录")
                                 : ("API code " + QString::number(code));
        m_diagLast = QString("code=%1").arg(code);
        emit foldersChanged();
        return;
    }

    // 没有收藏夹时 data 是 null —— toMap() 给空 map，不会崩
    const QVariantList list = root.value("data").toMap().value("list").toList();
    m_folders.clear();
    for (int i = 0; i < list.size(); ++i) {
        const QVariantMap m = list.at(i).toMap();
        Folder f;
        f.id = m.value("id").toLongLong();
        f.title = m.value("title").toString();
        f.mediaCount = m.value("media_count").toInt();
        if (f.id <= 0) continue;
        m_folders.append(f);
    }
    m_loadedLoggedIn = true;
    m_error.clear();
    m_diagLast = QString("code=0 folders=%1").arg(m_folders.size());
    emit foldersChanged();

    if (m_folders.isEmpty()) {
        if (m_current != -1) {
            m_current = -1;
            emit currentChanged();
        }
        return;
    }
    // 目录到手就自动选第一个（通常是"默认收藏夹"）。
    // 这一步刻意放在 C++：QML 里把 C++ 信号 connect 到 JS 函数在 BB10 上
    // 静默不连接，靠 QML 做这个时序协调会得到一个永远空着的列表。
    selectFolder(0);
}

// ============================================================================
// 【v89】DownloadStore —— "已下载"板块
// ============================================================================

DownloadStore::DownloadStore(QObject* parent)
    : QObject(parent)
    , m_count(0)
{
    // 和 FeedModel 一样用 ArrayDataModel：Cascades 的 ListView 只认它的子类。
    m_model = new bb::cascades::ArrayDataModel(this);

    // 提示条自动消失。Cascades 1.1 的 QML 里没有 Timer 类型（见 MEMORY 铁律），
    // 定时器一律放 C++ 侧。
    m_tipTimer = new QTimer(this);
    m_tipTimer->setSingleShot(true);
    connect(m_tipTimer, SIGNAL(timeout()), this, SLOT(onTipTimeout()));

    // 【v93】封面补下完成后就地更新那一行。编译期 connect，可靠。
    connect(DouyinApi::instance(), SIGNAL(downloadFinished(QString,QString,bool)),
            this, SLOT(onCoverFinished(QString,QString,bool)));
    // 【v93】给老下载补元数据的回调
    connect(DouyinApi::instance(), SIGNAL(netReply(QString,QString,bool)),
            this, SLOT(onMetaReply(QString,QString,bool)));
}

QObject* DownloadStore::model() const
{
    return m_model;
}

QString DownloadStore::bvidAt(int row) const
{
    if (!m_model || row < 0 || row >= m_model->size()) return QString();
    return m_model->value(row).toMap().value("bvid").toString();
}

/**
 * 【v91】长按菜单点了"删除" —— 只记下待删项，不真删。
 *
 * 删除是不可逆的（几十 MB 的 mp4，删错要重新下载），所以必须有二次确认。
 * 确认层由 DownloadPage 绑定 pendingBvid / pendingTitle 来显示，
 * 这里只负责把"待删的是谁"告诉 QML。
 *
 * 标题从当前 model 里反查：长按菜单只给了 bvid，而确认层上要显示
 * "删除「视频标题」？" —— 光一个 BV 号用户根本认不出是哪个视频。
 */
void DownloadStore::requestDelete(const QString& bvid)
{
    // 诊断日志：长按菜单点了"删除"但确认层没弹时，先看这条有没有打出来。
    // 没打 = QML 侧根本没调到（作用域/绑定问题）；打了但 pendingBvid 是空 = 传参丢了。
    qDebug() << "BerryLi requestDelete bvid=" << bvid;

    if (bvid.isEmpty()) return;

    QString title = bvid;
    if (m_model) {
        for (int i = 0; i < m_model->size(); ++i) {
            const QVariantMap row = m_model->value(i).toMap();
            if (row.value("bvid").toString() == bvid) {
                const QString t = row.value("title").toString();
                if (!t.isEmpty()) title = t;
                break;
            }
        }
    }

    m_pendingBvid = bvid;
    m_pendingTitle = title;
    emit pendingDeleteChanged();
}

void DownloadStore::confirmDelete()
{
    if (m_pendingBvid.isEmpty()) return;

    const QString bvid = m_pendingBvid;
    const QString title = m_pendingTitle;

    QDir home(QDir::homePath());
    const QString dir = home.absoluteFilePath("videos");

    // mp4 是正片，json 是下载时写的 sidecar（标题/封面/UP 主/时长）。
    // 两个必须一起删：只删 mp4 会留下孤儿 json；只删 json 则列表里
    // 标题会退化成 BV 号。
    const bool okVideo = QFile::remove(dir + "/" + bvid + ".mp4");
    QFile::remove(dir + "/" + bvid + ".json");

    qDebug() << "BerryLi confirmDelete bvid=" << bvid << "removed=" << okVideo;

    m_pendingBvid.clear();
    m_pendingTitle.clear();
    emit pendingDeleteChanged();

    reload();

    if (okVideo) {
        setTip(QString::fromUtf8("已删除：") + title);
    } else {
        // 常见原因：文件被播放器占着、或已经被别的途径删掉了。
        setTip(QString::fromUtf8("删除失败：文件不存在或不可写"));
    }
}

void DownloadStore::cancelDelete()
{
    if (m_pendingBvid.isEmpty() && m_pendingTitle.isEmpty()) return;
    m_pendingBvid.clear();
    m_pendingTitle.clear();
    emit pendingDeleteChanged();
}

void DownloadStore::onMetaReply(const QString& tag, const QString& json, bool ok)
{
    if (!tag.startsWith(QLatin1String("dlt:"))) return;
    if (!ok || json.isEmpty()) return;

    const QString bvid = tag.mid(4);
    JsonDataAccess jda;
    const QVariantMap root = jda.loadFromBuffer(json.toUtf8()).toMap();
    if (root.value("code").toInt() != 0) return;

    const QVariantMap data = root.value("data").toMap();
    const QString title = data.value("title").toString();
    if (title.isEmpty()) return;

    const QString pic = data.value("pic").toString();
    const QString owner = data.value("owner").toMap().value("name").toString();
    const int dur = data.value("duration").toInt();

    // 写回 sidecar，下次 reload 直接就有标题，不用再请求
    QVariantMap meta;
    meta.insert("bvid", bvid);
    meta.insert("title", title);
    meta.insert("pic", pic);
    meta.insert("duration", dur);
    meta.insert("owner", owner);
    // ts 用 int 不用 qint64：JsonDataAccess 对 LongLong 的支持存疑，
    // 序列化失败是静默的（save 返回 void），整个 sidecar 就白写了。
    meta.insert("ts", (int)QDateTime::currentDateTime().toTime_t());
    QDir home(QDir::homePath());
    jda.save(meta, home.absoluteFilePath("videos/" + bvid + ".json"));

    if (!m_model) return;
    // 只需要判断"这一条在不在当前列表里"，不再手工拼替换行 —— 行内容一律由
    // reload() 从 sidecar + 磁盘重新生成，单一数据源。
    for (int i = 0; i < m_model->size(); ++i) {
        const QVariantMap row = m_model->value(i).toMap();
        if (row.value("bvid").toString() != bvid) continue;

        // 【v103】原来这里是 `m_model->replace(i, row)`，和封面是同一个坑：
        // BB10 上「模型行变化 → ListView 重绘」不可靠，下载完成后停在"已下载"页
        // 看到的标题永远是 BV 号，必须切走再切回来才刷新。
        //
        // 改成整体 reload()。代价是列表重建一次（可能闪一下），但：
        //   1) 这份模型只扫 videos/*.mp4，条目数是"下载了几个视频"，几十行封顶；
        //   2) 在途下载不会出现在这里（reload 只认 .mp4，.part 不算），
        //      所以重建不会把"下载中"的进度行弄丢；
        //   3) 每个视频一辈子只会走到这里一次（标题补全后就不再请求）。
        // 正确性优先于一次闪烁。
        reload();
        break;
    }
}

void DownloadStore::onCoverFinished(const QString& tag, const QString& path, bool ok)
{
    if (!tag.startsWith(QLatin1String("dlc:"))) return;
    if (!m_coverRows.contains(tag)) return;   // 列表已经重扫过，这一条不再关心
    m_coverRows.remove(tag);

    // 【v103】成功时**什么都不用做** —— DouyinApi::onDownloadFinished 已经校验过
    // 并把这张封面登记进就绪表、bump 了 coverVersion，QML 绑定会自己重算。
    //
    // 原来这里走 m_model->replace(row, item)，和列表页是同一个坑：
    // BB10 上"模型行变化 → ListView 重绘"不可靠，补下载的封面永远不出现。
    if (ok) return;

    // 失败留下的 0 字节/半个文件必须删掉，否则下次会被当成有效缓存命中
    QFile::remove(path);
}

void DownloadStore::onTipTimeout()
{
    if (m_tip.isEmpty()) return;
    m_tip.clear();
    emit tipChanged();
}

void DownloadStore::setTip(const QString& text)
{
    m_tip = text;
    emit tipChanged();
    if (m_tipTimer) m_tipTimer->start(2600);
}

/**
 * 【v89】"已下载"目录扫描用的临时结构。
 *
 * 必须放在**文件作用域**：C++03（BB10 的 qcc 就是这个标准）不允许把函数内的
 * 局部类型当模板参数用，`QList<Entry>` 写在一个函数里会直接编译失败：
 *   template argument for 'class QList' uses local type 'DownloadStore::reload()::Entry'
 */
struct DlEntry {
    QString bvid;
    QString path;
    QDateTime ts;
    qint64 size;
};

void DownloadStore::reload()
{
    if (!m_model) return;
    m_model->clear();
    m_count = 0;
    // 行号全部重排，旧的补封面映射作废（回调里会因查不到而丢弃）
    m_coverRows.clear();

    QDir home(QDir::homePath());
    const QString dirPath = home.absoluteFilePath("videos");
    QDir vdir(dirPath);

    QList<DlEntry> list;

    if (vdir.exists()) {
        // *.mp4 就是"下载完成"的唯一判据：分段下载先写 .part，全部拼好才改名成
        // .mp4（见 VideoStore::finishSegmented），所以在途的下载不会出现在这里。
        const QStringList files = vdir.entryList(QStringList() << "*.mp4", QDir::Files, QDir::Time);
        for (int i = 0; i < files.size(); ++i) {
            const QString name = files.at(i);
            QFileInfo fi(vdir.absoluteFilePath(name));
            if (fi.size() <= 1024) continue;   // 残骸/空文件不算
            DlEntry e;
            e.bvid = name.left(name.length() - 4);   // 去掉 ".mp4"
            e.path = fi.absoluteFilePath();
            e.ts = fi.lastModified();
            e.size = fi.size();
            list.append(e);
        }
    }

    for (int i = 0; i < list.size(); ++i) {
        const DlEntry& e = list.at(i);
        QVariantMap row;
        row.insert("bvid", e.bvid);

        /**
         * 【v92】把 DownloadStore 自己作为一行数据带下去。
         *
         * 为什么要把 this 塞进 QVariantMap：
         * 长按菜单的 ActionItem 声明在 ListItemComponent 里，而那个作用域里
         * **既取不到文档级 id，也不保证能访问 context property（downloadStore）**。
         * v91 就是在 onTriggered 里直接写 `downloadStore.requestDelete(...)`，
         * 结果长按菜单能弹、点"删除"毫无反应 —— JS 在取值那一步就抛了 ReferenceError。
         *
         * 把对象指针放进 ListItemData 后，QML 侧只需要在 **属性绑定**里取它
         * （`store: ListItemData.store`），属性绑定一定会求值；
         * JS 函数体里只引用 delegate 根节点的 id（编译期绑定）。
         * 整条链路不依赖任何"作用域穿透"。
         */
        row.insert("store", QVariant::fromValue(static_cast<QObject*>(this)));

        // sidecar：下载完成时 VideoStore 写的同名 json
        QString title = e.bvid;
        QString cover = QString();
        long long dur = 0;
        QString owner;
        QFile mf(vdir.absoluteFilePath(e.bvid + ".json"));
        if (mf.open(QIODevice::ReadOnly)) {
            const QByteArray raw = mf.readAll();
            mf.close();
            JsonDataAccess jda;
            const QVariantMap meta = jda.loadFromBuffer(raw).toMap();
            const QString t = meta.value("title").toString();
            if (!t.isEmpty()) title = t;
            cover = meta.value("pic").toString();
            dur = meta.value("duration").toLongLong();
            owner = meta.value("owner").toString();
        }

        // 【v93】json 读不出标题就回退纯文本 sidecar（VideoStore 在 json 写坏时留的）。
        // 这是"已下载列表里视频没有标题、只剩 BV 号"的兜底：
        // BB10 的 JsonDataAccess::save() 失败是静默的，光看代码根本发现不了。
        if (title == e.bvid) {
            QFile tf(vdir.absoluteFilePath(e.bvid + ".title"));
            if (tf.open(QIODevice::ReadOnly)) {
                QTextStream ts(&tf);
                ts.setCodec("UTF-8");
                const QString t = ts.readLine().trimmed();
                const QString o = ts.readLine().trimmed();
                const QString p = ts.readLine().trimmed();
                const QString d = ts.readLine().trimmed();
                tf.close();
                if (!t.isEmpty()) title = t;
                if (cover.isEmpty() && !p.isEmpty()) cover = p;
                if (owner.isEmpty() && !o.isEmpty()) owner = o;
                if (dur <= 0 && !d.isEmpty()) dur = d.toLongLong();
            }
        }
        row.insert("title", title);

        // 【v93】标题还等于 bvid = sidecar 缺失或里面没有标题
        // （v92 及以前下载的视频，或者下载时详情还没加载完）。
        // 发一次 view 请求把真标题补回来；离线时请求失败，就保持 BV 号。
        // m_metaAsked 跨 reload 保留，避免每次切 tab 都重发一遍。
        if (title == e.bvid && !m_metaAsked.contains(e.bvid)) {
            m_metaAsked.insert(e.bvid);
            QVariantMap params;
            params.insert("bvid", e.bvid);
            // view 接口必须带 WBI 签名（裸请求会 -412），和播放页一致
            DouyinApi::instance()->getSigned("/x/web-interface/view", params,
                                           "dlt:" + e.bvid);
        }

        // 【v103】封面和列表页一样：行里只存**远端 URL（永不变化）**，
        // 本地路径由 QML 的 biliApi.coverUrlFor(url, coverVersion) 现场解析。
        //
        // 原来这里存 file:// 并在 onCoverFinished 里 m_model->replace(row, item)，
        // 和列表页是同一个坑：BB10 上"模型行变化 → ListView 重绘"不可靠，
        // 补下载的封面永远不出现。现在补下载完成后 coverVersion 一变，
        // 绑定自己重算，完全不需要碰模型。
        QString picUrl;
        if (!cover.isEmpty()) {
            picUrl = cover;   // 远端 URL，coverUrlFor 内部会归一化（加 @672w 等）
            const QString local = DouyinApi::coverCachePath(cover);
            if (!local.isEmpty() && !(QFileInfo(local).exists()
                                      && DouyinApi::isUsableImage(local))) {
                // 缓存缺失就补一张。封面下载 20 秒超时，不会占死并发槽位。
                const QString tag = QString("dlc:%1").arg(e.bvid);
                if (!m_coverRows.contains(tag)) {
                    m_coverRows.insert(tag, m_count);   // 行号只留作诊断
                    DouyinApi::instance()->download(DouyinApi::coverUrl(cover), local,
                                                  tag, -1, -1, -1, true);
                }
            }
        }
        row.insert("picUrl", picUrl);
        row.insert("api", QVariant::fromValue(static_cast<QObject*>(DouyinApi::instance())));
        row.insert("durationText", dur > 0 ? DouyinApi::durationText(dur) : QString());

        // metaText：体积 + 下载日期。比"作者名"有用 —— 这一页是用来管理存储的。
        QString sizeText;
        if (e.size >= 1024LL * 1024 * 1024) {
            sizeText = QString::number(e.size / (1024.0 * 1024.0 * 1024.0), 'f', 1)
                       + QString::fromUtf8(" GB");
        } else {
            sizeText = QString::number(e.size / (1024.0 * 1024.0), 'f', 1)
                       + QString::fromUtf8(" MB");
        }
        QString meta = sizeText;
        if (!owner.isEmpty()) meta = owner + QString::fromUtf8(" · ") + meta;
        if (e.ts.isValid()) meta += QString::fromUtf8(" · ") + e.ts.toString("MM-dd");
        row.insert("metaText", meta);
        row.insert("typeName", QString::fromUtf8("已下载"));

        m_model->append(row);
        m_count++;
    }

    emit countChanged();
}
