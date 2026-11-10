import bb.cascades 1.1

// 评论页（1.0.0.23 新增）。
//
// 【入口】DouyinFeedPage 右侧操作列「评论」按钮 → main.qml 的
//   pushComments(awemeId)：createObject() → push → **再赋 awemeId**
//   （与 pushDyVideo 同款顺序：赋值那一刻页面必须已在导航栈里）。
//   赋 awemeId 触发 onAwemeIdChanged → dyCommentModel.load(id)。
//
// 【数据通道】www.iesdouyin.com/web/api/v2/comment/list/（curl 实证免签可读），
//   cursor 被服务端忽略 → 客户端 cid 去重；拉不到新评论即 hasMore=false，
//   底部显示「已加载全部可获取的评论」（诚实降级，不做假分页）。
//
// 【作用域约束（老坑，别踩）】ListItemComponent 看不到 context property
//   （biliApi / ui / theme 在这里全是 ReferenceError，BiliBar v92 实证）。
//   头像走 C++ 塞进行里的 api 指针：ListItemData.api.faceUrlFor(...)；
//   时间格式化同理用 ListItemData.api.timeAgo(...)。颜色/尺寸全部写死。
//
// 【铁律】不要写 type: "item"；自定义 list item 必须给 preferredWidth；
//   绑定不能写成块 { ... }；Label 没有 maxLines，多行用 multiline。
Page {
    id: commentPage

    /** 上一级 NavigationPane（main.qml push 时注入）。 */
    property variant nav: null
    /** 视频 id。push 之后由 main.qml 赋值；赋值即触发加载。 */
    property string awemeId: ""

    /** 【1.0.0.29】发表结果提示的影子属性（NOTIFY 绑定 → 变化即回调）。 */
    property string cmtPubTip: dyCommentModel ? dyCommentModel.publishTip : ""
    onCmtPubTipChanged: {
        // 发表成功即清空输入框（失败保留文本，用户可改后重试）。
        if (commentPage.cmtPubTip === "发表成功") {
            cmtInput.text = "";
        }
    }

    onAwemeIdChanged: {
        if (commentPage.awemeId.length > 0) {
            if (dyCommentModel && dyCommentModel.load) {
                dyCommentModel.load(commentPage.awemeId);
            }
        }
    }

    /** 【1.0.0.29】键盘 Send 键 = 发表（与底部按钮同路，单表达式绑定）。 */
    function publishInput() {
        if (dyCommentModel && dyCommentModel.publish) {
            dyCommentModel.publish(cmtInput.text);
        }
    }

    // DockLayout：顶部条吸顶、列表填充、转圈/空态/错误居中（与 DouyinFeedPage 同款布局）。
    content: Container {
        id: cmtRoot
        horizontalAlignment: HorizontalAlignment.Fill
        verticalAlignment: VerticalAlignment.Fill
        background: theme.bg
        layout: DockLayout {}

        // ===== 评论列表（先声明，z 序在顶栏之下） =====
        ListView {
            id: cmtList
            objectName: "dyCommentList"
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Fill
            topMargin: 84
            // 【1.0.0.29】底部发表条约 88px 高：留出等量 margin，
            // 最后一条评论不会被输入框盖住。
            bottomMargin: 96
            dataModel: dyCommentModel

            listItemComponents: [
                ListItemComponent {
                    Container {
                        // 自定义 list item 必须给确定宽度（铁律 3）。
                        preferredWidth: 720
                        horizontalAlignment: HorizontalAlignment.Fill
                        background: Color.create("#101014")
                        topPadding: 12
                        bottomPadding: 12
                        leftPadding: 16
                        rightPadding: 16
                        layout: StackLayout {
                            orientation: LayoutOrientation.LeftToRight
                        }

                        // 头像：远端 URL 永远在行里，本地路径由 api 现场解析。
                        ImageView {
                            preferredWidth: 44
                            preferredHeight: 44
                            verticalAlignment: VerticalAlignment.Top
                            scalingMethod: ScalingMethod.AspectFill
                            imageSource: (ListItemData.api && ListItemData.face && ListItemData.face.length > 0)
                                ? ListItemData.api.faceUrlFor(ListItemData.face, ListItemData.api.coverVersion)
                                : "asset:///images/avatar_mask.png"
                        }

                        Container {
                            layoutProperties: StackLayoutProperties {
                                spaceQuota: 1
                            }
                            leftMargin: 12
                            layout: StackLayout {}

                            Label {
                                text: ListItemData.nick ? ListItemData.nick : ""
                                textStyle.color: Color.create("#8E9095")
                                textStyle.fontSize: FontSize.XSmall
                            }

                            Label {
                                text: ListItemData.text ? ListItemData.text : ""
                                textStyle.color: Color.create("#F2F3F5")
                                textStyle.fontSize: FontSize.Small
                                multiline: true
                                topMargin: 4
                            }

                            Label {
                                // 元信息：时间 + IP 归属 + 赞 + 回复数。
                                text: (ListItemData.api && ListItemData.createTime > 0
                                            ? ListItemData.api.timeAgo(ListItemData.createTime)
                                            : "")
                                      + (ListItemData.ipLabel && ListItemData.ipLabel.length > 0
                                            ? (" · " + ListItemData.ipLabel) : "")
                                      + (ListItemData.digg > 0 ? (" · " + ListItemData.digg + "赞") : "")
                                      + (ListItemData.replyTotal > 0
                                            ? (" · " + ListItemData.replyTotal + "回复") : "")
                                textStyle.color: Color.create("#8E9095")
                                textStyle.fontSize: FontSize.XSmall
                                topMargin: 8
                            }
                        }
                    }
                }
            ]

            attachedObjects: [
                ListScrollStateHandler {
                    id: scrollState
                    onAtEndChanged: {
                        if (atEnd && dyCommentModel && dyCommentModel.loadMore) {
                            dyCommentModel.loadMore();
                        }
                    }
                }
            ]
        }

        // ===== 顶部条：返回 + 标题（评论数） =====
        Container {
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Top
            preferredHeight: 84
            background: theme.surfaceSemi
            leftPadding: 16
            rightPadding: 16
            layout: StackLayout {
                orientation: LayoutOrientation.LeftToRight
            }

            Button {
                text: "< 返回"
                horizontalAlignment: HorizontalAlignment.Left
                verticalAlignment: VerticalAlignment.Center
                preferredWidth: 130
                preferredHeight: 56
                onClicked: {
                    if (commentPage.nav && commentPage.nav.pop) {
                        commentPage.nav.pop();
                    }
                }
            }

            Label {
                text: dyCommentModel.count > 0
                      ? ("评论 " + dyCommentModel.count)
                      : "评论"
                horizontalAlignment: HorizontalAlignment.Fill
                verticalAlignment: VerticalAlignment.Center
                textStyle.color: theme.textPrimary
                textStyle.fontSize: FontSize.Medium
                textStyle.fontWeight: FontSize.Bold
            }
        }

        // ===== 底部状态：翻页转圈 / 到底提示 =====
        // 【1.0.0.29】bottomMargin 96：让位于底部发表条（约 88px 高）。
        ActivityIndicator {
            horizontalAlignment: HorizontalAlignment.Center
            verticalAlignment: VerticalAlignment.Bottom
            preferredWidth: 80
            preferredHeight: 80
            bottomMargin: 96
            running: dyCommentModel.loadingMore
            visible: dyCommentModel.loadingMore
        }

        Label {
            id: endLabel
            horizontalAlignment: HorizontalAlignment.Center
            verticalAlignment: VerticalAlignment.Bottom
            bottomMargin: 112
            visible: !dyCommentModel.hasMore && dyCommentModel.count > 0 && !dyCommentModel.loadingMore
            text: "— 已加载全部可获取的评论 —"
            textStyle.color: theme.textMuted
            textStyle.fontSize: FontSize.XSmall
        }

        // ===== 首屏加载转圈 =====
        ActivityIndicator {
            horizontalAlignment: HorizontalAlignment.Center
            verticalAlignment: VerticalAlignment.Center
            preferredWidth: 120
            preferredHeight: 120
            running: dyCommentModel.loading
            visible: dyCommentModel.loading
        }

        // ===== 空态 / 错误 =====
        Container {
            id: msgBox
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Center
            visible: (dyCommentModel.error.length > 0)
                     || (!dyCommentModel.loading && dyCommentModel.count == 0
                         && dyCommentModel.error.length == 0 && dyCommentModel.awemeId.length > 0)
            leftPadding: 60
            rightPadding: 60
            topPadding: 40
            bottomPadding: 40
            layout: StackLayout {}

            Label {
                text: dyCommentModel.error.length > 0 ? dyCommentModel.error
                                                      : "这条视频还没有评论"
                horizontalAlignment: HorizontalAlignment.Center
                textStyle.color: theme.textOnVideo
                textStyle.fontSize: FontSize.Small
                multiline: true
                bottomMargin: 16
            }

            Button {
                text: "重试"
                horizontalAlignment: HorizontalAlignment.Center
                visible: dyCommentModel.error.length > 0
                onClicked: {
                    if (commentPage.awemeId.length > 0 && dyCommentModel.load) {
                        dyCommentModel.load(commentPage.awemeId);
                    }
                }
            }
        }

        // ===== 【1.0.0.29】底部发表评论条（TextField + 发表按钮）=====
        // DockLayout 吸底。策略与发表链路完全走 C++（douyin_store.cpp 的
        // DouyinCommentModel::publish）：POST /aweme/v1/comment/publish/
        // JSON {aweme_id,text}，结果进 publishTip（3s 自消），成功后自动
        // 重载评论列表（服务端 cursor 被忽略，重拉即含新评论的前 10 条）。
        // 键盘回车（SubmitKey.Send）与按钮同一条发布路径。
        Container {
            id: pubBar
            objectName: "pubBar"
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Bottom
            background: theme.surfaceSemi
            leftPadding: 12
            rightPadding: 12
            topPadding: 6
            bottomPadding: 8
            layout: StackLayout {}

            Container {
                horizontalAlignment: HorizontalAlignment.Fill
                preferredHeight: 72
                layout: StackLayout {
                    orientation: LayoutOrientation.LeftToRight
                }

                TextField {
                    id: cmtInput
                    objectName: "cmtInput"
                    horizontalAlignment: HorizontalAlignment.Fill
                    verticalAlignment: VerticalAlignment.Center
                    preferredHeight: 64
                    // Chat 输入模式 + Send 提交键（回车即发表，与按钮同路）。
                    inputMode: TextFieldInputMode.Chat
                    hintText: "说点什么…"
                    backgroundVisible: true
                    clearButtonVisible: true
                    textStyle.color: theme.textPrimary
                    textStyle.fontSize: FontSize.Small
                    layoutProperties: StackLayoutProperties {
                        spaceQuota: 1
                    }
                    input.submitKey: SubmitKey.Send
                    // 单表达式信号处理器（铁律 5：不得写块）。
                    // 回车与「发表」按钮同一条发布路径；设备侧 QML 日志可验。
                    input.onSubmitted: commentPage.publishInput()
                }

                Button {
                    id: pubBtn
                    horizontalAlignment: HorizontalAlignment.Right
                    verticalAlignment: VerticalAlignment.Center
                    preferredWidth: 116
                    preferredHeight: 64
                    leftMargin: 8
                    text: (dyCommentModel && dyCommentModel.publishing) ? "发布中" : "发表"
                    // 单飞：发布中禁按钮，防重复提交。
                    enabled: !(dyCommentModel && dyCommentModel.publishing)
                    onClicked: {
                        if (dyCommentModel && dyCommentModel.publish) {
                            dyCommentModel.publish(cmtInput.text);
                        }
                    }
                }
            }
        }

        // ===== 【1.0.0.37】发表结果居中浮动提示（toast）=====
        // 旧版 pubTip 贴在底部发表条上方：键盘弹起时被顶走/挡住，用户根本
        // 看不到那句"发表失败"——原反馈"发表失败位置错误"的工程实质。
        // 改成抖音式居中 toast：publishTip 非空即显示（3s 自消由 C++ 定时器），
        // 富文本全量展示（空 200 / 未登录都有一句可读结论，见 douyin_store.cpp）。
        // DockLayout 里声明在最后 = z 序最高，盖在列表与发表条之上。
        Container {
            id: pubToast
            objectName: "pubToast"
            horizontalAlignment: HorizontalAlignment.Center
            verticalAlignment: VerticalAlignment.Center
            visible: (dyCommentModel && dyCommentModel.publishTip)
                     ? dyCommentModel.publishTip.length > 0 : false
            // 【1.0.0.39】toast 显隐的机读留痕在 C++ setPublishTip（douyin_store.cpp
            // 落 "cmt PUB-TIP"）—— Cascades 1.1 无 onVisibleChanged，QML 侧不可留痕。
            background: theme.toastBg
            leftPadding: 20
            rightPadding: 20
            topPadding: 12
            bottomPadding: 12
            preferredWidth: 560
            layout: DockLayout {}

            Label {
                horizontalAlignment: HorizontalAlignment.Fill
                verticalAlignment: VerticalAlignment.Center
                text: (dyCommentModel && dyCommentModel.publishTip)
                      ? dyCommentModel.publishTip : ""
                textStyle.color: theme.textPrimary
                textStyle.fontSize: FontSize.Small
                multiline: true
            }
        }
    }
}
