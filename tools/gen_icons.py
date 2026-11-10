#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""生成 DouyinBar 播放控制图标（纯白 + 透明底 PNG，Cascades ImageView/Button 可用）。

背景：Cascades 1.1 无 Shape/SVG，QML 画不了矢量图标；BB10 上 Unicode 字形
（U+25B6 / U+23F8）经 Button.text 渲染时常变成豆腐块/字形丑陋（用户 1.0.0.28
原话"播放/暂停按钮的图标太丑了"）。这里用 PIL 以 4x 超采样绘制后 LANCZOS
缩放到 96x96，边缘抗锯齿，任何缩放下都不糊。

产物（均 96x96 RGBA，纯白 255,255,255，透明底）：
  play.png    右向圆角三角
  pause.png   两根圆角竖条
  refresh.png 圆环 + 箭头（顺时针回旋）
"""
import os
from PIL import Image, ImageDraw

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "images")
OUT = os.path.normpath(OUT)
S = 384          # 4x 超采样画布
FINAL = 96       # 交付尺寸
WHITE = (255, 255, 255, 255)


def canvas():
    return Image.new("RGBA", (S, S), (0, 0, 0, 0))


def finish(im, name):
    im = im.resize((FINAL, FINAL), Image.LANCZOS)
    path = os.path.join(OUT, name)
    im.save(path, "PNG", optimize=True)
    # 自检：必须纯白+透明，不透明像素要有足够面积（防止画出空图标）
    px = list(im.getdata())
    opaque = [p for p in px if p[3] > 128]
    bad = [p for p in opaque if p[0] != 255 or p[1] != 255 or p[2] != 255]
    assert not bad, "%s: %d non-white opaque px" % (name, len(bad))
    assert 200 < len(opaque) < S * S, "%s: opaque=%d" % (name, len(opaque))
    print("%s ok opaque=%d path=%s" % (name, len(opaque), path))


def rounded_poly(draw, pts, r, fill):
    """多边形 + 顶点圆角（近似：用大半径 line joint=curve 不够，直接画粗线）。"""
    draw.line(pts + [pts[0]], fill=fill, width=r, joint="curve")


def gen_play():
    im = canvas()
    d = ImageDraw.Draw(im)
    # 圆角三角：顶点在右，底边在左。视觉重心居中（右向播放键）。
    cx, cy = S * 0.40, S * 0.50
    w, h = S * 0.42, S * 0.52
    tip = (cx + w, cy)                 # 右尖角
    tl = (cx - w * 0.60, cy - h / 2)   # 左上
    bl = (cx - w * 0.60, cy + h / 2)   # 左下
    d.polygon([tip, tl, bl], fill=WHITE)
    # 三个角用小圆抠圆角：画底色圆会破坏形状，改用大 width 的 line joint=curve
    # 得到硬圆角即可（QPaint 风格）。简单可靠：直接画 3 次 1px 描边不够，
    # 用 PIL 的 rounded_rectangle 不行——改用 polygon + 半径 mask 方案：
    im2 = canvas()
    d2 = ImageDraw.Draw(im2)
    d2.polygon([tip, tl, bl], fill=WHITE)
    r = int(S * 0.055)
    d2.ellipse([tip[0] - r, tip[1] - r, tip[0] + r, tip[1] + r], fill=WHITE)
    d2.ellipse([tl[0] - r, tl[1] - r, tl[0] + r, tl[1] + r], fill=WHITE)
    d2.ellipse([bl[0] - r, bl[1] - r, bl[0] + r, bl[1] + r], fill=WHITE)
    finish(im2, "play.png")


def gen_pause():
    im = canvas()
    d = ImageDraw.Draw(im)
    bw = S * 0.16          # 条宽
    bh = S * 0.52          # 条高
    gap = S * 0.075
    cy = S * 0.5
    x0 = S * 0.5 - gap / 2 - bw
    x1 = S * 0.5 + gap / 2
    d.rounded_rectangle([x0, cy - bh / 2, x0 + bw, cy + bh / 2],
                        radius=bw * 0.32, fill=WHITE)
    d.rounded_rectangle([x1, cy - bh / 2, x1 + bw, cy + bh / 2],
                        radius=bw * 0.32, fill=WHITE)
    finish(im, "pause.png")


def gen_refresh():
    im = canvas()
    d = ImageDraw.Draw(im)
    cx, cy = S * 0.5, S * 0.5
    R = S * 0.34           # 圆环半径
    w = int(S * 0.085)     # 线宽
    # 画 300° 圆弧（留出箭头缺口，从 45° 到 315° 逆时针方向的缺口在右上）
    box = [cx - R, cy - R, cx + R, cy + R]
    d.arc(box, start=-60, end=270, fill=WHITE, width=w)
    # 箭头：缺口末端（270° 即正上方）画一个实心三角指向运动方向（顺时针→向右）
    import math
    # 弧终点角度 270°（PIL 角度：0=3 点方向，顺时针为正）
    ang = math.radians(270)
    ex, ey = cx + R * math.cos(ang), cy + R * math.sin(ang)
    tang = ang + math.pi / 2      # 运动切向（顺时针）
    L = S * 0.20                  # 箭头尺寸
    # 三角：顶点在弧末端外侧，两个后角沿切向展开
    tipx = ex + L * 0.55 * math.cos(tang)
    tipy = ey + L * 0.55 * math.sin(tang)
    b1x = ex - L * 0.45 * math.cos(tang) + L * 0.45 * math.cos(tang + math.pi / 2)
    b1y = ey - L * 0.45 * math.sin(tang) + L * 0.45 * math.sin(tang + math.pi / 2)
    b2x = ex - L * 0.45 * math.cos(tang) + L * 0.45 * math.cos(tang - math.pi / 2)
    b2y = ey - L * 0.45 * math.sin(tang) + L * 0.45 * math.sin(tang - math.pi / 2)
    d.polygon([(tipx, tipy), (b1x, b1y), (b2x, b2y)], fill=WHITE)
    finish(im, "refresh.png")


if __name__ == "__main__":
    if not os.path.isdir(OUT):
        os.makedirs(OUT)
    gen_play()
    gen_pause()
    gen_refresh()
    print("ALL ICONS OK")
