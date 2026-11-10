/*
 * BB10 抓帧权限边界探测 + 抓帧工具 v3（devuser 可跑；证据链判决用）。
 *
 * 背景：v2（dev_capfmt）用 SCREEN_DISPLAY_MANAGER_CONTEXT 创建 context，
 * devuser 下直接 EPERM。本工具按「权限逐级降级」顺序尝试，把每一级的
 * 成败与 errno 打印出来，用来判定 devuser 到底能读到哪一层：
 *
 *   A. screen_create_context(MANAGER)                 —— 预期 EPERM
 *   B. screen_create_context(0) 普通应用 context
 *      B1. SCREEN_PROPERTY_DISPLAY_COUNT / DISPLAYS / SIZE
 *      B2. screen_read_display 整屏读（普通 context 也能读吗？）
 *      B3. display 的 SCREEN_PROPERTY_WINDOWS → screen_read_window
 *
 * 若 B2 成：写 4-int 头（w,h,stride,fmt）+ 逐行 w*4 的 RAW（与 v2 同款），
 * host 侧 seeraw.py / rawone.py 直接可解。
 *
 * 编译（host）：
 *   QNX_HOST=D:/bbndk/host_10_1_0_132/win32/x86 \
 *   QNX_TARGET=D:/bbndk/target_10_1_0_1020/qnx6 \
 *   $QNX_HOST/usr/bin/arm-unknown-nto-qnx8.0.0eabi-gcc.exe \
 *     -I $QNX_TARGET/usr/include -L $QNX_TARGET/armle-v7/usr/lib \
 *     -o dev_shot dev_shot.c -lscreen
 */
#include <screen/screen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

static void dump_win(const char *tag, screen_context_t ctx, screen_window_t w, const char *out);

int main(int argc, char **argv)
{
    const char *out = (argc > 1) ? argv[1] : "/accounts/1000/shared/documents/devshot.raw";
    screen_context_t ctx = 0;
    screen_display_t *disps = 0;
    screen_pixmap_t pix = 0;
    screen_buffer_t buf = 0;
    char *ptr = 0;
    int rc, ndisp = 0, size[2] = {0, 0}, stride = 0, y;
    int dispFmt = -1, bufFmt = -1;
    int fmt = SCREEN_FORMAT_RGBA8888;
    int usage = SCREEN_USAGE_NATIVE | SCREEN_USAGE_READ | SCREEN_USAGE_WRITE;
    int bsz[2];
    FILE *f;
    int hdr[4];

    printf("uid=%d euid=%d pid=%d\n", getuid(), geteuid(), getpid());

    /* ---- A: manager context（预期失败，仅留痕） ---- */
    rc = screen_create_context(&ctx, SCREEN_DISPLAY_MANAGER_CONTEXT);
    printf("A manager ctx rc=%d errno=%d(%s)\n", rc, errno, strerror(errno));
    if (!rc) screen_destroy_context(ctx);
    ctx = 0;

    /* ---- B: 普通应用 context ---- */
    rc = screen_create_context(&ctx, 0);
    printf("B plain ctx rc=%d errno=%d(%s)\n", rc, errno, strerror(errno));
    if (rc) return 2;

    rc = screen_get_context_property_iv(ctx, SCREEN_PROPERTY_DISPLAY_COUNT, &ndisp);
    printf("B1 DISPLAY_COUNT rc=%d n=%d\n", rc, ndisp);
    if (rc || ndisp < 1) { printf("ERR B1 no displays\n"); return 3; }
    disps = (screen_display_t *)calloc(ndisp, sizeof(screen_display_t));
    rc = screen_get_context_property_pv(ctx, SCREEN_PROPERTY_DISPLAYS, (void **)disps);
    printf("B1 DISPLAYS rc=%d\n", rc);
    if (rc) return 3;

    rc = screen_get_display_property_iv(disps[0], SCREEN_PROPERTY_SIZE, size);
    {
        int f2 = screen_get_display_property_iv(disps[0], SCREEN_PROPERTY_FORMAT, &dispFmt);
        printf("B1 SIZE rc=%d %dx%d / FORMAT rc=%d fmt=%d\n",
               rc, size[0], size[1], f2, dispFmt);
    }
    if (rc || size[0] <= 0 || size[1] <= 0) return 3;

    /* ---- B2: 整屏读 ---- */
    rc = screen_create_pixmap(&pix, ctx);
    if (rc) { printf("ERR create_pixmap rc=%d\n", rc); return 4; }
    rc = screen_set_pixmap_property_iv(pix, SCREEN_PROPERTY_FORMAT, &fmt);
    rc |= screen_set_pixmap_property_iv(pix, SCREEN_PROPERTY_USAGE, &usage);
    bsz[0] = size[0]; bsz[1] = size[1];
    rc |= screen_set_pixmap_property_iv(pix, SCREEN_PROPERTY_BUFFER_SIZE, bsz);
    rc |= screen_create_pixmap_buffer(pix);
    if (rc) { printf("ERR pixmap setup rc=%d errno=%d(%s)\n", rc, errno, strerror(errno)); return 4; }
    rc = screen_get_pixmap_property_pv(pix, SCREEN_PROPERTY_RENDER_BUFFERS, (void **)&buf);
    if (rc || !buf) { printf("ERR RENDER_BUFFERS rc=%d\n", rc); return 4; }

    rc = screen_read_display(disps[0], buf, 0, 0, 0);
    printf("B2 screen_read_display rc=%d errno=%d(%s)\n", rc, errno, strerror(errno));

    if (!rc) {
        rc = screen_get_buffer_property_pv(buf, SCREEN_PROPERTY_POINTER, (void **)&ptr);
        rc |= screen_get_buffer_property_iv(buf, SCREEN_PROPERTY_STRIDE, &stride);
        rc |= screen_get_buffer_property_iv(buf, SCREEN_PROPERTY_FORMAT, &bufFmt);
        printf("B2 buffer stride=%d bufFmt=%d (req %d)\n", stride, bufFmt, fmt);
        if (rc || !ptr || stride <= 0) { printf("ERR buffer props\n"); return 5; }

        f = fopen(out, "wb");
        if (!f) { printf("ERR fopen %s errno=%d(%s)\n", out, errno, strerror(errno)); return 6; }
        hdr[0] = size[0]; hdr[1] = size[1]; hdr[2] = stride; hdr[3] = bufFmt;
        fwrite(hdr, sizeof(int), 4, f);
        for (y = 0; y < size[1]; ++y)
            fwrite(ptr + (size_t)y * stride, 1, (size_t)size[0] * 4, f);
        fclose(f);
        printf("OK %dx%d stride=%d fmt=%d bytes=%ld out=%s\n",
               size[0], size[1], stride, bufFmt, (long)(16 + (long)size[1] * size[0] * 4), out);
        screen_destroy_pixmap(pix);
        screen_destroy_context(ctx);
        free(disps);
        return 0;
    }

    /* ---- B3: 逐窗口读 ---- */
    {
        screen_window_t *wins = 0;
        int nwin = 0;
        rc = screen_get_context_property_pv(ctx, SCREEN_PROPERTY_DISPLAYS, (void **)disps);
        rc = screen_get_display_property_pv(disps[0], SCREEN_PROPERTY_WINDOWS, (void **)&wins);
        printf("B3 display WINDOWS rc=%d\n", rc);
        if (!rc && wins) {
            int i;
            for (i = 0; i < 64; ++i) {
                int psz[2] = {0, 0};
                char grp[128] = {0};
                rc = screen_get_window_property_iv(wins[i], SCREEN_PROPERTY_SIZE, psz);
                if (rc || psz[0] <= 0) break;
                rc = screen_get_window_property_cv(wins[i], SCREEN_PROPERTY_GROUP, sizeof(grp), grp);
                printf("B3 win[%d] size=%dx%d group=%s rc=%d\n", i, psz[0], psz[1], grp, rc);
                dump_win("B3", ctx, wins[i], out);
            }
            printf("B3 total windows seen=%d\n", i);
        }
    }

    screen_destroy_pixmap(pix);
    screen_destroy_context(ctx);
    free(disps);
    return 0;
}

