#include "bbplayer_wrapper.hpp"

#include <dlfcn.h>
#include <stdlib.h>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QStringList>

BbPlayer::BbPlayer()
    : m_lib(0)
    , m_handle(0)
    , m_ok(false)
    , m_cbState(-1)
    , m_eventCount(0)
    , m_caInstalled(false)
    , m_caDone(false)
    , m_caAdds(0)
    , m_create(0), m_destroy(0), m_set_window(0), m_net_config(0)
    , m_set_video_enabled(0), m_open_url(0), m_open_file(0), m_play(0), m_pause(0)
    , m_close(0), m_seek(0), m_position(0), m_duration(0), m_state(0)
{
    // BB10 的动态加载器按 LD_LIBRARY_PATH（app/native/lib）找库；我们已把两个 .so 打进包里。
    m_lib = dlopen("libbbplayer.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!m_lib) {
        qDebug("BiliClient dlopen libbbplayer.so 失败: %s", dlerror());
        return;
    }
    // libbbnet.so 是 libbbplayer.so 的 NEEDED 依赖，dlopen 会自动级联加载；
    // 这里再显式加载一次（RTLD_GLOBAL）确保它的符号对 libbbplayer 可见。
    if (!dlopen("libbbnet.so", RTLD_LAZY | RTLD_GLOBAL)) {
        qDebug("BiliClient dlopen libbnet.so 失败: %s", dlerror());
    }
    resolve();
}

void BbPlayer::resolve()
{
    m_create            = (PFN_create)           dlsym(m_lib, "bbplayer_create");
    m_destroy           = (PFN_destroy)          dlsym(m_lib, "bbplayer_destroy");
    m_set_window        = (PFN_set_window)       dlsym(m_lib, "bbplayer_set_window");
    m_net_config        = (PFN_net_config)       dlsym(m_lib, "bbplayer_net_config");
    m_set_video_enabled = (PFN_set_video_enabled)dlsym(m_lib, "bbplayer_set_video_enabled");
    m_open_url          = (PFN_open_url)         dlsym(m_lib, "bbplayer_open_url");
    // 【v89】可选符号：本地文件播放用。拿不到就退回 open_url（不列入 m_ok 判定，
    // 免得某个库版本缺这个符号时整个播放器都初始化失败）。
    m_open_file         = (PFN_open_file)        dlsym(m_lib, "bbplayer_open_file");
    m_play              = (PFN_play)             dlsym(m_lib, "bbplayer_play");
    m_pause             = (PFN_pause)            dlsym(m_lib, "bbplayer_pause");
    m_close             = (PFN_close)            dlsym(m_lib, "bbplayer_close");
    m_seek              = (PFN_seek)             dlsym(m_lib, "bbplayer_seek");
    m_position          = (PFN_position)         dlsym(m_lib, "bbplayer_position");
    m_duration          = (PFN_duration)         dlsym(m_lib, "bbplayer_duration");
    m_state             = (PFN_state)            dlsym(m_lib, "bbplayer_state");

    m_ok = m_create && m_destroy && m_set_window && m_net_config
        && m_set_video_enabled && m_open_url && m_play && m_pause && m_close
        && m_seek && m_position && m_duration && m_state;
    if (!m_ok) {
        qDebug("BiliClient libbbplayer.so 符号解析不完整");
    }
}

BbPlayer::~BbPlayer()
{
    destroy();
    if (m_lib) dlclose(m_lib);
}

