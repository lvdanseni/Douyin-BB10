#include "qrcodegen.hpp"

#include <QFile>
#include <QDebug>
#include <string.h>

// ============================================================================
// 标准表格（V1~V10）
// ============================================================================

// 每个版本的总 codewords 数
static const int TOTAL_CODEWORDS[11] = {
    0, 26, 44, 70, 100, 134, 172, 196, 242, 292, 346
};

// RS 块结构。每项形如 { 块数, 每块总 codewords, 每块数据 codewords }，最多两段。
struct RsBlocks {
    int n1, total1, data1;   // 第一段
    int n2, total2, data2;   // 第二段（n2 == 0 表示没有）
};

static const RsBlocks BLOCKS_L[11] = {
    {0, 0, 0, 0, 0, 0},
    {1,  26,  19, 0,  0,  0},
    {1,  44,  34, 0,  0,  0},
    {1,  70,  55, 0,  0,  0},
    {1, 100,  80, 0,  0,  0},
    {1, 134, 108, 0,  0,  0},
    {2,  86,  68, 0,  0,  0},
    {2,  98,  78, 0,  0,  0},
    {2, 121,  97, 0,  0,  0},
    {2, 146, 116, 0,  0,  0},
    {2,  86,  68, 2, 87, 69},
};

static const RsBlocks BLOCKS_M[11] = {
    {0, 0, 0, 0, 0, 0},
    {1,  26,  16, 0,  0,  0},
    {1,  44,  28, 0,  0,  0},
    {1,  70,  44, 0,  0,  0},
    {2,  50,  32, 0,  0,  0},
    {2,  67,  43, 0,  0,  0},
    {4,  43,  27, 0,  0,  0},
    {4,  49,  31, 0,  0,  0},
    {2,  60,  38, 2, 61, 39},
    {3,  58,  36, 2, 59, 37},
    {4,  69,  43, 1, 70, 44},
};

// alignment pattern 中心坐标
static const int ALIGN_POS[11][4] = {
    {0, 0, 0, 0},
    {0, 0, 0, 0},              // V1 没有
    {2, 6, 18, 0},
    {2, 6, 22, 0},
    {2, 6, 26, 0},
    {2, 6, 30, 0},
    {2, 6, 34, 0},
    {3, 6, 22, 38},
    {3, 6, 24, 42},
    {3, 6, 26, 46},
    {3, 6, 28, 50},
};

static int eccBits(char ecc)
{
    return (ecc == 'L') ? 0x01 : 0x00;   // L=01, M=00
}

static const RsBlocks* blocksFor(int version, char ecc)
{
    if (version < 1 || version > 10) return 0;
    return (ecc == 'L') ? &BLOCKS_L[version] : &BLOCKS_M[version];
}

static int dataCapacity(const RsBlocks* b)
{
    int cap = b->n1 * b->data1;
    if (b->n2 > 0) cap += b->n2 * b->data2;
    return cap;
}

// ============================================================================
// GF(256) 与 Reed-Solomon
// ============================================================================

static int gfMul(int x, int y)
{
    // 本原多项式 0x11D (x^8+x^4+x^3+x^2+1)。
    // 每轮必须截断到 8 位 —— 否则高位累积，`z >> 7` 判的就不是第 8 位了，
    // 表现为"数据部分全对、ECC 部分全错"。
    int z = 0;
    for (int i = 7; i >= 0; --i) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
        z &= 0xFF;
    }
    return z;
}

/**
 * 生成多项式 prod (x - a^i)，返回**降幂**系数（gen[0] == 1）。
 * 内部按升幂累乘，返回前 reverse。
 */
static void rsGenerator(int degree, unsigned char* gen)
{
    // gen 长度 = degree + 1
    unsigned char tmp[64];
    int len = 1;
    tmp[0] = 1;
    int root = 1;
    for (int d = 0; d < degree; ++d) {
        unsigned char next[64];
        memset(next, 0, sizeof(next));
        for (int i = 0; i < len; ++i) {
            next[i] ^= (unsigned char)gfMul(tmp[i], root);
            next[i + 1] ^= tmp[i];
        }
        ++len;
        memcpy(tmp, next, len);
        root = gfMul(root, 0x02);
    }
    // 反转成降幂
    for (int i = 0; i < degree + 1; ++i) {
        gen[i] = tmp[degree - i];
    }
}

