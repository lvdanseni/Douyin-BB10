#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""GATE9 双边验证：注入必挂（exit=1 + 精确报错串）+ 对照零误报（exit=0）。

注入样本全部来自本项目真实踩过的坑（注释里注明对应版本），
确保门禁防的是真回归而不是想象出来的退化。
"""
import io
import os
import re
import shutil
import sys
import tempfile

PROJ = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(PROJ, "build.sh")

FILES = [
    "src/video_prefetch.cpp",
    "src/video_prefetch.hpp",
    "src/douyin_store.cpp",
    "src/video_store.cpp",
    "Makefile",
    "assets/DouyinFeedPage.qml",
    "bar-descriptor.xml",
]


def extract():
    src = open(BUILD, "r", encoding="utf-8", errors="replace").read()
    m = re.search(r"<<'PYGATE9'\n(.*?)\nPYGATE9\n", src, re.S)
    if not m:
        print("FATAL: build.sh 里找不到 PYGATE9 段")
        sys.exit(2)
    return m.group(1)


def run_gate(paths):
    code = extract()
    buf = io.StringIO()
    old_out, old_argv = sys.stdout, sys.argv
    sys.stdout = buf
    sys.argv = ["gate9"] + list(paths)
    rc = 0
    ns = {"__name__": "__gate__"}
    try:
        exec(compile(code, "<PYGATE9>", "exec"), ns)
    except SystemExit as e:
        rc = e.code if isinstance(e.code, int) else 1
    except Exception as e:  # noqa: BLE001
        rc = 99
        buf.write("internal error: %r\n" % (e,))
    finally:
        sys.stdout, sys.argv = old_out, old_argv
    return rc, buf.getvalue()


def stage(tmp):
    """复制真实源码到 tmp，返回路径列表。"""
    paths = []
    for rel in FILES:
        dst = os.path.join(tmp, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy(os.path.join(PROJ, rel), dst)
        paths.append(dst)
    return paths


def sub(text, old, new, count=1):
    if old not in text:
        raise AssertionError("inject target missing: %r" % old[:60])
    return text.replace(old, new, count)


# ---------------------------------------------------------------- 注入样本
# (名称, 相对文件, 旧串, 新串, 期望报错子串)
INJECTIONS = [
    ("A-去掉在飞上限",
     "src/video_prefetch.cpp",
     "if (m_inflight.size() >= 2) return;     // 最多同时预取 2 个，别和播放抢",
     "// 删掉了：不设上限",
     "单飞+在飞上限"),
    ("B-rename前不remove",
     "src/video_prefetch.cpp",
     "QFile::remove(m_finalPath)",
     "// 删掉了：直接 rename",
     "rename 前没有 remove"),
    ("C-worker不收尸",
     "src/video_prefetch.cpp",
     "deleteLater()",
     "// 删掉了：让 worker 自生自灭",
     "没有 deleteLater 收尸"),
    ("D-proxy无file分支",
     "src/video_store.cpp",
     "    if (m_url.startsWith(\"file://\")) {\n        runLocal();\n        return;\n    }",
     "    // INJ-D: file:// 分支被删",
     "没有 file:// 分支"),
    ("E-QML不触发预取",
     "assets/DouyinFeedPage.qml",
     "        if (dyPlayerStore && dyPlayerStore.prefetchNext && feedPage.model && feedPage.model.rowAt) {\n"
     "            var nxtRow = feedPage.model.rowAt(idx + 1);\n"
     "            if (nxtRow && nxtRow.playUrl && nxtRow.playUrl.length > 0) dyPlayerStore.prefetchNext(nxtRow);\n"
     "        }",
     "        // INJ-E: 预取触发被整块删除",
     "没有触发下一片预取"),
    ("F-Makefile缺moc规则",
     "Makefile",
     "../../.moc/moc_video_prefetch.cpp: src/video_prefetch.hpp\n",
     "",
     "缺少 video_prefetch 的 moc 规则"),
    ("G-buildId忘了升",
     "bar-descriptor.xml",
     "<buildId>28</buildId>",
     "<buildId>27</buildId>",
     "buildId=27 < 28"),
    ("H-命中不换源",
     "src/douyin_store.cpp",
     "        src = localFile;",
     "        // 删掉了：源还用 CDN",
     "命中后没有把代理源换成 file://"),
]

# ---------------------------------------------------------------- 对照样本
CONTROLS = [
    ("Ctrl1-加无害声明",
     "src/video_prefetch.cpp",
     "void VideoPrefetcher::clearAll()",
     "static const int kG9Noop = 0; (void)kG9Noop;\nvoid VideoPrefetcher::clearAll()",
     "无害声明不得触发"),
    ("Ctrl2-QML注释形态改动",
     "assets/DouyinFeedPage.qml",
     "function swipe(dir) {",
     "// [g9-ctrl] 无关注释\n    function swipe(dir) {",
     "注释不得触发（strip 会剥掉）"),
]


def main():
    rc, out = run_gate([os.path.join(PROJ, r) for r in FILES])
    if rc != 0:
        print("baseline FAIL rc=%d:\n%s" % (rc, out))
        sys.exit(1)
    print("[ok] baseline 0 错 0 警告")

    tmp = tempfile.mkdtemp(prefix="gate9inj_")
    failures = 0
    try:
        paths = stage(tmp)
        by_rel = {os.path.relpath(p, tmp).replace(os.sep, "/"): p for p in paths}

        for name, rel, old, new, expect in INJECTIONS:
            tdir = tempfile.mkdtemp(prefix="g9i_", dir=tmp)
            tpaths = []
            for r in FILES:
                dst = os.path.join(tdir, r)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy(os.path.join(PROJ, r), dst)
                tpaths.append(dst)
            text = open(tpaths[FILES.index(rel)].read() if False else
                        os.path.join(tdir, rel), "r",
                        encoding="utf-8", errors="replace").read()
            text = sub(text, old, new)
            open(os.path.join(tdir, rel), "w", encoding="utf-8").write(text)
            rc, out = run_gate(tpaths)
            ok = rc == 1 and expect in out
            line = next((l for l in out.splitlines() if expect in l), "")
            print("%s %s%s" % ("[ok]" if ok else "[FAIL]", name,
                               ("  -> " + line.strip()) if line else ""))
            if not ok:
                failures += 1
                print("     rc=%d out=%s" % (rc, out.strip()[:300]))

        for name, rel, old, new, expect in CONTROLS:
            tdir = tempfile.mkdtemp(prefix="g9c_", dir=tmp)
            tpaths = []
            for r in FILES:
                dst = os.path.join(tdir, r)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy(os.path.join(PROJ, r), dst)
                tpaths.append(dst)
            text = open(os.path.join(tdir, rel), "r",
                        encoding="utf-8", errors="replace").read()
            text = sub(text, old, new)
            open(os.path.join(tdir, rel), "w", encoding="utf-8").write(text)
            rc, out = run_gate(tpaths)
            ok = rc == 0
            print("%s %s rc=%d%s" % ("[ok]" if ok else "[FAIL]", name, rc,
                                     "（零误报）" if ok else "  -> " + out.strip()[:200]))
            if not ok:
                failures += 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    total = len(INJECTIONS) + len(CONTROLS)
    if failures:
        print("GATE9 双边验证 FAIL：%d/%d 项不符" % (failures, total))
        sys.exit(1)
    print("GATE9 双边验证 PASS：%d 注入全拦 + %d 对照零误报" % (len(INJECTIONS), len(CONTROLS)))


if __name__ == "__main__":
    main()
