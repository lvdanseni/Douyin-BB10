#!/usr/bin/env python3
"""BB10 真机截屏 RAW → PNG + 右栏按钮几何分析。

用法: python shotanalyze.py <raw> <out.png> [tag]
RAW 格式: 4 个 int 头 [w,h,stride,fmt]，随后逐行 RGBA8888（stride 字节/行）。
"""
import sys, struct
import numpy as np
from PIL import Image

def load_raw(path):
    with open(path, 'rb') as f:
        data = f.read()
    w, h, stride_hdr, fmt = struct.unpack('<4i', data[:16])
    body = data[16:]
    # 信任数据长度推导的行距（头字段 stride 与实写行距曾不一致，真机实证）
    stride = len(body) // h
    rows = np.frombuffer(body[: h * stride], dtype=np.uint8).reshape(h, stride)
    px = rows[:, : w * 4].reshape(h, w, 4)
    return w, h, stride, px

def analyze(px, tag):
    h, w, _ = px.shape
    rgb = px[:, :, :3].astype(np.int32)
    bright = rgb.sum(axis=2) > 500          # 白色图标像素
    out = []
    out.append(f"[{tag}] {w}x{h} mean={rgb.mean():.1f} nonblack={(rgb.sum(axis=2)>24).mean()*100:.1f}%")
    # 右栏 x 带（列宽 96 rightMargin 10 → x 614..710）
    x0, x1 = 600, 720
    band = bright[:, x0:x1]
    # 逐行亮像素数 → 找图标行区间
    rown = band.sum(axis=1)
    # 简单地按 8px 分行统计亮像素列范围
    act = np.where(rown > 2)[0]
    if len(act):
        out.append(f"bright rows span y {act.min()}..{act.max()} (n={len(act)})")
    # 列直方图（亮像素在 x0..x1 的分布）
    coln = band.sum(axis=0)
    hot = np.where(coln > 8)[0]
    if len(hot):
        out.append(f"bright cols span x {x0+hot.min()}..{x0+hot.max()}")
    # chrome 检测：在 x 636..690 内找近似纯色矩形（Button 默认底衬）
    reg = rgb[:, 636:690]
    for y in range(0, h - 8, 8):
        blk = reg[y:y+48, :]
        if blk.size == 0:
            continue
        modal = np.median(blk.reshape(-1, 3), axis=0)
        frac = (np.abs(blk - modal).sum(axis=2) < 30).mean()
        if frac > 0.93:
            out.append(f"chrome-cand y={y}..{y+48} median={tuple(modal)} uniform={frac:.2f}")
    return "\n".join(out)

def main():
    raw, png = sys.argv[1], sys.argv[2]
    tag = sys.argv[3] if len(sys.argv) > 3 else raw
    w, h, stride, px = load_raw(raw)
    Image.fromarray(px[:, :, :3], 'RGB').save(png)
    print(f"saved {png} ({w}x{h} stride={stride})")
    print(analyze(px, tag))

if __name__ == '__main__':
    main()
