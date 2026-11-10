# GATE8 故障注入双边验证（1.0.0.27 两件套）
# 干净源码必须 0 错；每个注入必须 exit=1 且报对应规则；对照组（改无关处）
# 不得误报。锚点教训：同项分支必须按配对块查（全局查 keep/playInline(0)
# 会被别处顶位），main.qml 窗口按函数起点截取。
import os, shutil, subprocess, sys

PROJ = os.path.dirname(os.path.abspath(__file__))
FILES = ['assets/DouyinFeedPage.qml', 'assets/main.qml']
WORK = os.path.join(PROJ, 'analysis', 'p27', 'gate8_inj')

def fresh():
    if os.path.isdir(WORK):
        shutil.rmtree(WORK)
    os.makedirs(WORK)
    for f in FILES:
        dst = os.path.join(WORK, f)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(os.path.join(PROJ, f), dst)
    return [os.path.join(WORK, f) for f in FILES]

def run_gate(paths):
    p = subprocess.run([sys.executable, os.path.join(PROJ, 'gate8_test.py')] + paths,
                       capture_output=True, text=True, encoding='utf-8', errors='replace')
    return p.returncode, (p.stdout + p.stderr)

def mutate(path, old, new, count=1):
    with open(path, 'r', encoding='utf-8') as f:
        s = f.read()
    n = s.count(old)
    if n < count:
        raise SystemExit('注入点不存在（%d<%d）：%s' % (n, count, old[:60]))
    s = s.replace(old, new, count)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(s)

feed, main = None, None
fails = []
def check(name, expect_rule):
    rc, out = run_gate([feed, main])
    ok = (rc == 1) and (expect_rule in out)
    print('%-28s exit=%d  %s' % (name, rc, 'PASS' if ok else 'FAIL'))
    if not ok:
        fails.append(name)
        print('    ' + out.replace('\n', '\n    ')[:600])

def reset():
    global feed, main
    feed, main = fresh()

# ---- 基线：干净源码必须通过 ----
reset()
rc, out = run_gate([feed, main])
print('%-28s exit=%d  %s' % ('baseline-clean', rc, 'PASS' if rc == 0 else 'FAIL'))
if rc != 0:
    fails.append('baseline')
    print('    ' + out)

# ---- INJ-A：同项分支退回静默早退（MENU same kind keep 零反馈回归） ----
reset()
mutate(feed, 'MENU same kind=" + kind + " refresh"', 'MENU same kind=" + kind + " keep"')
check('INJ-A same-kind-silent', 'keep')

# ---- INJ-B：同项分支没有可见动作（删 playInline(0) 重播第一部） ----
reset()
mutate(feed, 'if (app && app.scrollFeedTo) app.scrollFeedTo(0);\n            feedPage.playInline(0);', 'if (app && app.scrollFeedTo) app.scrollFeedTo(0);')
check('INJ-B same-kind-no-action', 'playInline(0)')

# ---- INJ-C：设置按钮结果埋点被删（push 失败再次静默） ----
reset()
mutate(feed, 'MENU settings ret=', 'MENU settings done')
check('INJ-C settings-no-ret', 'MENU settings ret=')

# ---- INJ-D：videoUiLayer 不贴视频矩形（宽高写死 720） ----
# 注意：FWC 里也有 preferredWidth: feedPage.videoW（声明在 videoUiLayer 之前），
# 必须按 videoUiLayer 的两行连写注入，否则改到的是 FWC（门禁窗口截正确实盯层）。
reset()
mutate(feed, 'preferredWidth: feedPage.videoW\n            preferredHeight: feedPage.videoH',
       'preferredWidth: 720\n            preferredHeight: 720')
check('INJ-D uilayer-not-adaptive', 'videoW')

# ---- INJ-E：rectX 几何声明被删（控件层/守卫全都没有基准） ----
reset()
mutate(feed, 'property double rectX: (720 - feedPage.videoW) / 2\n', '')
check('INJ-E no-rectX', 'rectX')

# ---- INJ-F：tap 区顶栏排除被删（点顶栏误暂停回归） ----
reset()
mutate(feed, 'y > Math.max(76, feedPage.rectY + 2)', 'y > feedPage.rectY + 2')
check('INJ-F no-topbar-guard', 'Math.max(76,')

# ---- 对照组 1：改别处 onTouch（根容器 VIDEOLAYER 日志）不得误报 ----
reset()
mutate(feed, 'feedPage.logSwipe("VIDEOLAYER touch=" + event.touchType + " y=" + Math.round(event.windowY));',
       'feedPage.logSwipe("VIDEOLAYER t=" + event.touchType + " y=" + Math.round(event.windowY));')
rc, out = run_gate([feed, main])
print('%-28s exit=%d  %s' % ('CTRL-1 other-ontouchedit', rc, 'PASS' if rc == 0 else 'FAIL'))
if rc != 0:
    fails.append('CTRL-1')

# ---- 对照组 2：main.qml 改 openSettings 注释不得误报 ----
reset()
mutate(main, '/** 旧组件遗留调用点兜底：行为同 openLogin。 */', '/** legacy alias: openLogin */')
rc, out = run_gate([feed, main])
print('%-28s exit=%d  %s' % ('CTRL-2 mainq-comment', rc, 'PASS' if rc == 0 else 'FAIL'))
if rc != 0:
    fails.append('CTRL-2')

print()
if fails:
    print('!!! GATE8 双边验证失败：' + ', '.join(fails))
    sys.exit(1)
print('GATE8 双边验证全部通过（6 注入全拦 + 2 对照零误报）')
