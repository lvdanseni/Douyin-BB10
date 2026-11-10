#!/usr/bin/env python3
"""右栏颜色分类扫描：把 x 600..720 按行切成 24px 段，分类 红/亮/灰/暗 像素数。
用法: colscan.py <raw> [x0 x1 y0 y1]"""
import sys, struct
import numpy as np

def load_raw(path):
    d = open(path, 'rb').read()
    w, h = struct.unpack('<4i', d[:16])[:2]
    stride = (len(d) - 16) // h
    rows = np.frombuffer(d[16:16 + h * stride], dtype=np.uint8).reshape(h, stride)
    return rows[:, :w * 4].reshape(h, w, 4)

def main():
    raw = sys.argv[1]
    x0, x1, y0, y1 = (int(v) for v in sys.argv[2:6]) if len(sys.argv) >= 6 else (600, 720, 0, 720)
    img = load_raw(raw)
    reg = img[y0:y1, x0:x1, :3].astype(np.int16)
    R, G, B = reg[..., 0], reg[..., 1], reg[..., 2]
    is_red   = (R > 90) & (R - G > 40) & (R - B > 40)
    is_white = (R > 190) & (G > 190) & (B > 190)
    is_gray  = (R > 60) & (R < 190) & (abs(R - G) < 30) & (abs(R - B) < 30)
    is_dark  = (R <= 60) & (G <= 60) & (B <= 60)
    print(f"[{raw}] x{x0}..{x1} y{y0}..{y1}  每8px段: red/white/gray/dark")
    for i in range(0, reg.shape[0], 8):
        seg = slice(i, min(i + 8, reg.shape[0]))
        r, w, g, d = is_red[seg].sum(), is_white[seg].sum(), is_gray[seg].sum(), is_dark[seg].sum()
        if r + w + g > 20:
            bar = ('R' if r > 20 else '.') + ('W' if w > 20 else '.') + ('G' if g > 60 else '.') + ('d' if d > 400 else '_')
            print(f"  y{y0+i:4d}-{y0+min(i+8,reg.shape[0]):4d}  red={r:4d} white={w:4d} gray={g:4d} dark={d:5d}  {bar}")

if __name__ == '__main__':
    main()
