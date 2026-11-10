#include "video_store.hpp"
#include "douyin_api.hpp"
#include "bbplayer_wrapper.hpp"
#include "feed_model.hpp"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QStringList>
#include <QTextStream>
#include <QMutex>
#include <QMutexLocker>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QWaitCondition>
#include <curl/curl.h>
#include <dlfcn.h>
#include <string.h>
#include <bb/data/JsonDataAccess>
#include <bb/cascades/Application>
#include <bb/cascades/ArrayDataModel>
#include <bb/cascades/core/window.h>
// 【v85】分享 = 把视频链接写进系统剪贴板。
// bb::system::Clipboard 定义在 libbbsystem（Makefile 的 LIBS 里已有 -lbbsystem）。
#include <bb/system/Clipboard>

using namespace bb::data;

using namespace bb::data;

/**
 * 评论接口专用 UA。
 * 实测（2026-10-31，curl 复现）：
 *   - 带 "Mozilla/5.0 (BB10; BiliClient) ..." 请求 x/v2/reply/main → code -352（风控拦截）
 *   - 换标准桌面 Chrome UA → code 0，正常返回 20 条
 * 旧接口 x/v2/reply 在当前服务端已对所有客户端返回 replies=null（page.count=0），
 * 所以必须同时换接口 + 换 UA。
 */
static const char* BROWSER_UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/120.0.0.0 Safari/537.36";

QObject* VideoStore::relatedModel() const
{
    return m_relatedModel;
}

VideoStore::VideoStore(QObject* parent)
    : QObject(parent)
    , m_aid(0)
    , m_cid(0)
    , m_loading(false)
    , m_netConnected(false)
    , m_dlExpected(0)
    , m_dlPending(0)
    , m_dlSegmented(false)
    , m_dlLastBytes(0)
    , m_dlLastMs(0)
    , m_dlTimer(0)
    , m_proxy(0)
    , m_streamSize(0)
    , m_usingProxy(false)
    , m_detailRetry(0)
    , m_dlPercent(0)
    , m_commentModel(0)
    , m_commentCount(0)
    , m_relatedModel(0)
    // 【v85】互动状态
    , m_liked(false)
    , m_coinGiven(0)
    , m_favored(false)
    , m_followed(false)
    , m_likeCount(0)
    , m_coinCount(0)
    , m_favCount(0)
    , m_tipTimer(0)
    // 【v89】下载状态
    , m_dlState(0)
    // 【v90】下载任务隔离 / 挂起 / 卡死保护
    , m_dlGen(0)
    , m_dlForPlayback(false)
    , m_dlWanted(false)
    , m_dlProgressMs(0)
    , m_dlBestGot(0)
{
    m_tagPrefix = QString("video_%1").arg((quintptr)this, 0, 16);

    // 【v72】相关推荐模型。FeedModel 构造时会自己 connectNet()，不需要额外接线；
    // kind=Related 让它的 fetch() 走 /x/web-interface/archive/related。
    m_relatedModel = new FeedModel(FeedModel::Related, 0, this);
    m_commentModel = new bb::cascades::ArrayDataModel(this);
    // 【v89】自连 localVideoReady：下载完成有 3 个出口（缓存命中/单连接/分段拼接），
    // 逐个去加"写元数据 + 改状态"很容易漏一个。挂在自己的信号上只写一次。
    // 这是 C++→C++ 的老式 connect，同线程直接调用，可靠（铁律 4 只针对 QML）。
    QObject::connect(this, SIGNAL(localVideoReady(QString)),
                     this, SLOT(onLocalVideoReady(QString)));
    QObject::connect(this, SIGNAL(downloadFailed(QString)),
                     this, SLOT(onDownloadFailedReset(QString)));
    connectNet();
}

void VideoStore::connectNet()
{
    if (m_netConnected) return;
    m_netConnected = true;
    QObject::connect(DouyinApi::instance(), SIGNAL(netReply(QString, QString, bool)),
                     this, SLOT(onNetReply(QString, QString, bool)));
    QObject::connect(DouyinApi::instance(), SIGNAL(downloadFinished(QString, QString, bool)),
                     this, SLOT(onDownloadFinished(QString, QString, bool)));
}


void VideoStore::onNetReply(const QString& tag, const QString& json, bool ok)
{
    if (!tag.startsWith(m_tagPrefix)) return;
    const QString suffix = tag.mid(m_tagPrefix.size());
    if (suffix == ":detail") {
        onDetailReply(json, ok);
    } else if (suffix == ":play") {
        onPlayReply(json, ok, false);
    } else if (suffix == ":play64") {
        onPlayReply(json, ok, true);
    } else if (suffix == ":comment") {
        onCommentReply(json, ok);
    } else if (suffix == ":like" || suffix == ":coin" || suffix == ":favdeal"
               || suffix == ":relation" || suffix == ":favfolders") {
        onInteractReply(suffix, json, ok);
    }
}

void VideoStore::load(const QString& bvid)
{
    m_bvid = bvid;
    // 复位上一部的状态，避免旧视频的数据/错误残留到新视频
    m_detail.clear();      emit detailChanged();
    m_error.clear();       emit errorChanged();
    m_dlPercent = 0;       emit dlPercentChanged();
    m_dlSpeed.clear();     emit dlSpeedChanged();
    if (m_commentModel) m_commentModel->clear();
    // 【v87】换片：上一部的头像回调全部作废。
    // 【v103】m_faceRows 已删除 —— 评论头像不再按行号回填模型，
    // 改由 biliApi.coverVersion 驱动 QML 绑定重算，没有"行号串到新视频"的问题了。
    m_ownerFaceRemote.clear();
    // 【v84】这里原来调的是 ArrayDataModel::clear() —— 只清界面上的行，
    // m_items / m_pendingImg 还留着旧数据。在途的封面下载回调回来时会拿旧索引
    // 去 replace 一个已经空了的模型，轻则无效重则越界。clearFeed() 走完整 reset()。
    if (m_relatedModel) m_relatedModel->clearFeed();
    m_commentCount = 0;    emit commentCountChanged();

    // 【v89】下载状态跟着换片复位：videos/<bvid>.mp4 已经存在就是"已下载"。
    // 判据用 > 1024 字节，和别处一致 —— 下载失败留下的残骸不算。
    {
        QDir home(QDir::homePath());
        QFileInfo df(home.absoluteFilePath("videos/" + m_bvid + ".mp4"));
        const int st = (df.exists() && df.size() > 1024) ? 2 : 0;
        if (st != m_dlState) {
            m_dlState = st;
            emit dlStateChanged();
        }
    }

    // 【v85】互动状态跟着换片一起复位（req_user 回来后会被重新填上）
    m_liked = false;
    m_coinGiven = 0;
    m_favored = false;
    m_followed = false;
    m_ownerMid.clear();
    m_interactTip.clear();
    if (m_tipTimer) m_tipTimer->stop();
    emit interactChanged();

    m_loading = true;
    emit loadingChanged();
    // wbi/view 需要 WBI 签名（与可用的 rcmd 接口同路径前缀）；getSigned 在密钥未就绪时
    // 自动 fallback 为无签名请求，行为兼容旧逻辑；密钥就绪时带 w_rid 可避免服务端 -412。
    QVariantMap params;
    params.insert("bvid", bvid);
    DouyinApi::instance()->getSigned("/x/web-interface/wbi/view", params, m_tagPrefix + ":detail");
}

void VideoStore::onDetailReply(const QString& json, bool ok)
{
    if (!ok) {
        // view 接口偶发网络失败（DNS/连接/SSL 握手），自动重试 2 次后再报错，
        // 避免一次瞬时抖动就导致"播放页加载失败"。
        if (m_detailRetry < 2) {
            m_detailRetry++;
            qDebug("BiliClient view detail failed (retry %d), re-requesting", m_detailRetry);
            QVariantMap params;
            params.insert("bvid", m_bvid);
            DouyinApi::instance()->getSigned("/x/web-interface/wbi/view", params,
                                           m_tagPrefix + ":detail");
            return;
        }
        m_detailRetry = 0;
        finishLoading();
        m_error = "NET FAIL: view (" + DouyinApi::instance()->bizDiag() + ")";
        emit errorChanged();
        emit loadFailed(m_error);
        return;
    }
    m_detailRetry = 0;

    JsonDataAccess jda;
    QVariant root = jda.loadFromBuffer(json.toUtf8());
    QVariantMap rm = root.toMap();
    if (rm.value("code").toInt() != 0) {
        finishLoading();
        m_error = QString("VIDEO NOT FOUND (code=%1)").arg(rm.value("code").toInt());
        emit errorChanged();
        emit loadFailed(m_error);
        return;
    }
    QVariantMap v = rm.value("data").toMap();

    m_aid = v.value("aid").toLongLong();
    m_cid = v.value("cid").toLongLong();

    QVariantMap detail;
    detail.insert("bvid", v.value("bvid"));
    detail.insert("title", v.value("title"));
    // 【v89】封面直链。下载完成后要写进 sidecar，给"已下载"列表显示封面。
    // 尺寸后缀和 FeedModel 保持一致（672 宽），这样如果这个视频在推荐/搜索里
    // 出现过，封面早就缓存好了，不用再下一遍。
    {
        QString pic = v.value("pic").toString();
        if (pic.startsWith("//")) pic = "https:" + pic;
        if (pic.startsWith("http")) {
            const int at = pic.indexOf('@');
            if (at > 0) pic = pic.left(at);
            pic += "@672w_378h_1c.jpg";
        }
        detail.insert("pic", pic);
    }
    detail.insert("desc", v.value("desc").toString().isEmpty() ? QString::fromUtf8("（暂无简介）") : v.value("desc"));

    QVariantMap owner = v.value("owner").toMap();
    detail.insert("ownerName", owner.value("name"));
    // 【v87】头像不能把 https 直链直接给 ImageView —— BB10 的 TLS 握手会永久挂起，
    // 结果就是头像永远空白。改成 curl 落地后喂 file://。
    // 已缓存 → 立刻拿到本地路径；未缓存 → 先留空（显示灰色占位），
    // 下载完成后 onFaceDownloaded 回填并 emit detailChanged 刷新绑定。
    const QString faceUrl = owner.value("face").toString();
    m_ownerFaceRemote = faceUrl;
    detail.insert("ownerFace", resolveFace(faceUrl, m_tagPrefix + ":face"));
    // 【v85】UP 主 mid —— 关注/取关接口（/x/relation/modify）的 fid 参数。
    m_ownerMid = owner.value("mid").toString();

    // 【v85】当前用户对这个视频的互动状态。
    // 登录后 view 接口会带 data.req_user：
    //   { attention: bool（是否已关注）, favorite: bool（是否已收藏）,
    //     like: 0/1（是否已点赞）, coin: 0/1/2（已投币数）, dislike: 0/1 }
    // 未登录时整个字段不存在 → toMap() 得到空 map → 全部 false，正好是"未登录"的表现。
    const QVariantMap reqUser = v.value("req_user").toMap();
    m_followed = reqUser.value("attention").toBool();
    m_favored  = reqUser.value("favorite").toBool();
    m_liked    = reqUser.value("like").toInt() > 0;
    m_coinGiven = reqUser.value("coin").toInt();
    if (m_coinGiven < 0) m_coinGiven = 0;
    if (m_coinGiven > 2) m_coinGiven = 2;
    emit interactChanged();

    QVariantMap stat = v.value("stat").toMap();
    QStringList parts;
    parts << DouyinApi::countText(stat.value("view").toLongLong()) + QString::fromUtf8("播放")
          << DouyinApi::countText(stat.value("danmaku").toLongLong()) + QString::fromUtf8("弹幕")
          << DouyinApi::countText(stat.value("like").toLongLong()) + QString::fromUtf8("点赞")
          << DouyinApi::countText(stat.value("coin").toLongLong()) + QString::fromUtf8("投币")
          << DouyinApi::countText(stat.value("favorite").toLongLong()) + QString::fromUtf8("收藏")
          << DouyinApi::countText(stat.value("share").toLongLong()) + QString::fromUtf8("分享");
    long long pub = v.value("pubdate").toLongLong();
    if (pub > 0) parts << DouyinApi::timeAgo(pub);
    if (!v.value("tname").toString().isEmpty()) parts << v.value("tname").toString();
    detail.insert("statText", parts.join(QString::fromUtf8(" · ")));
    // 【v72】药丸按钮行的数字。statText 是拼好的长串，按钮要单独取。
    detail.insert("likeText", DouyinApi::countText(stat.value("like").toLongLong()));
    detail.insert("coinText", DouyinApi::countText(stat.value("coin").toLongLong()));
    detail.insert("favText",  DouyinApi::countText(stat.value("favorite").toLongLong()));
    detail.insert("shareText", DouyinApi::countText(stat.value("share").toLongLong()));
    // 【v85】保留原始计数，点赞/投币/收藏成功后 ±1 再重算文案。
    m_likeCount = stat.value("like").toLongLong();
    m_coinCount = stat.value("coin").toLongLong();
    m_favCount  = stat.value("favorite").toLongLong();

    // 【v66】视频真实宽高 —— 竖屏视频"左右被拉伸"的根因修复。
    //
    // 原来界面靠 VideoPlayer::videoAspect 算播放区尺寸，但那个属性从头到尾
    // 没有任何地方赋值（只有构造时的 0.0 和换片时的重置 0.0），于是
    // VideoPage::safeAspect() 永远走到兜底分支返回 16:9。9:16 的竖屏视频
    // 就按 16:9 算出 videoW = videoH * 1.7778 = 793du（比 720 的屏宽还宽），
    // ForeignWindowControl 再把屏幕窗口设成 793x446 → 画面被左右拉宽。
    //
    // view 接口的 data.dimension 就是权威来源：
    //   { "width": 1080, "height": 1920, "rotate": 0 }
    // rotate 为 90/270 时表示需要旋转播放（素材本身是横的），此时实际显示
    // 的宽高要对调 —— 不处理会把这类竖屏视频算成横屏。
    QVariantMap dim = v.value("dimension").toMap();
    int dimW = dim.value("width").toInt();
    int dimH = dim.value("height").toInt();
    int rot  = dim.value("rotate").toInt();
    if (((rot / 90) % 2) != 0) { int t = dimW; dimW = dimH; dimH = t; }
    // 接口偶发返回 0（版权/审核中的视频），此时留 0 让 QML 走兜底比例，
    // 绝不能填一个假值 —— 假比例比没有比例更糟（会稳定拉歪）。
    if (dimW < 0) dimW = 0;
    if (dimH < 0) dimH = 0;
    detail.insert("dimW", dimW);
    detail.insert("dimH", dimH);

    // 【v74】视频总时长（**秒**）。这是界面时长/进度条的权威来源 ——
    // libbbplayer 的 duration 字段在流式 HTTP 播放时经常是 0（见 video_store.hpp
    // 成员区注释），没有它整条控制条都不会显示。
    const int durSec = v.value("duration").toInt();
    detail.insert("durationSec", durSec);

    m_detail = detail;
    emit detailReady(detail);
    emit detailChanged();
    // 编译期 connect 到 VideoPlayer::setApiDurationSec（applicationui 里接）。
    if (durSec > 0) emit apiDurationReady(durSec);

    // playurl：fnval=1 返回单文件 mp4（已合流，不需要自己拼音视频轨）。
    // 走 platform=html5 拿 upos-*.bilivideo.com 稳定节点。
    // 必须与 view 一样用 getSigned 带 WBI 签名：否则服务端拒绝 → playUrl 拿不到 →
    // 页面永远停在"正在连接视频…"且无任何提示（这是 v29 后播放页卡死的主因）。
    // 密钥未就绪时 getSigned 自动 fallback 为无签名，行为兼容旧逻辑。
    // qn=32（480P）：BB10 屏幕最宽 720px，480P 已够，再高纯白下载体积。
    // 【v72】相关推荐：与 playurl 并行发出，不阻塞播放。
    // 接口不需要 WBI 签名，FeedModel::Related 走普通 get。
    if (m_relatedModel) {
        m_relatedModel->setRelatedBvid(m_bvid);
        m_relatedModel->load();
    }

    QVariantMap pp;
    pp.insert("bvid", m_bvid);
    pp.insert("cid", QString::number(m_cid));
    pp.insert("qn", "32");
    pp.insert("fnval", "1");
    pp.insert("platform", "html5");
    pp.insert("high_quality", "1");
    DouyinApi::instance()->getSigned("/x/player/playurl", pp, m_tagPrefix + ":play");

    // 评论：必须用 reply/main + 浏览器 UA（见文件顶部 BROWSER_UA 注释）
    DouyinApi::instance()->get(
        "https://api.bilibili.com/x/v2/reply/main?oid=" + QString::number(m_aid) +
        "&type=1&mode=3&next=0",
        m_tagPrefix + ":comment",
        QString::fromLatin1(BROWSER_UA));
}

void VideoStore::onPlayReply(const QString& json, bool ok, bool highQuality)
{
    QString playUrl;
    qint64 playSize = 0;
    int quality = 0;
    int code = -1;
    if (ok) {
        JsonDataAccess jda;
        QVariant root = jda.loadFromBuffer(json.toUtf8());
        QVariantMap rm = root.toMap();
        code = rm.value("code").toInt();
        if (code == 0) {
            QVariantMap dm = rm.value("data").toMap();
            quality = dm.value("quality").toInt();
            QVariantList durls = dm.value("durl").toList();
            if (!durls.isEmpty()) {
                QVariantMap first = durls.first().toMap();
                playUrl = first.value("url").toString();
                playSize = first.value("size").toLongLong();
            }
        }
    }

    // 请求 qn=32 但服务端只给到更低档（quality < 32，说明这部片子没有 480P），
    // 这时改请求 qn=64 —— 宁可文件大一点，也不要被迫看 360P 的糊画面。
    if (!highQuality && !playUrl.isEmpty() && quality > 0 && quality < 32) {
        qDebug("BiliClient playurl qn=32 fell back to quality=%d, retry with qn=64", quality);
        QVariantMap pp2;
        pp2.insert("bvid", m_bvid);
        pp2.insert("cid", QString::number(m_cid));
        pp2.insert("qn", "64");
        pp2.insert("fnval", "1");
        pp2.insert("platform", "html5");
        pp2.insert("high_quality", "1");
        DouyinApi::instance()->getSigned("/x/player/playurl", pp2, m_tagPrefix + ":play64");
        return;   // 等 :play64 回来再 finishLoading
    }

    emit playUrlReady(playUrl);

    // 【v89】已下载的片子直接播本地文件：秒开、不耗流量、离线可用。
    //
    // 这一步必须在 startStream 之前 —— 否则"已下载"板块点进去还是走流式，
    // 离线时直接失败，那就完全对不起"下载"这个功能。
    {
        QDir home(QDir::homePath());
        const QString cached = home.absoluteFilePath("videos/" + m_bvid + ".mp4");
        QFileInfo cf(cached);
        if (cf.exists() && cf.size() > 1024) {
            qDebug("BiliClient play from local download %s", qPrintable(cached));
            // 顺手把 sidecar 补一遍（老版本下载的文件可能没有），
            // 并让 dlState 变成"已下载"。
            m_dlBvid = m_bvid;
            m_dlMeta.clear();
            m_dlMeta.insert("bvid", m_bvid);
            m_dlMeta.insert("title", m_detail.value("title"));
            m_dlMeta.insert("pic", m_detail.value("pic"));
            m_dlMeta.insert("duration", m_detail.value("durationSec"));
            m_dlMeta.insert("owner", m_detail.value("ownerName"));
            emit localVideoReady(cached);
            emit videoReadyToPlay(cached);
            finishLoading();
            return;
        }
    }

    if (playUrl.isEmpty()) {
        // 拿不到直链：playurl 失败（网络/服务端拒绝）或服务端无 durl。
        // 必须明确报错，否则页面永远停在"正在连接视频…"且无任何提示，用户无法判断
        // 是网络问题还是播放器问题。
        QString diag;
        if (!ok) {
            diag = QString("curl ok=false (%1)").arg(DouyinApi::instance()->bizDiag());
        } else {
            diag = QString("code=%1").arg(code);
        }
        m_error = QString::fromUtf8("无法获取播放地址（playurl %1）").arg(diag);
        emit errorChanged();
        emit loadFailed(m_error);
        finishLoading();
        return;
    }

    finishLoading();

    // 真·在线流式播放（不落盘）：BBTube 用自带 libbbplayer.so（OpenMAX AL + 静态 FFmpeg）
    // 直接吃 B 站 CDN 直链，字节由 libbnet.so 逐块喂给解码器，全程不写沙盒。
    // 我们复用同一个库：VideoPlayer 在 play() 里 bbplayer_open_url + bbplayer_play，
    // 并用 bbplayer_net_config 注入 UA/Referer 绕过 CDN 的 403 校验。
    // 不再下载、不再走 loopback 代理——点开即播。
    // 【v90 关键修复】把直链和大小存下来。
    //
    // 之前只有 startStream() 会写这两个成员，而 startStream 现在**没有任何调用方**
    // （onPlayReply 直接把 CDN 直链交给播放器），于是 m_streamUrl 永远是空串 ——
    // 用户点"下载"只会闪一条"视频还没准备好"，看起来就是"点了没反应/没下载成功"。
    m_streamUrl = playUrl;
    m_streamSize = playSize;

    if (!playUrl.isEmpty()) {
        // 用户先点了下载、直链后到 —— 这里补上。
        if (m_dlWanted) {
            m_dlWanted = false;
            downloadVideo(playUrl, playSize, false);
        }
        emit videoReadyToPlay(playUrl);
    }
}

