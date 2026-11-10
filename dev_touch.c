/*
 * BB10 真机触摸注入器 v3（root 专用，不进 BAR）。
 *
 * v2 实证：事件被 screen 丢弃（display 可唤醒、app 无 ROOT DOWN）。
 * v3 依据 screen.h「Event Types: MTOUCH_*」官方属性全集重写：
 *   BUTTONS / DEVICE / KEY_MODIFIERS / POSITION / SEQUENCE_ID / SIZE /
 *   SOURCE_POSITION / SOURCE_SIZE / TOUCH_ID / TOUCH_ORIENTATION /
 *   TOUCH_PRESSURE / TOUCH_TYPE
 *   - 补 v2 漏掉的 SIZE（接触面大小）与 TOUCH_TYPE=FINGER / TOUCH_ORIENTATION=0
 *     / TOUCH_PRESSURE / KEY_MODIFIERS=0；
 *   - SEQUENCE_ID 同一手势（TOUCH/MOVE/RELEASE）内固定，不再逐事件自增；
 *   - SOURCE_SIZE 用设备分辨率（触摸屏全分辨率），与 POSITION 同坐标系。
 *
 * 诊断能力：
 *   list            —— 打印各 display 的 SIZE/IDLE_STATE 与各 device 的 TYPE，
 *                      用于挑选正确的 disp/dev 组合；
 *   idle            —— 只读 display[0] 的 idle 状态；
 *   tap/swipe        —— 注入前后打印 idle 状态，若注入后 1→0 说明事件
 *                      已被 screen 输入管线接受（可把问题定位到 app 侧路由）。
 *
 * 用法：
 *   dev_touch list
 *   dev_touch idle
 *   dev_touch [dispIdx] [devIdx|-] tap   x y [holdMs]
 *   dev_touch [dispIdx] [devIdx|-] swipe x1 y1 x2 y2 [ms]
 */
#include <screen/screen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#define FAIL(step) do { fprintf(stderr, "FAIL %s rc=%d errno=%d (%s)\n", \
                        step, rc, errno, strerror(errno)); return 2; } while (0)

static int get_idle(screen_display_t disp, int *idle)
{
    int rc = screen_get_display_property_iv(disp, SCREEN_PROPERTY_IDLE_STATE, idle);
    return rc;
}

static int send_touch(screen_context_t ctx, screen_display_t disp,
                      screen_device_t dev, int type,
                      int x, int y, int touchId, int seq)
{
    int rc;
    screen_event_t ev = 0;
    int pos[2]      = {x, y};
    int srcPos[2]   = {x, y};
    int srcSize[2]  = {720, 720};   /* 触摸屏全分辨率，与 POSITION 同坐标系 */
    int size[2]     = {24, 24};     /* 接触面 ~24x24 像素 */
    int btn = 0, mods = 0, orient = 0, pressure = 200, touchType = SCREEN_TOUCH_FINGER;

    rc = screen_create_event(&ev);
    if (rc) FAIL("create_event");
    rc = screen_set_event_property_iv(ev, SCREEN_PROPERTY_TYPE, &type);
    if (rc) FAIL("set TYPE");
    rc = screen_set_event_property_iv(ev, SCREEN_PROPERTY_TOUCH_ID, &touchId);
    if (rc) FAIL("set TOUCH_ID");
    rc = screen_set_event_property_iv(ev, SCREEN_PROPERTY_SEQUENCE_ID, &seq);
    if (rc) FAIL("set SEQUENCE_ID");
    rc = screen_set_event_property_iv(ev, SCREEN_PROPERTY_POSITION, pos);
    if (rc) FAIL("set POSITION");
    rc = screen_set_event_property_iv(ev, SCREEN_PROPERTY_SOURCE_POSITION, srcPos);
    if (rc) FAIL("set SOURCE_POSITION");
    rc = screen_set_event_property_iv(ev, SCREEN_PROPERTY_SOURCE_SIZE, srcSize);
    if (rc) FAIL("set SOURCE_SIZE");
    rc = screen_set_event_property_iv(ev, SCREEN_PROPERTY_SIZE, size);
    if (rc) FAIL("set SIZE");
    rc = screen_set_event_property_iv(ev, SCREEN_PROPERTY_BUTTONS, &btn);
    if (rc) FAIL("set BUTTONS");
    rc = screen_set_event_property_iv(ev, SCREEN_PROPERTY_KEY_MODIFIERS, &mods);
    if (rc) FAIL("set KEY_MODIFIERS");
    rc = screen_set_event_property_iv(ev, SCREEN_PROPERTY_TOUCH_ORIENTATION, &orient);
    if (rc) FAIL("set TOUCH_ORIENTATION");
    rc = screen_set_event_property_iv(ev, SCREEN_PROPERTY_TOUCH_PRESSURE, &pressure);
    if (rc) FAIL("set TOUCH_PRESSURE");
    rc = screen_set_event_property_iv(ev, SCREEN_PROPERTY_TOUCH_TYPE, &touchType);
    if (rc) FAIL("set TOUCH_TYPE");
    if (dev) {
        rc = screen_set_event_property_pv(ev, SCREEN_PROPERTY_DEVICE, (void **)&dev);
        if (rc) FAIL("set DEVICE");
    }
    rc = screen_inject_event(disp, ev);
    if (rc) FAIL("inject_event");
    screen_destroy_event(ev);
    return 0;
}

