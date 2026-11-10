# -*- coding: utf-8 -*-
# 【1.0.0.46】右侧操作列图标补全：与 play.png/pause.png/refresh.png 同风格
# 96x96 纯白透明底 PNG，4x 超采样抗锯齿（先画 384 再缩到 96）。
import math
from PIL import Image, ImageDraw

S = 96
OUT = 384          # 4x 超采样画布
SC = 4.0
W = 7 * 4          # 线宽等比放大
WHITE = (255, 255, 255, 255)

def new():
    return Image.new("RGBA", (OUT, OUT), (0, 0, 0, 0))

def save(im, name):
    im = im.resize((S, S), Image.LANCZOS)
    im.save(name)
    print("gen", name)

def star_pts(cx, cy, r_out, r_in, n=5):
    pts = []
    for i in range(n * 2):
        r = r_out if i % 2 == 0 else r_in
        a = -math.pi / 2 + i * math.pi / n
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts

# ---- 评论气泡：圆角矩形 + 小尾 ----
im = new(); d = ImageDraw.Draw(im)
d.rounded_rectangle([14*SC, 18*SC, 82*SC, 64*SC], radius=14*SC, outline=WHITE, width=W)
d.polygon([(34*SC, 62*SC), (34*SC, 80*SC), (52*SC, 64*SC)], fill=WHITE)
save(im, "ic_comment.png")

# ---- 星星：描边（未收藏）/ 实心（已收藏）----
pts = star_pts(48*SC, 50*SC, 34*SC, 14*SC)
im = new(); d = ImageDraw.Draw(im)
d.line(pts + [pts[0]], fill=WHITE, width=W, joint="curve")
save(im, "ic_star.png")

im = new(); d = ImageDraw.Draw(im)
d.polygon(star_pts(48*SC, 50*SC, 34*SC, 14*SC), fill=WHITE)
save(im, "ic_star_on.png")

# ---- 人形：描边（未关注）/ 实心（已关注）----
im = new(); d = ImageDraw.Draw(im)
d.ellipse([34*SC, 12*SC, 62*SC, 40*SC], outline=WHITE, width=W)
d.arc([16*SC, 46*SC, 80*SC, 110*SC], start=180, end=360, fill=WHITE, width=W)
save(im, "ic_person.png")

im = new(); d = ImageDraw.Draw(im)
d.ellipse([34*SC, 12*SC, 62*SC, 40*SC], fill=WHITE)
d.pieslice([16*SC, 46*SC, 80*SC, 110*SC], 180, 360, fill=WHITE)
save(im, "ic_person_on.png")

# ---- 文件夹：本地收藏夹 ----
im = new(); d = ImageDraw.Draw(im)
d.rounded_rectangle([14*SC, 26*SC, 82*SC, 76*SC], radius=8*SC, outline=WHITE, width=W)
d.rounded_rectangle([14*SC, 18*SC, 46*SC, 34*SC], radius=4*SC, fill=WHITE)
save(im, "ic_folder.png")

# ---- 下载：箭头 + 托盘 ----
im = new(); d = ImageDraw.Draw(im)
d.line([(48*SC, 14*SC), (48*SC, 58*SC)], fill=WHITE, width=W)
d.polygon([(28*SC, 46*SC), (68*SC, 46*SC), (48*SC, 74*SC)], fill=WHITE)
d.line([(20*SC, 80*SC), (76*SC, 80*SC)], fill=WHITE, width=W)
save(im, "ic_download.png")

# ---- 已下载：对勾 ----
im = new(); d = ImageDraw.Draw(im)
d.line([(22*SC, 52*SC), (42*SC, 72*SC)], fill=WHITE, width=W)
d.line([(42*SC, 72*SC), (74*SC, 26*SC)], fill=WHITE, width=W)
save(im, "ic_dl_ok.png")
print("done")