void VideoStore::startStream(const QString& url, qint64 total)
{
    if (!m_proxy) {
        m_proxy = new VideoProxy(this);
        QObject::connect(m_proxy, SIGNAL(streamFailed(QString)),
                         this, SLOT(onStreamFailed(QString)));
    }
    m_streamUrl = url;
    m_streamSize = total;

    const QString local = m_proxy->start(url, total);
    if (local.isEmpty()) {
        qDebug("BiliClient proxy listen failed, fallback to download");
        m_usingProxy = false;
        downloadVideo(url, total, true);
        return;
    }
    m_usingProxy = true;
    qDebug("BiliClient streaming %lld bytes via %s", (long long)total, qPrintable(local));
    emit streamReady(local);
    emit videoReadyToPlay(local);
}

void VideoStore::stopStream()
{
    if (m_proxy) m_proxy->stop();
    m_usingProxy = false;
}

void VideoStore::fallbackToDownload()
{
    if (m_streamUrl.isEmpty()) return;
    stopStream();
    downloadVideo(m_streamUrl, m_streamSize, true);
}

void VideoStore::onStreamFailed(const QString& reason)
{
    qDebug("BiliClient stream failed (%s), fallback to download", qPrintable(reason));
    fallbackToDownload();
}

// 分段数：bilibili 的 CDN 对单连接限速比较明显，切 4 段并发抓通常能快 2-3 倍。
// 小文件（< 6MB）不值得分段，直接单连接。
static const int kSegments = 4;
static const qint64 kSegmentThreshold = 6 * 1024 * 1024;

/* ================= 【v89】下载按钮 + 已下载板块的元数据 ================= */

void VideoStore::downloadCurrent()
{
    if (m_bvid.isEmpty()) return;

    // 已经有完整文件：直接告诉用户，不重复下（几十 MB 不是小数目）。
    if (m_dlState == 2) {
        setTip(QString::fromUtf8("已经下载过了"));
        return;
    }
    if (m_dlState == 1 || m_dlPending > 0) {
        setTip(QString::fromUtf8("正在下载中…"));
        return;
    }
    // 【v90】直链还没到手（playurl 还在路上）就**挂起**，等 onPlayReply 回来自动开工。
    // 原来这里直接 return 并提示"先点一下播放" —— 用户看到的就是"点了没反应"。
    if (m_streamUrl.isEmpty()) {
        m_dlWanted = true;
        setTip(QString::fromUtf8("正在获取地址，马上开始下载…"));
        return;
    }
    m_dlWanted = false;
    setTip(QString::fromUtf8("开始下载…"));
    downloadVideo(m_streamUrl, m_streamSize, false);
}

void VideoStore::onLocalVideoReady(const QString& path)
{
    Q_UNUSED(path);
    // 只有"当前正在看的这部"下载完成才改按钮状态。用户中途切走了就别动 ——
    // 那部新视频的按钮应该还是"下载"。
    if (!m_dlBvid.isEmpty() && m_dlBvid == m_bvid) {
        if (m_dlState != 2) {
            m_dlState = 2;
            emit dlStateChanged();
        }
    }
    writeDownloadMeta();
}

void VideoStore::onDownloadFailedReset(const QString& reason)
{
    Q_UNUSED(reason);
    // 失败时不能停在"下载中" —— 那个状态没有任何东西会再把它推出去，
    // 按钮会永远显示"下载中 42%"。
    if (m_dlState == 1) {
        m_dlState = 0;
        emit dlStateChanged();
    }
}

/**
 * 写 videos/<bvid>.json（sidecar）。
 *
 * "已下载"板块是纯文件系统扫描出来的，文件名只有 BV 号 —— 没有这份元数据，
 * 列表里就只能显示一串 BV 号，没有标题也没有封面。
 *
 * 【只认 m_dlMeta 快照】不能读 m_bvid / m_detail：下载要跑几十秒，用户完全可能
 * 中途切到别的视频。快照在 downloadVideo 开工时拍好，全程不变量。
 *
 * 封面这边顺带补一刀：视频详情页本身不显示封面，所以从播放页下载的视频，
 * 封面通常还没进过缓存。这里入队下一张，等用户切到"已下载"时就已经在了。
 */
void VideoStore::writeDownloadMeta()
{
    const QString bvid = m_dlMeta.value("bvid").toString();
    if (bvid.isEmpty()) return;

    QDir home(QDir::homePath());
    if (!home.exists("videos")) home.mkpath("videos");

    const QString pic = m_dlMeta.value("pic").toString();
    if (!pic.isEmpty() && pic.startsWith("http")) {
        // 【v93】缓存路径统一走 DouyinApi::coverCachePath —— 和列表页是同一个文件。
        // 以前这里自己拼一遍名字（不带 @672w 后缀），于是列表页已经缓存好的封面
        // 在"已下载"里查不到，还要重新下一张，存成两份。
        const QString local = DouyinApi::coverCachePath(pic);
        QFileInfo cf(local);
        if (!local.isEmpty() && !(cf.exists() && cf.size() > 1024)) {
            if (cf.exists()) QFile::remove(local);
            // tag 用 :dlcover —— onDownloadFinished 里不认这个 tag 就直接忽略，
            // 不需要额外的回调处理（落盘就算成功）。
            DouyinApi::instance()->download(DouyinApi::coverUrl(pic), local,
                                          m_tagPrefix + ":dlcover", -1, -1, -1, true);
        }
    }

    QVariantMap meta = m_dlMeta;
    // 【v93】时间戳用 int 而不是 qint64。
    // BB10 的 JsonDataAccess 对 QMetaType::LongLong 的支持存疑 —— 一旦它序列化
    // 失败，save() 是静默的（返回 void），整个 sidecar 就写不出来，
    // 于是"已下载"列表里所有视频都只剩 BV 号，没有标题。
    // 秒级时间戳用 int 到 2038 年才溢出，够用。
    meta.insert("ts", (int)QDateTime::currentDateTime().toTime_t());

    JsonDataAccess jda;
    const QString path = home.absoluteFilePath("videos/" + bvid + ".json");
    // 【注意】BB10 的 JsonDataAccess::save() 返回 void，没有成功/失败标志
    // （现代 Qt 的 QJsonDocument 才有）。所以这里不做返回值判断。
    jda.save(meta, path);

    // 【v93】写完立刻读回验证 —— 因为 save() 失败是完全静默的。
    // 读不回来就退化成纯文本 sidecar（.title），保证标题不会丢。
    // 读的时候 DownloadStore 优先用 json，json 不成立才回退这个文本。
    bool jsonOk = false;
    QFile vf(path);
    if (vf.open(QIODevice::ReadOnly)) {
        const QByteArray raw = vf.readAll();
        vf.close();
        if (!raw.isEmpty()) {
            const QVariantMap back = JsonDataAccess().loadFromBuffer(raw).toMap();
            jsonOk = !back.value("title").toString().isEmpty();
        }
    }
    const QString txtPath = home.absoluteFilePath("videos/" + bvid + ".title");
    if (jsonOk) {
        // json 正常就别留文本副本，避免两份数据以后不一致
        QFile::remove(txtPath);
    } else {
        QFile tf(txtPath);
        if (tf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QTextStream ts(&tf);
            ts.setCodec("UTF-8");
            // 一行一个字段，顺序固定：title / owner / pic / duration
            ts << m_dlMeta.value("title").toString() << "\n"
               << m_dlMeta.value("owner").toString() << "\n"
               << m_dlMeta.value("pic").toString() << "\n"
               << m_dlMeta.value("duration").toString() << "\n";
            tf.close();
        }
        qDebug() << "BerryLi sidecar json unusable, wrote text fallback:" << txtPath;
    }
}

void VideoStore::downloadVideo(const QString& url, qint64 expectedBytes, bool forPlayback)
{
    if (url.isEmpty()) {
        emit downloadFailed("EMPTY URL");
        return;
    }
    if (m_dlPending > 0) {
        // 上一个下载还在跑：直接放弃（同一时刻只允许一个播放缓存）
        setTip(QString::fromUtf8("上一个下载还没结束"));
        return;
    }

    // 【v90】新任务开一个序号：tag 里带上它，上一次的残留回调会被自动过滤掉。
    m_dlGen++;
    m_dlForPlayback = forPlayback;
    m_dlProgressMs = QDateTime::currentMSecsSinceEpoch();
    m_dlBestGot = 0;

    // 【v89】拍快照：下载期间用户可能切到别的视频，回调不能再用 m_bvid/m_detail。
    m_dlBvid = m_bvid;
    m_dlMeta.clear();
    m_dlMeta.insert("bvid", m_bvid);
    m_dlMeta.insert("title", m_detail.value("title"));
    m_dlMeta.insert("pic", m_detail.value("pic"));
    m_dlMeta.insert("duration", m_detail.value("durationSec"));
    m_dlMeta.insert("owner", m_detail.value("ownerName"));

    QDir home(QDir::homePath());
    if (!home.exists("videos")) {
        home.mkpath("videos");
    }
    const QString dest = home.absoluteFilePath("videos/" + m_bvid + ".mp4");

    // 已经下过且大小吻合：跳过下载
    QFileInfo fi(dest);
    if (expectedBytes > 0 && fi.exists() && fi.size() == expectedBytes) {
        emit downloadProgress(100);
        emit localVideoReady(dest);
        return;
    }

    m_dlUrl = url;
    m_dlFinal = dest;
    m_dlExpected = expectedBytes;
    m_dlLastBytes = 0;
    m_dlLastMs = QDateTime::currentMSecsSinceEpoch();
    emit downloadProgress(0);

    if (!m_dlTimer) {
        m_dlTimer = new QTimer(this);
        m_dlTimer->setInterval(400);
        QObject::connect(m_dlTimer, SIGNAL(timeout()), this, SLOT(onDownloadTick()));
    }
    m_dlTimer->start();

    // 【v89】进入"下载中"，药丸按钮的文案跟着变。
    if (m_dlState != 1) {
        m_dlState = 1;
        emit dlStateChanged();
    }

    if (expectedBytes >= kSegmentThreshold) {
        startSegmented(url, dest, expectedBytes);
    } else {
        startSingle(url, dest, expectedBytes);
    }
}

void VideoStore::startSegmented(const QString& url, const QString& finalPath, qint64 total)
{
    m_dlSegmented = true;
    m_dlTags.clear();
    m_dlParts.clear();
    m_dlPending = kSegments;

    const qint64 chunk = total / kSegments;
    for (int i = 0; i < kSegments; ++i) {
        const qint64 s = i * chunk;
        const qint64 e = (i == kSegments - 1) ? (total - 1) : (s + chunk - 1);
        const QString part = finalPath + QString(".part%1").arg(i);
        // 【v90】tag 带任务序号：分段失败回退单连接时，旧的 :dl0 和新的 :dl0
        // 曾经同名，旧线程晚到的回调会被当成新任务的，直接把 .part0 当成品交付。
        const QString tag = m_tagPrefix + QString(":dl%1_%2").arg(m_dlGen).arg(i);
        m_dlTags << tag;
        m_dlParts << part;
        DouyinApi::instance()->download(url, part, tag, e - s + 1, s, e);
    }
    qDebug("BiliClient download segmented into %d parts, total=%lld", kSegments, (long long)total);
}

void VideoStore::startSingle(const QString& url, const QString& finalPath, qint64 total)
{
    m_dlSegmented = false;
    m_dlTags.clear();
    m_dlParts.clear();
    m_dlPending = 1;
    const QString tag = m_tagPrefix + QString(":dl%1_0").arg(m_dlGen);
    m_dlTags << tag;
    m_dlParts << finalPath;
    DouyinApi::instance()->download(url, finalPath, tag, total);
}

void VideoStore::onDownloadTick()
{
    if (m_dlPending <= 0) return;

    qint64 got = 0;
    for (int i = 0; i < m_dlTags.size(); ++i) {
        got += DouyinApi::instance()->downloadWritten(m_dlTags.at(i));
    }

    // 【v90】卡死保护。
    //
    // 分段下载时 downloadWritten 读的是 worker 的实时字节数；某个 worker 完成并
    // deleteLater 之后 m_dlJobs 里就没有它了，读出来是 0 —— 所以进度会**变小**。
    // 因此这里只认历史最大值，不直接比较 got。
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (got > m_dlBestGot) {
        m_dlBestGot = got;
        m_dlProgressMs = nowMs;
    } else if (m_dlProgressMs > 0 && nowMs - m_dlProgressMs > 90000) {
        // 90 秒一个字节都没涨：BB10 的 TLS 握手挂起时线程会一直不动，
        // 没有这个保护按钮就会永远停在"下载中 x%"，用户只能杀进程。
        qDebug("BiliClient download stalled, abort");
        m_dlPending = 0;
        if (m_dlTimer) m_dlTimer->stop();
        for (int i = 0; i < m_dlParts.size(); ++i) QFile::remove(m_dlParts.at(i));
        QFile::remove(m_dlFinal);
        m_error = QString::fromUtf8("下载超时（网络卡住了）");
        emit errorChanged();
        setTip(m_error);
        emit downloadFailed(m_error);
        return;
    }

    // 【v90】不知道总大小时给 -1，QML 显示"下载中"而不是一直"下载中 0%"。
    int percent = -1;
    if (m_dlExpected > 0) {
        percent = (int)(m_dlBestGot * 100 / m_dlExpected);
        if (percent > 99) percent = 99;
    }
    m_dlPercent = percent;
    emit downloadProgress(percent);
    emit dlPercentChanged();

    // 速度 + 剩余时间
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 dt = now - m_dlLastMs;
    if (dt >= 300 && got > m_dlLastBytes) {
        const double bps = (double)(got - m_dlLastBytes) * 1000.0 / (double)dt;
        QString speed;
        if (bps > 1024.0 * 1024.0) {
            speed = QString::number(bps / (1024.0 * 1024.0), 'f', 1) + " MB/s";
        } else {
            speed = QString::number(bps / 1024.0, 'f', 0) + " KB/s";
        }
        if (bps > 1.0 && m_dlExpected > got) {
            const int sec = (int)((m_dlExpected - got) / bps);
            speed += QString::fromUtf8(" · 剩 ") + QString::number(sec) + "s";
        }
        m_dlLastBytes = got;
        m_dlLastMs = now;
        m_dlSpeed = speed;
        emit downloadSpeed(speed);
        emit dlSpeedChanged();
    }
}

void VideoStore::onDownloadFinished(const QString& tag, const QString& path, bool ok)
{
    // 【v87】头像下载走的是同一个 downloadFinished 信号，但和视频分段下载完全无关。
    // 必须在下面那条 `m_dlTags.contains(tag)` 的守卫之前拦下来，
    // 否则头像回调会被当成"不认识的分段"直接丢弃。
    if (tag.startsWith(m_tagPrefix + ":face") || tag.startsWith(m_tagPrefix + ":cface")) {
        onFaceDownloaded(tag, path, ok);
        return;
    }

    if (m_dlPending <= 0 || !m_dlTags.contains(tag)) return;

    if (!ok) {
        // 分段模式下任何一段失败 → 丢掉所有分段，退回单连接整段下载重试一次
        if (m_dlSegmented) {
            qDebug("BiliClient segmented download failed on %s, fallback to single",
                   qPrintable(tag));
            for (int i = 0; i < m_dlParts.size(); ++i) {
                QFile::remove(m_dlParts.at(i));
            }
            m_dlPending = 0;
            startSingle(m_dlUrl, m_dlFinal, m_dlExpected);
            return;
        }
        m_dlPending = 0;
        if (m_dlTimer) m_dlTimer->stop();
        m_error = QString::fromUtf8("下载失败（网络或 CDN 拒绝）");
        emit errorChanged();
        // 【v90】必须给用户一句可见的话。原来只 emit downloadFailed，界面什么都不显示，
        // 用户看到的就是按钮从"下载中"悄悄变回"下载" —— 也就是"没下载成功"。
        setTip(m_error);
        emit downloadFailed(m_error);
        return;
    }

    m_dlPending--;
    if (m_dlPending > 0) return;

    if (m_dlTimer) m_dlTimer->stop();

    if (!m_dlSegmented) {
        QFileInfo fi(path);
        if (!fi.exists() || fi.size() < 1024) {
            emit downloadFailed("下载文件无效");
            return;
        }
        emit downloadProgress(100);
        emit localVideoReady(path);
        // 【v90】用户主动点下载时不要把正在播的画面重开（会黑一下）。
        // 只有"流式失败 → 下载 → 播"这条降级路径才需要切。
        if (m_dlForPlayback) emit videoReadyToPlay(path);
        return;
    }

    finishSegmented();
}

void VideoStore::finishSegmented()
{
    // 按顺序拼接分段。用 256KB 缓冲流式拷贝，避免 40MB 一次性读进内存。
    QFile out(m_dlFinal);
    if (!out.open(QIODevice::WriteOnly)) {
        emit downloadFailed("无法写入缓存文件");
        return;
    }
    char buf[256 * 1024];
    for (int i = 0; i < m_dlParts.size(); ++i) {
        QFile in(m_dlParts.at(i));
        if (!in.open(QIODevice::ReadOnly)) {
            out.close();
            QFile::remove(m_dlFinal);
            emit downloadFailed("分段文件缺失");
            return;
        }
        while (!in.atEnd()) {
            const qint64 n = in.read(buf, sizeof(buf));
            if (n <= 0) break;
            if (out.write(buf, n) != n) {
                in.close();
                out.close();
                QFile::remove(m_dlFinal);
                emit downloadFailed("拼接缓存失败");
                return;
            }
        }
        in.close();
        QFile::remove(m_dlParts.at(i));
    }
    out.close();

    QFileInfo fi(m_dlFinal);
    if (!fi.exists() || fi.size() < 1024) {
        emit downloadFailed("缓存文件无效");
        return;
    }
    qDebug("BiliClient download done, size=%lld", (long long)fi.size());
    emit downloadProgress(100);
    emit localVideoReady(m_dlFinal);
    if (m_dlForPlayback) emit videoReadyToPlay(m_dlFinal);
}

void VideoStore::abortDownload(const QString& reason)
{
    m_dlPending = 0;
    if (m_dlTimer) m_dlTimer->stop();
    setTip(reason);
    emit downloadFailed(reason);
}

/* ============================================================================
 * 【v87】头像缓存
 *
 * 【为什么必须有这一层】UP 主头像和评论头像原来是把接口返回的 https 直链
 * **直接**塞给 `ImageView.imageSource`。而 BB10 的 QtNetwork/QSslSocket 做
 * HTTPS 握手会永久挂起（本工程整个网络层因此才换成 libcurl）——
 * 结果就是头像永远空白，一个都不显示。视频封面早在 v78 就改成 curl 落地了，
 * 头像这条路当时漏掉了。
 *
 * 现在统一：curl 下到沙盒 → 换成本地 file:// 路径喂 ImageView。
 * ==========================================================================*/

