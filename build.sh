#!/usr/bin/env bash
# BiliClient 一键构建脚本（Windows + BB10 NDK 10.1）
#
# 解决的已知环境坑：
#   1. Windows 下 ln -s 创建符号链接失败（需管理员权限）→ 用 cp 复制替代
#   2. ntoarm-objcopy 不在 PATH（strip 步骤）→ 跳过，devMode 包无需 strip
#   3. 系统 Java 21 太新，打包器用了已被移除的 SecurityManager → 用 NDK 自带 JDK 1.6
#   4. 【最致命】qmake 生成的 Makefile 里 moc 规则目标写成 `/.moc/xxx.cpp`（丢了盘符），
#      而依赖方要的是 `../../.moc/xxx.cpp`。两者字符串不等 → make 认为"无规则可生成"，
#      只要文件存在就直接复用，从此永不检查头文件是否变更。
#      后果：改了头文件（加信号/槽/属性）后 moc 仍是旧的，元对象与类布局错位，
#      QML 一读 Q_PROPERTY 就崩溃 —— 表现为"点图标无法打开/闪退"。
#      本脚本每次构建都强制重新生成 moc，并在检测到坏路径时自动修正 Makefile。
#
# 用法：bash build.sh

set -u

PROJ_DIR="E:/PCdesk/桌面/bilibili/DouyinBar"
BUILD_DIR="D:/bbuild/douyinbar"
NDK="D:/bbndk"
QNX_HOST="$NDK/host_10_1_0_132/win32/x86"
QNX_TARGET="$NDK/target_10_1_0_1020/qnx6"
NDK_JAVA="$NDK/jre/bin/java.exe"
LIB_DIR="$QNX_HOST/usr/lib"
# 注意：Makefile 用 ../../.moc 和 ../../.obj，相对 BUILD_DIR 解析后落在盘符根
MOC_DIR="D:/.moc"
OBJ_DIR="D:/.obj"

echo "==> [GATE] QML 全量静态自检（不过不许打包）"
# 【为什么做成门禁】QML 解析错误在桌面/编译期完全不可见：qcc 编译零错误、
# BAR 打包成功，只有装到设备上 QmlDocument 加载时才炸，且一炸就是整个首页不可用。
# 之前连续多轮 "QML PARSE ERROR" 都是靠用户截图往返定位的。
# 本门禁把「相对 import 目录必须存在」「components/ 内禁止相对 import」
# 「禁止属性/信号」「块状绑定」等已踩过的坑全部前移到构建之前。
if ! python "$PROJ_DIR/qmlcheck_full.py" "$PROJ_DIR/assets"; then
    echo "!!! QML 自检未通过，拒绝打包（避免再把 QML PARSE ERROR 交付到设备）"
    exit 1
fi
echo "    QML 自检通过"

echo "==> [GATE-DS] 设计规范合规自检（色板 / 图标矩阵 / 启动图标 / 资产引用）"
# 【为什么做成门禁】设计规范只写在文档里一定会被绕过：本项目已经吃过两次亏——
# ① 桌面图标一直沿用 BiliBar 的 bilibili 图标；② QML 色板与 C++ theme 各写一套、
# CASCADES_THEME 还留着哔哩粉。这些在 qcc 编译期零告警、装到设备上也不崩，
# 只有静态比对 tokens.json 才能拦住。事实源 design/tokens.json，报告 design/report/DRIFT.md。
if ! python "$(dirname "$PROJ_DIR")/design/check.py"; then
    echo "!!! 设计规范自检未通过，拒绝打包（见 design/report/DRIFT.md）"
    exit 1
fi

echo "==> [GATE2] 播放代理线程生命周期自检（防 1.0.0.11 SIGSEGV / 1.0.0.45 重传风暴回归）"
# 真机 1.0.0.11：ProxyStreamWorker 用 finished()→自身 deleteLater() 自毁，
# 而 VideoProxy::m_worker 是裸指针。curl 跑完一段 Range 后 worker 被释放，
# 主线程随后 onPump()/abortStream() 解引用 → QMutexLocker(&w->m_mutex)
# 读到被复用的堆内存（d=0x25）→ SIGSEGV @ QMutexLocker+0x9 ref=0x29。
# 这类 use-after-free 在桌面编译期零告警、链接零错误，只能靠静态门禁拦。
# 【1.0.0.45】新增两类回归防线：
#  a) onWorkerFinished 只许打 workerDone 标记，禁止停泵/断连——
#     线程结束≠交付完成，m_buf 没排空就关 = 播放器拿到截断流（1.0.0.44
#     "播不到头"的第二根因）；
#  b) onNewConnection 禁止 drop/abort 既有连接——libbbplayer 每个 Range
#     开一条连接还会流水线，杀掉在飞流 = 重传风暴（1.0.0.44 "加载很慢"的根因）。
if ! python - "$PROJ_DIR/src/video_store.cpp" "$PROJ_DIR/src/video_store.hpp" <<'PYGATE'
import re, sys
_cpp_raw = open(sys.argv[1], 'r', encoding='utf-8', errors='replace').read()
_hpp_raw = open(sys.argv[2], 'r', encoding='utf-8', errors='replace').read()

def strip(s):
    # 【1.0.0.45】剥注释但**保留字符串**（GATE6 同款状态机）。
    # 血泪：video_store.cpp 的修复注释里写着 "m_pump->stop()"、"dropClient()+abortStream()"
    # 这类字样（正是 1.0.0.44 的病灶记录），裸串匹配全踩中 → 干净源码被误杀。
    out = []
    i, n = 0, len(s)
    mode = 'code'
    while i < n:
        ch = s[i]
        nxt = s[i+1] if i+1 < n else ''
        if mode == 'code':
            if ch == '/' and nxt == '/':
                mode = 'line'; i += 2; continue
            if ch == '/' and nxt == '*':
                mode = 'block'; i += 2; continue
            if ch == '"':
                mode = 'str'; out.append(ch); i += 1; continue
            if ch == "'":
                mode = 'chr'; out.append(ch); i += 1; continue
            out.append(ch); i += 1; continue
        if mode == 'line':
            if ch == '\n':
                mode = 'code'; out.append(ch)
            i += 1; continue
        if mode == 'block':
            if ch == '*' and nxt == '/':
                mode = 'code'; i += 2; continue
            i += 1; continue
        if mode == 'str':
            if ch == '\\':
                out.append(ch); out.append(nxt); i += 2; continue
            if ch == '"':
                mode = 'code'
            out.append(ch); i += 1; continue
        if ch == '\\':
            out.append(ch); out.append(nxt); i += 2; continue
        if ch == "'":
            mode = 'code'
        out.append(ch); i += 1; continue
    return ''.join(out)

cpp = strip(_cpp_raw)
hpp = strip(_hpp_raw)
errs = []

def body_of(name):
    m = re.search(r'void\s+VideoProxy::' + name + r'\s*\(\s*\)\s*\{(.*?)\n\}', cpp, re.S)
    return m.group(1) if m else None

if re.search(r'SIGNAL\s*\(\s*finished\(\)\s*\)\s*,\s*\w+\s*,\s*SLOT\s*\(\s*deleteLater\(\)', cpp):
    errs.append('禁止 worker 自毁：finished() 连到 worker 自己的 deleteLater()，指针会悬空')
if 'void onWorkerFinished();' not in hpp:
    errs.append('video_store.hpp 缺少 onWorkerFinished 槽声明')
if 'SLOT(onWorkerFinished())' not in cpp:
    errs.append('finished() 未连接到 VideoProxy::onWorkerFinished')

b = body_of('onWorkerFinished')
if b is None:
    errs.append('找不到 VideoProxy::onWorkerFinished 实现')
else:
    if 'workerDone = true' not in b:
        errs.append('onWorkerFinished 必须打 workerDone 标记（线程结束≠交付完成，先排空再关）')
    if 'm_pump->stop()' in b or 'disconnectFromHost()' in b:
        errs.append('onWorkerFinished 禁止停泵/断连接（缓冲未排空即关=截断回归）')
    if '->deleteLater' not in b:
        errs.append('onWorkerFinished 孤儿路径必须 deleteLater 收尸')

n = body_of('onNewConnection')
if n is None:
    errs.append('找不到 VideoProxy::onNewConnection 实现')
else:
    if ('dropClient' in n) or ('abortStream' in n) or ('abortNow' in n):
        errs.append('onNewConnection 禁止 abort/drop 既有连接（1.0.0.45 重传风暴回归）')
    # closeConn 只允许出现在"回收空闲连接"或"超并发上限"两条守卫内；
    # 其它任何形式（"顺手把最老的关掉"）都是 1.0.0.44 重传风暴回归。
    nl = n.split('\n')
    for idx, ln in enumerate(nl):
        if 'closeConn(' in ln:
            ctx = ln + '\n' + (nl[idx - 1] if idx > 0 else '')
            if ('!c->worker' not in ctx) and ('kMaxConns' not in ctx):
                errs.append('onNewConnection 的 closeConn 必须带守卫（!c->worker 空闲回收 / kMaxConns 超限回收）')

if 'QList<ProxyConn*>' not in hpp.replace(' ', ''):
    errs.append('video_store.hpp 缺少多连接容器 QList<ProxyConn*>（1.0.0.45）')
if errs:
    for e in errs:
        print('  [LIFETIME] ' + e)
    sys.exit(1)
print('    线程生命周期自检通过（worker 不自毁 / 先标记排空再收尸 / 新连接不杀在飞流）')
PYGATE
then
    echo "!!! 播放代理线程生命周期自检未通过，拒绝打包"
    exit 1
fi

echo "==> [GATE3] NavigationPane pop 清理连接自检（防 1.0.0.12 死代码回归）"
# 真机 1.0.0.10~1.0.0.12：createScene() 只写
#   if (NavigationPane* nav = qobject_cast<NavigationPane*>(root)) connect(...)
# 但 main.qml 根节点是 TabbedPane，cast 恒失败 → onPagePopped() 从未连上，
# 系统返回手势/peek 退出播放页后 CDN 流与解码器会留在后台。
# 所有 device_boot_*.log 里都没有 "nav popTransitionEnded connected" 就是证据。
if ! python - "$PROJ_DIR/src/applicationui.cpp" "$PROJ_DIR/assets/main.qml" <<'PYGATE3'
import os, re, sys
cpp = open(sys.argv[1], 'r', encoding='utf-8', errors='replace').read()
qml = open(sys.argv[2], 'r', encoding='utf-8', errors='replace').read()
# 去掉注释再判断"旧写法是否复活"，否则修复说明本身会被误判
code = re.sub(r'/\*.*?\*/', '', cpp, flags=re.S)
code = re.sub(r'//[^\n]*', '', code)
errs = []
if 'findChildren<NavigationPane*>()' not in code:
    errs.append('createScene 必须遍历 findChildren<NavigationPane*>()（Tab 下的 nav 不是 root）')
if 'findChild<NavigationPane*>' not in code or 'dyNav' not in code:
    errs.append('缺少 dyNav 确定性兜底 findChild')
if 'connected count=' not in code:
    errs.append('缺少连接数量留痕，无法在真机证明 signal/slot 真的连上')
if re.search(r'if\s*\(\s*NavigationPane\s*\*\s*nav\s*=\s*qobject_cast\s*<\s*NavigationPane\s*\*\s*>\s*\(\s*root\s*\)\s*\)', code):
    errs.append('旧写法复活：只 qobject_cast 根节点会让 onPagePopped 永远不触发')
if 'nav page popped: ' not in code:
    errs.append('onPagePopped 缺少每次 pop 的留痕')
if not re.search(r'objectName\s*:\s*"dyNav"', qml):
    errs.append('main.qml 的 dyNav 缺少 objectName: "dyNav"')
# 【1.0.0.14】导航栈自检钩子必须保留：没有触摸注入通道，只能靠 push+pop 自证
hpp = open(os.path.join(os.path.dirname(sys.argv[1]), 'applicationui.hpp'),
           'r', encoding='utf-8', errors='replace').read()
if 'void onNavSelfTestPush();' not in hpp or 'void onNavSelfTestPop();' not in hpp:
    errs.append('缺少 nav selftest 槽声明（push/pop 不能被触摸注入替代）')
if 'kNavSelfTestFlag' not in code or 'SLOT(onNavSelfTestPush())' not in code:
    errs.append('缺少 nav_selftest.flag 触发路径')
if errs:
    for e in errs:
        print('  [NAVPOP] ' + e)
    sys.exit(1)
print('    NavigationPane pop 清理自检通过（遍历子树 + dyNav 兜底 + 连接留痕）')
PYGATE3
then
    echo "!!! NavigationPane pop 清理自检未通过，拒绝打包"
    exit 1
fi

echo "==> [GATE4] 封面链路 / 方屏比例 / 首屏自动播放自检（防 1.0.0.14 主页空白回归）"
# 真机 1.0.0.14 事故：cover.url_list 取 [0] 得到 .webp，却按 md5+".jpg" 落盘，
# Cascades ImageView 静默不解码 → 主页整屏没有封面；下载日志全是 code=0，
# isUsableImage 又对未知格式默认放行，三层"证据"全部失真。
# 本门禁把四个修复点钉死：JPEG URL 选择、严格字节校验、方屏 1:1 比例、自动播放触发。
if ! python - "$PROJ_DIR/src/douyin_api.cpp" "$PROJ_DIR/src/douyin_store.cpp" \
             "$PROJ_DIR/src/applicationui.cpp" "$PROJ_DIR/assets/components/DouyinFeedRow.qml" \
             "$PROJ_DIR/assets/DouyinPlayerPage.qml" "$PROJ_DIR/assets/DouyinFeedPage.qml" \
             "$PROJ_DIR/assets/main.qml" <<'PYGATE4'
import os, re, sys
api, store, appui, rowqml, playqml, feedqml, mainqml = [open(p, 'r', encoding='utf-8', errors='replace').read() for p in sys.argv[1:8]]
def strip(s):
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    return re.sub(r'[ \t]*//[^\n]*', '', s)
api_c, store_c, appui_c = strip(api), strip(store), strip(appui)
row_q, play_q, feed_q, main_q = strip(rowqml), strip(playqml), strip(feedqml), strip(mainqml)
errs = []
# 1) 封面 URL 必须走 JPEG 选择器，禁止直接取 url_list[0]
if 'DouyinApi::pickCoverUrl(video)' not in store_c:
    errs.append('douyin_store.cpp 封面必须用 DouyinApi::pickCoverUrl(video)（禁止 url_list[0] 拿 WebP）')
if re.search(r'value\("cover"\)\s*\.toMap\(\)\s*\.value\("url_list"\)', store_c):
    errs.append('旧写法复活：douyin_store.cpp 又在直接从 cover.url_list 取 [0]')
if 'pickJpegUrl' not in api_c or 'isJpegImageUrl' not in api_c:
    errs.append('douyin_api.cpp 缺少 pickJpegUrl/isJpegImageUrl（JPEG 候选选择器）')
# 2) isUsableImage 必须拒绝 ".jpg 扩展名 + 非 JPEG 内容"
if 'fmt != QLatin1String("JPEG")' not in api_c:
    errs.append('isUsableImage 必须拒绝 .jpg 后缀 + 非 JPEG 内容（WebP 伪装 .jpg 是本次事故根因）')
if 'imageFormatOf' not in api_c:
    errs.append('缺少 imageFormatOf（真实格式识别，自检与校验都靠它）')
if 'DouyinBar cover REJECT' not in api_c:
    errs.append('onDownloadFinished 缺少坏图 REJECT 日志（没有它就发现不了坏缓存）')
# 2.5) 【1.0.0.49】封面/头像磁盘缓存清扫（md5 jpg 只增不减缺陷的修复）：
#      sweepImageCaches 必须存在，且 ApplicationUI 构造期必须调用一次。
if 'int DouyinApi::sweepImageCaches' not in api_c:
    errs.append('douyin_api.cpp 缺少 DouyinApi::sweepImageCaches 实现（图片磁盘缓存只增不减）')
if 'sweepImageCaches(600' not in appui_c:
    errs.append('applicationui.cpp 未在构造期调用 sweepImageCaches(600, 128MB)（清扫不会自己跑）')