bool BbPlayer::installCaBundle(bool force)
{
    // 【v63 关键性能修复】装成功一次就永不再做。
    //
    // 这个函数要干的事很重：查 4 个候选路径 → QFile::remove + QFile::copy 复制 PEM 到
    // 沙盒 → dlopen libbbnet → bbnet_global_init → bbnet_ca_add_file（解析 121 张 X509）
    // → bbnet_ca_add_system（再解析 125 张）。在 BB10 的 CPU 上是几百毫秒级的开销。
    //
    // v62 及之前，同一次 ensurePlayer 里它会跑 2~3 遍：
    //   1) prepareCa()
    //   2) create() 里（而且 create() 还把 m_caAdds 重置成 0，把次数上限也废掉了）
    //   3) openUrl() 里
    // 这直接导致"从条目点进播放页"要卡好几秒 —— 而且这三遍全发生在 nav.push() 之前，
    // 用户连转场动画都看不到，只觉得点下去没反应。
    //
    // 更关键的是：播放默认已经把 https 降级成 http（见 VideoPlayer::play），
    // 这条路根本不做 TLS，装根证书纯属白烧 CPU。所以现在改成**只在真要打开 https
    // 时才装**（见 openUrl），并且装上之后不再重复。
    if (m_caDone) return m_caInstalled;
    if (m_caInstalled && !force) return true;
    if (m_caAdds >= 2) return m_caInstalled;   // 最多试 2 次，防止反复追加撑爆全局缓冲
    ++m_caAdds;

    // 1) 找随包分发的 cacert.pem。
    //    打包后位置是 app/native/assets/cacert.pem（bar-descriptor 里
    //    <asset path="o.le-v7/assets">assets</asset>），而 BB10 应用的
    //    QDir::currentPath() 是沙盒根，故首选 cwd + "/app/native/assets"。
    //    多列几个候选，避免不同固件/启动方式下 cwd 不同导致找不到。
    const QString cwd = QDir::currentPath();
    QStringList cands;
    cands << cwd + "/app/native/assets/cacert.pem"
          << cwd + "/assets/cacert.pem"
          << QDir::homePath() + "/cacert.pem"
          << cwd + "/cacert.pem";
    QString src;
    for (int i = 0; i < cands.size(); ++i) {
        if (QFile::exists(cands.at(i))) { src = cands.at(i); break; }
    }
    if (src.isEmpty()) {
        m_caStatus = QString("fail: 找不到 cacert.pem (cwd=%1)").arg(cwd);
        qDebug("BiliClient CA: %s", m_caStatus.toUtf8().constData());
        return false;
    }

    // 2) 复制一份到沙盒根再交给 curl。
    //    原因：app/native 是只读挂载，个别固件下 curl 以不同 uid 打开会报
    //    "access rights"；复制到可写沙盒后权限一定没问题。
    const QString dst = QDir::homePath() + "/cacert.pem";
    QString use = src;
    if (src != dst) {
        QFile::remove(dst);
        if (QFile::copy(src, dst)) use = dst;
    }

    // 3) 调 libbbnet 的全局 CA 装载接口。
    void* net = dlopen("libbbnet.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!net) net = dlopen("app/native/lib/libbbnet.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!net) {
        m_caStatus = "fail: dlopen libbbnet.so 失败";
        qDebug("BiliClient CA: %s", m_caStatus.toUtf8().constData());
        return false;
    }
    typedef int (*PFN_ca_add_file)(const char*);
    PFN_ca_add_file add = (PFN_ca_add_file)dlsym(net, "bbnet_ca_add_file");
    if (!add) {
        m_caStatus = "fail: 无 bbnet_ca_add_file 符号";
        qDebug("BiliClient CA: %s", m_caStatus.toUtf8().constData());
        return false;
    }
    // 保险起见先把 curl 全局态初始化好（幂等），避免往未初始化的 store 里写。
    typedef int (*PFN_global_init)(void);
    PFN_global_init ginit = (PFN_global_init)dlsym(net, "bbnet_global_init");
    if (ginit) ginit();

    QByteArray p = use.toUtf8();
    // 【v54 关键】把路径记下来 —— 这是 bbplayer_net_config 第三参数要用的值。
    // 之前只把 PEM 内容喂给 bbnet_ca_add_file 就以为完事了，其实那只是
    // "给 pinning 用的内存 store"，真正让 curl 句柄信任这些根证书的是
    // CURLOPT_CAINFO，而它的值只能通过 net_config 第三参数写进全局 config+16。
    m_caPath = use;
    const int rc = add(p.constData());
    // 【v52 关键修正】返回值不是错误码，而是**成功加入的证书张数**！
    // 反汇编 bbnet_ca_add_pem(0x3583c)：结尾 `mov r0, r7`，r7 = 解析出的证书数；
    // 只有入参为空 / 解析失败 / malloc 失败才返回 -1。
    // v51 误判成"rc != 0 即失败"，其实 121 正是这个包里的证书张数（grep 数过 = 121）。
    //
    // 另外：`bbnet_ca_add_file` 只把 PEM 解析进【全局内存缓冲】，
    // 它**不会**写 CURLOPT_CAINFO 的路径（反汇编 0x358cc：读完文件直接调
    // bbnet_ca_add_pem，没有任何存路径的动作）。所以系统 CA 库也顺手装一次，
    // 两条来源都备着（v51 实测 bbnet_ca_add_system 返回 125，说明系统库也可用）。
    typedef int (*PFN_ca_add_system)(void);
    PFN_ca_add_system sys = (PFN_ca_add_system)dlsym(net, "bbnet_ca_add_system");
    const int rc2 = sys ? sys() : -999;

    if (rc > 0) {
        m_caInstalled = true;
        m_caDone = true;
        m_caStatus = QString("ok %1certs sys=%2 %3").arg(rc).arg(rc2).arg(use);
    } else if (rc2 >= 0) {
        m_caInstalled = true;
        m_caDone = true;
        m_caStatus = QString("ok(sys %1certs) file rc=%2").arg(rc2).arg(rc);
    } else {
        m_caStatus = QString("fail rc=%1 sys=%2 %3").arg(rc).arg(rc2).arg(use);
    }
    qDebug("BiliClient CA: %s", m_caStatus.toUtf8().constData());

    // 【v55】环境变量兜底 —— 这是绕开"参数语义之争"的最后一道保险。
    // libcurl 在 CURLOPT_CAINFO 没有被显式设置时，会依次回退读
    // CURL_CA_BUNDLE / SSL_CERT_FILE 指定的 CA 文件。libbbnet 里 reader 建句柄
    // 时是从全局 config+16 取 CAINFO 的（0x36d2e），万一那条路没通（快照时机、
    // 槽位语义不符等），curl 就会退到默认路径去找 CA —— 在 QNX 上那个默认路径
    // 通常不存在，于是报 "Problem with the SSL CA cert"。
    // 把环境变量指到我们这份 121 张根证书的 pem 上，两条路总有一条能通。
    if (!m_caPath.isEmpty()) {
        const QByteArray ep = m_caPath.toUtf8();
        ::setenv("CURL_CA_BUNDLE", ep.constData(), 1);
        ::setenv("SSL_CERT_FILE",  ep.constData(), 1);
        m_caStatus += " env=1";
        qDebug("BiliClient CA env: CURL_CA_BUNDLE=%s", ep.constData());
    }
    return m_caInstalled;
}