QString VideoStore::faceCachePath(const QString& url)
{
    // 【v103】算法搬到 DouyinApi::faceCachePath —— QML 绑定里要用同一个函数算路径
    // （DouyinApi::faceUrlFor），两处各写一份必然有一天会不一致，那就是"头像永远空白"。
    return DouyinApi::faceCachePath(url);
}

QString VideoStore::resolveFace(const QString& rawUrl, const QString& tag)
{
    QString url = rawUrl;
    if (url.startsWith("//")) url = "https:" + url;
    if (url.isEmpty() || !url.startsWith("http")) return QString();

    // 【刻意不加 @宽w_高h_1c 后缀】封面那边必须加（原图几百 KB），但头像本来
    // 就只有 5~15KB，加 CDN 图片处理参数换不来多少体积，反而多一个"参数不被
    // 支持就 404"的失败点 —— 一次失败就是一整页头像全空。保持原样最稳。
    const QString local = faceCachePath(url);
    QFileInfo fi(local);
    // 512 字节：一个 120x120 的 jpeg 不可能更小。下载失败留下的残骸会被判无效。
    //
    // 【v103】光看大小不够 —— 还必须过 isUsableImage（头+尾双标记）。
    // 半截文件（连接被掐断）大小早就超过 512，只看大小会把它当有效缓存**永远命中**：
    // 头像永远显示不出来，也永远不会重下。这跟 v98 封面那个坑是同一个。
    if (fi.exists() && fi.size() > 512 && DouyinApi::isUsableImage(local, 512)) {
        return "file://" + local;
    }
    if (fi.exists()) QFile::remove(local);   // 残骸先删，否则永远命中不了也永远重不下

    // queued = true：进下载队列（最多 3 并发）。一页评论 20 个头像，
    // 全并发就是 20 次 TLS 握手，BB10 握不完（和 v84 封面那次是同一个坑）。
    // 【v90】prio=1：头像排在视频封面后面。否则 20 个头像会把相关推荐的 8 张封面
    // 堵在队尾几十秒，用户看到的就是"封面不显示"。
    DouyinApi::instance()->download(url, local, tag, -1, -1, -1, true, 1);
    return QString();
}

void VideoStore::onFaceDownloaded(const QString& tag, const QString& path, bool ok)
{
    if (!ok) {
        QFile::remove(path);   // 删残骸，下次还能重下
        qDebug("BiliClient face download failed tag=%s", qPrintable(tag));
        return;
    }
    QFileInfo fi(path);
    if (!fi.exists() || fi.size() <= 512) {
        QFile::remove(path);
        return;
    }

    if (tag == m_tagPrefix + ":face") {
        // UP 主头像：detail 是 QVariantMap 属性，改完 emit 一次 detailChanged
        // 就能让 QML 的绑定重新求值（铁律 4：只靠属性绑定，不靠信号直连）。
        m_detail.insert("ownerFace", "file://" + path);
        emit detailChanged();
        return;
    }

    // 评论头像：【v103】这里**什么都不用做**。
    // DouyinApi::onDownloadFinished 已经校验过这张图、登记进就绪表并 bump 了
    // coverVersion，QML 的 faceUrlFor 绑定会自己重算 —— 不需要碰模型。
    //
    // 原来这里是 `m_commentModel->replace(row, item)`（按行号回填 file://），
    // 在 BB10 上不重绘，等于白写；而且要维护"行号 ↔ tag"的映射，
    // 评论一刷新就串行。整条路都不要了。
}

void VideoStore::onCommentReply(const QString& json, bool ok)
{
    QVariantList comments;
    if (ok) {
        JsonDataAccess jda;
        QVariant root = jda.loadFromBuffer(json.toUtf8());
        QVariantMap rm = root.toMap();
        if (rm.value("code").toInt() == 0) {
            QVariantMap data = rm.value("data").toMap();
            QVariantList replies = data.value("replies").toList();
            for (int i = 0; i < replies.size(); ++i) {
                QVariantMap r = replies.at(i).toMap();
                QVariantMap item;
                QVariantMap member = r.value("member").toMap();
                item.insert("name", member.value("uname"));
                // 【v87】评论头像同样不能直连 https（见 resolveFace 的说明）。
                // 【v103】模型里只存**远端 URL（永不变化）**，不存 file:// ——
                // 原来下完之后用 m_commentModel->replace(row, item) 回填本地路径，
                // 和 v102 封面那次是同一个坑：BB10 上「模型行变化 → ListView 重绘」
                // 不可靠，首次进视频页评论头像永远不出现，必须退出重进才看得到。
                // 现在 QML 用 biliApi.faceUrlFor(url, biliApi.coverVersion) 现场解析，
                // 每张头像下完 bump 一次 coverVersion，绑定自己重算。
                {
                    const QString ftag = m_tagPrefix + QString(":cface%1").arg(i);
                    const QString avatar = member.value("avatar").toString();
                    resolveFace(avatar, ftag);   // 只为触发下载/命中缓存，返回值不用
                    item.insert("face", avatar);
                }
                // ListItemComponent 是独立组件作用域，看不到 biliApi 这个 context
                // property（ReferenceError），必须由 C++ 把指针塞进每一行。
                item.insert("api", QVariant::fromValue(static_cast<QObject*>(DouyinApi::instance())));
                item.insert("like", r.value("like").toLongLong());
                item.insert("time", DouyinApi::timeAgo(r.value("ctime").toLongLong()));
                item.insert("message", r.value("content").toMap().value("message"));
                comments.append(item);
            }
        }
    }
    if (m_commentModel) {
        m_commentModel->clear();
        for (int i = 0; i < comments.size(); ++i) {
            m_commentModel->append(comments.at(i));
        }
    }
    m_commentCount = comments.size();
    emit commentCountChanged();
    emit commentsReady(comments);
}


void VideoStore::finishLoading()
{
    m_loading = false;
    emit loadingChanged();
}

// ============================================================================
// VideoPlayer —— C++ 版播放器（见 video_store.hpp 里的说明）
// ============================================================================

VideoPlayer::VideoPlayer(QObject* parent)
    : QObject(parent)
    , m_player(0)
    , m_initialized(false)
    , m_windowId("biliVideoSurface")
    , m_state("idle")
    // 【1.0.0.49】循环播放默认开（用户要求的行为，QML 同款默认值，两处同步）
    , m_loopOn(true)
    // 【1.0.0.50】重播静默窗默认关（初始化列表紧随 m_loopOn，与声明序一致，
    // 不再踩 -Wreorder）
    , m_replayQuiet(false)
    , m_aspect(0.0)
    , m_position(0)
    , m_duration(0)
    , m_speed(1.0)
    , m_playing(false)
    // 【v85】控件层默认可见（刚进播放页要能看到进度条），播放后由 showControls() 起 4s 定时器
    , m_ctrlShown(true)
    , m_hideTimer(0)
    // 【v86】长按扫动：默认空闲
    , m_scanDir(0)
    , m_scanBaseMs(0)
    , m_scanTimer(0)
    , m_pressTimer(0)
    , m_pressX(0.0)
    , m_pressW(0.0)
    , m_pressHeld(false)
    , m_pressFired(false)
    // 【v74】时长/进度三保险（见 hpp 成员区注释）
    , m_apiDurMs(0)
    , m_ticksPerMs(0)
    , m_clockBaseMs(0)
    , m_clockOn(false)
    , m_rawPos(0)
    , m_rawDur(0)
    , m_rawState(-1)
    , m_pollSeq(0)
    , m_poll(0)
    , m_httpFallbackTried(false)
    , m_probeStamp(0)
    // 【1.0.0.49】初始化列表按声明序摆放（m_ended/m_eofLikely/m_lastEmitSec），
    // 修 -Wreorder 存量告警（三成员互不依赖，换序零行为变化）。
    , m_ended(false)
    , m_eofLikely(false)
    // 【1.0.0.52】伪 ENDED 护栏初始态：还没开播过，基线 -1、标记已消毒
    // （没播过就不存在"旧结尾值残留"可言；首次 play() 会重新埋真基线）。
    , m_openBasePos(-1)
    , m_posSanitized(true)
    , m_lastEmitSec(-1)
{
    // 每个播放器实例用唯一 windowId（镜像 BBTube PlayerController.newWindowId() 的做法）。
    // libbbplayer 把该字符串直接作为屏幕窗口的 SCREEN_PROPERTY_ID_STRING，ForeignWindowControl
    // 按它认领；用唯一 ID 可避免任何"上一个窗口残留/同 ID 冲突"导致的认领失败。
    m_windowId = QString("biliSurface_%1").arg((quintptr)this, 0, 16);

    // 窗口组 = 应用主窗口组。libbbbplayer 在 set_window 后会 screen_join_window_group 进这个组，
    // ForeignWindowControl 必须认领同一组。构造期 mainWindow() 已就绪，这里直接算好作为 CONSTANT
    // 暴露给 QML，FWC 会显式把它设成 windowGroup，保证两侧组字符串【完全一致】。
    bb::cascades::Application* app = bb::cascades::Application::instance();
    bb::cascades::Window* win = app ? app->mainWindow() : 0;
    if (win) m_groupId = win->groupId();
}

VideoPlayer::~VideoPlayer()
{
    // 【v86】定时器是 this 的子对象，析构会跟着删；但先停掉，
    // 避免析构过程中还在触发 onScanTick 去碰已经要销毁的 m_player。
    if (m_scanTimer) m_scanTimer->stop();
    if (m_pressTimer) m_pressTimer->stop();
    if (m_hideTimer) m_hideTimer->stop();
    stopPoll();
    if (m_player) { m_player->destroy(); delete m_player; }
}

void VideoPlayer::preparePlayer()
{
    // 提前建窗：在 FWC 加入场景前让 libbbplayer 的屏幕窗口先存在，FWC 才能认领到。
    ensurePlayer();
}

// ============================================================================
// v61：屏幕窗口探针 —— 直接问 libscreen「本进程里到底有哪些窗口」
//
// 【为什么加这个】
// v57~v60 连续四轮，QML 侧 ForeignWindowControl 的 windowId / windowGroup 与
// videoPlayer.windowId / groupId 在调试叠加层里**逐字符完全一致**，boundToWindow
// 依旧恒为 false。QML 侧能试的组合已经试尽：
//   - 显式 windowGroup（v60）        → fwc=N
//   - 不写 windowGroup 走默认组（v59）→ fwc=N
//   - windowId 空串 → 真实 ID 跳变（v57）→ fwc=N
//   - remove/add 重挂 24 次（v58/v59）   → fwc=N
//   - bindWindow 反复翻转（v58）        → fwc=N
// 继续盲试没有任何信息增量。必须换到"事实层"，问三个问题：
//   1) libbbplayer 到底有没有把屏幕窗口建出来？
//   2) 建出来的窗口 ID_STRING 是不是我们传的那个字符串？
//   3) 它 join 的是哪个窗口组？是不是 mainWindow()->groupId()？
// 这三问一旦有答案，fwc=N 的根因就唯一确定了：
//   - 没有窗口 / ID 不对   → 问题在 libbbplayer 的建窗流程（set_window 的 id 没被用上）
//   - 有窗口但组不同       → 问题在组字符串（libbbplayer 用了别的组，或根本没 join 组）
//   - 有窗口、ID 和组都对  → 问题在 FWC 本身（下一轮用自建窗口做二分）
//
// 【实现要点】
//   - 全部走 dlopen("libscreen.so") + dlsym，**不改 Makefile 的 LIBS**。
//     BB10 共享库允许保留未解析符号：新增 -l 缺失时编译零告警、链接成功，但设备上
//     dlopen 解析失败、应用直接打不开（项目铁律第 2 条）。dlopen 方式零风险。
//   - 常量取自 /d/bbndk/target_10_1_0_1020/qnx6/usr/include/screen/screen.h，
//     已逐个核对（见下面 enum 注释里的行号对应）。
//   - 应用上下文（SCREEN_APPLICATION_CONTEXT）是每进程唯一的，所以本进程内
//     libbbplayer 建的窗口一定能被枚举到；别的进程的窗口则看不到，正是我们要的。
//   - 1 秒节流：dbgText() 每轮轮询都会调它，不能每次都真去问 screen。
// ============================================================================
QString VideoPlayer::screenProbe()
{
    // ---- screen.h 常量（行号 = 头文件中的定义位置）----
    enum {
        CTX_APP          = 0,     // L169  SCREEN_APPLICATION_CONTEXT
        P_GROUP          = 18,    // L577  SCREEN_PROPERTY_GROUP              (char[])
        P_ID_STRING      = 20,    // L605  SCREEN_PROPERTY_ID_STRING          (char[])
        P_SIZE           = 40,    // L855  SCREEN_PROPERTY_SIZE               (int[2])
        P_VISIBLE        = 51,    // L1039 SCREEN_PROPERTY_VISIBLE            (int)
        P_RBUF_COUNT     = 53,    // L1079 SCREEN_PROPERTY_RENDER_BUFFER_COUNT(int)
        P_ZORDER         = 54,    // L1091 SCREEN_PROPERTY_ZORDER             (int)
        P_GROUP_COUNT    = 104,   // L1664 SCREEN_PROPERTY_GROUP_COUNT        (int)
        P_GROUPS         = 105,   // L1674 SCREEN_PROPERTY_GROUPS             (handle[])
        P_WINDOW_COUNT   = 108,   // L1696 SCREEN_PROPERTY_WINDOW_COUNT       (int)
        P_WINDOWS        = 109    // L1706 SCREEN_PROPERTY_WINDOWS            (handle[])
    };

    // 节流：1 秒内复用上次结果（dbgText 每轮轮询都会调用本方法）
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!m_probeCache.isEmpty() && (now - m_probeStamp) < 1000) {
        return m_probeCache;
    }
    m_probeStamp = now;

    void* lib = dlopen("libscreen.so", RTLD_LAZY);
    if (!lib) lib = dlopen("libscreen.so.1", RTLD_LAZY);
    if (!lib) {
        m_probeCache = QString("probe: dlopen libscreen.so 失败");
        return m_probeCache;
    }

    typedef int (*PFN_create_ctx) (void**, int);
    typedef int (*PFN_destroy_ctx)(void*);
    typedef int (*PFN_get_ctx_iv) (void*, int, int*);
    typedef int (*PFN_get_ctx_pv) (void*, int, void**);
    typedef int (*PFN_get_win_cv) (void*, int, int, char*);
    typedef int (*PFN_get_win_iv) (void*, int, int*);
    typedef int (*PFN_get_grp_cv) (void*, int, int, char*);

    PFN_create_ctx  createCtx  = (PFN_create_ctx) dlsym(lib, "screen_create_context");
    PFN_destroy_ctx destroyCtx = (PFN_destroy_ctx)dlsym(lib, "screen_destroy_context");
    PFN_get_ctx_iv  ctxIv      = (PFN_get_ctx_iv) dlsym(lib, "screen_get_context_property_iv");
    PFN_get_ctx_pv  ctxPv      = (PFN_get_ctx_pv) dlsym(lib, "screen_get_context_property_pv");
    PFN_get_win_cv  winCv      = (PFN_get_win_cv) dlsym(lib, "screen_get_window_property_cv");
    PFN_get_win_iv  winIv      = (PFN_get_win_iv) dlsym(lib, "screen_get_window_property_iv");
    PFN_get_grp_cv  grpCv      = (PFN_get_grp_cv) dlsym(lib, "screen_get_group_property_cv");

    if (!createCtx || !ctxIv || !ctxPv || !winCv || !winIv) {
        m_probeCache = QString("probe: libscreen 符号缺失");
        return m_probeCache;
    }

    void* ctx = 0;
    if (createCtx(&ctx, CTX_APP) != 0 || !ctx) {
        m_probeCache = QString("probe: screen_create_context 失败");
        return m_probeCache;
    }

    QStringList out;
    out << QString("probe 我方 wid=%1").arg(m_windowId);
    out << QString("probe 我方 gid=%1").arg(m_groupId.isEmpty() ? "(空!)" : m_groupId);

    // ---- 1) 枚举所有窗口组（确认我们传进去的组到底存不存在）----
    int gc = 0;
    if (ctxIv(ctx, P_GROUP_COUNT, &gc) == 0 && gc > 0 && gc <= 16) {
        void* gv[16];
        if (ctxPv(ctx, P_GROUPS, gv) == 0) {
            for (int i = 0; i < gc; ++i) {
                char gbuf[256];
                ::memset(gbuf, 0, sizeof(gbuf));
                if (grpCv && grpCv(gv[i], P_GROUP, (int)sizeof(gbuf) - 1, gbuf) == 0) {
                    const QString gn = QString::fromUtf8(gbuf);
                    out << QString("  grp%1=%2%3")
                           .arg(i).arg(gn)
                           .arg(gn == m_groupId ? QString(" <== 就是我方组") : QString());
                }
            }
        }
    } else {
        out << QString("  (窗口组枚举失败 gc=%1)").arg(gc);
    }

    // ---- 2) 枚举所有窗口 ----
    int n = 0;
    if (ctxIv(ctx, P_WINDOW_COUNT, &n) != 0) {
        if (destroyCtx) destroyCtx(ctx);
        m_probeCache = out.join("\n") + "\nprobe: 取 WINDOW_COUNT 失败";
        return m_probeCache;
    }
    out << QString("probe 窗口数=%1").arg(n);
    if (n > 0 && n <= 16) {
        void* wv[16];
        if (ctxPv(ctx, P_WINDOWS, wv) == 0) {
            for (int i = 0; i < n; ++i) {
                char idb[256];
                char grb[256];
                ::memset(idb, 0, sizeof(idb));
                ::memset(grb, 0, sizeof(grb));
                winCv(wv[i], P_ID_STRING, (int)sizeof(idb) - 1, idb);
                winCv(wv[i], P_GROUP,     (int)sizeof(grb) - 1, grb);

                int vis = -1, z = -1, rb = -1, sz[2] = { -1, -1 };
                winIv(wv[i], P_VISIBLE,    &vis);
                winIv(wv[i], P_ZORDER,     &z);
                winIv(wv[i], P_RBUF_COUNT, &rb);
                winIv(wv[i], P_SIZE,       sz);

                const QString ids = QString::fromUtf8(idb);
                const QString grs = grb[0] ? QString::fromUtf8(grb) : QString("(空)");
                QString hit;
                if (ids == m_windowId) hit += " <==ID匹配";
                if (grb[0] && QString::fromUtf8(grb) == m_groupId) hit += " <==组匹配";

                out << QString("  W%1 id=%2 grp=%3 vis=%4 z=%5 rb=%6 %7x%8%9")
                       .arg(i)
                       .arg(ids.isEmpty() ? QString("(空)") : ids)
                       .arg(grs)
                       .arg(vis).arg(z).arg(rb).arg(sz[0]).arg(sz[1])
                       .arg(hit);
            }
        }
    } else if (n > 16) {
        out << QString("  (窗口过多，跳过明细)");
    } else if (n == 0) {
        // 应用主窗口一定存在，所以 0 个窗口只可能是"探针拿到的是空上下文"，
        // 不能据此判定 libbbplayer 没建窗口 —— 否则会得出错误结论。
        out << QString("  (0 窗口 —— 应用界面正常则说明探针上下文不共享，本行不可信)");
    }

    if (destroyCtx) destroyCtx(ctx);
    m_probeCache = out.join("\n");
    return m_probeCache;
}

QString VideoPlayer::eventLog() const
{
    if (!m_player) return QString();
    return m_player->eventLog();
}

QString VideoPlayer::playerError() const
{
    if (!m_player) return QString();
    return m_player->lastError();
}

int VideoPlayer::cbState() const
{
    if (!m_player) return -1;
    return m_player->callbackState();
}

int VideoPlayer::eventCount() const
{
    if (!m_player) return 0;
    return m_player->eventCount();
}

QString VideoPlayer::caStatus() const
{
    if (!m_player) return QString();
    return m_player->caStatus();
}