/** 多项式除法取余（rem 长度 = degree） */
static void rsRemainder(const unsigned char* data, int dataLen, int degree,
                        unsigned char* rem)
{
    unsigned char gen[64];
    rsGenerator(degree, gen);
    memset(rem, 0, degree);
    for (int k = 0; k < dataLen; ++k) {
        const int factor = data[k] ^ rem[0];
        // rem 左移一格
        for (int i = 0; i < degree - 1; ++i) rem[i] = rem[i + 1];
        rem[degree - 1] = 0;
        if (factor) {
            for (int i = 0; i < degree; ++i) {
                rem[i] ^= (unsigned char)gfMul(gen[i + 1], factor);
            }
        }
    }
}

// ============================================================================
// 数据编码
// ============================================================================

/** byte mode 编码 → 数据 codewords（含 terminator 与 padding） */
static void encodeData(const QByteArray& utf8, int version, char ecc,
                       unsigned char* out, int outLen)
{
    const RsBlocks* b = blocksFor(version, ecc);
    const int totalData = dataCapacity(b);

    unsigned char bits[8192];
    int nbits = 0;

    // mode indicator 0100
    bits[nbits++] = 0;
    bits[nbits++] = 1;
    bits[nbits++] = 0;
    bits[nbits++] = 0;

    // character count
    const int cntBits = (version <= 9) ? 8 : 16;
    const int len = utf8.size();
    for (int i = cntBits - 1; i >= 0; --i) {
        bits[nbits++] = (len >> i) & 1;
    }

    // 数据字节
    for (int i = 0; i < len; ++i) {
        const unsigned char c = (unsigned char)utf8.at(i);
        for (int j = 7; j >= 0; --j) {
            bits[nbits++] = (c >> j) & 1;
        }
    }

    // terminator（最多 4 个 0）
    const int cap = totalData * 8;
    int term = 4;
    if (cap - nbits < 4) term = cap - nbits;
    for (int i = 0; i < term; ++i) bits[nbits++] = 0;

    // 补齐到字节边界
    while (nbits % 8) bits[nbits++] = 0;

    // 转成 codewords
    int n = 0;
    for (int i = 0; i < nbits; i += 8) {
        int v = 0;
        for (int j = 0; j < 8; ++j) v = (v << 1) | bits[i + j];
        out[n++] = (unsigned char)v;
    }

    // pad bytes
    const unsigned char pad[2] = { 0xEC, 0x11 };
    int k = 0;
    while (n < totalData && n < outLen) {
        out[n++] = pad[k % 2];
        ++k;
    }
}

/** 分块算 ECC 并按标准交织 */
static void interleave(const unsigned char* data, int version, char ecc,
                       unsigned char* out, int outLen)
{
    const RsBlocks* b = blocksFor(version, ecc);

    unsigned char datas[16][128];
    unsigned char eccs[16][64];
    int dataLenArr[16], eccLenArr[16];
    int nb = 0;

    int pos = 0;
    int maxData = 0, maxEcc = 0;

    for (int seg = 0; seg < 2; ++seg) {
        const int cnt = (seg == 0) ? b->n1 : b->n2;
        const int total = (seg == 0) ? b->total1 : b->total2;
        const int dnum = (seg == 0) ? b->data1 : b->data2;
        if (cnt <= 0) continue;
        const int en = total - dnum;
        for (int i = 0; i < cnt && nb < 16; ++i) {
            memcpy(datas[nb], data + pos, dnum);
            pos += dnum;
            dataLenArr[nb] = dnum;
            rsRemainder(datas[nb], dnum, en, eccs[nb]);
            eccLenArr[nb] = en;
            if (dnum > maxData) maxData = dnum;
            if (en > maxEcc) maxEcc = en;
            ++nb;
        }
    }

    int o = 0;
    for (int i = 0; i < maxData; ++i) {
        for (int j = 0; j < nb; ++j) {
            if (i < dataLenArr[j] && o < outLen) out[o++] = datas[j][i];
        }
    }
    for (int i = 0; i < maxEcc; ++i) {
        for (int j = 0; j < nb; ++j) {
            if (i < eccLenArr[j] && o < outLen) out[o++] = eccs[j][i];
        }
    }
}

// ============================================================================
// 矩阵布局
// ============================================================================

namespace {

struct Matrix {
    int version;
    int size;
    unsigned char m[64][64];    // 0/1
    unsigned char res[64][64];  // 是否已被功能图形占用

    Matrix(int v) : version(v), size(v * 4 + 17)
    {
        memset(m, 0, sizeof(m));
        memset(res, 0, sizeof(res));
    }