bool BbPlayer::create()
{
    if (!m_ok) return false;
    if (m_handle) destroy();
    {
        QMutexLocker lock(&m_mutex);
        m_log.clear();
        m_lastError.clear();
        m_cbState = -1;
        m_eventCount = 0;
    }
    // 【v48 关键】第一个参数是事件回调，第二个是 userdata —— 不是保留参数。
    // 反汇编 bbplayer_create(0x58ca8)：`str r0,[r4,#0]; str r1,[r4,#4]`，之后所有
    // 回调派发（跳板 0x55a34）都读句柄的 [0]/[4] 两个字段。v43~v47 传 (0,0)
    // 导致回调为 NULL，libbbplayer 的错误原文与状态事件全被丢弃。
    // 【v55 时序 / v63 精简】网络配置必须在 m_create() 之前写好：
    // bbplayer_net_config 只是把 UA/Referer/CA路径 写进 libbbplayer 的 .bss 全局槽，
    // 真正读这份快照的是 create/open 流程；create 之后再设等于白设（v54 真机实证）。
    //
    // 【v63】这里不再调 installCaBundle —— 理由见 installCaBundle 开头的注释：
    // 播放默认走 http，不需要 TLS 根证书，而那一步是页面打开卡顿的主因。
    // CA 改为只在 openUrl 遇到 https 时才按需装载（幂等，装上后不再重复）。
    // applyNetConfig 用当前的 m_caPath（可能为空）写快照，libbbplayer 对空串会
    // 跳过第三槽、沿用内置默认，不影响 UA/Referer。
    applyNetConfig();

    m_handle = m_create((void*)&BbPlayer::eventTrampoline, this);
    if (!m_handle) return false;

    // create 之后再写一次：万一快照发生在 create 内部、或 open_url 时才读，
    // 这次也能覆盖到。applyNetConfig 幂等，重复调用无副作用。
    applyNetConfig();
    return true;
}