static void dump_win(const char *tag, screen_context_t ctx, screen_window_t w, const char *out)
{
    screen_pixmap_t p = 0;
    screen_buffer_t b = 0;
    char *ptr = 0;
    int rc, stride = 0, wpx[2] = {0, 0}, fmt = -1, y;
    int reqfmt = SCREEN_FORMAT_RGBA8888;
    int usage = SCREEN_USAGE_NATIVE | SCREEN_USAGE_READ | SCREEN_USAGE_WRITE;
    const int rect[5] = {0, 0, 0, 0, -1};
    FILE *f;
    int hdr[4];

    rc = screen_get_window_property_iv(w, SCREEN_PROPERTY_SIZE, wpx);
    if (rc || wpx[0] <= 0 || wpx[1] <= 0) { printf("%s no SIZE rc=%d\n", tag, rc); return; }

    rc = screen_create_pixmap(&p, ctx);
    if (rc) { printf("%s create_pixmap rc=%d\n", tag, rc); return; }
    rc = screen_set_pixmap_property_iv(p, SCREEN_PROPERTY_FORMAT, &reqfmt);
    rc |= screen_set_pixmap_property_iv(p, SCREEN_PROPERTY_USAGE, &usage);
    rc |= screen_set_pixmap_property_iv(p, SCREEN_PROPERTY_BUFFER_SIZE, wpx);
    rc |= screen_create_pixmap_buffer(p);
    if (rc) { printf("%s pixmap setup rc=%d errno=%d(%s)\n", tag, rc, errno, strerror(errno)); return; }
    rc = screen_get_pixmap_property_pv(p, SCREEN_PROPERTY_RENDER_BUFFERS, (void **)&b);
    if (rc || !b) { printf("%s RENDER_BUFFERS rc=%d\n", tag, rc); return; }

    rc = screen_read_window(w, b, 1, rect, 0);
    printf("%s screen_read_window rc=%d errno=%d(%s)\n", tag, rc, errno, strerror(errno));
    if (rc) { screen_destroy_pixmap(p); return; }
    rc = screen_get_buffer_property_pv(b, SCREEN_PROPERTY_POINTER, (void **)&ptr);
    rc |= screen_get_buffer_property_iv(b, SCREEN_PROPERTY_STRIDE, &stride);
    rc |= screen_get_buffer_property_iv(b, SCREEN_PROPERTY_FORMAT, &fmt);
    printf("%s winbuf size=%dx%d stride=%d fmt=%d rc=%d\n", tag, wpx[0], wpx[1], stride, fmt, rc);
    if (rc || !ptr || stride <= 0) return;
    f = fopen(out, "wb");
    if (!f) return;
    hdr[0] = wpx[0]; hdr[1] = wpx[1]; hdr[2] = stride; hdr[3] = fmt;
    fwrite(hdr, sizeof(int), 4, f);
    for (y = 0; y < wpx[1]; ++y)
        fwrite(ptr + (size_t)y * stride, 1, (size_t)wpx[0] * 4, f);
    fclose(f);
    printf("%s WROTE %s %dx%d stride=%d fmt=%d bytes=%ld\n", tag, out, wpx[0], wpx[1], stride, fmt,
           (long)(16 + (long)wpx[1] * wpx[0] * 4));
}
