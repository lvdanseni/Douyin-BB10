#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""GATE10 独立验证：1.0.0.29 四项需求轮（可观测性/图标/刷新/发表）。

单一事实源：直接抽 build.sh 内嵌的 <<'PYGATE10' ... PYGATE10 段执行，
不在本文件复制逻辑（复制 = 两处漂移 = 门禁自己骗自己）。
真实源码必须 0 错 0 警告。
"""
import io
import os
import re
import sys

PROJ = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(PROJ, "build.sh")


def extract():
    src = open(BUILD, "r", encoding="utf-8", errors="replace").read()
    m = re.search(r"<<'PYGATE10'\n(.*?)\nPYGATE10\n", src, re.S)
    if not m:
        print("FATAL: build.sh 里找不到 PYGATE10 段")
        sys.exit(2)
    return m.group(1)


def run_gate(paths):
    """执行抽出的 GATE10 段，返回 (exit_code, stdout_lines)。"""
    code = extract()
    buf = io.StringIO()
    old_out, old_argv = sys.stdout, sys.argv
    sys.stdout = buf
    sys.argv = ["gate10"] + list(paths)
    ns = {"__name__": "__gate__"}
    rc = 0
    try:
        exec(compile(code, "<PYGATE10>", "exec"), ns)
    except SystemExit as e:
        rc = e.code if isinstance(e.code, int) else 1
    except Exception as e:  # noqa: BLE001 - 门禁代码自身崩了也是失败
        rc = 99
        buf.write("GATE10 internal error: %r\n" % (e,))
    finally:
        sys.stdout, sys.argv = old_out, old_argv
    return rc, buf.getvalue().splitlines()


def main():
    files = [
        os.path.join(PROJ, "assets/main.qml"),
        os.path.join(PROJ, "assets/DouyinFeedPage.qml"),
        os.path.join(PROJ, "assets/DouyinCommentPage.qml"),
        os.path.join(PROJ, "assets/DouyinHistoryPage.qml"),
        os.path.join(PROJ, "assets/images/play.png"),
        os.path.join(PROJ, "assets/images/pause.png"),
        os.path.join(PROJ, "assets/images/refresh.png"),
        os.path.join(PROJ, "src/douyin_api.hpp"),
        os.path.join(PROJ, "src/douyin_api.cpp"),
        os.path.join(PROJ, "src/douyin_store.hpp"),
        os.path.join(PROJ, "src/douyin_store.cpp"),
        os.path.join(PROJ, "bar-descriptor.xml"),
    ]
    missing = [p for p in files if not os.path.exists(p)]
    if missing:
        print("FATAL: 缺少被检文件: %s" % missing)
        sys.exit(2)
    rc, lines = run_gate(files)
    for ln in lines:
        print(ln)
    if rc != 0:
        print("GATE10 FAIL rc=%d（真实源码必须 0 错 0 警告）" % rc)
        sys.exit(1)
    print("GATE10 PASS（独立版与 build.sh 内嵌版同一份代码）")


if __name__ == "__main__":
    main()