int main(int argc, char **argv)
{
    screen_context_t ctx = 0;
    screen_display_t *disps = 0;
    screen_device_t *devs = 0;
    int rc, ndisp = 0, ndev = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: dev_touch list | idle | [dispIdx] [devIdx|-] tap x y [ms] | [dispIdx] [devIdx|-] swipe x1 y1 x2 y2 [ms]\n");
        return 1;
    }

    rc = screen_create_context(&ctx, SCREEN_INPUT_PROVIDER_CONTEXT);
    if (rc) FAIL("create_context(INPUT_PROVIDER)");

    rc = screen_get_context_property_iv(ctx, SCREEN_PROPERTY_DISPLAY_COUNT, &ndisp);
    if (rc || ndisp < 1) FAIL("get DISPLAY_COUNT");
    disps = (screen_display_t *)calloc(ndisp, sizeof(screen_display_t));
    rc = screen_get_context_property_pv(ctx, SCREEN_PROPERTY_DISPLAYS, (void **)disps);
    if (rc) FAIL("get DISPLAYS");

    rc = screen_get_context_property_iv(ctx, SCREEN_PROPERTY_DEVICE_COUNT, &ndev);
    if (rc == 0 && ndev > 0) {
        devs = (screen_device_t *)calloc(ndev, sizeof(screen_device_t));
        rc = screen_get_context_property_pv(ctx, SCREEN_PROPERTY_DEVICES, (void **)devs);
        if (rc) FAIL("get DEVICES");
    }

    printf("displays=%d devices=%d\n", ndisp, ndev);
    for (int i = 0; i < ndisp; ++i) {
        int sz[2] = {0, 0}, idle = -1;
        screen_get_display_property_iv(disps[i], SCREEN_PROPERTY_SIZE, sz);
        rc = get_idle(disps[i], &idle);
        printf("  disp[%d]=%p size=%dx%d idleState=%d(rc=%d)\n",
               i, (void *)disps[i], sz[0], sz[1], idle, rc);
    }
    for (int i = 0; i < ndev; ++i) {
        int t = -1;
        screen_get_device_property_iv(devs[i], SCREEN_PROPERTY_TYPE, &t);
        printf("  dev[%d]=%p type=%d\n", i, (void *)devs[i], t);
    }

    if (strcmp(argv[1], "list") == 0) {
        screen_destroy_context(ctx);
        return 0;
    }
    if (strcmp(argv[1], "idle") == 0) {
        screen_destroy_context(ctx);
        return 0;
    }

    /* 前两个参数可缺省：若 argv[1] 不是整数则视为 mode，disp/dev 取默认 */
    int argi = 1;
    int dispIdx = 0, devIdx = -1;
    char *endp = 0;
    long v = strtol(argv[1], &endp, 10);
    if (endp && *endp == 0 && argc > 2 && strcmp(argv[2], "tap") != 0 && strcmp(argv[2], "swipe") != 0) {
        dispIdx = (int)v;
        argi = 2;
        if (argc <= argi) { fprintf(stderr, "expect mode after dispIdx\n"); return 1; }
        v = strtol(argv[argi], &endp, 10);
        if (endp && *endp == 0) { devIdx = (int)v; ++argi; }
    }
    if (dispIdx < 0 || dispIdx >= ndisp) {
        fprintf(stderr, "bad dispIdx %d (0..%d)\n", dispIdx, ndisp - 1);
        return 1;
    }
    screen_display_t disp = disps[dispIdx];
    screen_device_t dev = (devIdx >= 0 && devIdx < ndev) ? devs[devIdx] : 0;

    const char *mode = argv[argi++];
    int isSwipe = (strcmp(mode, "swipe") == 0);
    if (!isSwipe && strcmp(mode, "tap") != 0) {
        fprintf(stderr, "bad mode %s (tap|swipe)\n", mode);
        return 1;
    }
    int nCoord = isSwipe ? 4 : 2;
    if (argc < argi + nCoord) {
        fprintf(stderr, "usage: dev_touch [d] [dev|-] %s ...\n", mode);
        return 1;
    }
    int x1 = atoi(argv[argi]), y1 = atoi(argv[argi + 1]);
    int x2 = isSwipe ? atoi(argv[argi + 2]) : x1;
    int y2 = isSwipe ? atoi(argv[argi + 3]) : y1;
    int dur = argc >= argi + nCoord + 1 ? atoi(argv[argi + nCoord]) : 0;
    if (dur <= 0) dur = isSwipe ? 300 : 80;
    if (dur > 5000) dur = 5000;

    int idleBefore = -1;
    get_idle(disp, &idleBefore);

    /* 同一手势全程共用同一 SEQUENCE_ID（TOUCH/MOVE/RELEASE 配对） */
    int seq = 1;
    rc = send_touch(ctx, disp, dev, SCREEN_EVENT_MTOUCH_TOUCH, x1, y1, 0, seq);
    if (rc) FAIL("send TOUCH");
    if (isSwipe) {
        int steps = dur / 16;
        if (steps < 2) steps = 2;
        if (steps > 64) steps = 64;
        for (int i = 1; i <= steps; ++i) {
            int xi = x1 + (x2 - x1) * i / steps;
            int yi = y1 + (y2 - y1) * i / steps;
            usleep(16000);
            rc = send_touch(ctx, disp, dev, SCREEN_EVENT_MTOUCH_MOVE, xi, yi, 0, seq);
            if (rc) FAIL("send MOVE");
        }
    } else {
        usleep(dur * 1000);
    }
    rc = send_touch(ctx, disp, dev, SCREEN_EVENT_MTOUCH_RELEASE, x2, y2, 0, seq);
    if (rc) FAIL("send RELEASE");

    usleep(100000);
    int idleAfter = -1;
    get_idle(disp, &idleAfter);

    printf("OK %s (%d,%d)->(%d,%d) dur=%dms disp=%d dev=%d idle %d->%d\n",
           mode, x1, y1, x2, y2, dur, dispIdx, devIdx, idleBefore, idleAfter);
    screen_destroy_context(ctx);
    free(disps);
    return 0;
}
