# -*- coding: utf-8 -*-
"""
DouyinBar（BB10 Cascades Qt 4.8）全量 QML 自检脚本 —— 构建门禁版。

【为什么存在】QML 解析错误在桌面编译期完全不可见：qcc 零错误、BAR 打包成功，
只有装到设备上 QmlDocument 加载时才炸，且一炸就是整个首页（Type xxx unavailable）
不可用。本脚本把已踩过的坑全部前移到构建之前，由 build.sh 的 [GATE] 强制调用。

【2026-11-05 新增：相对 import 解析模拟器】
真机截图错误：
    48:13 Type HomePage unavailable
    24:14 Type NoteFeedPage unavailable
     4:1 import "components" has no qmldir and no namespace
根因是 assets/components/NoteFeedPage.qml 第 4 行写了 import "components"。
Qt 4.8 的相对 import 是「相对于当前 .qml 所在目录」解析的，于是变成
assets/components/components/ —— 该目录不存在，且没有 qmldir，所以报
"has no qmldir and no namespace"，再级联让 NoteFeedPage / HomePage 全部 unavailable。

【架构 ground truth（真机可运行的 BiliClient.bar 解包实证）】
    native/assets/*.qml            写 import "components"（根目录，解析成功）
    native/assets/components/*.qml  只有 import bb.cascades 1.1，无任何相对 import
    native/assets/components/       没有 qmldir，靠 Qt 4.8 隐式目录导入
因此本脚本强制：components/ 内不得出现相对 import，且不得存在 qmldir。

检查清单：
   1. 每个 .qml 以 `import bb.cascades 1.1` 开头
   2. assets 根目录下使用 components/ 组件的文件必须有 `import "components"`
   3. 相对 import 解析模拟器：目标目录必须真实存在（复现设备侧解析规则）
   4. components/ 内禁止任何相对 import（回归 golden 架构）
   5. components/ 下不得存在 qmldir
   6. 使用到的组件类型，对应的 assets/components/<Type>.qml 必须存在
   7. ComponentDefinition source 指向的文件必须存在
   8. 禁止属性（Cascades 1.1 不存在）：background 只能用在 Container /
      StandardPickerItem 上（container.h:99），Page/ImageView/Label 等都没有；
      filterColor、Label.maxLines、ListView.scrollViewProperties 一律禁止
   9. 禁止信号/处理器：onPopTransitionEnded、onVisibleChanged、
      Container.onTriggered、StandardListItem.onTriggered/onClicked
  10. 真正的块状属性绑定 `prop: { ... }`（prop 不以 on 开头）
  11. ListItemComponent 内部禁止 `type: "item"`
  12. QML 类型可解析性：所有 `Type {` 必须是 1.1 内置类型（白名单）或
      assets 下真实存在的 .qml 文件；`Pane` 等不存在的类型直接拦截
  13. 括号 / 引号平衡（正确词法：先剥字符串再剥注释，避免 http:// 被当注释）
  14. 语法残留：异常标识符粘连、同一行两个孤立标识符（Conta  Container { 那类）
  15. 属性归属类型栈：`layout:` 若挂在 `ListView` 上，值必须是
      StackListLayout / GridListLayout / FlowListLayout。写 Container 的
      StackLayout / DockLayout 会报 "Cannot assign object to property"
      （listview.h:315 的 Q_PROPERTY 是 bb::cascades::ListLayout*），
      并让整个页面 Type unavailable。真机 golden BAR 的 ListView 一律不写 layout
   16. main.qml 中 `page.<sig>.connect(...)` 的目标 ComponentDefinition 页面
      必须真的声明 `signal <sig>`，否则 page.<sig> 是 undefined，会在
      nav.push() 之前抛 TypeError，表现为"点列表项没反应"
   17. 自定义组件的属性赋值合法性：给 `DouyinFeedRow { ... }` 这类自定义组件
      赋值时，属性名必须是该组件自己的 `property` 声明，或来自 Cascades 1.1
      头文件的 Q_PROPERTY 全集（由 NDK 抽取，不手维护白名单）。真机事故：
      DouyinFeedPage.qml:77 写 `meta: ...`，而组件声明的是 `metaText`，设备报
      `Cannot assign to non-existent property "meta"` 并让整个 DouyinFeedPage
      Type unavailable。规则会给出最相近的已声明属性作为纠错提示
   18. 内置 Cascades 类型的属性归属：属性必须属于该类型（含继承链）的
       Q_PROPERTY 集合，集合由 NDK 头文件按 class 继承链解析生成。真机事故：
       `Button` 在 1.1 只有 text/image/imageSource，把 `color` /
       `textStyle.fontSize` 写上去会报 `Cannot assign to non-existent property`
       并让整页 Type unavailable。规则 17 的全局属性并集抓不到这类
       「A 类型有、B 类型没有」的错误，故单列规则 18

误报抑制：
   - `onXxx: { ... }` 是合法信号处理器，不是块状属性绑定
   - CamelCase 类名（ApplicationUI、OpenMAX…）不是标识符粘连
   - 注释、字符串字面量里的 `type: "item"` 不算

用法：python qmlcheck_full.py [assets_dir]
退出码：0 = 通过；1 = 存在 P0 错误（build.sh 会据此拒绝打包）
"""

import re
import os
import sys

ASSETS_DIR = sys.argv[1] if len(sys.argv) > 1 else "assets"
COMPONENTS_DIR = os.path.join(ASSETS_DIR, "components")

COMPONENT_TYPES = ('VideoRow', 'NoteRow', 'DouyinFeedRow',
                   'RegionChip', 'FeedPage', 'NoteFeedPage')

