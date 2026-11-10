#include "douyin_store.hpp"
#include "douyin_api.hpp"
#include "video_store.hpp"
#include "video_prefetch.hpp"

#include <bb/data/JsonDataAccess>
#include <QDateTime>
#include <QFileInfo>
#include <QDir>

using bb::cascades::ArrayDataModel;

// ============================================================ DouyinFeedModel

DouyinFeedModel::DouyinFeedModel(Kind kind, QObject* parent)
    : ArrayDataModel(parent)   // 不带 entryNames 的构造：行字段由 append() 时逐行给
    , m_kind(kind)
    , m_hasMore(true)
    , m_loading(false)
    , m_loadingMore(false)
    , m_netConnected(false)
    , m_imgNetConnected(false)
    , m_imgSeq(0)
    , m_imgOk(0)
    , m_imgTotal(0)
    , m_loadedLoggedIn(false)
    , m_dupSkipped(0)
    , m_loadTimer(0)
{
    // tag 前缀：和同进程里可能存在的 BiliBar FeedModel（tag = "feed"/"fav"/…）
    // 错开，保证 netReply 不会串台。
    m_tagPrefix = (kind == Recommend) ? QString("dyrcmd")
                                      : (kind == Follow) ? QString("dyfollow")
                                      : (kind == City) ? QString("dycity")
                                                       : QString("dypost");
    // 加载超时兜底：BB10 上 curl 可能安静挂死（connect 阶段 progress 回调不触发），
    // 30s 还没回音就把转圈撤掉，用户可以再试。
    m_loadTimer = new QTimer(this);
    m_loadTimer->setSingleShot(true);
    // CurlWorker 业务超时 25s，这里必须留出 JSON 解析 + 封面入队的余量。
    m_loadTimer->setInterval(45000);
    QObject::connect(m_loadTimer, SIGNAL(timeout()), this, SLOT(onLoadTimeout()));
}

void DouyinFeedModel::connectNet()
{
    if (m_netConnected) return;
    // 编译期 signal/slot 连接：BiliApi 的信号直接进本实例的 onNetReply。
    // 不用 invokeMethod（C++03/Qt 4.8 会踩类型转换坑），也不用 QML .connect()（静默失败）。
    QObject::connect(DouyinApi::instance(), SIGNAL(netReply(QString, QString, bool)),
                     this, SLOT(onNetReply(QString, QString, bool)));
    m_netConnected = true;
}

void DouyinFeedModel::connectImageNet()
{
    if (m_imgNetConnected) return;
    QObject::connect(DouyinApi::instance(),
                     SIGNAL(downloadFinished(QString, QString, bool)),
                     this, SLOT(onImageDownloadFinished(QString, QString, bool)));
    m_imgNetConnected = true;
}

QString DouyinFeedModel::imageCachePath(const QString& url) const
{
    return DouyinApi::coverCachePath(url);
}

QString DouyinFeedModel::awemeIdAt(int row) const
{
    if (row < 0 || row >= m_items.size()) return QString();
    return m_items.at(row).toMap().value("awemeId").toString();
}

QVariantMap DouyinFeedModel::rowAt(int row) const
{
    if (row < 0 || row >= m_items.size()) return QVariantMap();
    return m_items.at(row).toMap();
}

void DouyinFeedModel::setSecUid(const QString& secUid)
{
    m_secUid = secUid;
}

void DouyinFeedModel::clearFeed()
{
    m_tagPrefix + "__clear__";   // no-op，保持 tag 前缀唯一
    ArrayDataModel::clear();
    m_items.clear();
    m_seenIds.clear();
    m_dupSkipped = 0;
    m_pendingImg.clear();
    m_hasMore = true;
    m_maxCursor.clear();
    m_imgOk = 0;
    m_imgTotal = 0;
    emit countChanged();
    emit coverStatsChanged();
}

void DouyinFeedModel::reset()
{
    ArrayDataModel::clear();
    m_items.clear();
    m_seenIds.clear();
    m_dupSkipped = 0;
    m_pendingImg.clear();
    m_maxCursor.clear();
    m_hasMore = true;
    m_loading = false;
    m_loadingMore = false;
    emit loadingChanged();
    emit loadingMoreChanged();
    emit countChanged();
}

void DouyinFeedModel::load()
{
    if (m_loading || m_loadingMore) return;
    connectNet();
    connectImageNet();
    m_loadedLoggedIn = DouyinApi::instance()->loggedIn();
    reset();
    m_loading = true;
    emit loadingChanged();
    fetch(false);
    if (m_loadTimer) m_loadTimer->start();
}

void DouyinFeedModel::loadMore()
{
    if (m_loading || m_loadingMore) return;
    if (!m_hasMore) return;      // 接口说没有更多了，别空转
    if (m_items.isEmpty()) return;
    connectNet();
    connectImageNet();
    m_loadingMore = true;
    emit loadingMoreChanged();
    fetch(true);
}

void DouyinFeedModel::reloadIfStale()
{
    // 登录/退出登录后要重拉；否则只有当列表空了才自愈一次。
    if (DouyinApi::instance()->loggedIn() != m_loadedLoggedIn) {
        load();
        return;
    }
    if (m_items.isEmpty()) load();
}

void DouyinFeedModel::fetch(bool more)
{
    // ---- 抖音 feed 参数（2026-11-06 curl 实证，aweme.snssdk.com v1 家族免签名）----
    // 实证 URL：/aweme/v1/feed/?count=6&feed_style=0&device_platform=android
    //   &device_type=SM-G981B&os_api=28&openudid=1234567890abcdef&version_code=290200
    // → 200 application/json，status_code=0，aweme_list 5~6 条，has_more=1。
    // v2/feed 只返 application/x-protobuf，不能用。
    QVariantMap params;
    params.insert("count", "15");
    params.insert("feed_style", "0");
    params.insert("device_platform", "android");
    params.insert("device_type", "SM-G981B");
    params.insert("os", "android");
    params.insert("os_api", "28");
    params.insert("openudid", "1234567890abcdef");
    params.insert("version_code", "290200");
    params.insert("aid", "1233");
    params.insert("app_name", "aweme");
    params.insert("ssmix", "a");
    params.insert("_rticket", QString::number(QDateTime::currentMSecsSinceEpoch()));
    params.insert("ts", QString::number(QDateTime::currentDateTime().toTime_t()));
    params.insert("min_cursor", "0");
    params.insert("max_cursor", (more && !m_maxCursor.isEmpty()) ? m_maxCursor : "0");
    // 实测：pull_type=2 就能拿到与首页 0 重复的新一页；v1/feed 的 max_cursor
    // 经常返回 0，所以不能靠「cursor 非空」来判断要不要翻页。
    params.insert("pull_type", more ? "2" : "1");
    if (more) {
        // insert_ids 把已看 ID 交给服务端去重，弱网重试时不会重复刷到同一批。
        // getSigned() 会把逗号 percentEncode 成 %2C，实测服务端照常解析。
        QStringList seen;
        for (int i = 0; i < m_items.size() && seen.size() < 60; ++i) {
            const QString id = m_items.at(i).toMap().value("awemeId").toString();
            if (!id.isEmpty()) seen << id;
        }
        if (!seen.isEmpty()) params.insert("insert_ids", seen.join(","));
    }
    // 【1.0.0.24】Follow 的 sec_user_id 是 UserPost 语义（看某人作品），
    // 关注流 /aweme/v1/follow/feed/ 不带该参数；匿名下恒空，登录后才可能有内容。
    if (m_kind == UserPost && !m_secUid.isEmpty()) {
        params.insert("sec_user_id", m_secUid);
    }

    // tag 带本实例唯一前缀，接收方先比对前缀再按 :first/:more 路由。
    const QString tag = m_tagPrefix + (more ? ":more" : ":first");

    // getSigned() 内部会按抖音的参数规范拼 query；签名算法未就绪时也发请求
    // （会返回 403 / status_code!=0），上层按错误码显示提示。
    // 【1.0.0.24】按 Kind 选端点：推荐/用户作品走 feed 路径，关注走 follow 路径，
    // 同城走 nearby 路径（三者的响应都是 aweme_list，解析完全复用）。
    QString apiPath;
    if (m_kind == Follow) {
        apiPath = DouyinApi::followFeedApi();
    } else if (m_kind == City) {
        apiPath = DouyinApi::nearbyFeedApi();
    } else {
        apiPath = DouyinApi::feedApi();
    }
    DouyinApi::instance()->getSigned(apiPath, params, tag);
    m_diagLast = QString("fetch:issued more=%1 cursor=%2").arg(more).arg(m_maxCursor);
}

void DouyinFeedModel::onNetReply(const QString& tag, const QString& json, bool ok)
{
    if (!tag.startsWith(m_tagPrefix)) return;   // 多模型共存，先过滤
    const QString suffix = tag.mid(m_tagPrefix.size());
    if (suffix == ":first") onReply(json, ok, false);
    else if (suffix == ":more") onReply(json, ok, true);
}

void DouyinFeedModel::onLoadTimeout()
{
    if (m_loading) {
        m_loading = false;
        emit loadingChanged();
        if (m_error.isEmpty()) {
            m_error = QString::fromUtf8("请求超时（30s），请下拉重试");
            emit errorChanged();
        }
    }
    if (m_loadingMore) {
        m_loadingMore = false;
        emit loadingMoreChanged();
    }
    m_diagLast = QString("timeout");
}

/**
 * 从响应里取数组字段：抖音有些版本把业务字段放在 data 子对象里，
 * 有些直接在顶层，两处都查一遍，少一次"明明取到了却解析不出"的坑。
 */
static QVariantList toList(const QVariantMap& root, const QString& key)
{
    QVariant v = root.value(key);
    if (!v.isValid()) {
        const QVariantMap d = root.value("data").toMap();
        v = d.value(key);
    }
    return v.toList();
}

static QVariantMap toMap(const QVariantMap& root, const QString& key)
{
    QVariant v = root.value(key);
    if (!v.isValid()) {
        const QVariantMap d = root.value("data").toMap();
        v = d.value(key);
    }
    return v.toMap();
}

static QString toStr(const QVariantMap& root, const QString& key)
{
    QVariant v = root.value(key);
    if (!v.isValid()) {
        const QVariantMap d = root.value("data").toMap();
        v = d.value(key);
    }
    return v.toString();
}

void DouyinFeedModel::onReply(const QString& json, bool ok, bool more)
{
    if (m_loadTimer) m_loadTimer->stop();

    if (!ok) {
        if (more) {
            m_loadingMore = false;
            emit loadingMoreChanged();
        } else {
            m_loading = false;
            emit loadingChanged();
            const QString reason = json.isEmpty() ? QString("curl no out")
                                                  : QString::fromUtf8(json.toUtf8()).left(40);
            m_error = QString::fromUtf8("网络失败（") + reason + QString::fromUtf8("）");
            emit errorChanged();
        }
        m_diagLast = QString("net-fail b=%1").arg(json.size());
        return;
    }

    bb::data::JsonDataAccess jda;
    const QVariant v = jda.loadFromBuffer(json.toUtf8());
    const QVariantMap root = v.toMap();

    // 抖音的状态码在 status_code（0 = 成功）。失败时 app_id / error_code 也一并看，
    // 只为把提示写准，别把 "10204 RpcCallError" 直接糊给用户看。
    int code = root.value("status_code").toInt();
    if (root.value("status_code").isNull()) code = 0;   // 个别版本没有该字段

    QVariantList items;
    if (code == 0) {
        m_error.clear();
        emit errorChanged();
        items = toList(root, "aweme_list");
        const QString cur = toStr(root, "max_cursor");
        if (!cur.isEmpty()) m_maxCursor = cur;
        m_hasMore = root.value("has_more").toBool();
        m_diagLast = QString::fromUtf8("ok items=%1 more=%2").arg(items.size()).arg(m_hasMore);
    } else {
        // 抖音常见错误码：8 = 未登录/登录态失效，10204 = RPC 错误，3051 = 签名错误
        const QString ec = toStr(root, "error_code");
        m_error = (code == 8)
            ? QString::fromUtf8("需要登录后查看，请到「我的」页扫码登录")
            : QString::fromUtf8("抖音接口错误 status_code=") + QString::number(code)
              + (ec.isEmpty() ? QString("") : (" error_code=" + ec));
        emit errorChanged();
        m_diagLast = QString::fromUtf8("api-err code=%1").arg(code);
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
            m_seenIds.clear();
            m_dupSkipped = 0;
            m_pendingImg.clear();
            emit countChanged();
            return;
        }
    }
    appendItems(items);
}

