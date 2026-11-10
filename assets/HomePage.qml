import bb.cascades 1.1

import "components"
// 首页推荐 —— 对应 BBTube HomePage(FEwhat_to_watch)
// Xiaohongshu 版：使用 noteModel 替代原 rcmdModel
Page {
    id: homePage

    // 本页所在的 NavigationPane，由 main.qml 注入（nav: %sNav 的形式）。
    // NotePage 拿它直接 push 详情页 —— 整条点击链路不做任何信号中转。
    property variant nav: null
    // 根节点（TabbedPane）引用，NotePage 用它调用 root.pushNote() 建页
    property variant host: null

    titleBar: TitleBar {
        title: "首页"
        acceptAction: ActionItem {
            title: "刷新"
            onTriggered: {
                noteModel.load();
            }
        }
    }
    content: NoteFeedPage {
        model: noteModel
        nav: homePage.nav
        host: homePage.host
    }

    onCreationCompleted: {
        noteModel.load();
    }
}
