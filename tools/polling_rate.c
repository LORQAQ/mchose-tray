/*
 * polling_rate.c — 用 Windows Raw Input 实测鼠标回报率（物理测量，非读配置）
 * ---------------------------------------------------------------------------
 * 为什么需要它：`12 67` 读到的只是设备**存储的配置档位**，不是真实上报频率。
 * 本工具通过 Raw Input (RIDEV_INPUTSINK) 直接统计鼠标上报到系统的报文条数与间隔，
 * 从而独立验证配置值是否与实际一致。
 *
 * 已知上限：WM_INPUT 的投递速率受本进程消息循环速度限制（约每秒数千条），
 * 因此对 4000/8000Hz 只能给出"≥ 本进程上限"的下界，对 125~2000Hz 是准确的。
 *
 * 编译： gcc -O2 -Wall -o polling_rate.exe polling_rate.c -luser32
 * 用法： polling_rate.exe [测量秒数=10]
 *       运行后请**持续移动鼠标**。
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static LARGE_INTEGER g_freq;
static int      g_count = 0;
static ULONGLONG g_first = 0, g_last = 0;
static ULONGLONG g_minGap = 0;
static int      g_hist[400];      /* 0.1ms 一档，最多 40ms */
static int      g_overflow = 0;
static char     g_device[512] = {0};

static ULONGLONG Now(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (ULONGLONG)c.QuadPart;
}