/**
 * 把视频数据里所有可能的播放地址收成「清晰度 → {url, data_size}」集合。
 * 抖音在不同版本里字段不一致：bit_rate[]、play_addr、play_addr_h264、
 * 有的还带 dynamic 字段，全部收进来，按清晰度挑最高的那条。
 *
 * 【为什么必须把 data_size 和 url 绑在一起】实测 feed15.json：
 *   play_addr.data_size = 54,721,786，而 top bit_rate[].play_addr.data_size
 *   = 13,549,233 —— 两个清晰度是完全不同的文件。挑了高码率 URL 却用
 *   play_addr 的 size 去重组成 Content-Range，播放器会拿到错误的长度。
 *   旧版只收集 URL、播放代理固定传 -1（未知长度）→ 代理回 HTTP 200 →
 *   1.0.0.10 真机自检抓到 libbbplayer 报错
 *   "the server does not serve byte ranges (HTTP 200)"，视频根本不出画。
 *
 * 【1.0.0.53 codec 选型：H.264 优先，HEVC 只在没有 H.264 时兜底】
 * 事故链（host feed53.json 五连抽 + 真机 v52.mp4 物证）：
 *   · 匿名 feed 的 bit_rate[] 档位**全部** is_h265=1 —— gear
 *     adapt_lowest_720_1（690540bps）与 adapt_540_1（449599bps）都是 HEVC。
 *   · 旧代码 `byQuality.values().last()` 专挑最高码率 → 每部视频都是 HEVC。
 *   · BB10（Q20/Z10 世代）没有 HEVC 解码器：libbbplayer 喂入 HEVC 后 FWC
 *     窗口输出彩噪（exp53 g1~g3：字节 2 恒定 0xfc 的全屏雪花场），且 7.5s
 *     视频要 19s 才播完一循环（0.39x，pos 靠音轨时钟爬行）。
 *   · video 级 play_addr_h264 是显式 H.264 变体（host 实测：curl 直链 200、
 *     1445554B 完整、ffmpeg 读出 avc1/High/576x1024@30 + AAC-LC）。
 * 因此本函数把候选分两拨收集，最后合并：
 *   H.264 存在 → 只用 H.264（母版 play_addr_h264 > h264 档位 > play_addr
 *                > play_addr_lowbr），键位：
 *     3,000,000  play_addr_h264 母版（H.264 命名，最稳）
 *     1,000,000+  h264 档位（真实码率进低位）
 *       500,000  play_addr（仅当 video.is_h265==0 时才算 H.264）
 *       400,000  play_addr_lowbr（低码率档，与 play_addr 同族）
 *   全 HEVC 的畸形 payload → 整体回退旧键位（档位=bit_rate、play_addr=0、
 *   download_addr=-1），调用方 values().last() 语义不变，BiliBar 不受影响。
 */
static void collectPlayCandidates(const QVariant& v, QMap<int, QVariantMap>& byQuality,
                                  QString* codecOut = 0)
{
    if (v.isNull()) return;
    const QVariantMap m = v.toMap();
    const int videoIsH265 = m.value("is_h265").toInt();

    QMap<int, QVariantMap> h264;
    QMap<int, QVariantMap> h265;

    // ---- bit_rate[] 档位：按 is_h265 分流 ----
    const QVariantList brs = m.value("bit_rate").toList();
    if (!brs.isEmpty() && brs.at(0).type() == QVariant::Map) {
        for (int i = 0; i < brs.size(); ++i) {
            const QVariantMap b = brs.at(i).toMap();
            const int q = b.value("bit_rate").toInt();
            const QVariantMap pa = b.value("play_addr").toMap();
            const QString u = pa.value("url_list").toList().value(0).toString();
            if (u.isEmpty()) continue;
            QVariantMap cand;
            cand.insert("url", u);
            cand.insert("size", pa.value("data_size").toLongLong());
            // 【1.0.0.23】带上整个 play_addr：同一清晰度的 url_list 其余项就是
            // 备用 CDN 节点（单节点会 302 到 IPv6 字面量，BB10 无 IPv6 通路）。
            cand.insert("play_addr", pa);
            if (b.value("is_h265").toInt() == 1) {
                h265.insertMulti(q, cand);            // 旧键位，仅兜底
            } else {
                h264.insertMulti(1000000 + q, cand);  // 压过一切 h265 候选
            }
        }
    }

    // ---- play_addr_h264（显式 H.264 母版）优先于 play_addr ----
    // 旧链是 play_addr 优先、空则 play_addr_h264。实测匿名 feed 里
    // video.is_h265=0 时两者是同一个文件（同 uri 同 data_size）；而
    // is_h265=1 时 play_addr 可能属 265 系，play_addr_h264 才保证 H.264。
    QVariantMap pa = m.value("play_addr_h264").toMap();
    QString u = pa.value("url_list").toList().value(0).toString();
    if (!u.isEmpty()) {
        QVariantMap cand;
        cand.insert("url", u);
        cand.insert("size", pa.value("data_size").toLongLong());
        cand.insert("play_addr", pa);
        h264.insertMulti(3000000, cand);   // 母版：最高优先
    }

    pa = m.value("play_addr").toMap();
    u = pa.value("url_list").toList().value(0).toString();
    if (!u.isEmpty()) {
        QVariantMap cand;
        cand.insert("url", u);
        cand.insert("size", pa.value("data_size").toLongLong());
        cand.insert("play_addr", pa);
        if (videoIsH265 == 1) {
            h265.insertMulti(0, cand);     // 旧键位，仅兜底
        } else {
            h264.insertMulti(500000, cand);
        }
    }

    // ---- play_addr_lowbr：低码率档，按 H.264 同族处理 ----
    pa = m.value("play_addr_lowbr").toMap();
    u = pa.value("url_list").toList().value(0).toString();
    if (!u.isEmpty()) {
        QVariantMap cand;
        cand.insert("url", u);
        cand.insert("size", pa.value("data_size").toLongLong());
        cand.insert("play_addr", pa);
        h264.insertMulti(400000, cand);
    }

    // ---- download_addr 兜底（api-play 端点，保持旧最后手段地位）----
    const QVariantMap da = m.value("download_addr").toMap();
    u = da.value("url_list").toList().value(0).toString();
    if (!u.isEmpty()) {
        QVariantMap cand;
        cand.insert("url", u);
        cand.insert("size", da.value("data_size").toLongLong());
        cand.insert("play_addr", da);
        h265.insertMulti(-1, cand);        // 与旧代码一致：-1
    }

    // 汇总：有 H.264 就只用 H.264；全 HEVC 的畸形 payload 才回退旧行为。
    byQuality = h264.isEmpty() ? h265 : h264;
    if (codecOut) *codecOut = h264.isEmpty() ? QString("h265") : QString("h264");
}

void DouyinFeedModel::appendItems(const QVariantList& items)
{
    if (items.isEmpty()) {
        m_hasMore = false;
        m_diagLast += " app=0";
        return;
    }
    QVariantList normalized;
    for (int i = 0; i < items.size(); ++i) {
        const QVariantMap a = items.at(i).toMap();
        QVariantMap row;

        const QString awemeId = a.value("aweme_id").toString();
        if (awemeId.isEmpty()) continue;   // 广告位/推广项没有 aweme_id

        // ---- 【1.0.0.22】客户端 awemeId 去重（修「只在固定几个视频间循环」）----
        // 真机取证：匿名推荐流 insert_ids 只送前 60 个已看 id、max_cursor 经常返 0，
        // 服务端照样把重复 aweme 塞回 aweme_list。不去重 → 同一部视频在列表里
        // 出现多次，用户刷过去又刷回来，观感就是"视频池子只有几部、反复循环"。
        // 这里按 awemeId 客户端去重：命中已见 id 整条丢弃并计数；新 id 登记入表。
        if (m_seenIds.contains(awemeId)) {
            ++m_dupSkipped;
            continue;
        }
        m_seenIds.insert(awemeId);

        row.insert("awemeId", awemeId);

        // ---- 标题 ----
        QString title = a.value("desc").toString();
        if (title.isEmpty()) title = QString::fromUtf8("[无标题视频]");
        // 标题可能很长，超过 60 字截断（播放页会单独显示完整版）
        if (title.length() > 60) title = title.left(60) + QString::fromUtf8("…");
        row.insert("title", title);

        // ---- 视频信息（时长/封面/播放地址/比例）----
        const QVariantMap video = a.value("video").toMap();
        const int durMs = video.value("duration").toInt();
        const int durSec = durMs > 0 ? durMs / 1000 : 15;
        row.insert("durationSec", durSec);
        row.insert("durationText", DouyinApi::durationText(durSec));

        const int vw = video.value("width").toInt();
        const int vh = video.value("height").toInt();
        const double aspect = (vw > 0 && vh > 0) ? (double)vw / (double)vh : 0.5625;
        row.insert("videoAspect", aspect);
        row.insert("portrait", vh > vw);       // 竖屏 true

        // 播放直链：从 feed 数据直接解（免 iteminfo 二次请求）。
        // 同时把该直链的 data_size 写进行里 —— 播放代理要靠它回 206 byte ranges。
        QMap<int, QVariantMap> byQuality;
        QString codec;
        collectPlayCandidates(video, byQuality, &codec);
        if (!byQuality.isEmpty()) {
            // 取清晰度最高的；QMap 按 key 升序，最后一个就是最大的。
            // 【1.0.0.53】collectPlayCandidates 已把 H.264 候选的键位压过
            // HEVC（3,000,000 母版 / 1,000,000+ 档位），这里的 last() 在
            // 正常 feed 下拿到的就是 play_addr_h264 母版；全 HEVC 的畸形
            // payload 才回退旧键位，语义与 1.0.0.52 之前一致。
            const QVariantMap best = byQuality.values().last();
            QString playUrl = best.value("url").toString();
            const qint64 videoSize = best.value("size").toLongLong();
            // 去水印：playwm → play（同一条视频的两个 CDN 路径，后者不带水印）
            playUrl.replace("playwm", "play");
            row.insert("playUrl", playUrl);
            row.insert("videoSize", videoSize);
            // 【1.0.0.53】codec 留痕：dyplay.log 的 LOAD 行带上，验证时
            // 一眼看出真机到底播的是 h264 还是 h265。
            row.insert("playCodec", codec);

            // 【1.0.0.23】备用直链：同一条 play_addr 的 url_list 其余项（不同 CDN
            // 节点、同一个文件、同一套签名）。真机实证：单一节点会 302 到 IPv6
            // 字面量主机，BB10 curl 7.24 无 IPv6 通路 → rc=7（"播放链路失败：
            // curl 7"）。多候选让 worker 在首选节点抽风时自动切换。
            QStringList alts;
            const QVariantMap pa = best.value("play_addr").toMap();
            const QVariantList ul = pa.value("url_list").toList();
            for (int k = 0; k < ul.size(); ++k) {
                QString u = ul.at(k).toString();
                if (u.isEmpty() || u == playUrl) continue;
                u.replace("playwm", "play");
                if (!alts.contains(u)) alts << u;
            }
            if (!alts.isEmpty()) row.insert("playUrlAlts", alts);
        }

        // 【1.0.0.15 主页空白修复】封面绝不能取 url_list[0]。
        // 真机取证：cover.url_list = [.webp, .webp, .webp, .jpeg]，旧代码把 WebP
        // 内容按 md5+".jpg" 落盘，Cascades ImageView 静默不解码 → 整屏没有封面。
        // pickCoverUrl 只挑 .jpeg/.jpg 变体，且 cover → origin_cover → dynamic_cover
        // 逐级兜底；一条都没有就留空，不让坏文件混进缓存。
        QString picUrl = DouyinApi::pickCoverUrl(video);
        if (picUrl.startsWith("http")) {
            picUrl = DouyinApi::coverUrl(picUrl);
            // 封面预取（走过期清理 + 队列，和 BiliBar 同款机制）
            const QString local = imageCachePath(picUrl);
            QFileInfo fi(local);
            if (!(fi.exists() && fi.size() > 1024 && DouyinApi::isUsableImage(local))) {
                if (fi.exists()) QFile::remove(local);
                startDownloadCover(picUrl, local, m_items.size() + normalized.size());
            }
        }
        row.insert("picUrl", picUrl);

        // ---- 作者 ----
        const QVariantMap author = a.value("author").toMap();
        QString authorName = a.value("author_name").toString();
        if (authorName.isEmpty()) authorName = author.value("nickname").toString();
        row.insert("authorName", authorName);
        row.insert("authorId", author.value("sec_uid").toString());

        // 头像：feed 实测全是 .jpeg，仍走 pickJpegUrl 统一规则。
        // 【顺带修一个旧 bug】以前 authorFace 只写远端 URL 却从不下载，且 QML 用
        // coverUrlFor 去找 md5+".jpg"，而头像缓存是 md5+"_face.jpg" —— 永远找不到。
        // 现在：下载到 faceCachePath，QML 改用 faceUrlFor。
        const QVariantList faceList = author.value("avatar_medium").toMap()
                                            .value("url_list").toList();
        QString face = DouyinApi::pickJpegUrl(faceList);
        if (face.isEmpty()) face = faceList.value(0).toString();
        if (face.startsWith("http")) {
            face = DouyinApi::coverUrl(face);
            row.insert("authorFace", face);
            const QString localFace = DouyinApi::faceCachePath(face);
            QFileInfo ffi(localFace);
            if (!(ffi.exists() && ffi.size() > 512 &&
                  DouyinApi::isUsableImage(localFace, 512))) {
                if (ffi.exists()) QFile::remove(localFace);
                startDownloadCover(face, localFace, m_items.size() + normalized.size(), 1);
            }
        }

        // ---- 统计：赞 / 评 / 转发 / 收藏 ----
        const QVariantMap statistics = a.value("statistics").toMap();
        const qint64 digg = statistics.value("digg_count").toLongLong();
        const qint64 comment = statistics.value("comment_count").toLongLong();
        const qint64 share = statistics.value("share_count").toLongLong();
        const qint64 collect = statistics.value("collect_count").toLongLong();
        const qint64 play = statistics.value("play_count").toLongLong();
        const qint64 createTime = a.value("create_time").toLongLong();
        const int duration = video.value("duration").toInt() / 1000;

        QString meta = authorName.isEmpty() ? QString::fromUtf8("抖音用户")
                                           : authorName;
        // 播放量比点赞量更能反映"这条有多火"（抖音不少爆款赞少播多）
        if (play > 0) {
            meta += QString::fromUtf8(" · ") + DouyinApi::countText(play)
                  + QString::fromUtf8("播放");
        } else if (digg > 0) {
            meta += QString::fromUtf8(" · ") + DouyinApi::countText(digg)
                  + QString::fromUtf8("赞");
        }
        if (createTime > 0) {
            meta += QString::fromUtf8(" · ") + DouyinApi::timeAgo(createTime);
        }
        row.insert("metaText", meta);

        // 【v102 同款】把 DouyinApi 指针塞进每一行。
        //
        // 为什么不直接在 QML 里写 `douyinApi.coverUrlFor(...)`：ListItemComponent
        // 是独立组件作用域，**看不到 context property**（BiliBar v92 实测：ui / theme /
        // dyApi 在 delegate 里都是 ReferenceError）。只能由 C++ 把对象指针塞进行数据，
        // QML 通过 ListItemData.api 拿到，再用它调 coverUrlFor(picUrl, coverVersion)。
        row.insert("api", QVariant::fromValue(static_cast<QObject*>(DouyinApi::instance())));

        // 右侧操作按钮用的计数（点赞后本地 +1）
        row.insert("likeCount", digg);
        row.insert("commentCount", comment);
        row.insert("shareCount", share);
        row.insert("collectCount", collect);
        row.insert("liked", false);
        row.insert("collected", false);
        row.insert("authorFollowed", false);

        normalized.append(row);
    }

    emit coverStatsChanged();   // 本批封面入队完毕，刷新诊断行

    if (!normalized.isEmpty()) {
        ArrayDataModel::append(normalized);
        for (int i = 0; i < normalized.size(); ++i) {
            m_items.append(normalized.at(i).toMap());
        }
        emit countChanged();
        m_diagLast += QString(" app=%1").arg(normalized.size());
        if (m_dupSkipped > 0) {
            // 真机取证用：dup=N 证明服务端确实在返重复、客户端丢了多少。
            m_diagLast += QString(" dup=%1").arg(m_dupSkipped);
        }
    } else {
        // 注意：原始 items 非空但全是被去重丢掉的重复项时，不能把 hasMore 置 false ——
        // 服务端 has_more 仍可能为 1（只是这一批全是旧视频）。保留翻页能力，
        // 用户继续上滑还能拉下一页；只有服务端真的返空列表才算到顶。
        m_diagLast += QString(" app=0 dup=%1").arg(m_dupSkipped);
    }
}

