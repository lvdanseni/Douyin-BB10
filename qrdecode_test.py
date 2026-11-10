# -*- coding: utf-8 -*-
"""二维码端到端验证：用 qrtest.py 的算法渲染 PNG（与 C++ 完全相同的 scale/静区），
再用真实解码器（OpenCV QRCodeDetector）解回来。

为什么需要这个脚本：之前只比对了矩阵格子，但那次测试串只有 83 字节（V5），
而 V5 根本不需要版本信息块。真实 B 站二维码 URL 有 131 字节 → V8，
V7 及以上必须画 18 位版本信息，漏了就会让数据码字错位、彻底扫不出来。
矩阵比对发现不了这个问题，只有真解码才能发现。
"""
import sys
import cv2
import numpy as np

sys.path.insert(0, ".")
import qrtest


def render(matrix, scale=8, quiet=4):
    """按 C++ QrCodeGen::generate 的方式渲染：白底、四周 4 模块静区。"""
    size = len(matrix)
    dim = (size + quiet * 2) * scale
    img = np.full((dim, dim), 255, dtype=np.uint8)
    for y in range(size):
        for x in range(size):
            if matrix[y][x]:
                img[(y + quiet) * scale:(y + quiet + 1) * scale,
                    (x + quiet) * scale:(x + quiet + 1) * scale] = 0
    return img


def decode(img):
    det = cv2.QRCodeDetector()
    text, pts, _ = det.detectAndDecode(img)
    return text


def check(label, text, ecc):
    version, m = qrtest.make(text, ecc)
    img = render(m)
    got = decode(img)
    ok = (got == text)
    print(f"[{label}] V{version} size={len(m)} {len(m)}x{len(m)} "
          f"-> {'OK' if ok else 'FAIL'}  decoded={got[:60]!r}")
    return ok


if __name__ == "__main__":
    # 真实 B 站扫码 URL（2026-11-02 实测 131 字节，V8/M）
    real = ("https://account.bilibili.com/h5/account-h5/auth/scan-web"
            "?navhide=1&callback=close&qrcode_key="
            "4b7538af7954c89f784e2b2ca57a44ea&from=")
    cases = [
        ("V2 短串", "hello", "M"),
        ("V5 中串", "https://www.bilibili.com/h5/login-app?qrcode_key=" + "a" * 32, "M"),
        ("V8 真实URL", real, "M"),
        ("V8 真实URL/L", real, "L"),
    ]
    allok = True
    for label, text, ecc in cases:
        allok = check(label, text, ecc) and allok
    print("\nALL PASS" if allok else "\nHAS FAILURE")
