/*
 * BB10 真机抓帧取证工具（只在 root shell 下跑，不进 BAR、不进产品代码）。
 *
 * 为什么需要它：blackberry-deploy / BarDeploy.jar 没有截图命令，设备上的
 * /usr/bin/snapshot 只是 viewport 演示程序。但 QNX Screen 提供了
 * screen_read_display()，只要用 SCREEN_DISPLAY_MANAGER_CONTEXT（特权上下文，
 * root 才有）就能把整块显示读进一个 NATIVE buffer。
 *
 * 输出是一个极简 RAW：4 个 int 头 [w,h,stride,format] + 逐行 RGBA8888 像素，
 * 由 host 侧 Python 转成 PNG。之所以不在设备上编 PNG，是为了少链一个库、少一个
 * 变量——抓帧失败时能从 RAW 大小一眼看出来卡在哪一步。
 *
 * 编译（宿主）：
 *   ntoarmv7-gcc -I <NDK>/target_10_1_0_1020/qnx6/usr/include \
 *                -L <NDK>/target_10_1_0_1020/qnx6/armle-v7/usr/lib \
 *                -o dev_screencap dev_screencap.c -lscreen
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
    const char *out = (argc > 1) ? argv[1] : "/tmp/bb10_screen.raw";
    screen_context_t ctx = 0;
    screen_display_t *disps = 0;
    screen_pixmap_t pix = 0;
    screen_buffer_t buf = 0;
    char *ptr = 0;
    int rc, ndisp = 0, size[2] = {0, 0}, stride = 0, y;
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

    f = fopen(out, "wb");
    if (!f) { fprintf(stderr, "FAIL fopen %s errno=%d\n", out, errno); return 3; }
    hdr[0] = size[0]; hdr[1] = size[1]; hdr[2] = stride; hdr[3] = fmt;
    fwrite(hdr, sizeof(int), 4, f);
    for (y = 0; y < size[1]; ++y)
        fwrite(ptr + (size_t)y * stride, 1, (size_t)size[0] * 4, f);
    fclose(f);
    fprintf(stdout, "OK %dx%d stride=%d fmt=%d out=%s\n",
            size[0], size[1], stride, fmt, out);

    screen_destroy_pixmap(pix);
    screen_destroy_context(ctx);
    free(disps);
    return 0;
}