# Cascades 1.1 (NDK 10.1.0.1020) + Qt 4.8 QML 内置类型白名单。
# 来源：D:/bbndk/target_10_1_0_1020/qnx6/usr/include/bb/cascades/ 下头文件类名，
# 加上真机可运行 BiliClient.bar 里实际用过的类型。
# 【必须查证后再加】加到这里的每个类型都应当能在 NDK 头文件里找到对应类。
CASCADES_TYPES = set('''
TabbedPane Tab NavigationPane Page Container Label ImageView ListView
ComponentDefinition StackLayout DockLayout AbsoluteLayout GridLayout ListItemComponent
StandardListItem TextField Button ImageButton ScrollView ActivityIndicator
ForeignWindowControl WindowProperty FlickMode SnapMode LayoutUpdateHandler
TextArea ToggleButton Slider ProgressIndicator Divider Header Subtitle TitleBar
ActionItem MenuDefinition Menu HelpActionItem DeleteActionItem MultiSelectActionItem
ActionSet ListScrollStateHandler Option SegmentedControl RadioGroup RadioOption
CheckBox DateTimePicker DropDown ImageTracker NineSliceImageView WebView
ScrollViewProperties StackLayoutProperties DockLayoutProperties
HorizontalAlignment VerticalAlignment Color Theme ColorTheme SystemDefaults
TextStyle TextStyleDefinition TextStyleRule FontFaceStyleRule FontSizeStyleRule
FontWeightStyleRule ResourceState ImagePaintDefinition ImagePaint ColorPaint
SystemListModel ArrayDataModel GroupDataModel QmlDocument ListItemListener
DataModel ItemType ListItemProvider VisualNode UIObject AbstractPane
AbstractActionItem AbstractToggleButton AbstractButton AbstractTextControl
Control AbstractA11yObject A11yRole KeyListener Shortcut LabelAutoSizeMode
OverScrollEffectMode Overlay ContainerLayout XmlDataModel JsonDataAccess
Invocation InvocationQuery InvokeActionItem InvokeQuery OrientationSupport
SupportedDisplayOrientation DisplayDirection UIConfig SystemShortcut
AbstractAnimation AnimationState FadeTransition RotateTransition ScaleTransition
TranslateTransition SequentialAnimation ParallelAnimation
TouchPropagationMode PropagationMode Paint TileImagePaint RepeatPattern
FontSize FontWeight LayoutOrientation LayoutUpdateHandler
StandardPickerItem SegmentedControlOption PickerProvider
'''.split())

# 明确不存在于 Cascades 1.1 的类型（写进去必然 Type xxx unavailable）。
# Pane：NDK 只有 page.h / abstractpane.h / navigationpane.h / tabbedpane.h，
#       没有 pane.h；golden BAR 与 BiliBar 的播放页根节点都是 Page。
FORBIDDEN_TYPES = {'Pane', 'Connections', 'TapHandler'}

# 1.1 中真正拥有 background Q_PROPERTY 的类型（container.h:99 / standardpickeritem.h:94）
BACKGROUND_ALLOWED = {'Container', 'StandardPickerItem'}

ERRORS = []
WARNINGS = []


def err(msg):
    ERRORS.append(msg)
    print("  [ERROR] " + msg)


def warn(msg):
    WARNINGS.append(msg)
    print("  [WARN]  " + msg)


def norm(p):
    return p.replace('\\', '/')


def strip_comments(s):
    """只去注释、**保留字符串内容**（source: "Xxx.qml" 这类值还要拿来用）。

    单遍词法扫描，避免字符串里的 "http://" 被 // 规则当成行注释，
    把同一行后面的 } 一起吃掉（那是之前 6 个假括号错误的来源）。
    """
    out = []
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c in ('"', "'"):
            q = c
            out.append(c)
            i += 1
            while i < n:
                if s[i] == '\\' and i + 1 < n:
                    out.append(s[i])
                    out.append(s[i + 1])
                    i += 2
                    continue
                out.append(s[i])
                if s[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if c == '/' and i + 1 < n and s[i + 1] == '/':
            while i < n and s[i] != '\n':
                i += 1
            continue
        if c == '/' and i + 1 < n and s[i + 1] == '*':
            i += 2
            while i + 1 < n and not (s[i] == '*' and s[i + 1] == '/'):
                if s[i] == '\n':
                    out.append('\n')
                i += 1
            i += 2
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def lex_strip(s):
    """单遍词法扫描：字符串与注释替换为空、保留换行，用于括号平衡检查。"""
    out = []
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c in ('"', "'"):
            q = c
            i += 1
            while i < n:
                if s[i] == '\\':
                    i += 2
                    continue
                if s[i] == q:
                    i += 1
                    break
                if s[i] == '\n':
                    out.append('\n')
                i += 1
            continue
        if c == '/' and i + 1 < n and s[i + 1] == '/':
            while i < n and s[i] != '\n':
                i += 1
            continue
        if c == '/' and i + 1 < n and s[i + 1] == '*':
            i += 2
            while i + 1 < n and not (s[i] == '*' and s[i + 1] == '/'):
                if s[i] == '\n':
                    out.append('\n')
                i += 1
            i += 2
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def is_comment_line(line):
    return line.lstrip().startswith('//') or line.lstrip().startswith('/*')


def in_components(path):
    return os.path.basename(os.path.dirname(os.path.abspath(path))) == 'components'


def collect_qml_files(root):
    files = []
    for dirpath, dirnames, filenames in os.walk(root):
        for f in filenames:
            if f.endswith('.qml'):
                files.append(os.path.join(dirpath, f))
    return sorted(files)


def read_qml(path):
    try:
        with open(path, encoding='utf-8') as f:
            return f.read()
    except Exception as e:
        err("%s: 读取失败 %s" % (norm(path), e))
        return ""


def used_component_types(src):
    clean = strip_comments(src)
    used = set()
    for t in COMPONENT_TYPES:
        if re.search(r'\b' + t + r'\s*\{', clean):
            used.add(t)
    return used


def check_import_header(path, src):
    if not src.lstrip().startswith("import bb.cascades 1.1"):
        err("%s: 缺少 `import bb.cascades 1.1`（Qt 4.8 需要它作为第一条声明）" % norm(path))


def check_components_import(path, src):
    """只有 assets 根目录下的文件才需要显式 import "components"。"""
    used = used_component_types(src)
    if not used or in_components(path):
        return
    if not re.search(r'import\s+"components"', src):
        err("%s: 使用了 components/ 下的组件 %s，但缺少 `import \"components\"`"
            % (norm(path), sorted(used)))


def check_relative_imports(path, src):
    """【相对 import 解析模拟器】按 Qt 4.8 规则复现设备侧解析。

    `import "xxx"` 相对于当前 .qml 所在目录解析；目标目录不存在时设备报：
        import "xxx" has no qmldir and no namespace
    并让当前类型 unavailable，进而级联到所有引用它的页面。
    """
    base = os.path.dirname(os.path.abspath(path))
    for i, line in enumerate(src.split('\n'), 1):
        if is_comment_line(line):
            continue
        m = re.match(r'\s*import\s+"([^"]+)"', line)
        if not m:
            continue
        rel = m.group(1)
        target = os.path.normpath(os.path.join(base, rel))
        if not os.path.isdir(target):
            err("%s:%d: `import \"%s\"` 解析到不存在的目录 %s —— 设备会报 "
                "\"has no qmldir and no namespace\"，并级联成 Type xxx unavailable"
                % (norm(path), i, rel, norm(target)))


def check_components_no_relative_import(path, src):
    """components/ 内的兄弟类型由 Qt 4.8 隐式解析，写相对 import 必然踩坑。"""
    if not in_components(path):
        return
    for i, line in enumerate(src.split('\n'), 1):
        if is_comment_line(line):
            continue
        m = re.match(r'\s*import\s+"([^"]+)"', line)
        if m:
            rel = m.group(1)
            target = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(path)), rel))
            extra = ("，解析到不存在的 %s" % norm(target)) if not os.path.isdir(target) else ""
            err("%s:%d: components/ 内的 .qml 禁止相对 `import \"%s\"`%s。"
                "真机可运行架构中它们只写 import bb.cascades 1.1；"
                "兄弟类型由 Qt 4.8 隐式同目录解析，无需任何 import"
                % (norm(path), i, rel, extra))