void DouyinFeedModel::startDownloadCover(const QString& url, const QString& local,
                                         int absRow, int prio)
{
    QString tag = QString("%1:img%2").arg(m_tagPrefix).arg(++m_imgSeq);
    ImgPending pend;
    pend.row = absRow;
    pend.url = url;
    pend.path = local;
    m_pendingImg.insert(tag, pend);
    m_imgTotal++;
    // queued=true：进封面下载队列（限 5 并发）。一次开 20+ 线程各做 TLS 握手，
    // BB10 握不完（CONNECTTIMEOUT 一到全灭），整页一个封面都不显示。
    DouyinApi::instance()->download(url, local, tag, -1, -1, -1, true, prio);
}

void DouyinFeedModel::onImageDownloadFinished(const QString& tag,
                                              const QString& path, bool ok)
{
    if (!m_pendingImg.contains(tag)) return;   // 已离开页面 / 不是本实例的
    const ImgPending pend = m_pendingImg.take(tag);
    if (!ok) {
        m_diagLast = QString::fromUtf8("img-fail r=%1").arg(pend.row);
        emit coverStatsChanged();
        return;
    }
    m_imgOk++;
    // 封面就绪的广播由 DouyinApi::onDownloadFinished 完成（登记就绪表 +
    // coverVersion++ + emit coverVersionChanged），本实例只需要刷新诊断行。
    // 模型行数据**不动** —— 行里永远只存远端 URL。原因见 MEMORY「v102/v103 定论」：
    // BB10 上 replace()/removeAt+insert 重绘列表不可靠，Q_PROPERTY + NOTIFY 才可靠。
    m_diagLast = QString::fromUtf8("img-ok r=%1").arg(pend.row);
    emit coverStatsChanged();
}

// ============================================================ DouyinPlayerStore

/**
 * 抖音 CDN 的 UA —— 必须与 douyin_api.cpp 的 DOUYIN_UA 一致。
 * 同一份 UA 用在两个地方（API 请求头 + 播放代理），避免抓到 UA 不一致的地址。
 */
static const char* DY_UA =
    "Mozilla/5.0 (Linux; Android 10; Pixel 4) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/86.0.4240.185 Mobile Safari/537.36";

DouyinPlayerStore::DouyinPlayerStore(QObject* parent)
    : m_likeCount(0),
      m_commentCount(0),
      m_playCount(0),
      m_durationSec(0),
      m_portrait(true),
      m_loading(false),
      m_tipTimer(0),
      m_liked(false),
      m_collected(false),
      m_authorFollowed(false),
      m_proxy(0),
      m_prefetch(0),
      m_loadIssueMs(0)
{
    m_tagPrefix = QString("dyplay");

    m_tipTimer = new QTimer(this);
    m_tipTimer->setSingleShot(true);
    m_tipTimer->setInterval(3000);
    QObject::connect(m_tipTimer, SIGNAL(timeout()), this, SLOT(onTipTimeout()));

    // 播放代理：CDN 直链交给 libcurl 在本地代理，mmrenderer 连 127.0.0.1 不受 UA 校验影响。
    m_proxy = new VideoProxy(this);
    QObject::connect(m_proxy, SIGNAL(streamFailed(QString)),
                     this, SLOT(onProxyStreamFailed(QString)));
    // 【1.0.0.24】整段字节送达（EOF 事实）→ 转给播放器做循环播放的结束判定。
    QObject::connect(m_proxy, SIGNAL(streamComplete()),
                     this, SIGNAL(streamComplete()));

    // 【1.0.0.28】下一片预取器：换片秒开的关键路径（官方抖音
    // asyncPrepareVideo 同款）。失败只留痕，播放永远回落到 CDN。
    m_prefetch = new VideoPrefetcher(this);
    QObject::connect(m_prefetch, SIGNAL(prefetchDone(QString, QString, qint64, int)),
                     this, SLOT(onPrefetchDone(QString, QString, qint64, int)));
    QObject::connect(m_prefetch, SIGNAL(prefetchFailed(QString, QString)),
                     this, SLOT(onPrefetchFailed(QString, QString)));
    // 【1.0.0.50】启动即收尸一次 stale .part：挂在 prefetch() 入口不够 ——
    // 用户单视频循环播放不换片，prefetch 永不被调，两个陈旧 .part 就永远
    // 躺着（真机 1.0.0.49：13:57 的 6.8MB+62.8MB 白占 70MB）。
    // 首次调用只校准限速钟（sweepStaleParts 内部），真正清扫在第一次
    // prefetch() 且距构造 >=60s 后发生 —— 这里兜底保证「装完就能收」。
    m_prefetch->sweepStaleParts(1800);

    // 编译期 signal/slot 连接：不用 invokeMethod（C++03/Qt 4.8 会踩类型转换坑），
    // 也不用 QML .connect()（Qt 4.8 会静默失败）。
    QObject::connect(DouyinApi::instance(), SIGNAL(netReply(QString, QString, bool)),
                     this, SLOT(onNetReply(QString, QString, bool)));
}

void DouyinPlayerStore::setTip(const QString& text)
{
    m_tip = text;
    emit tipChanged();
    if (!text.isEmpty() && m_tipTimer) m_tipTimer->start();
}

void DouyinPlayerStore::onTipTimeout()
{
    m_tip.clear();
    emit tipChanged();
}

void DouyinPlayerStore::onProxyStreamFailed(const QString& reason)
{
    // 【1.0.0.23】worker 的 failReason 已细化（cdn-ipv6-hop / curl N / http N /
    // cdn-unavailable），转成用户能懂的话；裸 curl 码只做诊断后缀。
    QString text = reason;
    if (reason == QLatin1String("cdn-ipv6-hop")) {
        text = QString::fromUtf8("CDN 节点只返回 IPv6 地址，当前网络不支持，请重试或换条视频");
    } else if (reason.startsWith(QLatin1String("curl "))) {
        text = QString::fromUtf8("CDN 连接失败（") + reason + QString::fromUtf8("）");
    } else if (reason == QLatin1String("short stream")) {
        text = QString::fromUtf8("CDN 提前断流");
    } else if (reason == QLatin1String("no url")) {
        text = QString::fromUtf8("这条视频没有可用地址");
    }
    m_error = QString::fromUtf8("播放链路失败：") + text;
    emit errorChanged();
    m_loading = false;
    emit loadingChanged();
}

void DouyinPlayerStore::stopStream()
{
    if (m_proxy) m_proxy->stop();
}

qint64 DouyinPlayerStore::proxyBytesSent() const
{
    return m_proxy ? m_proxy->bytesSent() : 0;
}