    void setf(int x, int y, int val)
    {
        if (x < 0 || y < 0 || x >= size || y >= size) return;
        m[y][x] = (unsigned char)(val ? 1 : 0);
        res[y][x] = 1;
    }

    void drawFinder(int cx, int cy)
    {
        for (int dy = -4; dy <= 4; ++dy) {
            for (int dx = -4; dx <= 4; ++dx) {
                const int d = qMax(qAbs(dx), qAbs(dy));
                setf(cx + dx, cy + dy, (d != 2 && d != 4) ? 1 : 0);
            }
        }
    }

    void drawAlignment(int cx, int cy)
    {
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                const int d = qMax(qAbs(dx), qAbs(dy));
                setf(cx + dx, cy + dy, (d != 1) ? 1 : 0);
            }
        }
    }

    void drawTiming()
    {
        for (int i = 0; i < size; ++i) {
            if (!res[6][i]) setf(i, 6, (i % 2 == 0) ? 1 : 0);
            if (!res[i][6]) setf(6, i, (i % 2 == 0) ? 1 : 0);
        }
    }

    void drawFormat(char ecc, int mask)
    {
        const int data = (eccBits(ecc) << 3) | mask;
        int rem = data;
        for (int i = 0; i < 10; ++i) {
            rem = (rem << 1) ^ ((rem >> 9) * 0x537);
        }
        const int bits = ((data << 10) | rem) ^ 0x5412;

#define BIT(i) (((bits) >> (i)) & 1)
        for (int i = 0; i < 6; ++i)  setf(8, i, BIT(i));
        setf(8, 7, BIT(6));
        setf(8, 8, BIT(7));
        setf(7, 8, BIT(8));
        for (int i = 9; i < 15; ++i) setf(14 - i, 8, BIT(i));

        for (int i = 0; i < 8; ++i)  setf(size - 1 - i, 8, BIT(i));
        for (int i = 8; i < 15; ++i) setf(8, size - 15 + i, BIT(i));
        setf(8, size - 8, 1);   // dark module
#undef BIT
    }

    /**
     * V7 及以上必须画 18 位版本信息（右上角、左下角各一份，各 3x6）。
     *
     * 【曾经漏掉，代价是"二维码看着像但扫不出来"】
     * 漏了这 36 个模块不会被标记为功能图形，drawCodewords 会把数据位写进去，
     * 于是码字整体错位 —— 矩阵形状、三个定位角、时序线全都对，唯独内容解不出来。
     * 更坑的是 V1~V6 根本不需要它，所以拿短串（< 106 字节）测永远发现不了；
     * 而真实 B 站扫码 URL 有 131 字节 → V8，正好落在需要版本信息的区间。
     */
    void drawVersion()
    {
        if (version < 7) return;
        int rem = version;
        for (int i = 0; i < 12; ++i) {
            rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
        }
        const int bits = (version << 12) | rem;
        for (int i = 0; i < 18; ++i) {
            const int bit = (bits >> i) & 1;
            const int a = size - 11 + i % 3;
            const int b = i / 3;
            setf(a, b, bit);
            setf(b, a, bit);
        }
    }

    void drawFunctionPatterns(char ecc, int mask)
    {
        drawFinder(3, 3);
        drawFinder(size - 4, 3);
        drawFinder(3, size - 4);
        drawTiming();

        const int* pos = ALIGN_POS[version];
        const int n = pos[0];
        for (int a = 0; a < n; ++a) {
            for (int b2 = 0; b2 < n; ++b2) {
                const int cx = pos[1 + a];
                const int cy = pos[1 + b2];
                // 跳过与三个 finder 重叠的位置
                if ((cx == 6 && cy == 6)
                    || (cx == 6 && cy == size - 7)
                    || (cx == size - 7 && cy == 6)) {
                    continue;
                }
                drawAlignment(cx, cy);
            }
        }
        drawVersion();
        drawFormat(ecc, mask);
    }

    void drawCodewords(const unsigned char* cw, int cwLen)
    {
        int idx = 0;
        int right = size - 1;
        bool upward = true;
        while (right >= 1) {
            if (right == 6) --right;   // 跳过竖向 timing 列
            for (int vert = 0; vert < size; ++vert) {
                const int y = upward ? (size - 1 - vert) : vert;
                for (int j = 0; j < 2; ++j) {
                    const int x = right - j;
                    if (!res[y][x]) {
                        int val = 0;
                        if (idx < cwLen * 8) {
                            val = (cw[idx >> 3] >> (7 - (idx & 7))) & 1;
                        }
                        m[y][x] = (unsigned char)val;
                        ++idx;
                    }
                }
            }
            upward = !upward;
            right -= 2;
        }
    }

    void applyMask(int mask)
    {
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                if (res[y][x]) continue;
                bool inv = false;
                switch (mask) {
                case 0: inv = ((x + y) % 2) == 0; break;
                case 1: inv = (y % 2) == 0; break;
                case 2: inv = (x % 3) == 0; break;
                case 3: inv = ((x + y) % 3) == 0; break;
                case 4: inv = ((x / 3 + y / 2) % 2) == 0; break;
                case 5: inv = ((x * y) % 2 + (x * y) % 3) == 0; break;
                case 6: inv = (((x * y) % 2 + (x * y) % 3) % 2) == 0; break;
                case 7: inv = (((x + y) % 2 + (x * y) % 3) % 2) == 0; break;
                default: break;
                }
                if (inv) m[y][x] ^= 1;
            }
        }
    }

    int penalty() const
    {
        int p = 0;
        // 规则 1：连续同色 >= 5
        for (int y = 0; y < size; ++y) {
            int run = 1;
            for (int x = 1; x < size; ++x) {
                if (m[y][x] == m[y][x - 1]) {
                    ++run;
                } else {
                    if (run >= 5) p += 3 + (run - 5);
                    run = 1;
                }
            }
            if (run >= 5) p += 3 + (run - 5);
        }
        for (int x = 0; x < size; ++x) {
            int run = 1;
            for (int y = 1; y < size; ++y) {
                if (m[y][x] == m[y - 1][x]) {
                    ++run;
                } else {
                    if (run >= 5) p += 3 + (run - 5);
                    run = 1;
                }
            }
            if (run >= 5) p += 3 + (run - 5);
        }
        // 规则 2：2x2 同色块
        for (int y = 0; y < size - 1; ++y) {
            for (int x = 0; x < size - 1; ++x) {
                const int v = m[y][x];
                if (v == m[y][x + 1] && v == m[y + 1][x] && v == m[y + 1][x + 1]) {
                    p += 3;
                }
            }
        }
        // 规则 3：finder 相似图案
        static const int pat1[11] = { 1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0 };
        static const int pat2[11] = { 0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1 };
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x + 11 <= size; ++x) {
                bool a = true, b2 = true;
                for (int k = 0; k < 11; ++k) {
                    if (m[y][x + k] != pat1[k]) a = false;
                    if (m[y][x + k] != pat2[k]) b2 = false;
                }
                if (a || b2) p += 40;
            }
        }
        for (int x = 0; x < size; ++x) {
            for (int y = 0; y + 11 <= size; ++y) {
                bool a = true, b2 = true;
                for (int k = 0; k < 11; ++k) {
                    if (m[y + k][x] != pat1[k]) a = false;
                    if (m[y + k][x] != pat2[k]) b2 = false;
                }
                if (a || b2) p += 40;
            }
        }
        // 规则 4：暗色比例
        int dark = 0;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) dark += m[y][x];
        }
        const int total = size * size;
        int ratio = (dark * 100) / total;
        int dev = ratio - 50;
        if (dev < 0) dev = -dev;
        p += (dev / 5) * 10;
        return p;
    }
};

} // namespace

