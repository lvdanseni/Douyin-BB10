# Douyin-BB10
App为黑莓bb10原生抖音客户端，有AI构建仅供个人学习

> BB10 抖音客户端 —— 基于 BlackBerry 10 Cascades 1.1 / Qt 4.8 的原生抖音客户端

[![GitHub release](https://img.shields.io/github/v/release/lvdanseni/Douyin-BB10?label=Latest%20Release)](https://github.com/lvdanseni/Douyin-BB10/releases)
[![Platform](https://img.shields.io/badge/platform-BlackBerry%2010-blue.svg)](https://github.com/lvdanseni/Douyin-BB10)
[![Qt Version](https://img.shields.io/badge/Qt-4.8-green.svg)](https://github.com/lvdanseni/Douyin-BB10)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

## 项目简介

Douyin-BB10 是一款运行在 BlackBerry 10 设备上的原生抖音客户端，基于 Cascades 1.1 / Qt 4.8 框架开发，使用 NDK armv7le 编译，通过 BarDeploy 443 端口部署到 BB10 真机（SQC100-4）。

本项目支持抖音视频流浏览、上滑分页加载、收藏、下载、循环播放、Cookie 登录等核心功能，并包含完整的构建门禁系统（GATE 门禁）与设备取证流程。

## 技术栈

| 模块 | 技术 |
|------|------|
| UI 框架 | BlackBerry Cascades 1.1 / Qt 4.8 |
| 编译器 | `qcc -Vgcc_ntoarmv7le` (NDK armv7le) |
| Java 打包 | `D:\bbndk\jre\bin\java.exe` (NDK JDK 1.6，禁止用系统 Java21) |
| 构建脚本 | `build.sh` (6 步流程) + `Makefile` |
| 门禁检查 | `qmlcheck_full.py` (20 条 QML 规则，0 错 0 警) |
| 部署工具 | `BarDeploy.jar` (443 端口，不依赖 SSH) |
| 目标设备 | BlackBerry SQC100-4, IP `169.254.0.1` |

## 目录结构

```
Douyin-BB10/
├── README.md              # 项目说明（本文件）
├── LICENSE                # MIT 许可证
├── .gitignore             # Git 忽略规则
├── bar-descriptor.xml     # BB10 应用描述符（版本号、图标、权限、启动图标）
├── build.sh               # 一键构建脚本（6 步 + 全门禁 + BAR 打包）
├── Makefile               # Qt 项目 Makefile
├── bili_client.pro        # Qt 项目文件
├── icon.png               # 应用启动图标 (114×114 RGBA)
├── assets/                # QML 页面与资源（28 个 QML 文件）
│   ├── main.qml
│   ├── DouyinFeedPage.qml # 视频流主页（核心手势与分页逻辑）
│   ├── DouyinPlayerPage.qml # 视频播放页
│   ├── components/        # 可复用组件
│   └── images/            # 图标资源矩阵（96/44 尺寸，_s/_sp 双态）
├── src/                   # C++ 源码（26 个文件）
│   ├── main.cpp
│   ├── applicationui.cpp/hpp
│   ├── douyin_store.cpp/hpp # 抖音数据模型（DouyinFeedModel）
│   ├── feed_model.cpp/hpp   # 进馈模型
│   ├── video_prefetch.cpp/hpp # 视频预取器件（单飞/在飞上限/收尸）
│   ├── video_store.cpp/hpp    # 视频存储
│   ├── bili_api.cpp/hpp       # B站 API（复用架构）
│   ├── douyin_api.cpp/hpp     # 抖音 API
│   └── qrcodegen.cpp/hpp      # 二维码生成
├── tools/                 # 开发辅助脚本（Python）
│   ├── gen_icons.py       # 图标生成
│   ├── make_icon.py       # 启动图标制作
│   ├── make_tab_icons.py  # 标签页图标制作
│   ├── qmlcheck.py        # QML 基础检查
│   └── ...                # 其他辅助工具
├── qmlcheck_full.py       # QML 全量门禁检查（20 条规则）
├── gate*_test.py          # GATE 门禁测试文件
└── DouyinBar_1.0.0.58.bar # 已构建的 BAR 安装包（已上传至 GitHub Releases）
```

## 核心功能

- **视频流浏览**：抖音风格上滑分页，自动预取下一片视频
- **手势优化**：修复 `gestureActive` 1500ms 去重阻断，支持快速连滑
- **触底反馈**：列表绝对末端触底 `loadMore()` 时显示 toast 提示
- **视频预取**：单飞下载、在飞上限控制（3 个）、`.part` 中间态原子落地、收尸回收（300s 间隔）
- **循环播放**：支持单视频循环，`ICON loop=on/off` 埋点
- **Cookie 登录**：粘贴 Cookie → `profile/self` 校验，落盘 `homePath()/douyin_cookie.txt`
- **收藏/下载**：支持视频收藏与离线下载

## 环境要求

### 构建环境（Windows）
- BlackBerry Native SDK for BB10
- NDK 路径：`D:\bbndk`
- Java：必须使用 `D:\bbndk\jre\bin\java.exe`（系统 Java21 会导致打包崩溃）
- 构建输出目录：`D:\bbuild\douyinbar\`

### 目标设备
- 设备型号：BlackBerry SQC100-4
- 设备 IP：`169.254.0.1`
- 应用包名：`com.doubao.douyin.testDev_ubao_douyin54f5c08f`
- 部署密码：`0000`

## 编译构建

### 一键构建
```bash
# 进入项目目录
cd Douyin-BB10

# 执行一键构建（6 步流程 + 全门禁）
bash build.sh
```

构建流程：
1. 生成版本号头文件（数据源：`bar-descriptor.xml`）
2. 同步源码
3. 同步 BBTube 流式播放后端 `.so` 库
4. 修正 Makefile 中 moc 路径，强制重新生成 moc
5. 编译（`qcc -Vgcc_ntoarmv7le`）
6. 补齐 so 副本 + 打包 devMode BAR

### 构建产物
- `DouyinBar.bar` —— 6.25 MB，devMode BAR 安装包
- `libBiliClient.so.1.0.0` —— 播放后端库

### 版本号管理
版本号唯一数据源为 `bar-descriptor.xml` 中的 `<buildId>`：
```xml
<versionNumber>1.0.0</versionNumber>
<buildId>58</buildId>
```
每次迭代只需将 `buildId` +1，`build.sh` 会自动生成 `src/app_version.hpp`。

## 部署到 BB10 真机

### 部署脚本（示例）
```bash
JAVA="D:/bbndk/jre/bin/java.exe"
DEPLOYJAR="D:/bbndk/host_10_1_0_132/win32/x86/usr/lib/BarDeploy.jar"
BAR="DouyinBar.bar"
PKG="com.doubao.douyin.testDev_ubao_douyin54f5c08f"
DEV="169.254.0.1"

# 覆盖安装（不清沙盒）
"$JAVA" -Djava.awt.headless=true -Xmx512M -jar "$DEPLOYJAR" \
    -installApp -package "$BAR" -device "$DEV" -password 0000

# 启动应用
"$JAVA" -Djava.awt.headless=true -Xmx512M -jar "$DEPLOYJAR" \
    -launchApp -package-fullname "$PKG" -device "$DEV" -password 0000

# 查看已安装版本
"$JAVA" -Djava.awt.headless=true -Xmx512M -jar "$DEPLOYJAR" \
    -listInstalledApps -device "$DEV" -password 0000 | grep "$PKG"
```

### 拉取设备日志（取证）
```bash
"$JAVA" -Djava.awt.headless=true -Xmx512M -jar "$DEPLOYJAR" \
    -getFile "data/douyin_boot.log" "./douyin_boot.log" "$DEV" -password 0000 -package-fullname "$PKG"

"$JAVA" -Djava.awt.headless=true -Xmx512M -jar "$DEPLOYJAR" \
    -getFile "data/douyin_swipe.log" "./douyin_swipe.log" "$DEV" -password 0000 -package-fullname "$PKG"

"$JAVA" -Djava.awt.headless=true -Xmx512M -jar "$DEPLOYJAR" \
    -getFile "data/dyplay.log" "./dyplay.log" "$DEV" -password 0000 -package-fullname "$PKG"
```

## 门禁系统

构建前必须通过 `qmlcheck_full.py` 20 条 QML 规则检查（0 错 0 警），以及 `build.sh` 中的全门禁（GATE-DS/BAR 等）：

| GATE | 内容 | 通过标准 |
|------|------|----------|
| GATE-DS | 设计规范合规（色板/图标矩阵/启动图标/资产引用） | 资产 69/69 一致 |
| GATE2 | 播放代理线程生命周期（防 SIGSEGV / 重传风暴） | worker 不自毁 |
| GATE3 | NavigationPane pop 清理连接（防死代码） | 遍历子树 + dyNav 兜底 |
| GATE4 | 封面链路 / 方屏比例 / 首屏自动播放（防主页空白） | JPEG 选择 + 720x720 |
| GATE5 | 抖音式上滑分页（防手势收不到回归） | 顶层手势层 + Cancel 结算 |
| GATE9 | 预取器件自检（防预取回归） | 单飞/原子落地/收尸/buildId |

## 重要约定（铁律）

1. **打包 Java 必须用 `D:\bbndk\jre\bin\java.exe`**，系统 Java21 会崩溃
2. **`<id>` 与 so 入口名永不改**，改了等于换一个应用（沙盒、cookie、下载全部丢失）
3. **启动图标必须是 `<icon>icon.png</image></icon>` 结构**，裸文本会被静默忽略
4. **改头文件后必须运行 `build.sh`**，否则 `Q_PROPERTY` 按旧索引派发 → 崩溃
5. **`Q_INVOKABLE` 漏声明 = 静默故障**，QML 调用 C++ 方法不报错也不执行
6. **图标按钮必须用 `ImageButton`**，`Button` 默认 chrome 去不掉
7. **修改 `video_prefetch.cpp` 常量后，必须同步更新 `build.sh` GATE9 门禁字符串**

## 版本历史

| 版本 | 日期 | 主要变更 |
|------|------|----------|
| 1.0.0.45 | - | 播放修复 |
| 1.0.0.47 | - | 紧凑化 UI |
| 1.0.0.48 | - | 右栏六钮 ImageButton 44×44 + GATE4 门禁 |
| 1.0.0.53 | - | H.264 优先选型修复 |
| 1.0.0.54 | - | 设计规范 + 抖音图标，真机部署 |
| 1.0.0.55 | - | UI 代码审查闭环，循环 Button→ImageButton |
| 1.0.0.57 | - | 上滑翻页「滑不动」根因修复（gestureActive 去重） |
| 1.0.0.58 | 2026-11-10 | swipe() 触底 toast + 预取并发/收尸调优 |

## 下载已构建 BAR 包

已构建的 `DouyinBar_1.0.0.58.bar` 安装包已上传至 [GitHub Releases](https://github.com/lvdanseni/Douyin-BB10/releases)，可直接下载安装到 BB10 设备。

## 许可证

本项目采用 [MIT 许可证](LICENSE)。

---

> 更多开发细节、踩坑记录与修复经验请参考项目文档。如需继续迭代，请遵循门禁驱动开发流程：`修改源码 → qmlcheck_full 0/0 → build.sh 全门禁 → BarDeploy 部署 → 拉取设备日志 → 核验埋点 → 写验证报告`。
