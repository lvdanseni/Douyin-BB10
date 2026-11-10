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
# ENDED 块：setState/emit 会同步触发 QML replay() -> play() 重入改掉成员，
# 日志值必须先快照，否则打出 eof=0 trustedDur=0 的失真行。
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
if 'pstate' not in fq or 'videoPlayer.replay()' not in fq:
    errs.append('循环播放缺链路：pstate 影子属性 -> ended -> videoPlayer.replay()')
if 'loopOn' not in fq:
    errs.append('缺少 loopOn 循环开关属性')
if 'dyHistoryModel.add' not in fq:
    errs.append('playInline 换片未记观看历史（dyHistoryModel.add）')
if '\\u23F8' not in fq or '\\u25B6' not in fq:
    errs.append('控制条缺少播放/暂停按钮（\\u23F8 / \\u25B6）')
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