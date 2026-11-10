# -*- coding: utf-8 -*-
"""QR 编码器参考实现（V1-V10, byte mode, ECC L/M）。
目的：先用 Python 写出与将来 C++ 版本**完全一致**的算法，再和 qrcode 库逐位对照，
确认无误后翻译成 C++ —— 本机没有 C++ 编译器，这是唯一可靠的验证途径。"""
import qrcode
from qrcode.constants import ERROR_CORRECT_L, ERROR_CORRECT_M

# ---- 每个版本的总 codewords 数 ----
TOTAL_CODEWORDS = {
    1: 26, 2: 44, 3: 70, 4: 100, 5: 134,
    6: 172, 7: 196, 8: 242, 9: 292, 10: 346,
}

# ---- RS 块结构：(块数, 每块总 codewords, 每块数据 codewords) 的列表 ----
# L 级
BLOCKS_L = {
    1:  [(1, 26, 19)],
    2:  [(1, 44, 34)],
    3:  [(1, 70, 55)],
    4:  [(1, 100, 80)],
    5:  [(1, 134, 108)],
    6:  [(2, 86, 68)],
    7:  [(2, 98, 78)],
    8:  [(2, 121, 97)],
    9:  [(2, 146, 116)],
    10: [(2, 86, 68), (2, 87, 69)],
}
# M 级
BLOCKS_M = {
    1:  [(1, 26, 16)],
    2:  [(1, 44, 28)],
    3:  [(1, 70, 44)],
    4:  [(2, 50, 32)],
    5:  [(2, 67, 43)],
    6:  [(4, 43, 27)],
    7:  [(4, 49, 31)],
    8:  [(2, 60, 38), (2, 61, 39)],
    9:  [(3, 58, 36), (2, 59, 37)],
    10: [(4, 69, 43), (1, 70, 44)],
}

# ---- alignment pattern 中心坐标 ----
ALIGN_POS = {
    1: [], 2: [6, 18], 3: [6, 22], 4: [6, 26], 5: [6, 30],
    6: [6, 34], 7: [6, 22, 38], 8: [6, 24, 42], 9: [6, 26, 46], 10: [6, 28, 50],
}

ECC_BITS = {"L": 0b01, "M": 0b00}


# ============ GF(256) 与 Reed-Solomon ============
def gf_mul(x, y):
    # GF(2^8)，本原多项式 0x11D (x^8+x^4+x^3+x^2+1)
    # ⚠️ 每轮都必须截断到 8 位：否则 z 会累积高位，`z >> 7` 判的就不是"第 8 位"
    # 而是更高的位，模运算整个错掉（表现为 ECC 全错、数据部分却是对的）。
    z = 0
    for i in range(7, -1, -1):
        z = (z << 1) ^ ((z >> 7) * 0x11D)
        z ^= ((y >> i) & 1) * x
        z &= 0xFF
    return z


def rs_generator(degree):
    """生成多项式 prod (x - a^i)，返回**降幂**系数（gen[0] == 1，最高次项）。

    内部乘法是按升幂累乘的（乘以 (x+root) 时低位乘 root、高位乘 x），
    最后翻转一次即可。不翻转的话 rs_remainder 里的 gen[i+1] 会取到反的系数，
    结果是"数据部分全对、ECC 部分全错"这种特别迷惑的症状。"""
    result = [1]
    root = 1
    for _ in range(degree):
        new = [0] * (len(result) + 1)
        for i, c in enumerate(result):
            new[i] ^= gf_mul(c, root)
            new[i + 1] ^= c
        result = new
        root = gf_mul(root, 0x02)
    result.reverse()
    return result


def rs_remainder(data, degree):
    """多项式除法取余"""
    gen = rs_generator(degree)
    rem = [0] * degree
    for b in data:
        factor = b ^ rem[0]
        rem = rem[1:] + [0]
        for i in range(degree):
            rem[i] ^= gf_mul(gen[i + 1], factor)
    return rem


# ============ 数据编码 ============
def encode_data(text, version, ecc):
    """byte mode 编码 → 数据 codewords（含 terminator/padding）"""
    data = text.encode("utf-8")
    blocks = (BLOCKS_L if ecc == "L" else BLOCKS_M)[version]
    total_data = sum(n * d for (n, _t, d) in blocks)

    bits = []

    def put(val, n):
        for i in range(n - 1, -1, -1):
            bits.append((val >> i) & 1)

    put(0b0100, 4)                       # byte mode
    put(len(data), 8 if version <= 9 else 16)
    for b in data:
        put(b, 8)

    # terminator
    cap = total_data * 8
    put(0, min(4, cap - len(bits)))
    # 补齐到字节边界
    while len(bits) % 8:
        bits.append(0)

    codewords = []
    for i in range(0, len(bits), 8):
        v = 0
        for j in range(8):
            v = (v << 1) | bits[i + j]
        codewords.append(v)

    # pad
    pad = [0xEC, 0x11]
    k = 0
    while len(codewords) < total_data:
        codewords.append(pad[k % 2])
        k += 1
    return codewords


