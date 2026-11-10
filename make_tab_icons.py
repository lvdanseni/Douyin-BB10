# -*- coding: utf-8 -*-
"""
生成底部 Tab 用的线条图标。

【为什么要有这个脚本】
Cascades 的 Tab 不设 imageSource 时，10 个 Tab 会全部显示成系统默认的齿轮图标 ——
既难看又完全分不清。这里按每个板块的语义各画一个 96x96 的线条图标。

【画法】统一 4 倍超采样再缩回 96，得到抗锯齿的边缘；
颜色用浅灰 #D8DADC（BB10 深色 tab 栏上清楚，且不抢标题）。
"""
from PIL import Image, ImageDraw, ImageChops

S = 96      # 最终尺寸
K = 4       # 超采样倍数
C = (216, 218, 220, 255)     # #D8DADC
LW = 7 * K  # 线宽（最终 7px）


def canvas():
    return Image.new("RGBA", (S * K, S * K), (0, 0, 0, 0))


def finish(im, name):
    im = im.resize((S, S), Image.LANCZOS)
    im.save("assets/images/%s.png" % name)
    print("wrote", name)


def p(*pts):
    """把 96 坐标系里的点换算到超采样画布。"""
    return [(x * K, y * K) for (x, y) in pts]


def line(d, pts, w=LW):
    d.line(p(*pts), fill=C, width=w, joint="curve")


def ellipse(d, box, w=LW):
    x0, y0, x1, y1 = box
    d.ellipse([x0 * K, y0 * K, x1 * K, y1 * K], outline=C, width=w)


def rrect(d, box, r, w=LW):
    x0, y0, x1, y1 = box
    d.rounded_rectangle([x0 * K, y0 * K, x1 * K, y1 * K],
                        radius=r * K, outline=C, width=w)


def ring(outer, inner):
    """
    【为什么要这个】ImageDraw.polygon(outline=..., width=...) 是逐段画粗线拼出来的，
    密集采样时相邻线段在拐点处会留下台阶，缩到 96 之后就是一圈毛刺（心形踩过）。
    改成「实心外轮廓减去实心内轮廓」：填充是硬边，只有最后 LANCZOS 缩小那一次
    产生抗锯齿，边缘干净。
    """
    sz = S * K
    m1 = Image.new("L", (sz, sz), 0)
    ImageDraw.Draw(m1).polygon(p(*outer), fill=255)
    m2 = Image.new("L", (sz, sz), 0)
    ImageDraw.Draw(m2).polygon(p(*inner), fill=255)
    mask = ImageChops.subtract(m1, m2)
    im = Image.new("RGBA", (sz, sz), (0, 0, 0, 0))
    im.paste(Image.new("RGBA", (sz, sz), C), (0, 0), mask)
    return im


# ---------------------------------------------------------------- 推荐：房子
im = canvas(); d = ImageDraw.Draw(im)
line(d, [(14, 46), (48, 16), (82, 46)])          # 屋顶
line(d, [(22, 44), (22, 82)])                    # 左墙
line(d, [(74, 44), (74, 82)])                    # 右墙
line(d, [(22, 82), (74, 82)])                    # 地面
rrect(d, (38, 56, 58, 82), 2, LW)                # 门
finish(im, "tab_home")

# ---------------------------------------------------------------- 关注：心
# 【为什么不用「两个圆 + 一个倒三角」】那样拼出来的心形，两个圆和心尖之间
# 有缝，看起来像"两只眼睛加一个 V"。这里改用心的参数方程取一条闭合曲线：
#   x = 16sin³t,  y = 13cos t − 5cos2t − 2cos3t − cos4t
# 一条曲线描边，绝对连得上。
import math