# 3) 头像必须下载并走 faceUrlFor（旧版 authorFace 只有远端 URL、永远显示不出）
#    1.0.0.17 起头像显示在 DouyinFeedPage 固定操作列，DouyinFeedRow 只剩封面海报。
if 'faceCachePath(face)' not in store_c:
    errs.append('douyin_store.cpp 缺少头像下载（faceCachePath）')
if 'faceSourceOf' not in feed_q or 'faceUrlFor' not in feed_q:
    errs.append('DouyinFeedPage.qml 头像必须走 faceSourceOf→faceUrlFor（coverUrlFor 找的是 _face.jpg 之外的路径）')
if 'faceUrlFor' in row_q:
    errs.append('DouyinFeedRow.qml 不应再有头像逻辑（1.0.0.17 起职责收窄为滑动载体 + 封面海报）')
# 1.5) 客户端 awemeId 去重（1.0.0.22 修「只在固定几个视频之间循环」）
if 'm_seenIds.contains(awemeId)' not in store_c or 'm_seenIds.insert(awemeId)' not in store_c:
    errs.append('douyin_store.cpp 缺少 awemeId 客户端去重（服务端匿名流返重复，不去重 → 刷过去的视频又回来）')
# 4) Q20 方屏 1:1：卡片高 720，封面必须 AspectFill 裁满"与 FWC 完全同矩形"的视频框，
#    播放页 screenH 720。
#    【1.0.0.22 修「视频叠在封面之上」】封面容器矩形必须按 curRow.videoAspect
#    算出的居中矩形（coverW×coverH），与内嵌 ForeignWindowControl 的 x/y/width/height
#    逐像素重合；AspectFit 会按封图自身比例留边，视频窗口盖不住 → 人物浮在山门封面上。
if re.search(r'preferredHeight\s*:\s*1280', row_q):
    errs.append('DouyinFeedRow.qml preferredHeight 又变回 1280（Q20 是 1:1 方屏，卡片必须 720）')
if 'ScalingMethod.AspectFill' not in row_q:
    errs.append('DouyinFeedRow.qml 封面必须用 ScalingMethod.AspectFill 裁满视频矩形（AspectFit 会露边，视频窗口盖不住封面 → 叠加错位）')
if 'coverW' not in row_q or 'coverH' not in row_q or 'videoAspect' not in row_q:
    errs.append('DouyinFeedRow.qml 缺少 coverW/coverH/videoAspect（封面矩形必须与 FWC 用同一真实视频比例同算）')
if re.search(r'screenH\s*:\s*1280', play_q):
    errs.append('DouyinPlayerPage.qml screenH 又变回 1280（Q20 方屏必须 720）')
# 5) 封面自检 + 首屏自动播放触发器必须保留（无截屏通道，只能靠框架解码器自证）
if 'onCoverSelfTest' not in appui_c or 'ImageTracker' not in appui_c:
    errs.append('applicationui.cpp 缺少 cover selftest / ImageTracker 解码自证')
# 9) 1.0.0.50 解码停滞看护（零依赖自愈网）。playing 态下 rawPosition
#    连续 5 tick（15s）不推进 = 解码链真卡死（缓冲死/网络死）→ replay()
#    原地重开媒体源（1.0.0.51 起 replay 不再拆窗，见 GATE7 ①）。
#    注意：它**不是**"循环播放有声无画"的修复——那个缺陷的 position 照走
#    （真机 ENDED pos=38000 40s/轮），修的是 replay 不拆窗口（GATE7）。
#    （screen API SCREEN_PROPERTY_POWER_MODE 跳变方案实测首 tick 即崩，已撤，
#     禁止引回：若 applicationui.cpp 出现 screen_create_context 视为回归）
if 'onStallWatchTick' not in appui_c:
    errs.append('applicationui.cpp 缺少 onStallWatchTick（解码链卡死无人重启）')
if 'rawPosition' not in appui_c:
    errs.append('停滞看护没读 rawPosition（零依赖方案的核心判据，缺了等于没在看）')
if 'm_player->replay()' not in appui_c:
    errs.append('停滞看护只探测不动手：命中停滞必须调 m_player->replay() 重开媒体源')
if 'onAutoPlayTick' not in appui_c or 'fireAutoPlay' not in api_c:
    errs.append('缺少首屏自动播放触发链（feed 就绪 → autoPlayTrigger → QML push）')
if 'maybeAutoPlay' not in feed_q or 'autoPlayTrigger' not in feed_q:
    errs.append('DouyinFeedPage.qml 缺少 maybeAutoPlay / autoPlayTrigger 绑定')
# 6) 播放页加载数据的时机：createObject 结束时 onCreationCompleted 已触发完，
#    而 main.qml 在 createObject 之后才赋 videoRow —— 没有属性变化钩子的话
#    播放页被 push 了也从不 load()（1.0.0.15 真机日志 pushPage 后无 startStream）。
if 'onVideoRowChanged' not in play_q or 'attachedId' not in play_q:
    errs.append('DouyinPlayerPage.qml 缺少 onVideoRowChanged + attachedId 守卫（否则播放页 push 了也不拉流）')
pi, ri = main_q.find('nav.push'), main_q.find('page.videoRow = row')
if pi == -1 or ri == -1 or ri < pi:
    errs.append('main.qml 顺序错误：必须先 nav.push(page) 再 page.videoRow = row（1.0.0.15 真机实证）')
# 7) 1.0.0.17 首页内嵌播放：上滑直接播放，不再 push 播放页
#    - FeedPage 必须有内嵌 FWC 固定层 + 上滑换流 + 点按暂停；
#    - 换流入口必须先 stop() 再 load()（与「退出播放页→下一部」同一序列，
#      VideoProxy::start 入口先 stop，VideoPlayer::stop 拆窗后 ensurePlayer 以同一 windowId 重建）。
if 'ForeignWindowControl' not in feed_q or 'windowId:' not in feed_q:
    errs.append('DouyinFeedPage.qml 缺少内嵌播放 ForeignWindowControl（windowId 绑 videoPlayer）')
if 'ListScrollStateHandler' not in feed_q or 'onFirstVisibleItemChanged' not in feed_q:
    errs.append('DouyinFeedPage.qml 缺少 ListScrollStateHandler.onFirstVisibleItemChanged（上滑不换流）')
if 'function playInline' not in feed_q:
    errs.append('DouyinFeedPage.qml 缺少 playInline() 换流函数')
i_stop, i_load = feed_q.find('videoPlayer.stop()'), feed_q.find('dyPlayerStore.load(row)')
if i_stop == -1 or i_load == -1 or i_load < i_stop:
    errs.append('playInline 换流顺序错误：必须先 videoPlayer.stop() 再 dyPlayerStore.load(row)')
if 'togglePlay' not in feed_q:
    errs.append('DouyinFeedPage.qml 缺少点按 togglePlay（内嵌视频无暂停手段）')
if errs:
    for e in errs:
        print('  [COVER] ' + e)
    sys.exit(1)
print('    封面链路/方屏比例/自动播放自检通过（JPEG 选择 + 严格校验 + 720x720 + 自检钩子）')
PYGATE4
then
    echo "!!! 封面链路自检未通过，拒绝打包"
    exit 1
fi

echo "==> [GATE5] 抖音式上滑分页自检（防 1.0.0.17/18 ListScrollStateHandler 收不到手势回归）"
if ! python - "$PROJ_DIR/assets/DouyinFeedPage.qml" "$PROJ_DIR/src/applicationui.cpp" <<'PYGATE5'
import re, sys
feed, appui = [open(p, 'r', encoding='utf-8', errors='replace').read() for p in sys.argv[1:3]]
def strip(s):
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    return re.sub(r'[ \t]*//[^\n]*', '', s)
f, a = strip(feed), strip(appui)
errs = []
# 1) 1.0.0.18 真机根因：全屏覆盖层默认 OverlapTouchPolicy.Deny 独占触摸，
#    ListView 完全收不到手势，firstVisibleItemChanged 永远不会因上滑而变化。
#    因此分页必须由根容器 onTouch 主动判定，且 ListView 关掉原生 flick。
if 'flickMode: FlickMode.SingleItem' in f:
    errs.append('ListView 又改回 SingleItem（覆盖层 Deny 时它收不到手势，1.0.0.18 真机实证）')
if 'flickMode: FlickMode.None' not in f:
    errs.append('ListView 必须 flickMode: None（分页手势由根容器独占，避免滚两行）')
if 'function gestureDown' not in f or 'function gestureUp' not in f:
    errs.append('缺少根容器手势判定函数 gestureDown/gestureUp')
if 'TouchType.Down' not in f or 'TouchType.Up' not in f:
    errs.append('根容器 onTouch 必须分别处理 TouchType.Down / TouchType.Up')
if 'feedPage.swipe(dy < 0 ? 1 : -1)' not in f:
    errs.append('gestureUp 未按 dy 方向调用 swipe()（上滑 +1 / 下滑 -1）')
if 'function swipe(' not in f or 'minSwipe' not in f:
    errs.append('缺少 swipe(dir) 分页函数或最小位移阈值')
# 2) 换流 + 程序化翻页必须成对，且顺序固定（先换流，封面随后跟上）
if 'feedPage.playInline(target)' not in f or 'app.scrollFeedTo(target)' not in f:
    errs.append('swipe() 必须同时调用 playInline(target) 与 app.scrollFeedTo(target)')
i_play, i_scroll = f.find('feedPage.playInline(target)'), f.find('app.scrollFeedTo(target)')
if i_play == -1 or i_scroll == -1 or i_scroll < i_play:
    errs.append('swipe() 顺序错误：必须先 playInline(target) 再 app.scrollFeedTo(target)')
# 3) C++ 窄桥：QML 直接调 scrollToItem 有两个重载（枚举版 + int workaround 版），
#    交给 C++ 用 findChild 明确类型更可靠，且能打日志自证。
if 'ApplicationUI::scrollFeedTo' not in a or 'findChild<ListView*>' not in a:
    errs.append('applicationui.cpp 缺少 scrollFeedTo() / findChild<ListView*> 翻页桥')
if 'ScrollAnimation::Smooth' not in a:
    errs.append('scrollFeedTo() 必须用 Smooth 动画（一次上滑平滑翻一屏）')
if 'objectName: "dyFeedList"' not in f:
    errs.append('ListView 缺少 objectName="dyFeedList"（C++ findChild 靠它）')
if 'ApplicationUI::swipeLog' not in a:
    errs.append('缺少 swipeLog 埋点（手势链路无法真机取证）')
# 4) 最顶层全局手势层 + Cancel 结算：1.0.0.19 真机 5 次从底部信息条起手的
#    上滑 0 次到达根容器（ListView 抢手势发 Cancel）。必须有最高 z 序接收层，
#    且 gestureTouch 要处理 Move 末点与 Cancel，否则手势会静默丢失。
if 'function gestureTouch' not in f or 'gestureActive' not in f:
    errs.append('缺少 gestureTouch() 统一手势入口 / gestureActive 去重')
if 'TouchType.Move' not in f or 'event.touchType === TouchType.Up || event.touchType === TouchType.Cancel' not in f:
    errs.append('gestureTouch 必须用同一分支结算 Up 与 Cancel（ListView 抢手势只发 Cancel）')
if f.count('onTouch: feedPage.gestureTouch(event)') < 2:
    errs.append('根容器与最顶层手势层都要接 gestureTouch（单一接收层会漏掉部分起手位置）')
if f.count('OverlapTouchPolicy.Allow') < 1:
    errs.append('最顶层手势层缺少 overlapTouchPolicy: Allow（下面的按钮会点不到）')
if 'objectName: "dyGestureLayer"' not in f:
    errs.append('缺少声明在最后的全局手势接收层 dyGestureLayer（1.0.0.19 真机实证）')
# 【1.0.0.27 收紧】tap 区守卫从写死的 157~562/<490 改为按视频矩形实算：
# rectX/rectY/videoW/videoH + 顶栏排除(Math.max(76,...))。竖屏 405x720 /
# 横屏 720x405 都贴视频；控件已移入视频内（videoUiLayer），tap 上界改
# ctrlZoneTop。守卫仍然必须有 —— 没有它点右侧操作列会连带暂停视频。
if 'rectX + 2' not in f or 'videoW - 2' not in f or 'ctrlZoneTop' not in f:
    errs.append('点按 togglePlay 缺少视频矩形坐标守卫（rectX/videoW/ctrlZoneTop 实算，防误伤右侧操作列与控件区）')
if 'Math.max(76,' not in f:
    errs.append('tap 区缺少顶栏排除（Math.max(76, ...)：09:41:33 点顶栏把视频暂停了的回归）')
# 5) 健壮性兜底：丢失结束事件（BB10 底部中央上滑触发系统导航手势会吞掉
#    Release/Cancel）会让 gestureActive 卡死，导致整个页面上滑永久失效。
#    用时间戳兜底强制接管，对正常连续刷视频流程零影响。
if 'gestureDownAt' not in f or 'ROOT RESET stale' not in f:
    errs.append('缺少 gestureActive 时间戳兜底（丢结束事件会永久卡死上滑，1.0.0.21 真机加固）')
if errs:
    for e in errs:
        print('  [SWIPE] ' + e)
    sys.exit(1)
print('    上滑分页自检通过（顶层手势层 + Cancel 结算 + 程序化翻页 + C++ 窄桥）')
PYGATE5
then
    echo "!!! 上滑分页自检未通过，拒绝打包"
    exit 1
fi

echo "==> [GATE6] 1.0.0.23 三件套自检（CDN IPv6 修复 / 评论页 / 设置入口）"
# 事故与需求来源（用户 2026-11-06 截图 + 反馈）：
#   ① 播放几秒后弹「播放链路失败：curl 7」—— 协议级根因：CDN 直链约 50% 302，
#      Location 是 IPv6 字面量主机（http://[2409:8c20:...]/...），BB10 curl 7.24
#      无 IPv6 通路 → CURLE_COULDNT_CONNECT(7)。修复三件套：手工走跳转链
#      （FOLLOWLOCATION=0 + headerCb 抓 Location）+ IPv6 字面量否决 +
#      多候选 URL 重试（同一 play_addr 的 url_list）。
#   ② 右侧加评论按钮 → 评论页（web v2 免签端点 + cid 去重兜底）。
#   ③ 隐藏三条杠后侧边栏没了 → 顶条常驻「去登录/设置」入口。
# 三条都容易被后续"顺手清理"掉且编译期零告警，钉死在门禁里。
if ! python - "$PROJ_DIR/src/video_store.cpp" "$PROJ_DIR/src/video_store.hpp" \
             "$PROJ_DIR/src/douyin_api.cpp" "$PROJ_DIR/src/douyin_store.cpp" \
             "$PROJ_DIR/src/applicationui.cpp" "$PROJ_DIR/assets/main.qml" \
             "$PROJ_DIR/assets/DouyinFeedPage.qml" "$PROJ_DIR/assets/DouyinCommentPage.qml" <<'PYGATE6'
import os, re, sys
vstore, vhpp, api, store, appui, mainqml, feedqml, cmtqml = [
    open(p, 'r', encoding='utf-8', errors='replace').read() for p in sys.argv[1:9]]
def strip(s):
    # 【1.0.0.23 修门禁自身 bug】不能用 r'/\*.*?\*/'：video_store.cpp 里有
    # curl_slist_append(hs, "Accept: */*") —— 字符串字面量中的 */* 会被当成
    # 块注释开头，一路吃到下一个 */，把中间几十行真代码（FOLLOWLOCATION/
    # HEADERFUNCTION/headerCb）全吞掉 → 门禁误报"修复没写"。改成状态机：
    # 跳过字符串/字符字面量，只剥真正的注释。
    out = []
    i, n = 0, len(s)
    mode = 'code'   # code / line / block / str / chr
    while i < n:
        ch = s[i]
        nxt = s[i+1] if i+1 < n else ''
        if mode == 'code':
            if ch == '/' and nxt == '/':
                mode = 'line'; i += 2; continue
            if ch == '/' and nxt == '*':
                mode = 'block'; i += 2; continue
            if ch == '"':
                mode = 'str'; out.append(ch); i += 1; continue
            if ch == "'":
                mode = 'chr'; out.append(ch); i += 1; continue
            out.append(ch); i += 1; continue
        if mode == 'line':
            if ch == '\n':
                mode = 'code'; out.append(ch)
            i += 1; continue
        if mode == 'block':
            if ch == '*' and nxt == '/':
                mode = 'code'; i += 2; continue
            i += 1; continue
        if mode == 'str':
            if ch == '\\':
                i += 2; continue
            if ch == '"':
                mode = 'code'
            out.append(ch); i += 1; continue
        # chr
        if ch == '\\':
            i += 2; continue
        if ch == "'":
            mode = 'code'
        out.append(ch); i += 1; continue
    return ''.join(out)