def check_no_qmldir():
    """回归 golden 架构：不引入未经真机验证的 qmldir。"""
    q = os.path.join(COMPONENTS_DIR, "qmldir")
    if os.path.exists(q):
        err("%s: 存在 qmldir —— 真机可运行的 BiliClient.bar 没有它也能正常解析 "
            "import \"components\"；保留它反而会让 Qt 4.8 改用声明式注册，"
            "未声明的类型全部不可用。请删除并清掉构建目录里的历史副本" % norm(q))


def check_component_files_exist(path, src):
    used = used_component_types(src)
    for t in sorted(used):
        f = os.path.join(COMPONENTS_DIR, t + '.qml')
        if not os.path.isfile(f):
            err("%s: 使用了组件类型 %s，但找不到 %s" % (norm(path), t, norm(f)))


def check_component_definition(path, src):
    clean = strip_comments(src)
    base = os.path.dirname(os.path.abspath(path))
    for m in re.finditer(r'ComponentDefinition\s*\{[^}]*?source\s*:\s*"([^"]+)"', clean, re.S):
        source = m.group(1)
        if '../' in source or source.startswith('asset:///'):
            err("%s: ComponentDefinition source '%s' 不应写 ../ 或 asset:///，"
                "应写 assets 根下平级文件名" % (norm(path), source))
            continue
        if not os.path.isfile(os.path.join(base, source)):
            err("%s: ComponentDefinition source '%s' 指向的文件不存在（%s）"
                % (norm(path), source, norm(os.path.join(base, source))))


def check_forbidden_props(path, src):
    lines = src.split('\n')
    # 纠偏：Container.background 在 1.1 **是合法的**（container.h:99），
    # golden BAR 真机也大量使用；真正没有的是 Page / ImageView / Label 等。
    # 归属分析用花括号栈求「最内层包裹对象」：
    #   - `Type {` / `prop: Type {` 入栈 Type；
    #   - 匿名 `{`（onTriggered: 等函数块）入栈 None；
    #   - `}` 弹栈。
    # 单行自闭合的 `layout: DockLayout {}` 净深度为 0，不影响归属；
    # `content: Container { ... background: ... }` 正确归属到 Container。
    def scan_owners(src_text):
        owners = {}  # 行号(1基) -> 该行首个 background: 的归属类型
        stack = []
        for ln, line in enumerate(src_text.split('\n'), 1):
            for m in re.finditer(r'([A-Z][A-Za-z0-9_]*)\s*\{|[{}]|background\s*:', line):
                tok = m.group(0)
                if tok.startswith('background'):
                    owners[ln] = stack[-1] if stack else None
                elif tok == '{':
                    stack.append(None)
                elif tok == '}':
                    if stack:
                        stack.pop()
                else:
                    stack.append(m.group(1))
        return owners

    owners = scan_owners(strip_comments(src))
    for i, line in enumerate(lines, 1):
        if is_comment_line(line) or not re.search(r'\bbackground\s*:', line):
            continue
        owner = owners.get(i)
        if owner and owner not in BACKGROUND_ALLOWED:
            err("%s:%d: %s 上没有 background 属性（1.1 只有 Container / "
                "StandardPickerItem 有，见 container.h:99）；用铺满的 Container 垫底色"
                % (norm(path), i, owner))
    clean = strip_comments(src)
    if re.search(r'\bfilterColor\s*:', clean):
        err("%s: 禁止 filterColor（Cascades 1.1 ImageView 无此属性，1.4 才有）" % norm(path))
    if re.search(r'\bmaxLines\s*:', clean):
        err("%s: 禁止 maxLines（Label 只有 multiline）" % norm(path))
    for m in re.finditer(r'ListView\s*\{', clean):
        window = clean[m.end():m.end() + 3000]
        if re.search(r'scrollViewProperties\s*\{', window):
            err("%s: scrollViewProperties 是 ScrollView 的属性，ListView 上不存在" % norm(path))
            break