void VideoPlayer::ensurePlayer()
{
    // 已建好句柄+窗口则直接返回（避免每次 play 都重建）。
    if (m_player && m_initialized) return;

    if (!m_player) {
        m_player = new BbPlayer();
        if (!m_player->loaded()) {
            qDebug("BiliClient BbPlayer 未能加载 libbbplayer.so");
            return;
        }
    }
    // 【v63】不再在这里 prepareCa()。
    // 原来这一行会解析 121 张包内证书 + 125 张系统证书，然后 create() 里又解析一遍
    // （create 还会把次数上限重置），openUrl 里第三遍 —— 三遍全在 nav.push() 之前，
    // 用户看到的就是"点进播放页要等好几秒，连转场都没有"。
    // 播放默认走 http（见 play() 的 scheme 降级），这条路不做 TLS，根证书根本用不到；
    // 现在 CA 改成 openUrl 遇到 https 时才按需装载，且装成功一次就不再重复。
    m_player->setNetConfig(QString::fromLatin1(BROWSER_UA),
                           QString::fromLatin1("https://www.bilibili.com"));

    if (!m_player->create()) {
        qDebug("BiliClient bbplayer_create 失败");
        return;
    }
    // create 默认不开启视频输出，必须显式置 1，否则只有声音没有画面。
    m_player->setVideoEnabled(true);
    // 拿到应用主窗口组，让 libbbplayer 把视频窗口 join 到同一组，
    // 这样 QML 的 ForeignWindowControl（显式 windowGroup = 同一组）才能认领到画面。
    // m_groupId 在构造期已算好（mainWindow()->groupId()），这里直接用。
    //
    // 【v62 重大修正】参数顺序是 (group, windowId) —— 组在前、ID 在后。
    // v43~v61 一直传成 (windowId, group)，后果是：
    //   窗口 ID_STRING 被设成主窗口组名，同时去 join 一个叫 "biliSurface_xxx" 的
    //   不存在的组 → join 失败 → 库内部 screen_destroy_window 把窗口销毁
    //   → 屏幕窗口从来没建出来过 → ForeignWindowControl 永远 boundToWindow=false。
    // 反汇编证据链见 bbplayer_wrapper.hpp 里 setWindow 的注释。
    m_player->setWindow(m_groupId, m_windowId);
    m_initialized = true;
}

void VideoPlayer::play(const QString& url)
{
    if (url.isEmpty()) return;
    ensurePlayer();
    if (!m_player || !m_player->loaded()) {
        setState("player-unavailable");
        return;
    }

    // 换片先清掉上一部片的状态，避免短暂沿用旧比例/旧进度。
    m_aspect = 0.0;
    emit videoAspectChanged();
    m_position = 0;
    m_duration = 0;
    m_lastEmitSec = -1;      // v64：换片后允许立刻刷新一次时间标签
    // 【1.0.0.24】新的一轮播放：收起"播完"标志与 EOF 证据。
    m_ended = false;
    m_eofLikely = false;
    if (m_ended) emit endedChanged();
    emit positionChanged();
    emit durationChanged();
    // 【v56 关键】把 https 降级成 http 再交给播放器。
    //
    // 为什么必须这么做（v55 真机实证）：
    //   libbbnet 静态链入的 curl 找不到可用的 CA，https 直链 100% 失败；
    //   v55 的自动回退是先 close() 再 openUrl(http) —— 而 bbplayer_close() 会把
    //   preparePlayer() 提前建好的屏幕窗口销毁掉。BB10 的 ForeignWindowControl
    //   在加入场景时只扫一次匹配 windowId 的窗口，之后不会回头重扫（v44 已踩过），
    //   于是窗口没了但音频/解码器还活着 → 有声音、没画面、fwc=N。
    //
    // 改成在第一次 openUrl 之前就把 scheme 换掉，窗口只建一次、FWC 只认领一次，
    // 不会有"销毁-重建-不重扫"的问题。B 站 CDN 双协议都支持，v55 已实证
    // http 下 30fps 解码、29.6 帧实际送出、时间正常推进。
    //
    // 想强制走 https（等 CA 问题解决后）时设环境变量 BILI_FORCE_HTTPS=1 即可，
    // 那时会退回 v55 的 close+重开逻辑，需要同时处理窗口重建。
    QString playUrl = url;
    const bool forceHttps = !QByteArray(qgetenv("BILI_FORCE_HTTPS")).isEmpty();
    if (!forceHttps && playUrl.startsWith("https://")) {
        playUrl.replace(0, 8, "http://");
        qDebug("BiliClient scheme 降级: %s", playUrl.toUtf8().constData());
    }

    // v55：记住本次实际播放的 url（降级后的），并复位回退标志。
    m_lastUrl = playUrl;
    m_httpFallbackTried = false;
    m_lastErrShown.clear();   // 换片后允许重新展示/处理新错误
    setState("opening");
    playTrace(QString("PLAY urlLen=%1").arg(playUrl.size()));
    // v48：立刻开始轮询（原来只在成功路径末尾启动）。openUrl 失败或库在拉流阶段
    // 异步报错时，轮询也必须活着，否则回调事件/错误原文永远刷不到界面上。
    startPoll();

    // 【1.0.0.52】开播基线（伪 ENDED 护栏，见 hpp m_openBasePos 长注释）：
    // openUrl 之后的一小段时间里，positionTicks() 读到的可能还是**上一轮结尾
    // 的残留值**（1.0.0.51 实锤：open 后 282ms 读到旧 total=22822 → 误判
    // ENDED → 二次 REPLAY → EOF 态 openUrl → 管线 0.3x 降速）。
    // 这里在 open **之前**拍下原始 position 作基线：
    //   - 句柄刚建（首次播放）：基线 0 → 无残留可清，立刻算"已消毒"；
    //   - 句柄复用（EOF 重播/换片）：基线 = 旧结尾值 → 必须真的观察到位置
    //     跌破基线（新流从头起跑），ENDED 判定才解锁。
    // m_openClock 另开 2.5s 硬保护窗（onPoll 里配合）。
    m_openBasePos = m_player ? m_player->positionTicks() : 0;
    m_posSanitized = (m_openBasePos <= 0);
    m_openClock.start();

    // 直接把 CDN 直链交给 libbbplayer 流式打开（不落盘）。
    // 【v89】本地文件（已下载）必须走 openFile —— open_url 是给网络流用的，
    // 沙盒绝对路径交给它打不开。
    const bool isLocal = !playUrl.startsWith(QLatin1String("http"), Qt::CaseInsensitive);
    const bool opened = isLocal ? m_player->openFile(playUrl) : m_player->openUrl(playUrl);
    if (!opened) {
        setState("open-fail");
        return;
    }
    if (!m_player->play()) {
        setState("play-fail");
        return;
    }
    // 【v74】墙钟起点：单位自校准和"播放器 position 恒 0"的兜底都要用它。
    m_clockBaseMs = 0;
    m_clock.start();
    m_clockOn = true;
    m_playing = true;
    // 【1.0.0.50】replay() 静默窗内不抖通知（防循环重播图标闪断）。
    if (!m_replayQuiet) emit playingChanged();
    setState("playing");
}

void VideoPlayer::pause()
{
    if (!m_player) return;
    m_player->pause();
    // 墙钟冻结在当前位置（恢复播放时从这儿接着走，不会把暂停的时间也算进去）
    m_clockBaseMs = m_position;
    m_clockOn = false;
    m_playing = false;
    emit playingChanged();
    setState("paused");
    stopPoll();
    // 【v85】暂停时把控件层亮出来 —— 否则用户看不到"继续播放"的按钮。
    showControls();
}

void VideoPlayer::stop()
{
    if (!m_player) return;
    // 【v86】换片/退出播放页时必须停掉扫动，否则定时器会在新视频上继续跳。
    stopScan();
    stopPoll();
    // 【1.0.0.52】stop() 不再 close() —— 屏幕窗口终身只建一次。
    //
    // 根因链（反汇编 + 三代真机实证）：
    //   1) bbplayer_create() 只建句柄不建窗口；窗口是 openUrl/openFile 时才建的；
    //   2) bbplayer_close() 销毁屏幕窗口；
    //   3) BB10 的 ForeignWindowControl 加入场景后，对"窗口被销毁再以同一
    //      windowId 重建"没有重新认领能力（v44/v55/1.0.0.50 三次踩坑同因）。
    // 后果：stop() 里 close() → ensurePlayer() 重建窗口 → FWC 永远
    // boundToWindow=false → 换片/循环重播只剩封面、有声无画。
    // 1.0.0.50 实证：循环 REPLAY 后前台截图视频区 0.00% 帧差分（静态封面），
    // 而 dyplay.log 里 ENDED/REPLAY/pos=38000 照走（解码链活着）。
    //
    // 正确姿势（v56 的 tryHttpFallback 已实证）：窗口不动、句柄不动，
    // 直接在原窗口上 openUrl 换流 —— v55 播放中换流实测 30fps 正常。
    // pause() 负责立刻停声（换片期间旧片不许再响），窗口留着由 mask 遮住。
    // 退路：真需要拆窗的终态路径（应用退出）走 releasePlayer()。
    if (m_initialized) m_player->pause();
    // 注意：m_initialized **保持 true**。下一次 play() 的 ensurePlayer() 会
    // 直接复用既有句柄与窗口，openUrl 在其上换流。
    m_playing = false;
    m_position = 0;
    m_duration = 0;
    // 【v74】换片必须清掉上一部视频的 API 时长，否则新视频会先显示旧的总时长。
    // m_ticksPerMs 保留 —— tick 单位是库的属性，不会随视频变。
    m_apiDurMs = 0;
    m_clockBaseMs = 0;
    m_clockOn = false;
    m_lastEmitSec = -1;
    emit positionChanged();
    emit durationChanged();
    // 【1.0.0.50】replay() 静默窗内不抖通知（stop 的 playing=false 由静默窗
    // 关闭后的最终值补发覆盖；非重播路径的换片/退出仍然立即通知，语义不变）。
    if (!m_replayQuiet) emit playingChanged();
    setState("stopped");
    playTrace("STOP");
}

void VideoPlayer::releasePlayer()
{
    // 【1.0.0.52】终态拆窗：close() 销毁屏幕窗口 + 复位 m_initialized。
    // 只有播放面从场景里消失的路径才允许调用（应用退出 cleanup）。
    // 拆窗之后若还要播，ensurePlayer() 会以同一 windowId 重建窗口 ——
    // FWC 对重建窗口的认领已证死（v44/v55/1.0.0.50），正常流程不走这条路。
    if (!m_player) return;
    stopScan();
    stopPoll();
    m_player->close();
    m_initialized = false;
    m_playing = false;
    m_position = 0;
    m_duration = 0;
    m_apiDurMs = 0;
    m_clockBaseMs = 0;
    m_clockOn = false;
    m_lastEmitSec = -1;
    emit positionChanged();
    emit durationChanged();
    emit playingChanged();
    setState("released");
    playTrace("RELEASE");
}

void VideoPlayer::togglePlay()
{
    if (!m_player) return;
    if (m_playing) {
        pause();
    } else {
        if (!m_player->play()) {
            setState("play-fail");
            return;
        }
        // 从暂停处恢复：墙钟基准接在当前进度上
        m_clockBaseMs = m_position;
        m_clock.start();
        m_clockOn = true;
        m_playing = true;
        emit playingChanged();
        setState("playing");
        startPoll();
        // 【v85】开始播放后 4 秒自动隐藏控件层（showControls 里按 m_playing 判断）
        showControls();
    }
}

void VideoPlayer::seekBy(int deltaMs)
{
    if (!m_player) return;
    seekTo(m_position + deltaMs);
}

/**
 * 【v85】绝对拖动。
 *
 * 原来只有 seekBy（相对拖动），QML 那边用 `scrubMs - positionMs` 算差值。
 * 问题在于 positionMs 在手指抬起那一刻可能刚被 250ms 的轮询改过，
 * 差值就算偏了 —— 表现是"拖到 50% 却跳到了别的位置"。
 * 现在直接给目标毫秒数，不再有中间量。
 */
void VideoPlayer::seekTo(int ms)
{
    if (!m_player) return;
    const int total = durationMs();
    if (total <= 0) return;

    if (ms < 0) ms = 0;
    if (ms > total) ms = total;

    // 单位跟着自校准结果走；还没校准成功才退回 wrapper 的默认猜测值（微秒）。
    const qint64 tpm = (m_ticksPerMs > 0) ? m_ticksPerMs : BbPlayer::TICKS_PER_MS;
    const qint64 ticks = (qint64)ms * tpm;
    if (!m_player->seek(ticks, 0)) {
        qDebug("BiliClient seek(%d ms) failed", ms);
        return;
    }

    // 立刻把界面刷到目标位置，别等下一次轮询。
    m_position = ms;
    // 墙钟基准必须一起跳，否则"兜底进度"会瞬间弹回 seek 之前的位置。
    m_clockBaseMs = ms;
    if (m_clockOn) m_clock.restart();
    // 【v85】开保护窗口：接下来 1.5 秒内 onPoll 不采信播放器回报的位置。
    // 解码器重开期间 positionTicks() 还是旧值，照常写回就是用户说的"回弹"。
    m_seekClock.start();
    m_lastEmitSec = -1;   // 强制刷新一次（否则同秒内不 emit）
    emit positionChanged();
}

/* ============================================================================
 * 【v86】长按左/右半屏 → 3 倍速后退/快进
 *
 * 【为什么不是真倍速】
 * 把 libbbplayer.so 的导出符号表整列出来看过，只有：
 *   create / destroy / set_window / net_config / set_video_enabled /
 *   open_url / open_file / open_segments / open_source / play / pause /
 *   close / seek / position / duration / state / tags_* / waveform / version
 * 没有任何 rate/speed 入口；字符串表里 PlaybackRate / XAPlaybackRateItf 也都是 0 次。
 * 所以"改播放速率"这条路在后端层面就不存在。
 *
 * 【退而求其次：连续 seek】
 * 按住期间每 600ms 跳一次，目标位置按
 *     base + dir * elapsed * 3
 * 实时算出来 —— 600ms 一跳正好是 1.8 秒，等效 3 倍速。
 *
 * 【为什么是"实时算"而不是"每次加 1.8 秒"】
 * 每次 seek 都有几十毫秒的执行误差，累加十几次就偏出好几秒，按住久了明显跑飞。
 * 固定基准点 + 墙钟求值，误差恒在最后一次 tick 之内，不会滚雪球。
 *
 * 观感：画面一段段往前/往后跳，不是平滑 3 倍速。这是该后端能做到的最接近效果。
 * ==========================================================================*/

/* ---------- 【v86】长按判定：按下 → 500ms → 判左/右 → 起扫 ---------- */

void VideoPlayer::pressAt(double x, double w)
{
    m_pressX = x;
    m_pressW = w;
    m_pressHeld = true;
    m_pressFired = false;

    if (!m_pressTimer) {
        m_pressTimer = new QTimer(this);
        m_pressTimer->setSingleShot(true);
        // 500ms 是各家播放器的通用长按阈值：比它短会和"点一下"打架，
        // 比它长用户会觉得没反应。
        m_pressTimer->setInterval(500);
        QObject::connect(m_pressTimer, SIGNAL(timeout()), this, SLOT(onPressHold()));
    }
    m_pressTimer->start();
}

void VideoPlayer::cancelPress()
{
    m_pressHeld = false;
    m_pressFired = false;
    if (m_pressTimer) m_pressTimer->stop();
    stopScan();
}

bool VideoPlayer::releasePress()
{
    const bool wasLong = m_pressFired;
    m_pressHeld = false;
    m_pressFired = false;
    if (m_pressTimer) m_pressTimer->stop();
    stopScan();
    return wasLong;
}

void VideoPlayer::onPressHold()
{
    if (!m_pressHeld) return;
    if (m_pressW <= 0.0) return;          // 布局还没算完，判不了左右
    // 右半屏 → 快进；左半屏 → 后退。正中算右（用户按住中间时更可能是想快进）。
    const int dir = (m_pressX >= m_pressW / 2.0) ? 1 : -1;
    startScan(dir);
    if (m_scanDir != 0) m_pressFired = true;
}

void VideoPlayer::startScan(int dir)
{
    if (dir == 0) { stopScan(); return; }
    if (!m_player) return;
    const int total = durationMs();
    if (total <= 0) return;            // 还不知道总时长，没得扫

    dir = (dir > 0) ? 1 : -1;

    // 同方向重复按下（QML 那边 Move 事件很密集）直接忽略，
    // 否则每次 Move 都会重置基准点，扫动就永远走不动。
    if (m_scanDir == dir && m_scanTimer && m_scanTimer->isActive()) return;

    m_scanDir = dir;
    m_scanBaseMs = m_position;
    if (m_scanBaseMs < 0) m_scanBaseMs = 0;
    if (m_scanBaseMs > total) m_scanBaseMs = total;
    m_scanClock.start();

    if (!m_scanTimer) {
        m_scanTimer = new QTimer(this);
        m_scanTimer->setInterval(600);
        QObject::connect(m_scanTimer, SIGNAL(timeout()), this, SLOT(onScanTick()));
    }
    m_scanTimer->start();

    // 扫动期间控件必须亮着（用户要看到进度在走），并且抑制自动隐藏 ——
    // 直接调 showControls() 不行，它会在 4 秒后把控件收掉。
    if (m_hideTimer) m_hideTimer->stop();
    if (!m_ctrlShown) {
        m_ctrlShown = true;
        emit ctrlShownChanged();
    }

    emit scanningChanged();
    // 不在这一刻立刻跳：此时 elapsed≈0，等于 seek 到原地，白白重开一次解码器。
    // 首次真正跳落在 600ms 后；QML 那边同时会亮出"3× 快进"提示，反馈不延迟。
}

void VideoPlayer::stopScan()
{
    if (m_scanTimer) m_scanTimer->stop();
    if (m_scanDir == 0) return;
    m_scanDir = 0;
    emit scanningChanged();
    // 手指抬起后把控件亮几秒，让用户看清停在哪儿（暂停状态下会一直留着）。
    showControls();
}

void VideoPlayer::onScanTick()
{
    if (m_scanDir == 0) return;

    const int total = durationMs();
    if (total <= 0) { stopScan(); return; }

    const int target = m_scanBaseMs + m_scanDir * (int)(m_scanClock.elapsed() * 3);

    // 到边界就停：继续 seek 只会让解码器在片头/片尾反复重开。
    if (target <= 0) {
        seekTo(0);
        stopScan();
        return;
    }
    if (target >= total - 200) {
        // 不 seek 到 total 整：正好落在片尾容易让后端直接进结束态。
        seekTo(total - 200);
        stopScan();
        return;
    }
    seekTo(target);
}

void VideoPlayer::toggleControls()
{
    if (m_ctrlShown) {
        hideControls();
    } else {
        showControls();
    }
}

void VideoPlayer::showControls()
{
    if (!m_ctrlShown) {
        m_ctrlShown = true;
        emit ctrlShownChanged();
    }
    if (!m_hideTimer) {
        m_hideTimer = new QTimer(this);
        m_hideTimer->setSingleShot(true);
        QObject::connect(m_hideTimer, SIGNAL(timeout()), this, SLOT(hideControls()));
    }
    m_hideTimer->stop();
    // 只有正在播的时候才自动隐藏。暂停时把控件留在屏幕上，
    // 否则用户看不到"继续播放"的按钮。
    if (m_playing) m_hideTimer->start(4000);
}

void VideoPlayer::hideControls()
{
    if (m_hideTimer) m_hideTimer->stop();
    if (m_ctrlShown) {
        m_ctrlShown = false;
        emit ctrlShownChanged();
    }
}

void VideoPlayer::setSpeed(double speed)
{
    // 限制在合理范围内（UI 仍是 0.5/1/1.5/2/3 五档）。
    if (speed < 0.5) speed = 0.5;
    if (speed > 3.0) speed = 3.0;
    // 【v74 结论】libbbplayer 确实**没有**倍速入口，这不是"还没接"，是"库里不存在"：
    // 导出符号表只有 create/destroy/set_window/net_config/set_video_enabled/
    // open_url/open_file/open_segments/open_source/play/pause/close/seek/
    // position/duration/state/tags_*/waveform/version，没有任何 rate/speed；
    // 字符串表里也没有 PlaybackRate / XAPlaybackRateItf 的痕迹，说明它虽然链了
    // libOpenMAXAL，却从没去取那个接口。所以要真倍速只能改播放后端（换成
    // bb::multimedia::MediaPlayer + 本地代理注入 UA/Referer），那是另一个量级的改动。
    //
    // 这里保留属性与 UI，行为上如实记录（不再假装"设置成功"），等后端方案定了再接。
    if (m_speed != speed) {
        m_speed = speed;
        emit speedChanged();
    }
}

