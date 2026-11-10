#ifndef BBBPLAYER_WRAPPER_HPP
#define BBBPLAYER_WRAPPER_HPP

#include <QString>
#include <QMutex>

// 流式播放后端：直接复用 BBTube 自带的 libbbplayer.so（OpenMAX AL + 静态 FFmpeg +
// QNX 视频缓冲队列）+ libbbnet.so（curl 流式 reader）。整套库完全绕开系统
// bb::multimedia::MediaPlayer / mmrenderer，支持给 HTTP 请求注入 UA/Referer，
// 因此能直接吃 B 站 CDN 直链做"边下边播、不落盘"。
//
// 用 dlopen/dlsym 动态加载，不进链接期依赖 —— 既规避了 BB10 共享库"允许未解析符号"
// 导致设备上 dlopen 失败的坑，也能在库缺失时优雅降级（loaded() == false）。
//
// 下面这套 C 签名是从 libbbplayer.so 的 ARM 反汇编逐条核对出来的（已铁证）：
//   bbplayer_create(void* event_cb, void* userdata)             -> handle（5600 字节结构）
//       【v48 重大修正】前两个参数不是保留参数，而是 **事件回调函数指针 + userdata**！
//       反汇编 bbplayer_create(0x58ca8) 开头即 `str r0,[r4,#0]; str r1,[r4,#4]`，
//       把两个入参分别存进句柄的第 0 / 第 4 个字节。之后库里所有回调派发（通用跳板
//       0x55a34）都是 `ldr fn,[handle]; ldr ud,[handle,#4]; bx fn`。
//       v43~v47 一直传 (0,0) → 回调为 NULL → libbbplayer 的**错误消息与状态变更事件
//       全部被静默丢弃**，日志里一条 ERROR 都抓不到，state 只能靠轮询句柄字段猜。
//   bbplayer_destroy(void*)                                     -> void
//   bbplayer_set_window(void*, const char* id, const char* grp)  -> 0/-1
//   bbplayer_net_config(const char* ua, const char* ref, const char* ca) -> 0/-1
//       【v54 重大修正】**没有 handle 参数**，是全局配置接口，三个纯字符串！
//       反汇编 0x50220 逐条对照：入口 `mov r6,r0; mov r5,r1; mov r4,r2`，随后
//         if (r6) snprintf(槽A, 512, fmt, r6)   ← UA
//         if (r5) snprintf(槽B, 256, fmt, r5)   ← Referer
//         if (r4 && r4[0]) snprintf(槽C, 160, fmt, r4)  ← CA 证书文件路径
//       且三处都有 `cbz rX` 后改用内置默认串的分支，说明入参就是字符串指针。
//       v43~v53 一直写成 (handle, ua, ref, proxy) 四个参数 → handle 指针被当 UA
//       打印进 512 槽（UA 变二进制垃圾），ua 占用了 Referer 槽，ref 占用了 160 字节
//       的 CA 槽，真正的 proxy 落在栈上根本没被读。这解释了为什么
//       bbnet_ca_add_file 明明返回 121 张证书，HTTPS 依旧报
//       "Problem with the SSL CA cert" —— 因为 reader 建 curl 句柄时读的是
//       全局 config 的 +16 槽（CA 路径），而那个槽只有本接口的第三参数会写。
//       旁证：proxy 有独立导出接口 bbnet_set_proxy(0x35ab8)，CA 没有对应的
//       "set path" 接口（只有 ca_add_file/add_pem/add_system 三个"喂内容"的），
//       所以 CA 路径只能是本接口第三参数。
//   bbplayer_set_video_enabled(void*, int)                      -> 0/-1（create 默认不开启，必须显式置 1）
//   bbplayer_open_url(void*, const char* url, int flags, const char* type) -> 0/-1
//   bbplayer_play/pause/close(void*)                           -> 0/-1
//   bbplayer_seek(void*, int64_t position, int flags)          -> 0/-1
//       关键：position 是 64 位，占 r2,r3；flags 落在栈上；r1 是未使用的对齐填充。
//       所以 C 原型必须写成 (handle, int64, int)，否则参数错位会直接崩设备。
//   bbplayer_position(void*)  -> int64   （句柄偏移 0x1218）
//   bbplayer_duration(void*)  -> int64   （句柄偏移 0x1220）
//   bbplayer_state(void*)     -> int     （句柄偏移 0x1210）
//
// 【回调调用约定（反汇编铁证）】fn(userdata, type, a, b, c, d, msg)
//   寄存器：r0=userdata、r1=type、r2=a、r3=b；栈：[sp+0]=c、[sp+4]=d、[sp+8]=msg。
//   实测到的 type：
//     1 → 状态变更，a = 新 state（state=3 播放中，5 = 错误，0 = 停止，2 = 缓冲/准备）
//     7 → 错误，a = 错误码，msg = 错误原文（在 mux_start 的 "ERROR: %s" 分支发射）
//     8 → 日志，msg = 日志原文
//   msg 由库内部 snprintf 到临时栈缓冲区后传出，**只在回调瞬间有效，必须立即拷贝**。
//
// 时间单位（position / duration / seek）：libbbplayer 内部以"tick"计，rodata 里能看到
// "in microseconds" 字样的注释，故默认按微秒处理（TICKS_PER_MS = 1000）。若真机上
// 时长/进度显示差 1000 倍，把这里改成 1 即可（device-validation 项）。

