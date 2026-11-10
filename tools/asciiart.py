#!/usr/bin/env python3
"""ASCII 截图渲染：亮度+颜色分类字符。用法: asciiart.py <raw> [cols] [region x0 y0 x1 y1]"""
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
    cols = int(sys.argv[2]) if len(sys.argv) >= 3 else 90
    img = load_raw(raw).astype(np.int16)
    if len(sys.argv) >= 7:
        x0, y0, x1, y1 = (int(v) for v in sys.argv[3:7])
        img = img[y0:y1, x0:x1]
    h, w = img.shape[:2]
    cw = w / cols
    rows = max(1, int(h / cw / 2.1))
    out = []
    for r in range(rows):
        line = []
        for c in range(cols):
            ys = slice(int(r * h / rows), max(int(r * h / rows) + 1, int((r + 1) * h / rows)))
            xs = slice(int(c * w / cols), max(int(c * w / cols) + 1, int((c + 1) * w / cols)))
            cell = img[ys, xs, :3].reshape(-1, 3)
            R, G, B = cell[:, 0], cell[:, 1], cell[:, 2]
            lum = (R + G + B).mean() / 3
            redf = ((R - G > 40) & (R - B > 40)).mean()
            bluef = ((B - R > 40) & (B - G > 40)).mean()
            if redf > 0.25:
                line.append('R')
            elif bluef > 0.25:
                line.append('B')
            elif lum < 12:
                line.append(' ')
            elif lum < 45:
                line.append('.')
            elif lum < 95:
                line.append(':')
            elif lum < 150:
                line.append('+')
            elif lum < 200:
                line.append('*')
            else:
                line.append('#')
        out.append(''.join(line))
    print(f"[{raw}] {w}x{h} -> {cols}x{rows}")
    print('\n'.join(out))

if __name__ == '__main__':
    main()