def check_forbidden_signals(path, src):
    clean = strip_comments(src)
    for sig in ['onPopTransitionEnded', 'onVisibleChanged']:
        if re.search(r'\b' + sig + r'\s*:', clean):
            err("%s: 禁止 %s（Cascades 1.1 Page 没有此信号）" % (norm(path), sig))
    if re.search(r'Container\s*\{[^}]*?\bonTriggered\s*:', clean, re.S):
        err("%s: Container 上禁止 onTriggered（Container 无 triggered() 信号）" % norm(path))
    if re.search(r'StandardListItem\s*\{[^}]*?\b(onTriggered|onClicked)\s*:', clean, re.S):
        err("%s: StandardListItem 不支持 onTriggered/onClicked" % norm(path))


def check_block_binding(path, src):
    for i, line in enumerate(src.split('\n'), 1):
        if is_comment_line(line):
            continue
        m = re.match(r'\s*([A-Za-z_][\w.]*)\s*:\s*\{', line)
        if m:
            name = m.group(1)
            if name.startswith('on'):
                continue
            if name in ('layout', 'layoutProperties', 'textStyle'):
                continue
            err("%s:%d: 块状属性绑定 `%s: { ... }` —— Qt 4.8 把 { 当对象字面量，"
                "必须改成单表达式或函数调用" % (norm(path), i, name))


def check_type_item_in_listitem(path, src):
    clean = strip_comments(src)
    for m in re.finditer(r'ListItemComponent\s*\{(.*?)\}', clean, re.S):
        if re.search(r'type\s*:\s*"item"', m.group(1)):
            err("%s: ListItemComponent 内禁止 type: \"item\"（ArrayDataModel itemType "
                "默认空字符串，写了匹配失败，整页只显示 bvid）" % norm(path))
            break


def check_syntax_residue(path, src):
    known_classes = {'ApplicationUI', 'OpenMAX', 'QDeclarative', 'QNetwork', 'QSslSocket',
                     'QVariantMap', 'QVariantList', 'QJsonObject', 'QJsonDocument',
                     'QNetworkRequest', 'QNetworkReply', 'QAbstractListModel',
                     'ArrayDataModel', 'DataModel', 'QThread', 'QTimer', 'QObject',
                     'QUrl', 'QByteArray', 'QString', 'QHash', 'QList', 'QDir',
                     'QFile', 'QFileInfo', 'QDateTime', 'QTextStream', 'QCoreApplication',
                     'QProcess', 'QCryptographicHash', 'QSslCertificate', 'QSslError',
                     'QSslConfiguration', 'QSsl', 'QUuid',
                     'qmlLogText', 'proxyEnvDiag', 'playerLogText', 'bootLogText', 'diagText'}
    for i, line in enumerate(src.split('\n'), 1):
        if is_comment_line(line):
            continue
        line_no_str = re.sub(r'"[^"]*"', '""', line)
        for m in re.finditer(r'[a-z][A-Z][A-Z]', line_no_str):
            matched = m.group(0)
            if not any(matched in cls for cls in known_classes):
                err("%s:%d: 疑似标识符粘连：%s" % (norm(path), i, line.strip()))
                break


def qml_type_names(assets_dir):
    names = set()
    for dp, dn, fns in os.walk(assets_dir):
        for fn in fns:
            if fn.endswith('.qml'):
                names.add(fn[:-4])
    return names


def check_qml_types(path, src, custom_names):
    """所有 `Type {` 必须能解析，否则设备上就是 Type xxx unavailable。"""
    clean = lex_strip(src)  # 字符串里的内容不能当类型名
    reported = set()
    for m in re.finditer(r'\b([A-Z][A-Za-z0-9_]*)\s*\{', clean):
        t = m.group(1)
        if t in reported:
            continue
        if t in FORBIDDEN_TYPES:
            reported.add(t)
            err("%s: 使用了 Cascades 1.1 不存在的 QML 类型 `%s`（NDK 无对应头文件/"
                "类注册），会让本页 Type unavailable。请改用真机验证过的类型"
                % (norm(path), t))
        elif (t not in CASCADES_TYPES and t not in custom_names
              and not t.startswith('Window')):
            reported.add(t)
            err("%s: 未知 QML 类型 `%s` —— 既不是 1.1 内置白名单里的类型，"
                "也找不到 assets 下的 %s.qml；请先查 NDK 头文件确认后再加白名单"
                % (norm(path), t, t))


def check_delimiter_balance(path, src):
    """括号/引号平衡。正确词法，避免字符串里的 // 干扰。"""
    s = lex_strip(src)
    stack = []
    pairs = {'}': '{', ')': '(', ']': '['}
    line = 1
    for ch in s:
        if ch == '\n':
            line += 1
        elif ch in '({[':
            stack.append((ch, line))
        elif ch in '})]':
            if not stack or stack[-1][0] != pairs[ch]:
                err("%s:%d: 括号不匹配 '%s'（栈顶 %s）"
                    % (norm(path), line, ch, stack[-1] if stack else '空'))
                return
            stack.pop()
    if stack:
        err("%s: 括号未闭合 %s" % (norm(path), stack[-3:]))


def check_double_identifier(path, src):
    """抓 `Conta            Container {` 这类编辑残留。"""
    for i, line in enumerate(src.split('\n'), 1):
        if is_comment_line(line):
            continue
        if re.match(r'^\s*[A-Za-z_]\w*\s{2,}[A-Za-z_]\w*\s*\{', line):
            err("%s:%d: 同一行出现两个孤立标识符，疑似语法残留：%s"
                % (norm(path), i, line.strip()))


