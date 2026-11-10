/*
 * BB10 真机抓帧取证工具 v2（只 root shell 跑，不进 BAR、不进产品代码）。
 *
 * 为什么有 v2：v1（dev_screencap.c / 设备上的 dexdump）留下的 RAW 让
 * host 侧读出「byte2 恒定 0xfc、byte0/1/3 携带灰度内容」的怪图案，无法
 * 区分是「App 真的退化成彩噪」还是「screen_read_display 没做格式转换」。
 * v2 直接把三件事打印出来：
 *   1. 显示器原生格式（screen_get_display_property_iv SCREEN_PROPERTY_FORMAT）
 *   2. 显式申请 RGBA8888 后，screen_read_display 交给我们的 buffer 的
 *      真实 STRIDE / FORMAT（转换失败时这里会露馅）
 *   3. 像素数据本体（md5 + 逐行 w*4）
 *
 * 编译（宿主）：
 *   ntoarmv7-gcc -I <NDK>/target_10_1_0_1020/qnx6/usr/include \
 *                -L <NDK>/target_10_1_0_1020/qnx6/armle-v7/usr/lib \
 *                -o dev_capfmt dev_capfmt.c -lscreen
 */
#include <screen/screen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define FAIL(step) do { fprintf(stderr, "FAIL %s rc=%d errno=%d (%s)\n", \
                        step, rc, errno, strerror(errno)); return 2; } while (0)

int main(int argc, char **argv)
{
    const char *out = (argc > 1) ? argv[1] : "/var/tmp/bb10_capfmt.raw";
    screen_context_t ctx = 0;
    screen_display_t *disps = 0;
    screen_pixmap_t pix = 0;
    screen_buffer_t buf = 0;
    char *ptr = 0;
    int rc, ndisp = 0, size[2] = {0, 0}, stride = 0, y;
    int dispFmt = -1;
    int bufFmt = -1;
    int fmt = SCREEN_FORMAT_RGBA8888;
    int usage = SCREEN_USAGE_NATIVE | SCREEN_USAGE_READ | SCREEN_USAGE_WRITE;
    int bsz[2];
    FILE *f;
    int hdr[4];

    rc = screen_create_context(&ctx, SCREEN_DISPLAY_MANAGER_CONTEXT);
    if (rc) FAIL("screen_create_context(DISPLAY_MANAGER)");

    rc = screen_get_context_property_iv(ctx, SCREEN_PROPERTY_DISPLAY_COUNT, &ndisp);
    if (rc || ndisp < 1) FAIL("get DISPLAY_COUNT");
    disps = (screen_display_t *)calloc(ndisp, sizeof(screen_display_t));
    rc = screen_get_context_property_pv(ctx, SCREEN_PROPERTY_DISPLAYS, (void **)disps);
    if (rc) FAIL("get DISPLAYS");

    rc = screen_get_display_property_iv(disps[0], SCREEN_PROPERTY_SIZE, size);
    if (rc || size[0] <= 0 || size[1] <= 0) FAIL("get display SIZE");
    rc = screen_get_display_property_iv(disps[0], SCREEN_PROPERTY_FORMAT, &dispFmt);
    fprintf(stdout, "display size=%dx%d nativeFmt=%d\n", size[0], size[1], dispFmt);

    rc = screen_create_pixmap(&pix, ctx);
    if (rc) FAIL("create_pixmap");
    rc = screen_set_pixmap_property_iv(pix, SCREEN_PROPERTY_FORMAT, &fmt);
    if (rc) FAIL("set FORMAT");
    rc = screen_set_pixmap_property_iv(pix, SCREEN_PROPERTY_USAGE, &usage);
    if (rc) FAIL("set USAGE");
    bsz[0] = size[0]; bsz[1] = size[1];
    rc = screen_set_pixmap_property_iv(pix, SCREEN_PROPERTY_BUFFER_SIZE, bsz);
    if (rc) FAIL("set BUFFER_SIZE");
    rc = screen_create_pixmap_buffer(pix);
    if (rc) FAIL("create_pixmap_buffer");
    rc = screen_get_pixmap_property_pv(pix, SCREEN_PROPERTY_RENDER_BUFFERS, (void **)&buf);
    if (rc || !buf) FAIL("get RENDER_BUFFERS");

    /* count=0 + read_rects=NULL => 整屏 */
    rc = screen_read_display(disps[0], buf, 0, 0, 0);
    if (rc) FAIL("screen_read_display");

    rc = screen_get_buffer_property_pv(buf, SCREEN_PROPERTY_POINTER, (void **)&ptr);
    if (rc || !ptr) FAIL("get buffer POINTER");
    rc = screen_get_buffer_property_iv(buf, SCREEN_PROPERTY_STRIDE, &stride);
    if (rc || stride <= 0) FAIL("get buffer STRIDE");
    rc = screen_get_buffer_property_iv(buf, SCREEN_PROPERTY_FORMAT, &bufFmt);
    fprintf(stdout, "buffer stride=%d bufFmt=%d (requested %d)\n", stride, bufFmt, fmt);

    f = fopen(out, "wb");
    if (!f) { fprintf(stderr, "FAIL fopen %s errno=%d\n", out, errno); return 3; }
    /* v1 同款 4-int 头，host 侧按它解析；不再用 md5 头（来源不明的 dexdump 用的） */
    hdr[0] = size[0]; hdr[1] = size[1]; hdr[2] = stride; hdr[3] = fmt;
    fwrite(hdr, sizeof(int), 4, f);
    for (y = 0; y < size[1]; ++y)
        fwrite(ptr + (size_t)y * stride, 1, (size_t)size[0] * 4, f);
    fclose(f);
    fprintf(stdout, "OK %dx%d stride=%d fmt=%d bytes=%ld out=%s\n",
            size[0], size[1], stride, fmt, (long)(16 + size[1] * size[0] * 4), out);

    screen_destroy_pixmap(pix);
    screen_destroy_context(ctx);
    free(disps);
    return 0;
}
