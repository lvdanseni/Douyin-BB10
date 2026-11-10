#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""GATE11 独立验证：跨 .o 弱符号碰撞（ODR）自检。

单一事实源：直接抽 build.sh 内嵌的 <<'PYGATE11' ... PYGATE11 段执行。
编译后运行（D:/.obj 的 .o 必须存在）。

背景（1.0.0.29 回归根因）：douyin_api.cpp 与 bili_api.cpp 各自定义同名
class CurlWorker/CurlDownloadWorker（QThread 子类），GCC 把这些成员函数
叮成弱符号，链接期 bili_api.o 靠前胜出 → douyin 的 run() 成死码。
.29 给 douyin 版加 m_contentType（.30 加 m_httpCode）后成员偏移错位：
perform 的返回值写进 m_contentType 槽、code() 读 +4 处从未被写的 m_code
槽 → code 恒 -99、bytes 恒 0，真机表现为「请求发出 23 秒零回复」。
修法：改名 DyCurlWorker/DyCurlDownloadWorker，符号唯一。

故障注入方法（已验证）：对改名前的旧 .o 跑本门禁，必须报出
_ZN10CurlWorker3runEv 等重叠且 rc=1。
"""
import io
import os
import re
import subprocess
import sys

PROJ = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(PROJ, "build.sh")


def extract():
    src = open(BUILD, "r", encoding="utf-8", errors="replace").read()
    m = re.search(r"<<'PYGATE11'\n(.*?)\nPYGATE11\n", src, re.S)
    if not m:
        print("FATAL: build.sh 里找不到 PYGATE11 段")
        sys.exit(2)
    return m.group(1)


def main():
    files = [os.path.join(PROJ, "src/douyin_api.cpp"),
             os.path.join(PROJ, "src/douyin_api.hpp")]
    missing = [p for p in files if not os.path.exists(p)]
    if missing:
        print("FATAL: 缺少被检文件: %s" % missing)
        sys.exit(2)
    obj = None
    for cand in (r"D:\.obj", os.path.abspath("/.obj")):
        if os.path.isdir(cand) and os.path.exists(os.path.join(cand, "douyin_api.o")):
            obj = cand
            break
    if not obj:
        print("FATAL: 找不到 /.obj（必须先编译一次）")
        sys.exit(2)
    env = dict(os.environ)
    env["GATE11_OBJDIR"] = obj
    rc = subprocess.run([sys.executable, "-c",
                         "import sys;exec(compile(sys.stdin.read(),'<PYGATE11>','exec'))",
                         ] + files, input=extract(), capture_output=True, text=True, env=env)
    sys.stdout.write(rc.stdout)
    sys.stderr.write(rc.stderr)
    if rc.returncode != 0:
        print("GATE11 FAIL rc=%d（存在 ODR 碰撞或 douyin 侧类名未 Dy 化）" % rc.returncode)
        sys.exit(1)
    print("GATE11 PASS（源码级 + .o 级双通过）")


if __name__ == "__main__":
    main()