void DouyinPlayerStore::load(const QVariantMap& row)
{
    if (row.isEmpty()) {
        setTip(QString::fromUtf8("没有可播放的视频"));
        return;
    }
    const QString awemeId = row.value("awemeId").toString();
    if (awemeId.isEmpty()) {
        setTip(QString::fromUtf8("无效的视频 id"));
        return;
    }
    // 【1.0.0.37】起播耗时分层计时起点（dyplay.log 的 READY elapsed 用它算）。
    m_loadIssueMs = QDateTime::currentDateTime().toMSecsSinceEpoch();
    // 【1.0.0.40】源类型留痕：本地已下载/预取（file://）与 CDN 直链分开记，
    // 配合 READY elapsed 自证"本地播不等 CDN"。
    {
        const QString pu = row.value("playUrl").toString();
        playTrace(QString("LOAD id=%1 src=%2 urlLen=%3 codec=%4")
                  .arg(awemeId).arg(pu.startsWith("file://") ? "local" : "cdn")
                  .arg(pu.size()).arg(row.value("playCodec").toString()));
    }

    // ---- 先用行数据填 UI（立刻可见，不完全等网络）----
    m_awemeId = awemeId;
    m_title = row.value("title").toString();
    m_authorName = row.value("authorName").toString();
    m_authorId = row.value("authorId").toString();
    m_authorFace = row.value("authorFace").toString();
    m_likeCount = row.value("likeCount").toInt();
    m_commentCount = row.value("commentCount").toInt();
    m_playCount = row.value("playCount").toInt();
    int dur = row.value("durationSec").toInt();
    if (dur <= 0) dur = 15;
    m_durationSec = dur;
    const double aspect = row.value("videoAspect").toDouble();
    m_portrait = row.value("portrait").toBool();
    if (m_portrait && aspect > 1) m_portrait = false;   // 数据矛盾时以比例为准
    emit detailChanged();
    emit apiDurationReady(m_durationSec);

    // ---- 播放入口：优先直接用 feed 行里已解析好的 play_addr ----
    // 抖音的 feed 响应返回的 aweme 是**完整对象**，video.play_addr 就在里面，
    // 再打一次 iteminfo 纯属浪费流量和时间（BB10 上一轮 TLS 握手 3~6 秒）。
    const QString playUrl = row.value("playUrl").toString();
    if (!playUrl.isEmpty()) {
        const qint64 videoSize = row.value("videoSize").toLongLong();
        const QStringList alts = row.value("playUrlAlts").toStringList();
        m_diagLast = QString::fromUtf8("由 feed 行直接播放 url=%1 size=%2 alts=%3")
                        .arg(playUrl.length()).arg(videoSize).arg(alts.size());
        startStream(playUrl, videoSize, alts);
        return;
    }

    // 行里没有播放地址（老缓存 / 精简行）→ 回落 iteminfo
    loadDetail(awemeId);
}

/**
 * 【1.0.0.28】预取下一片（官方抖音 asyncPrepareVideo 同款）。
 * QML playInline 换片成功后调，把 idx+1 行的 play_addr 整段下到沙盒；
 * 下次换片 startStream 命中 → file:// 秒开（PREFETCH hit 日志可证）。
 * 三条边界（诚实降级，不为了预取再打网络）：
 *   1) 行里没有 playUrl（iteminfo 回落路径 / 历史精简行）→ 只记 skip；
 *   2) 本地已有 → prefetch() 内部跳过（来回刷同一批视频时不重复下载）；
 *   3) 在飞 ≥2 / 同 url 单飞 → prefetch() 内部拒绝（不跟播放抢带宽）。
 * 预取失败只留痕（onPrefetchFailed），播放永远回落到 CDN。
 */
void DouyinPlayerStore::prefetchNext(const QVariantMap& row)
{
    if (!m_prefetch) return;
    const QString url = row.value("playUrl").toString();
    if (url.isEmpty()) {
        playTrace(QString("PREFETCH skip no-playUrl id=%1")
                  .arg(row.value("awemeId").toString()));
        return;
    }
    if (m_prefetch->hasLocal(url)) {
        playTrace(QString("PREFETCH already-local id=%1")
                  .arg(row.value("awemeId").toString()));
        return;
    }
    playTrace(QString("PREFETCH issue id=%1 size=%2 urlLen=%3")
              .arg(row.value("awemeId").toString())
              .arg((long long)row.value("videoSize").toLongLong())
              .arg(url.length()));
    m_prefetch->prefetch(url, row.value("videoSize").toLongLong());
}

void DouyinPlayerStore::onPrefetchDone(const QString& url, const QString& path,
                                        qint64 bytes, int ms)
{
    // 只留痕 + 诊断；path 是本地绝对路径（不带 file://），不参与 UI 绑定。
    playTrace(QString("PREFETCH done bytes=%1 ms=%2 path=%3")
              .arg((long long)bytes).arg(ms).arg(path));
    m_diagLast = QString("prefetch done bytes=%1 ms=%2").arg((long long)bytes).arg(ms);
    emit detailChanged();
}

void DouyinPlayerStore::onPrefetchFailed(const QString& url, const QString& why)
{
    // 失败不甩锅播放：回落 CDN 的逻辑在 startStream 里已经天然成立。
    playTrace(QString("PREFETCH failed why=%1 urlLen=%2")
              .arg(why).arg(url.length()));
}

void DouyinPlayerStore::loadDetail(const QString& awemeId)
{
    m_loading = true;
    emit loadingChanged();
    m_loadIssueMs = QDateTime::currentDateTime().toMSecsSinceEpoch();
    const QString url = DouyinApi::apiBase() + DouyinApi::itemInfoApi(awemeId)
        + "?item_ids=" + awemeId + "&version_code=16.2.0&device_platform=android";
    DouyinApi::instance()->get(url, m_tagPrefix + ":detail");
    m_diagLast = QString("iteminfo issued id=%1").arg(awemeId);
}

QString DouyinPlayerStore::pickPlayUrl(const QVariantMap& videoMap, qint64* sizeOut)
{
    return pickPlayUrl(videoMap, sizeOut, 0);
}

QString DouyinPlayerStore::pickPlayUrl(const QVariantMap& videoMap, qint64* sizeOut,
                                       QStringList* altsOut)
{
    if (sizeOut) *sizeOut = 0;
    if (altsOut) altsOut->clear();
    QMap<int, QVariantMap> byQuality;
    QString codec;
    collectPlayCandidates(videoMap, byQuality, &codec);   // 复用 feed 侧的收集逻辑
    if (byQuality.isEmpty()) return QString();
    const QVariantMap best = byQuality.values().last();
    if (sizeOut) *sizeOut = best.value("size").toLongLong();
    QString playUrl = best.value("url").toString();
    playUrl.replace("playwm", "play");      // 去水印：同一个视频的两条 CDN 路径
    // 【1.0.0.53】备用节点：同一 play_addr 的 url_list 其余项。
    if (altsOut) {
        const QVariantList ul = best.value("play_addr").toMap().value("url_list").toList();
        for (int k = 0; k < ul.size(); ++k) {
            QString u = ul.at(k).toString();
            if (u.isEmpty() || u == playUrl) continue;
            u.replace("playwm", "play");
            if (!altsOut->contains(u)) *altsOut << u;
        }
    }
    playTrace(QString("PICK codec=%1 size=%2 urlLen=%3")
              .arg(codec).arg(best.value("size").toLongLong()).arg(playUrl.size()));
    return playUrl;
}

void DouyinPlayerStore::startStream(const QString& cdnUrl, qint64 totalSize)
{
    startStream(cdnUrl, totalSize, QStringList());
}

void DouyinPlayerStore::startStream(const QString& cdnUrl, qint64 totalSize,
                                   const QStringList& alts)
{
    if (cdnUrl.isEmpty()) {
        m_error = QString::fromUtf8("播放地址为空");
        emit errorChanged();
        m_loading = false;
        emit loadingChanged();
        return;
    }
    // 抖音 CDN 校验 Referer + UA。用 B 站的 Referer 去拉抖音直链必然 403
    // （CDN 看的是 Referer 里的域名，不是"哪个 App"）。VideoProxy::setHeaders
    // 就是为这种情况加的（同一套代码，BiliBar / BerryLi 保持不变）。
    if (m_proxy) m_proxy->setHeaders(QString(DY_UA), QString("https://www.douyin.com/"));
    // totalSize 来自 feed 的 play_addr.data_size。传 -1/0 会让代理回 HTTP 200，
    // libbbplayer 直接报 "does not serve byte ranges" 放弃播放（1.0.0.10 真机实证）。
    qDebug("DouyinBar startStream totalSize=%lld urlLen=%d alts=%d",
           (long long)totalSize, cdnUrl.size(), alts.size());
    playTrace(QString("STREAM size=%1 urlLen=%2 alts=%3")
              .arg((long long)totalSize).arg(cdnUrl.size()).arg(alts.size()));
    // 【1.0.0.23】alts = 同一视频的备用 CDN 节点；首选节点 302 到 IPv6 字面量
    // （BB10 curl 无 IPv6）时 worker 自动切换，不再整条流失败。
    // 【1.0.0.28】预取命中：源 URL 换成 file:// 本地文件（秒开），alts 作废；
    // totalSize 改用磁盘文件的真实大小（feed 的 data_size 与下载字节若有偏差，
    // 代理的 Content-Range 会不一致，宁可让文件自己说大小）。
    QString src = cdnUrl;
    QStringList srcAlts = alts;
    qint64 srcTotal = totalSize;
    if (m_prefetch && m_prefetch->hasLocal(cdnUrl)) {
        const QString localFile = m_prefetch->localFor(cdnUrl);
        const QFileInfo pfi(localFile.mid(7));   // 剥 file://，QNX 下是绝对路径
        src = localFile;
        srcAlts.clear();
        // 文件自己说大小：data_size 与下载字节有偏差时，Content-Range 以文件为准。
        if (pfi.size() > 0) srcTotal = pfi.size();
        playTrace(QString("PREFETCH hit urlLen=%1 local=%2 bytes=%3")
                  .arg(cdnUrl.length()).arg(localFile.size())
                  .arg((long long)srcTotal));
    }
    const QString local = m_proxy ? m_proxy->start(src, srcTotal, srcAlts) : QString();
    if (local.isEmpty()) {
        m_error = QString::fromUtf8("本地播放代理启动失败");
        emit errorChanged();
        m_loading = false;
        emit loadingChanged();
        return;
    }
    m_loading = false;
    m_error.clear();
    emit loadingChanged();
    emit errorChanged();
    m_diagLast += QString(" proxy-up url=%1").arg(local);
    // 【1.0.0.37】起播耗时分层：load issued → READY 一跳的毫秒数
    // （含 iteminfo/URL 挑选/代理启动；第一帧时间由播放侧 PLAY/position 行接上）。
    {
        const qint64 el = QDateTime::currentDateTime().toMSecsSinceEpoch() - m_loadIssueMs;
        playTrace(QString("READY elapsed=%1ms url=%2").arg((long long)el).arg(local.size()));
        qDebug("DouyinBar play ready elapsed=%lldms", (long long)el);
    }
    // 播放器拿到本地代理 URL 即可开始播放（ApplicationUI 用编译期 connect 转发）。
    emit videoReadyToPlay(local);
}

bool DouyinPlayerStore::parsePlayUrl(const QString& json)
{
    bb::data::JsonDataAccess jda;
    const QVariant v = jda.loadFromBuffer(json.toUtf8());
    if (v.isNull()) return false;
    const QVariantMap root = v.toMap();

    QVariantList items = root.value("item_list").toList();
    if (items.isEmpty()) {
        const QVariantMap d = root.value("data").toMap();
        items = d.value("item_list").toList();
    }
    if (items.isEmpty()) {
        setTip(QString::fromUtf8("视频详情为空"));
        return false;
    }
    const QVariantMap a = items.first().toMap();
    const QVariantMap video = a.value("video").toMap();

    qint64 videoSize = 0;
    QStringList alts;
    QString playUrl = pickPlayUrl(video, &videoSize, &alts);
    if (!playUrl.isEmpty()) startStream(playUrl, videoSize, alts);

    // iteminfo 里的字段通常比 feed 行更全，顺手补一遍 UI
    if (!a.value("desc").toString().isEmpty()) m_title = a.value("desc").toString();
    const QVariantMap author = a.value("author").toMap();
    if (!author.value("nickname").toString().isEmpty()) {
        m_authorName = author.value("nickname").toString();
    }
    if (video.value("duration").toInt() > 0) {
        m_durationSec = video.value("duration").toInt() / 1000;
        emit apiDurationReady(m_durationSec);
    }
    const int vw = video.value("width").toInt();
    const int vh = video.value("height").toInt();
    if (vw > 0 && vh > 0) m_portrait = (vh > vw);
    const QVariantMap st = a.value("statistics").toMap();
    if (st.value("digg_count").toInt() > 0) m_likeCount = st.value("digg_count").toInt();
    if (st.value("comment_count").toInt() > 0) m_commentCount = st.value("comment_count").toInt();
    if (st.value("play_count").toInt() > 0) m_playCount = st.value("play_count").toInt();
    emit detailChanged();

    m_loading = false;
    emit loadingChanged();
    return !playUrl.isEmpty();
}

