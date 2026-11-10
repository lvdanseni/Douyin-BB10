#!/usr/bin/env python3
"""右栏连通域分析：x600..720 非暗像素的 4-连通簇，输出 bbox/中心/主色/红占比。
用法: colcc.py <raw> [x0 x1]"""
import sys, struct
import numpy as np

def load_raw(path):
    d = open(path, 'rb').read()
    w, h = struct.unpack('<4i', d[:16])[:2]
    stride = (len(d) - 16) // h
    rows = np.frombuffer(d[16:16 + h * stride], dtype=np.uint8).reshape(h, stride)
    return rows[:, :w * 4].reshape(h, w, 4)

def components(mask, min_px=80):
    """4-连通域 BFS，返回 [(x0,y0,x1,y1,npx,mean_rgb)]"""
    h, w = mask.shape
    seen = np.zeros_like(mask, dtype=bool)
    out = []
    for y in range(h):
        for x in range(w):
            if mask[y, x] and not seen[y, x]:
                stack = [(y, x)]
                seen[y, x] = True
                pts = []
                while stack:
                    cy, cx = stack.pop()
                    pts.append((cy, cx))
                    for dy, dx in ((1,0),(-1,0),(0,1),(0,-1)):
                        ny, nx = cy+dy, cx+dx
                        if 0 <= ny < h and 0 <= nx < w and mask[ny, nx] and not seen[ny, nx]:
                            seen[ny, nx] = True
                            stack.append((ny, nx))
                if len(pts) >= min_px:
                    ys = [p[0] for p in pts]; xs = [p[1] for p in pts]
                    out.append((min(xs), min(ys), max(xs), max(ys), len(pts)))
    return out

def main():
    raw = sys.argv[1]
    x0, x1 = (int(v) for v in sys.argv[2:4]) if len(sys.argv) >= 4 else (600, 720)
    img = load_raw(raw).astype(np.int16)
    reg = img[:, x0:x1, :3]
    R, G, B = reg[..., 0], reg[..., 1], reg[..., 2]
    bright = (R + G + B) > 150   # 非暗
    print(f"[{raw}] 右栏 x{x0}..{x1} 连通域（按 y 排序）")
    for (bx, by, bx1, by1, n) in sorted(components(bright), key=lambda c: c[1]):
        cx, cy = (bx + bx1) // 2, (by + by1) // 2
        patch = reg[by:by1+1, bx:bx1+1].reshape(-1, 3)
        lum = patch.sum(axis=1)
        sel = patch[lum > 150]
        if len(sel) == 0:
            continue
        mr, mg, mb = sel.mean(axis=0)
        redfrac = float(((sel[:, 0] - sel[:, 1] > 40) & (sel[:, 0] - sel[:, 2] > 40)).mean())
        tag = 'RED' if redfrac > 0.5 else ('white' if mr > 190 else 'gray')
        print(f"  x{bx+x0:4d}..{bx1+x0:4d}  y{by:4d}..{by1:4d}  c=({cx+x0},{cy})  w={bx1-bx+1:3d} h={by1-by+1:3d} n={n:5d} rgb=({mr:3.0f},{mg:3.0f},{mb:3.0f}) {tag}")

if __name__ == '__main__':
    main()
