# GATE8 独立版：1.0.0.27 两件套（菜单点击必反馈 / 控件入画跟随视频矩形）
# 规则与 build.sh [GATE8] 完全一致；供 gate8_inject_test.py 故障注入使用。
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
    # 'if \\(kind === feedPage.feedKind'），真正起始花括号从起点向后找
    # （if 条件里可能带 && / 三元），再按花括号深度配对（跳过字符串）。
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
    if 'keep' in same_kind:
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
if 'feedPage.loopOn ? "循环" : "不循环"' not in fq:
    errs.append('循环开关文案缺失')
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