static double ToMs(ULONGLONG ticks)
{
    return (double)ticks * 1000.0 / (double)g_freq.QuadPart;
}

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_INPUT) {
        UINT size = 0;
        GetRawInputData((HRAWINPUT)l, RID_INPUT, NULL, &size, sizeof(RAWINPUTHEADER));
        if (size > 0 && size <= 4096) {
            BYTE buf[4096];
            if (GetRawInputData((HRAWINPUT)l, RID_INPUT, buf, &size,
                                sizeof(RAWINPUTHEADER)) == size) {
                RAWINPUT *ri = (RAWINPUT *)buf;
                if (ri->header.dwType == RIM_TYPEMOUSE) {
                    ULONGLONG t = Now();
                    if (g_count == 0) {
                        g_first = t;
                    } else {
                        ULONGLONG gap = t - g_last;
                        if (g_minGap == 0 || gap < g_minGap) g_minGap = gap;
                        int bucket = (int)(ToMs(gap) * 10.0);
                        if (bucket >= 0 && bucket < 400) g_hist[bucket]++;
                        else g_overflow++;
                    }
                    g_last = t;
                    g_count++;

                    if (g_device[0] == 0) {
                        UINT n = sizeof(g_device);
                        GetRawInputDeviceInfoA(ri->header.hDevice, RIDI_DEVICENAME,
                                               g_device, &n);
                    }
                }
            }
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int main(int argc, char **argv)
{
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, NULL, _IONBF, 0);

    int seconds = 8;          /* 检测到移动后的测量时长 */
    int waitSeconds = 60;     /* 等待第一次移动的上限 */
    if (argc > 1) { int v = atoi(argv[1]); if (v > 0) seconds = v; }
    if (argc > 2) { int v = atoi(argv[2]); if (v > 0) waitSeconds = v; }

    QueryPerformanceFrequency(&g_freq);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"PollRateMeasureWnd";
    if (!RegisterClassExW(&wc)) {
        printf("RegisterClassExW 失败: %lu\n", GetLastError());
        return 1;
    }

    HWND h = CreateWindowExW(0, L"PollRateMeasureWnd", L"", WS_POPUP,
                             0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    if (h == NULL) { printf("CreateWindowExW 失败: %lu\n", GetLastError()); return 1; }

    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01;         /* Generic Desktop */
    rid.usUsage     = 0x02;         /* Mouse */
    rid.dwFlags     = RIDEV_INPUTSINK;   /* 后台也能收到，不需要窗口在前台 */
    rid.hwndTarget  = h;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        printf("RegisterRawInputDevices 失败: %lu\n", GetLastError());
        return 1;
    }

    printf("=== 鼠标回报率物理测量 ===\n");
    printf(">>> 请现在开始【持续移动鼠标】（画圈或来回移动）<<<\n");
    printf("检测到第一次移动后开始计时，测量 %d 秒；最多等待 %d 秒。\n\n",
           seconds, waitSeconds);

    /* 阶段一：等第一次移动（避免"还没开始动就已经测完"） */
    DWORD waitStart = GetTickCount();
    while (g_count == 0 && (int)(GetTickCount() - waitStart) < waitSeconds * 1000) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        SwitchToThread();
    }

    if (g_count == 0) {
        /*
         * 没等到真实移动。必须区分两种原因：
         *   (a) 本进程收不到 WM_INPUT（沙箱/桌面隔离）→ 测量方法不可用
         *   (b) 只是鼠标没动 → 测量方法可用
         * 用 SendInput 注入一小段位移：注入输入同样经过 Raw Input 投递。
         * 若注入后仍 0 条，即判定为 (a)。
         */
        printf("等待 %d 秒仍未检测到鼠标移动，正在做注入自检以判断原因...\n", waitSeconds);

        g_count = 0;
        g_first = g_last = 0;
        int injected = 0;
        for (int i = 0; i < 400; i++) {
            INPUT in;
            memset(&in, 0, sizeof(in));
            in.type = INPUT_MOUSE;
            in.mi.dx = (i % 2) ? 2 : -2;
            in.mi.dy = (i % 3) ? 1 : -1;
            in.mi.dwFlags = MOUSEEVENTF_MOVE;
            if (SendInput(1, &in, sizeof(in)) == 1) injected++;

            MSG msg;
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(2);
        }
        DWORD drain = GetTickCount();
        while ((int)(GetTickCount() - drain) < 500) {
            MSG msg;
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            SwitchToThread();
        }

        printf("\n--- 注入自检 ---\n");
        printf("SendInput 注入次数 : %d\n", injected);
        printf("注入后 WM_INPUT 数 : %d 条\n", g_count);
        if (g_count > 0) {
            printf("结论: Raw Input 通道正常 —— 之前 0 条是真的没有鼠标移动。\n");
            printf("      请重新运行本工具并持续移动鼠标。\n");
        } else {
            printf("结论: **本进程收不到鼠标 Raw Input**（连注入输入也收不到）。\n");
            printf("      说明执行环境把该进程与交互桌面隔离了，\n");
            printf("      本工具在当前环境下无法物理测量回报率。\n");
            printf("      请在普通用户会话（自己开 CMD/PowerShell）中运行本工具。\n");
        }
        return 3;
    }

    /* 阶段二：从第一次移动开始计时 */
    printf("[已检测到移动，开始计时]\n");
    ULONGLONG measureStart = Now();
    while ((int)ToMs(Now() - measureStart) < seconds * 1000) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        SwitchToThread();
    }

    printf("--- 结果 ---\n");
    printf("原始报文条数 : %d\n", g_count);
    if (g_device[0]) printf("设备路径     : %s\n", g_device);

    if (g_count < 2) {
        printf("未检测到足够的鼠标移动。请重新运行并持续移动鼠标。\n");
        return 2;
    }

    double activeMs = ToMs(g_last - g_first);
    double rate = (double)(g_count - 1) * 1000.0 / activeMs;
    printf("有效活动时长 : %.1f ms\n", activeMs);
    printf("平均上报频率 : %.0f Hz\n", rate);
    printf("最小报文间隔 : %.3f ms  (对应瞬时 %.0f Hz)\n",
           ToMs(g_minGap), 1000.0 / ToMs(g_minGap));

    /* 间隔分布的峰值与中位数：更能反映设备稳定上报间隔 */
    long long total = 0;
    for (int i = 0; i < 400; i++) total += g_hist[i];
    if (total > 0) {
        long long acc = 0;
        int median = 0;
        for (int i = 0; i < 400; i++) {
            acc += g_hist[i];
            if (acc * 2 >= total) { median = i; break; }
        }
        int peak = 0;
        for (int i = 1; i < 400; i++) if (g_hist[i] > g_hist[peak]) peak = i;
        printf("间隔中位数   : %.2f ms  (约 %.0f Hz)\n", median / 10.0, 10000.0 / (median ? median : 1));
        printf("间隔众数     : %.2f ms  (约 %.0f Hz)\n", peak / 10.0, 10000.0 / (peak ? peak : 1));
        printf("众数档计数   : %d 条\n", g_hist[peak]);
    }

    printf("\n--- 常见档位对照 ---\n");
    static const int kRates[] = { 125, 250, 500, 1000, 2000, 4000, 8000 };
    for (int i = 0; i < 7; i++) {
        double period = 1000.0 / kRates[i];
        printf("  %5d Hz -> 周期 %.3f ms\n", kRates[i], period);
    }
    printf("\n注：本进程消息循环约有每秒数千条的投递上限，"
           "对 4000/8000Hz 只能给出下界。\n");
    return 0;
}