def interleave(data_codewords, version, ecc):
    """按 RS 块切分、算 ECC、交织"""
    blocks = (BLOCKS_L if ecc == "L" else BLOCKS_M)[version]
    datas, eccs = [], []
    pos = 0
    max_data = 0
    max_ecc = 0
    for (n, total, dnum) in blocks:
        en = total - dnum
        max_data = max(max_data, dnum)
        max_ecc = max(max_ecc, en)
        for _ in range(n):
            chunk = data_codewords[pos:pos + dnum]
            pos += dnum
            datas.append(chunk)
            eccs.append(rs_remainder(chunk, en))

    out = []
    for i in range(max_data):
        for blk in datas:
            if i < len(blk):
                out.append(blk[i])
    for i in range(max_ecc):
        for blk in eccs:
            if i < len(blk):
                out.append(blk[i])
    return out


# ============ 矩阵布局 ============
class Matrix:
    def __init__(self, version):
        self.version = version
        self.size = version * 4 + 17
        self.m = [[0] * self.size for _ in range(self.size)]
        self.res = [[False] * self.size for _ in range(self.size)]  # 是否已占用

    def setf(self, x, y, val):
        self.m[y][x] = 1 if val else 0
        self.res[y][x] = True

    def draw_finder(self, cx, cy):
        for dy in range(-4, 5):
            for dx in range(-4, 5):
                x, y = cx + dx, cy + dy
                if 0 <= x < self.size and 0 <= y < self.size:
                    d = max(abs(dx), abs(dy))
                    self.setf(x, y, d != 2 and d != 4)

    def draw_alignment(self, cx, cy):
        for dy in range(-2, 3):
            for dx in range(-2, 3):
                d = max(abs(dx), abs(dy))
                self.setf(cx + dx, cy + dy, d != 1)

    def draw_timing(self):
        for i in range(self.size):
            if not self.res[6][i]:
                self.setf(i, 6, i % 2 == 0)
            if not self.res[i][6]:
                self.setf(6, i, i % 2 == 0)

    def draw_format(self, ecc, mask):
        data = (ECC_BITS[ecc] << 3) | mask
        rem = data
        for _ in range(10):
            rem = (rem << 1) ^ ((rem >> 9) * 0x537)
        bits = ((data << 10) | rem) ^ 0x5412

        def bit(i):
            return (bits >> i) & 1

        # 左上角
        for i in range(6):
            self.setf(8, i, bit(i))
        self.setf(8, 7, bit(6))
        self.setf(8, 8, bit(7))
        self.setf(7, 8, bit(8))
        for i in range(9, 15):
            self.setf(14 - i, 8, bit(i))
        # 右上 / 左下
        for i in range(8):
            self.setf(self.size - 1 - i, 8, bit(i))
        for i in range(8, 15):
            self.setf(8, self.size - 15 + i, bit(i))
        self.setf(8, self.size - 8, 1)   # dark module

    def draw_version(self):
        """V7 及以上必须画 18 位版本信息（两份，右上角与左下角各一份）。
        【曾经漏掉】漏了的话这 36 个模块不会被标记成功能图形，draw_codewords
        会把数据位写进去，导致码字整体错位 —— 矩阵"看起来像二维码"但永远扫不出来。
        而且 V1~V6 不需要它，所以拿短串测试永远发现不了这个 bug。"""
        if self.version < 7:
            return
        rem = self.version
        for _ in range(12):
            rem = (rem << 1) ^ ((rem >> 11) * 0x1F25)
        bits = (self.version << 12) | rem
        for i in range(18):
            bit = (bits >> i) & 1
            a = self.size - 11 + i % 3
            b = i // 3
            self.setf(a, b, bit)
            self.setf(b, a, bit)

    def draw_function_patterns(self, ecc, mask):
        self.draw_finder(3, 3)
        self.draw_finder(self.size - 4, 3)
        self.draw_finder(3, self.size - 4)
        self.draw_timing()
        pos = ALIGN_POS[self.version]
        for cy in pos:
            for cx in pos:
                # 跳过与三个 finder 重叠的位置
                if (cx == 6 and cy == 6) or (cx == 6 and cy == self.size - 7) \
                        or (cx == self.size - 7 and cy == 6):
                    continue
                self.draw_alignment(cx, cy)
        self.draw_version()
        self.draw_format(ecc, mask)

    def draw_codewords(self, codewords):
        bits = []
        for cw in codewords:
            for i in range(7, -1, -1):
                bits.append((cw >> i) & 1)
        idx = 0
        right = self.size - 1
        upward = True
        while right >= 1:
            if right == 6:
                right -= 1
            for vert in range(self.size):
                y = (self.size - 1 - vert) if upward else vert
                for j in range(2):
                    x = right - j
                    if not self.res[y][x]:
                        val = bits[idx] if idx < len(bits) else 0
                        self.m[y][x] = val
                        idx += 1
            upward = not upward
            right -= 2

    def apply_mask(self, mask):
        for y in range(self.size):
            for x in range(self.size):
                if self.res[y][x]:
                    continue
                inv = False
                if mask == 0:
                    inv = (x + y) % 2 == 0
                elif mask == 1:
                    inv = y % 2 == 0
                elif mask == 2:
                    inv = x % 3 == 0
                elif mask == 3:
                    inv = (x + y) % 3 == 0
                elif mask == 4:
                    inv = (x // 3 + y // 2) % 2 == 0
                elif mask == 5:
                    inv = (x * y) % 2 + (x * y) % 3 == 0
                elif mask == 6:
                    inv = ((x * y) % 2 + (x * y) % 3) % 2 == 0
                elif mask == 7:
                    inv = ((x + y) % 2 + (x * y) % 3) % 2 == 0
                if inv:
                    self.m[y][x] ^= 1

    def penalty(self):
        p = 0
        s = self.size
        # 规则1：连续同色
        for y in range(s):
            run = 1
            for x in range(1, s):
                if self.m[y][x] == self.m[y][x - 1]:
                    run += 1
                else:
                    if run >= 5:
                        p += 3 + (run - 5)
                    run = 1
            if run >= 5:
                p += 3 + (run - 5)
        for x in range(s):
            run = 1
            for y in range(1, s):
                if self.m[y][x] == self.m[y - 1][x]:
                    run += 1
                else:
                    if run >= 5:
                        p += 3 + (run - 5)
                    run = 1
            if run >= 5:
                p += 3 + (run - 5)
        # 规则2：2x2 同色块
        for y in range(s - 1):
            for x in range(s - 1):
                v = self.m[y][x]
                if v == self.m[y][x + 1] == self.m[y + 1][x] == self.m[y + 1][x + 1]:
                    p += 3
        # 规则3：finder-like 图案
        pat1 = [1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0]
        pat2 = [0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1]
        for y in range(s):
            for x in range(s - 10):
                seg = self.m[y][x:x + 11]
                if seg == pat1 or seg == pat2:
                    p += 40
        for x in range(s):
            for y in range(s - 10):
                seg = [self.m[y + i][x] for i in range(11)]
                if seg == pat1 or seg == pat2:
                    p += 40
        # 规则4：暗色比例
        dark = sum(sum(row) for row in self.m)
        total = s * s
        k = abs(dark * 100 // total - 50) // 5
        p += k * 10
        return p


def make(text, ecc="M"):
    data = text.encode("utf-8")
    # 选最小版本
    version = None
    for v in range(1, 11):
        blocks = (BLOCKS_L if ecc == "L" else BLOCKS_M)[v]
        cap = sum(n * d for (n, _t, d) in blocks)
        need = 2 + len(data)   # mode(4bit) + count(8bit) ≈ 2 字节，宽松估计
        if cap >= need:
            version = v
            break
    if version is None:
        raise ValueError("too long")

    cw = encode_data(text, version, ecc)
    allcw = interleave(cw, version, ecc)

    best = None
    best_p = None
    for mask in range(8):
        mtx = Matrix(version)
        mtx.draw_function_patterns(ecc, mask)
        mtx.draw_codewords(allcw)
        mtx.apply_mask(mask)
        p = mtx.penalty()
        if best_p is None or p < best_p:
            best_p = p
            best = mtx
    return version, best.m


def reference_matrix(text, ecc="M"):
    lvl = ERROR_CORRECT_M if ecc == "M" else ERROR_CORRECT_L
    q = qrcode.QRCode(error_correction=lvl, box_size=1, border=0)
    q.add_data(text)
    q.make(fit=True)
    m = q.get_matrix()
    return q.version, [[1 if c else 0 for c in row] for row in m]


if __name__ == "__main__":
    test = "https://www.bilibili.com/h5/login-app?qrcode_key=" + "a" * 32
    print("测试串长度:", len(test))
    for ecc in ("L", "M"):
        v1, m1 = make(test, ecc)
        v2, m2 = reference_matrix(test, ecc)
        print(f"ECC={ecc}  mine V{v1} size={len(m1)}  ref V{v2} size={len(m2)}")
        if v1 == v2 and len(m1) == len(m2):
            diff = sum(1 for y in range(len(m1)) for x in range(len(m1))
                       if m1[y][x] != m2[y][x])
            print(f"   差异格子数 = {diff} / {len(m1) ** 2}")
        else:
            print("   版本不一致，无法逐格比对")