class BbPlayer {
public:
    static const qint64 TICKS_PER_MS = 1000;

    /** 事件回调签名（见上方"回调调用约定"）。 */
    typedef void (*PFN_event_cb)(void* userdata, int type, int a, int b,
                                 int c, int d, const char* msg);

    BbPlayer();
    ~BbPlayer();

    bool loaded() const { return m_ok; }

    // 返回 bool 的函数：true = 调用成功（底层返回 0），false = 句柄无效或底层返回非 0。
    bool create();
    /**
     * 设置视频屏幕窗口的【组名】和【窗口 ID】。
     *
     * 【v62 重大修正 —— 参数顺序原来一直是反的】
     * 底层签名是 `bbplayer_set_window(void* handle, const char* group, const char* id)`，
     * **组在前、ID 在后**。v43~v61 一直按 (id, group) 传，导致：
     *   - 窗口的 SCREEN_PROPERTY_ID_STRING 被设成【主窗口组名】
     *   - screen_join_window_group 去 join 一个叫 "biliSurface_xxxx" 的【不存在的组】
     *   - join 失败 → 库内部直接 screen_destroy_window 把窗口销毁
     *   → 屏幕窗口从来就没建出来过，ForeignWindowControl 自然永远 boundToWindow=false。
     *
     * 反汇编证据链（libbbplayer.so，逐条核对）：
     *   bbplayer_set_window(0x58e00):
     *     0x58e08  mov  r6, r1                    ; r6 = arg1
     *     0x58e0a  mov  r5, r2                    ; r5 = arg2
     *     0x58e20  blx  snprintf(handle+0x1148, 128, "%s", r6)   ; arg1 → +0x1148
     *     0x58e34  blx  snprintf(handle+0x11c8,  64, "%s", r5)   ; arg2 → +0x11c8
     *   唯一调用建窗函数的地方(0x55c8c):
     *     0x55c4c  snprintf(bufA, 128, "%s", handle+0x1148)
     *     0x55c5c  snprintf(bufB,  64, "%s", handle+0x11c8)
     *     0x55c86  r1 = bufA ; 0x55c7e  r2 = bufB ; 0x55c8c  bl 0x622e4
     *   建窗函数(0x622e4):
     *     0x6231c  snprintf(struct+0x18, 64, "%s", arg2)          ; arg2 是 ID
     *     0x623be  screen_set_window_property_cv(win, 20 [ID_STRING], len, struct+0x18)
     *     0x623c6  screen_join_window_group(win, arg1)            ; arg1 是组
     *     0x623e6  join 失败 → screen_destroy_window(win) + screen_destroy_context(ctx)
     *
     * @param groupId  窗口组名（必须是已存在的组，通常 = Application::mainWindow()->groupId()）
     * @param windowId 窗口 ID（ForeignWindowControl 按它认领）
     */
    void setWindow(const QString& groupId, const QString& windowId);
    /**
     * 设置全局网络配置（UA / Referer）。**必须在 create() 之前调用。**
     *
     * 【v55】底层 bbplayer_net_config(ua, ref, caPath) 的函数体（libbbplayer 0x50220）
     * 只有三段 snprintf + 解锁就返回，写入的是 .bss 里三个固定槽：
     *   UA  槽 0x67a1a6 (512B)
     *   Ref 槽 0x67a3a6 (256B)
     *   第三槽 0x679302 (160B)
     * 也就是说它只写"全局快照"，真正读这份快照的是 create/open 流程。
     * v54 在 create 之后才调 → 快照写得再对也没人看。
     *
     * 本方法会先把 ua/referer 缓存到成员里，若句柄已存在则立即应用一次；
     * create() 内部会在 m_create() 前后各应用一次，确保两种读取时机都覆盖。
     */
    void setNetConfig(const QString& ua, const QString& referer);
    /** 提前解析 cacert.pem 路径（供 setNetConfig 在 create 之前使用）。 */
    bool prepareCa() { return installCaBundle(true); }
    void setVideoEnabled(bool on);
    bool openUrl(const QString& url, int flags = 0);
    /** 【v89】打开本地文件（已下载的视频）。库里没有 bbplayer_open_file 时
     *  自动退回 openUrl —— 行为不会比原来更差。 */
    bool openFile(const QString& path, int flags = 0);
    bool play();
    bool pause();
    bool close();
    bool seek(qint64 positionTicks, int flags = 0);
    qint64 positionTicks();
    qint64 durationTicks();
    int state();
    void destroy();