void VideoPlayer::setApiDurationSec(int sec)
{
    if (sec <= 0) return;
    const int ms = sec * 1000;
    if (ms == m_apiDurMs) return;
    m_apiDurMs = ms;
    // 进度条/时间标签的 visible 都挂在 durationMs 上，必须立刻通知。
    emit durationChanged();
}

/**
 * 【1.0.0.24】循环播放：播完自动从头再来。
 *
 * 为什么不直接 openUrl(m_lastUrl) 了事：
 *  本片已播到 EOF，解码器句柄还开着但流已耗尽，openUrl 重开的语义未在真机验证；
 *  而 stop()+play() 是每次上滑换片都在走的序列（拆窗口→ensurePlayer 重建→FWC
 *  重新认领），行为已知。代价是要重拉一次 totalSize 的字节 —— 本地代理按 Range
 *  服务，播放器从 0 开始请求，天然就是"从头播"。
 *
 * stop() 会清掉 m_apiDurMs（避免沿用旧片时长），所以这里先取出来、play 后补回，
 * 否则重播期间进度条总时长会短暂掉成 0。
 *
 * 【1.0.0.52 再修正】以上两段已被真机证伪，现行实现（v52）：
 *  - 不再 stop()+play()：stop() 不再拆窗（见 stop() 长注释），且 1.0.0.50
 *    实证 stop()+play() 的"拆窗→重建"路径 FWC 认领死亡 → 有声无画；
 *  - 也不直接 openUrl：EOF 态句柄上 openUrl 会进入解码降速态
 *    （1.0.0.51 实锤：0.29-0.4x，22.8s 的片 56-100s 才播完）。
 *  正解 = 媒体源/窗口/网络连接全部不动，只把解码位置拨回 0：
 *      pause() → seek(0, 0) → play()
 *  这等价于库内的 loop 语义（本库无 loop 接口，符号表全列见 v86 注释），
 *  seek 失败才回退 openUrl 旧路径（保留兜底，好过完全不播）。
 */
void VideoPlayer::replay()
{
    if (m_lastUrl.isEmpty()) {
        setState("error: 循环播放缺少地址");
        return;
    }
    const int apiDurSec = m_apiDurMs / 1000;
    qDebug("BiliClient replay url=%s apiDur=%d eof=%d", m_lastUrl.toUtf8().constData(),
           apiDurSec, m_eofLikely ? 1 : 0);
    playTrace(QString("REPLAY urlLen=%1 apiDur=%2 eof=%3")
              .arg(m_lastUrl.size()).arg(apiDurSec).arg(m_eofLikely ? 1 : 0));
    // 【1.0.0.50】重播静默窗：ended 期间 playing 保持 true，NOTIFY 不向 QML
    // 抖出。旧时序下 ended 与 replay 各自 emit，真机每轮循环播放钮都要
    // 闪一次"播放"图标（swipe log 的 ICON play→ICON pause 成对）。
    // 成员照常维护（pause/togglePlay 语义不变），只是 NOTIFY 延到窗关闭后
    // 按最终值补发一次 —— 通常与 ended 前一致，QML 无感。
    //
    // 【1.0.0.51】循环重播**不再调用 stop()**（修复"循环播放有声音没画面"）。
    // 根因（1.0.0.50 真机一小时实证）：stop() 内部的 bbplayer_close() 会销毁
    // libbbplayer 的屏幕窗口，而 FWC 对"销毁再以同一 windowId 重建"的窗口
    // 没有重新认领能力（v44/v55/1.0.0.50 三次同因实证）→ 重建出的窗口
    // 永远 boundToWindow=false → 播放页只剩封面图、有声无画。
    // 【1.0.0.52】再递进：光"不拆窗"还不够 —— 1.0.0.51 只去掉 stop() 后，
    // replay 走 openUrl 在 **EOF 态句柄**上重启媒体源，解码管线进入
    // 0.29-0.4x 降速态（22.8s 的片 56-100s 才播完，dyplay.log 实证）。
    // 所以 v52 主路径改成 seek(0) 拨回位置、媒体源原样不动（见函数头注释）。
    m_replayQuiet = true;

    // 【1.0.0.52】伪 ENDED 护栏埋基线（与 play() 同一套，见 hpp 长注释）：
    // EOF 瞬间原始 position 就是旧结尾值， seek(0)/openUrl 之后必须观察到位置
    // 跌破它才恢复 ENDED 判定 —— 否则"旧值 pin 住 → 误判 ENDED → 再 replay"
    // 的双重链会再次把管线拖进降速态。
    m_openBasePos = m_player ? m_player->positionTicks() : 0;
    m_posSanitized = (m_openBasePos <= 0);
    m_openClock.start();

    // 先 pause：EOF 态播放器可能还在吐尾帧/保持 play 状态，
    // 不停住就 seek 有概率被忽略（且回退路径的 openUrl 前也应处于静止态）。
    if (m_player && m_player->loaded()) m_player->pause();

    // 主路径：seek(0) 把解码位置拨回 0，媒体源/网络流/窗口全部不动。
    // 单位用自校准结果（本片刚播完，m_ticksPerMs 必然已定；万一没有才退回
    // wrapper 默认猜测值 —— 目标 0 在任何单位下都是 0，不受单位争议影响）。
    bool seeked = false;
    if (m_player && m_player->loaded()) {
        seeked = m_player->seek(0, 0);
    }
    if (seeked) {
        // 位置/状态复位（不 openUrl，m_duration/m_apiDurMs/m_ticksPerMs 全部
        // 保留：同一个媒体源还在，时长不会变；m_eofLikely 也保留 —— 字节早已
        // 全部送达，仍是硬事实，第二轮的 ENDED 判定还需要它）。
        m_position = 0;
        m_ended = false;
        if (m_ended) emit endedChanged();
        emit positionChanged();
        m_clockBaseMs = 0;
        m_clock.start();
        m_clockOn = true;
        // m_playing 本就还是 true（ended 期间保持，见 onPoll ENDED 分支注释）；
        // pause() 之后这里重新 play() 恢复解码输出。
        if (!m_player->play()) {
            setState("play-fail");
            m_replayQuiet = false;
            emit playingChanged();
            return;
        }
        startPoll();   // 幂等：poll 活着时是 no-op；stall-watch 触发时确保轮询恢复
        if (apiDurSec > 0) setApiDurationSec(apiDurSec);
        setState("playing");
        playTrace("REPLAY seek0 ok");
    } else {
        // 兜底：EOF 态句柄不接受 seek（库行为未知，保留旧路径）。
        // 已知代价：openUrl 重启媒体源有 0.3x 降速风险 —— 但好过完全不播。
        // play() 会重新埋开播基线（覆盖上面那份）。
        qDebug("BiliClient replay seek(0) failed -> fallback openUrl");
        playTrace("REPLAY seek0 fail -> openUrl fallback");
        play(m_lastUrl);
        if (apiDurSec > 0) setApiDurationSec(apiDurSec);
    }
    m_replayQuiet = false;
    emit playingChanged();
}

/** 【1.0.0.49】循环播放开关 setter。QML 按钮与播放器两侧同步的唯一写入口。 */
void VideoPlayer::setLoopOn(bool on)
{
    if (m_loopOn == on) return;
    m_loopOn = on;
    emit loopOnChanged();
}

/** 【1.0.0.24】代理 EOF 事实到达（DouyinPlayerStore 转发）。 */
void VideoPlayer::setEofLikely()
{
    m_eofLikely = true;
}

int VideoPlayer::durationMs() const
{
    // 播放器自己算出来的优先（它来自容器真实解析，能反映"实际可播长度"）。
    if (m_duration > 0) return m_duration;
    // 播放器给不出（流式 HTTP 常见）：用 view 接口的权威秒数兜底。
    if (m_apiDurMs > 0) return m_apiDurMs;
    return 0;
}

int VideoPlayer::positionMs() const
{
    // m_position 已在 onPoll() 里统一换算过（含 tick 单位自校准与墙钟兜底），
    // 这里不再二次加工，避免两条换算路径漂移。
    return m_position;
}

void VideoPlayer::startPoll()
{
    if (!m_poll) {
        m_poll = new QTimer(this);
        QObject::connect(m_poll, SIGNAL(timeout()), this, SLOT(onPoll()));
    }
    // 【v64】轮询间隔从 250ms 放宽到 500ms。
    // 界面按秒刷新，250ms 采样纯属多余；而每次采样都要跨库调 3 个函数（内部要拿锁），
    // 在播放线程也在抢锁的情况下，采样越密越容易造成抖动。
    // 500ms 对"播放结束"的判定（留 200ms 余量）依然够用。
    if (!m_poll->isActive()) m_poll->start(500);
}

void VideoPlayer::stopPoll()
{
    if (m_poll) m_poll->stop();
}

void VideoPlayer::onPoll()
{
    if (!m_player) return;
    const qint64 pos = m_player->positionTicks();
    const qint64 dur = m_player->durationTicks();
    const int st = m_player->state();
    // 缓存原始 ticks / state（零缩放），供诊断读取。
    m_rawPos = pos;
    m_rawDur = dur;
    m_rawState = st;

    // 【1.0.0.52】伪 ENDED 护栏第一道（埋点见 play()/replay()，说明见 hpp）：
    // 只要位置跌破开播基线一次（= 新流确实从头起跑了），就解锁 ENDED 判定。
    // 放在 poll 最前面：seek 保护窗提前 return 也不能漏掉这次观察。
    if (!m_posSanitized && m_openClock.isValid() && pos < m_openBasePos) {
        m_posSanitized = true;
    }

    // ================= 【v85】seek 保护窗口 =================
    //
    // 刚 seek 完，播放器要重开解码器，这期间 positionTicks() 读到的还是**旧位置**。
    // 如果照常写回 m_position，进度条就会先弹回拖动之前、再跳回来 ——
    // 这就是用户说的"有时会回弹"。
    //
    // 1.5 秒内改用墙钟推进（seek 时 m_clockBaseMs 已同步跳到目标值），
    // 同时跳过下面的 tick 单位自校准（此刻的 pos 是旧值，拿它算比值会把单位定错）。
    if (m_seekClock.isValid() && m_seekClock.elapsed() < 1500) {
        int est;
        if (m_scanDir != 0) {
            // 【v86】扫动期间进度按 3 倍速走，公式与 onScanTick 完全一致。
            // 否则每次 seek 都会重置墙钟基准，进度条被算成 1 倍速，
            // 看着比实际慢一大截（画面已经跳过去了，条子还在慢慢爬）。
            est = m_scanBaseMs + m_scanDir * (int)(m_scanClock.elapsed() * 3);
        } else {
            est = m_clockOn ? (m_clockBaseMs + (int)m_clock.elapsed()) : m_position;
        }
        if (est < 0) est = 0;
        if (m_duration > 0 && est > m_duration) est = m_duration;
        if (m_apiDurMs > 0 && est > m_apiDurMs) est = m_apiDurMs;
        m_position = est;
        const int gSec = est / 1000;
        if (gSec != m_lastEmitSec) {
            m_lastEmitSec = gSec;
            emit positionChanged();
            m_pollSeq++;
            emit pollSeqChanged();
        }
        return;
    }

    // ================= 【v74】tick 单位自校准 =================
    //
    // wrapper 里的 TICKS_PER_MS = 1000 是**猜的**（假设 libbbplayer 用微秒）。
    // 猜错的后果很直观：若实际是毫秒，播了 10 秒的 pos=10000 → /1000 = 10ms →
    // 时间标签永远停在 0:00，进度条一动不动。
    //
    // 校准办法：拿"墙钟真实经过的毫秒"当尺子，和 bbplayer_position 的原始值求比值，
    // 看它落在哪个量级。窗口取 3~20 秒 —— 太短会被首帧缓冲干扰，太长则用户要干等。
    // 只校准一次（m_ticksPerMs 一旦定下就不再改），因为播放中途暂停/缓冲会让比值失真。
    if (m_ticksPerMs == 0 && m_clockOn && pos > 0) {
        const qint64 elapsed = m_clock.elapsed();
        if (elapsed >= 3000 && elapsed <= 20000) {
            const double ratio = (double)pos / (double)elapsed;   // 期望 ≈ ticksPerMs
            if (ratio > 0.3 && ratio < 3.0)            m_ticksPerMs = 1;        // 毫秒
            else if (ratio > 300.0 && ratio < 3000.0)  m_ticksPerMs = 1000;     // 微秒
            else if (ratio > 3.0e5 && ratio < 3.0e6)   m_ticksPerMs = 1000000;  // 纳秒
            // 落在别的量级说明取到的是垃圾值（例如刚 open 完还没起播），不设，
            // 下一轮再来 —— 宁可多等几秒也不要定错单位。
        }
    }

    int posMs;
    int durMs;
    if (m_ticksPerMs != 0) {
        posMs = (int)(pos / m_ticksPerMs);
        durMs = (int)(dur / m_ticksPerMs);
    } else {
        // 还没校准成功：说明 pos 要么是 0、要么单位未知。
        // 用墙钟给界面一个"至少在动"的进度（缓冲会让它略快于画面，可接受）。
        posMs = m_clockOn ? (m_clockBaseMs + (int)m_clock.elapsed()) : m_position;
        durMs  = 0;   // 时长交给 m_apiDurMs 兜底（见 durationMs()）
    }
    // 内部值保持精确（seekBy 要用），但**是否通知界面**按"秒"判定。
    m_position = posMs;

    // 【v64 关键】界面刷新从 4Hz 降到 1Hz。
    //
    // 原来每 250ms 就 emit 一次 positionChanged → QML 的 timeLabel 文本变化 →
    // Label 重绘 → Cascades 让合成器重新合成这一帧（此时场景里还压着一个 30fps
    // 的视频窗口）→ 视频管线被打断，音频跟着重新同步，听起来就是"一卡一下"。
    // 时间显示按秒刷新完全够用，没必要每秒惊动四次合成器。
    bool dirty = false;
    const int posSec = posMs / 1000;
    if (posSec != m_lastEmitSec) {
        m_lastEmitSec = posSec;
        dirty = true;
    }
    // 【v74】只有真拿到 > 0 的时长才写回 m_duration。
    // 原来无条件赋值，流式播放时每轮都把 m_duration 覆盖成 0，
    // 连 API 兜底都救不回来（durationMs() 里 m_duration>0 才优先）。
    if (durMs > 0 && durMs != m_duration) {
        m_duration = durMs;
        dirty = true;
    }
    if (dirty) {
        emit positionChanged();
        emit durationChanged();
        // 轮询序列号与界面刷新同频，避免又开一条 4Hz 的绑定重算路径。
        m_pollSeq++;
        emit pollSeqChanged();
    }

    // v48：事件回调里的错误优先展示。库内部 state=5 就是"错误"，但错误原文只在
    // type=7 回调里出现（snprintf 到临时栈缓冲），轮询句柄字段读不到。
    // v55：lastError() 是累积值不会自清，用 m_lastErrShown 去重，否则 http 回退
    // 成功后这条旧错误会立刻把状态又刷回 error。
    // v64：只读一次，别每轮都去拿锁拷字符串（回调线程也在抢这把锁）。
    const QString cbErr = m_player->lastError();
    if (!cbErr.isEmpty() && cbErr != m_lastErrShown) {
        m_lastErrShown = cbErr;
        setState("error: " + cbErr);
        // 证书/SSL 类错误 → 用 http 重试一次，验证链路其余部分是否完好。
        if (cbErr.indexOf("SSL", 0, Qt::CaseInsensitive) >= 0
            || cbErr.indexOf("cert", 0, Qt::CaseInsensitive) >= 0) {
            tryHttpFallback();
        }
    }
    // 播放自然结束：进度追上时长（留 200ms 余量），翻转 playing 状态。
    // 【v74】用 durationMs()（含 API 兜底）而不是 durMs —— 播放器给不出时长时
    // durMs 恒 0，这个判定会永久失效，视频播完状态还停在 playing。
    // 【1.0.0.24】双通道事实判定（修"缓冲卡死被误判成播完→ premature loop"）：
    //   A. m_eofLikely：代理已把整段字节送达播放器（硬事实，卡带不可能伪造）；
    //   B. trustedDur：播放器自报容器时长（durMs>0）且 tick 单位已校准
    //      （m_ticksPerMs!=0）—— 此时 posMs 不是墙钟虚跑值。
    // 两通道都没有时保持 playing（宁可多显示一帧，不可错误重启）。
    //
    // 【1.0.0.52】伪 ENDED 护栏第二道（见 hpp m_openBasePos 长注释）：
    //   1) openFresh：开播后 2.5s 内 positionTicks() 读数整体不可信
    //      （openUrl/seek 后解码器要重起，1.0.0.51 实锤 282ms 时还读到旧结尾值）；
    //   2) m_posSanitized：必须真的观察到位置回落过，否则"旧值 pin 在结尾"会
    //      被判成播完 → ENDED→REPLAY→ENDED 双重重播 → EOF 态 openUrl 降速。
    {
        const int total = durationMs();
        const bool trustedDur = (durMs > 0 && m_ticksPerMs != 0);
        const bool openFresh = m_openClock.isValid() && m_openClock.elapsed() < 2500;
        if (total > 0 && posMs >= total - 200 && m_playing && (m_eofLikely || trustedDur)
            && !openFresh && m_posSanitized) {
            // 快照：setState()/emit 会【同步】触发 QML 的 pstate 处理器。
            // 1.0.0.24~48 该处理器同步调 replay()，重入play()改掉成员；
            // 1.0.0.49 起 replay 移到本块末尾（通知已返回后才跑），重入不再
            // 发生 —— 快照保留仅为日志诚实（防御未来再有人把重入加回来）。
            const int snapPos = posMs;
            const int snapTotal = total;
            const int snapEof = m_eofLikely ? 1 : 0;
            const int snapTrust = trustedDur ? 1 : 0;
            const int snapSan = m_posSanitized ? 1 : 0;
            // 【1.0.0.50】循环重播 = 用户视角"没停过"：ended 期间 playing
            // 保持 true。旧代码在此先把 m_playing 置 false 再 emit，
            // 真机 swipe log 每轮循环都出现 `LOOP ended → ICON play →
            // （0.4s 恢复期）→ ICON pause` 的图标闪断 —— pause 图标正是
            // 抖音式常态。QML 面（vplaying 影子 / 中央指示器 / 播放钮）
            // 在 ended 期间持 true 均已被证实安全：撤罩早已完成、指示器
            // 不该因换片闪出、播放钮应保持 pause 图标。
            // 注意 stop() 仍有自己的 m_playing=false，由 replay() 的
            // m_replayQuiet 静默窗统一下发NOTIFY（见 replay()）。
            m_clockOn = false;
            m_ended = true;
            // 【1.0.0.49】重构：setState/emit 仍会【同步】触发 QML 的 pstate
            // 处理器，但该处理器 1.0.0.49 起只写日志（LOOP ended），不再调用
            // replay() —— 循环重播的决策移到这里（C++ 状态机单一所有权），
            // 消除了"绑定求值链内同步 setState"导致的重入（真机证据：
            // 每次循环重播 3 条 Binding loop detected for property pstate）。
            // 本轮的三条通知（含 QML 处理）此刻已全部返回，此处 replay() 是
            // 干净的新栈帧，其内部 setState("stopped"/"playing") 都是独立的
            // 顶层求值，不再重生入。
            setState("ended");
            // playing 值未变（1.0.0.50 起 ended 期间保持 true），不再发通知。
            emit endedChanged();
            qDebug("BiliClient playback ended pos=%d total=%d eof=%d trustedDur=%d sanitized=%d",
                   snapPos, snapTotal, snapEof, snapTrust, snapSan);
            playTrace(QString("ENDED pos=%1 total=%2 eof=%3 trustedDur=%4 sanitized=%5")
                      .arg(snapPos).arg(snapTotal).arg(snapEof).arg(snapTrust).arg(snapSan));
            // 【1.0.0.49】循环重播：QML 侧不再触发，播放器自己决策。
            // m_lastUrl 空的情况 replay() 内部会置 error 态并返回。
            if (m_loopOn) {
                replay();
            }
        }
    }
}

