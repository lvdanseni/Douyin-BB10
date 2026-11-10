import bb.cascades 1.1


import "components"
// 分区页：横向分区导航 + 分区内容列表（对应 BBTube 的频道导航/分区浏览）
Page {
    id: regionPage

    // 本页所在的 NavigationPane，由 main.qml 注入（nav: %sNav 的形式）。
    // FeedPage 拿它直接 push 详情页 —— 整条点击链路不做任何信号中转。
    property variant nav: null
    // 根节点（TabbedPane）引用，FeedPage 用它调用 root.pushVideo() 建页
    property variant host: null

    titleBar: TitleBar {
        title: "分区"
    }

    content: Container {
        layout: StackLayout {}

        // 分区选择条
        ScrollView {
            preferredHeight: ui.du(75)

            Container {
                id: regionRow
                layout: StackLayout {}
                leftPadding: ui.du(10)
                rightPadding: ui.du(10)
                topPadding: ui.du(5)
                bottomPadding: ui.du(5)
            }
        }

        Divider {
        }

        FeedPage {
            model: regionModel
            nav: regionPage.nav
        host: regionPage.host
        }
    }

    attachedObjects: [
        ComponentDefinition {
            id: chipDef
            source: "components/RegionChip.qml"
        }
    ]

    onCreationCompleted: {
        regionModel.load();
        var defs = [
            [1, "动画"], [3, "音乐"], [6, "游戏"], [36, "知识"], [188, "科技"],
            [181, "影视"], [160, "生活"], [119, "鬼畜"], [95, "数码"], [129, "舞蹈"], [211, "美食"]
        ];
        for (var i = 0; i < defs.length; i++) {
            var chip = chipDef.createObject();
            chip.rid = defs[i][0];
            chip.text = defs[i][1];
            regionRow.add(chip);
        }
    }
}