def type_stack_properties(src, prop='layout'):
    """用大括号类型栈判断某个属性的归属类型（比"向前找最近 Type {" 精确）。"""
    s = lex_strip(src)
    stack, hits = [], []
    line, i, n = 1, 0, len(s)
    while i < n:
        c = s[i]
        if c == '\n':
            line += 1
            i += 1
            continue
        if c == prop[0]:
            m = re.match(re.escape(prop) + r'\s*:', s[i:])
            if m and (i == 0 or s[i - 1] in ' \t\n'):
                owner = next((t for t in reversed(stack) if t), '?')
                rest = s[i + m.end(): i + m.end() + 60].split('\n')[0].strip()
                hits.append((line, owner, rest))
                i += m.end()
                continue
        m = re.match(r'([A-Z][A-Za-z0-9_]*)\s*\{', s[i:])
        if m:
            stack.append(m.group(1))
            i += m.end()
            continue
        if c == '{':
            stack.append('')
            i += 1
            continue
        if c == '}':
            if stack:
                stack.pop()
            i += 1
            continue
        i += 1
    return hits


def check_listview_layout(path, src):
    for line, owner, rest in type_stack_properties(src, 'layout'):
        if owner == 'ListView' and not re.search(
                r'\b(StackListLayout|GridListLayout|FlowListLayout)\b', rest):
            err("%s:%d: `layout:` 挂在 ListView 上，值是 `%s`。ListView.layout 的类型是 "
                "bb::cascades::ListLayout*（listview.h:315），Container 的 StackLayout/"
                "DockLayout 塞进去会报 Cannot assign object to property 并让整页 "
                "Type unavailable。真机 golden BAR 的 ListView 一律不写 layout —— 删掉即可"
                % (norm(path), line, rest))


def check_page_signal_connects(assets_dir):
    """main.qml 里给 createObject() 出来的 page 挂 .connect 时，页面必须声明该 signal。

    这次的真实事故：pushDyVideo 里 page.back.connect(...)，但 DouyinPlayerPage
    没声明 signal back —— TypeError 让 nav.push(page) 永远执行不到。
    """
    main_qml = os.path.join(assets_dir, 'main.qml')
    if not os.path.isfile(main_qml):
        return
    with open(main_qml, encoding='utf-8') as f:
        clean = strip_comments(f.read())

    defs = {}
    for m in re.finditer(r'ComponentDefinition\s*\{([^{}]*)\}', clean, re.S):
        mid = re.search(r'\bid\s*:\s*(\w+)', m.group(1))
        msrc = re.search(r'\bsource\s*:\s*"([^"]+)"', m.group(1))
        if mid and msrc:
            defs[mid.group(1)] = msrc.group(1)

    cur_var, cur_source = None, None
    for i, line in enumerate(clean.split('\n'), 1):
        m = re.search(r'\bvar\s+(\w+)\s*=\s*(\w+)\.createObject\s*\(', line)
        if m:
            cur_var, cur_source = m.group(1), defs.get(m.group(2))
            continue
        if re.search(r'\bnav\.push\s*\(', line):
            cur_var, cur_source = None, None
            continue
        if not (cur_var and cur_source):
            continue
        target = os.path.join(assets_dir, cur_source)
        if not os.path.isfile(target):
            continue
        for cm in re.finditer(r'\b' + re.escape(cur_var) + r'\.(\w+)\.connect\s*\(', line):
            sig = cm.group(1)
            with open(target, encoding='utf-8') as pf:
                psrc = strip_comments(pf.read())
            if not re.search(r'\bsignal\s+' + re.escape(sig) + r'\b', psrc):
                err("main.qml:%d: 对 %s.%s.connect(...) 挂回调，但 %s 没有声明 "
                    "`signal %s`；%s.%s 是 undefined，会在 nav.push() 之前抛 "
                    "TypeError（表现为点击无反应）。要么在页面里声明 signal，"
                    "要么改同步直调"
                    % (i, cur_var, sig, cur_source, sig, cur_var, sig))


def cascades_property_names():
    """从 NDK Cascades 1.1 头文件抽取 Q_PROPERTY 全集（继承属性合法集）。

    以 SDK 头文件为唯一 ground truth，不手维护白名单。找不到 NDK 时返回
    None，调用方跳过规则 17 并告警，避免在没有 SDK 的机器上大面积误报。
    """
    import glob as _glob
    bases = _glob.glob("D:/bbndk/target*/qnx6/usr/include/bb/cascades")
    if not bases:
        return None
    props = set()
    for h in _glob.glob(bases[0] + "/**/*.h", recursive=True):
        try:
            with open(h, encoding="utf-8", errors="replace") as f:
                s = f.read()
        except Exception:
            continue
        for m in re.finditer(r'Q_PROPERTY\s*\(([^)]*)\)', s, re.S):
            body = " ".join(m.group(1).split())
            mm = re.match(r'(?:[\w:<>,*&\s]+?)\s+(\w+)\s+'
                          r'(?:READ|WRITE|NOTIFY|RESET|CONSTANT|FINAL|MEMBER)\b', body)
            if mm:
                props.add(mm.group(1))
            else:
                toks = body.split()
                if len(toks) >= 2:
                    props.add(toks[1])
    return props or None


def custom_component_properties(assets_dir):
    """每个 .qml 文件名 = 一个自定义组件类型，收集其 property 声明。"""
    decl = {}
    for dp, dn, fns in os.walk(assets_dir):
        for fn in fns:
            if not fn.endswith('.qml'):
                continue
            p = os.path.join(dp, fn)
            with open(p, encoding="utf-8") as f:
                s = strip_comments(f.read())
            decl[fn[:-4]] = set(re.findall(
                r'^\s*property\s+\w+\s+(\w+)\s*[:=]', s, re.M))
    return decl


# QML 语言层面 / 分组属性：不是 Q_PROPERTY，但对自定义组件合法
QML_ONLY_PROPS = set("""id objectName property signal function listItemComponents
attachedObjects animations gestures shortcuts eventHandlers actions
contextMenuHandler accessibility focusPolicy effect layoutProperties
inputRoute activationChanged""".split())