void VideoPlayer::tryHttpFallback()
{
    if (m_httpFallbackTried) return;
    if (!m_player || m_lastUrl.isEmpty()) return;
    if (!m_lastUrl.startsWith("https://")) return;
    m_httpFallbackTried = true;

    QString u = m_lastUrl;
    u.replace(0, 8, "http://");   // "https://" → "http://"（长度相同，直接覆盖前 8 字节）
    qDebug("BiliClient http fallback: %s", u.toUtf8().constData());

    // 【v56】这里**不要**调 m_player->close()！
    // v55 调了 close 再 openUrl，结果 bbplayer_close 把 preparePlayer() 提前建好的
    // 屏幕窗口一起销毁了；而 BB10 的 ForeignWindowControl 加入场景时只扫一次匹配
    // windowId 的窗口，之后不回头重扫 → boundToWindow 永远 false，表现为
    // 「有声音、解码器 30fps 在跑、就是没画面」。真机截图 fwc=N 正是这个。
    // 直接 openUrl 新地址，让 libbbplayer 在同一个窗口上换流即可。
    m_position = 0;
    m_duration = 0;
    emit positionChanged();
    emit durationChanged();

    if (!m_player->openUrl(u) || !m_player->play()) {
        setState("error: http 回退也失败");
        return;
    }
    m_playing = true;
    emit playingChanged();
    setState("playing(http)");
}

void VideoPlayer::setState(const QString& s)
{
    m_state = s;
    emit stateChanged(m_state);
}

// ============================================================================
// 【已废弃】流式播放：ProxyStreamWorker + VideoProxy
//
// v29~v34 曾试图用 127.0.0.1 loopback 代理 + bb::multimedia::MediaPlayer 实现边下边播，
// 但 mmrenderer 的 http 客户端既加不了 UA/Referer（CDN 403）又连不上 loopback，全部失败。
//
// v35 起改为直接复用 BBTube 的 libbbplayer.so（见 bbplayer_wrapper.*）：它绕开 mmrenderer，
// 用自带 curl reader 吃 CDN 直链、字节逐块喂解码器，全程不落盘。下面这套 ProxyStreamWorker
// + VideoProxy 代码已不再被调用，保留仅作历史参考，勿在新流程里使用。
// ============================================================================

// 缓冲区高水位：超过就阻塞 curl 的写回调，等主线程泵完再继续。
// 2MB 足够吸收网络抖动，又不会让内存随片长无限增长（40MB 的片子也只占 2MB）。
static const int kProxyHighWater = 2 * 1024 * 1024;

// ============================================================================
// 【1.0.0.24】刷视频流畅度优化 + 量测通道
//
// 优化一：跨 worker 共享 DNS 缓存。
//   每个 Range 请求都是一个新建的 easy handle（ProxyStreamWorker 一次一生），
//   意味着每个 Range 都要重新解析一遍 CDN 域名。BB10 走运营商 DNS，一次解析
//   几十到几百毫秒；一部短视频 6~10 个 Range，连续刷视频同样的 CDN 主机
//   （365yg.com / douyinvod.com 就那么几个）被反复解析 —— 这就是换片
//   "要等半天"的组成部分。curl_share 的 DNS 缓存共享是官方支持的多线程
//   安全用法，零行为副作用（不共享连接缓存：worker 随时可能被 abort/detach，
//   跨线程连接复用在 curl 7.24 上与短命 worker 组合风险不可控，项目已有
//   worker UAF 前科，见 GATE2）。
//
// 量测通道：playTrace() 把播放链路关键事件按毫秒时间戳写进 dyplay.log
//   （data/ 持久 + tmp/），与 douyin_swipe.log（同为 hh:mm:ss.zzz）放在
//   同一时间轴上算换片时延：SWITCH → PROXY start → req → first bytes →
//   PLAY → ENDED → REPLAY。没有量测的优化是猜，有了它才能自证快没快。
// ============================================================================
void playTrace(const QString& s)
{
    const QString line = QDateTime::currentDateTime().toString("hh:mm:ss.zzz") + "  " + s;
    QStringList dirs;
    dirs << QDir::tempPath() << QDir::homePath();
    for (int i = 0; i < dirs.size(); ++i) {
        if (dirs.at(i).isEmpty()) continue;
        QFile f(QDir(dirs.at(i)).filePath("dyplay.log"));
        if (f.open(QIODevice::Append | QIODevice::Text)) {
            QTextStream ts(&f);
            ts << line << "\n";
            f.flush();
            f.close();
        }
    }
}

static CURLSH* g_dnsShare = 0;          // 进程级 DNS 缓存共享句柄
static QMutex g_dnsCreateMutex;         // 只保护创建
static QMutex g_dnsLockMutex;           // curl 锁回调用（与创建锁分开，防重入死锁）

extern "C" {
static void dnsShareLock(CURL* handle, curl_lock_data data,
                         curl_lock_access access, void* userptr)
{
    Q_UNUSED(handle) Q_UNUSED(data) Q_UNUSED(access) Q_UNUSED(userptr)
    g_dnsLockMutex.lock();
}
static void dnsShareUnlock(CURL* handle, curl_lock_data data, void* userptr)
{
    Q_UNUSED(handle) Q_UNUSED(data) Q_UNUSED(userptr)
    g_dnsLockMutex.unlock();
}
}

/** 惰性创建 DNS 共享句柄（多线程安全；失败时返回 0，调用方照常播放）。 */
static CURLSH* dnsShare()
{
    if (g_dnsShare) return g_dnsShare;
    QMutexLocker ck(&g_dnsCreateMutex);
    if (g_dnsShare) return g_dnsShare;
    CURLSH* sh = curl_share_init();
    if (!sh) return 0;
    curl_share_setopt(sh, CURLSHOPT_LOCKFUNC, &dnsShareLock);
    curl_share_setopt(sh, CURLSHOPT_UNLOCKFUNC, &dnsShareUnlock);
    // 只共享 DNS 缓存；连接缓存（CURL_LOCK_DATA_CONNECT）故意不共享，理由见上。
    // 7.57 才把 DNS 缓存拆成独立的 CURL_LOCK_DATA_DNS_CACHE；本 NDK 是 curl 7.24
    // 家族，只有 CURL_LOCK_DATA_DNS（旧名，语义 = 共享 DNS 缓存）。
    curl_share_setopt(sh, CURLSHOPT_SHARE, (long)CURL_LOCK_DATA_DNS);
    g_dnsShare = sh;
    return g_dnsShare;
}

/**
 * 缓存上限 32MB（覆盖绝大多数抖音视频整文件）；超出从头部丢弃。
 * 定义见 video_store.hpp（value 成员需要完整类型）。
 */
static const int kProxyCacheCap = 32 * 1024 * 1024;

class ProxyStreamWorker : public QThread {
public:
    /**
     * 【1.0.0.23】alts = 同一视频的备用 CDN 直链（feed 行 play_addr.url_list 的其余项）。
     * 为什么需要：抖音 CDN 会 302 到 IPv6 字面量主机（[2409:8c20:...]），BB10 的
     * curl 7.24 没有可用 IPv6 通路，跟过去必 rc=7（COULDNT_CONNECT）——真机日志
     * `rc=7 http=302` 与弹窗"播放链路失败：curl 7"就是这么来的。手动走跳转链 +
     * 多候选重试后，单个节点抽风不再等于播放失败。
     */
    ProxyStreamWorker(const QString& url, qint64 start, qint64 end,
                      const QString& ua = QString(), const QString& referer = QString(),
                      const QStringList& alts = QStringList(),
                      ProxyCache* cache = 0,
                      const QByteArray& cacheSlice = QByteArray(),
                      qint64 cacheSliceStart = -1)
        : m_url(url)
        , m_start(start)
        , m_end(end)
        , m_ua(ua)
        , m_referer(referer)
        , m_alts(alts)
        , m_cache(cache)
        , m_cacheSlice(cacheSlice)
        , m_cacheSliceStart(cacheSliceStart)
        , m_absPos(start)
        , m_abort(false)
        , m_done(false)
        , m_failed(false)
        , m_newResponse(true)
        , m_bodyDiscard(false)
    {
        qDebug("BiliClient proxy worker CTOR this=%p range=%lld-%lld alts=%d cache=%d",
               (void*)this, (long long)start, (long long)end, m_alts.size(),
               cacheSliceStart >= 0 ? 1 : (cache ? 2 : 0));
    }

    ~ProxyStreamWorker()
    {
        qDebug("BiliClient proxy worker DTOR this=%p", (void*)this);
        abortNow();
        wait(2000);
    }

    /** 请求停止（可在任意线程调用）。 */
    void abortNow()
    {
        QMutexLocker lk(&m_mutex);
        m_abort = true;
        m_cond.wakeAll();
    }

    /** 主线程取走缓冲区里全部数据。 */
    QByteArray take()
    {
        QMutexLocker lk(&m_mutex);
        QByteArray out;
        out.swap(m_buf);
        m_cond.wakeAll();   // 唤醒可能正卡在高水位上的写回调
        return out;
    }

    int buffered()
    {
        QMutexLocker lk(&m_mutex);
        return m_buf.size();
    }

    bool isDone()
    {
        QMutexLocker lk(&m_mutex);
        return m_done;
    }

    bool hasFailed()
    {
        QMutexLocker lk(&m_mutex);
        return m_failed;
    }

    QString failReason()
    {
        QMutexLocker lk(&m_mutex);
        return m_err;
    }

protected:
    void run();

private:
    static size_t writeCb(char* ptr, size_t size, size_t nmemb, void* ud);
    /** 【1.0.0.28】本地预取文件直服：seek(m_start) 后分块读进 m_buf
     *  （curl 的 file:// 不认 CURLOPT_RANGE，BB10 curl 7.24 实测忽略，
     *   所以不能让 curl 碰 file://，必须自己 seek/read）。 */
    void runLocal();
    /** 【1.0.0.45】会话缓存直服：把快照切片按 seek/read 的节拍喂进 m_buf，
     *  与 curl / runLocal 的接口完全一致（m_buf / m_done / m_failed 语义相同）。 */
    void runCache();
    /** 【1.0.0.23】响应头回调：抓状态行 + Location，并决定 3xx 的 body 是否丢弃。 */
    static size_t headerCb(char* ptr, size_t size, size_t nmemb, void* ud);
    // 注意：NDK 里的 libcurl 是 7.24，没有 CURLOPT_XFERINFOFUNCTION（7.32 才有），
    // 只能用老式 PROGRESSFUNCTION（double 参数）。它的唯一作用就是给我们一个
    // "每秒会被叫几次"的钩子，用来及时响应 abort —— 光靠 LOW_SPEED_TIME 的话，
    // 链路卡死时要等 20 秒才能停下来。
    static int progressCb(void* ud, double dlt, double dln, double ult, double uln);

    QMutex m_mutex;
    QWaitCondition m_cond;
    QByteArray m_buf;
    QString m_url;
    /** 【1.0.0.23】同一视频的备用 CDN 直链。 */
    QStringList m_alts;
    /** 【1.0.0.45】会话级 CDN 缓存（可空；value 语义由 VideoProxy 持有，这里
     *  只借指针。worker 线程在 writeCb 里 append，主线程在 handleRequest 里快照）。 */
    ProxyCache* m_cache;
    /** 【1.0.0.45】缓存直服模式：源数据快照 + 其文件偏移（>=0 即缓存模式）。 */
    QByteArray m_cacheSlice;
    qint64 m_cacheSliceStart;
    /** 当前正文字节的文件绝对偏移（仅 worker 线程碰；3xx body 被丢弃不推进）。 */
    qint64 m_absPos;
    qint64 m_start;
    qint64 m_end;
    /** 【抖音客户端】拉 CDN 用的 UA/Referer；为空则用 B 站默认值。 */
    QString m_ua;
    QString m_referer;
    bool m_abort;
    bool m_done;
    bool m_failed;
    QString m_err;
    /** 头回调解析状态（只有 worker 线程会碰，不需要锁）。 */
    bool m_newResponse;      /**< 下一次头数据应是状态行（每个 perform 前重置）。 */
    bool m_bodyDiscard;      /**< 3xx 跳转响应的 body 直接丢弃，不许混进视频流。 */
    QByteArray m_hdrLine;    /**< 未完结的头行累积。 */
    QString m_curLocation;   /**< 本次响应的 Location 头。 */
};

size_t ProxyStreamWorker::writeCb(char* ptr, size_t size, size_t nmemb, void* ud)
{
    ProxyStreamWorker* w = static_cast<ProxyStreamWorker*>(ud);
    const size_t n = size * nmemb;

    QMutexLocker lk(&w->m_mutex);
    // 背压：缓冲超过高水位就挂住，不再从 CDN 拉。主线程每 20ms 泵一次并唤醒这里。
    while (!w->m_abort && w->m_buf.size() > kProxyHighWater) {
        w->m_cond.wait(&w->m_mutex, 200);
    }
    if (w->m_abort) return 0;   // 返回 0 = 让 curl 中止这次传输

    // 【1.0.0.23】3xx 跳转响应的 body 一律丢弃：那是 CDN 的提示页，不是视频字节。
    // 放进缓冲区会直接毁掉整条流的前几KB（播放器拿到 HTML 开头即解析失败）。
    if (w->m_bodyDiscard) return n;

    // 【1.0.0.45】顺路进会话缓存（仅 CDN 源；3xx body 已在上面丢弃，不会污染缓存）。
    if (w->m_cache) {
        QMutexLocker ck(&w->m_cache->mutex);
        const qint64 cacheEnd = w->m_cache->start + w->m_cache->buf.size();
        if (w->m_absPos >= cacheEnd) {
            // 正常前进（紧贴缓存尾）；absPos > cacheEnd = 前面有洞（进度条回拖
            // 后重新抓取）→ 重建缓存，自愈。
            if (w->m_absPos > cacheEnd) {
                w->m_cache->buf.clear();
                w->m_cache->start = w->m_absPos;
            }
            w->m_cache->buf.append(ptr, (int)n);
        } else if (w->m_absPos + (qint64)n > cacheEnd) {
            // 部分重叠：只补尾部超出缓存的一段（头部已在缓存里，不重复）。
            const qint64 skip = cacheEnd - w->m_absPos;
            w->m_cache->buf.append(ptr + skip, (int)((qint64)n - skip));
        }
        if (w->m_cache->buf.size() > kProxyCacheCap) {
            const int drop = w->m_cache->buf.size() - kProxyCacheCap;
            w->m_cache->buf.remove(0, drop);
            w->m_cache->start += drop;
        }
    }

    w->m_buf.append(ptr, (int)n);
    w->m_absPos += (qint64)n;
    return n;
}

int ProxyStreamWorker::progressCb(void* ud, double dlt, double dln, double ult, double uln)
{
    Q_UNUSED(dlt) Q_UNUSED(dln) Q_UNUSED(ult) Q_UNUSED(uln)
    ProxyStreamWorker* w = static_cast<ProxyStreamWorker*>(ud);
    QMutexLocker lk(&w->m_mutex);
    return w->m_abort ? 1 : 0;
}

/**
 * 【1.0.0.23】响应头回调（替代 CURLOPT_FOLLOWLOCATION 的自动跟随）。
 *
 * 为什么关掉自动跟随：BB10 的 curl 7.24 跟随 302 到 IPv6 字面量主机
 * （Location: http://[2409:8c20:...]/...）时连接必失败 → rc=7。host 侧协议
 * 实证：同一条直链有时 200、有时 302 到 IPv6 字面量节点（负载均衡），自动跟随
 * 等于把命运交给对方 DNS。这里手工走链：Location 是 IPv6 字面量就否决该候选，
 * 换 url_list 里的备用直链；3xx 的响应体一律丢弃，不进视频缓冲区。
 *
 * 头数据可能按任意切分到达（不保证一行一次回调），所以按 '\n' 切行、行内累积。
 */
size_t ProxyStreamWorker::headerCb(char* ptr, size_t size, size_t nmemb, void* ud)
{
    ProxyStreamWorker* w = static_cast<ProxyStreamWorker*>(ud);
    const size_t n = size * nmemb;
    if (!w) return n;

    w->m_hdrLine.append(ptr, (int)n);
    int cut;
    while ((cut = w->m_hdrLine.indexOf('\n')) >= 0) {
        QByteArray line = w->m_hdrLine.left(cut).trimmed();
        w->m_hdrLine.remove(0, cut + 1);
        if (line.isEmpty()) continue;
        if (w->m_newResponse) {
            w->m_newResponse = false;
            if (line.startsWith("HTTP/")) {
                const QList<QByteArray> parts = line.split(' ');
                if (parts.size() >= 2) {
                    const int code = parts.at(1).toInt();
                    // 3xx = 跳转响应：body 丢掉；2xx = 正文开始，放行。
                    w->m_bodyDiscard = (code >= 300 && code < 400);
                }
            }
            continue;
        }
        const QByteArray lower = line.toLower();
        if (lower.startsWith("location:")) {
            w->m_curLocation = QString::fromUtf8(line.mid(9).trimmed());
        }
    }
    return n;
}

