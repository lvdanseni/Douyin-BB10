#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 BiliClient 图标（114x114 PNG，粉底圆角 + 白色块状 B），纯标准库实现"""
import struct, zlib, os

W = H = 114
PINK = (251, 114, 153, 255)
WHITE = (255, 255, 255, 255)
CLEAR = (0, 0, 0, 0)

# 5x7 块状 B 字模（1=白）
B_GLYPH = [
    "11110",
    "10001",
    "10001",
    "11110",
    "10001",
    "10001",
    "11110",
]
GLYPH_W, GLYPH_H = 5, 7
SCALE = 7  # 35x49

def rounded_rect_mask(px, py, w, h, radius):
    mask = [[False] * w for _ in range(h)]
    r2 = radius * radius
    for y in range(h):
        for x in range(w):
            dx = min(x, w - 1 - x, px + radius - 1 - x) if False else 0
            # corners
            cx = min(x, w - 1 - x)
            cy = min(y, h - 1 - y)
            if cx >= radius and cy >= radius:
                mask[y][x] = True
            elif cx < radius and cy < radius:
                ddx = radius - 1 - cx
                ddy = radius - 1 - cy
                mask[y][x] = (ddx * ddx + ddy * ddy) <= r2
            elif cx < radius:
                mask[y][x] = True
            else:
                mask[y][x] = True
    return mask

def make_pixels():
    # 画布
    px = [[CLEAR for _ in range(W)] for _ in range(H)]
    margin = 6
    bw, bh = W - 2 * margin, H - 2 * margin
    radius = 26
    mask = rounded_rect_mask(0, 0, bw, bh, radius)
    for y in range(bh):
        for x in range(bw):
            if mask[y][x]:
                px[margin + y][margin + x] = PINK
    # B 字
    gw, gh = GLYPH_W * SCALE, GLYPH_H * SCALE
    ox = (W - gw) // 2
    oy = (H - gh) // 2
    for gy in range(GLYPH_H):
        for gx in range(GLYPH_W):
            if B_GLYPH[gy][gx] == '1':
                for dy in range(SCALE):
                    for dx in range(SCALE):
                        px[oy + gy * SCALE + dy][ox + gx * SCALE + dx] = WHITE
    return px

def write_png(path, pixels):
    raw = bytearray()
    for y in range(H):
        raw.append(0)  # filter none
        for x in range(W):
            r, g, b, a = pixels[y][x]
            raw += bytes((r, g, b, a))
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        c += struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
        return c
    ihdr = struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", ihdr)
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)
    print("wrote", path, os.path.getsize(path), "bytes")

if __name__ == "__main__":
    write_png(r"E:\PCdesk\桌面\bilibili\BiliBar\icon.png", make_pixels())