def assignment_owner_hits(src):
    """逐字符类型栈；只认「行首位置的 prop:」赋值，排除三元与 JS 对象字面量。

    返回 [(行号, 归属类型, 属性名, 值前缀)]。归属类型取最近的非空类型栈帧，
    与真实 QML 作用域一致（嵌套的 Label/Container 会遮住外层自定义组件）。
    """
    s = lex_strip(src)
    stack, hits = [], []
    line, line_start, i, n = 1, 0, 0, len(s)
    while i < n:
        c = s[i]
        if c == '\n':
            line += 1
            i += 1
            line_start = i
            continue
        m = re.match(r'([A-Z][A-Za-z0-9_]*)\s*\{', s[i:])
        if m:
            stack.append(m.group(1))
            i += m.end()
            continue
        if c == '{':
            stack.append('')
            i += 1
            continue
        if c == '}':
            if stack:
                stack.pop()
            i += 1
            continue
        if (c.isalpha() or c == '_') and s[line_start:i].strip() == '':
            am = re.match(r'([A-Za-z_]\w*)\s*:\s*([^\n]*)', s[i:])
            if am:
                owner = next((t for t in reversed(stack) if t), '?')
                hits.append((line, owner, am.group(1), am.group(2).strip()[:50]))
                i += am.end()
                continue
        i += 1
    return hits


def check_custom_component_props(path, src, decl, cprops):
    """规则 17：自定义组件的赋值属性必须真实存在。"""
    if not cprops:
        return
    import difflib
    for line, owner, prop, val in assignment_owner_hits(src):
        if owner not in decl:
            continue
        if prop.startswith('on') or prop in QML_ONLY_PROPS or prop in cprops:
            continue
        if prop in decl[owner]:
            continue
        hint = ""
        close = difflib.get_close_matches(prop, sorted(decl[owner]), n=1, cutoff=0.4)
        if close:
            hint = "；该组件声明了相近的 `%s`，是否拼错？" % close[0]
        err("%s:%d: 对自定义组件 %s 赋值了不存在的属性 `%s`（值 `%s`）—— 设备会报 "
            "`Cannot assign to non-existent property \"%s\"` 并让整页 Type unavailable%s"
            " 属性必须在该组件的 `property` 声明里，或来自 Cascades 1.1 的 Q_PROPERTY"
            % (norm(path), line, owner, prop, val, prop, hint))


def cascades_type_properties():
    """返回 {QML 类型名: 含继承链的 Q_PROPERTY 集合}，以 NDK 头文件为 ground truth。

    规则 17 用的是「所有类型的属性并集」，只能判断属性在 Cascades 里是否存在；
    本函数进一步按类型细分，用来抓「把 Label 的属性写到 Button 上」这类错误。
    """
    import glob as _glob
    bases = _glob.glob("D:/bbndk/target*/qnx6/usr/include/bb/cascades")
    if not bases:
        return {}
    cls = {}
    for h in _glob.glob(bases[0] + "/**/*.h", recursive=True):
        try:
            with open(h, encoding="utf-8", errors="replace") as f:
                src = f.read()
        except Exception:
            continue
        cm = re.search(r'\bclass\s+(?:BBCASCADES_EXPORT|BB_CASCADES_EXPORT)\s+'
                       r'(\w+)\s*:\s*public\s+([\w:]+)', src)
        if not cm:
            continue
        props = set()
        for pm in re.finditer(r'Q_PROPERTY\s*\(([^)]*)\)', src, re.S):
            body = " ".join(pm.group(1).split())
            mm = re.match(r'(?:[\w:<>,*&\s]+?)\s+(\w+)\s+'
                          r'(?:READ|WRITE|NOTIFY|RESET|CONSTANT|FINAL|MEMBER)\b', body)
            if mm:
                props.add(mm.group(1))
            else:
                toks = body.split()
                if len(toks) >= 2:
                    props.add(toks[1])
        cls[cm.group(1)] = (cm.group(2).split('::')[-1], props)

    def inherited(name, seen=None):
        seen = seen or set()
        if name in seen or name not in cls:
            return set()
        seen.add(name)
        base, own = cls[name]
        return own | inherited(base, seen)

    return dict((k, inherited(k)) for k in cls)


# QML 语法层 / 分组属性 / NDK 头文件未收录但真机验证过的合法属性
BUILTIN_WHITELIST = set("""id objectName property signal function attachedObjects
animations gestures shortcuts eventHandlers actions layoutProperties accessibility
listItemComponents titleBar acceptAction dismissAction submitKey""".split())


def typed_assignment_hits(src):
    """按大括号类型栈归属属性赋值；支持 `textStyle.fontSize:` 这类分组属性。

    与 assignment_owner_hits 的差别：命中后只跳过 `prop:` 本身，不吞掉整行，
    这样 `layout: StackLayout { ... }` 里的内嵌类型才会正确入栈，避免把
    TitleBar / ListItemComponent 的属性误算到外层类型头上。
    """
    s = lex_strip(src)
    stack, res = [], []
    line, ls, i, n = 1, 0, 0, len(s)
    while i < n:
        c = s[i]
        if c == '\n':
            line += 1
            i += 1
            ls = i
            continue
        m = re.match(r'([A-Z][A-Za-z0-9_]*)\s*\{', s[i:])
        if m:
            stack.append(m.group(1))
            i += m.end()
            continue
        if c == '{':
            stack.append('')
            i += 1
            continue
        if c == '}':
            if stack:
                stack.pop()
            i += 1
            continue
        if (c.isalpha() or c == '_') and s[ls:i].strip() == '':
            am = re.match(r'([A-Za-z_]\w*)((?:\.\w+)*)\s*:', s[i:])
            if am:
                owner = next((t for t in reversed(stack) if t), '?')
                val = s[i + am.end():i + am.end() + 50].split('\n')[0].strip()
                res.append((line, owner, am.group(1), val))
                i += am.end()
                continue
        i += 1
    return res


