# -*- coding: utf-8 -*-
"""把 QML 里用到的属性名，逐个拿到 Cascades 头文件里核对是否存在 Q_PROPERTY。
找不到的列出来人工判断（可能是 attached property / 枚举 / model 字段 / 局部 id）。"""
import re
import os
import sys

INC = r"D:/bbndk/target_10_1_0_1020/qnx6/usr/include/bb/cascades"

def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    s = re.sub(r'//[^\n]*', '', s)
    return s

def collect_qml(path):
    src = strip_comments(open(path, encoding='utf-8').read())
    props = {}
    # 属性赋值： ident:  或  a.b:
    for m in re.finditer(r'([A-Za-z_][\w.]*)\s*:(?!:)', src):
        name = m.group(1)
        if name == 'id':
            continue
        # 取最后一段
        leaf = name.split('.')[-1]
        line = src[:m.start()].count('\n') + 1
        props.setdefault(leaf, line)
    return props

def header_props():
    """扫描所有头文件，收集 Q_PROPERTY 的属性名"""
    names = set()
    for root, dirs, files in os.walk(INC):
        for f in files:
            if not f.endswith('.h'):
                continue
            try:
                txt = open(os.path.join(root, f), encoding='utf-8', errors='ignore').read()
            except Exception:
                continue
            for m in re.finditer(r'Q_PROPERTY\s*\(([^)]*)\)', txt):
                body = m.group(1)
                # 形如： bool visible READ visible WRITE ...
                #       bb::cascades::DataModel *dataModel READ ...
                # 取 READ 之前最后一个标识符才是属性名（中间可能有 *）
                head = body.split('READ')[0]
                toks = re.findall(r'[A-Za-z_]\w*', head)
                if toks:
                    names.add(toks[-1])
    return names

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'assets/VideoPage.qml'
    qprops = collect_qml(path)
    hprops = header_props()

    # 这些是 QML 语言/框架层面必然存在的，不算风险
    whitelist = {
        'id', 'property', 'signal', 'function', 'onTriggered', 'onCreationCompleted',
        'onTouch', 'onClicked', 'onActivated', 'onCheckedChanged',
    }

    missing = []
    for name, line in sorted(qprops.items(), key=lambda kv: kv[1]):
        if name in whitelist:
            continue
        if name in hprops:
            continue
        missing.append((line, name))

    print("头文件 Q_PROPERTY 总数: %d" % len(hprops))
    print("QML 属性总数: %d" % len(qprops))
    print()
    print("=== 头文件里找不到的（%d 个，需人工判断）===" % len(missing))
    for line, name in missing:
        print("  L%-5d %s" % (line, name))

if __name__ == '__main__':
    main()
