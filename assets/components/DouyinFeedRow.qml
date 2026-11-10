import bb.cascades 1.1

// 抖音风格视频流卡片 —— 列表行。
//
// 【1.0.0.17 起职责收窄为"滑动载体 + 封面海报"】
//   首页改成内嵌播放：ForeignWindowControl 作为固定层盖在卡片中央（见
//   DouyinFeedPage.qml），上滑落定直接换流播放，不再点进播放页。
//   因此作者/标题/点赞数/操作按钮全部上移到页面的固定层（绑定 dyPlayerStore，
//   反映"当前正在播的行"），本行只保留封面图 —— 视频未起播前它是海报，
//   起播后被原生视频窗口盖住（视频区外仍是本卡的黑色底）。
//
// 【作用域约束（老坑，别踩）】
// ListItemComponent 是独立的组件作用域，取不到文档级 id，也取不到全局 ui 对象。
// 尺寸直接用数字，颜色用 Color.create(...)，context property 由 C++ 塞进
// ListItemData.api。
Container {
    id: dyRow

    property string picUrl: ""
    /** DouyinApi 指针，由 C++ 塞进行数据（ListItemComponent 看不到 context property）。 */
    property variant api: null
    /**
     * 【1.0.0.22】真实视频宽高比（douyin_store 从 video.width/height 算好写进行）。
     * 封面必须和内嵌 FWC 视频窗口用同一个比例算同一个居中矩形，否则视频盖不住
     * 封面边缘 → "视频叠在封面上"的错位（真机截图实证）。
     */
    property double videoAspect: (ListItemData.videoAspect > 0) ? ListItemData.videoAspect : 0.5625
    // letterbox 公式与 DouyinFeedPage.videoW/H 完全一致：竖屏 405x720 居中、
    // 横屏 720x405，保证封面矩形 == 视频窗口矩形。
    property double coverH: Math.min(720 / dyRow.videoAspect, 720)
    property double coverW: Math.min(dyRow.coverH * dyRow.videoAspect, 720)

    // 自定义 list item 必须给确定宽度，否则会缩到内容宽（铁律 3）。
    preferredWidth: 720
    // 【1.0.0.15】Q20 (SQC100) 是 1:1 方屏 720x720 —— 这里必须等于屏幕高。
    // 写 1280（Z10 抄来）会让 FlickMode.SingleItem 对齐后看到的永远是卡片中段。
    preferredHeight: 720

    background: Color.create("#000000")

    layout: DockLayout {}

    // ---- 封面：与内嵌 FWC 视频窗口同一矩形（按真实视频比例 letterbox）----
    // 【1.0.0.22 修复"视频叠在封面上/边缘露封面"】旧写法封面 AspectFit 全屏按图
    // 自身比例显示，与视频窗口 letterbox 用的比例不一致 → 两者矩形错位，视频盖不住
    // 封面边缘。现在封面与 FWC 用同一 videoAspect 算同一个居中矩形，封面用
    // AspectFill 裁填满该矩形；视频起播后无缝盖住封面，边界严格对齐。
    Container {
        horizontalAlignment: HorizontalAlignment.Center
        verticalAlignment: VerticalAlignment.Center
        preferredWidth: dyRow.coverW
        preferredHeight: dyRow.coverH
        background: Color.create("#000000")    // 视频窗口外的左右/上下黑边底色
        layout: DockLayout {}

        ImageView {
            id: cover
            horizontalAlignment: HorizontalAlignment.Fill
            verticalAlignment: VerticalAlignment.Fill
            // ⚠️ 必须是单表达式，不能写成块（铁律 5）。第二参 coverVersion 是
            // 依赖锚点：封面下完 coverVersion++ 绑定才重算。
            imageSource: dyRow.picSource(dyRow.picUrl,
                                         dyRow.api ? dyRow.api.coverVersion : -1)
            // AspectFill：裁掉多出部分，把封面铺满与视频窗口一致的矩形。
            scalingMethod: ScalingMethod.AspectFill
        }
    }

    // ---------------- 函数 ----------------

    /**
     * 把 picUrl 解析成 ImageView 能用的地址。
     * 独立成函数而不是内联块：Qt 4.8 的 QML 里 `prop: { ... }` 会被当成 JS
     * 对象字面量（铁律 5），块绑定直接失效。
     */
    function picSource(url, ver) {
        if (!url || url.length === 0) {
            return "";
        }
        if (url.indexOf("file://") === 0) {
            return url;
        }
        if (api) {
            return api.coverUrlFor(url, ver);
        }
        return "";
    }
}