def check_builtin_type_props(path, src, tprops):
    """规则 18：内置 Cascades 类型的属性必须属于该类型（含继承链）。"""
    if not tprops:
        return
    for line, owner, prop, val in typed_assignment_hits(src):
        if owner not in tprops:
            continue
        if prop in BUILTIN_WHITELIST or prop.startswith('on'):
            continue
        if prop in tprops[owner]:
            continue
        err("%s:%d: `%s` 上不存在属性 `%s`（值 `%s`）—— 该类型在 Cascades 1.1 "
            "的 Q_PROPERTY 集合（含继承链，共 %d 个）来自 NDK 头文件。设备会报 "
            "`Cannot assign to non-existent property \"%s\"` 并让整页 Type unavailable"
            % (norm(path), line, owner, prop, val, len(tprops[owner]), prop))


# ---------------------------------------------------------------------------
# 规则 19：pane 子节点的根节点类型（1.0.0.5 真机 `120:13 Cannot assign
# object to property` 的漏网之鱼）
#
#   NavigationPane 的子节点是 bb::cascades::Page*（AbstractPane 派生），
#   Tab 的 content 同样是 AbstractPane*。Container 只是 Control*，赋不进去。
#   Qt 4.8 不会报「类型不匹配」，只含糊地报 "Cannot assign object to property"，
#   错误信息里既没有组件名也没有属性名，极难定位 —— 所以要静态拦。
# ---------------------------------------------------------------------------
PANE_ROOT_TYPES = set(['Page', 'NavigationPane', 'TabbedPane',
                       'AbstractPane', 'Sheet', 'Dialog'])


def root_qml_type(path):
    """取 QML 根节点类型名（先去注释/字符串，避免注释里的 `Xxx {` 干扰）。"""
    s = lex_strip(read_qml(path))
    m = re.search(r'^\s*([A-Z][A-Za-z0-9_]*)\s*\{', s, re.M)
    return m.group(1) if m else '?'


def type_instance_hits(src):
    """返回 (行号, 类型名, 直接父级类型名)；用于判断谁被塞进了 pane。"""
    s = lex_strip(src)
    stack, res = [], []
    line, i, n = 1, 0, len(s)
    while i < n:
        c = s[i]
        if c == '\n':
            line += 1
            i += 1
            continue
        m = re.match(r'([A-Z][A-Za-z0-9_]*)\s*\{', s[i:])
        if m:
            t = m.group(1)
            parent = next((x for x in reversed(stack) if x), '')
            res.append((line, t, parent))
            stack.append(t)
            i += m.end()
            continue
        if c == '{':
            stack.append('')
            i += 1
            continue
        if c == '}':
            if stack:
                stack.pop()
            i += 1
            continue
        i += 1
    return res


def check_pane_root_types(assets_dir):
    """规则 19：页面根节点 / pane 子节点 / 页面工厂的目标都必须是 pane 类型。"""
    files = collect_qml_files(assets_dir)
    roots = {}
    for path in files:
        name = os.path.splitext(os.path.basename(path))[0]
        roots[name] = (root_qml_type(path), path)

    # 19a：assets 根目录下每个 .qml 都是页面（main.qml = TabbedPane），
    #      components/ 下的才是 Control（Container 根），必须区分对待。
    for path in files:
        if in_components(path):
            continue
        name = os.path.splitext(os.path.basename(path))[0]
        if name == 'main':
            continue
        rt = roots[name][0]
        if rt not in PANE_ROOT_TYPES:
            err("%s: 页面根节点是 `%s`（Control*），但 assets 根目录下的 QML "
                "都作为 NavigationPane/Tab 的子页面使用，根节点必须是 Page "
                "或其它 AbstractPane 派生类型。若确实要放 Control，请用 "
                "`Page { content: Container { ... } }` 包一层"
                % (norm(path), rt))

    for path in files:
        src = read_qml(path)
        # 19b：NavigationPane / Tab 直接实例化的自定义组件
        for line, t, parent in type_instance_hits(src):
            if parent not in ('NavigationPane', 'Tab'):
                continue
            if t not in roots:
                continue
            rt = roots[t][0]
            if rt not in PANE_ROOT_TYPES:
                err("%s:%d: `%s {` 被直接放进 `%s`，但 %s.qml 的根节点是 `%s`"
                    "（Control*）；pane 子节点要求 AbstractPane*，设备会报 "
                    "`Cannot assign object to property`。把 %s.qml 根节点改成 "
                    "Page 并把原内容包进 content: Container {}"
                    % (norm(path), line, t, parent, t, rt, t))
        # 19c：ComponentDefinition 的页面工厂（createObject 后 nav.push）
        #      components/ 下的 Control 工厂不在此列（例如 RegionChip.qml）。
        clean = strip_comments(src)
        for m in re.finditer(
                r'ComponentDefinition\s*\{[^}]*?source\s*:\s*"([^"]+)"',
                clean, re.S):
            source = m.group(1)
            if '/' in source:
                continue
            name = os.path.splitext(source)[0]
            if name in roots and roots[name][0] not in PANE_ROOT_TYPES:
                err("%s: ComponentDefinition source '%s' 的根节点是 `%s`，"
                    "createObject() 得到的不是 Page，nav.push() 会失败"
                    % (norm(path), source, roots[name][0]))
    return roots


# 规则 20：禁止把 C++ 类名当成 QML 全局单例访问
#   （1.0.0.6/1.0.0.7 设备日志 `NotePage.qml:256: ReferenceError:
#    Can't find variable: BiliApi` 的漏网之鱼）
#
#   `BiliApi.countText(...)` 在语法上完全合法，Qt 4.8 解析期一声不吭，
#   只有页面真正实例化、表达式求值时才抛 ReferenceError。前 19 条规则都
#   查属性/类型/根节点，查不到这种「标识符根本没注入」的问题。
#   C++ 类不会自动变成 QML 全局对象，必须用 setContextProperty 注入的
#   小写名字（本工程是 biliApi）。类名表直接从 src/*.hpp 自动提取，
#   以后新增 Store/Api 类也自动纳入检查。
# ---------------------------------------------------------------------------
SINGLETON_SUFFIXES = ('Api', 'API', 'Client', 'Store', 'Model',
                      'Manager', 'Service', 'Helper', 'Adapter', 'Controller')


