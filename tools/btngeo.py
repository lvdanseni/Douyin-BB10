#!/usr/bin/env python3
"""右栏按钮几何测量：在任意背景下定位 chrome 均匀矩形 + 白色图标包围盒。

用法: python btngeo.py <raw> [tag]
"""
import sys, struct
import numpy as np

def load(p):
    d = open(p, 'rb').read()
    w, h = struct.unpack('<4i', d[:16])[:2]
    s = (len(d) - 16) // h
    b = np.frombuffer(d[16:16 + h * s], dtype=np.uint8).reshape(h, s)
    return b[:, :w * 4].reshape(h, w, 4)[:, :, :3].astype(np.int32)

def icon_boxes(rgb, x0=605, x1=720, y0=90, y1=610, bright_thr=430, neutral_thr=42):
    """白色/中性亮图标连通簇 → 包围盒列表。"""
    band = rgb[y0:y1, x0:x1]
    r, g, b = band[:, :, 0], band[:, :, 1], band[:, :, 2]
    neutral = (np.abs(r - g) + np.abs(g - b) < neutral_thr)
    bright = (r + g + b > bright_thr) & neutral
    # 膨胀 2px 连通（同行/邻接近似即可，图标是整块）
    m = bright.copy()
    m[:, 1:] |= bright[:, :-1]
    m[:, :-1] |= bright[:, 1:]
    m[1:, :] |= bright[:-1, :]
    m[:-1, :] |= bright[1:, :]
    boxes = []
    used = np.zeros_like(m, bool)
    H, W = m.shape
    for yy in range(H):
        xx = 0
        while xx < W:
            if m[yy, xx] and not used[yy, xx]:
                # BFS
                stack = [(yy, xx)]
                used[yy, xx] = True
                minx = maxx = xx; miny = maxy = yy; n = 0
                while stack:
                    cy, cx = stack.pop()
                    n += 1
                    minx = min(minx, cx); maxx = max(maxx, cx)
                    miny = min(miny, cy); maxy = max(maxy, cy)
                    for dy in (-1, 0, 1):
                        for dx in (-1, 0, 1):
                            ny, nx = cy + dy, cx + dx
                            if 0 <= ny < H and 0 <= nx < W and m[ny, nx] and not used[ny, nx]:
                                used[ny, nx] = True
                                stack.append((ny, nx))
                if n >= 60:
                    boxes.append((x0 + minx, y0 + miny, x0 + maxx + 1, y0 + maxy + 1, n))
                xx = maxx + 1
            else:
                xx += 1
    return sorted(boxes, key=lambda t: t[1])

def chrome_boxes(rgb, size=48, x0=600, x1=720, y0=100, y1=600, uni=0.90, step=6):
    """内 uniform + 外环有对比的候选矩形（Button 默认 chrome）。"""
    out = []
    for y in range(y0, y1 - size, step):
        for x in range(x0, x1 - size, step):
            blk = rgb[y:y + size, x:x + size]
            med = np.median(blk.reshape(-1, 3), axis=0)
            frac = (np.abs(blk - med).sum(axis=2) < 24).mean()
            if frac < uni:
                continue
            # 外环：上下各扩 6px 的横条，要求与内部中位数有明显差
            ry0, ry1 = max(0, y - 8), min(rgb.shape[0], y + size + 8)
            rx0, rx1 = max(0, x - 8), min(rgb.shape[1], x + size + 8)
            ring = rgb[ry0:ry1, rx0:rx1].copy()
            ring[y - ry0:y - ry0 + size, x - rx0:x - rx0 + size] = -999
            ringv = ring[ring[:, :, 0] >= 0]
            if ringv.size == 0:
                continue
            rmed = np.median(ringv.reshape(-1, 3), axis=0)
            delta = np.abs(rmed - med).sum()
            if delta < 40:
                continue
            out.append((x, y, x + size, y + size, round(frac, 2), tuple(med.astype(int)), int(delta)))
    # 去重：保留每个 y 行最左的
    dedup = []
    lasty = -99
    for o in sorted(out, key=lambda t: (t[1], t[0])):
        if o[1] - lasty >= size // 2:
            dedup.append(o); lasty = o[1]
    return dedup

def main():
    raw = sys.argv[1]
    tag = sys.argv[2] if len(sys.argv) > 2 else raw
    rgb = load(raw)
    print(f"[{tag}] icons (x0,y0,x1,y1,npx):")
    for b in icon_boxes(rgb):
        print(f"  icon bbox x{b[0]}..{b[2]} y{b[1]}..{b[3]}  w={b[2]-b[0]} h={b[3]-b[1]} n={b[4]}")
    print(f"[{tag}] chrome candidates (48x48 uniform+deltaring):")
    for c in chrome_boxes(rgb):
        print(f"  chrome x{c[0]}..{c[2]} y{c[1]}..{c[3]} uni={c[4]} med={c[5]} delta={c[6]}")

if __name__ == '__main__':
    main()