void BbPlayer::eventTrampoline(void* userdata, int type, int a, int b,
                               int c, int d, const char* msg)
{
    BbPlayer* self = static_cast<BbPlayer*>(userdata);
    if (self) self->onEvent(type, a, b, c, d, msg);
}

void BbPlayer::onEvent(int type, int a, int b, int c, int d, const char* msg)
{
    Q_UNUSED(b);
    Q_UNUSED(c);
    Q_UNUSED(d);

    // 【v64 关键性能修复 —— 声音一卡一下的元凶】
    //
    // 本回调运行在 **libbbplayer 自己的线程**里，很可能是时间敏感的喂数据/音频线程。
    // v60 真机日志里能看到 type=3 是库内部的周期性事件，每 ~250ms 一条
    // （EV 段：t3 a2783 / t3 a3036 / t3 a3289 / t3 a3542，a 值等差 253）。
    // 以前每一条都要走完整流程：
    //     QString 拼接分配 → m_log 追加 → 超过 3000 字符时 m_log.right(3000) 整段拷贝
    // 等于在播放线程上每 250ms 做一次堆分配 + 3000 字符拷贝，节奏和"声音一卡一下"
    // 完全对上（尤其 v63 把画面接通后，线程本身负载更高，这点抖动就足以让音频欠载）。
    //
    // 调试叠加层在 v63 已经移除，这类周期性事件没有任何诊断价值，直接跳过：
    // 只保留真正的状态变更(1)、错误(7)、日志(8)。
    QMutexLocker lock(&m_mutex);
    ++m_eventCount;

    if (type != 1 && type != 7 && type != 8) return;   // 周期性事件：只计数，不建字符串

    // ⚠️ 只有 type=7(错误) / 8(日志) 的栈上第三个参数才是字符串指针。
    // 其它 type 的这个槽位可能是任意值（甚至非指针），绝不能拿去做 fromUtf8，
    // 否则会野指针崩溃 —— 这是接回调时最容易踩的坑。
    const bool hasMsg = (type == 7 || type == 8);
    // msg 由库 snprintf 到临时栈缓冲后传入，只在本次调用内有效 → 立刻深拷贝。
    QString m = (hasMsg && msg) ? QString::fromUtf8(msg) : QString();
    m = m.trimmed();
    if (m.size() > 200) m = m.left(200);

    // 单条事件太长会把界面撑爆，统一压成一行
    m = m.replace('\n', ' ').replace('\r', ' ');
    QString line = QString("t%1 a%2 %3").arg(type).arg(a).arg(m);
    if (!m_log.isEmpty()) m_log += QLatin1Char('\n');
    m_log += line;
    if (m_log.size() > 1200) m_log = m_log.right(1200);

    if (type == 1) m_cbState = a;
    if (type == 7) {
        m_lastError = QString("code=%1 %2").arg(a).arg(m);
    }
}

QString BbPlayer::eventLog() const
{
    QMutexLocker lock(&m_mutex);
    return m_log;
}

QString BbPlayer::lastError() const
{
    QMutexLocker lock(&m_mutex);
    return m_lastError;
}

int BbPlayer::callbackState() const
{
    QMutexLocker lock(&m_mutex);
    return m_cbState;
}

int BbPlayer::eventCount() const
{
    QMutexLocker lock(&m_mutex);
    return m_eventCount;
}

void BbPlayer::setWindow(const QString& groupId, const QString& windowId)
{
    if (!m_ok || !m_handle) return;
    // 【v62 关键】参数顺序：底层是 (handle, group, id)，组在前。
    // 顺序传反会导致 join 一个不存在的组 → 库内部销毁窗口 → 永远没有屏幕窗口。
    // 完整反汇编证据见 bbplayer_wrapper.hpp 里 setWindow 的注释。
    QByteArray gid = groupId.toUtf8();
    QByteArray wid = windowId.toUtf8();
    m_set_window(m_handle, gid.constData(), wid.constData());
}