def cpp_class_names(assets_dir):
    """从 src/*.hpp 提取所有 C++ 类名，作为「不可当 QML 全局对象」的黑名单。"""
    src_dir = os.path.join(os.path.dirname(os.path.abspath(assets_dir)), 'src')
    names = set()
    if not os.path.isdir(src_dir):
        return names
    for fn in os.listdir(src_dir):
        if not fn.endswith(('.hpp', '.h')):
            continue
        try:
            with open(os.path.join(src_dir, fn), encoding='utf-8',
                      errors='ignore') as fh:
                text = fh.read()
        except IOError:
            continue
        names |= set(re.findall(r'^\s*class\s+([A-Z][A-Za-z0-9_]*)', text, re.M))
    return names


def check_cpp_singleton_refs(path, src, cpp_names):
    """规则 20：`XxxCppClass.member` 或 `XxxApi.member` 一律拦下。"""
    # lex_strip 同时去掉注释与字符串字面量，避免 main.qml 里复盘注释、
    # "asset:///BiliClient.bar" 这类文本造成误报。
    s = lex_strip(src)
    seen = set()
    for m in re.finditer(r'\b([A-Z][A-Za-z0-9_]*)\s*\.', s):
        name = m.group(1)
        if name not in cpp_names and not name.endswith(SINGLETON_SUFFIXES):
            continue
        if name in seen:
            continue
        seen.add(name)
        line = s.count('\n', 0, m.start()) + 1
        err("%s:%d: `%s.` 把 C++ 类型当成 QML 全局对象访问。C++ 类不会自动注册"
            "成 QML 单例，运行到这一行会抛 `ReferenceError: Can't find variable: "
            "%s`。请改用 setContextProperty 注入的名字（如 `biliApi.xxx`），"
            "或确认该名字确实是 QML 类型/枚举而非 C++ 类名"
            % (norm(path), line, name, name))


def print_import_matrix(files):
    """打印全部相对 import 的解析结果，便于人工核对解析路径。"""
    print("== 相对 import 解析模拟器（Qt 4.8：相对于当前 .qml 所在目录）==")
    for path in files:
        with open(path, encoding='utf-8') as f:
            lines = f.read().split('\n')
        for i, line in enumerate(lines, 1):
            m = re.match(r'\s*import\s+"([^"]+)"', line)
            if not m or is_comment_line(line):
                continue
            rel = m.group(1)
            target = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(path)), rel))
            ok = os.path.isdir(target)
            print("  %s  %s:%d  \"%s\" -> %s"
                  % ("OK  " if ok else "FAIL", norm(os.path.relpath(path, ASSETS_DIR)),
                     i, rel, norm(target)))
    print()


def main():
    if not os.path.isdir(ASSETS_DIR):
        err("assets 目录不存在: %s" % norm(ASSETS_DIR))
        sys.exit(1)

    files = collect_qml_files(ASSETS_DIR)
    print("共发现 %d 个 QML 文件（assets=%s）" % (len(files), norm(ASSETS_DIR)))
    print()
    print_import_matrix(files)
    check_no_qmldir()
    check_page_signal_connects(ASSETS_DIR)
    pane_roots = check_pane_root_types(ASSETS_DIR)
    print("规则 19 页面根节点表：%d 个 QML（pane 类型 %d 个）"
          % (len(pane_roots),
             sum(1 for rt, _ in pane_roots.values() if rt in PANE_ROOT_TYPES)))
    custom_names = qml_type_names(ASSETS_DIR)
    cpp_names = cpp_class_names(ASSETS_DIR)
    print("规则 20 C++ 类名表：%d 个（禁止当成 QML 全局单例）" % len(cpp_names))
    cprops = cascades_property_names()
    decl = custom_component_properties(ASSETS_DIR)
    tprops = cascades_type_properties()
    if cprops is None:
        warn("未找到 NDK Cascades 头文件，跳过规则 17（自定义组件属性合法性）")
    else:
        print("规则 17 属性全集：NDK Q_PROPERTY %d 个 / 自定义组件 %d 个"
              % (len(cprops), len(decl)))
        if tprops:
            print("规则 18 类型属性表：%d 个 Cascades 类型（含继承链）" % len(tprops))
        else:
            warn("未找到 NDK Cascades 头文件，跳过规则 18（内置类型属性归属）")

    for path in files:
        rel = norm(os.path.relpath(path, ASSETS_DIR))
        src = read_qml(path)
        if src == "":
            continue
        print("== 检查 %s ==" % rel)
        check_import_header(path, src)
        check_components_import(path, src)
        check_relative_imports(path, src)
        check_components_no_relative_import(path, src)
        check_component_files_exist(path, src)
        check_component_definition(path, src)
        check_forbidden_props(path, src)
        check_forbidden_signals(path, src)
        check_block_binding(path, src)
        check_type_item_in_listitem(path, src)
        check_qml_types(path, src, custom_names)
        check_listview_layout(path, src)
        check_delimiter_balance(path, src)
        check_double_identifier(path, src)
        check_syntax_residue(path, src)
        check_custom_component_props(path, src, decl, cprops)
        check_builtin_type_props(path, src, tprops)
        check_cpp_singleton_refs(path, src, cpp_names)

    print()
    print("=" * 60)
    print("自检结果：错误 %d 个 / 警告 %d 个" % (len(ERRORS), len(WARNINGS)))
    if ERRORS:
        print("【必须全部修复后才能构建】：")
        for e in ERRORS:
            print("  - " + e)
        sys.exit(1)
    print("全部通过，无 QML PARSE ERROR 隐患")


if __name__ == '__main__':
    main()