// ============================================================================
// 对外接口
// ============================================================================

/** 每个版本在码字之后剩下的"余数位"数（规范表）。用于自检自由模块数。 */
static int remainderBits(int v)
{
    if (v == 1) return 0;
    if (v <= 6) return 7;
    if (v <= 13) return 0;
    if (v <= 20) return 3;
    if (v <= 27) return 4;
    if (v <= 34) return 3;
    return 0;
}

QByteArray QrCodeGen::generateMatrix(const QString& text, char ecc, int* outSize)
{
    if (ecc != 'L' && ecc != 'M') ecc = 'M';
    const QByteArray utf8 = text.toUtf8();
    const int len = utf8.size();

    // 选最小版本。需要的数据 codewords = ceil((4 + cntBits + 8*len) / 8)，
    // 其中 cntBits 在 V1~V9 是 8、V10 是 16（byte mode 的字符计数位宽）。
    // 【别用 len + 2 这种估算】V10 的 cntBits 是 16，实际要 len + 3，
    // 估算会挑到装不下的版本，encodeData 里就会静默截断。
    int version = 0;
    for (int v = 1; v <= 10; ++v) {
        const RsBlocks* b = blocksFor(v, ecc);
        if (!b) continue;
        const int cntBits = (v <= 9) ? 8 : 16;
        const int need = (4 + cntBits + 8 * len + 7) / 8;
        if (dataCapacity(b) >= need) { version = v; break; }
    }
    if (version == 0) {
        qWarning("QrCodeGen: content too long (%d bytes)", len);
        return QByteArray();
    }

    unsigned char dataCw[512];
    memset(dataCw, 0, sizeof(dataCw));
    encodeData(utf8, version, ecc, dataCw, (int)sizeof(dataCw));

    unsigned char allCw[512];
    memset(allCw, 0, sizeof(allCw));
    const int totalCw = TOTAL_CODEWORDS[version];
    interleave(dataCw, version, ecc, allCw, (int)sizeof(allCw));

    // 选 penalty 最小的 mask
    int bestMask = 0;
    int bestPenalty = -1;
    Matrix* best = 0;
    for (int mask = 0; mask < 8; ++mask) {
        Matrix* mtx = new Matrix(version);
        mtx->drawFunctionPatterns(ecc, mask);
        mtx->drawCodewords(allCw, totalCw);
        mtx->applyMask(mask);
        const int p = mtx->penalty();
        if (bestPenalty < 0 || p < bestPenalty) {
            bestPenalty = p;
            bestMask = mask;
            delete best;
            best = mtx;
        } else {
            delete mtx;
        }
    }
    if (!best) return QByteArray();

    // 【关键自检】自由模块数必须恰好等于 totalCw*8 + 余数位。
    // 这个等式能一次性抓住"漏画某个功能图形"的 bug —— 比如 V7+ 的版本信息块
    // 漏掉时，会多出 36 个自由模块，数据码字整体错位，而矩阵看起来完全正常
    // （三个定位角、时序线、格式信息全对），只有真解码才会发现扫不出来。
    // v81 就是这么翻车的：短串测试全过（V5 不需要版本信息），真实 URL 是 V8 才暴露。
    {
        int freeModules = 0;
        for (int y = 0; y < best->size; ++y) {
            for (int x = 0; x < best->size; ++x) {
                if (!best->res[y][x]) ++freeModules;
            }
        }
        const int expect = totalCw * 8 + remainderBits(version);
        if (freeModules != expect) {
            qWarning("QrCodeGen: module count mismatch v%d free=%d expect=%d "
                     "(missing function pattern?)", version, freeModules, expect);
        }
    }

    const int sz = best->size;
    QByteArray out(sz * sz, 0);
    for (int y = 0; y < sz; ++y) {
        for (int x = 0; x < sz; ++x) {
            out[y * sz + x] = (char)best->m[y][x];
        }
    }
    delete best;
    if (outSize) *outSize = sz;
    qDebug("QrCodeGen: v%d ecc=%c mask=%d size=%d len=%d",
           version, ecc, bestMask, sz, len);
    return out;
}