vs, api_c, store_c, appui_c = strip(vstore), strip(api), strip(store), strip(appui)
vh, main_q, feed_q, cmt_q = strip(vhpp), strip(mainqml), strip(feedqml), strip(cmtqml)
errs = []
# ---- ① CDN IPv6 修复三件套 ----
if 'CURLOPT_FOLLOWLOCATION, 0L' not in vs:
    errs.append('worker 必须关掉 FOLLOWLOCATION 手工走跳转链（自动follow 遇 IPv6 字面量即 rc=7）')
if 'headerCb' not in vs or 'CURLOPT_HEADERFUNCTION' not in vs:
    errs.append('缺少 headerCb / CURLOPT_HEADERFUNCTION（拿不到 Location 就无法手工跳转）')
if 'loc.contains("://[")' not in vs:
    errs.append('缺少 IPv6 字面量否决（302 到 [2409:...] 时 BB10 curl 无通路，必须换候选）')
if 'cdn-ipv6-hop' not in vs or 'cdn-ipv6-hop' not in store_c:
    errs.append('缺少 cdn-ipv6-hop 失败原因（真机日志无法区分"节点抽风"与"网络断开"）')
if 'kMaxHops' not in vs:
    errs.append('缺少 kMaxHops 跳数上限（手工走链防环）')
if 'QStringList m_alts' not in vh:
    errs.append('VideoProxy 缺少 m_alts 成员（多候选重试的载体）')
# 【1.0.0.28】预取命中分支把源换成 src/srcTotal/srcAlts（本地文件没有备用节点，
#   命中时 srcAlts 清空是正确语义），锚点随之更新； intent 不变：alts 必须透传。
if 'QStringList srcAlts = alts' not in store_c:
    errs.append('DouyinPlayerStore::startStream 缺少 srcAlts 拷贝（alts 来源断了）')
if 'm_proxy->start(src, srcTotal, srcAlts)' not in store_c:
    errs.append('DouyinPlayerStore::startStream 未把 alts 透传给 VideoProxy::start')
# ---- ② 评论功能 ----
if 'commentListUrl' not in api_c or 'www.iesdouyin.com' not in api_c:
    errs.append('douyin_api.cpp 缺少 commentListUrl（web v2 免签评论端点，主机不是 aweme.snssdk.com）')
if 'DouyinCommentModel::load(const QString& awemeId)' not in store_c:
    errs.append('douyin_store.cpp 缺少 DouyinCommentModel::load 实现（类声明/实现缺失）')
if 'm_seenCids.contains(cid)' not in store_c or 'm_seenCids.insert(cid)' not in store_c:
    errs.append('评论模型缺少 cid 客户端去重（服务端忽略 cursor，不去重会重复 append 同一批）')
if 'dycmt' not in store_c:
    errs.append('评论模型缺少 dycmt tag 前缀（netReply 多模型共存靠前缀路由）')
if 'setContextProperty("dyCommentModel"' not in appui_c:
    errs.append('applicationui.cpp 未注册 dyCommentModel context property')
if 'onAwemeIdChanged' not in cmt_q:
    errs.append('DouyinCommentPage.qml 缺少 onAwemeIdChanged（push 后赋 awemeId 才触发加载）')
if 'ListItemData.api.faceUrlFor' not in cmt_q:
    errs.append('评论行头像必须走 ListItemData.api.faceUrlFor（ListItemComponent 看不到 context property）')
if 'commentDef' not in main_q or 'pushComments' not in main_q:
    errs.append('main.qml 缺少 commentDef 工厂 / pushComments 函数')
# ---- ⑥ pushComments 顺序（限定在函数体内，防 1.0.0.24 pushHistory 新增同款调用后误判）----
mpc = re.search(r'function pushComments\(.*?\n    \}', main_q, re.S)
if not mpc:
    errs.append('main.qml 缺少 pushComments 函数')
else:
    pb = mpc.group(0).find('dyNav.push(page)')
    rb = mpc.group(0).find('page.awemeId = awemeId')
    if pb == -1 or rb == -1 or rb < pb:
        errs.append('main.qml pushComments 顺序错误：必须先 dyNav.push(page) 再 page.awemeId = awemeId')
if 'pushComments' not in feed_q:
    errs.append('DouyinFeedPage.qml 右侧操作列缺少评论按钮（pushComments 调用）')
# ---- ③ 设置入口常驻 ----
if '设置 >' not in feed_q:
    errs.append('顶条缺少登录态「设置 >」入口（用户反馈隐藏三条杠后推荐/设置入口丢失）')
if re.search(r'visible:\s*!\(biliApi\s*&&\s*biliApi\.loggedIn\)', feed_q):
    errs.append('旧写法复活：顶条又按登录态整体隐藏了（设置入口会再次消失）')
if errs:
    for e in errs:
        print('  [V123] ' + e)
    sys.exit(1)
print('    1.0.0.23 三件套自检通过（手工跳转+IPv6否决+多候选 / 评论页 / 常驻设置入口）')
PYGATE6
then
    echo "!!! 1.0.0.23 三件套自检未通过，拒绝打包"
    exit 1
fi

echo "==> [GATE7] 1.0.0.24 六件套自检（循环播放 / 左上菜单五项 / 作者名+控制条 / 历史页 / 同城关注流 / DNS 共享）"
# 需求来源（用户 2026-11-06 原话）：播完自动循环；左上角菜单按钮，菜单含
#   「同城、推荐、关注、设置、历史」；左下角作者用户名；视频内时长 + 播放/暂停
#   按钮 + 进度条；优化刷视频流畅度。
# 每条都对应一个已经真机验证过的机制（或 host 探针实证过的端点），但都容易被
# 后续"顺手清理"掉且编译期零告警，钉死在门禁里。
if ! python - "$PROJ_DIR/src/video_store.cpp" "$PROJ_DIR/src/video_store.hpp" \
             "$PROJ_DIR/src/douyin_store.cpp" "$PROJ_DIR/src/douyin_store.hpp" \
             "$PROJ_DIR/src/douyin_api.cpp" "$PROJ_DIR/src/douyin_api.hpp" \
             "$PROJ_DIR/src/applicationui.cpp" "$PROJ_DIR/assets/DouyinFeedPage.qml" \
             "$PROJ_DIR/assets/main.qml" "$PROJ_DIR/assets/DouyinHistoryPage.qml" <<'PYGATE7'
import re, sys
vcpp, vhpp, scpp, shpp, acpp, ahpp, uicpp, feedq, mainq, histq = [
    open(p, 'r', encoding='utf-8', errors='replace').read() for p in sys.argv[1:11]]
def strip(s):
    # 与 GATE6 同款状态机：剥注释但**保留字符串**（\u2630 / "同城" 等锚点在字符串里，
    # 清空字符串会让菜单/连接两条规则静默失效）。
    out = []
    i, n = 0, len(s)
    mode = 'code'
    while i < n:
        ch = s[i]
        nxt = s[i+1] if i+1 < n else ''
        if mode == 'code':
            if ch == '/' and nxt == '/':
                mode = 'line'; i += 2; continue
            if ch == '/' and nxt == '*':
                mode = 'block'; i += 2; continue
            if ch == '"':
                mode = 'str'; out.append(ch); i += 1; continue
            if ch == "'":
                mode = 'chr'; out.append(ch); i += 1; continue
            out.append(ch); i += 1; continue
        if mode == 'line':
            if ch == '\n':
                mode = 'code'; out.append(ch)
            i += 1; continue
        if mode == 'block':
            if ch == '*' and nxt == '/':
                mode = 'code'; i += 2; continue
            i += 1; continue
        if mode == 'str':
            if ch == '\\':
                out.append(ch); out.append(nxt); i += 2; continue
            if ch == '"':
                mode = 'code'
            out.append(ch); i += 1; continue
        if ch == '\\':
            out.append(ch); out.append(nxt); i += 2; continue
        if ch == "'":
            mode = 'code'
        out.append(ch); i += 1; continue
    return ''.join(out)
vc, vh = strip(vcpp), strip(vhpp)
sc, sh = strip(scpp), strip(shpp)
ac, ah = strip(acpp), strip(ahpp)
uc = strip(uicpp)
fq, mq, hq = strip(feedq), strip(mainq), strip(histq)
errs = []
# ---- ① 循环播放：双通道 ended 判定 + replay() ----
if 'Q_PROPERTY(bool ended' not in vh:
    errs.append('video_store.hpp 缺少 Q_PROPERTY(bool ended)（QML 无法知道播完）')
if 'void replay();' not in vh or 'void setEofLikely();' not in vh:
    errs.append('video_store.hpp 缺少 replay() / setEofLikely() 槽声明')
if 'void VideoPlayer::replay()' not in vc:
    errs.append('video_store.cpp 缺少 VideoPlayer::replay() 实现')
# 【1.0.0.49】循环重播决策移入 C++ 状态机（修 QML pstate 绑定环，真机 9 条
# "Binding loop detected" 实证）。负面锚点：QML 里不得再出现 videoPlayer.replay()
# —— 那是 1.0.0.24~48 的旧链路，同步 setState 重入是病根。
if 'videoPlayer.replay()' in fq:
    errs.append('DouyinFeedPage 又调 videoPlayer.replay()：1.0.0.48 起循环重播归 C++ 状态机，QML 只写日志（否则 pstate 绑定环复发）')
if 'Q_PROPERTY(bool loopOn' not in vh:
    errs.append('video_store.hpp 缺少 Q_PROPERTY(bool loopOn)（播放器侧循环开关）')
if 'void VideoPlayer::setLoopOn' not in vc:
    errs.append('video_store.cpp 缺少 VideoPlayer::setLoopOn 实现')
if 'if (m_loopOn)' not in vc:
    errs.append('ENDED 分支末尾缺少 if (m_loopOn) replay()（循环重播没接进状态机）')
if 'm_loopOn(true)' not in vc:
    errs.append('VideoPlayer 构造未把 m_loopOn 初始化为 true（默认循环与 QML 不一致）')
if 'videoPlayer.loopOn = feedPage.loopOn' not in fq:
    errs.append('循环开关按钮未同步给播放器：videoPlayer.loopOn = feedPage.loopOn')
# 【1.0.0.50】循环重播静默窗（修真机每轮 ICON play→0.4s→ICON pause 图标
# 闪断）：ENDED 分支保持 m_playing=true；replay() 内经 m_replayQuiet 把
# stop()/play() 的 playingChanged 压到窗关闭后按最终值补发一次。
# 负面锚点：ENDED 快照区不得再提前置 m_playing=false（那是闪断的直接源头，
# 真机形式 = swipe log 每轮 LOOP ended 紧跟一条 ICON play）。
need_replay_quiet = ('bool m_replayQuiet;' in vh) and ('m_replayQuiet' in vc)
if not need_replay_quiet:
    errs.append('video_store 缺少 m_replayQuiet 重播静默窗（循环重播图标闪断回归）')
if 'm_replayQuiet = true;' not in vc or 'm_replayQuiet = false;' not in vc:
    errs.append('replay() 静默窗没有成对开/关（m_replayQuiet = true/false;）')
# 【1.0.0.51】循环重播不得销毁屏幕窗口（修真机一小时实证的"循环播放有声
# 无画"）：stop()→bbplayer_close() 销毁 libbbplayer 屏幕窗口，play()→
# ensurePlayer() 以同一 windowId 重建，而 QML ForeignWindowControl 加入
# 场景只扫一次窗口、不回头重扫（v44 教训）→ 重建窗口永远 boundToWindow=
# false。真机证据：REPLAY 之后 swipe log 再无 `first-frame`；01:51/02:06
# 前台截图视频区 0.00% 差分；dyplay.log ENDED/REPLAY 40s/轮照走（pos=
# total=38000，解码链活着）—— 纯粹是输出面死了。
# 正面锚点：replay() 函数体内不得出现 stop() 调用，必须原地 play() 重开。
m_replay = re.search(r'void VideoPlayer::replay\(\)\s*\{.*?\n\}\n', vc, re.S)
if not m_replay:
    errs.append('replay() 函数体提取失败（GATE7 自检失效，需人工复核）')
else:
    _rb = m_replay.group(0)
    if re.search(r'^\s*stop\(\);', _rb, re.M):
        errs.append('replay() 又调 stop()：销毁屏幕窗口 → FWC 不重扫 → 循环播放有声无画（1.0.0.51 回归防线）')
    if 'play(m_lastUrl);' not in _rb:
        errs.append('replay() 必须在既有窗口上原地重开媒体源（play(m_lastUrl)，不走 stop/拆窗）')
if 'm_eofLikely' not in vc or 'trustedDur' not in vc:
    errs.append('ended 判定必须是双通道（m_eofLikely 代理 EOF 硬事实 + trustedDur 播放器自报时长）')
if 'emit streamComplete()' not in vc:
    errs.append('VideoProxy::onPump 缺少 emit streamComplete()（EOF 硬事实通道）')
if 'void playTrace' not in vc or 'void playTrace' not in vh:
    errs.append('playTrace 量测通道缺失（换片时延只能靠它留痕）')
# ---- ② Store 转发 + 历史模型 + 同城/关注端点 ----
if 'QObject::connect(m_proxy, SIGNAL(streamComplete())' not in sc or 'this, SIGNAL(streamComplete()))' not in sc:
    errs.append('DouyinPlayerStore 未把代理 streamComplete 转发出去（循环少一条事实通道）')
if 'class DouyinHistoryModel' not in sh:
    errs.append('douyin_store.hpp 缺少 DouyinHistoryModel 类')
if 'Q_INVOKABLE void add' not in sh or 'Q_INVOKABLE void clear' not in sh or 'Q_INVOKABLE QVariantMap rowAt' not in sh:
    errs.append('DouyinHistoryModel 缺少 add/clear/rowAt 之一')
if 'm.remove("api")' not in sc:
    errs.append('历史落盘未剥离 api 指针（QObject* 进 JSON 会写坏文件）')
if 'm.insert("api"' not in sc:
    errs.append('历史读盘未补 api 指针（ListItemComponent 解析不到封面）')
if '/aweme/v1/nearby/feed/' not in ac or '/aweme/v1/follow/feed/' not in ac:
    errs.append('douyin_api.cpp 缺少同城/关注端点（nearby 匿名可用已实证，follow 匿名恒空）')
if 'nearbyFeedApi()' not in ah or 'followFeedApi()' not in ah:
    errs.append('douyin_api.hpp 缺少 nearbyFeedApi/followFeedApi 声明')
if 'nearbyFeedApi()' not in sc or 'followFeedApi()' not in sc:
    errs.append('douyin_store.cpp fetch() 未按 Kind 路由到同城/关注端点')
# ---- ③ 流畅度：curl_share DNS 缓存共享 ----
if 'CURL_LOCK_DATA_DNS' not in vc:
    errs.append('缺少 CURL_LOCK_DATA_DNS（刷视频流畅度优化一：换片不再重复解析 CDN 域名；NDK curl 7.24 只有旧名 DNS，没有 7.57 才有的 DNS_CACHE）')
