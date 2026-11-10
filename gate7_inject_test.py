# GATE7 针对性故障注入测试（1.0.0.24 二修收紧后双边验证）
# 重点：收紧前漏检的 INJ-C（Slider onTouch 被根容器顶位）与 INJ-D（快照声明
# 被 .arg() 引用顶位）；另含防误报对照（改别处 onTouch 不得误报）。
import os, shutil, subprocess, sys

PROJ = os.path.dirname(os.path.abspath(__file__))
FILES = ['src/video_store.cpp', 'src/video_store.hpp', 'src/douyin_store.cpp',
         'src/douyin_store.hpp', 'src/douyin_api.cpp', 'src/douyin_api.hpp',
         'src/applicationui.cpp', 'assets/DouyinFeedPage.qml', 'assets/main.qml',
         'assets/DouyinHistoryPage.qml']
WORK = os.path.join(PROJ, 'analysis', 'p24', 'gate7_inj')

def fresh():
    if os.path.isdir(WORK):
        shutil.rmtree(WORK)
    os.makedirs(os.path.dirname(WORK), exist_ok=True)
    os.makedirs(WORK)
    for f in FILES:
        rel = os.path.relpath(f, '.')
        dst = os.path.join(WORK, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(os.path.join(PROJ, f), dst)
    return [os.path.join(WORK, f) for f in FILES]

def run_gate(paths):
    p = subprocess.run([sys.executable, os.path.join(PROJ, 'gate7_test.py')] + paths,
                       capture_output=True, text=True, encoding='utf-8', errors='replace')
    return p.returncode, (p.stdout + p.stderr)

def mutate(path, old, new, count=1):
    s = open(path, 'r', encoding='utf-8', errors='replace').read()
    if old not in s:
        raise SystemExit('ANCHOR NOT FOUND in %s: %r' % (path, old[:60]))
    open(path, 'w', encoding='utf-8').write(s.replace(old, new, count))

CASES = []
def case(name, expect, fn):
    CASES.append((name, expect, fn))

# ---- 收紧后必须被抓的故障 ----
def inj_slider_ontouch(paths):
    # 只改 Slider 自己的 onTouch（20 空格缩进 + Down 判据），不动根容器/视频层
    feed = [p for p in paths if p.endswith('DouyinFeedPage.qml')][0]
    mutate(feed, '                    onTouch: {\n', '                    onNothing: {\n')
case('INJ-C 删 Slider 的 onTouch（收紧前被根容器顶位漏检）', 1, inj_slider_ontouch)

def inj_slider_touchtype(paths):
    # 精确打 Slider 块内的 Down 判据（锚点带「起拖基准」注释，视频层/根层不会误中）
    feed = [p for p in paths if p.endswith('DouyinFeedPage.qml')][0]
    mutate(feed,
           'if (event.touchType === TouchType.Down) {\n                            // 起拖基准 = 当前位置，别让条子先跳 0。',
           'if (false) {\n                            // 起拖基准 = 当前位置，别让条子先跳 0。')
case('INJ-C2 Slider onTouch 只处理 Up 不处理 Down', 1, inj_slider_touchtype)

def inj_seek_guard(paths):
    feed = [p for p in paths if p.endswith('DouyinFeedPage.qml')][0]
    mutate(feed, 'if (!feedPage.seeking) return;', 'if (false) return;')
case('INJ-B 删 seeking 守卫（程序化回写风暴回归）', 1, inj_seek_guard)

def inj_valuechanged(paths):
    feed = [p for p in paths if p.endswith('DouyinFeedPage.qml')][0]
    mutate(feed, 'onImmediateValueChanged: {', 'onValueChanged: {')
case('INJ-A 进度条退回 onValueChanged（1.0.0.24 首版风暴根因）', 1, inj_valuechanged)

def inj_snap_decl(paths):
    vc = [p for p in paths if p.endswith('video_store.cpp')][0]
    mutate(vc, '            const int snapEof = m_eofLikely ? 1 : 0;\n', '')
case('INJ-D 删 snapEof 声明行（收紧前被 .arg(snapEof) 顶位漏检）', 1, inj_snap_decl)

def inj_snap_decl2(paths):
    vc = [p for p in paths if p.endswith('video_store.cpp')][0]
    mutate(vc, '            const int snapTrust = trustedDur ? 1 : 0;\n', '')
case('INJ-D2 删 snapTrust 声明行', 1, inj_snap_decl2)

def inj_timeago(paths):
    ac = [p for p in paths if p.endswith('douyin_api.cpp')][0]
    s = open(ac, encoding='utf-8').read()
    i = s.find('QString DouyinApi::timeAgo')
    j = s.find('\n}\n', i)
    body = s[i:j]
    stub = body.split('{', 1)[0] + '{\n    Q_UNUSED(unix);\n    return QString("");\n'
    open(ac, 'w', encoding='utf-8').write(s[:i] + stub + s[j:])
case('INJ-E timeAgo 退回空桩（历史/评论时间列全空）', 1, inj_timeago)

def inj_slider_gone(paths):
    feed = [p for p in paths if p.endswith('DouyinFeedPage.qml')][0]
    s = open(feed, encoding='utf-8').read()
    i = s.find('                Slider {')
    j = s.find('\n                }\n', i)
    open(feed, 'w', encoding='utf-8').write(s[:i] + s[j + len('\n                }\n'):])
case('INJ-F 整个 Slider 块被删', 1, inj_slider_gone)

def inj_replay(paths):
    feed = [p for p in paths if p.endswith('DouyinFeedPage.qml')][0]
    mutate(feed, 'videoPlayer.replay()', 'videoPlayer.nothing()')
case('INJ-G 循环链路 replay() 断', 1, inj_replay)

def inj_drawer_guard(paths):
    # 删掉抽屉避让：手势层又会在抽屉区域内起手 → Cancel/Up 藏抽屉 → 按钮点击死
    feed = [p for p in paths if p.endswith('DouyinFeedPage.qml')][0]
    mutate(feed, 'if (feedPage.menuOpen && event.windowX < 304 && event.windowY >= 74) return;',
          'if (false) return;')
case('INJ-I 删抽屉区域手势避让（1.0.0.24/25 菜单项点击死回归）', 1, inj_drawer_guard)

def inj_rename_cover(paths):
    sc = [p for p in paths if p.endswith('douyin_store.cpp')][0]
    mutate(sc, '    QFile::remove(m_path);\n    QFile::rename(part, m_path);',
          '    QFile::rename(part, m_path);')
case('INJ-J 历史落盘 rename 前不删旧文件（QFile::rename 不覆盖，历史停第一行）', 1, inj_rename_cover)

def inj_hist_api(paths):
    sc = [p for p in paths if p.endswith('douyin_store.cpp')][0]
    mutate(sc, 'm.remove("api");', 'm.remove("nothing");')
case('INJ-H 历史落盘不剥 api 指针（QObject* 进 JSON）', 1, inj_hist_api)

# ---- 防误报对照：改别处的 onTouch / 别处代码，门禁必须仍然通过 ----
def ctrl_videolayer(paths):
    feed = [p for p in paths if p.endswith('DouyinFeedPage.qml')][0]
    mutate(feed, '            onTouch: {\n                if (event.touchType === TouchType.Down',
          '            onOther: {\n                if (event.touchType === TouchType.Down')
case('CTRL-1 视频层 onTouch 改名（非 Slider，不得误报）', 0, ctrl_videolayer)

def ctrl_root(paths):
    feed = [p for p in paths if p.endswith('DouyinFeedPage.qml')][0]
    mutate(feed, '        onTouch: feedPage.gestureTouch(event)\n', '        onTouch: feedPage.gestureTouch2(event)\n', 1)
case('CTRL-2 根容器 onTouch 目标改名（不得误报）', 0, ctrl_root)

def ctrl_comment(paths):
    vc = [p for p in paths if p.endswith('video_store.cpp')][0]
    mutate(vc, '            const int snapEof = m_eofLikely ? 1 : 0;',
          '            // 快照：snapEof 注释里也提到 const int snapEof = m_eofLikely 字样\n            const int snapEof = m_eofLikely ? 1 : 0;')
case('CTRL-3 快照声明前加同名注释（strip 后必须消歧，不得误报/漏报变化）', 0, ctrl_comment)

fails = 0
for name, expect, fn in CASES:
    paths = fresh()
    try:
        fn(paths)
    except SystemExit as e:
        print('  [SETUP-FAIL] %s -> %s' % (name, e))
        fails += 1
        continue
    rc, out = run_gate(paths)
    ok = (rc == expect) if expect == 1 else (rc == 0)
    # expect==1 时还要求确实报了 V124 行（而非语法错误等);
    if expect == 1:
        ok = ok and ('[V124]' in out)
    print('  %s %s (exit=%d, expect=%d)' % ('PASS' if ok else 'FAIL', name, rc, expect))
    if expect == 1:
        for line in out.splitlines():
            if '[V124]' in line:
                print('       ' + line.strip())
    if not ok:
        print(out)
        fails += 1

print('----')
print('INJECT_TEST: %d cases, %d failures' % (len(CASES), fails))
sys.exit(1 if fails else 0)