bool QrCodeGen::generate(const QString& text, char ecc, const QString& path, int scale)
{
    int size = 0;
    const QByteArray mat = generateMatrix(text, ecc, &size);
    if (mat.isEmpty() || size <= 0) return false;

    // 四周留 4 个模块的白边（QR 规范要求静区），否则扫码器容易识别不到边界
    const int quiet = 4;

    // scale <= 0 表示自动：整图控制在约 560px（BB10 屏宽 720，设置页左右各 20du
    // 内边距，可用 680），同时保证每个模块不小于 6px —— 模块太小手机对不上焦。
    if (scale <= 0) {
        scale = 560 / (size + quiet * 2);
        if (scale < 6) scale = 6;
        if (scale > 12) scale = 12;
    }
    const int dim = (size + quiet * 2) * scale;

    QImage img(dim, dim, QImage::Format_RGB32);
    if (img.isNull()) return false;
    img.fill(0xFFFFFFFF);   // 白底

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (mat.at(y * size + x) == 0) continue;
            const int px = (x + quiet) * scale;
            const int py = (y + quiet) * scale;
            for (int dy = 0; dy < scale; ++dy) {
                QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(py + dy));
                for (int dx = 0; dx < scale; ++dx) {
                    line[px + dx] = 0xFF000000;
                }
            }
        }
    }

    if (!img.save(path, "PNG")) {
        qWarning("QrCodeGen: save PNG failed -> %s", qPrintable(path));
        return false;
    }
    qDebug("QrCodeGen: saved %s (%dx%d)", qPrintable(path), dim, dim);
    return true;
}