def heart_at(scale, cy):
    """把参数方程采样成画布坐标；scale = 高度像素数，cy = 垂直中心。"""
    raw = []
    for i in range(240):
        t = i * 2.0 * math.pi / 240.0
        hx = 16.0 * math.sin(t) ** 3
        hy = 13.0 * math.cos(t) - 5.0 * math.cos(2 * t) \
             - 2.0 * math.cos(3 * t) - math.cos(4 * t)
        raw.append((hx, -hy))            # 画布 y 轴向下，取负翻转
    xs = [q[0] for q in raw]; ys = [q[1] for q in raw]
    w = max(xs) - min(xs); h = max(ys) - min(ys)
    k = scale / h
    cx = (max(xs) + min(xs)) / 2.0
    cy0 = (max(ys) + min(ys)) / 2.0
    return [(48 + (q[0] - cx) * k, cy + (q[1] - cy0) * k) for q in raw]


im = ring(heart_at(70.0, 50.0), heart_at(70.0 - 2 * 7, 50.0))
finish(im, "tab_follow")

# ---------------------------------------------------------------- 收藏：五角星
# 内半径从 15 提到 20：15 的时候描边内圈会缩成一个"星中星"，很乱。
im = canvas(); d = ImageDraw.Draw(im)
pts = []
for i in range(10):
    ang = -math.pi / 2 + i * math.pi / 5
    r = 36 if i % 2 == 0 else 20
    pts.append((48 + r * math.cos(ang), 48 + r * math.sin(ang)))
d.polygon(p(*pts), outline=C, width=LW)
finish(im, "tab_fav")

# ---------------------------------------------------------------- 历史：时钟
im = canvas(); d = ImageDraw.Draw(im)
ellipse(d, (14, 14, 82, 82))
line(d, [(48, 30), (48, 50), (64, 58)])
finish(im, "tab_history")

# ---------------------------------------------------------------- 已下载：箭头 + 托盘
im = canvas(); d = ImageDraw.Draw(im)
line(d, [(48, 14), (48, 58)])                    # 竖杆
line(d, [(30, 42), (48, 60), (66, 42)])          # 箭头
line(d, [(20, 74), (20, 84), (76, 84), (76, 74)])  # 托盘
finish(im, "tab_download")

# ---------------------------------------------------------------- 热门：火焰
im = canvas(); d = ImageDraw.Draw(im)
# 外焰：用折线拼一个水滴/火焰轮廓
line(d, [(48, 12), (64, 34), (70, 52), (64, 72), (48, 84), (32, 72), (26, 52), (34, 32), (48, 12)])
line(d, [(48, 50), (58, 66), (48, 78), (38, 66), (48, 50)])   # 内焰
finish(im, "tab_hot")

# ---------------------------------------------------------------- 排行：三根柱
im = canvas(); d = ImageDraw.Draw(im)
rrect(d, (16, 56, 32, 84), 3, LW)
rrect(d, (40, 36, 56, 84), 3, LW)
rrect(d, (64, 18, 80, 84), 3, LW)
finish(im, "tab_rank")

# ---------------------------------------------------------------- 分区：2x2 网格
im = canvas(); d = ImageDraw.Draw(im)
rrect(d, (16, 16, 44, 44), 5, LW)
rrect(d, (52, 16, 80, 44), 5, LW)
rrect(d, (16, 52, 44, 80), 5, LW)
rrect(d, (52, 52, 80, 80), 5, LW)
finish(im, "tab_region")

# ---------------------------------------------------------------- 搜索：放大镜
im = canvas(); d = ImageDraw.Draw(im)
ellipse(d, (14, 14, 62, 62))
line(d, [(58, 58), (82, 82)])
finish(im, "tab_search")

# ---------------------------------------------------------------- 设置：齿轮
# 外圈半径从 32 缩到 26、齿加长到 r=40，齿才看得出来 ——
# 原来齿只有 12 长且是圆头，缩到 tab 栏的小尺寸就是一个太阳。
im = canvas(); d = ImageDraw.Draw(im)
ellipse(d, (22, 22, 74, 74))                     # 外圈
ellipse(d, (38, 38, 58, 58))                     # 内孔
for i in range(8):                               # 8 个齿
    ang = i * math.pi / 4
    x0 = 48 + 24 * math.cos(ang)
    y0 = 48 + 24 * math.sin(ang)
    x1 = 48 + 40 * math.cos(ang)
    y1 = 48 + 40 * math.sin(ang)
    line(d, [(x0, y0), (x1, y1)], int(LW * 1.6))
finish(im, "tab_settings")

print("all done")
