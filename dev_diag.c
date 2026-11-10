/*
 * BB10 真机取证工具（抓帧 + 手势注入合一，只在 root shell 下跑，不进 BAR）。
 *
 * 【为什么要合并成一个文件 / 原地覆盖】
 *   BB10 禁止执行「新建」的可执行文件：scp/cp 出来的新 inode 即使 chmod 755
 *   也报 `Operation not permitted`（getfacl 与老文件完全一致，说明不是 ACL）。
 *   只有早先被登记为可信的那个 inode 能跑。因此必须用
 *       ssh bb10 "cat > /accounts/1000/shared/misc/dev_screencap" < dev_diag
 *   **原地覆盖**（O_TRUNC，inode 不变）来升级工具，绝不能 scp 到新文件名。
 *
 * 用法（兼容老调用）：
 *   dev_screencap <out.raw>                      # 等价于 cap
 *   dev_screencap cap <out.raw>
 *   dev_screencap swipe <x0> <y0> <x1> <y1> [steps] [delay_ms] [mtouch|pointer]
 *   例：上滑一屏  dev_screencap swipe 360 600 360 120 16 25
 *
 * 编译（宿主）：
 *   export QNX_HOST=.../host_10_1_0_132/win32/x86
 *   export QNX_TARGET=.../target_10_1_0_1020/qnx6
 *   ntoarmv7-gcc -O2 -o dev_diag dev_diag.c -lscreen
 */
#include <screen/screen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

static void fail(const char *step, int rc)
{
    fprintf(stderr, "FAIL %s rc=%d errno=%d (%s)\n", step, rc, errno, strerror(errno));
}

static int set1(screen_event_t ev, int pname, int v)
{
    return screen_set_event_property_iv(ev, pname, &v);
}

static int set2(screen_event_t ev, int pname, int a, int b)
{
    int v[2];
    v[0] = a; v[1] = b;
    return screen_set_event_property_iv(ev, pname, v);
}

/* ---------------- 抓帧 ---------------- */
static int do_cap(const char *out)
{
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
    if (rc) { fail("create_context(DISPLAY_MANAGER)", rc); return 2; }
    rc = screen_get_context_property_iv(ctx, SCREEN_PROPERTY_DISPLAY_COUNT, &ndisp);
    if (rc || ndisp < 1) { fail("get DISPLAY_COUNT", rc); return 3; }
    disps = (screen_display_t *)calloc(ndisp, sizeof(screen_display_t));
    rc = screen_get_context_property_pv(ctx, SCREEN_PROPERTY_DISPLAYS, (void **)disps);
    if (rc) { fail("get DISPLAYS", rc); return 4; }
    rc = screen_get_display_property_iv(disps[0], SCREEN_PROPERTY_SIZE, size);
    if (rc || size[0] <= 0) { fail("get display SIZE", rc); return 5; }

    rc = screen_create_pixmap(&pix, ctx);
    if (rc) { fail("create_pixmap", rc); return 6; }
    rc = screen_set_pixmap_property_iv(pix, SCREEN_PROPERTY_FORMAT, &fmt);
    if (rc) { fail("set FORMAT", rc); return 7; }
    rc = screen_set_pixmap_property_iv(pix, SCREEN_PROPERTY_USAGE, &usage);
    if (rc) { fail("set USAGE", rc); return 8; }
    bsz[0] = size[0]; bsz[1] = size[1];
    rc = screen_set_pixmap_property_iv(pix, SCREEN_PROPERTY_BUFFER_SIZE, bsz);
    if (rc) { fail("set BUFFER_SIZE", rc); return 9; }
    rc = screen_create_pixmap_buffer(pix);
    if (rc) { fail("create_pixmap_buffer", rc); return 10; }
    rc = screen_get_pixmap_property_pv(pix, SCREEN_PROPERTY_RENDER_BUFFERS, (void **)&buf);
    if (rc || !buf) { fail("get RENDER_BUFFERS", rc); return 11; }
    rc = screen_read_display(disps[0], buf, 0, 0, 0);
    if (rc) { fail("screen_read_display", rc); return 12; }
    rc = screen_get_buffer_property_pv(buf, SCREEN_PROPERTY_POINTER, (void **)&ptr);
    if (rc || !ptr) { fail("get buffer POINTER", rc); return 13; }
    rc = screen_get_buffer_property_iv(buf, SCREEN_PROPERTY_STRIDE, &stride);
    if (rc || stride <= 0) { fail("get buffer STRIDE", rc); return 14; }

    f = fopen(out, "wb");
    if (!f) { fprintf(stderr, "FAIL fopen %s errno=%d\n", out, errno); return 15; }
    hdr[0] = size[0]; hdr[1] = size[1]; hdr[2] = stride; hdr[3] = fmt;
    fwrite(hdr, sizeof(int), 4, f);
    for (y = 0; y < size[1]; ++y)
        fwrite(ptr + (size_t)y * stride, 1, (size_t)size[0] * 4, f);
    fclose(f);
    fprintf(stdout, "OK %dx%d stride=%d fmt=%d out=%s\n", size[0], size[1], stride, fmt, out);

    screen_destroy_pixmap(pix);
    screen_destroy_context(ctx);
    free(disps);
    return 0;
}

