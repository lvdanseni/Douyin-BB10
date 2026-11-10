import bb.cascades 1.1


import "components"
// 【v84】收藏 —— 收藏夹目录（顶部横向切换条）+ 夹内视频（大封面列表）。
//
// 数据链路：
//   favStore.loadFolders()  → /x/v3/fav/folder/created/list-all
//        ↓（目录到货后在 C++ 里自动 selectFolder(0)）
//   favStore.videoModel     → FeedModel(Kind=Fav) → /x/v3/fav/resource/list
//   FeedPage                → 复用首页那张大封面卡片
//
// 为什么"自动选中第一个收藏夹"放在 C++ 而不是这里：
// QML 里把 C++ 信号 connect 到 JS 函数在 BB10/Qt4.8 上会静默不连接（项目铁律 4），
// 靠 QML 做这个时序协调，最后会得到一个永远空着的列表。
Page {
    id: favPage

    // 本页所在的 NavigationPane，由 main.qml 注入（nav: %sNav 的形式）。
    property variant nav: null
    // 根节点（TabbedPane）引用，FeedPage 用它调用 root.pushVideo() 建页
    property variant host: null

    titleBar: TitleBar {
        title: favStore.currentTitle.length > 0 ? favStore.currentTitle : "收藏"
        acceptAction: ActionItem {
            title: "刷新"
            onTriggered: {
                favStore.loadFolders();
            }
        }
    }

    content: Container {
        layout: StackLayout {}

        // ---- 收藏夹切换条 ----
        //
        // 只放 6 个按钮，不是偷懒：Cascades 1.1 没有 Repeater，动态生成控件
        // （createObject）在本工程已经反复返回过 null。静态 6 个 + folderName(i)
        // 取值是唯一确定能跑通的写法。绝大多数人收藏夹不超过 6 个。
        //
        // 横向滚动交给外层 ScrollView 的 scrollMode，不用 ListView
        // （Cascades 的 ListView 只有竖向）。
        ScrollView {
            // 92 = Button 默认高度(~62) + 上下内边距各 10 + 余量。
            // 给少了按钮会被裁掉一半，看起来像"按钮坏了"。
            preferredHeight: ui.du(92)
            scrollViewProperties {
                scrollMode: ScrollMode.Horizontal
            }
            Container {
                layout: StackLayout {}
                topPadding: ui.du(10)
                bottomPadding: ui.du(10)
                leftPadding: ui.du(8)
                rightPadding: ui.du(8)

                Button {
                    text: favStore.count > 0 ? ((favStore.current === 0 ? "● " : "") + favStore.folderName(0)) : ""
                    visible: favStore.count > 0
                    onClicked: {
                        favStore.selectFolder(0);
                    }
                }
                Button {
                    text: favStore.count > 1 ? ((favStore.current === 1 ? "● " : "") + favStore.folderName(1)) : ""
                    visible: favStore.count > 1
                    onClicked: {
                        favStore.selectFolder(1);
                    }
                }
                Button {
                    text: favStore.count > 2 ? ((favStore.current === 2 ? "● " : "") + favStore.folderName(2)) : ""
                    visible: favStore.count > 2
                    onClicked: {
                        favStore.selectFolder(2);
                    }
                }
                Button {
                    text: favStore.count > 3 ? ((favStore.current === 3 ? "● " : "") + favStore.folderName(3)) : ""
                    visible: favStore.count > 3
                    onClicked: {
                        favStore.selectFolder(3);
                    }
                }
                Button {
                    text: favStore.count > 4 ? ((favStore.current === 4 ? "● " : "") + favStore.folderName(4)) : ""
                    visible: favStore.count > 4
                    onClicked: {
                        favStore.selectFolder(4);
                    }
                }
                Button {
                    text: favStore.count > 5 ? ((favStore.current === 5 ? "● " : "") + favStore.folderName(5)) : ""
                    visible: favStore.count > 5
                    onClicked: {
                        favStore.selectFolder(5);
                    }
                }
            }
        }

        // ---- 夹内视频 ----
        FeedPage {
            layoutProperties: StackLayoutProperties {}
            model: favStore.videoModel
            nav: favPage.nav
            host: favPage.host
            requireLogin: true
        }
    }

    // 同样不写 onCreationCompleted 去拉数据：
    // TabbedPane 启动时会把所有 Tab 页都建出来，那一刻通常还没登录。
    // 真正的加载时机是 main.qml 的 onActiveTabChanged → favStore.reload()。
}