    /** 回调累积的事件/日志文本（线程安全拷贝），供界面显示。 */
    QString eventLog() const;
    /** 最近一次 type=7 错误（含错误码与原文）；空串表示没收到过错误事件。 */
    QString lastError() const;
    /** 回调收到的最近一次状态（type=1 的 a 值）；-1 表示还没收到过。 */
    int callbackState() const;
    /** 回调总次数（判断回调是否真的被调用）。 */
    int eventCount() const;
    /** CA 根证书装载状态（诊断用）："ok rc=0 <path>" / "fail ..." 等。 */
    QString caStatus() const { return m_caStatus; }
    /** 解析到的 cacert.pem 绝对路径（空串 = 没找到）；供诊断显示。 */
    QString caPath() const { return m_caPath; }

private:
    void resolve();
    /**
     * 解析 cacert.pem 路径 + 把它喂进 libbbnet 的证书 store（幂等、可重复调）。
     *
     * 【v54 认知修正】这个函数**并不能**让 reader 用上根证书，别指望它解决 HTTPS：
     *   - `bbnet_ca_add_file(path)` 只把 PEM 解析进一个【全局内存 store】
     *     （反汇编 0x358cc → bbnet_ca_add_pem），供 cert pinning / fingerprint 用；
     *   - 真正给 curl 句柄设 `CURLOPT_CAINFO` 的代码在 reader 建句柄流程里
     *     （反汇编 0x36d2e / 0x379ae：`ldr r2,[config,#16]; cmp r2,#0; beq 跳过;
     *      setopt(handle, CURLOPT_CAINFO=10065, r2)`），读的是全局 config 的 +16 槽；
     *   - 写那个槽的唯一通路是 `bbplayer_net_config` 的**第三参数**（见 setNetConfig）。
     *
     * 所以本函数现在的职责是：找到/复制出一个可读的 cacert.pem 绝对路径，
     * 存进 m_caPath 供 setNetConfig 使用；顺带把内容喂进 store（对 pinning 有益无害）。
     *
     * 【返回值语义】bbnet_ca_add_file 返回的是**成功加入的证书张数**，
     * 不是错误码；只有失败才返回 -1。所以 `> 0` 才是成功（本包 = 121 张）。
     *
     * @param force true 时忽略"已装载"缓存重新走一遍（用于 create 之后再补一次）。
     */
    bool installCaBundle(bool force = false);
    /** 把 m_ua / m_ref / m_caPath 应用到 libbbplayer 的全局快照（幂等，可反复调）。 */
    void applyNetConfig();
    /** 回调实际处理（在 libbbplayer 的线程里执行，不得碰 QObject / GUI）。 */
    void onEvent(int type, int a, int b, int c, int d, const char* msg);
    /** 传给 bbplayer_create 的 C 回调跳板。 */
    static void eventTrampoline(void* userdata, int type, int a, int b,
                                int c, int d, const char* msg);

    void*  m_lib;
    void*  m_handle;
    bool   m_ok;

    mutable QMutex m_mutex;
    QString m_log;
    QString m_lastError;
    int     m_cbState;
    int     m_eventCount;
    QString m_caStatus;
    QString m_caPath;
    QString m_ua;
    QString m_ref;
    bool    m_caInstalled;
    /** 【v63】CA 是否已经成功装过一次。装成功后永不再做 —— 解析 121 张包内证书 +
     *  125 张系统证书是页面打开卡顿的主因，v62 之前每次 ensurePlayer 要重复 2~3 遍。 */
    bool    m_caDone;
    int     m_caAdds;

    typedef void*  (*PFN_create)(void*, void*);

    typedef void   (*PFN_destroy)(void*);
    typedef int    (*PFN_set_window)(void*, const char*, const char*);
    // 【v54】注意：**没有 handle 参数**，是全局配置（见文件头注释）。
    typedef int    (*PFN_net_config)(const char*, const char*, const char*);
    typedef int    (*PFN_set_video_enabled)(void*, int);
    typedef int    (*PFN_open_url)(void*, const char*, int, const char*);
    /** 【v89】本地文件入口。反汇编 0x58e6c 确认签名：r0=handle, r1=path, r2=flags，
     *  返回 0 成功 / -1 参数非法。open_url 是给网络流用的，本地路径必须走这个。 */
    typedef int    (*PFN_open_file)(void*, const char*, int);
    typedef int    (*PFN_play)(void*);
    typedef int    (*PFN_pause)(void*);
    typedef int    (*PFN_close)(void*);
    typedef int    (*PFN_seek)(void*, long long, int);
    typedef long long (*PFN_position)(void*);
    typedef long long (*PFN_duration)(void*);
    typedef int    (*PFN_state)(void*);

    PFN_create            m_create;
    PFN_destroy           m_destroy;
    PFN_set_window        m_set_window;
    PFN_net_config        m_net_config;
    PFN_set_video_enabled m_set_video_enabled;
    PFN_open_url          m_open_url;
    PFN_open_file         m_open_file;   // 【v89】可选：拿不到时退回 openUrl
    PFN_play              m_play;
    PFN_pause             m_pause;
    PFN_close             m_close;
    PFN_seek              m_seek;
    PFN_position          m_position;
    PFN_duration          m_duration;
    PFN_state             m_state;
};

#endif // BBBPLAYER_WRAPPER_HPP
