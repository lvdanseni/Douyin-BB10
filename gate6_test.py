#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""GATE6 独立验证 + 双边（锚点 2026-11-07 随 1.0.0.28 预取分支更新）。

单一事实源：抽 build.sh 内嵌 <<'PYGATE6' ... PYGATE6 段执行。
守两条 alts 透传锚点（srcAlts 拷贝 + 三参 start），防"顺手清理"回退。
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
    "src/video_store.cpp",
    "src/video_store.hpp",
    "src/douyin_api.cpp",
    "src/douyin_store.cpp",
    "src/applicationui.cpp",
    "assets/main.qml",
    "assets/DouyinFeedPage.qml",
    "assets/DouyinCommentPage.qml",
]


def extract_gate(tag):
    src = open(BUILD, "r", encoding="utf-8", errors="replace").read()
    m = re.search(r"<<'%s'\n(.*?)\n%s\n" % (tag, tag), src, re.S)
    if not m:
        print("FATAL: build.sh 里找不到 %s 段" % tag)
        sys.exit(2)
    return m.group(1)


def run_gate(paths):
    code = extract_gate("PYGATE6")
    buf = io.StringIO()
    old_out, old_argv = sys.stdout, sys.argv
    sys.stdout = buf
    sys.argv = ["gate6"] + list(paths)
    rc = 0
    ns = {"__name__": "__gate__"}
    try:
        exec(compile(code, "<PYGATE6>", "exec"), ns)
    except SystemExit as e:
        rc = e.code if isinstance(e.code, int) else 1
    except Exception as e:  # noqa: BLE001
        rc = 99
        buf.write("internal error: %r\n" % (e,))
    finally:
        sys.stdout, sys.argv = old_out, old_argv
    return rc, buf.getvalue()


def sub(text, old, new):
    if old not in text:
        raise AssertionError("inject target missing: %r" % old[:60])
    return text.replace(old, new, 1)


INJECTIONS = [
    ("A-删srcAlts拷贝",
     "src/douyin_store.cpp",
     "    QStringList srcAlts = alts;",
     "    QStringList srcAlts;  // INJ-A: 断了 alts 来源",
     "缺少 srcAlts 拷贝"),
    ("B-start退回两参",
     "src/douyin_store.cpp",
     "const QString local = m_proxy ? m_proxy->start(src, srcTotal, srcAlts) : QString();",
     "const QString local = m_proxy ? m_proxy->start(src, srcTotal) : QString();  // INJ-B",
     "未把 alts 透传"),
]


def main():
    paths = [os.path.join(PROJ, r) for r in FILES]
    missing = [p for p in paths if not os.path.exists(p)]
    if missing:
        print("FATAL: 缺少被检文件: %s" % missing)
        sys.exit(2)
    rc, out = run_gate(paths)
    if rc != 0:
        print("baseline FAIL rc=%d:\n%s" % (rc, out))
        sys.exit(1)
    print("[ok] GATE6 baseline 0 错 0 警告")

    failures = 0
    for name, rel, old, new, expect in INJECTIONS:
        tmp = tempfile.mkdtemp(prefix="g6i_")
        try:
            tpaths = []
            for r in FILES:
                dst = os.path.join(tmp, r)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy(os.path.join(PROJ, r), dst)
                tpaths.append(dst)
            text = open(os.path.join(tmp, rel), "r",
                        encoding="utf-8", errors="replace").read()
            open(os.path.join(tmp, rel), "w", encoding="utf-8").write(sub(text, old, new))
            rc, out = run_gate(tpaths)
            ok = rc == 1 and expect in out
            line = next((l for l in out.splitlines() if expect in l), "")
            print("%s %s%s" % ("[ok]" if ok else "[FAIL]", name,
                               ("  -> " + line.strip()) if line else "  rc=%d" % rc))
            if not ok:
                failures += 1
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    if failures:
        print("GATE6 双边 FAIL：%d 项不符" % failures)
        sys.exit(1)
    print("GATE6 双边 PASS：%d 注入全拦" % len(INJECTIONS))


if __name__ == "__main__":
    main()