/* ---------------- 手势注入 ---------------- */
static int do_swipe(int argc, char **argv, int argi)
{
    int x0 = 360, y0 = 600, x1 = 360, y1 = 120;
    int steps = 16, delay_ms = 25;
    const char *mode = "mtouch";
    screen_context_t ctx = 0;
    screen_display_t *disps = 0;
    screen_event_t ev = 0;
    int rc, ndisp = 0, i, x, y, type, seq = 1;
    int is_pointer = 0, ok = 0, bad = 0;

    if (argc > argi)     x0 = atoi(argv[argi++]);
    if (argc > argi)     y0 = atoi(argv[argi++]);
    if (argc > argi)     x1 = atoi(argv[argi++]);
    if (argc > argi)     y1 = atoi(argv[argi++]);
    if (argc > argi)     steps = atoi(argv[argi++]);
    if (argc > argi)     delay_ms = atoi(argv[argi++]);
    if (argc > argi)     mode = argv[argi++];
    if (steps < 2) steps = 2;
    if (delay_ms < 0) delay_ms = 0;
    is_pointer = (strcmp(mode, "pointer") == 0);

    rc = screen_create_context(&ctx, SCREEN_INPUT_PROVIDER_CONTEXT);
    if (rc) { fail("create_context(INPUT_PROVIDER)", rc); return 2; }
    rc = screen_get_context_property_iv(ctx, SCREEN_PROPERTY_DISPLAY_COUNT, &ndisp);
    if (rc || ndisp < 1) { fail("get DISPLAY_COUNT", rc); return 3; }
    disps = (screen_display_t *)calloc(ndisp, sizeof(screen_display_t));
    rc = screen_get_context_property_pv(ctx, SCREEN_PROPERTY_DISPLAYS, (void **)disps);
    if (rc || !disps[0]) { fail("get DISPLAYS", rc); return 4; }
    rc = screen_create_event(&ev);
    if (rc || !ev) { fail("create_event", rc); return 5; }

    for (i = 0; i < steps; ++i) {
        x = x0 + (x1 - x0) * i / (steps - 1);
        y = y0 + (y1 - y0) * i / (steps - 1);
        if (is_pointer) {
            type = SCREEN_EVENT_POINTER;
        } else {
            type = (i == 0) ? SCREEN_EVENT_MTOUCH_TOUCH
                 : (i == steps - 1) ? SCREEN_EVENT_MTOUCH_RELEASE
                                    : SCREEN_EVENT_MTOUCH_MOVE;
        }
        set1(ev, SCREEN_PROPERTY_TYPE, type);
        set2(ev, SCREEN_PROPERTY_POSITION, x, y);
        set2(ev, SCREEN_PROPERTY_SOURCE_POSITION, x, y);
        if (is_pointer) {
            set1(ev, SCREEN_PROPERTY_BUTTONS,
                 (i == steps - 1) ? 0 : SCREEN_LEFT_MOUSE_BUTTON);
        } else {
            set2(ev, SCREEN_PROPERTY_SOURCE_SIZE, 1, 1);
            set1(ev, SCREEN_PROPERTY_TOUCH_ID, 0);
            set1(ev, SCREEN_PROPERTY_SEQUENCE_ID, seq);
            set1(ev, SCREEN_PROPERTY_TOUCH_PRESSURE, 100);
            set1(ev, SCREEN_PROPERTY_TOUCH_ORIENTATION, 0);
            set1(ev, SCREEN_PROPERTY_BUTTONS, (i == steps - 1) ? 0 : 1);
        }
        rc = screen_inject_event(disps[0], ev);
        if (rc) {
            bad++;
            fprintf(stderr, "INJECT i=%d type=%d xy=(%d,%d) rc=%d errno=%d (%s)\n",
                    i, type, x, y, rc, errno, strerror(errno));
        } else {
            ok++;
        }
        if (delay_ms > 0) usleep((useconds_t)delay_ms * 1000);
    }
    fprintf(stdout, "OK mode=%s (%d,%d)->(%d,%d) steps=%d injected=%d failed=%d\n",
            mode, x0, y0, x1, y1, steps, ok, bad);
    screen_destroy_event(ev);
    screen_destroy_context(ctx);
    free(disps);
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: dev_screencap <out.raw> | cap <out.raw> | swipe x0 y0 x1 y1 [steps] [delay_ms] [mode]\n");
        return 1;
    }
    if (strcmp(argv[1], "cap") == 0) {
        return do_cap(argc > 2 ? argv[2] : "/tmp/bb10_screen.raw");
    }
    if (strcmp(argv[1], "swipe") == 0) {
        return do_swipe(argc, argv, 2);
    }
    /* 兼容老调用：第一个参数就是输出路径 */
    return do_cap(argv[1]);
}