# ---- ③b 1.0.0.24 二修：seek 反馈风暴 + ENDED 日志重入失真 ----
# 真机 1.0.0.24 首版：positionMs 绑定每秒回写 Slider value，程序化写同样触发
# immediateValueChanged/valueChanged → 被当成"用户在拖" → 每秒两次 seekTo →
# 解码器反复重启（douyin_swipe.log 1346 条 SEEK drag 实证，无真人触摸）。
# 判据必须落在触摸上；松手 seek 也走 onTouch Up，不再看 valueChanged。
# 【为什么限定 Slider 块】全局查 onTouch 会被根容器/视频层的手势 onTouch 顶位：
# 注入测试删掉 Slider 自己的 onTouch 后全局检查仍通过（INJ-C 漏检），必须取
# Slider { ... } 配对块（跳过字符串与嵌套花括号）再查判据。
def qml_block(s, kw):
    m = re.search(kw + r'\s*\{', s)
    if not m:
        return ''
    i = m.end() - 1
    depth = 0
    quote = ''
    while i < len(s):
        ch = s[i]
        if quote:
            if ch == '\\':
                i += 2
                continue
            if ch == quote:
                quote = ''
        elif ch == '"' or ch == "'":
            quote = ch
        elif ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                return s[m.start():i + 1]
        i += 1
    return s[m.start():]
slb = qml_block(fq, 'Slider')
if not slb:
    errs.append('控制条缺少进度条（Slider 块：fromValue/onTouch/onImmediateValueChanged/seekTo）')
else:
    if 'onTouch' not in slb:
        errs.append('进度条缺少 onTouch（Slider 块内必须是真实触摸判据，防程序化回写风暴）')
    if 'TouchType.Down' not in slb or 'TouchType.Up' not in slb:
        errs.append('Slider onTouch 必须分别处理 TouchType.Down / TouchType.Up')
    if 'onImmediateValueChanged' not in slb:
        errs.append('Slider 块内缺少 onImmediateValueChanged（拖动中实时 seek 的唯一通道）')
    if 'if (!feedPage.seeking) return;' not in slb:
        errs.append('onImmediateValueChanged 缺少 seeking 守卫（程序化回写会被当成拖动）')
    if 'seekTo' not in slb:
        errs.append('Slider 块内未接 seekTo（只能看不能拖）')
if 'onValueChanged' in fq:
    errs.append('进度条又用回 onValueChanged 触发 seek（1.0.0.24 首版 1346 条自动 SEEK 的根因）')
# ENDED 块：setState/emit 会同步触发 QML 的 pstate 处理器（1.0.0.49 起只写
# 日志，replay 改到本块末尾由 C++ 状态机执行）。日志值仍先快照，否则打出
# eof=0 trustedDur=0 的失真行。
# 【为什么查声明行】只查 'snapEof' in vc 会被 .arg(snapEof) 引用顶位（INJ-D 删掉
# 声明行后仍通过），必须锚定声明本身。
if 'const int snapEof = m_eofLikely' not in vc or 'const int snapTrust = trustedDur' not in vc:
    errs.append('ENDED 块缺少快照声明（const int snapEof = m_eofLikely / snapTrust = trustedDur）：重入会让日志打出 0/0 假值')
# ---- ③c 1.0.0.26 抽屉按钮可点性（防 1.0.0.24/25 两代真机连击零命中回归） ----
# 真机证据：两代版本用户连点抽屉十次，日志只有 MENU toggle / MENU close，
# 零 MENU switch / MENU history —— 手势层在抽屉区域内起手后，Cancel/Up 走
# gestureUp 的 menuOpen 分支把抽屉藏掉，按钮 onClicked 永远不跑。
# 抽屉区域（x<304 且 y>=74）的触摸必须整段让给抽屉自己的按钮。
if 'menuOpen && event.windowX < 304' not in fq or 'windowY >= 74' not in fq:
    errs.append('抽屉区域缺少手势避让（menuOpen && windowX < 304 && windowY >= 74）：菜单项点击会被手势层藏抽屉杀死')
# ---- ③d 1.0.0.26 历史落盘 rename 覆盖（真机 .part 残留实证） ----
# QFile::rename 不覆盖已存在目标：首建之后每次 saveFile 的 rename 都静默失败，
# 磁盘历史永远停在第一行（真机 douyin_history.json.part mtime 22:56 vs .json 21:51）。
if 'QFile::remove(m_path);' not in sc:
    errs.append('saveFile 的 rename 前未删旧文件（QFile::rename 不覆盖已存在目标：历史永远停在第一行）')
# timeAgo 不能是空桩（历史行/评论行的时间列都靠它）。
if 'Q_UNUSED(unix)' in ac:
    errs.append('DouyinApi::timeAgo 又是空桩（历史/评论时间列会全空）')
if '分钟前' not in ac:
    errs.append('DouyinApi::timeAgo 缺少中文相对时间实现')
if 'CURLOPT_SHARE' not in vc:
    errs.append('worker 未挂 CURLOPT_SHARE，DNS 共享没接上')
if 'dnsShare' not in vc:
    errs.append('缺少 dnsShare() 惰性创建（共享句柄）')
# ---- ④ applicationui 连接与注册 ----
if 'SLOT(setEofLikely())' not in uc:
    errs.append('applicationui.cpp 未连接 streamComplete -> setEofLikely（循环判定断链）')
for cp in ('dyFollowModel', 'dyCityModel', 'dyHistoryModel'):
    if ('setContextProperty("%s"' % cp) not in uc:
        errs.append('applicationui.cpp 未注册 %s context property' % cp)
# ---- ⑤ DouyinFeedPage：菜单 / 循环 / 作者名 / 控制条 ----
if '\\u2630' not in fq:
    errs.append('左上角缺少菜单按钮（\\u2630 汉堡）')
if 'function switchFeed' not in fq:
    errs.append('DouyinFeedPage.qml 缺少 switchFeed(kind) 流切换函数')
for k in ('switchFeed("city")', 'switchFeed("recommend")', 'switchFeed("follow")'):
    if k not in fq:
        errs.append('菜单缺少 %s 调用' % k)
for lab in ('同城', '推荐', '关注', '设置', '历史'):
    if ('"%s"' % lab) not in fq:
        errs.append('抽屉菜单缺少「%s」项' % lab)
if 'pushHistory' not in fq:
    errs.append('菜单「历史」未调 host.pushHistory()')
if 'pstate' not in fq:
    errs.append('循环播放缺链路：pstate 影子属性（ended 通知通道，1.0.0.49 起只写日志）')
if 'LOOP ended id=' not in fq:
    errs.append('pstate 处理器缺少 LOOP ended 埋点（循环重播无运行时证据）')
if 'loopOn' not in fq:
    errs.append('缺少 loopOn 循环开关属性')
if 'dyHistoryModel.add' not in fq:
    errs.append('playInline 换片未记观看历史（dyHistoryModel.add）')
# 【1.0.0.29 形态升级】播放/暂停从文字字形（\u23F8/\u25B6，BB10 上渲染丑陋）
# 换成 PIL 图标（imageSource）。判据跟着改成"图标引用 + togglePlay 行为"，
# 不再限定字形 —— 真正要防的回归是"控件被删/点击失效"。
if ('asset:///images/play.png' not in fq) or ('asset:///images/pause.png' not in fq):
    errs.append('控制条缺少播放/暂停按钮（imageSource 引用 play/pause.png，1.0.0.29 起禁用文字字形）')
# togglePlay 行为锚点在上方 GATE7 第 207 行已有（点按 togglePlay），此处不重复
if 'Slider' not in fq or 'fromValue' not in fq or 'onImmediateValueChanged' not in fq:
    errs.append('控制条缺少进度条（Slider fromValue/onImmediateValueChanged）')
if 'seekTo' not in fq:
    errs.append('进度条未接 seekTo（只能看不能拖）')
if 'function fmtTime' not in fq:
    errs.append('缺少 fmtTime() 时长格式化')
if 'durationMs' not in fq:
    errs.append('控制条未显示视频时长（durationMs）')
if 'authorName' not in fq:
    errs.append('左下角缺少作者用户名叠加层（authorName）')
# 【1.0.0.27 收紧】控件区守卫从写死的 downY >= 645 改为跟随视频矩形的
# ctrlZoneTop（控件已移入视频内，横屏视频只有 405 高，写死值会把整个视频区
# 判成控件区导致翻页全废；反之删掉守卫则拖进度条会触发翻页）。
if 'ctrlZoneTop' not in fq:
    errs.append('缺少控件区手势守卫（ctrlZoneTop：起手在视频矩形内且低于它 = 够控件，不翻页）')
if 'downX >= feedPage.rectX' not in fq:
    errs.append('控件区守卫未按视频矩形界定的（downX >= feedPage.rectX，防黑边区翻页被误伤）')
# ---- ⑥ main.qml / 历史页 ----
if 'id: feedHome' not in mq:
    errs.append('main.qml 推荐页缺少 id: feedHome（历史复播要回到这一页）')
if 'historyDef' not in mq:
    errs.append('main.qml 缺少 historyDef ComponentDefinition')
if 'function pushHistory' not in mq:
    errs.append('main.qml 缺少 pushHistory()')
if 'function playHistoryItem' not in mq:
    errs.append('main.qml 缺少 playHistoryItem(row)（历史点击复播入口）')
mpi = mq.find('function pushHistory')
mp = mq.find('dyNav.push(page)', mpi)
mn = mq.find('page.nav = dyNav', mpi)
if mp == -1 or mn == -1 or mn > mp:
    errs.append('pushHistory 顺序错误：必须先在 push 前赋 nav（createObject 时页面未入栈）')
if 'feedHome.playRow' not in mq:
    errs.append('playHistoryItem 未调 feedHome.playRow(row)（同步直调，不走信号）')
if 'function switchFeed' in fq and 'playRow' not in fq:
    errs.append('DouyinFeedPage.qml 缺少 playRow()（历史复播落点）')
if 'host.playHistoryItem(row)' not in hq:
    errs.append('DouyinHistoryPage.qml 点击行未调 host.playHistoryItem(row)')
if 'dyHistoryModel.clear' not in hq:
    errs.append('DouyinHistoryPage.qml 缺少「清空历史」按钮')
if 'ListItemData.api.coverUrlFor' not in hq:
    errs.append('历史行封面必须走 ListItemData.api.coverUrlFor（delegate 看不到 context property）')
if errs:
    for e in errs:
        print('  [V124] ' + e)
    sys.exit(1)
print('    1.0.0.24 六件套自检通过（循环双通道 / 菜单五项 / 作者名+控制条 / 历史页 / 同城关注 / DNS 共享）')
PYGATE7
then
    echo "!!! 1.0.0.24 六件套自检未通过，拒绝打包"
    exit 1
fi

echo "==> [GATE8] 1.0.0.27 两件套自检（菜单点击必反馈 / 控件入画跟随视频）"
# 需求来源（用户 2026-11-07 原话）：①"菜单里同城、推荐、关注、设置、历史点击后
#   都没有反应"；②"将进度条和'播放/暂停'放到视频中去跟随视频自动适配"。
# 根因（今早真机日志实证，analysis/p26/douyin_swipe.log）：
#   - switchFeed 同项早退零反馈：08:18:33.630 "MENU same kind=recommend keep"
#     之后画面零变化，抽屉随即关闭 → 用户体感"点了没反应"；
#   - 抽屉「设置」只 menuOpen=false + openLogin()，无日志无结果：08:18:31 /
#     09:41:30 两次抽屉静默消失（无任何 MENU 日志），push 失败无从感知；
#   - 顶栏误触：09:41:33.242 点顶栏标签（y=39）把视频暂停了（tap 区写死
#     y<490 不含顶栏排除）。
if ! python - "$PROJ_DIR/assets/DouyinFeedPage.qml" "$PROJ_DIR/assets/main.qml" <<'PYGATE8'
import re, sys
feedq, mainq = [open(p, 'r', encoding='utf-8', errors='replace').read() for p in sys.argv[1:3]]
def strip(s):
    # 与 GATE6 同款状态机：剥注释但**保留字符串**（"同城"/"pushed" 等锚点在其中）。
    out = []
    i, n = 0, len(s)
    mode = 'code'
    while i < n:
        ch = s[i]
        nxt = s[i+1] if i+1 < n else ''
        if mode == 'code':
            if ch == '/' and nxt == '/':
                mode = 'line'; i += 2; continue
            if ch == '/' and nxt == '*':
                mode = 'block'; i += 2; continue
            if ch == '"':
                mode = 'str'; out.append(ch); i += 1; continue
            if ch == "'":
                mode = 'chr'; out.append(ch); i += 1; continue
            out.append(ch); i += 1; continue
        if mode == 'line':
            if ch == '\n':
                mode = 'code'; out.append(ch)
            i += 1; continue
        if mode == 'block':
            if ch == '*' and nxt == '/':
                mode = 'code'; i += 2; continue
            i += 1; continue
        if mode == 'str':
            if ch == '\\':
                out.append(ch); out.append(nxt); i += 2; continue
            if ch == '"':
                mode = 'code'
            out.append(ch); i += 1; continue
        if ch == '\\':
            out.append(ch); out.append(nxt); i += 2; continue
        if ch == "'":
            mode = 'code'
        out.append(ch); i += 1; continue
    return ''.join(out)
fq, mq = strip(feedq), strip(mainq)
errs = []
def qml_block(s, kw, occurrence=0):
    # 取第 occurrence 个 kw 开头的 { ... } 配对块。kw 只定位起点（如
    # 'if \\(kind === feedPage.feedKind'），起始花括号从起点向后找（if 条件里
    # 可能带 && / 三元），再按花括号深度配对（跳过字符串与嵌套块）。
    starts = [m.start() for m in re.finditer(kw, s)]
    if len(starts) <= occurrence:
        return ''
    base = starts[occurrence]
    i = base
    quote = ''
    while i < len(s):
        ch = s[i]
        if quote:
            if ch == '\\':
                i += 2; continue
            if ch == quote:
                quote = ''
        elif ch == '"' or ch == "'":
            quote = ch
        elif ch == '{':
            break
        i += 1
    if i >= len(s):
        return ''
    depth = 0
    while i < len(s):
        ch = s[i]
        if quote:
            if ch == '\\':
                i += 2; continue
            if ch == quote:
                quote = ''
        elif ch == '"' or ch == "'":
            quote = ch
        elif ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                return s[base:i + 1]
        i += 1
    return s[base:]
# ---- ① 菜单同项不得静默早退：必须有可见动作（回顶部+重播+HUD） ----
same_kind = qml_block(fq, 'if \\(kind === feedPage.feedKind')
if not same_kind:
    errs.append('switchFeed 缺少同项分支块（if (kind === feedPage.feedKind ...）')
else:
    if '" + kind + " keep"' in same_kind or 'keep' in same_kind:
        errs.append('同项分支又是静默早退（08:18:33.630 MENU same kind keep 零反馈回归）')
    if 'playInline(0)' not in same_kind:
        errs.append('同项分支没有可见动作（必须回顶部重播第一部：playInline(0)）')
    if 'scrollFeedTo(0)' not in same_kind:
        errs.append('同项分支没有回顶部（scrollFeedTo(0)）')
    if 'showTip' not in same_kind:
        errs.append('同项分支没有 HUD 反馈（用户点完看不到任何变化）')
# ---- ② 设置/历史/顶栏登录必须落结果日志 + 失败明示 ----
for anchor in ('MENU settings ret=', 'MENU history ret=', 'TOPBAR openLogin ret='):
    if anchor not in fq:
        errs.append('缺少菜单/顶栏动作结果埋点（%s）：push 失败会再次静默' % anchor)
if 'pushed' not in fq:
    errs.append('菜单/顶栏未判 main.qml 的 push 返回值（pushed 才算成功）')
# ---- ③ 控件入画：videoUiLayer 必须贴视频矩形 ----
if 'id: videoUiLayer' not in fq or 'objectName: "videoUiLayer"' not in fq:
    errs.append('缺少控件入画层 videoUiLayer（进度条/播放暂停必须放进视频矩形）')
ui_idx = fq.find('id: videoUiLayer')
ui_win = fq[ui_idx:ui_idx + 900] if ui_idx >= 0 else ''
if 'preferredWidth: feedPage.videoW' not in ui_win:
    errs.append('videoUiLayer 宽度没跟视频矩形（preferredWidth: feedPage.videoW）')
