import bb.cascades 1.1

import "components"
// 发现页 —— 对应 BBTube 的发现流
// Xiaohongshu 版：使用 noteModel (Kind=Discover)
Page {
    id: discoverPage

    property variant nav: null
    property variant host: null

    titleBar: TitleBar {
        title: "发现"
        acceptAction: ActionItem {
            title: "刷新"
            onTriggered: {
                noteModel.load();
            }
        }
    }
    content: NoteFeedPage {
        model: noteModel
        nav: discoverPage.nav
        host: discoverPage.host
    }

    onCreationCompleted: {
        noteModel.load();
    }
}
