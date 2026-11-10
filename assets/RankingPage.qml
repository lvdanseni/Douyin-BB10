import bb.cascades 1.1


import "components"
Page {
    id: rankingPage

    // 本页所在的 NavigationPane，由 main.qml 注入（nav: %sNav 的形式）。
    // FeedPage 拿它直接 push 详情页 —— 整条点击链路不做任何信号中转。
    property variant nav: null
    // 根节点（TabbedPane）引用，FeedPage 用它调用 root.pushVideo() 建页
    property variant host: null

    titleBar: TitleBar {
        title: "排行榜"
        acceptAction: ActionItem {
            title: "刷新"
            onTriggered: {
                rankingModel.load();
            }
        }
    }
    content: FeedPage {
        model: rankingModel
        nav: rankingPage.nav
        host: rankingPage.host
    }

    onCreationCompleted: {
        rankingModel.load();
    }
}