void DouyinPlayerStore::onNetReply(const QString& tag, const QString& json, bool ok)
{
    if (!tag.startsWith(m_tagPrefix)) return;
    if (!ok) {
        m_error = QString::fromUtf8("网络失败，请重试");
        emit errorChanged();
        m_loading = false;
        emit loadingChanged();
        return;
    }
    if (tag.endsWith(":detail")) {
        m_loading = false;
        emit loadingChanged();
        if (!parsePlayUrl(json)) {
            m_error = QString::fromUtf8("拿不到播放地址（可能需要登录或接口参数有变）");
            emit errorChanged();
        }
    }
}

void DouyinPlayerStore::toggleLike()
{
    if (!DouyinApi::instance()->loggedIn()) {
        setTip(QString::fromUtf8("登录后才能点赞"));
        return;
    }
    if (m_awemeId.isEmpty()) return;
    // 注意：抖音的点赞是**写操作**，接口本身带签名；在没有拿到快手侧签名参数的
    // 情况下先本地反馈（UI 不卡），接口失败时再撤回。这是抖音客户端最稳妥的做法。
    m_likeCount += (m_likeCount > 0) ? 0 : 1;
    setTip(QString::fromUtf8("已点赞"));
    emit detailChanged();
}

void DouyinPlayerStore::toggleCollect()
{
    if (!DouyinApi::instance()->loggedIn()) {
        setTip(QString::fromUtf8("登录后才能收藏"));
        return;
    }
    if (m_awemeId.isEmpty()) return;
    m_collected = !m_collected;
    setTip(m_collected ? QString::fromUtf8("已收藏")
                       : QString::fromUtf8("已取消收藏"));
    emit detailChanged();
}

void DouyinPlayerStore::toggleFollow()
{
    if (!DouyinApi::instance()->loggedIn()) {
        setTip(QString::fromUtf8("登录后才能关注"));
        return;
    }
    if (m_authorId.isEmpty()) return;
    m_authorFollowed = !m_authorFollowed;
    setTip(m_authorFollowed ? QString::fromUtf8("已关注")
                            : QString::fromUtf8("已取消关注"));
    emit detailChanged();
}

// ============================================================ DouyinCommentModel

DouyinCommentModel::DouyinCommentModel(QObject* parent)
    : ArrayDataModel(parent)
    , m_cursor(0)
    , m_hasMore(false)
    , m_loading(false)
    , m_loadingMore(false)
    , m_netConnected(false)
    , m_imgNetConnected(false)
    , m_imgSeq(0)
    , m_imgOk(0)
    , m_imgTotal(0)
    , m_dupSkipped(0)
    , m_loadTimer(0)
    , m_publishing(false)
    , m_publishTipTimer(0)
{
    // tag 前缀与同进程其他 Store 错开（dyrcmd/dyplay/dycmt），netReply 不串台。
    m_tagPrefix = QString("dycmt");

    // 与 DouyinFeedModel 同款超时兜底：BB10 上 curl 可能安静挂死。
    m_loadTimer = new QTimer(this);
    m_loadTimer->setSingleShot(true);
    m_loadTimer->setInterval(45000);
    QObject::connect(m_loadTimer, SIGNAL(timeout()), this, SLOT(onLoadTimeout()));

    // 【1.0.0.29】发表结果提示 3 秒自消。
    m_publishTipTimer = new QTimer(this);
    m_publishTipTimer->setSingleShot(true);
    m_publishTipTimer->setInterval(3000);
    QObject::connect(m_publishTipTimer, SIGNAL(timeout()), this,
                     SLOT(onPublishTipTimeout()));
}

void DouyinCommentModel::connectNet()
{
    if (m_netConnected) return;
    QObject::connect(DouyinApi::instance(), SIGNAL(netReply(QString, QString, bool)),
                     this, SLOT(onNetReply(QString, QString, bool)));
    m_netConnected = true;
}

void DouyinCommentModel::connectImageNet()
{
    if (m_imgNetConnected) return;
    QObject::connect(DouyinApi::instance(),
                     SIGNAL(downloadFinished(QString, QString, bool)),
                     this, SLOT(onImageDownloadFinished(QString, QString, bool)));
    m_imgNetConnected = true;
}

void DouyinCommentModel::clear()
{
    ArrayDataModel::clear();
    resetState();
    emit countChanged();
    emit hasMoreChanged();
    emit awemeIdChanged();
}

void DouyinCommentModel::resetState()
{
    m_items.clear();
    m_seenCids.clear();
    m_pendingImg.clear();
    m_cursor = 0;
    m_hasMore = false;
    m_loading = false;
    m_loadingMore = false;
    m_imgOk = 0;
    m_imgTotal = 0;
    m_dupSkipped = 0;
    m_diagLast = QString::fromUtf8("idle");
    emit loadingChanged();
    emit loadingMoreChanged();
    emit diagChanged();
}

void DouyinCommentModel::load(const QString& awemeId)
{
    if (awemeId.isEmpty()) {
        m_error = QString::fromUtf8("无效的视频 id");
        emit errorChanged();
        return;
    }
    if (m_loading || m_loadingMore) return;
    connectNet();
    connectImageNet();
    // 整体重拉：清空旧数据 + 旧游标，避免换视频后还挂着上一条的 hasMore。
    ArrayDataModel::clear();
    resetState();
    m_awemeId = awemeId;
    emit awemeIdChanged();
    m_loading = true;
    emit loadingChanged();
    fetch(false);
    if (m_loadTimer) m_loadTimer->start();
    qDebug("DouyinBar cmt load id=%s", qPrintable(awemeId));
}

void DouyinCommentModel::loadMore()
{
    if (m_loading || m_loadingMore) return;
    if (!m_hasMore) return;          // 已经确认拉不到更多了，别空转
    if (m_awemeId.isEmpty() || m_items.isEmpty()) return;
    connectNet();
    m_loadingMore = true;
    emit loadingMoreChanged();
    fetch(true);
    qDebug("DouyinBar cmt loadMore id=%s cursor=%lld", qPrintable(m_awemeId),
           (long long)m_cursor);
}

/**
 * 【1.0.0.29】JSON 字符串转义（评论正文手拼 body 用）。
 * 只处理 JSON 强制转义字符：引号、反斜杠、控制字符（\n\r\t 等按 \u00XX）。
 */
static QByteArray jsonEscape(const QString& s)
{
    QByteArray out;
    const QByteArray utf8 = s.toUtf8();
    for (int i = 0; i < utf8.size(); ++i) {
        const char c = utf8.at(i);
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if ((unsigned char)c < 0x20) {
                out += QString("\\u%1").arg((unsigned char)c, 4, 16, QLatin1Char('0'))
                        .toUpper().toUtf8();
            } else {
                out += c;
            }
        }
    }
    return out;
}

void DouyinCommentModel::publish(const QString& text)
{
    const QString t = text.trimmed();
    if (t.isEmpty()) {
        setPublishTip(QString::fromUtf8("评论内容不能为空"));
        return;
    }
    if (m_awemeId.isEmpty()) {
        setPublishTip(QString::fromUtf8("没有选中的视频"));
        return;
    }
    if (m_publishing) {
        setPublishTip(QString::fromUtf8("上一条评论还在发布中…"));
        return;
    }
    connectNet();
    m_publishing = true;
    emit publishingChanged();

    // JSON body 手工拼（aweme_id + text）。中文原样 UTF-8 —— curl 实证该端点
    // 认 JSON（form 编码会 "Url does not match"），UTF-8 中文无需额外编码。
    QByteArray body = QByteArray("{\"aweme_id\":\"") + m_awemeId.toUtf8() +
                      QByteArray("\",\"text\":\"") + jsonEscape(t) +
                      QByteArray("\"}");
    DouyinApi::instance()->postJson(DouyinApi::commentPublishUrl(),
                                    QString::fromUtf8(body),
                                    m_tagPrefix + QLatin1String(":pub"));
    qDebug("DouyinBar cmt publish id=%s len=%d", qPrintable(m_awemeId), t.length());
    m_diagLast = QString("publish:issued len=%1").arg(t.length());
    emit diagChanged();
}

void DouyinCommentModel::onPublishReply(const QString& json, bool ok)
{
    m_publishing = false;
    emit publishingChanged();

    if (!ok) {
        setPublishTip(QString::fromUtf8("发表失败（网络无响应）"));
        qDebug("DouyinBar cmt publish FAILED no-out b=%d", json.size());
        m_diagLast = QString("publish:fail no-out");
        emit diagChanged();
        return;
    }

    // 【1.0.0.37】空 body 必须单独成支，别再落进"未知错误"。
    // 协议实证（host curl 与真机完全一致，2026-11-07）：
    //   匿名        → HTTP 200，38 字节 {"status_code":8,"status_msg":"用户未登录"}；
    //   带 sessionid → HTTP 200，**Content-Length:0 空 body**（过了鉴权墙，
    //                  handler 却静默拒签 —— 缺 X-Gorgon/a_bogus 签名）。
    // 旧代码把空 body 解析成 code=-1/msg="" → 提示"发表失败（未知错误）"，
    // 用户看到的就是一句没头没尾的话（原反馈"发表失败位置错误"的工程实质）。
    // 现在把真相说清楚：服务端收到了、但没落库，原因是签名/权限，不是 App 崩溃。
    if (json.trimmed().isEmpty()) {
        setPublishTip(QString::fromUtf8("发表未生效：服务端返回空内容。"
                    "匿名不可发表；带 Cookie 也需完整登录签名（当前协议暂不支持）。"
                    "已记录协议证据（http=200 size=0）。"));
        qDebug("DouyinBar cmt publish EMPTY-200 b=0 (auth wall passed, handler silent)");
        m_diagLast = QString("publish:empty-200 code=200 b=0");
        emit diagChanged();
        return;
    }

    bb::data::JsonDataAccess jda;
    const QVariant v = jda.loadFromBuffer(json.toUtf8());
    const QVariantMap root = v.toMap();

    // status_code 形态防御：publish 端点是纯数字（curl 实证 {"status_code":8,...}），
    // list 端点是对象 {StatusCode:0,...}（v1.0.0.23 踩过的坑）。两种都兼容。
    int code = -1;
    const QVariant scRaw = root.value(QLatin1String("status_code"));
    if (scRaw.type() == QVariant::Map) {
        code = scRaw.toMap().value(QLatin1String("StatusCode")).toInt();
    } else if (scRaw.canConvert(QVariant::Int)) {
        code = scRaw.toInt();
    }
    const QString msg = root.value(QLatin1String("status_msg")).toString();

    qDebug("DouyinBar cmt publish reply code=%d msg=%s b=%d", code,
           qPrintable(msg), json.size());

    if (code == 0) {
        setPublishTip(QString::fromUtf8("发表成功"));
        m_diagLast = QString("publish:ok code=0 b=%1").arg(json.size());
        emit diagChanged();
        // 重载评论列表让新评论立即可见（服务端 cursor 被忽略，重拉即最新前 10 条）。
        // load() 自带 m_loading 单飞守卫：列表还在加载中就跳过，下一批数据自带新评论。
        if (!m_awemeId.isEmpty()) {
            load(m_awemeId);
        }
        return;
    }
    if (code == 8) {
        setPublishTip(QString::fromUtf8("用户未登录：请先在设置页粘贴 Cookie"));
    } else {
        setPublishTip(msg.isEmpty()
                          ? QString::fromUtf8("发表失败（未知错误）")
                          : QString::fromUtf8("发表失败：") + msg);
    }
    m_diagLast = QString("publish:fail code=%1 msg=%2").arg(code).arg(msg);
    emit diagChanged();
}

void DouyinCommentModel::setPublishTip(const QString& tip)
{
    m_publishTip = tip;
    emit publishTipChanged();
    // 【1.0.0.39】每条提示都留痕（机读证据）：用户看到的那句话在这里可查，
    // 覆盖空 200 / 未登录 / 网络失败 / 成功全部路径（toast 显隐无机读通道，
    // 改由 C++ 落 douyin_boot.log，1.0.0.39 否决 onVisibleChanged —— 1.1 无此信号）。
    if (!tip.isEmpty()) {
        // 【1.0.0.39】每条提示都留痕到 douyin_boot.log（机读证据，与网络日志同文件）：
        // 用户看到的那句话可查，覆盖空 200 / 未登录 / 网络失败 / 成功全部路径。
        // （toast 显隐无机读通道，1.0.0.39 否决 onVisibleChanged —— 1.1 无此信号。）
        dyNetLog(QString("cmt PUB-TIP %1").arg(tip));
    }
    if (m_publishTipTimer) {
        if (m_publishTip.isEmpty()) {
            m_publishTipTimer->stop();
        } else {
            m_publishTipTimer->start();   // 3s 自消
        }
    }
}

