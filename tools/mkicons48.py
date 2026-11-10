#!/usr/bin/env python3
"""1.0.0.48 右栏图标素材生成：96x96 -> 44x44 默认态 + 44x44 压暗按压态。
用法: python tools/mkicons48.py   （在 DouyinBar/ 下运行）
输出: assets/images/ic_<name>_s.png  (44x44 默认)
      assets/images/ic_<name>_sp.png (44x44 按压, RGB*0.6, alpha 不变)
"""
from PIL import Image
import os

SRC = 'assets/images/'
ICONS = [
    'ic_like', 'ic_like_gray', 'ic_comment', 'ic_star', 'ic_star_on',
    'ic_person', 'ic_person_on', 'ic_folder', 'ic_folder_on',
    'ic_download', 'ic_dl_ok',
]
SIZE = 44
PRESS_K = 0.6

for name in ICONS:
    src = SRC + name + '.png'
    if not os.path.exists(src):
        print('MISSING', src)
        continue
    im = Image.open(src).convert('RGBA')
    small = im.resize((SIZE, SIZE), Image.LANCZOS)
    out = SRC + name + '_s.png'
    small.save(out, optimize=True)

    # 按压态：RGB 压暗 60%，alpha 不变
    px = small.load()
    for y in range(SIZE):
        for x in range(SIZE):
            r, g, b, a = px[x, y]
            px[x, y] = (int(r * PRESS_K), int(g * PRESS_K), int(b * PRESS_K), a)
    out_p = SRC + name + '_sp.png'
    small.save(out_p, optimize=True)
    print(f'{name}: {os.path.getsize(out)}B  +press {os.path.getsize(out_p)}B')

print('done')
