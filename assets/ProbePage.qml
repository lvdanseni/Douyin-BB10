import bb.cascades 1.1


// 【诊断探针页】只用来验证 ComponentDefinition 的 source 路径能否解析。
// createObject() 成功返回非空 = 路径解析正常；
// 返回 null = 路径/机制问题（与 VideoPage.qml 的内容无关）。
// 导航修好后请删除本文件及 FeedPage.qml 里的 probeDef / probe()。
Page {
    titleBar: TitleBar {
        title: "probe"
    }
    content: Label {
        text: "probe ok"
    }
}
