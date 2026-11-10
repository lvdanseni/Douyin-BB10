#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""GATE10 双边验证：21 个故障注入必须全部拦截 + 2 个对照零误报。

做法与 gate9_inject_test 相同：把真实受检文件复制到临时树 → 对单个文件做
精确字符串替换（模拟"改坏了"）→ 用 argv 指向临时树跑抽出的 GATE10 段 →
断言 exit!=0 且输出包含该用例的预期锚点。
测试树还必须带 src/applicationui.{hpp,cpp}：G 段（1.0.0.33 测试命令通道）
从 api_hpp/api_cpp 的目录推导这两个文件的路径，缺文件门禁会 99 崩而不是干净报错。
"""
import io
import os
import re
import shutil
import sys
import tempfile

PROJ = os.path.dirname(os.path.abspath(__file__))

# 受检文件（前 12 个进门禁 argv，顺序不可变；其余供 A4/G 段按目录推导读取）
FILES = [
    "assets/main.qml",
    "assets/DouyinFeedPage.qml",
    "assets/DouyinCommentPage.qml",
    "assets/DouyinHistoryPage.qml",
    "assets/images/play.png",
    "assets/images/pause.png",
    "assets/images/refresh.png",
    "src/douyin_api.hpp",
    "src/douyin_api.cpp",
    "src/douyin_store.hpp",
    "src/douyin_store.cpp",
    "bar-descriptor.xml",
    "src/applicationui.hpp",
    "src/applicationui.cpp",
    "assets/SettingsPage.qml",
    "assets/DouyinPlayerPage.qml",
    "assets/NotePage.qml",
]


def extract_gate():
    src = open(os.path.join(PROJ, "build.sh"), "r", encoding="utf-8",
               errors="replace").read()
    m = re.search(r"<<'PYGATE10'\n(.*?)\nPYGATE10\n", src, re.S)
    if not m:
        print("FATAL: build.sh 里找不到 PYGATE10 段")
        sys.exit(2)
    return m.group(1)


GATE_CODE = extract_gate()


def run_gate(paths):
    buf = io.StringIO()
    old_out, old_argv = sys.stdout, sys.argv
    sys.stdout = buf
    sys.argv = ["gate10"] + list(paths)
    rc = 0
    try:
        exec(compile(GATE_CODE, "<PYGATE10>", "exec"), {"__name__": "__gate__"})
    except SystemExit as e:
        rc = e.code if isinstance(e.code, int) else 1
    except Exception as e:  # noqa: BLE001
        rc = 99
        buf.write("GATE10 internal error: %r\n" % (e,))
    finally:
        sys.stdout, sys.argv = old_out, old_argv
    return rc, buf.getvalue()


def make_tree(base):
    for rel in FILES:
        dst = os.path.join(base, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(os.path.join(PROJ, rel), dst)
    return base


def paths_of(base):
    # 门禁 argv 仍只要原 12 个（applicationui 由 G 段按 src/ 目录推导）。
    return [os.path.join(base, rel) for rel in FILES[:12]]


def sub_file(path, old, new, must=True):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        src = f.read()
    if old not in src:
        if must:
            raise RuntimeError("注入失败：锚点不在 %s\n%r" % (path, old[:80]))
        return False
    with open(path, "w", encoding="utf-8") as f:
        f.write(src.replace(old, new, 1))
    return True


# ---------------------------------------------------------------------------
# 注入用例：name → (mutate_fn, 预期输出锚点)
# ---------------------------------------------------------------------------
def inj_openlogin_no_try(t):
    """main.qml openLogin 拆掉 try/catch（退回 1.0.0.28 零反馈形态）。"""
    p = os.path.join(t, "assets/main.qml")
    sub_file(p,
             """        if (!settingsDef) {
            return "def-missing";
        }
        try {
            var page = settingsDef.createObject();
            if (!page) {
                return "settings-null";
            }
            page.nav = dyNav;
            dyNav.push(page);
            return "pushed";
        } catch (e) {
            return "EX:" + e;
        }""",
             """        if (!settingsDef) {
            return "def-missing";
        }
        var page = settingsDef.createObject();
        if (!page) {
            return "settings-null";
        }
        page.nav = dyNav;
        dyNav.push(page);
        return "pushed";""")


def inj_openlogin_no_guard(t):
    """main.qml openLogin 删 def 守卫（settingsDef null → TypeError 吞埋点）。"""
    p = os.path.join(t, "assets/main.qml")
    sub_file(p,
             """        if (!settingsDef) {
            return "def-missing";
        }
        try {""",
             """        try {""")


def inj_feed_callsite_plain(t):
    """DouyinFeedPage 顶条调用点（doTopbarLogin）退回无兜底一行式。"""
    p = os.path.join(t, "assets/DouyinFeedPage.qml")
    sub_file(p,
             """        var r0 = "no-host";
        try {
            if (feedPage.host && feedPage.host.openLogin) {
                r0 = feedPage.host.openLogin();
            }
        } catch (e) {
            r0 = "EX:" + e;
        }""",
             """        var r0 = (feedPage.host && feedPage.host.openLogin) ? feedPage.host.openLogin() : "no-host";""")


def inj_settings_nav_prop_removed(t):
    """SettingsPage 删 property variant nav（1.0.0.33 真机 bug 复现）。"""
    p = os.path.join(t, "assets/SettingsPage.qml")
    sub_file(p, "    property variant nav: null\n", "")


def inj_icon_changed_log_removed(t):
    """播放/暂停按钮删 onImageSourceChanged → ICON 埋点（图标切换无证据）。"""
    p = os.path.join(t, "assets/DouyinFeedPage.qml")
    sub_file(p, '                        onImageSourceChanged: feedPage.logSwipe("ICON " + (videoPlayer && videoPlayer.playing ? "pause" : "play"))\n',
             "")


def inj_toggleplay_dispatch_removed(t):
    """测试命令分发删 togglePlay 分支（图标态无法程序化驱动）。"""
    p = os.path.join(t, "assets/DouyinFeedPage.qml")
    sub_file(p, '            } else if (cmd === "togglePlay") {',
             '            } else if (cmd === "togglePlayX") {')


def inj_play_png_missing(t):
    """play.png 缺失。"""
    os.remove(os.path.join(t, "assets/images/play.png"))


def inj_play_button_glyph(t):
    """播放按钮退回文字字形（\u23F8/\u25B6）。"""
    p = os.path.join(t, "assets/DouyinFeedPage.qml")
    sub_file(p,
             """                        text: ""
                        imageSource: (videoPlayer && videoPlayer.playing)
                                    ? "asset:///images/pause.png"
                                    : "asset:///images/play.png\"""",
             """                        text: (videoPlayer && videoPlayer.playing) ? "\\u23F8" : "\\u25B6\"""")


def inj_refresh_png_missing(t):
    """refresh.png 缺失。"""
    os.remove(os.path.join(t, "assets/images/refresh.png"))


def inj_refresh_func_removed(t):
    """refreshFeed 函数整块删除。"""
    p = os.path.join(t, "assets/DouyinFeedPage.qml")
    sub_file(p,
             """    function refreshFeed() {
        feedPage.logSwipe("TOPBAR refresh kind=" + feedPage.feedKind);""",
             """    function refreshFeedOld() {
        feedPage.logSwipe("MENU switch kind=old");""")


def inj_publish_impl_removed(t):
    """duyin_store.cpp publish 实现删掉（hpp 仍声明）。"""
    p = os.path.join(t, "src/douyin_store.cpp")
    sub_file(p,
             "void DouyinCommentModel::publish(const QString& text)",
             "void DouyinCommentModel::publishRemoved(const QString& text)")


def inj_publish_url_changed(t):
    """douyin_api.cpp 端点 URL 改错（publish 失效）。"""
    p = os.path.join(t, "src/douyin_api.cpp")
    sub_file(p,
             'QString("https://www.iesdouyin.com/aweme/v1/comment/publish/")',
             'QString("https://www.iesdouyin.com/aweme/v1/comment/publish_old/")')


def inj_cmt_input_removed(t):
    """DouyinCommentPage 输入框 objectName 改名（等价于删输入条）。"""
    p = os.path.join(t, "assets/DouyinCommentPage.qml")
    sub_file(p, 'objectName: "cmtInput"', 'objectName: "cmtInputX"')


def inj_buildid_regress(t):
    """buildId 回退到 32（<33，测试命令通道这一版未升版）。"""
    p = os.path.join(t, "bar-descriptor.xml")
    sub_file(p, "<buildId>36</buildId>", "<buildId>35</buildId>")


def inj_dynetlog_removed(t):
    """dyNetLog() 改名（网络层失去写 boot log 的通道）。"""
    p = os.path.join(t, "src/douyin_api.cpp")
    sub_file(p, "void dyNetLog(const QString& s)",
             "void dyNetLogRemoved(const QString& s)")


def inj_net_reply_log_removed(t):
    """onCurlFinished 的 'net reply tag=' 埋点删掉。"""
    p = os.path.join(t, "src/douyin_api.cpp")
    sub_file(p, 'dyNetLog(QString("net reply tag=%1 code=%2 http=%3 bytes=%4")',
             '// removed')


def inj_diag_verdict_removed(t):
    """onDiagFinished 的 'net diag verdict' 埋点删掉。"""
    p = os.path.join(t, "src/douyin_api.cpp")
    sub_file(p, 'dyNetLog(QString("net diag verdict %1 code=%2 http=%3 bytes=%4")',
             '// removed')


def inj_response_code_partial(t):
    """只在一个 worker 里取 CURLINFO_RESPONSE_CODE（下载层失去 http 判据）。"""
    p = os.path.join(t, "src/douyin_api.cpp")
    sub_file(p, """        m_code = curl_easy_perform(c);
        // 【1.0.0.30】HTTP 码单独取（Range 206 才算真命中 CDN）。
        {
            long rc = 0;
            if (curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &rc) == CURLE_OK) {
                m_httpCode = (int)rc;
            }
        }
        curl_easy_cleanup(c);""",
             """        m_code = curl_easy_perform(c);
        curl_easy_cleanup(c);""")


def inj_perform_exit_log_removed(t):
    """perform-exit 埋点删掉（拿不到 errbuf 原因）。"""
    p = os.path.join(t, "src/douyin_api.cpp")
    sub_file(p, 'dyNetLog(QString("net worker perform-exit tag=%1 code=%2 err=%3")',
             '// removed')


# ---- G. 测试命令通道（1.0.0.33）----
def inj_testcmd_prop_renamed(t):
    """applicationui.hpp testCmd Q_PROPERTY 改名（C++→QML 通道断）。"""
    p = os.path.join(t, "src/applicationui.hpp")
    sub_file(p, "Q_PROPERTY(QString testCmd READ testCmd NOTIFY testCmdChanged)",
             "Q_PROPERTY(QString testCmdX READ testCmd NOTIFY testCmdChanged)")


def inj_runtestcmd_removed(t):
    """DouyinFeedPage runTestCmd 分发函数删除（appTestCmd 绑定掉進坑）。"""
    p = os.path.join(t, "assets/DouyinFeedPage.qml")
    sub_file(p, "    function runTestCmd(raw) {",
             "    function runTestCmdOld(raw) {")


def inj_qml_dispatch_removed(t):
    """main.qml onAppTestCmdChanged 分发删掉（命令到了 QML 却不执行）。"""
    p = os.path.join(t, "assets/main.qml")
    sub_file(p, "    onAppTestCmdChanged: feedHome.runTestCmd(dyNav.appTestCmd)",
             "    // removed")


def inj_testcmd_recv_log_removed(t):
    """applicationui.cpp TESTCMD recv 埋点删掉（命令到达无可核对痕迹）。

    注意文件里有两处 TESTCMD recv（正常行 + open-fail 行），必须都改掉，
    否则锚点仍在、门禁正确地放过——那是注入用例不合格，不是门禁漏拦。
    """
    p = os.path.join(t, "src/applicationui.cpp")
    sub_file(p, 'bootLog("TESTCMD recv " + cmd);', 'bootLog("TESTCMD got " + cmd);')
    sub_file(p, 'bootLog("TESTCMD recv open-fail path=" + path);',
             'bootLog("TESTCMD open-fail path=" + path);')
    sub_file(p, 'bootLog("TESTCMD recv empty-file path=" + path);',
             'bootLog("TESTCMD empty-file path=" + path);')


def inj_menu_settings_reverted(t):
    """抽屉「设置」按钮退回内联旧代码（refactor 被改回，两条路径漂移）。"""
    p = os.path.join(t, "assets/DouyinFeedPage.qml")
    sub_file(p, "                onClicked: feedPage.doMenuSettings()",
             "                onClicked: { feedPage.menuOpen = false; }")


def ctrl_clean(t):
    """对照：干净源码必须 PASS。"""
    pass


def ctrl_benign_edit(t):
    """对照：良性修改（改文案/注释）必须 PASS —— 只动门禁不检的串。"""
    sub_file(os.path.join(t, "assets/DouyinCommentPage.qml"),
             'hintText: "说点什么…"', 'hintText: "友善评论，温暖交流…"')
    sub_file(os.path.join(t, "assets/DouyinFeedPage.qml"),
             'feedPage.logSwipe("CTRL togglePlay");',
             'feedPage.logSwipe("CTRL togglePlay v2");')


CASES = [
    # (name, mutate, 预期错误锚点, 期望 exit!=0)
    ("INJ-A1 openLogin 无 try/catch", inj_openlogin_no_try,
     "openLogin() 函数体缺 'try {'", True),
    ("INJ-A2 openLogin 无 def 守卫", inj_openlogin_no_guard,
     "openLogin() 缺 if (!settingsDef) 守卫", True),
    ("INJ-A3 顶条调用点无兜底", inj_feed_callsite_plain,
     "缺 try/catch 兜底锚点", True),
    ("INJ-A4 SettingsPage 删 nav 属性", inj_settings_nav_prop_removed,
     "缺 property variant nav", True),
    ("INJ-B3 图标切换埋点删除", inj_icon_changed_log_removed,
     "缺 onImageSourceChanged", True),
    ("INJ-B1 play.png 缺失", inj_play_png_missing,
     "图标 play.png missing", True),
    ("INJ-B2 播放按钮回退文字字形", inj_play_button_glyph,
     "仍含文字字形", True),
    ("INJ-C1 refresh.png 缺失", inj_refresh_png_missing,
     "图标 refresh.png missing", True),
    ("INJ-C2 refreshFeed 删除", inj_refresh_func_removed,
     "缺 refreshFeed() 函数", True),
    ("INJ-D1 publish 实现删除", inj_publish_impl_removed,
     "缺 DouyinCommentModel::publish 实现", True),
    ("INJ-D2 发表 URL 改错", inj_publish_url_changed,
     "return 语句端点不对", True),
    ("INJ-D3 评论输入条删除", inj_cmt_input_removed,
     "缺发表器件 objectName: \"cmtInput\"", True),
    ("INJ-E buildId 回退", inj_buildid_regress,
     "buildId=35 < 36", True),
    ("INJ-F1 dyNetLog 改名", inj_dynetlog_removed,
     "缺 dyNetLog()", True),
    ("INJ-F2 onCurlFinished 埋点删除", inj_net_reply_log_removed,
     "缺 'net reply tag=' 埋点", True),
    ("INJ-F3 diag verdict 埋点删除", inj_diag_verdict_removed,
     "缺 'net diag verdict' 埋点", True),
    ("INJ-F4 只一处 CURLINFO_RESPONSE_CODE", inj_response_code_partial,
     "两个 worker 都要取 CURLINFO_RESPONSE_CODE", True),
    ("INJ-F5 perform-exit 埋点删除", inj_perform_exit_log_removed,
     "缺一级诊断锚点 net worker perform-exit tag=", True),
    ("INJ-G1 testCmd Q_PROPERTY 改名", inj_testcmd_prop_renamed,
     "缺 testCmd Q_PROPERTY", True),
    ("INJ-G2 runTestCmd 分发删除", inj_runtestcmd_removed,
     "缺 runTestCmd() 分发函数", True),
    ("INJ-G3 QML 分发 onAppTestCmdChanged 删除", inj_qml_dispatch_removed,
     "缺 onAppTestCmdChanged", True),
    ("INJ-G4 TESTCMD recv 埋点删除", inj_testcmd_recv_log_removed,
     "缺测试命令通道锚点", True),
    ("INJ-G5 抽屉设置按钮 refactor 被改回", inj_menu_settings_reverted,
     "未走 doMenuSettings()", True),
    ("INJ-G6 togglePlay 分发删除", inj_toggleplay_dispatch_removed,
     "缺 togglePlay 分支", True),
    ("CTRL-1 干净源码", ctrl_clean, None, False),
    ("CTRL-2 良性修改", ctrl_benign_edit, None, False),
]


def main():
    base = tempfile.mkdtemp(prefix="gate10inj_")
    fails = []
    try:
        for name, mutate, expect_err, expect_fail in CASES:
            tree = os.path.join(base, re.sub(r"[^0-9A-Za-z]+", "_", name))
            os.makedirs(tree, exist_ok=True)
            make_tree(tree)
            mutate(tree)
            rc, out = run_gate(paths_of(tree))
            ok = True
            if expect_fail:
                if rc == 0 or expect_err not in out:
                    ok = False
            else:
                if rc != 0:
                    ok = False
            status = "PASS" if ok else "FAIL"
            print("[%s] %s rc=%d" % (status, name, rc))
            if not ok:
                fails.append(name)
                print("    ---- gate output ----")
                for ln in out.splitlines():
                    print("    " + ln)
                print("    ---------------------")
    finally:
        shutil.rmtree(base, ignore_errors=True)

    print()
    print("GATE10 双边结果：%d 用例，失败 %d" % (len(CASES), len(fails)))
    if fails:
        for f in fails:
            print("  - " + f)
        sys.exit(1)
    print("全部通过：24 个注入全拦 + 2 个对照零误报；干净源码 PASS")


if __name__ == "__main__":
    main()