void BbPlayer::applyNetConfig()
{
    if (!m_ok || !m_net_config) return;
    if (m_ua.isEmpty() && m_ref.isEmpty() && m_caPath.isEmpty()) return;
    QByteArray a = m_ua.toUtf8();
    QByteArray b = m_ref.toUtf8();
    QByteArray c = m_caPath.toUtf8();
    // 三参数、无 handle（见 hpp 文件头注释）。
    m_net_config(a.constData(), b.constData(), c.constData());
}

void BbPlayer::setNetConfig(const QString& ua, const QString& referer)
{
    // 【v55 关键】先缓存。libbbplayer 的 net_config 只是把三个字符串写进 .bss
    // 全局槽（0x67a1a6 / 0x67a3a6 / 0x679302），真正读它的是 create/open 流程。
    // 所以调用方应当在 create() 之前调本方法；create() 内部也会用缓存值再应用一次。
    //
    // 【v63】这里不再触发 installCaBundle —— CA 只在真要打开 https 时才装（见 openUrl）。
    // v62 之前这里会在 m_caPath 为空时装一遍，等于把几百毫秒的证书解析塞进了
    // 播放页的打开路径里。
    m_ua  = ua;
    m_ref = referer;
    applyNetConfig();
}

void BbPlayer::setVideoEnabled(bool on)
{
    if (!m_ok || !m_handle) return;
    m_set_video_enabled(m_handle, on ? 1 : 0);
}

bool BbPlayer::openUrl(const QString& url, int flags)
{
    if (!m_ok || !m_handle) return false;
    // 【v63】CA 只在真正要走 TLS 时才装。
    // 播放默认已经把 https 降级成 http（VideoPlayer::play），那条路不做 TLS，
    // 装 121 张包内 + 125 张系统根证书纯属浪费 CPU —— 而且它正是"点条目进播放页
    // 要等好几秒"的主因。这里保留按需路径，是为了 BILI_FORCE_HTTPS=1 时仍然可用。
    // installCaBundle 内部已改成"装成功一次就永不再做"，重复调用是零成本的。
    if (url.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)) {
        installCaBundle(false);
        applyNetConfig();   // 把刚拿到的 CA 路径写进全局快照（open 时才读）
    }
    QByteArray u = url.toUtf8();
    return m_open_url(m_handle, u.constData(), flags, 0) == 0;
}

/**
 * 【v89】打开本地文件。
 *
 * 【为什么不能直接 open_url】open_url 是给网络流准备的（内部会走 libbbnet/curl），
 * 传一个沙盒绝对路径进去大概率打不开。库里有独立的 bbplayer_open_file
 * （反汇编 0x58e6c：r0=handle, r1=path, r2=flags，返回 0/-1），这才是本地文件的入口。
 */
bool BbPlayer::openFile(const QString& path, int flags)
{
    if (!m_ok || !m_handle || path.isEmpty()) return false;
    if (!m_open_file) {
        // 老库没有这个符号：退回 open_url，至少不会比 v88 更差。
        return openUrl(path, flags);
    }
    QByteArray p = path.toUtf8();
    return m_open_file(m_handle, p.constData(), flags) == 0;
}

bool BbPlayer::play()
{
    return m_ok && m_handle && m_play(m_handle) == 0;
}

bool BbPlayer::pause()
{
    return m_ok && m_handle && m_pause(m_handle) == 0;
}

bool BbPlayer::close()
{
    return m_ok && m_handle && m_close(m_handle) == 0;
}

bool BbPlayer::seek(qint64 positionTicks, int flags)
{
    if (!m_ok || !m_handle) return false;
    return m_seek(m_handle, (long long)positionTicks, flags) == 0;
}

qint64 BbPlayer::positionTicks()
{
    if (!m_ok || !m_handle || !m_position) return 0;
    return (qint64)m_position(m_handle);
}

qint64 BbPlayer::durationTicks()
{
    if (!m_ok || !m_handle || !m_duration) return 0;
    return (qint64)m_duration(m_handle);
}

int BbPlayer::state()
{
    if (!m_ok || !m_handle || !m_state) return -1;
    return m_state(m_handle);
}

void BbPlayer::destroy()
{
    if (m_handle && m_ok) m_destroy(m_handle);
    m_handle = 0;
}