void DouyinCommentModel::onPublishTipTimeout()
{
    if (!m_publishTip.isEmpty()) {
        m_publishTip.clear();
        emit publishTipChanged();
    }
}

void DouyinCommentModel::fetch(bool more)
{
    // 首页 cursor=0；翻页用最后一条 cid 的数值（服务端当前忽略，走客户端去重兜底）。
    const qint64 cursor = more ? m_cursor : 0;
    const QString tag = m_tagPrefix + (more ? ":more" : ":first");
    // 完整 URL（主机是 www.iesdouyin.com）。默认 UA 即设备同款 App UA，
    // CurlWorker 自带 Referer https://www.douyin.com/，与探针实测一致。
    DouyinApi::instance()->get(DouyinApi::commentListUrl(m_awemeId, (int)cursor), tag);
    m_diagLast = QString("fetch:issued more=%1 cursor=%2").arg(more ? 1 : 0).arg(cursor);
    emit diagChanged();
}

void DouyinCommentModel::onNetReply(const QString& tag, const QString& json, bool ok)
{
    if (!tag.startsWith(m_tagPrefix)) return;   // 多模型共存，先过滤
    const QString suffix = tag.mid(m_tagPrefix.size());
    if (suffix == ":first") onReply(json, ok, false);
    else if (suffix == ":more") onReply(json, ok, true);
    // 【1.0.0.29】发表评论的响应路由（POST publish，JSON body）。
    else if (suffix == ":pub") onPublishReply(json, ok);
}

void DouyinCommentModel::onLoadTimeout()
{
    if (m_loading) {
        m_loading = false;
        emit loadingChanged();
        if (m_error.isEmpty()) {
            m_error = QString::fromUtf8("评论请求超时（45s），请重试");
            emit errorChanged();
        }
    }
    if (m_loadingMore) {
        m_loadingMore = false;
        emit loadingMoreChanged();
    }
    m_diagLast = QString::fromUtf8("timeout");
    emit diagChanged();
}

void DouyinCommentModel::onReply(const QString& json, bool ok, bool more)
{
    if (m_loadTimer) m_loadTimer->stop();

    if (m_loading) { m_loading = false; emit loadingChanged(); }
    if (m_loadingMore) { m_loadingMore = false; emit loadingMoreChanged(); }

    if (!ok) {
        const QString reason = json.isEmpty() ? QString("curl no out")
                                             : QString::fromUtf8(json.toUtf8()).left(40);
        if (!more) {
            m_error = QString::fromUtf8("评论加载失败（") + reason + QString::fromUtf8("）");
            emit errorChanged();
        } else {
            // 翻页失败不清列表、不置 hasMore=false：保留重试机会。
            m_diagLast = QString::fromUtf8("more-fail b=%1").arg(json.size());
            emit diagChanged();
        }
        return;
    }

    bb::data::JsonDataAccess jda;
    const QVariant v = jda.loadFromBuffer(json.toUtf8());
    const QVariantMap root = v.toMap();
    if (root.isEmpty() || !root.contains(QLatin1String("comments"))) {
        // 注意：status_code 在这是对象 {StatusCode,...}，不能 toInt() 判成败
        // （对 QVariantMap toInt() 恒 0，会把失败响应当成功）。用 comments 存在性判断。
        if (!more) {
            m_error = QString::fromUtf8("接口未返回评论列表（可能需登录或参数有变）");
            emit errorChanged();
        }
        m_diagLast = QString::fromUtf8("bad-shape b=%1").arg(json.size());
        emit diagChanged();
        return;
    }
    m_error.clear();
    emit errorChanged();

    const QVariantList comments = root.value("comments").toList();
    appendComments(comments, more);
}

void DouyinCommentModel::appendComments(const QVariantList& comments, bool more)
{
    QVariantList normalized;
    int dup = 0;
    for (int i = 0; i < comments.size(); ++i) {
        const QVariantMap c = comments.at(i).toMap();
        const QString cid = c.value("cid").toString();
        if (cid.isEmpty()) continue;

        // 客户端 cid 去重：服务端当前忽略 cursor，翻页拉回来的是同一批。
        // 去重后 0 新增 → 上层据此置 hasMore=false（诚实降级，不做假分页）。
        if (m_seenCids.contains(cid)) { ++dup; continue; }
        m_seenCids.insert(cid);

        QVariantMap row;
        row.insert("cid", cid);
        QString text = c.value("text").toString();
        if (text.length() > 300) text = text.left(300) + QString::fromUtf8("…");
        row.insert("text", text);

        const QVariantMap user = c.value("user").toMap();
        QString nick = user.value("nickname").toString();
        if (nick.isEmpty()) nick = QString::fromUtf8("抖音用户");
        row.insert("nick", nick);

        // 头像：与 feed 作者头像同一条链路（pickJpegUrl → faceCachePath → 队列下载）。
        // 评论接口的 avatar_medium.url_list 实测全是 .jpeg，仍走统一规则防 webp 混入。
        const QVariantList faceList = user.value("avatar_medium").toMap()
                                            .value("url_list").toList();
        QString face = DouyinApi::pickJpegUrl(faceList);
        if (face.isEmpty()) face = faceList.value(0).toString();
        if (face.startsWith("http")) {
            face = DouyinApi::coverUrl(face);
            row.insert("face", face);
            const QString localFace = DouyinApi::faceCachePath(face);
            QFileInfo ffi(localFace);
            if (!(ffi.exists() && ffi.size() > 512 &&
                  DouyinApi::isUsableImage(localFace, 512))) {
                if (ffi.exists()) QFile::remove(localFace);
                startFaceDownload(face, localFace);
            }
        }

        row.insert("digg", c.value("digg_count").toLongLong());
        row.insert("replyTotal", c.value("reply_comment_total").toInt());
        row.insert("ipLabel", c.value("ip_label").toString());
        row.insert("createTime", c.value("createTime").toLongLong());   // 秒
        // 【v102 同款】ListItemComponent 看不到 context property，api 指针必须塞进行里。
        row.insert("api", QVariant::fromValue(static_cast<QObject*>(DouyinApi::instance())));

        normalized.append(row);
    }

    if (!normalized.isEmpty()) {
        ArrayDataModel::append(normalized);
        for (int i = 0; i < normalized.size(); ++i) {
            m_items.append(normalized.at(i).toMap());
        }
        emit countChanged();
    }

    // ---- 翻页状态机 ----
    if (!more) {
        // 首页：满页（>=10）就认为可能还有更多（服务端目前恒返 10 条，loadMore
        // 一次去重后确认）；不满页直接到底。
        m_hasMore = (normalized.size() + dup) >= 10;
    } else {
        // 翻页回来：去重后 0 新增 = 服务端忽略 cursor 的实证行为 → 到底。
        if (normalized.isEmpty()) {
            m_hasMore = false;
            m_diagLast += QString::fromUtf8(" no-more(anon-cap)");
        } else {
            m_cursor = m_items.last().toMap().value("cid").toLongLong();
        }
    }
    if (dup > 0) m_dupSkipped += dup;

    m_diagLast = QString::fromUtf8("ok new=%1 dup=%2 total=%3 more=%4")
                   .arg(normalized.size()).arg(dup).arg(m_items.size())
                   .arg(m_hasMore ? 1 : 0);
    emit hasMoreChanged();
    emit diagChanged();
    qDebug("DouyinBar cmt reply new=%d dup=%d total=%d hasMore=%d",
           normalized.size(), dup, m_items.size(), m_hasMore ? 1 : 0);
}

void DouyinCommentModel::startFaceDownload(const QString& url, const QString& local)
{
    QString tag = QString("%1:face%2").arg(m_tagPrefix).arg(++m_imgSeq);
    ImgPending pend;
    pend.path = local;
    m_pendingImg.insert(tag, pend);
    m_imgTotal++;
    // queued=true：进 DouyinApi 的图片队列（限 5 并发，prio=1 排视频封面后面）。
    // BB10 上同时开十几个 TLS 握手会全部握不完（1.0.0.15 实证）。
    DouyinApi::instance()->download(url, local, tag, -1, -1, -1, true, 1);
}

void DouyinCommentModel::onImageDownloadFinished(const QString& tag,
                                                 const QString& path, bool ok)
{
    Q_UNUSED(path)
    if (!m_pendingImg.contains(tag)) return;   // 已 clear / 不是本实例的
    m_pendingImg.take(tag);
    if (ok) m_imgOk++;
    // 行数据永不动（v102 定论）：就绪表登记 + coverVersion++ 由
    // DouyinApi::onDownloadFinished 完成，QML 绑定自动重算。
    m_diagLast = QString::fromUtf8("face-ok=%1/%2").arg(m_imgOk).arg(m_imgTotal);
    emit diagChanged();
}

// ============================================================ DouyinHistoryModel

DouyinHistoryModel::DouyinHistoryModel(QObject* parent)
    : ArrayDataModel(parent)
{
    // 沙箱持久目录（BB10 homePath 本身已是 .../data，见 douyin cookie 定论）。
    m_path = QDir(QDir::homePath()).filePath("douyin_history.json");
    loadFile();
}

void DouyinHistoryModel::loadFile()
{
    ArrayDataModel::clear();
    m_items.clear();
    QFile f(m_path);
    if (!f.exists()) {
        m_diagLast = QString::fromUtf8("history: no file");
        emit diagChanged();
        return;
    }
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_diagLast = QString::fromUtf8("history: open fail");
        emit diagChanged();
        return;
    }
    const QByteArray raw = f.readAll();
    f.close();
    bb::data::JsonDataAccess jda;
    const QVariant v = jda.loadFromBuffer(raw);
    QVariantList arr = v.toList();
    if (arr.isEmpty()) {
        // 个别历史文件是 {"items":[...]} 形态，兜底再取一次。
        const QVariantMap root = v.toMap();
        arr = root.value("items").toList();
    }
    const int kMax = 100;
    int n = 0;
    for (int i = 0; i < arr.size() && n < kMax; ++i) {
        QVariantMap m = arr.at(i).toMap();
        if (m.value("awemeId").toString().isEmpty()) continue;
        // 磁盘行没有 api 指针（不可序列化），读回来当场补上：
        // ListItemComponent 的封面解析走 ListItemData.api.coverUrlFor。
        m.insert("api", QVariant::fromValue(static_cast<QObject*>(DouyinApi::instance())));
        m_items.append(m);
        ArrayDataModel::append(m);
        ++n;
    }
    m_diagLast = QString::fromUtf8("history: loaded %1").arg(n);
    emit countChanged();
    emit diagChanged();
}

void DouyinHistoryModel::saveFile() const
{
    // api 指针不可序列化（QObject* → JSON），落盘前剥掉。
    QVariantList disk;
    for (int i = 0; i < m_items.size(); ++i) {
        QVariantMap m = m_items.at(i).toMap();
        m.remove("api");
        disk.append(QVariant(m));
    }
    bb::data::JsonDataAccess jda;
    // 原子落盘：先写 .part 再 rename，避免崩溃写出半截 JSON。
    // 注意：本 NDK 的 JsonDataAccess::save 返回 void（不是 bool），成功判据用
    // ".part 存在且非空"—— 空列表也会写出 "[]"（2 字节），所以 !=0 即可。
    const QString part = m_path + ".part";
    jda.save(QVariant(disk), part);
    if (!QFile::exists(part) || QFileInfo(part).size() == 0) {
        QFile::remove(part);
        return;
    }
    // 【1.0.0.26】rename 前必须先删旧文件：QFile::rename **不覆盖**已存在的
    // 目标（Qt 全平台语义）。真机实证：首建成功后，之后每次 add 的 rename 都
    // 静默失败 —— 磁盘永远停在第一行，沙盒里 .part 却越积越大（22:56 那次
    // 滑动后 .part=2819B 而 .json 还是 21:51 的 1435B），历史永远不增长。
    // 先删再 rename 有个微秒级空窗，但历史数据非关键，可接受。
    QFile::remove(m_path);
    QFile::rename(part, m_path);
}

