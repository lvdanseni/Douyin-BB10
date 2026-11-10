#ifndef QRCODEGEN_HPP
#define QRCODEGEN_HPP

#include <QString>
#include <QImage>

/**
 * 极简 QR 编码器 —— 只做 B 站扫码登录需要的那一档能力：
 *   · byte mode（UTF-8 字节流）
 *   · 版本 1~10 自适应（21x21 ~ 57x57）
 *   · 纠错等级 L / M
 *
 * 为什么自己写：BB10 上没有任何可用的二维码库，而扫码登录必须把
 * passport.bilibili.com 给的 url 画成二维码。移植第三方库（nayuki 等）也是
 * 几百行，不如按标准自己实现一遍，顺便只保留 V1~V10 的表格（40 个版本的
 * 完整表有上千行，这里用不到）。
 *
 * 【正确性验证方式】本机没有 C++ 编译器，所以先用 Python 写了**同一套算法**
 * （BiliBar/qrtest.py），和 Python qrcode 库逐位对照：
 *   · 交织后的 codewords（数据 + RS 纠错）完全一致
 *   · 矩阵在同一个 mask 下逐格完全一致（差异 0）
 * 确认算法无误后才翻译成下面的 C++。若将来要改算法，请先用 qrtest.py 验证。
 *
 * 两个曾经踩过的坑（都在 Python 阶段被抓住）：
 *   1) GF(256) 乘法每轮必须截断到 8 位，否则高位累积会让 `z >> 7` 判错，
 *      表现为"数据 codewords 全对、ECC 全错"。
 *   2) 生成多项式内部按升幂累乘，返回前必须 reverse 成降幂，否则
 *      rs_remainder 取到的是反过来的系数，同样是 ECC 全错。
 */
class QrCodeGen
{
public:
    /**
     * 生成二维码并保存为 PNG。
     *
     * @param text    要编码的内容（UTF-8）
     * @param ecc     'L' 或 'M'（其它值按 'M' 处理）
     * @param path    输出 PNG 的绝对路径
     * @param scale   每个模块放大成多少像素（整图 = (size + 8) * scale，8 是四周留白）
     * @return true = 生成并保存成功
     */
    static bool generate(const QString& text, char ecc, const QString& path, int scale = 8);

    /** 只生成矩阵（调试/测试用），返回 size*size 的 0/1 数组，size 由 outSize 带回。 */
    static QByteArray generateMatrix(const QString& text, char ecc, int* outSize);
};

#endif // QRCODEGEN_HPP