if 'preferredHeight: feedPage.videoH' not in ui_win:
    errs.append('videoUiLayer 高度没跟视频矩形（preferredHeight: feedPage.videoH）')
for decl in ('property double rectX', 'property double rectY', 'property double ctrlZoneTop'):
    if decl not in fq:
        errs.append('缺少视频矩形几何声明（%s）' % decl)
if 'Math.max(76,' not in fq:
    errs.append('tap 区缺少顶栏排除（Math.max(76, ...：09:41:33 点顶栏误暂停视频回归）')
if 'videoPlayer && !videoPlayer.playing' not in fq:
    errs.append('缺少中央暂停提示（暂停时画面中央亮 \\u25B6）')
# 循环钮必须在视频层内（入画后行宽不够，右上角）
# 【1.0.0.55 / UI-AUDIT P1-1】判据从「文字文案」换成「图标化 + 状态双态」：
# 旧 Button 40px 带 chrome、「循环/不循环」切文字会改宽度，同时违反
# §6 IconButton 契约与 minTouch 44；现在只改图形颜色，宽度恒 44。
for anchor in ('asset:///images/ic_loop_s.png', 'asset:///images/ic_loop_on_s.png',
               'asset:///images/ic_loop_sp.png', 'asset:///images/ic_loop_on_sp.png'):
    if anchor not in fq:
        errs.append('循环开关缺少图标资产 %s（必须 ImageButton 44x44 双态+按压态）' % anchor)
if 'feedPage.loopOn ? "循环" : "不循环"' in fq:
    errs.append('循环开关仍是文字态切换（宽度会变，1.0.0.55 起必须只改图形）')
if 'ICON loop=' not in fq:
    errs.append('循环开关缺 ICON loop 状态埋点（真机无法取证状态切换）')
# ---- ④ main.qml 侧：openLogin/pushHistory 必须返回可判定的结果串 ----
open_idx = mq.find('function openLogin')
open_win = mq[open_idx:open_idx + 700] if open_idx >= 0 else ''
for ret in ('"pushed"', '"settings-null"'):
    if ret not in open_win:
        errs.append('main.qml openLogin() 未返回可判定结果（%s）' % ret)
if errs:
    for e in errs:
        print('  [V127] ' + e)
    sys.exit(1)
print('    1.0.0.27 两件套自检通过（菜单必反馈 / 控件入画跟随视频矩形）')
PYGATE8
then
    echo "!!! 1.0.0.27 两件套自检未通过，拒绝打包"
    exit 1
fi

echo "==> [GATE9] 1.0.0.28 下一片预取器件自检（防预取回归：官方 asyncPrepareVideo 移植）"
# 需求来源（用户 2026-11-07 原话）："使用rea逆向学习一下官方抖音的播放滑动
#   逻辑和ui设计将我们的app进行优化升级包括播放滑动的流畅度"。
# 逆向结论（analysis/p26/rea_notes.md）：官方 FeedCacheConfig.asyncPrepareVideo
#   = 当前片起播即后台预取下一片，换片本地秒开。BB10 等价实现见
#   video_prefetch.hpp 头注释。本关防的退化（都是本项目真机踩过的坑）：
#   - 同 url 重复下载 / 在飞不设上限（打满带宽，播放流被饿死）；
#   - QFile::rename 前不 remove（1.0.0.25 历史落盘静默失败同款）；
#   - worker 自毁 / 先 deleteLater 再摘链（1.0.0.11 SIGSEGV 同款）；
#   - 半截 .part 被当成完整文件误判命中（换片拿到坏源）；
#   - startStream 忘了把源换成 file://（预取了却不用，白下载）；
#   - QML 换片后不触发 prefetchNext（预取器永远空转）。
if ! python - "$PROJ_DIR/src/video_prefetch.cpp" "$PROJ_DIR/src/video_prefetch.hpp" \
              "$PROJ_DIR/src/douyin_store.cpp" \
              "$PROJ_DIR/src/video_store.cpp" "$PROJ_DIR/Makefile" \
              "$PROJ_DIR/assets/DouyinFeedPage.qml" "$PROJ_DIR/bar-descriptor.xml" <<'PYGATE9'
import re, sys
pf_cpp, pf_hpp, ds_cpp, vs_cpp, mk, fq, desc = [
    open(p, 'r', encoding='utf-8', errors='replace').read() for p in sys.argv[1:8]]
def strip(s):
    # 与 GATE6 同款状态机：剥注释但**保留字符串**（C++ 字符串含 key 锚点）。
    out = []
    i, n = 0, len(s)
    mode = 'code'
    while i < n:
        ch = s[i]
        nxt = s[i+1] if i+1 < n else ''
        if mode == 'code':
            if ch == '/' and nxt == '/':
                mode = 'line'; i += 2; continue
            if ch == '/' and nxt == '*':
                mode = 'block'; i += 2; continue
            if ch == '"':
                mode = 'str'; out.append(ch); i += 1; continue
            if ch == "'":
                mode = 'chr'; out.append(ch); i += 1; continue
            out.append(ch); i += 1; continue
        if mode == 'line':
            if ch == '\n':
                mode = 'code'; out.append(ch)
            i += 1; continue
        if mode == 'block':
            if ch == '*' and nxt == '/':
                mode = 'code'; i += 2; continue
            i += 1; continue
        if mode == 'str':
            if ch == '\\':
                out.append(ch); out.append(nxt); i += 2; continue
            if ch == '"':
                mode = 'code'
            out.append(ch); i += 1; continue
        if ch == '\\':
            out.append(ch); out.append(nxt); i += 2; continue
        if ch == "'":
            mode = 'code'
        out.append(ch); i += 1; continue
    return ''.join(out)
pf, pfh, ds, vs = strip(pf_cpp), strip(pf_hpp), strip(ds_cpp), strip(vs_cpp)
mk_s, fq_s, desc_s = strip(mk), strip(fq), strip(desc)
errs = []
def need(cond, msg):
    if not cond:
        errs.append(msg)
# ---- ① 预取器本体：单飞/上限/原子落地/收尸纪律 ----
need('VideoPrefetchWorker' in pf and 'VideoPrefetcher' in pf,
     'video_prefetch.cpp 缺少 worker/调度器类定义')
need('m_inflight.contains(url)' in pf and 'm_inflight.size() >= 3' in pf,
     '预取没有单飞+在飞上限（会重复下载/和播放抢带宽）')
need('QFile::remove(m_finalPath)' in pf,
     'rename 前没有 remove（1.0.0.25 历史落盘静默失败回归）')
need('.part' in pf, '缺少 .part 中间态（半截文件会被误判为完整缓存）')
need('deleteLater' in pf, 'worker 没有 deleteLater 收尸')
need(re.search(r'removeAt|takeAt|removeAll|m_inflight\.remove', pf) is not None,
     '收尸时没有先从 m_inflight 摘链（先 deleteLater 会踩 1.0.0.11 UAF）')
need('trimCache' in pf, '缺少缓存封顶回收（Q20 沙盒会被预取塞满）')
# 【1.0.0.50】stale .part 收尸：trimCache 只扫 *.mp4、clearAll 无调用方，
# worker 被杀后 fopen 的 .part 无人收 —— 真机 1.0.0.49 躺着两个 13:57 的
# 孤儿（6.8MB+62.8MB=70MB）。prefetch() 入口限速清扫是唯一回收路径。
need('sweepStaleParts' in pf and 'sweepStaleParts(300)' in pf,
     '缺少 stale .part 收尸（dy_prefetch/.part 只增不减，白占沙盒）')
need('public QThread' in pfh, 'worker 不是 QThread（预取必须真后台，不能卡 UI 线程）')
# ---- ② store 接入：命中换源 + totalSize 用文件真实大小 + 三槽 ----
need('m_prefetch->hasLocal(cdnUrl)' in ds, 'startStream 没有预取命中分支（预取了却不用）')
need('PREFETCH hit' in ds, '缺少 PREFETCH hit 协议证据行')
need('src = localFile' in ds, '命中后没有把代理源换成 file://')
need('pfi.size()' in ds, '命中后 totalSize 没有改用文件真实大小（Content-Range 会不一致）')
need('void DouyinPlayerStore::prefetchNext' in ds, '缺少 prefetchNext 实现')
need('void DouyinPlayerStore::onPrefetchDone' in ds, '缺少 onPrefetchDone 槽')
need('void DouyinPlayerStore::onPrefetchFailed' in ds, '缺少 onPrefetchFailed 槽')
need('PREFETCH issue' in ds, '缺少 PREFETCH issue 证据行（自证预取真的发起了）')
# ---- ③ proxy 侧 file:// 分支：curl 7.24 不认 file:// RANGE，必须 seek+read ----
need('startsWith("file://")' in vs, 'ProxyStreamWorker 没有 file:// 分支')
need('void ProxyStreamWorker::runLocal' in vs, '缺少 runLocal 实现（file:// 必须 QFile seek+read）')
# ---- ④ Makefile 四处接入 ----
need(mk_s.count('video_prefetch') >= 4,
     'Makefile 接入不完整（需 SOURCES/OBJECTS/moc 规则/编译规则共 4+ 处）')
need('moc_video_prefetch.cpp: src/video_prefetch.hpp' in mk_s,
     'Makefile 缺少 video_prefetch 的 moc 规则（Q_OBJECT 不生成 → 链接期符号缺失）')
# ---- ⑤ QML 换片触发：playInline 内必须 prefetchNext(idx+1) ----
pi = fq_s.find('function playInline')
pi_win = fq_s[pi:pi+3000] if pi >= 0 else ''
need('prefetchNext' in pi_win, 'playInline 换片后没有触发下一片预取（预取器永远空转）')
need('rowAt(idx + 1)' in pi_win or 'rowAt(idx+1)' in pi_win,
     'playInline 没有取下一行（rowAt(idx+1)）')
# ---- ⑥ buildId 必须 >= 28（防忘了升版本号：装机后分不清是哪一版） ----
bid = re.search(r'<buildId>\s*(\d+)\s*</buildId>', desc_s)
if not bid:
    errs.append('bar-descriptor.xml 解析不到 buildId')
elif int(bid.group(1)) < 28:
    errs.append('buildId=%s < 28：预取特性没升版本号（改完铁律：每次迭代只改 buildId）' % bid.group(1))
if errs:
    for e in errs:
        print('  [V128] ' + e)
    sys.exit(1)
print('    1.0.0.28 预取器件自检通过（单飞/原子落地/收尸/file:// 分支/QML 触发/buildId=%s）'
      % (bid.group(1) if bid else '?'))
PYGATE9
then
    echo "!!! 1.0.0.28 预取器件自检未通过，拒绝打包"
    exit 1
fi

# ---------------------------------------------------------------------------
# 【1.0.0.29/.30 GATE10】五项需求轮防回归：
#   A. 设置/去登录可观测性：main.qml 七个 push 函数全部含 try/catch + "EX:" 折返，
#      openLogin 另含 if (!settingsDef) 守卫（1.0.0.28 真机零反馈根因）；
#      DouyinFeedPage 4 个调用点（去登录/设置/历史/评论）+ DouyinHistoryPage
#      复播点全部 try/catch 兜底（logSwipe 必执行、失败 showTip 明示）。
#   B. 图标美化：play/pause/refresh 三枚 PNG 存在且 ≥48px、PNG magic；
#      videoUiLayer 播放/暂停走 imageSource（禁止退回 "\u23F8"/"\u25B6" 文字字形）。
#   C. 右上角刷新：refreshFeed() 函数 + 顶条按钮（在「去登录」左侧）+ 埋点。
#   D. 发表评论：api postJson/commentPublishUrl（JSON 端点）、model publish/
#      onPublishReply（status_code 0/8 分支）、评论页输入条（cmdInput/pubBar）。
#   E. 网络埋点（1.0.0.30）：dyNetLog（同 bootLog 双目录）、每次 curl 结束
#      写 code/http/bytes（onCurlFinished/onDiagFinished/onDownloadFinished 三点），
#      CURLINFO_RESPONSE_CODE 必须两个 worker 都取 —— 1.0.0.28/.29 真机空 feed
#      只有请求行没有回流行，无法区分断网/限流/解析空，这就是让不可见变可见的一层。
#   E2. 一级诊断（1.0.0.31）：.30 真机首现 code=-99（构造初值未写）却收到
#      finished()，run() 三点埋点（enter/perform-enter/perform-exit+errbuf）+
#      curl_version + 5 处 start() 返回值，一次重启定位「线程没起 / 库行为异常」。
#   F. buildId ≥ 31。
# 锚点全部封闭在函数体/配对块内，不用全局同名查（照 GATE6/9 的教训）。
# ---------------------------------------------------------------------------
if ! python - "$PROJ_DIR/assets/main.qml" "$PROJ_DIR/assets/DouyinFeedPage.qml" \
             "$PROJ_DIR/assets/DouyinCommentPage.qml" "$PROJ_DIR/assets/DouyinHistoryPage.qml" \
             "$PROJ_DIR/assets/images/play.png" "$PROJ_DIR/assets/images/pause.png" \
             "$PROJ_DIR/assets/images/refresh.png" \
             "$PROJ_DIR/src/douyin_api.hpp" "$PROJ_DIR/src/douyin_api.cpp" \
             "$PROJ_DIR/src/douyin_store.hpp" "$PROJ_DIR/src/douyin_store.cpp" \
             "$PROJ_DIR/bar-descriptor.xml" <<'PYGATE10'
# -*- coding: utf-8 -*-
import os
import re
import struct
import sys

A = sys.argv[1:]
(main_qml, feed_qml, cmt_qml, hist_qml,
 play_png, pause_png, refresh_png,
 api_hpp, api_cpp, store_hpp, store_cpp,
 descriptor) = A

errs = []