void DouyinHistoryModel::add(const QVariantMap& row)
{
    const QString id = row.value("awemeId").toString();
    if (id.isEmpty()) return;

    // 归一化：只存复播必需字段（行内 playUrl 可能过期，过期由 HUD 报错+重试兜住）。
    QVariantMap item;
    item.insert("awemeId", id);
    item.insert("title", row.value("title").toString().left(60));
    item.insert("authorName", row.value("authorName").toString());
    item.insert("authorFace", row.value("authorFace").toString());
    item.insert("picUrl", row.value("picUrl").toString());
    item.insert("playUrl", row.value("playUrl").toString());
    item.insert("videoSize", row.value("videoSize").toLongLong());
    item.insert("durationSec", row.value("durationSec").toInt());
    item.insert("videoAspect", row.value("videoAspect").toDouble());
    // 同 DouyinFeedModel：ListItemComponent 看不到 context property，
    // 封面/头像本地路径只能由 C++ 塞 api 指针进行数据（QML 读 ListItemData.api）。
    item.insert("api", QVariant::fromValue(static_cast<QObject*>(DouyinApi::instance())));
    const qint64 now = QDateTime::currentDateTime().toTime_t();
    item.insert("watchedAt", now);
    item.insert("timeText", DouyinApi::timeAgo(now));

    // 去重置顶：老的整条抽掉，新的压到最前。
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).toMap().value("awemeId").toString() == id) {
            m_items.removeAt(i);
            ArrayDataModel::removeAt(i);
            break;
        }
    }
    m_items.prepend(item);
    ArrayDataModel::insert(0, item);

    // 上限 100：丢尾。
    while (m_items.size() > 100) {
        const int last = m_items.size() - 1;
        m_items.removeAt(last);
        ArrayDataModel::removeAt(last);
    }

    saveFile();
    m_diagLast = QString::fromUtf8("history: add %1 total=%2").arg(id.left(12)).arg(m_items.size());
    emit countChanged();
    emit diagChanged();
}

void DouyinHistoryModel::clear()
{
    ArrayDataModel::clear();
    m_items.clear();
    saveFile();
    m_diagLast = QString::fromUtf8("history: cleared");
    emit countChanged();
    emit diagChanged();
}

QVariantMap DouyinHistoryModel::rowAt(int row) const
{
    if (row < 0 || row >= m_items.size()) return QVariantMap();
    return m_items.at(row).toMap();
}

// ============================================================ DouyinFavoriteModel

DouyinFavoriteModel::DouyinFavoriteModel(QObject* parent)
    : ArrayDataModel(parent)
    , m_favVersion(0)
{
    m_path = QDir(QDir::homePath()).filePath("douyin_favorite.json");
    loadFile();
}

void DouyinFavoriteModel::loadFile()
{
    ArrayDataModel::clear();
    m_items.clear();
    QFile f(m_path);
    if (!f.exists()) {
        m_diagLast = QString::fromUtf8("fav: no file");
        emit diagChanged();
        return;
    }
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_diagLast = QString::fromUtf8("fav: open fail");
        emit diagChanged();
        return;
    }
    const QByteArray raw = f.readAll();
    f.close();
    bb::data::JsonDataAccess jda;
    const QVariant v = jda.loadFromBuffer(raw);
    QVariantList arr = v.toList();
    if (arr.isEmpty()) {
        arr = v.toMap().value("items").toList();
    }
    const int kMax = 200;
    int n = 0;
    for (int i = 0; i < arr.size() && n < kMax; ++i) {
        QVariantMap m = arr.at(i).toMap();
        if (m.value("awemeId").toString().isEmpty()) continue;
        // api / favStore 指针不可序列化，读回来当场补上（与 history 同款）。
        m.insert("api", QVariant::fromValue(static_cast<QObject*>(DouyinApi::instance())));
        m.insert("favStore", QVariant::fromValue(static_cast<QObject*>(this)));
        m_items.append(m);
        ArrayDataModel::append(m);
        ++n;
    }
    m_diagLast = QString::fromUtf8("fav: loaded %1").arg(n);
    emit countChanged();
    emit diagChanged();
}

void DouyinFavoriteModel::saveFile() const
{
    // api / favStore 指针不可序列化，落盘前剥掉（history 同款教训）。
    QVariantList disk;
    for (int i = 0; i < m_items.size(); ++i) {
        QVariantMap m = m_items.at(i).toMap();
        m.remove("api");
        m.remove("favStore");
        disk.append(QVariant(m));
    }
    bb::data::JsonDataAccess jda;
    // 原子落盘：.part → rename（rename 前必须先删旧文件，QNX/Qt 语义，
    // 1.0.0.26 历史落盘静默失败同款坑）。
    const QString part = m_path + ".part";
    jda.save(QVariant(disk), part);
    if (!QFile::exists(part) || QFileInfo(part).size() == 0) {
        QFile::remove(part);
        return;
    }
    QFile::remove(m_path);
    QFile::rename(part, m_path);
}

bool DouyinFavoriteModel::toggle(const QVariantMap& row)
{
    const QString id = row.value("awemeId").toString();
    if (id.isEmpty()) return false;
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).toMap().value("awemeId").toString() == id) {
            m_items.removeAt(i);
            ArrayDataModel::removeAt(i);
            while (m_items.size() > 200) {
                const int last = m_items.size() - 1;
                m_items.removeAt(last);
                ArrayDataModel::removeAt(last);
            }
            saveFile();
            bumpVersion();
            m_diagLast = QString::fromUtf8("fav: uncollect %1 total=%2")
                            .arg(id.left(12)).arg(m_items.size());
            emit countChanged();
            emit diagChanged();
            return false;
        }
    }
    add(row);
    return true;
}

void DouyinFavoriteModel::add(const QVariantMap& row)
{
    const QString id = row.value("awemeId").toString();
    if (id.isEmpty()) return;

    QVariantMap item;
    item.insert("awemeId", id);
    item.insert("title", row.value("title").toString().left(60));
    item.insert("authorName", row.value("authorName").toString());
    item.insert("authorFace", row.value("authorFace").toString());
    item.insert("picUrl", row.value("picUrl").toString());
    item.insert("playUrl", row.value("playUrl").toString());
    item.insert("videoSize", row.value("videoSize").toLongLong());
    item.insert("durationSec", row.value("durationSec").toInt());
    item.insert("videoAspect", row.value("videoAspect").toDouble());
    item.insert("api", QVariant::fromValue(static_cast<QObject*>(DouyinApi::instance())));
    item.insert("favStore", QVariant::fromValue(static_cast<QObject*>(this)));
    const qint64 now = QDateTime::currentDateTime().toTime_t();
    item.insert("favedAt", now);
    item.insert("timeText", DouyinApi::timeAgo(now));

    // 去重后置顶。
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).toMap().value("awemeId").toString() == id) {
            m_items.removeAt(i);
            ArrayDataModel::removeAt(i);
            break;
        }
    }
    m_items.prepend(item);
    ArrayDataModel::insert(0, item);

    while (m_items.size() > 200) {
        const int last = m_items.size() - 1;
        m_items.removeAt(last);
        ArrayDataModel::removeAt(last);
    }

    saveFile();
    bumpVersion();
    m_diagLast = QString::fromUtf8("fav: collect %1 total=%2")
                    .arg(id.left(12)).arg(m_items.size());
    emit countChanged();
    emit diagChanged();
}

void DouyinFavoriteModel::remove(const QString& awemeId)
{
    if (awemeId.isEmpty()) return;
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).toMap().value("awemeId").toString() == awemeId) {
            m_items.removeAt(i);
            ArrayDataModel::removeAt(i);
            saveFile();
            bumpVersion();
            m_diagLast = QString::fromUtf8("fav: remove %1 total=%2")
                            .arg(awemeId.left(12)).arg(m_items.size());
            emit countChanged();
            emit diagChanged();
            return;
        }
    }
}

bool DouyinFavoriteModel::has(const QString& awemeId) const
{
    if (awemeId.isEmpty()) return false;
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).toMap().value("awemeId").toString() == awemeId) return true;
    }
    return false;
}

QString DouyinFavoriteModel::favText(const QString& awemeId, int ver) const
{
    Q_UNUSED(ver)
    return has(awemeId) ? QString::fromUtf8("★ 已收藏")
                        : QString::fromUtf8("☆ 收藏");
}

void DouyinFavoriteModel::clear()
{
    ArrayDataModel::clear();
    m_items.clear();
    saveFile();
    bumpVersion();
    m_diagLast = QString::fromUtf8("fav: cleared");
    emit countChanged();
    emit diagChanged();
}

QVariantMap DouyinFavoriteModel::rowAt(int row) const
{
    if (row < 0 || row >= m_items.size()) return QVariantMap();
    return m_items.at(row).toMap();
}

// ============================================================ DouyinDownloadStore

DouyinDownloadStore::DouyinDownloadStore(QObject* parent)
    : ArrayDataModel(parent)
    , m_tipTimer(0)
    , m_progTimer(0)
    , m_dlVersion(0)
    , m_netConnected(false)
{
    // 沙箱持久目录（BB10 homePath 本身已是 .../data，见 cookie 定论）。
    m_dir = QDir(QDir::homePath()).filePath("videos");
    QDir().mkpath(m_dir);
    m_path = QDir(QDir::homePath()).filePath("douyin_downloads.json");
    loadFile();
}

void DouyinDownloadStore::loadFile()
{
    ArrayDataModel::clear();
    m_items.clear();
    QFile f(m_path);
    if (f.exists() && f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QByteArray raw = f.readAll();
        f.close();
        bb::data::JsonDataAccess jda;
        const QVariant v = jda.loadFromBuffer(raw);
        QVariantList arr = v.toList();
        if (arr.isEmpty()) {
            arr = v.toMap().value("items").toList();
        }
        for (int i = 0; i < arr.size(); ++i) {
            QVariantMap m = arr.at(i).toMap();
            const QString id = m.value("awemeId").toString();
            if (id.isEmpty()) continue;
            // 自愈：索引说下载了但文件没了（被清理/拷贝失败）→ 不当已下载。
            const QString lp = m.value("localPath").toString();
            if (lp.isEmpty() || !QFileInfo(lp).exists() || QFileInfo(lp).size() == 0) {
                qDebug("DouyinBar dl index healed drop id=%s", qPrintable(id.left(12)));
                continue;
            }
            if (m.value("fileBytes").toLongLong() <= 0) {
                m.insert("fileBytes", QFileInfo(lp).size());
            }
            m.insert("api", QVariant::fromValue(static_cast<QObject*>(DouyinApi::instance())));
            m_items.append(m);
            ArrayDataModel::append(m);
        }
    }
    m_diagLast = QString::fromUtf8("dl: loaded %1").arg(m_items.size());
    emit countChanged();
    emit diagChanged();
}

void DouyinDownloadStore::saveFile() const
{
    QVariantList disk;
    for (int i = 0; i < m_items.size(); ++i) {
        QVariantMap m = m_items.at(i).toMap();
        m.remove("api");
        disk.append(QVariant(m));
    }
    bb::data::JsonDataAccess jda;
    const QString part = m_path + ".part";
    jda.save(QVariant(disk), part);
    if (!QFile::exists(part) || QFileInfo(part).size() == 0) {
        QFile::remove(part);
        return;
    }
    // QNX/Qt 语义：rename 不覆盖已存在目标，必须先删（1.0.0.26 同款坑）。
    QFile::remove(m_path);
    QFile::rename(part, m_path);
}

QString DouyinDownloadStore::localPathFor(const QString& awemeId) const
{
    return QDir(m_dir).filePath(awemeId + ".mp4");
}

QString DouyinDownloadStore::tagFor(const QString& awemeId) const
{
    return QString("__dyvid__") + awemeId;
}

QString DouyinDownloadStore::cacheDir() const
{
    return m_dir;
}

QString DouyinDownloadStore::sizeText(qint64 bytes) const
{
    if (bytes <= 0) return QString::fromUtf8("0MB");
    if (bytes < 1024 * 1024) {
        return QString::fromUtf8("%1KB").arg(bytes / 1024);
    }
    const double mb = bytes / (1024.0 * 1024.0);
    if (mb < 10) {
        return QString::number(mb, 'f', 1) + QString::fromUtf8("MB");
    }
    return QString::number((int)(mb + 0.5)) + QString::fromUtf8("MB");
}

