import bb.cascades 1.1

// 设置 / 账号页（抖音 Cookie 登录 + 运行诊断）
//
// 【为什么登录界面直接内联在这一页，而不是 push 一个 LoginPage】
// 本工程踩过太多次 createObject() 返回 null 的坑（ComponentDefinition 必须放在
// main.qml 根节点、source 只能写 assets 根下的平级文件名…）。登录界面只是一个
// 输入框 + 一个按钮 + 一行状态，内联在 ScrollView 里零跳转、零 createObject、
// 零失败点。
//
// 【为什么是 Cookie 登录而不是扫码】
// 抖音扫码接口实测 error_code 16（需完整签名），不可用；而
// /aweme/v1/user/profile/self/ 免签名可用，匿名/失效凭证稳定返回
// status_code=8 + user=null，有效凭证返回 status_code=0 + user。
// 因此改为粘贴 Cookie → C++ 发 self 校验 → 只有校验通过才落盘并显示登录态。
//
// 【context property 名叫 biliApi 是历史遗留】实际指向 DouyinApi 单例
// （applicationui.cpp 里 setContextProperty("biliApi", DouyinApi::instance())）。
// 全工程 QML 都按这个名字绑定，改名收益低、风险高，保持不动。
Page {
    id: settingsPage

    /**
     * 【1.0.0.33 修「设置/去登录点击无效」真根因】main.qml 的 openLogin() 在
     * createObject() 之后会执行 page.nav = dyNav；本页此前没有声明 nav 属性，
     * 赋值即抛 "Cannot assign to non-existent property \"nav\"" 的 TypeError。
     * 1.0.0.28 无 try/catch → 调用方埋点全被吞 → 真机表现"点了没反应、零日志"。
     * 与 DouyinCommentPage/DouyinHistoryPage 保持同一约定：注入属性必须先声明。
     * （本页当前不主动使用 nav，保留声明供未来页内返回/刷新直调。）
     */
    property variant nav: null

    titleBar: TitleBar {
        title: "设置"
    }

    content: ScrollView {
        scrollRole: ScrollRole.Main
        horizontalAlignment: HorizontalAlignment.Fill
        verticalAlignment: VerticalAlignment.Fill

        Container {
            background: theme.bg
            leftPadding: ui.du(20)
            rightPadding: ui.du(20)
            topPadding: ui.du(20)

            // ================= 账号 =================
            Label {
                text: "账号"
                textStyle.color: theme.accent
                textStyle.fontSize: FontSize.Medium
            }

            // ---- 已登录：头像 + 昵称 + UID + 退出 ----
            Container {
                visible: biliApi.loggedIn
                topMargin: ui.du(12)
                layout: StackLayout {}

                // 圆形头像：Cascades 没有 clip，靠「方图铺满 + 中心透明四周填底色的
                // 遮罩图叠在上层」模拟。
                Container {
                    preferredWidth: ui.du(48)
                    preferredHeight: ui.du(48)
                    verticalAlignment: VerticalAlignment.Center
                    layout: DockLayout {}
                    background: theme.thumbBg

                    ImageView {
                        horizontalAlignment: HorizontalAlignment.Fill
                        verticalAlignment: VerticalAlignment.Fill
                        imageSource: biliApi.userFace
                        scalingMethod: ScalingMethod.AspectFill
                    }
                    ImageView {
                        horizontalAlignment: HorizontalAlignment.Fill
                        verticalAlignment: VerticalAlignment.Fill
                        imageSource: "asset:///images/avatar_mask.png"
                    }
                }

                Container {
                    leftMargin: ui.du(12)
                    verticalAlignment: VerticalAlignment.Center
                    layoutProperties: StackLayoutProperties {
                        spaceQuota: 1
                    }

                    Label {
                        text: biliApi.userName.length > 0 ? biliApi.userName : "已登录"
                        textStyle.color: theme.textPrimary
                        textStyle.fontSize: FontSize.Large
                        textFormat: TextFormat.Plain
                    }
                    Label {
                        text: biliApi.userMid.length > 0 ? ("UID " + biliApi.userMid) : ""
                        visible: biliApi.userMid.length > 0
                        textStyle.color: theme.textSecondary
                        textStyle.fontSize: FontSize.Small
                    }
                }
            }

            Button {
                visible: biliApi.loggedIn
                text: "退出登录"
                topMargin: ui.du(12)
                horizontalAlignment: HorizontalAlignment.Left
                onClicked: {
                    biliApi.logout()
                }
            }

            // ---- 未登录：粘贴 Cookie ----
            Container {
                visible: !biliApi.loggedIn
                topMargin: ui.du(12)

                Label {
                    text: "登录后可同步账号信息。抖音扫码登录不可用，请粘贴 Cookie。"
                    textStyle.color: theme.textSecondary
                    multiline: true
                    textFormat: TextFormat.Plain
                }

                TextField {
                    id: cookieInput
                    hintText: "粘贴抖音 Cookie（完整串或裸 sessionid）"
                    horizontalAlignment: HorizontalAlignment.Fill
                    topMargin: ui.du(12)
                }

                Button {
                    text: "验证并登录"
                    topMargin: ui.du(8)
                    horizontalAlignment: HorizontalAlignment.Left
                    enabled: biliApi.loginState != 1
                    onClicked: {
                        biliApi.loginWithCookie(cookieInput.text)
                    }
                }

                ActivityIndicator {
                    horizontalAlignment: HorizontalAlignment.Left
                    topMargin: ui.du(8)
                    running: biliApi.loginState == 1
                    visible: biliApi.loginState == 1
                }

                Label {
                    // loginTip 由 C++ 按校验结果给出（校验中 / 已失效 / 登录成功 / 网络失败）
                    text: biliApi.loginTip.length > 0
                          ? biliApi.loginTip
                          : "在浏览器登录 douyin.com，从开发者工具复制 Cookie 后粘贴到上方。也可只粘贴 sessionid 的值。"
                    textStyle.color: theme.textSecondary
                    multiline: true
                    textFormat: TextFormat.Plain
                    topMargin: ui.du(8)
                }

                Label {
                    text: "校验通道：/aweme/v1/user/profile/self/（status_code=0 且 user 非空才算登录成功）"
                    textStyle.color: theme.textSecondary
                    textStyle.fontSize: FontSize.XSmall
                    multiline: true
                    textFormat: TextFormat.Plain
                    topMargin: ui.du(8)
                }
            }

            Divider {
            }

            // ================= 关于 =================
            Label {
                text: "Douyin " + app.version
                textStyle.color: theme.textPrimary
                textStyle.fontSize: FontSize.Large
                textStyle.fontWeight: FontWeight.Normal
            }
            Label {
                text: "抖音原生客户端（BlackBerry 10 / Cascades）"
                textStyle.color: theme.textSecondary
                textStyle.fontSize: FontSize.Small
            }
            Label {
                // 业务请求计数 / 封面下载成功失败计数，排查「刷不出视频」时最有用。
                text: "网络 " + biliApi.bizDiag()
                textStyle.color: theme.textSecondary
                textStyle.fontSize: FontSize.XSmall
                multiline: true
                textFormat: TextFormat.Plain
                topMargin: ui.du(4)
            }

            Divider {
            }

            Label {
                text: "架构"
                textStyle.color: theme.accent
                textStyle.fontSize: FontSize.Medium
                topMargin: ui.du(12)
            }
            Label {
                text: "· UI 层：Cascades QML（TabbedPane + 单条 ListView 上滑翻页）"
                textStyle.color: theme.textSecondary
                multiline: true
            }
            Label {
                text: "· 数据层：C++ DouyinApi（libcurl + OpenSSL）+ DouyinFeedModel 分页 Store"
                textStyle.color: theme.textSecondary
                multiline: true
            }
            Label {
                text: "· 协议：aweme.snssdk.com /aweme/v1/feed/ 返回 JSON，播放走行内 play_addr.url_list"
                textStyle.color: theme.textSecondary
                multiline: true
            }
            Label {
                text: "· 播放层：VideoProxy 本地代理 + VideoPlayer 播放 CDN MP4 直链"
                textStyle.color: theme.textSecondary
                multiline: true
            }

            Divider {
            }

            Label {
                text: "说明"
                textStyle.color: theme.accent
                textStyle.fontSize: FontSize.Medium
                topMargin: ui.du(12)
            }
            Label {
                text: "· 匿名也能刷推荐流；登录只用于读取账号信息\n· Cookie 只保存在本机沙盒 data/douyin_cookie.txt，不会外传\n· 仅供学习研究，请遵守抖音服务条款"
                textStyle.color: theme.textSecondary
                multiline: true
                textFormat: TextFormat.Plain
            }

            Divider {
            }

            Label {
                text: "运行诊断"
                textStyle.color: theme.accent
                textStyle.fontSize: FontSize.Medium
                topMargin: ui.du(12)
            }
            Button {
                text: "播放自检（约 14 秒）"
                horizontalAlignment: HorizontalAlignment.Left
                onClicked: {
                    var r = app.playbackSelfTest();
                    diagLabel.text = "播放自检：" + r
                        + "\n后台跑真实链路：feed 行 → CDN 直链 → 本地代理 → libbbplayer"
                        + "\n约 14 秒后再点「刷新诊断信息」看 PASS / FAIL 结论。";
                }
            }
            Button {
                text: "刷新诊断信息"
                horizontalAlignment: HorizontalAlignment.Left
                topMargin: ui.du(8)
                onClicked: {
                    diagLabel.text = "网络/模型：\n" + app.diagText()
                        + "\n\n登录：" + (biliApi.loggedIn ? "已登录 " + biliApi.userName : "未登录")
                        + " cookie=" + biliApi.cookie().length + "B state=" + biliApi.loginState
                        + "\n\nCookie 通道：" + biliApi.cookieDiag()
                        + "\n\n解析器自检：" + biliApi.selfParserCheck()
                        + "\n\n播放自检：\n" + app.playbackSelfTestLog()
                        + "\n\n代理环境：\n" + app.proxyEnvDiag()
                        + "\n\n播放层：\n" + app.playerLogText()
                        + "\n\n启动日志：\n" + app.bootLogText()
                        + "\n\nQML 报错：\n" + app.qmlLogText()
                }
            }
            Label {
                id: diagLabel
                text: "点上方按钮获取。若页面一直转圈，把这里的内容反馈即可定位问题。"
                textStyle.color: theme.textSecondary
                textStyle.fontSize: FontSize.XSmall
                multiline: true
                textFormat: TextFormat.Plain
                topMargin: ui.du(8)
            }

            bottomPadding: ui.du(20)
        }
    }
}