def read(p):
    with open(p, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def func_body(src, name):
    """取 function NAME(...) 的函数体（到配对右花括号），找不到返回 None。"""
    m = re.search(r"function\s+" + re.escape(name) + r"\s*\([^)]*\)\s*\{", src)
    if not m:
        return None
    i = m.end() - 1          # 指向函数体的 {
    depth = 0
    for j in range(i, len(src)):
        c = src[j]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
    return None


# ---- A1. main.qml 七个 push 函数：try/catch + "EX:" 折返 ----
main_src = read(main_qml)
GUARD_FUNCS = ["openLogin", "pushComments", "pushHistory",
               "playHistoryItem", "pushNote", "pushDyVideo", "probe"]
for fn in GUARD_FUNCS:
    body = func_body(main_src, fn)
    if body is None:
        errs.append("main.qml 缺 function %s()" % fn)
        continue
    for token in ("try {", "catch (e)", "EX:"):
        if token not in body:
            errs.append("main.qml %s() 函数体缺 %r（失败必须折成返回值，"
                        "1.0.0.28 零反馈根因）" % (fn, token))
            break
# openSettings 是纯转发：内部 openLogin 已守卫，只要求它真的转发
if 'function openSettings()' not in main_src or "dyNav.openLogin()" not in main_src:
    errs.append("main.qml openSettings 缺 openLogin 转发")

# ---- A2. openLogin 的 def 守卫（结构弱点：settingsDef null 时 Type
#         Error 会吞掉调用方全部埋点）----
ol = func_body(main_src, "openLogin")
if ol is not None and "if (!settingsDef)" not in ol:
    errs.append("main.qml openLogin() 缺 if (!settingsDef) 守卫")

# ---- A3. DouyinFeedPage 四个调用点全部 try/catch 兜底 ----
feed_src = read(feed_qml)
for anchor in ('var r0 = "no-host";',     # 顶条 去登录
               'var r1 = "no-host";',     # 抽屉 设置
               'var r2 = "no-host";',     # 抽屉 历史
               'var rc = "no-host";'):    # 右侧 评论
    if anchor not in feed_src:
        errs.append("DouyinFeedPage 调用点缺 try/catch 兜底锚点 %s" % anchor)
for log_anchor in ("TOPBAR openLogin ret=", "MENU settings ret=",
                   "MENU history ret=", "CMT-BTN push ret="):
    if log_anchor not in feed_src:
        errs.append("DouyinFeedPage 缺结果埋点 %s" % log_anchor)
if feed_src.count("catch (e)") < 4:
    errs.append("DouyinFeedPage catch (e) 块 < 4（四个调用点都要）")

# ---- A4. ComponentDefinition 页面必须声明被注入的属性（1.0.0.33 真机实证）----
# openLogin/pushComments/pushHistory/pushDyVideo/pushNote 都会执行 page.nav = ...
# （history/note 另赋 host）。页面没声明该属性 → 赋值抛 TypeError。1.0.0.33
# 真机：SettingsPage 缺 property variant nav → openLogin 返回
# "EX:Error: Cannot assign to non-existent property \"nav\"" ——
# 这就是「设置/去登录点击无效」在 .28 里静默、.29 起可见的根因。
ASSETS_DIR = os.path.dirname(feed_qml)
NAV_PAGES = ["SettingsPage.qml", "DouyinCommentPage.qml", "DouyinHistoryPage.qml",
             "DouyinPlayerPage.qml", "NotePage.qml"]
HOST_PAGES = ["DouyinHistoryPage.qml", "NotePage.qml"]
for page in NAV_PAGES:
    pp = os.path.join(ASSETS_DIR, page)
    if not os.path.exists(pp):
        errs.append("A4 缺被注入页面文件 %s" % page)
        continue
    psrc = read(pp)
    if "property variant nav" not in psrc:
        errs.append("%s 缺 property variant nav（createObject 后 page.nav = "
                    "dyNav 会抛 TypeError，按钮点击零反馈）" % page)
for page in HOST_PAGES:
    psrc = read(os.path.join(ASSETS_DIR, page))
    if "property variant host" not in psrc:
        errs.append("%s 缺 property variant host（pushHistory/pushNote 注入）" % page)
# main.qml 里每个 ComponentDefinition 的 source 必须真实存在于 assets 根
for src_name in ("NotePage.qml", "ProbePage.qml", "DouyinPlayerPage.qml",
                 "SettingsPage.qml", "DouyinCommentPage.qml", "DouyinHistoryPage.qml"):
    if ('source: "%s"' % src_name) not in main_src:
        errs.append("main.qml 缺 ComponentDefinition source: \"%s\"" % src_name)

# ---- A4. DouyinHistoryPage 复播点 try/catch ----
hist_src = read(hist_qml)
if 'var rh = "no-host";' not in hist_src or "HIST-ITEM ret=" not in hist_src:
    errs.append("DouyinHistoryPage 复播点缺 try/catch + HIST-ITEM 埋点")

# ---- B. 图标：存在 + PNG magic + IHDR ≥48px ----
def png_size_ok(p):
    try:
        with open(p, "rb") as f:
            head = f.read(24)
    except OSError:
        return "missing"
    if len(head) < 24 or head[:8] != b"\x89PNG\r\n\x1a\n":
        return "not-png"
    w, h = struct.unpack(">II", head[16:24])
    if w < 48 or h < 48:
        return "too-small"
    return None


for label, p in (("play", play_png), ("pause", pause_png), ("refresh", refresh_png)):
    bad = png_size_ok(p)
    if bad:
        errs.append("图标 %s.png %s（%s）" % (label, bad, p))

# ---- B2. 播放/暂停按钮走 imageSource，禁止退回文字字形 ----
# 先剥注释（字符串保留）：本检查只认字形锚点，注释里提到历史写法不算违规
# （GATE6/9 的教训：不清注释的锚点会被自己的说明文字误伤）。
def strip_line_comments(src):
    """行注释剥离，字符串感知（asset:/// 里的 // 不是注释；\\" 转义不结束字符串）。
    照 qmlcheck_full 的 strip_comments 语义：注释清掉、字符串保留。"""
    out = []
    for ln in src.splitlines():
        res = []
        in_str = False
        i = 0
        n = len(ln)
        while i < n:
            c = ln[i]
            if in_str:
                res.append(c)
                if c == "\\" and i + 1 < n:
                    res.append(ln[i + 1])
                    i += 2
                    continue
                if c == '"':
                    in_str = False
                i += 1
                continue
            if c == '"':
                in_str = True
                res.append(c)
                i += 1
                continue
            if c == "/" and i + 1 < n and ln[i + 1] == "/":
                break
            res.append(c)
            i += 1
        out.append("".join(res))
    return "\n".join(out)


feed_code = strip_line_comments(feed_src)
for glyph in ('"\\u23F8"', '"\\u25B6"'):
    if glyph in feed_code:
        errs.append("DouyinFeedPage 仍含文字字形 %s（必须用 imageSource 图标，"
                    "BB10 上 Unicode 媒体符号渲染丑陋/豆腐块）" % glyph)
for need in ("asset:///images/play.png", "asset:///images/pause.png"):
    if need not in feed_code:
        errs.append("DouyinFeedPage 缺图标引用 %s" % need)

# ---- C. 右上角刷新 ----
if func_body(feed_src, "refreshFeed") is None:
    errs.append("DouyinFeedPage 缺 refreshFeed() 函数")
if "TOPBAR refresh kind=" not in feed_src:
    errs.append("DouyinFeedPage refreshFeed 缺埋点 TOPBAR refresh")
if "asset:///images/refresh.png" not in feed_src:
    errs.append("DouyinFeedPage 缺 refresh.png 按钮引用")
if "feedPage.refreshFeed();" not in feed_src:
    errs.append("DouyinFeedPage 缺刷新按钮 onClicked → refreshFeed()")
# 刷新按钮必须排在「去登录」按钮之前（右上角、登录左侧）
i_ref = feed_src.find("asset:///images/refresh.png")
i_login = feed_src.find('text: (biliApi && biliApi.loggedIn) ? "设置 >" : "去登录 >"')
if i_ref < 0 or i_login < 0 or i_ref > i_login:
    errs.append("刷新按钮位置错：必须在「去登录」之前（右上角）")

# ---- B3. 播放/暂停图标运行时证据（1.0.0.35：无 root 截屏的替代证明）----
# defaultImageSourceChanged NOTIFY 回调写 "ICON play/pause" —— 这是图标真的在
# 播放/暂停态之间切换的唯一可机读证据（截图通道要 root，见 dev_diag.c）。
# 【1.0.0.49】播放钮由 Button 改 ImageButton（去 chrome 背景，用户要求），
# NOTIFY 信号随之从 imageSourceChanged 换成 defaultImageSourceChanged
# （imagebutton.h:106 实证该 Q_PROPERTY 存在）——埋点意图不变，仅换锚点。
if "onDefaultImageSourceChanged: feedPage.logSwipe(\"ICON \"" not in feed_src:
    errs.append("DouyinFeedPage 播放/暂停按钮缺 onDefaultImageSourceChanged → ICON 埋点"
                "（图标切换无运行时证据）")

# ---- D1. 评论发表 C++ 器件 ----
ah = read(api_hpp)
ac = read(api_cpp)
sh = read(store_hpp)
sc = read(store_cpp)
if "void postJson(" not in ah:
    errs.append("douyin_api.hpp 缺 postJson 声明")
if "commentPublishUrl" not in ah:
    errs.append("douyin_api.hpp 缺 commentPublishUrl 声明")
if "aweme/v1/comment/publish/" not in ac:
    errs.append("douyin_api.cpp 缺 /aweme/v1/comment/publish/ 端点")
if 'return QString("https://www.iesdouyin.com/aweme/v1/comment/publish/")' not in ac:
    errs.append("douyin_api.cpp commentPublishUrl() 的 return 语句端点不对"
                "（必须是 https://www.iesdouyin.com/aweme/v1/comment/publish/）")
if '"application/json"' not in ac:
    errs.append("douyin_api.cpp postJson 缺 Content-Type: application/json")
if "Q_INVOKABLE void publish(" not in sh:
    errs.append("douyin_store.hpp 缺 Q_INVOKABLE publish()")
if "publishTip" not in sh:
    errs.append("douyin_store.hpp 缺 publishTip Q_PROPERTY")
if "void DouyinCommentModel::publish(const QString& text)" not in sc:
    errs.append("douyin_store.cpp 缺 DouyinCommentModel::publish 实现")
if "void DouyinCommentModel::onPublishReply(" not in sc:
    errs.append("douyin_store.cpp 缺 onPublishReply（:pub 后缀路由）")
if '":pub"' not in sc:
    errs.append("douyin_store.cpp onNetReply 缺 :pub 路由")
if "code == 8" not in sc:
    errs.append("douyin_store.cpp 缺 status_code==8（未登录）分支")

# ---- D2. 评论页输入条 ----
cs = read(cmt_qml)
for need in ('objectName: "cmtInput"', 'objectName: "pubBar"',
             "dyCommentModel.publish(cmtInput.text)",
             "SubmitKey.Send", "publishTip"):
    if need not in cs:
        errs.append("DouyinCommentPage 缺发表器件 %s" % need)
if "cmtPubTip" not in cs:
    errs.append("DouyinCommentPage 缺 cmtPubTip 影子属性（成功后清空输入框）")

# ---- E. 网络埋点：每次 curl 结束必留 code/http/bytes ----
if "void dyNetLog(" not in ac:
    errs.append("douyin_api.cpp 缺 dyNetLog()（网络层写 boot log 的通道，"
                "设备取不到 slog2）")
if "net reply tag=" not in ac:
    errs.append("douyin_api.cpp onCurlFinished 缺 'net reply tag=' 埋点")
if "net diag verdict" not in ac:
    errs.append("douyin_api.cpp onDiagFinished 缺 'net diag verdict' 埋点")
if "net dl reply" not in ac:
    errs.append("douyin_api.cpp onDownloadFinished 缺 'net dl reply' 埋点")
if ac.count("CURLINFO_RESPONSE_CODE") < 2:
    errs.append("douyin_api.cpp 两个 worker 都要取 CURLINFO_RESPONSE_CODE"
                "（cur=%d）" % ac.count("CURLINFO_RESPONSE_CODE"))
if "m_httpCode" not in ac:
    errs.append("douyin_api.cpp 缺 m_httpCode 成员")
# ---- E2. .30 一级诊断（真机 code=-99 铁证：perform 没执行但 finished() 到了）----
for need in ("net worker enter tag=", "net worker perform-enter tag=",
             "net worker perform-exit tag=", "net curl ver=",
             "CURLOPT_ERRORBUFFER"):
    if need not in ac:
        errs.append("douyin_api.cpp 缺一级诊断锚点 %s" % need)

# ---- G. 测试命令通道（1.0.0.33）：C++ 轮询 dy_test_cmd.txt → Q_PROPERTY → QML 分发 ----
# 为什么必须有门禁：这是无 root 环境下四大功能真机验证的唯一通道，
# 任何一环被删（轮询没起 / 属性没 NOTIFY / QML 没分发 / 按钮没 refactor 抽出）
# 都会静默退回"点了没反应且零日志"，与 1.0.0.28 事故同型。
auh = read(os.path.join(os.path.dirname(api_hpp), "applicationui.hpp"))
auc = read(os.path.join(os.path.dirname(api_cpp), "applicationui.cpp"))
if "Q_PROPERTY(QString testCmd READ testCmd NOTIFY testCmdChanged)" not in auh:
    errs.append("applicationui.hpp 缺 testCmd Q_PROPERTY（C++→QML 唯一可靠通道）")
if "void testCmdChanged();" not in auh:
    errs.append("applicationui.hpp 缺 testCmdChanged 信号声明")
if "void onTestCmdTick();" not in auh:
    errs.append("applicationui.hpp 缺 onTestCmdTick 轮询槽")
for need in ("dy_test_cmd.txt", "TESTCMD recv ", "TESTCMD dispatch ",
             "test cmd channel armed"):
    if need not in auc:
        errs.append("applicationui.cpp 缺测试命令通道锚点 %s" % need)
if "m_testCmdTimer->start();" not in auc:
    errs.append("applicationui.cpp 缺 m_testCmdTimer->start()（轮询没武装）")
if func_body(feed_src, "runTestCmd") is None:
    errs.append("DouyinFeedPage 缺 runTestCmd() 分发函数（appTestCmd 绑定的落点）")
for need in ("TESTCMD exec ", "doTopbarLogin", "doMenuSettings"):
    if need not in feed_src:
        errs.append("DouyinFeedPage 测试命令分发缺 %s" % need)
if '"togglePlay"' not in feed_src:
    errs.append("DouyinFeedPage 测试命令分发缺 togglePlay 分支（图标态无法程序化驱动）")
# refactor 后按钮必须仍走同一函数（防改回去：onClicked 里重抄一份逻辑就会漂移）
if "onClicked: feedPage.doTopbarLogin()" not in feed_src:
    errs.append("顶条「去登录」按钮未走 doTopbarLogin()（refactor 被改回）")
if "onClicked: feedPage.doMenuSettings()" not in feed_src:
    errs.append("抽屉「设置」按钮未走 doMenuSettings()（refactor 被改回）")
if "onAppTestCmdChanged: feedHome.runTestCmd(" not in main_src:
    errs.append("main.qml 缺 onAppTestCmdChanged → feedHome.runTestCmd 分发")

# ---- F. buildId ≥ 33 ----
bid = re.search(r"<buildId>\s*(\d+)\s*</buildId>", read(descriptor))
if not bid:
    errs.append("bar-descriptor.xml 缺 buildId")
elif int(bid.group(1)) < 36:
    errs.append("buildId=%s < 36（1.0.0.36 未升版）" % bid.group(1))

if errs:
    for e in errs:
        print("  [V131] " + e)
    sys.exit(1)
print("    1.0.0.31 自检通过（可观测性/图标/刷新/发表/网络埋点/诊断/buildId=%s）"
      % (bid.group(1) if bid else "?"))
PYGATE10
then
    echo "!!! 1.0.0.30 GATE10 自检未通过，拒绝打包"
    exit 1
fi

# ---------------------------------------------------------------------------
# 【1.0.0.37 GATE12】六项新需求防回归（用户原话逐条对应）：
#   G. 残留帧：DouyinFeedPage switching + switchMask + stop 前置换流窗口 +
#      FWC mask ON/OFF 埋点；暂停符换流期不亮。
#   H. 发表空 200：douyin_store.cpp onPublishReply 空 body 独立分支
#      （"empty-200" + 协议证据文案），不再落"未知错误"；
#      DouyinCommentPage pubTip 改居中浮动 toast（pubToast）。
#   I. 收藏：douyin_store DouyinFavoriteModel（favVersion/favText）+ main.qml
#      favDef/pushFavorites + DouyinFavoritePage（nav/host 声明）+ 菜单项。
#   J. 下载：douyin_store DouyinDownloadStore（__dyvid__ tag / progressFor /
#      start/cancel/remove）+ main.qml downloadDef/pushDownloads +
#      DouyinDownloadPage + 视频页 DL-BTN。
#   K. 加载诊断：loadDetail Q_INVOKABLE + READY elapsed + IPRESOLVE_V4 两点。
#   L. buildId ≥ 37。
# ---------------------------------------------------------------------------
if ! python - "$PROJ_DIR/src/douyin_store.hpp" "$PROJ_DIR/src/douyin_store.cpp" \
             "$PROJ_DIR/src/douyin_api.cpp" \
             "$PROJ_DIR/src/video_prefetch.cpp" \
             "$PROJ_DIR/assets/DouyinFeedPage.qml" "$PROJ_DIR/assets/main.qml" \
             "$PROJ_DIR/assets/DouyinCommentPage.qml" \
             "$PROJ_DIR/assets/DouyinFavoritePage.qml" \
             "$PROJ_DIR/assets/DouyinDownloadPage.qml" \
             "$PROJ_DIR/bar-descriptor.xml" <<'PYGATE12'
# -*- coding: utf-8 -*-
import os
import re
import sys

A = sys.argv[1:]
(store_hpp, store_cpp, api_cpp, prefetch_cpp, feed_qml, main_qml, cmt_qml,
 fav_qml, dl_qml, descriptor) = A

errs = []


def read(p):
    with open(p, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def func_body(src, name):
    """取 function NAME(...) 的函数体（到配对右花括号），找不到返回 None。"""
    m = re.search(r"function\s+" + re.escape(name) + r"\s*\([^)]*\)\s*\{", src)
    if not m:
        return None
    i = m.end() - 1
    depth = 0
    for j in range(i, len(src)):
        c = src[j]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
    return None


def strip_comments(src):
    """去掉 // 与 /* */ 注释但保留字符串内容（防注释里的 token 误报/漏报）。"""
    out = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c == '"' or c == "'":
            q = c
            out.append(c)
            i += 1
            while i < n:
                out.append(src[i])
                if src[i] == "\\" and i + 1 < n:
                    out.append(src[i + 1])
                    i += 2
                    continue
                if src[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            i += 2
            while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                i += 1
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


sh = read(store_hpp)
sc = read(store_cpp)
feed = read(feed_qml)
main = read(main_qml)
cmt = read(cmt_qml)
favsrc = read(fav_qml)
dlsrc = read(dl_qml)
apisrc = read(api_cpp)
prefetchsrc = read(prefetch_cpp)

# ---- G. 残留帧修复 ----
for need in ('property bool switching: false',
             'property bool vplaying: videoPlayer ? videoPlayer.playing : false',
             'id: switchMask',
             'visible: feedPage.switching'):
    if need not in feed:
        errs.append("DouyinFeedPage 残留帧修复缺锚点 %r" % need)
if 'feedPage.switching = true' not in feed:
    errs.append("DouyinFeedPage 换流前未置 switching = true（残留帧会复现）")
if feed.count('feedPage.switching = true') < 4:
    errs.append("switching = true 置位点 < 4（playInline/switchFeed/refreshFeed/playRow 四处都要）")
if 'FWC mask ON switch id=' not in feed or 'FWC mask OFF first-frame id=' not in feed:
    errs.append("DouyinFeedPage 缺 FWC mask ON/OFF 埋点（机读证据）")
if feed.count('FWC mask ON switch id=') < 2:
    errs.append("FWC mask ON 埋点 < 2 处（playInline/playRow 两条换流路径都要，否则机读证据断链）")
if '&& !feedPage.switching' not in feed:
    errs.append("中央暂停符未排除换流窗口（switching 期会亮大播放键误导）")

# ---- H. 发表空 200 + toast ----
if 'json.trimmed().isEmpty()' not in sc:
    errs.append("douyin_store.cpp onPublishReply 缺空 body 分支（会显示未知错误）")
if 'empty-200' not in sc:
    errs.append("douyin_store.cpp 缺 empty-200 协议证据埋点")
if '服务端返回空内容' not in sc:
    errs.append("空 body 分支缺可读结论文案")
if 'id: pubToast' not in cmt:
    errs.append("DouyinCommentPage 缺居中浮动 toast（pubTip 位置错误回归）")
if 'cmt PUB-TIP ' not in sc:
    errs.append("setPublishTip 缺机读留痕（用户看到的提示在日志里不可查）")
if 'id: pubTip' in cmt:
    errs.append("DouyinCommentPage 又把提示挪回底部 pubBar（pubTip 旧位复现）")

# ---- I. 收藏 ----
for need in ('class DouyinFavoriteModel',
             'Q_PROPERTY(int favVersion READ favVersion NOTIFY favVersionChanged)',
             'Q_INVOKABLE bool toggle(const QVariantMap& row)',
             'Q_INVOKABLE QString favText(const QString& awemeId, int ver) const'):
    if need not in sh:
        errs.append("douyin_store.hpp 收藏模型缺 %r" % need)
if 'douyin_favorite.json' not in sc:
    errs.append("douyin_store.cpp 收藏模型缺持久化文件锚点")
for need in ('id: favDef', 'function pushFavorites()', 'function playFeedRow(row)'):
    if need not in main:
        errs.append("main.qml 收藏链路缺 %r" % need)
pf = func_body(main, "pushFavorites")
if pf is not None:
    for token in ("try {", "catch (e)", "EX:"):
        if token not in pf:
            errs.append("main.qml pushFavorites() 缺 %r" % token)
for page in (fav_qml, dl_qml):
    src = strip_comments(read(page))
    if 'property variant nav: null' not in src or 'property variant host: null' not in src:
        errs.append("%s 缺 nav/host 属性声明（注入即 TypeError）" % os.path.basename(page))
    if 'type: "item"' in src:
        errs.append("%s 出现 type: \"item\"（ItemType 默认空串，写了只剩 bvid）" % os.path.basename(page))
if 'text: "收藏"' not in feed or 'text: "已下载"' not in feed:
    errs.append("DouyinFeedPage 抽屉菜单缺「收藏 / 已下载」入口")
if 'FAV-BTN id=' not in feed or 'DL-BTN id=' not in feed:
    errs.append("DouyinFeedPage 缺收藏/下载按钮埋点")
if 'dyFavModel.favText(' not in feed or 'dyDownloadStore.dlText(' not in feed:
    errs.append("DouyinFeedPage 按钮文字未走 favText/dlText（版本锚点机制）")

# ---- J. 下载 ----
for need in ('class DouyinDownloadStore',
             'Q_PROPERTY(int dlVersion READ dlVersion NOTIFY dlVersionChanged)',
             'Q_INVOKABLE void start(const QVariantMap& row)',
             'Q_INVOKABLE QString progressFor(const QString& awemeId, int ver) const',
             '__dyvid__'):
    if need not in sh:
        errs.append("douyin_store.hpp 下载 Store 缺 %r" % need)
if 'douyin_downloads.json' not in sc:
    errs.append("douyin_store.cpp 下载索引缺持久化锚点")
if 'onDownloadFinished(const QString& tag' not in sc:
    errs.append("douyin_store.cpp 缺 downloadFinished 接收槽（下载完成无回调）")
if 'QVariantMap DouyinDownloadStore::rowAt' not in sc:
    errs.append("DouyinDownloadStore 缺 rowAt 实现（已下载页点击=静默 TypeError）")
if sh.count('Q_INVOKABLE QVariantMap rowAt(int row) const;') < 4:
    errs.append("douyin_store.hpp 四个行模型（feed/history/fav/dl）都要声明 rowAt（QML 调用点静默失败之源，1.0.0.41 漏 dl）")
if 'id: downloadDef' not in main:
    errs.append("main.qml 缺 downloadDef 工厂")
pd = func_body(main, "pushDownloads")
if pd is not None:
    for token in ("try {", "catch (e)", "EX:"):
        if token not in pd:
            errs.append("main.qml pushDownloads() 缺 %r" % token)
if 'progressFor(ListItemData.awemeId, dyDownloadStore.dlVersion)' not in dlsrc:
    errs.append("DouyinDownloadPage 行内进度缺版本锚点（dlVersion 不进依赖图）")

# ---- K. 加载诊断 ----
if 'Q_INVOKABLE void loadDetail' not in sh:
    errs.append("loadDetail 未开放 Q_INVOKABLE（重试换新地址不可达）")
if 'READY elapsed=' not in sc:
    errs.append("startStream 缺 READY elapsed 分层埋点（起播耗时不可测）")
if 'LOAD id=%1 src=%2' not in sc:
    errs.append("DouyinPlayerStore::load 缺源类型埋点（本地/CDN 不可分辨，本地播放无法自证）")
for c in ("dlplay", "favplay", "dlrm"):
    if ('cmd === "' + c + '"') not in feed:
        errs.append("DouyinFeedPage 缺 %s 测试命令（已下载/收藏列表项点击无机读路径）" % c)
if apisrc.count('CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4') < 2:
    errs.append("douyin_api.cpp 两个 worker 都要 CURLOPT_IPRESOLVE_V4（IPv6 跳转=curl 7）")
if '__dyvid__' not in apisrc or '"0-"' not in apisrc:
    errs.append("dyvid 视频下载未补 Range: 0-（无 Range 整文件 GET=CDN 302 跳 IPv6 字面量，1.0.0.43 真机复现）")
if prefetchsrc.count('CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4') < 1:
    errs.append("video_prefetch.cpp 缺 CURLOPT_IPRESOLVE_V4")
if prefetchsrc.count('Range: 0-') < 1 or strip_comments(prefetchsrc).count('Range: 0-') < 1:
    errs.append("video_prefetch.cpp 缺 Range: 0-（预取同款 302->IPv6 死路）")
if 'loadDetail(id)' not in feed:
    errs.append("DouyinFeedPage 重试播放未升级为 loadDetail 换新地址")

# ---- L. buildId ≥ 37 ----
bid = re.search(r"<buildId>\s*(\d+)\s*</buildId>", read(descriptor))
if not bid:
    errs.append("bar-descriptor.xml 缺 buildId")
elif int(bid.group(1)) < 37:
    errs.append("buildId=%s < 37（1.0.0.37 未升版）" % bid.group(1))

if errs:
    for e in errs:
        print("  [V137] " + e)
    sys.exit(1)
print("    1.0.0.37 GATE12 自检通过（残留帧/空200 toast/收藏/下载/加载诊断/buildId=%s）"
      % (bid.group(1) if bid else "?"))
PYGATE12
then
    echo "!!! 1.0.0.37 GATE12 自检未通过，拒绝打包"
    exit 1
fi

echo "==> [0/6] 生成版本号头文件（数据源：bar-descriptor.xml）"
# 【为什么在构建时生成，而不是在 C++ 里写死】
# 版本号只在 bar-descriptor.xml 里维护一处，改完 buildId 直接跑本脚本，
# app_version.hpp 自动跟着变 —— 不会出现"设置页显示 1.0.0.88、安装包其实是 .90"。
# 生成之后必须删掉 applicationui.o：Makefile 的依赖列表里没有这个新头文件，
# 不删的话 make 认为 applicationui.cpp 没变，版本号会一直停留在上一次编译的值。
APP_VER=$(sed -n 's:.*<versionNumber>\([^<]*\)</versionNumber>.*:\1:p' "$PROJ_DIR/bar-descriptor.xml" | head -1 | tr -d ' \r')
APP_BID=$(sed -n 's:.*<buildId>\([^<]*\)</buildId>.*:\1:p' "$PROJ_DIR/bar-descriptor.xml" | head -1 | tr -d ' \r')
if [ -z "$APP_VER" ] || [ -z "$APP_BID" ]; then
    echo "!!! 无法从 bar-descriptor.xml 解析版本号（versionNumber='$APP_VER' buildId='$APP_BID'）"
    exit 1
fi
cat > "$PROJ_DIR/src/app_version.hpp" <<EOF
#ifndef APP_VERSION_HPP
#define APP_VERSION_HPP
// 本文件由 build.sh 自动生成，请勿手工编辑。
// 数据源：bar-descriptor.xml 的 <versionNumber> + <buildId>。
#define APP_VERSION_STRING "$APP_VER.$APP_BID"
#endif
EOF
rm -f "$OBJ_DIR/applicationui.o"
echo "    版本号 = $APP_VER.$APP_BID"

echo "==> [1/6] 同步源码"
cd "$BUILD_DIR" || exit 1
cp -f "$PROJ_DIR/src/"*.cpp "$PROJ_DIR/src/"*.hpp src/ || exit 1
cp -f "$PROJ_DIR/bar-descriptor.xml" . 2>/dev/null
# Makefile 是手工维护的（qmake 生成后改过），也要同步，否则本机改了 SOURCES/OBJECTS
# （比如新增 bbplayer_wrapper.cpp）不会进构建。注意：下面 [2/6] 会修正 moc 路径，
# 所以这里拷的是 qmake 原样的 /.moc/，[2/6] 会把它改成 ../../.moc/。
cp -f "$PROJ_DIR/Makefile" . 2>/dev/null
# 应用图标：bar-descriptor 里 <asset path="icon.png"> 指的是**构建目录**下的
# icon.png，不是 assets 里的。漏拷这一行的话，改了源图标但包里还是旧图标
# （构建目录里那份是历史遗留的，会一直沿用下去）。
cp -f "$PROJ_DIR/icon.png" . 2>/dev/null
# 资源：含 components 子目录（FeedPage.qml / RegionChip.qml 在这里，漏拷会导致 QML 导入失败）
mkdir -p o.le-v7/assets/components assets/components
# 播放控件图标（v67：从 BBTube 的 bb-shared/media/images 复用 —— disc/play/pause/knob
# 这几个是 BBTube 真机验证过的，96x96 带 alpha，配 filterColor 染色）。
# 漏拷的后果是 ImageView 空白，中央播放按钮变成一个看不见的透明块。
mkdir -p o.le-v7/assets/images assets/images
cp -f "$PROJ_DIR/assets/images/"*.png o.le-v7/assets/images/ 2>/dev/null
# .amd 是九宫格（9-slice）资源描述，缺了它圆角图会被当普通图整张拉伸，
# 圆角就变成"被拉扁的圆角"。文件名必须是 <png名>.amd。
cp -f "$PROJ_DIR/assets/images/"*.amd o.le-v7/assets/images/ 2>/dev/null
cp -f "$PROJ_DIR/assets/"*.qml o.le-v7/assets/ 2>/dev/null
cp -f "$PROJ_DIR/assets/"*.png o.le-v7/assets/ 2>/dev/null
# CA 根证书包：libbbnet 的静态 curl 默认找不到系统 CA 库，必须显式
# bbnet_ca_add_file(<path>) 装载，否则所有 https 请求报
# "Problem with the SSL CA cert (path? access rights?)"（v50 真机实证）。
cp -f "$PROJ_DIR/assets/"*.pem o.le-v7/assets/ 2>/dev/null
cp -f "$PROJ_DIR/assets/components/"*.qml o.le-v7/assets/components/ 2>/dev/null
# 【回归真机验证架构】components/ 下不放 qmldir。
# ground truth：设备可运行的 BiliClient.bar 里 native/assets/components/ 只有
# FeedPage.qml / RegionChip.qml / VideoRow.qml 三个 .qml，没有 qmldir，
# 根 QML 的 import "components" 依然正常解析（Qt 4.8 隐式目录导入）。
# 更关键的是：components/*.qml 内部不能写 `import "components"` —— 相对路径
# 会解析到不存在的 components/components/，报
#   import "components" has no qmldir and no namespace
# 并级联成 Type HomePage unavailable。此规则已由下方 QML 自检门禁强制拦截。
# 构建目录里可能有历史遗留的 qmldir，必须删干净，否则 BAR 里仍会带上。
rm -f o.le-v7/assets/components/qmldir assets/components/qmldir
cp -f "$PROJ_DIR/assets/"*.qml assets/ 2>/dev/null
cp -f "$PROJ_DIR/assets/"*.pem assets/ 2>/dev/null

echo "==> [1b] 同步 BBTube 逆向复用的流式播放后端 .so"
# libbbplayer.so（OpenMAX AL + 静态 FFmpeg）+ libbbnet.so（curl 流式 reader）。
# SONAME 均为 libbbplayer.so / libbnet.so（无版本后缀），与 BBTube.so 的 NEEDED 一致；
# 运行时由 VideoPlayer 通过 dlopen 加载，落到 app/native/lib（LD_LIBRARY_PATH 已含）。
cp -f "E:/PCdesk/桌面/bilibili/bbtube_extracted/native/lib/libbbplayer.so" o.le-v7/ 2>/dev/null
cp -f "E:/PCdesk/桌面/bilibili/bbtube_extracted/native/lib/libbbnet.so" o.le-v7/ 2>/dev/null
if [ ! -f o.le-v7/libbbplayer.so ] || [ ! -f o.le-v7/libbbnet.so ]; then
    echo "!!! 未找到 libbbplayer.so / libbnet.so（逆向素材路径变了？）"
    exit 1
fi

echo "==> [2/6] 修正 Makefile 中的 moc 路径（防坑 4）"
# 只在 `/ .moc/` 前面不是 `..` 时才替换，否则会把已正确的 ../../.moc/ 改成 ../..../../.moc/
if grep -qE '(^|[[:space:]])/\.moc/' Makefile 2>/dev/null; then
    sed -i -E 's#(^|[[:space:]])/\.moc/#\1../../.moc/#g' Makefile
    echo "    已修正 Makefile 中缺失盘符的 moc 路径"
fi

# 播放器用 bb::multimedia::MediaPlayer（libbbmultimedia.so，系统自带）。
# Makefile 是手工维护的，不会被 qmake 重新生成，所以这里幂等地补 -l。
# 漏掉这条的后果：编译零告警、链接成功，但设备上 dlopen 解析失败 → 应用完全打不开。
if ! grep -q "lbbmultimedia" Makefile 2>/dev/null; then
    sed -i 's/^LIBS\( *\)= \(.*\)$/LIBS\1= \2 -lbbmultimedia/' Makefile
    echo "    已在 Makefile 的 LIBS 中补 -lbbmultimedia"
fi

echo "==> [3/6] 强制重新生成 moc"
rm -f "$MOC_DIR"/moc_*.cpp "$OBJ_DIR"/moc_*.o
echo "    已清理旧 moc，make 将按头文件重新生成"

echo "==> [4/6] 编译（qcc -Vgcc_ntoarmv7le）"
export QNX_HOST QNX_TARGET
export PATH="$QNX_HOST/usr/bin:$PATH"
"$QNX_HOST/usr/bin/make.exe" -j2 > make_log.txt 2>&1

# 真实编译错误（排除已知无害的 ln -s / ntoarm-objcopy）
REAL_ERR=$(grep -E "error:" make_log.txt | grep -v "ntoarm-objcopy" | head -20)
if [ -n "$REAL_ERR" ]; then
    echo "!!! 编译失败："
    echo "$REAL_ERR"
    exit 1
fi
if [ ! -f libBiliClient.so.1.0.0 ]; then
    echo "!!! 未生成 libBiliClient.so.1.0.0"
    tail -20 make_log.txt
    exit 1
fi
WARN=$(grep -cE "warning:" make_log.txt)
echo "    编译完成，告警数=$WARN"

# 关键自检：moc 必须真的重新生成过，否则元对象可能是旧的
if [ ! -f "$MOC_DIR/moc_bili_api.cpp" ] || [ ! -f "$MOC_DIR/moc_feed_model.cpp" ]; then
    echo "!!! moc 未生成，元对象会缺失，应用将无法启动"
    exit 1
fi
if ! grep -q "netReply(QString,QString,bool)" "$MOC_DIR/moc_bili_api.cpp"; then
    echo "!!! moc_bili_api.cpp 缺少 netReply 信号（moc 未更新）"
    exit 1
fi
if ! grep -q "onNetReply(QString,QString,bool)" "$MOC_DIR/moc_feed_model.cpp"; then
    echo "!!! moc_feed_model.cpp 缺少 onNetReply 槽（moc 未更新）"
    exit 1
fi
echo "    moc 自检通过"

# 关键自检 2：未解析的 bb:: 符号。
# ELF 共享库允许把符号留到运行期解析 —— 编译零告警、链接成功、打包成功，
# 但设备上 dlopen 找不到这些符号，应用完全打不开，连崩溃日志都没有。
# Makefile 的 LIBS 里只有 -lbbdata -lbbsystem -lbps（没有 -lbbdevice / -lbbplatform），
# 所以只要出现这些命名空间的 U 符号，就必然是坑。
UNRESOLVED=$(ntoarm-nm.exe -u libBiliClient.so.1.0.0 2>/dev/null \
             | grep -E "_ZN2bb6device|_ZN2bb8platform|_ZN2bb7network|_ZN2bb11permissions|_ZN2bb10multimedia")
if [ -n "$UNRESOLVED" ]; then
    echo "!!! 发现未解析的 bb:: 符号（LIBS 未链接对应库）："
    echo "$UNRESOLVED"
    echo "    这类符号编译不报错，但设备上 dlopen 失败 → 应用完全打不开。"
    echo "    解决：改用纯 QML 实现，或在 Makefile 的 LIBS 里补 -l<库名>"
    exit 1
fi
echo "    符号自检通过（无未链接的 bb:: 依赖）"

# ---------------------------------------------------------------------------
# 【1.0.0.32 GATE11】跨 .o 弱符号碰撞自检（ODR 违规）。
# 根因回顾（1.0.0.29 回归，两天的空 feed 都是它）：douyin_api.cpp 与
# bili_api.cpp（BiliBar 遗留，仍参与链接）各自定义了同名 class CurlWorker /
# CurlDownloadWorker（QThread 子类）。GCC 把这些成员函数叮成弱符号(W)，
# 链接期 bili_api.o 靠前 → bili 的 run()/vtable 被选中、douyin 的成死码。
# .28 及以前两个类成员布局一致，相安无事；.29 给 douyin 版加了
# m_contentType（.30 又加 m_httpCode）→ 偏移错位：
# perform 的返回值写进 m_contentType 槽，onCurlFinished 的 code() 读
# 偏移 +4 处从未被写的 m_code 槽 → code 恒 -99、bytes 恒 0，表现为
# 「请求发出 23 秒零回复、run() 埋点不打印」的灵异现象。
# 本门禁：.o 级扫跨文件弱符号定义重叠（Qt 内联例行噪声除外）+ 源码级
# 禁用两个碰撞类名（douyin 侧必须 Dy 前缀）。修法：改名 DyCurlWorker /
# DyCurlDownloadWorker，符号唯一，douyin 的 run() 才真正上场。
# ---------------------------------------------------------------------------
export GATE11_OBJDIR="$OBJ_DIR" NTOARM_NM="$QNX_HOST/usr/bin/ntoarm-nm.exe"
if ! python - "$PROJ_DIR/src/douyin_api.cpp" "$PROJ_DIR/src/douyin_api.hpp" <<'PYGATE11'
# -*- coding: utf-8 -*-
import os
import subprocess
import sys

api_cpp, api_hpp = sys.argv[1], sys.argv[2]
errs = []

# ---- A. 源码级：douyin 侧禁用两个碰撞类名 ----
for label, p in (("douyin_api.cpp", api_cpp), ("douyin_api.hpp", api_hpp)):
    src = open(p, "r", encoding="utf-8", errors="replace").read()
    for bad in ("class CurlWorker", "class CurlDownloadWorker"):
        if bad in src:
            errs.append("%s 仍含 %r（与 bili_api.cpp ODR 碰撞：bili 的 run() 会"
                        "在链接期胜出，douyin 的 run() 成死码）" % (label, bad))
    for need in ("class DyCurlWorker", "class DyCurlDownloadWorker"):
        if need not in src:
            errs.append("%s 缺 %r（Dy 前缀是防 ODR 的唯一手段）" % (label, need))

# ---- B. .o 级：跨文件弱符号定义重叠 ----
NM = os.environ.get("NTOARM_NM") or "ntoarm-nm.exe"
OBJ_DIR = os.environ.get("GATE11_OBJDIR") or os.path.abspath("/.obj")
try:
    import shutil
    if not shutil.which(NM):
        NM = r"D:\bbndk\host_10_1_0_132\win32\x86\usr\bin\ntoarm-nm.exe"
except Exception:
    NM = r"D:\bbndk\host_10_1_0_132\win32\x86\usr\bin\ntoarm-nm.exe"
targets = ["bili_api.o", "douyin_api.o", "douyin_store.o", "applicationui.o",
           "feed_model.o", "video_store.o", "video_prefetch.o", "collect_store.o",
           "qrcodegen.o", "bbplayer_wrapper.o", "main.o"]
objs = [os.path.join(OBJ_DIR, t) for t in targets]
objs = [o for o in objs if os.path.exists(o)]
if len(objs) < len(targets):
    # .o 不全（比如首次构建）时只做源码级检查
    print("    [GATE11] .o 不全，仅做源码级检查（%d/%d）" % (len(objs), len(targets)))
else:
    # Qt 内联函数 / Qt 值类型（QString、QByteArray……）在多个 .o 里各出一份是
    # 正常的（同一头文件展开、内容相同），链接期选哪份都一样，不算 ODR 违规。
    # 判定：把 mangling 的 _ZN/_ZNK/_ZTV… 前缀剥掉，看剩下的类名是不是 Qt 的。
    QT_NAMES = ("7QString", "10QByteArray", "11QStringList", "8QVariant",
                "4QMap", "5QList", "5QHash", "9QHashNode", "11QHashData",
                "11QMapNode", "9QHashData", "6QDebug", "6qDebug", "12QMutexLocker", "15QBasicAtomicInt",
                "7QObject", "11QMetaObject", "10QTextStream", "6QTimer",
                "8QDateTime", "5QFile", "4QDir", "9QWriteLocker", "9QReadLocker",
                "11QMutexLocker", "11QWaitCondition", "9QSemaphore", "7QThread")
    OPERATORS = ("pl", "ls", "eq", "ne", "ml", "mi", "aS", "ix")

    def is_qt_noise(sym):
        core = sym
        for pre in ("_ZTVN", "_ZTIN", "_ZTS", "_ZThn", "_ZThn", "_ZNK", "_ZN", "_Z"):
            if core.startswith(pre):
                core = core[len(pre):]
                break
        return any(core.startswith(n) for n in QT_NAMES) or \
            any(core.startswith(o) for o in OPERATORS)

    def weak_defs(o):
        out = subprocess.run([NM, "--defined-only", o], capture_output=True,
                             text=True, errors="replace").stdout
        res = set()
        for ln in out.splitlines():
            p = ln.split()
            if len(p) >= 3 and p[1] in ("W", "w"):
                res.add(p[-1])
        return res

    sets = {o: weak_defs(o) for o in objs}
    for i, a in enumerate(objs):
        for b in objs[i + 1:]:
            inter = {s for s in (sets[a] & sets[b]) if not is_qt_noise(s)}
            if inter:
                errs.append("弱符号跨 .o 重叠 %s <-> %s：%s"
                            % (os.path.basename(a), os.path.basename(b),
                               ", ".join(sorted(inter)[:8])))

if errs:
    for e in errs:
        print("  [V132/ODR] " + e)
    sys.exit(1)
print("    GATE11 通过（无跨 .o 弱符号碰撞；douyin 侧已用 Dy 前缀）")
PYGATE11
then
    echo "!!! ODR 弱符号碰撞自检未通过，拒绝打包"
    exit 1
fi

echo "==> [5/6] 补齐 so 副本（替代 ln -s）"
for v in libBiliClient.so libBiliClient.so.1 libBiliClient.so.1.0; do
    cp -f libBiliClient.so.1.0.0 "$v"
done
cp -f libBiliClient.so.1.0.0 o.le-v7/BiliClient.so

echo "==> [6/6] 打包 devMode BAR（NDK JDK 1.6）"
# 产物名跟着应用名走：BiliClient.bar → DouyinBar.bar。
# 注意 libBiliClient.so / o.le-v7/BiliClient.so 不改名 —— 那是 descriptor 里
# <asset entry="true"> 指定的入口 so 名，改了要同步改 descriptor 和 Makefile 的 TARGET。
BAR_NAME="DouyinBar.bar"
rm -f "$BAR_NAME" BiliClient.bar
"$NDK_JAVA" -Djava.awt.headless=true -Xmx512M \
    -cp "$LIB_DIR/EccpressoJDK15ECC.jar;$LIB_DIR/EccpressoAll.jar;$LIB_DIR/TrustpointAll.jar;$LIB_DIR/TrustpointJDK15.jar;$LIB_DIR/TrustpointProviders.jar;$LIB_DIR/BarPackager.jar;$LIB_DIR/BarSigner.jar;$LIB_DIR/BarDeploy.jar;$LIB_DIR/BarAir.jar" \
    com.qnx.bbt.nativepackager.BarNativePackager \
    -devMode -package "$BAR_NAME" bar-descriptor.xml 2>&1 | tail -5

if [ -f "$BAR_NAME" ]; then
    ls -la "$BAR_NAME" libBiliClient.so.1.0.0

    echo "==> [GATE-DS/BAR] 包内图标与资产复检（防默认图标回归 / 防源包不一致）"
    # 【为什么源码门禁不够】编译期只能看到 descriptor 文本与源 PNG，看不到
    # BarNativePackager 最终写进 MANIFEST 的 Entry-Point-Icon。1.0.0.54 首版
    # 实测：源图标 md5 完全一致、descriptor 也写了 icon，但打包器悄悄把入口
    # 图标换成自带的 blackberry-tablet-default-icon.png，桌面显示默认图标。
    # 这类"打包后才存在的事实"必须在 BAR 上复检，且必须在拷贝交付之前拦住。
    if ! python - "$BAR_NAME" "$PROJ_DIR" <<'PYBAR'
# -*- coding: utf-8 -*-
import hashlib
import os
import re
import sys
import zipfile

bar, proj = sys.argv[1], sys.argv[2]
errs = []


def md5(b):
    return hashlib.md5(b).hexdigest()


def fopen(p):
    with open(p, "rb") as f:
        return f.read()


z = zipfile.ZipFile(bar)
names = z.namelist()

# ① 入口图标必须指向我们自己的 icon.png，且默认图标不许进包
manifest = z.read("META-INF/MANIFEST.MF").decode("utf-8", "replace")
entry = [l.split(":", 1)[1].strip() for l in manifest.splitlines()
         if l.startswith("Entry-Point-Icon:")]
if entry != ["native/icon.png"]:
    errs.append("Entry-Point-Icon=%s，应为 native/icon.png（打包器用了默认图标）" % entry)
defaults = [n for n in names if "blackberry-tablet-default-icon" in n]
if defaults:
    errs.append("包内混入打包器默认图标: %s" % defaults)

# ② 包内图标逐字节等于源图标（也等于 tokens.json brand.launcher_md5）
src_icon = fopen(os.path.join(proj, "icon.png"))
if "native/icon.png" not in names:
    errs.append("包内缺 native/icon.png")
elif md5(z.read("native/icon.png")) != md5(src_icon):
    errs.append("包内 native/icon.png 与源 icon.png md5 不一致")

# ③ 全部图标资产源包一致（生成器重跑后漏同步 / 旧包残留都在这里被拦下）
imgdir = os.path.join(proj, "assets", "images")
srcs = dict((n, md5(fopen(os.path.join(imgdir, n))))
            for n in os.listdir(imgdir) if n.endswith(".png"))
packed = dict((os.path.basename(n), md5(z.read(n))) for n in names
              if n.startswith("native/assets/images/") and n.endswith(".png"))
missing = sorted(set(srcs) - set(packed))
diff = sorted(n for n in set(srcs) & set(packed) if srcs[n] != packed[n])
if missing:
    errs.append("包内缺图标资产 %d 个: %s" % (len(missing), ", ".join(missing[:8])))
if diff:
    errs.append("包内图标资产与源不一致 %d 个: %s" % (len(diff), ", ".join(diff[:8])))
extra = sorted(set(packed) - set(srcs))
if extra:
    errs.append("包内多出源里没有的图标: %s" % ", ".join(extra[:8]))

# ④ 版本号与 descriptor 一致
desc = fopen(os.path.join(proj, "bar-descriptor.xml")).decode("utf-8", "replace")
bid = re.search(r"<buildId>(\d+)</buildId>", desc)
ver = re.search(r"<versionNumber>([^<]+)</versionNumber>", desc)
want = "%s.%s" % (ver.group(1), bid.group(1)) if bid and ver else "?"
got = re.search(r"^Package-Version:\s*(\S+)", manifest, flags=re.M)
if not got or got.group(1) != want:
    errs.append("Package-Version=%s，descriptor 期望 %s"
                % (got.group(1) if got else "缺失", want))

if errs:
    for e in errs:
        print("    [BAR] ERROR: " + e)
    sys.exit(1)
print("    [BAR] Entry-Point-Icon=native/icon.png 默认图标=0 启动图标md5=%s" % md5(src_icon))
print("    [BAR] 图标资产源包一致 %d/%d 个，Package-Version=%s" % (len(packed), len(srcs), want))
PYBAR
    then
        echo "!!! BAR 包内复检未通过，拒绝交付"
        exit 1
    fi

    echo "OK: $BUILD_DIR/$BAR_NAME"
    # 直接把交付包拷到桌面（在本会话内 BAR 可见，避免构建沙箱隔离 D: 写出的问题）
    cp -f "$BAR_NAME" "E:/PCdesk/桌面/bilibili/$BAR_NAME" && echo "delivered: E:/PCdesk/桌面/bilibili/$BAR_NAME"
else
    echo "!!! 打包失败"
    exit 1
fi