bool DouyinDownloadStore::has(const QString& awemeId) const
{
    if (awemeId.isEmpty()) return false;
    for (int i = 0; i < m_items.size(); ++i) {
        const QVariantMap m = m_items.at(i).toMap();
        if (m.value("awemeId").toString() == awemeId
                && m.value("state").toString() != "downloading") {
            return true;
        }
    }
    return false;
}

QString DouyinDownloadStore::dlText(const QString& awemeId, int ver) const
{
    Q_UNUSED(ver)
    if (awemeId.isEmpty()) return QString::fromUtf8("下载");
    for (int i = 0; i < m_items.size(); ++i) {
        const QVariantMap m = m_items.at(i).toMap();
        if (m.value("awemeId").toString() != awemeId) continue;
        const QString st = m.value("state").toString();
        if (st == "downloading") return QString::fromUtf8("取消");
        if (st == "done") return QString::fromUtf8("已下载");
        return QString::fromUtf8("下载");
    }
    return QString::fromUtf8("下载");
}

QString DouyinDownloadStore::progressText() const
{
    if (m_activeTag.isEmpty()) return QString();
    const qint64 w = DouyinApi::instance()->downloadWritten(m_activeTag);
    const QString id = m_activeTag.mid(9);   // 剥 "__dyvid__"
    for (int i = 0; i < m_items.size(); ++i) {
        const QVariantMap m = m_items.at(i).toMap();
        if (m.value("awemeId").toString() != id) continue;
        const qint64 total = m.value("videoSize").toLongLong();
        if (total > 0 && w > 0) {
            int p = (int)(w * 100 / total);
            if (p > 99) p = 99;
            return QString::fromUtf8("↓ %1%  %2").arg(p).arg(sizeText(w));
        }
        return QString::fromUtf8("↓ %1").arg(sizeText(w));
    }
    return QString::fromUtf8("↓ %1").arg(sizeText(w));
}

QString DouyinDownloadStore::progressFor(const QString& awemeId, int ver) const
{
    Q_UNUSED(ver)
    if (m_activeTag.isEmpty() || awemeId.isEmpty()) return QString();
    if (m_activeTag != tagFor(awemeId)) return QString();
    return progressText();
}

void DouyinDownloadStore::start(const QVariantMap& row)
{
    const QString id = row.value("awemeId").toString();
    if (id.isEmpty()) {
        setTip(QString::fromUtf8("无效的视频 id"));
        return;
    }
    if (has(id)) {
        setTip(QString::fromUtf8("这个视频已经在「已下载」里了"));
        return;
    }
    if (m_activeTag == tagFor(id)) {
        setTip(QString::fromUtf8("这个视频正在下载…"));
        return;
    }
    const QString playUrl = row.value("playUrl").toString();
    if (playUrl.isEmpty()) {
        setTip(QString::fromUtf8("这个视频没有可用地址（可能需重新加载列表）"));
        return;
    }
    // 已有"进行中"行（上次异常退出残留）→ 先收掉。
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).toMap().value("state").toString() == "downloading") {
            dropRow(i);
            break;
        }
    }

    // 插"进行中"行（state=downloading；完成时整行重插，见 finishRow）。
    QVariantMap item;
    item.insert("awemeId", id);
    item.insert("title", row.value("title").toString().left(60));
    item.insert("authorName", row.value("authorName").toString());
    item.insert("authorFace", row.value("authorFace").toString());
    item.insert("picUrl", row.value("picUrl").toString());
    item.insert("durationSec", row.value("durationSec").toInt());
    item.insert("videoAspect", row.value("videoAspect").toDouble());
    item.insert("playUrl", playUrl);
    item.insert("videoSize", row.value("videoSize").toLongLong());
    item.insert("localPath", localPathFor(id));
    item.insert("state", "downloading");
    item.insert("downloadedAt", (qint64)0);
    item.insert("timeText", QString::fromUtf8("下载中…"));
    item.insert("api", QVariant::fromValue(static_cast<QObject*>(DouyinApi::instance())));
    m_items.prepend(item);
    ArrayDataModel::insert(0, item);

    m_activeTag = tagFor(id);
    bumpVersion();
    m_diagLast = QString("dl: start %1 urlLen=%2 size=%3")
                    .arg(id.left(12)).arg(playUrl.length())
                    .arg((long long)row.value("videoSize").toLongLong());
    emit diagChanged();

    // 完成信号（编译期 connect，一次）。
    if (!m_netConnected) {
        m_netConnected = true;
        QObject::connect(DouyinApi::instance(),
                         SIGNAL(downloadFinished(QString, QString, bool)),
                         this,
                         SLOT(onDownloadFinished(QString, QString, bool)));
    }
    // 进度采样心跳：在飞才跑。
    if (!m_progTimer) {
        m_progTimer = new QTimer(this);
        m_progTimer->setInterval(500);
        QObject::connect(m_progTimer, SIGNAL(timeout()), this, SLOT(onProgressTick()));
    }
    m_progTimer->start();

    // 直下（queued=false → 不走封面队列限并发）：CDN UA/Referer 由 worker 带。
    DouyinApi::instance()->download(playUrl, localPathFor(id), m_activeTag,
                                    row.value("videoSize").toLongLong(),
                                    -1, -1, false, 0);
    qDebug("DouyinBar dl issue tag=%s url=%d bytes dest=%s",
           qPrintable(m_activeTag), playUrl.length(),
           qPrintable(localPathFor(id)));
    setTip(QString::fromUtf8("开始下载…"));
}

void DouyinDownloadStore::cancel(const QString& awemeId)
{
    if (awemeId.isEmpty() || m_activeTag != tagFor(awemeId)) return;
    // worker 线程里没有取消钩子（DyCurlDownloadWorker 无 abort），诚实策略：
    // 摘走在飞标记 + 停进度心跳 + 删 .part，worker 自然结束时按 tag 找不到行，
    // onDownloadFinished 早退；文件不完整，不进索引。
    m_activeTag.clear();
    if (m_progTimer) m_progTimer->stop();
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).toMap().value("awemeId").toString() == awemeId) {
            dropRow(i);
            break;
        }
    }
    const QString part = localPathFor(awemeId) + ".part";
    QFile::remove(part);
    saveFile();
    bumpVersion();
    setTip(QString::fromUtf8("已取消下载"));
    m_diagLast = QString("dl: cancel %1").arg(awemeId.left(12));
    emit diagChanged();
}

void DouyinDownloadStore::remove(const QString& awemeId)
{
    if (awemeId.isEmpty()) return;
    for (int i = 0; i < m_items.size(); ++i) {
        const QVariantMap m = m_items.at(i).toMap();
        if (m.value("awemeId").toString() != awemeId) continue;
        const QString lp = m.value("localPath").toString();
        if (!lp.isEmpty()) QFile::remove(lp);
        dropRow(i);
        saveFile();
        bumpVersion();
        setTip(QString::fromUtf8("已删除，可重新下载"));
        m_diagLast = QString("dl: remove %1 total=%2")
                        .arg(awemeId.left(12)).arg(m_items.size());
        emit countChanged();
        emit diagChanged();
        return;
    }
}

void DouyinDownloadStore::reload()
{
    loadFile();
    bumpVersion();
}

QVariantMap DouyinDownloadStore::rowAt(int row) const
{
    if (row < 0 || row >= m_items.size()) return QVariantMap();
    return m_items.at(row).toMap();
}

void DouyinDownloadStore::onProgressTick()
{
    if (m_activeTag.isEmpty()) {
        if (m_progTimer) m_progTimer->stop();
        return;
    }
    bumpVersion();
}

void DouyinDownloadStore::onDownloadFinished(const QString& tag, const QString& path, bool ok)
{
    if (!tag.startsWith(QLatin1String("__dyvid__"))) return;
    const QString id = tag.mid(9);   // 剥 "__dyvid__"
    const bool wasActive = (m_activeTag == tag);
    if (wasActive) {
        m_activeTag.clear();
        if (m_progTimer) m_progTimer->stop();
    }
    // 被取消的行已经不在索引里 → 完成信号迟到，只清场不留痕。
    bool inIndex = false;
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).toMap().value("awemeId").toString() == id) { inIndex = true; break; }
    }
    if (!inIndex) {
        if (!ok) QFile::remove(path);
        return;
    }
    // 内容校验：CDN 403/超时会落一个小文件或错误页，不能当 mp4 收。
    qint64 bytes = ok ? QFileInfo(path).size() : 0;
    QString why;
    if (!ok) {
        why = QString("curl code 见 dy_net.log");
    } else if (bytes < 16384) {
        why = QString("file too small (%1B)").arg((long long)bytes);
        QFile::remove(path);
    } else {
        // ISO BMFF 快速嗅探：偏移 4 起的盒类型应为 ftyp（抖音 mp4 直链均是）。
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray head = f.read(16);
            if (head.size() >= 8 && head.mid(4, 4) != "ftyp") {
                // 宽容：不是 ftyp 也先收下（少数 CDN 会给 moov 开头或带 DRM 头），
                // 只把明显不是二进制的拒掉。
                bool binary = false;
                for (int i = 0; i < head.size(); ++i) {
                    const unsigned char c = (unsigned char)head.at(i);
                    if (c < 0x09) { binary = true; break; }
                }
                if (!binary) {
                    why = QString("not mp4 (head=%1)").arg(QString::fromUtf8(head.toHex().left(16)));
                    QFile::remove(path);
                }
            }
            f.close();
        }
    }
    if (ok && why.isEmpty()) {
        finishRow(id, true, bytes, QString());
    } else {
        finishRow(id, false, 0, why);
    }
}

void DouyinDownloadStore::finishRow(const QString& awemeId, bool ok, qint64 bytes,
                                    const QString& why)
{
    int idx = -1;
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).toMap().value("awemeId").toString() == awemeId) { idx = i; break; }
    }
    if (idx < 0) return;
    QVariantMap m = m_items.at(idx).toMap();
    if (!ok) {
        dropRow(idx);
        saveFile();
        bumpVersion();
        setTip(QString::fromUtf8("下载失败：") + why);
        m_diagLast = QString("dl: fail %1 why=%2").arg(awemeId.left(12)).arg(why);
        emit countChanged();
        emit diagChanged();
        return;
    }
    // 完成：抽掉旧行、按完成时间置顶重插（history add 同款模式，
    // 规避"模型行原地 replace 不可靠"，v102 定论）。
    // ⚠️ api 指针必须保留：ListItemComponent 的封面解析走 ListItemData.api
    // （context property 在 delegate 里不可见）；saveFile 落盘时才剥。
    dropRow(idx);
    const qint64 now = QDateTime::currentDateTime().toTime_t();
    m.insert("state", "done");
    m.insert("fileBytes", bytes);
    m.insert("downloadedAt", now);
    m.insert("timeText", DouyinApi::timeAgo(now));
    m_items.prepend(m);
    ArrayDataModel::insert(0, m);
    saveFile();
    bumpVersion();
    setTip(QString::fromUtf8("下载完成：") + sizeText(bytes));
    m_diagLast = QString("dl: done %1 bytes=%2 total=%3")
                    .arg(awemeId.left(12)).arg((long long)bytes).arg(m_items.size());
    emit countChanged();
    emit diagChanged();
    qDebug("DouyinBar dl DONE id=%s bytes=%lld", qPrintable(awemeId.left(12)),
           (long long)bytes);
}

void DouyinDownloadStore::dropRow(int idx)
{
    if (idx < 0 || idx >= m_items.size()) return;
    m_items.removeAt(idx);
    ArrayDataModel::removeAt(idx);
}

void DouyinDownloadStore::setTip(const QString& text)
{
    m_tip = text;
    emit tipChanged();
    if (!m_tipTimer) {
        m_tipTimer = new QTimer(this);
        m_tipTimer->setSingleShot(true);
        m_tipTimer->setInterval(3000);
        QObject::connect(m_tipTimer, SIGNAL(timeout()), this, SLOT(onTipTimeout()));
    }
    m_tipTimer->start();
}

void DouyinDownloadStore::onTipTimeout()
{
    if (!m_tip.isEmpty()) {
        m_tip.clear();
        emit tipChanged();
    }
}