void ProxyStreamWorker::run()
{
    // 【1.0.0.45】缓存直服优先级最高：命中会话缓存的 Range 不碰网络
    // （libbbplayer 的"当前位置到文件尾"式大范围请求常见，省 TLS+重传）。
    if (m_cacheSliceStart >= 0) {
        runCache();
        return;
    }
    // 【1.0.0.28】file:// 源 = 本地预取文件（换片秒开，官方抖音
    // asyncPrepareVideo 同款思路，见 video_prefetch.hpp）。
    // curl 7.24 的 file:// 处理器忽略 CURLOPT_RANGE（要么给整文件、要么报错），
    // 而播放器只会按 Range 请求，所以必须自己 seek/read 供字节。
    if (m_url.startsWith("file://")) {
        runLocal();
        return;
    }

    // curl 7.24 的 CURLOPT_RANGE 就是 "start-end" / "start-" 形式
    QByteArray range = QByteArray::number(m_start) + "-";
    if (m_end >= m_start) {
        range += QByteArray::number(m_end);
    }

    CURL* h = curl_easy_init();
    if (!h) {
        QMutexLocker lk(&m_mutex);
        m_failed = true;
        m_err = "curl init";
        m_done = true;
        m_cond.wakeAll();
        return;
    }

    struct curl_slist* hs = 0;
    // 【抖音客户端】UA / Referer 可被 VideoProxy::setHeaders() 覆盖。
    //
    // 为什么必须能改：CDN 会校验请求来源。抖音直链带
    // `Referer: https://www.bilibili.com/` 必然 403 —— 不是"抖音更严"，
    // 而是 Referer 里的站点根本不是抖音。留空时沿用 B 站默认值，
    // BiliBar / BerryLi 的行为一点不变。
    const QString ua = m_ua.isEmpty()
        ? QString("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                  "AppleWebKit/537.36 (KHTML, like Gecko) "
                  "Chrome/120.0.0.0 Safari/537.36")
        : m_ua;
    const QString ref = m_referer.isEmpty()
        ? QString("https://www.bilibili.com/")
        : m_referer;
    hs = curl_slist_append(hs, ("User-Agent: " + ua).toUtf8().constData());
    hs = curl_slist_append(hs, ("Referer: " + ref).toUtf8().constData());
    hs = curl_slist_append(hs, ("Origin: " + ref).toUtf8().constData());
    hs = curl_slist_append(hs, "Accept: */*");

    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hs);
    curl_easy_setopt(h, CURLOPT_RANGE, range.constData());
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, &ProxyStreamWorker::writeCb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, this);
    // 【1.0.0.24】共享 DNS 缓存：同一批 CDN 主机的后续 Range 不再重复解析。
    if (CURLSH* sh = dnsShare()) {
        curl_easy_setopt(h, CURLOPT_SHARE, sh);
    }
    // 【1.0.0.23】手工走跳转链：关掉自动跟随，改由下面的循环一格格走，
    // 以便识别 IPv6 字面量跳转并在候选间切换（见 headerCb 注释）。
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, &ProxyStreamWorker::headerCb);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, this);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 12L);
    // 卡住 20 秒只有 1KB/s 以下 → 认为链路死了，交给上层回退
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_PROGRESSFUNCTION, &ProxyStreamWorker::progressCb);
    curl_easy_setopt(h, CURLOPT_PROGRESSDATA, this);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);

    // 候选 URL：首选 + 备用（同视频的其它 CDN 节点），去重保序。
    QStringList urls;
    if (!m_url.isEmpty()) urls << m_url;
    for (int i = 0; i < m_alts.size(); ++i) {
        const QString a = m_alts.at(i);
        if (!a.isEmpty() && !urls.contains(a)) urls << a;
    }
    if (urls.isEmpty()) {
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, 0);
        curl_slist_free_all(hs);
        curl_easy_cleanup(h);
        QMutexLocker lk(&m_mutex);
        m_failed = true;
        m_err = "no url";
        m_done = true;
        m_cond.wakeAll();
        return;
    }

    const int kMaxHops = 6;   // 一条候选最多跟 6 跳，防 Location 环
    bool got = false;
    QString lastWhy = "cdn-unavailable";

    for (int ci = 0; ci < urls.size() && !m_abort && !got; ++ci) {
        QByteArray cur = urls.at(ci).toUtf8();
        for (int hop = 0; hop < kMaxHops && !m_abort && !got; ++hop) {
            m_hdrLine.clear();
            m_curLocation.clear();
            m_newResponse = true;
            m_bodyDiscard = false;
            curl_easy_setopt(h, CURLOPT_URL, cur.constData());

            const CURLcode rc = curl_easy_perform(h);

            long httpCode = 0;
            curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &httpCode);

            if (m_abort) break;   // 用户切片/退出：直接结束，不算失败

            if (rc != CURLE_OK) {
                QMutexLocker lk(&m_mutex);
                m_failed = true;
                m_err = QString("curl %1").arg((int)rc);
                m_done = true;
                m_cond.wakeAll();
                qDebug("BiliClient proxy worker RUN-END this=%p rc=%d http=%ld buffered=%d hops=%d",
                       (void*)this, (int)rc, httpCode, m_buf.size(), hop);
                curl_easy_setopt(h, CURLOPT_HTTPHEADER, 0);
                curl_slist_free_all(hs);
                curl_easy_cleanup(h);
                return;
            }

            if (httpCode == 200 || httpCode == 206) {
                got = true;   // 正文已在 headerCb 判定后由 writeCb 收进 m_buf
                break;
            }

            if (httpCode >= 300 && httpCode < 400 && !m_curLocation.isEmpty()) {
                const QString loc = m_curLocation;
                // 【1.0.0.23】Location 主机是 IPv6 字面量（http://[2409:...]/...）：
                // BB10 curl 7.24 无 IPv6 通路，跟过去必 rc=7。
                // 该候选节点走不通，立刻换下一条候选（不在此候选上重试跳转）。
                if (loc.contains("://[") || loc.startsWith("//[")) {
                    lastWhy = "cdn-ipv6-hop";
                    qDebug("BiliClient proxy worker IPv6-literal redirect, drop candidate #%d: %s",
                           ci, qPrintable(loc.left(80)));
                    break;
                }
                // 相对 Location 基于当前 URL 解析（curl 语义）。
                QUrl base = QUrl::fromEncoded(cur);
                QUrl next = base.resolved(QUrl(loc));
                cur = next.toEncoded();
                continue;
            }

            if (httpCode >= 400) {
                // 403/404 之类：候选本身失效，换下一条（有时换 UA 无效，但备用
                // 节点的签名是独立的，值得一试）。
                lastWhy = QString("http %1").arg(httpCode);
                break;
            }

            // 1xx / 未预料：不重试，换候选。
            lastWhy = QString("code %1").arg(httpCode);
            break;
        }
    }

    curl_easy_setopt(h, CURLOPT_HTTPHEADER, 0);
    curl_slist_free_all(hs);
    curl_easy_cleanup(h);

    QMutexLocker lk(&m_mutex);
    if (m_abort) {
        m_done = true;
        m_cond.wakeAll();
        return;
    }
    if (!got) {
        m_failed = true;
        m_err = lastWhy;
    }
    m_done = true;
    m_cond.wakeAll();
    qDebug("BiliClient proxy worker RUN-END this=%p got=%d http=done buffered=%d",
           (void*)this, got ? 1 : 0, m_buf.size());
}

// ---------------------------------------------------------------------------

/**
 * 【1.0.0.28】本地预取文件直服（file:// 源）。
 *
 * 为什么值得一个独立分支：预取命中的换片不再等 CDN 建连 + TLS + 首字节，
 * STREAM→firstbytes 时延直接塌到磁盘读的几毫秒（真机对比见 dyplay.log 的
 * "PREFETCH hit" / "PROXY firstbytes" 两行时间戳）。
 *
 * 与 curl 分支的接口完全一致（m_buf / m_done / m_failed / m_abort 语义相同），
 * VideoProxy 的 onPump / 收尸纪律一行都不用改。seek/read 分块 256KB，
 * 同样受高水位背压约束（writeCb 的同款等待逻辑）。
 */
void ProxyStreamWorker::runLocal()
{
    // 剥 file:// 前缀：QNX 下 homePath() 是绝对路径（/accounts/1000/...），
    // 拼出来的 file://<abs> 剥掉 scheme 就是可直接 open 的路径。
    const QString path = m_url.mid(7);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMutexLocker lk(&m_mutex);
        m_failed = true;
        m_err = "local open";
        m_done = true;
        m_cond.wakeAll();
        qDebug("BiliClient proxy worker LOCAL open failed: %s", qPrintable(path));
        return;
    }

    const qint64 fsize = f.size();
    qint64 pos = m_start > 0 ? m_start : 0;
    if (fsize > 0 && pos >= fsize) {
        // 越界请求（播放器偶尔要 start == total）：按空流优雅收尾，不算失败。
        f.close();
        QMutexLocker lk(&m_mutex);
        m_done = true;
        m_cond.wakeAll();
        qDebug("BiliClient proxy worker LOCAL beyond-eof pos=%lld size=%lld",
               (long long)pos, (long long)fsize);
        return;
    }
    if (!f.seek(pos)) {
        f.close();
        QMutexLocker lk(&m_mutex);
        m_failed = true;
        m_err = "local seek";
        m_done = true;
        m_cond.wakeAll();
        return;
    }

    // 剩余量：end 有界按 end 算，否则到 EOF（-1 = 不限）。
    qint64 left = -1;
    if (m_end >= pos) left = m_end - pos + 1;

    const int kChunk = 256 * 1024;
    while (!m_abort && left != 0) {
        {
            QMutexLocker lk(&m_mutex);
            // 与 writeCb 同一套背压：缓冲堆过高就挂住等主线程来取。
            while (!m_abort && m_buf.size() > kProxyHighWater) {
                m_cond.wait(&m_mutex, 200);
            }
            if (m_abort) break;
        }
        qint64 want = (left > 0 && left < kChunk) ? left : kChunk;
        QByteArray chunk = f.read(want);
        if (chunk.isEmpty()) break;   // EOF（或读错误，都按流结束处理）
        {
            QMutexLocker lk(&m_mutex);
            if (m_abort) break;
            m_buf.append(chunk);
            m_cond.wakeAll();
        }
        if (left > 0) left -= chunk.size();
    }
    f.close();

    QMutexLocker lk(&m_mutex);
    if (m_abort) {
        m_done = true;
        m_cond.wakeAll();
        return;
    }
    // 文件比承诺的短 = 磁盘文件坏了（不该发生：预取是全量下载 + 大小校验）。
    // 交 proxy 的 short stream 回退（HUD 报错 + 重试回落 CDN）。
    if (left > 0) {
        m_failed = true;
        m_err = "local short";
    }
    m_done = true;
    m_cond.wakeAll();
    qDebug("BiliClient proxy worker LOCAL done this=%p start=%lld size=%lld",
           (void*)this, (long long)pos, (long long)fsize);
}

// ---------------------------------------------------------------------------

/**
 * 【1.0.0.45】会话缓存直服。
 *
 * 切片在 spawn 时由 handleRequest 从 ProxyCache 快照出来（mutex 保护），
 * 这里按 runLocal 同款节拍（256KB / 高水位背压）喂进 m_buf，交付完
 * m_done。cache short = 快照与承诺范围不符（理论只可能发生在缓存被 trim 后
 * 的竞态）→ m_failed，由 onPump 关连接让播放器重请求（不会再 abort 别人）。
 */
void ProxyStreamWorker::runCache()
{
    const int kChunk = 256 * 1024;   // 与 runLocal 同款块大小
    qint64 pos = m_start;
    while (!m_abort && pos <= m_end) {
        {
            QMutexLocker lk(&m_mutex);
            while (!m_abort && m_buf.size() > kProxyHighWater) {
                m_cond.wait(&m_mutex, 200);
            }
            if (m_abort) break;
        }
        const qint64 off = pos - m_cacheSliceStart;
        if (off < 0 || off >= (qint64)m_cacheSlice.size()) break;
        qint64 want = (qint64)m_cacheSlice.size() - off;
        if (want > kChunk) want = kChunk;
        if (want > m_end - pos + 1) want = m_end - pos + 1;
        const QByteArray chunk = m_cacheSlice.mid((int)off, (int)want);
        {
            QMutexLocker lk(&m_mutex);
            if (m_abort) break;
            m_buf.append(chunk);
            m_cond.wakeAll();
        }
        pos += chunk.size();
    }

    QMutexLocker lk(&m_mutex);
    if (m_abort) {
        m_done = true;
        m_cond.wakeAll();
        return;
    }
    if (pos <= m_end) {
        m_failed = true;
        m_err = "cache short";
    }
    m_done = true;
    m_cond.wakeAll();
    qDebug("BiliClient proxy worker CACHE done this=%p start=%lld end=%lld pos=%lld",
           (void*)this, (long long)m_start, (long long)m_end, (long long)pos);
}

VideoProxy::VideoProxy(QObject* parent)
    : QObject(parent)
    , m_server(0)
    , m_pump(0)
    , m_total(0)
    , m_reported(false)
    , m_delivered(0)
    , m_complete(false)
    , m_sessionBytes(0)
{
}

VideoProxy::~VideoProxy()
{
    stop();
}

QString VideoProxy::start(const QString& sourceUrl, qint64 totalSize)
{
    return start(sourceUrl, totalSize, QStringList());
}

QString VideoProxy::start(const QString& sourceUrl, qint64 totalSize, const QStringList& alts)
{
    stop();
    if (sourceUrl.isEmpty()) return QString();

    m_sourceUrl = sourceUrl;
    m_total = totalSize;
    m_alts = alts;
    m_reported = false;
    // 【1.0.0.24】一次 start = 一次播放会话：EOF 判定从零开始。
    m_delivered = 0;
    m_complete = false;
    m_sessionBytes = 0;
    if (!sourceUrl.startsWith("file://")) {
        // CDN 源：重置会话缓存（file:// 源是磁盘读，不需要缓存）。
        QMutexLocker lk(&m_cache.mutex);
        m_cache.buf.clear();
        m_cache.start = 0;
    }

    m_server = new QTcpServer(this);
    // 监听所有地址：BB10 的 mmrenderer 媒体服务跑在独立进程，
    // 可能连不上 app 进程的 loopback 127.0.0.1，改成 Any 后外部进程
    //（包括 mmrenderer）才能连上本端口。端口仍随机分配，只在本机。
    if (!m_server->listen(QHostAddress::Any, 0)) {
        qDebug("BiliClient proxy listen failed: %s", qPrintable(m_server->errorString()));
        delete m_server;
        m_server = 0;
        return QString();
    }
    QObject::connect(m_server, SIGNAL(newConnection()), this, SLOT(onNewConnection()));

    if (!m_pump) {
        m_pump = new QTimer(this);
        m_pump->setInterval(20);
        QObject::connect(m_pump, SIGNAL(timeout()), this, SLOT(onPump()));
    }

    const quint16 port = m_server->serverPort();
    qDebug("BiliClient proxy listening 127.0.0.1:%d", (int)port);
    playTrace(QString("PROXY start size=%1 urlLen=%2 alts=%3")
              .arg((long long)totalSize).arg(sourceUrl.size()).arg(alts.size()));
    return QString("http://127.0.0.1:%1/v.mp4").arg((int)port);
}

void VideoProxy::stop()
{
    if (m_pump) m_pump->stop();
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = 0;
    }
    // 先摘链再逐条收尸：worker 的 finished() 回来时 findConnByWorker 找不到
    // → 走孤儿路径 deleteLater，绝不会解引用已删除的 conn。
    QList<ProxyConn*> conns;
    conns.swap(m_conns);
    for (int i = 0; i < conns.size(); ++i) {
        ProxyConn* c = conns.at(i);
        if (c->worker) {
            if (c->workerDone) {
                c->worker->deleteLater();   // 线程已结束，安全收尸
            } else {
                c->worker->abortNow();      // 在飞：非阻塞中止，finished() 后孤儿删除
            }
            c->worker = 0;
        }
        if (c->sock) {
            c->sock->disconnect(this);
            c->sock->abort();
            c->sock->deleteLater();
        }
        delete c;
    }
    // 缓存活着（value 成员），只清内容：中止中的 worker 还可能往里 append，
    // 清空后它会按"洞"自愈重建，不会写已释放内存。
    {
        QMutexLocker lk(&m_cache.mutex);
        m_cache.buf.clear();
        m_cache.start = 0;
    }
}

/**
 * 【抖音客户端】覆盖拉 CDN 用的 UA / Referer。
 *
 * 必须在 start() 之前调用 —— start() 会把当时的 m_ua/m_referer 传给新建的 worker。
 * 传空串则回落到 B 站默认值（BiliBar / BerryLi 行为不变）。
 */
void VideoProxy::setHeaders(const QString& ua, const QString& referer)
{
    m_ua = ua;
    m_referer = referer;
    qDebug("BiliClient proxy headers set ua=%d ref=%s",
           ua.size(), qPrintable(referer));
}

void VideoProxy::closeConn(ProxyConn* c)
{
    if (!c) return;
    const int idx = m_conns.indexOf(c);
    if (idx >= 0) m_conns.removeAt(idx);
    if (c->worker) {
        if (c->workerDone) {
            // 线程已结束（数据即使没排空也没人要了：连接要关）：安全收尸。
            c->worker->deleteLater();
        } else {
            // 在飞：非阻塞中止。finished() 回来时 findConnByWorker 找不到 →
            // 孤儿路径 deleteLater，绝不 wait()（主线程卡 2 秒 = 播放卡死）。
            c->worker->abortNow();
        }
        c->worker = 0;
    }
    if (c->sock) {
        c->sock->disconnect(this);   // 先断信号，避免 abort() 触发的 disconnected 又打回来
        c->sock->abort();
        c->sock->deleteLater();
    }
    delete c;
}

VideoProxy::ProxyConn* VideoProxy::findConn(QTcpSocket* s)
{
    if (!s) return 0;
    for (int i = 0; i < m_conns.size(); ++i) {
        if (m_conns.at(i)->sock == s) return m_conns.at(i);
    }
    return 0;
}

VideoProxy::ProxyConn* VideoProxy::findConnByWorker(ProxyStreamWorker* w)
{
    if (!w) return 0;
    for (int i = 0; i < m_conns.size(); ++i) {
        if (m_conns.at(i)->worker == w) return m_conns.at(i);
    }
    return 0;
}

void VideoProxy::onWorkerFinished()
{
    // 老 worker（连接已被 closeConn 摘链）跑完也会进来：只删自己，
    // 绝不能动别人的 conn / m_pump。
    // ProxyStreamWorker 没有 Q_OBJECT 宏（只是 QThread 子类），qobject_cast 会退化成
    // 对 QThread::staticMetaObject 的判断，既不可靠也没必要。连接是本类自己建的，
    // sender() 必然是 ProxyStreamWorker*，用 static_cast 即可。
    if (!sender()) return;
    ProxyStreamWorker* w = static_cast<ProxyStreamWorker*>(sender());
    ProxyConn* c = findConnByWorker(w);
    qDebug("BiliClient proxy worker FINISHED this=%p conn=%p buffered=%d",
           (void*)w, (void*)c, w->buffered());

    if (!c) {
        // 孤儿（连接已关/回收）：线程已结束，直接收尸。
        w->deleteLater();
        return;
    }
    // 【1.0.0.45 血泪修复】线程结束 ≠ 交付完成。旧代码在这里
    // m_worker=0 + m_pump->stop() + m_client->disconnectFromHost()，
    // 于是 m_buf 里还没被泵走的字节（高水位内最多 2MB）直接蒸发，
    // 播放器收到 Content-Length 比实际短的流 → 卡死 / "提前断流" /
    // 反复重请求 → 慢且播不到头。现在只打标记，由 onPump 排空后才关连接。
    c->workerDone = true;
}

void VideoProxy::onNewConnection()
{
    if (!m_server) return;
    QTcpSocket* s = m_server->nextPendingConnection();
    if (!s) return;

    // 【1.0.0.45 血泪修复】并发连接是 libbbplayer 的正常行为：每个 Range 请求
    // 一条 TCP 连接，还会在上一条还没送完时就开下一条（流水线）。旧代码在这里
    // dropClient()+abortStream() 把在飞的流整条杀掉 → 播放器只收到截断的
    // Range → 按真实位置重请求（常常直接要"当前位置到文件尾"）→ 又被下一条
    // 新连接 abort → 重传风暴。真机 1.0.0.44 dyplay.log：同一条
    // range=0-524287 被请求 5 次、1310720-1346323 被请求 4 次。
    // 现在：保留所有在飞连接，互不 abort；只在超过并发上限时回收空闲连接。
    static const int kMaxConns = 8;
    if (m_conns.size() >= kMaxConns) {
        for (int i = 0; i < m_conns.size(); ++i) {
            ProxyConn* c = m_conns.at(i);
            if (!c->worker) { closeConn(c); break; }   // 空闲/已排空的先回收
        }
        if (m_conns.size() >= kMaxConns) closeConn(m_conns.first());
    }

    ProxyConn* c = new ProxyConn();
    c->sock = s;
    m_conns.append(c);
    QObject::connect(s, SIGNAL(readyRead()), this, SLOT(onReadyRead()));
    QObject::connect(s, SIGNAL(disconnected()), this, SLOT(onClientGone()));
    playTrace(QString("PROXY conn open n=%1").arg(m_conns.size()));
}

void VideoProxy::onClientGone()
{
    QTcpSocket* s = qobject_cast<QTcpSocket*>(sender());
    ProxyConn* c = findConn(s);
    if (c) closeConn(c);   // 播放器关了这条连接：它的流没人要了，回收
    if (m_conns.isEmpty() && m_pump) m_pump->stop();
}

void VideoProxy::onReadyRead()
{
    QTcpSocket* s = qobject_cast<QTcpSocket*>(sender());
    ProxyConn* c = findConn(s);
    if (!c) { if (s) s->readAll(); return; }
    if (c->headersSent) { s->readAll(); return; }   // 一条连接只服务一个请求（Connection: close）

    c->reqBuf.append(s->readAll());
    const int end = c->reqBuf.indexOf("\r\n\r\n");
    if (end < 0) {
        if (c->reqBuf.size() > 8192) {   // 不是 HTTP 请求，别陪它耗
            qDebug("BiliClient proxy got garbage, dropping client");
            closeConn(c);
        }
        return;
    }

    const QByteArray head = c->reqBuf.left(end);
    c->reqBuf.clear();
    handleRequest(c, head);
}

