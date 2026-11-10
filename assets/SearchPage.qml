import bb.cascades 1.1


import "components"
// 搜索页 —— 对应 BBTube SearchPage：搜索框 + 历史 + 结果列表
Page {
    id: searchPage

    // 本页所在的 NavigationPane，由 main.qml 注入（nav: %sNav 的形式）。
    // FeedPage 拿它直接 push 详情页 —— 整条点击链路不做任何信号中转。
    property variant nav: null
    // 根节点（TabbedPane）引用，FeedPage 用它调用 root.pushVideo() 建页
    property variant host: null

    titleBar: TitleBar {
        title: "搜索"
    }

    content: Container {
        layout: StackLayout {}
        topPadding: ui.du(15)
        leftPadding: ui.du(20)
        rightPadding: ui.du(20)

        Container {
                layout: StackLayout {}

                TextField {
                id: keywordField
                hintText: "输入关键词，如：黑神话"
                inputMode: TextFieldInputMode.Text
                input {
                    submitKey: SubmitKey.Search
                    onSubmitted: {
                        runSearch();
                    }
                }
                layoutProperties: StackLayoutProperties {}
            }
            Button {
                text: "搜索"
                onClicked: {
                    runSearch();
                }
                layoutProperties: StackLayoutProperties {}
            }
        }

        Divider {
        }

        FeedPage {
            model: searchModel
            nav: searchPage.nav
        host: searchPage.host
        }
    }

    function runSearch() {
        var q = keywordField.text.trim();
        if (q.length == 0) {
            return;
        }
        searchModel.setKeyword(q);
        searchModel.load();
    }
}