void VideoProxy::handleRequest(ProxyConn* c, const QByteArray& head)
{
    const QList<QByteArray> lines = head.split('\n');

    // 第一行："GET /v.mp4 HTTP/1.1"
    QByteArray method = "GET";
    if (!lines.isEmpty()) {
        const QList<QByteArray> first = lines.first().trimmed().split(' ');
        if (!first.isEmpty()) method = first.first().toUpper();
    }

    qint64 start = 0;
    qint64 end = m_total > 0 ? (m_total - 1) : -1;
    bool hasRange = false;

    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        if (!line.toLower().startsWith("range:")) continue;
        QByteArray v = line.mid(6).trimmed();
        if (!v.startsWith("bytes=")) continue;
        v = v.mid(6);
        const int dash = v.indexOf('-');
        if (dash < 0) continue;
        const QByteArray a = v.left(dash).trimmed();
        const QByteArray b = v.mid(dash + 1).trimmed();
        if (!a.isEmpty()) start = a.toLongLong();
        if (!b.isEmpty()) end = b.toLongLong();
        hasRange = true;
    }

    // 越界保护：播放器偶尔会请求 start == total 的位置
    if (m_total > 0) {
        if (start < 0 || start >= m_total) start = 0;
        if (end < 0 || end >= m_total) end = m_total - 1;
    }
    if (end >= 0 && end < start) end = m_total > 0 ? m_total - 1 : -1;

    // 【1.0.0.24】从头开始的请求 = 新的一轮播放（换片重新 start 已复位过一次，
    // 这里兜住循环重播：同一代理会话上播放器再次请求 range 0-）。
    // 不复位的话，第二轮的累计会把 m_complete 一直卡在 true，EOF 事件不再触发。
    if (start == 0) {
        m_delivered = 0;
        m_complete = false;
    }

    QByteArray resp;
    if (hasRange && m_total > 0) {
        resp += "HTTP/1.1 206 Partial Content\r\n";
    } else {
        resp += "HTTP/1.1 200 OK\r\n";
    }
    resp += "Content-Type: video/mp4\r\n";
    resp += "Accept-Ranges: bytes\r\n";
    if (m_total > 0) {
        resp += "Content-Length: " + QByteArray::number(end - start + 1) + "\r\n";
        if (hasRange) {
            resp += "Content-Range: bytes " + QByteArray::number(start) + "-" +
                    QByteArray::number(end) + "/" + QByteArray::number(m_total) + "\r\n";
        }
    }
    // HEAD 也要返回完整响应头（Content-Length / Accept-Ranges / Content-Range），
    // 否则 MediaPlayer/mmrenderer 先发 HEAD 探测，收到空响应或连接重置就会放弃，
    // 表现为"正在连接视频"永远不播。GET 再发正文。
    resp += "Connection: close\r\n\r\n";

    c->sock->write(resp);
    c->sock->flush();
    c->headersSent = true;
    c->expectedLen = (m_total > 0) ? (end - start + 1) : -1;
    c->sentBytes = 0;
    c->reqStart = start;
    c->reqEnd = end;

    qDebug("BiliClient proxy %s range %lld-%lld of %lld",
           method.constData(), (long long)start, (long long)end, (long long)m_total);
    playTrace(QString("PROXY req %1 range=%2-%3 of %4")
              .arg(QString::fromLatin1(method.constData()))
              .arg((long long)start).arg((long long)end).arg((long long)m_total));

    // HEAD 只问大小，不要正文：头已发完，直接断开
    if (method == "HEAD") {
        c->sock->disconnectFromHost();
        return;
    }

    // 取数源优先级：【1.0.0.45】会话缓存命中 > CDN 直链 / 本地预取文件。
    ProxyStreamWorker* w = 0;
    if (m_total > 0 && !m_sourceUrl.startsWith("file://")) {
        QMutexLocker lk(&m_cache.mutex);
        const qint64 cacheEnd = m_cache.start + m_cache.buf.size();
        if (start >= m_cache.start && end < cacheEnd && cacheEnd > m_cache.start) {
            // 整段都在缓存里：快照一份直服，不碰网络。
            const QByteArray slice = m_cache.buf.mid((int)(start - m_cache.start),
                                                     (int)(end - start + 1));
            w = new ProxyStreamWorker(m_sourceUrl, start, end, m_ua, m_referer,
                                      QStringList(), &m_cache, slice, start);
            playTrace(QString("PROXY cache hit range=%1-%2 of %3")
                      .arg((long long)start).arg((long long)end).arg((long long)m_total));
        }
    }
    if (!w) {
        w = new ProxyStreamWorker(m_sourceUrl, start, m_total > 0 ? end : -1,
                                  m_ua, m_referer, m_alts, &m_cache);
    }
    c->worker = w;
    // 【1.0.0.12】worker 不许自毁。
    // 旧写法 finished() → worker->deleteLater()，但 m_worker 是裸指针：curl 把
    // Range 跑完 → worker 被释放 → 紧接着 onPump() / onClientGone→abortStream()
    // 拿 m_worker 解引用，QMutexLocker(&w->m_mutex) 读到被复用的堆内存
    // （真机 SIGSEGV @ QMutexLocker+0x9, ref=0x29）。
    // 改由 VideoProxy 统一收尸，且收尸槽里【先摘指针再 deleteLater】。
    QObject::connect(w, SIGNAL(finished()),
                     this, SLOT(onWorkerFinished()));
    qDebug("BiliClient proxy worker START this=%p", (void*)w);
    w->start();
    m_pump->start();
}

void VideoProxy::onPump()
{
    if (m_conns.isEmpty()) {
        if (m_pump) m_pump->stop();
        return;
    }

    // 拷贝一份遍历：本轮循环里 closeConn 会改 m_conns。
    const QList<ProxyConn*> conns = m_conns;
    for (int i = 0; i < conns.size(); ++i) {
        ProxyConn* c = conns.at(i);
        if (m_conns.indexOf(c) < 0) continue;   // 本轮已被回收

        // 先看 socket 自己的写缓冲：堆到 1MB 就暂停取新数据，
        // 让 worker 那边的写回调撞上高水位自然停住 —— 这就是整条链路的背压。
        if (c->worker && c->sock && c->sock->bytesToWrite() < 1024 * 1024) {
            const QByteArray chunk = c->worker->take();
            if (!chunk.isEmpty()) {
                c->sock->write(chunk);
                c->sentBytes += chunk.size();
                m_delivered += chunk.size();
                m_sessionBytes += chunk.size();
                // 【播放链路硬证据】第一次把视频字节写进这条 socket 时打一行。
                // 落 data/bbplayer.log + dyplay.log，供无人值守自检读取。
                if (c->sentBytes == (qint64)chunk.size()) {
                    qDebug("BiliClient proxy FIRST video bytes delivered=%lld",
                           (long long)c->sentBytes);
                    playTrace(QString("PROXY firstbytes=%1").arg((long long)c->sentBytes));
                }
            }
        }

        // worker 失败（且线程已结束）：只有"这条连接一个正文字节都没送出去"
        // 才值得让上层回退整段下载；送出去过的，播放器自己会重请求
        // （1.0.0.45 起重请求不再 abort 别人，重试成本很低）。
        if (c->worker && c->workerDone && c->worker->hasFailed()) {
            if (c->sentBytes == 0 && !m_reported) {
                m_reported = true;
                qDebug("BiliClient proxy stream failed conn=%p why=%s",
                       (void*)c, qPrintable(c->worker->failReason()));
                emit streamFailed(c->worker->failReason());
            }
            closeConn(c);
            continue;
        }

        // 排空完成 = 线程结束 + 缓冲取空 + socket 写完。
        // 【1.0.0.45】旧代码在 onWorkerFinished 里直接停泵断连，这里剩下的
        // m_buf（最多 2MB）会蒸发 → 播放器拿到截断流 → 卡死/重传风暴。
        if (c->worker && c->workerDone && c->worker->buffered() == 0 &&
            (!c->sock || c->sock->bytesToWrite() == 0)) {
            if (c->expectedLen > 0 && c->sentBytes < c->expectedLen) {
                // 流比承诺短（CDN 提前断 / 缓存快照不符）：关连接让播放器按真实
                // 位置重请求。不再弹"提前断流"错误——重请求就能 recover。
                qDebug("BiliClient proxy short stream conn=%p %lld/%lld, retryable",
                       (void*)c, (long long)c->sentBytes, (long long)c->expectedLen);
            } else if (m_total > 0 && c->reqEnd >= m_total - 1 && !m_complete) {
                // 这条连接交付的是文件尾段且如数交付 → 整段视频都已交给播放器
                // （EOF 硬事实）。循环播放的"播完"判定以此为主证据之一。
                m_complete = true;
                qDebug("BiliClient proxy stream complete pos=%lld of %lld",
                       (long long)(c->reqEnd + 1), (long long)m_total);
                playTrace(QString("PROXY eof delivered=%1 of %2")
                          .arg((long long)(c->reqEnd + 1)).arg((long long)m_total));
                emit streamComplete();
            }
            closeConn(c);
            continue;
        }

        // 只有头没有正文的请求（HEAD）：头发完就关。
        if (!c->worker && c->headersSent &&
            (!c->sock || c->sock->bytesToWrite() == 0)) {
            closeConn(c);
            continue;
        }

        // 兜底：socket 已断开但没等到 disconnected 信号（异常路径）。
        if (c->sock && c->sock->state() == QAbstractSocket::UnconnectedState) {
            closeConn(c);
            continue;
        }
    }

    if (m_conns.isEmpty() && m_pump) m_pump->stop();
}



// ============================================================================
// 【v85】互动：点赞 / 投币 / 收藏 / 关注 / 分享
//
// 这四个写操作都是 **POST 表单 + csrf**，csr 就是 cookie 里的 bili_jct。
// 少了 csrf 一律返回 -111「csrf 校验失败」，这也是"点了没反应"最常见的原因。
// ============================================================================

/** 拼 application/x-www-form-urlencoded 的 body（值统一 percentEncode）。 */
static QString formBody(const QList<QPair<QString, QString> >& kv)
{
    QStringList out;
    for (int i = 0; i < kv.size(); ++i) {
        out << DouyinApi::percentEncode(kv.at(i).first)
               + QLatin1Char('=')
               + DouyinApi::percentEncode(kv.at(i).second);
    }
    // 【坑】Qt 4.8 的 QStringList 只有 join(const QString&)，没有 join(QChar)，
    // 传 QLatin1Char 会直接编译失败。
    return out.join(QString::fromLatin1("&"));
}

/** 操作名的中文说法，用于错误提示。 */
static QString interactName(const QString& suffix)
{
    if (suffix == QLatin1String(":like"))      return QString::fromUtf8("点赞");
    if (suffix == QLatin1String(":coin"))      return QString::fromUtf8("投币");
    if (suffix == QLatin1String(":favdeal"))   return QString::fromUtf8("收藏");
    if (suffix == QLatin1String(":relation"))  return QString::fromUtf8("关注");
    return QString::fromUtf8("操作");
}

void VideoStore::setTip(const QString& t)
{
    m_interactTip = t;
    emit interactChanged();
    if (!m_tipTimer) {
        m_tipTimer = new QTimer(this);
        m_tipTimer->setSingleShot(true);
        QObject::connect(m_tipTimer, SIGNAL(timeout()), this, SLOT(clearTip()));
    }
    m_tipTimer->start(2500);
}

void VideoStore::clearTip()
{
    if (m_interactTip.isEmpty()) return;
    m_interactTip.clear();
    emit interactChanged();
}

void VideoStore::setCountText(const QString& key, qint64 n)
{
    m_detail.insert(key, DouyinApi::countText(n));
    emit detailChanged();
}

/** 统一的登录/csrf 前置检查。返回空串表示可以继续，否则是给用户看的提示。 */
static QString interactGuard()
{
    if (!DouyinApi::instance()->loggedIn()) {
        return QString::fromUtf8("请先到设置页登录");
    }
    if (DouyinApi::instance()->csrf().isEmpty()) {
        return QString::fromUtf8("登录凭证不完整，请重新登录");
    }
    return QString();
}

void VideoStore::toggleLike()
{
    const QString bad = interactGuard();
    if (!bad.isEmpty()) { setTip(bad); return; }
    if (m_bvid.isEmpty()) return;

    QList<QPair<QString, QString> > kv;
    kv << qMakePair(QString::fromLatin1("bvid"), m_bvid);
    // 1 = 点赞，2 = 取消点赞
    kv << qMakePair(QString::fromLatin1("like"), QString::number(m_liked ? 2 : 1));
    kv << qMakePair(QString::fromLatin1("csrf"), DouyinApi::instance()->csrf());
    DouyinApi::instance()->post(QString::fromLatin1("https://api.bilibili.com/x/web-interface/archive/like"),
                              formBody(kv), m_tagPrefix + QLatin1String(":like"));
}

void VideoStore::addCoin()
{
    const QString bad = interactGuard();
    if (!bad.isEmpty()) { setTip(bad); return; }
    if (m_bvid.isEmpty()) return;
    if (m_coinGiven >= 2) { setTip(QString::fromUtf8("已经投过 2 枚了")); return; }

    QList<QPair<QString, QString> > kv;
    kv << qMakePair(QString::fromLatin1("bvid"), m_bvid);
    kv << qMakePair(QString::fromLatin1("multiply"), QString::fromLatin1("1"));
    // select_like=0：投币不顺带点赞，点赞由用户单独控制（否则会出现"我明明没点赞"）
    kv << qMakePair(QString::fromLatin1("select_like"), QString::fromLatin1("0"));
    kv << qMakePair(QString::fromLatin1("csrf"), DouyinApi::instance()->csrf());
    DouyinApi::instance()->post(QString::fromLatin1("https://api.bilibili.com/x/web-interface/coin/add"),
                              formBody(kv), m_tagPrefix + QLatin1String(":coin"));
}

void VideoStore::toggleFav()
{
    const QString bad = interactGuard();
    if (!bad.isEmpty()) { setTip(bad); return; }
    if (m_aid <= 0) return;

    // 收藏接口必须给 media_id（收藏夹 id），而我们只在上一次点收藏时才知道。
    // 所以第一次点要先拉一次收藏夹目录，拉回来再接着执行。
    if (m_favFolderIds.isEmpty()) {
        m_pendingFav = m_favored ? QString::fromLatin1("del") : QString::fromLatin1("add");
        fetchFavFolders();
        return;
    }
    doFavDeal(!m_favored);
}

void VideoStore::fetchFavFolders()
{
    const QString mid = DouyinApi::instance()->userMid();
    if (mid.isEmpty()) {
        setTip(QString::fromUtf8("还没拿到用户信息，请稍后重试"));
        m_pendingFav.clear();
        return;
    }
    DouyinApi::instance()->get(
        QString::fromLatin1("https://api.bilibili.com/x/v3/fav/folder/created/list-all?up_mid=")
            + mid + QLatin1String("&web_location=333.1387"),
        m_tagPrefix + QLatin1String(":favfolders"));
}

void VideoStore::doFavDeal(bool add)
{
    if (m_aid <= 0 || m_favFolderIds.isEmpty()) return;

    // 取消收藏时把**所有**收藏夹 id 都传过去：我们不知道它在哪个夹里，
    // 而从没放过的夹子传了也无害。这样比只传默认夹可靠得多。
    QString ids;
    for (int i = 0; i < m_favFolderIds.size(); ++i) {
        if (i > 0) ids += QLatin1Char(',');
        ids += m_favFolderIds.at(i);
    }

    QList<QPair<QString, QString> > kv;
    kv << qMakePair(QString::fromLatin1("rid"), QString::number(m_aid));
    kv << qMakePair(QString::fromLatin1("type"), QString::fromLatin1("2"));  // 2 = 视频
    kv << qMakePair(QString::fromLatin1(add ? "add_media_ids" : "del_media_ids"), ids);
    kv << qMakePair(QString::fromLatin1("csrf"), DouyinApi::instance()->csrf());
    DouyinApi::instance()->post(QString::fromLatin1("https://api.bilibili.com/x/v3/fav/resource/deal"),
                              formBody(kv), m_tagPrefix + QLatin1String(":favdeal"));
}

void VideoStore::toggleFollow()
{
    const QString bad = interactGuard();
    if (!bad.isEmpty()) { setTip(bad); return; }
    if (m_ownerMid.isEmpty()) { setTip(QString::fromUtf8("还没拿到 UP 主信息")); return; }

    QList<QPair<QString, QString> > kv;
    kv << qMakePair(QString::fromLatin1("fid"), m_ownerMid);
    // act: 1 = 关注，2 = 取关
    kv << qMakePair(QString::fromLatin1("act"), QString::number(m_followed ? 2 : 1));
    kv << qMakePair(QString::fromLatin1("re_src"), QString::fromLatin1("11"));
    kv << qMakePair(QString::fromLatin1("csrf"), DouyinApi::instance()->csrf());
    DouyinApi::instance()->post(QString::fromLatin1("https://api.bilibili.com/x/relation/modify"),
                              formBody(kv), m_tagPrefix + QLatin1String(":relation"));
}

void VideoStore::shareVideo()
{
    if (m_bvid.isEmpty()) return;
    const QString link = QString::fromLatin1("https://www.bilibili.com/video/") + m_bvid;
    bb::system::Clipboard cb;
    cb.clear();
    const bool ok = cb.insert(QString::fromLatin1("text/plain"), link.toUtf8());
    setTip(ok ? QString::fromUtf8("链接已复制到剪贴板") : QString::fromUtf8("复制失败"));
}

void VideoStore::onInteractReply(const QString& suffix, const QString& json, bool ok)
{
    int code = -1;
    QVariantMap data;
    if (ok && !json.isEmpty()) {
        JsonDataAccess jda;
        const QVariantMap root = jda.loadFromBuffer(json.toUtf8()).toMap();
        code = root.value("code").toInt();
        data = root.value("data").toMap();
    }

    // ---- 收藏夹目录（不是用户动作本身，是前置数据） ----
    if (suffix == QLatin1String(":favfolders")) {
        if (code != 0) {
            setTip(QString::fromUtf8("取收藏夹失败（%1）").arg(code));
            m_pendingFav.clear();
            return;
        }
        m_favFolderIds.clear();
        const QVariantList list = data.value("list").toList();
        for (int i = 0; i < list.size(); ++i) {
            const qint64 id = list.at(i).toMap().value("id").toLongLong();
            if (id > 0) m_favFolderIds << QString::number(id);
        }
        if (m_favFolderIds.isEmpty()) {
            setTip(QString::fromUtf8("没有可用的收藏夹"));
            m_pendingFav.clear();
            return;
        }
        if (!m_pendingFav.isEmpty()) {
            const bool add = (m_pendingFav == QLatin1String("add"));
            m_pendingFav.clear();
            doFavDeal(add);
        }
        return;
    }

    if (code != 0) {
        // 收藏失败很可能是因为缓存的收藏夹 id 过期了（换了账号 / 夹子被删），
        // 清掉让下次重新拉一遍目录，别让一次失败变成永久失败。
        if (suffix == QLatin1String(":favdeal")) m_favFolderIds.clear();
        // -101 未登录 / -111 csrf 失败 / 34005 超过限制，都是能一句话说清的
        QString why = QString::number(code);
        if (code == -101) why = QString::fromUtf8("未登录");
        else if (code == -111) why = QString::fromUtf8("csrf 校验失败，请重新登录");
        else if (code == 34005) why = QString::fromUtf8("超过操作上限");
        else if (code == 65004) why = QString::fromUtf8("已取消点赞");
        setTip(interactName(suffix) + QString::fromUtf8("失败（") + why + QString::fromUtf8("）"));
        return;
    }

    if (suffix == QLatin1String(":like")) {
        m_liked = !m_liked;
        m_likeCount += m_liked ? 1 : -1;
        if (m_likeCount < 0) m_likeCount = 0;
        setCountText(QString::fromLatin1("likeText"), m_likeCount);
        setTip(m_liked ? QString::fromUtf8("已点赞") : QString::fromUtf8("已取消点赞"));
    } else if (suffix == QLatin1String(":coin")) {
        m_coinGiven = qMin(2, m_coinGiven + 1);
        m_coinCount += 1;
        setCountText(QString::fromLatin1("coinText"), m_coinCount);
        setTip(m_coinGiven >= 2 ? QString::fromUtf8("已投满 2 枚硬币")
                                : QString::fromUtf8("已投币"));
    } else if (suffix == QLatin1String(":favdeal")) {
        m_favored = !m_favored;
        m_favCount += m_favored ? 1 : -1;
        if (m_favCount < 0) m_favCount = 0;
        setCountText(QString::fromLatin1("favText"), m_favCount);
        setTip(m_favored ? QString::fromUtf8("已收藏") : QString::fromUtf8("已取消收藏"));
    } else if (suffix == QLatin1String(":relation")) {
        m_followed = !m_followed;
        setTip(m_followed ? QString::fromUtf8("已关注") : QString::fromUtf8("已取消关注"));
    }
    emit interactChanged();
}
