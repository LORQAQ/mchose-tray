/*
 * main.cpp — mchose-tray 宿主进程
 * ---------------------------------------------------------------------------
 * 参考 Iris-0109/rapoo-tray（MIT）的总体结构：一个隐藏的消息窗口 + 托盘图标 +
 * 后台 HID 工作线程 + 状态回调驱动 UI。协议层与设备层为本项目独立实现。
 *
 * 编译见 build.bat；协议细节见 docs/PROTOCOL.md。
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "device_manager.h"
#include "tray_ui.h"

#define WM_APP_TRAY  (WM_APP + 1)
#define WM_APP_STATE (WM_APP + 2)

#define IDM_OSD      1300
#define IDM_STYLE    1301
#define IDM_AUTORUN  1302
#define IDM_EXIT     1303
#define IDM_REFRESH  1304
#define IDF_RATE     1400
#define IDM_RATE(i)  (1500 + (i))          /* 1500..1505 */
#define IDM_SLEEP_OFF 1600
#define IDM_SLEEP(i) (1601 + (i))          /* 1601..1606（下标，不是分钟数） */
#define IDM_DPI(s)   (1700 + (s))          /* 1700..1705 */

/* 注意：这些区间必须互不重叠。曾因 IDM_SLEEP 用「分钟数」编码（1602/1604/…/1661）
 * 且处理分支写成 cmd >= 1601 && cmd < 1801，把 1700..1705 的 DPI 档位也吞掉，
 * 导致点「DPI 第 N 档」实际下发了 100 分钟左右的休眠设置。 */
static_assert(IDM_SLEEP(5) < IDM_DPI(0), "sleep and dpi menu id ranges must not overlap");
static_assert(IDM_RATE(5) < IDM_SLEEP_OFF, "rate and sleep menu id ranges must not overlap");
/* 关键防线：处理分支实际使用的区间也必须互不重叠
 * （原缺陷正是 1601+200 的宽区间吞掉了 1700..1705 的 DPI 档位） */
static_assert(IDM_SLEEP(0) + 6 <= IDM_DPI(0), "sleep handler range must not reach dpi ids");

namespace {

/* 日志绝对路径：exe 同目录。用绝对路径是为了不依赖工作目录
 * （从 explorer 启动时 CWD 可能是别处，相对路径会让日志"消失"）。 */
const wchar_t *GuiLogPath()
{
    static wchar_t path[MAX_PATH] = {0};
    if (path[0] == L'\0') {
        wchar_t exe[MAX_PATH] = {0};
        DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) { wcscpy(exe, L".\\mchose-tray.exe"); }
        wchar_t *slash = wcsrchr(exe, L'\\');
        if (slash != NULL) *(slash + 1) = L'\0';
        else               exe[0] = L'\0';
        swprintf(path, MAX_PATH, L"%lsmchose-tray-gui.log", exe);
    }
    return path;
}

/* GUI 模式下的启动日志（-mwindows 无控制台，排错用） */
void GuiLog(const char *msg)
{
    HANDLE h = CreateFileW(GuiLogPath(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    char line[256];
    int n = snprintf(line, sizeof(line), "%s\n", msg);
    /* snprintf 返回的是"本应写入的长度"，截断时会大于缓冲区；
     * 直接当长度用会越界读栈。这里必须夹紧并始终以真实长度写入。 */
    if (n < 0) { CloseHandle(h); return; }
    size_t len = ((size_t)n >= sizeof(line)) ? (sizeof(line) - 1) : (size_t)n;
    DWORD w = 0;
    WriteFile(h, line, (DWORD)len, &w, NULL);
    CloseHandle(h);
}

/*
 * 安全追加格式化文本。
 * 不能写成 `off += snprintf(out + off, sizeof(out) - off, ...)`：
 * 一旦 off 超过 sizeof(out)，`sizeof(out) - off` 会因无符号提升变成约 2^64，
 * 等于把天文数字的长度交给 snprintf → 栈越界。这里显式夹紧并保证 NUL 结尾。
 */
void AppendFmt(char *buf, size_t cap, size_t &off, const char *fmt, ...)
{
    if (buf == NULL || cap == 0 || off >= cap - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, cap - off, fmt, ap);
    va_end(ap);
    if (n < 0) { buf[off] = '\0'; return; }
    size_t room = cap - off - 1;
    off += ((size_t)n > room) ? room : (size_t)n;
    buf[off] = '\0';
}

/* ---------------------------------------------------------------- 图标预览
 *
 * 托盘图标只有 16×16，数字很容易糊掉或被裁切，而开发者未必能看到真实托盘。
 * 这里把图标按 scale 倍最近邻放大、合成到一张图纸上导出 BMP，
 * 便于用图片查看器（或转成 PNG 后）肉眼核对渲染效果。
 */
bool WriteBmp24(const wchar_t *path, const unsigned char *bgrTopDown, int w, int h)
{
    const int rowRaw = w * 3;
    const int rowPad = (rowRaw + 3) & ~3;
    const DWORD pixBytes = (DWORD)rowPad * (DWORD)h;
    const DWORD fileBytes = 14 + 40 + pixBytes;

    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;

    unsigned char fh[14] = { 'B', 'M', 0 };
    unsigned char ih[40] = {0};
#define PUT32(p, v) do { (p)[0]=(unsigned char)(v); (p)[1]=(unsigned char)((unsigned)(v)>>8); \
                         (p)[2]=(unsigned char)((unsigned)(v)>>16); (p)[3]=(unsigned char)((unsigned)(v)>>24); } while (0)
    PUT32(fh + 2, fileBytes);
    PUT32(fh + 10, 14 + 40);
    PUT32(ih + 0, 40);
    PUT32(ih + 4, (DWORD)w);
    PUT32(ih + 8, (DWORD)h);
    ih[12] = 1; ih[13] = 0;              /* planes = 1 */
    ih[14] = 24; ih[15] = 0;             /* bpp = 24 */
    PUT32(ih + 20, pixBytes);
#undef PUT32

    DWORD wr = 0;
    WriteFile(f, fh, 14, &wr, NULL);
    WriteFile(f, ih, 40, &wr, NULL);

    unsigned char *row = (unsigned char *)malloc((size_t)rowPad);
    for (int y = h - 1; y >= 0; y--) {          /* BMP 自下而上 */
        memset(row, 0, (size_t)rowPad);
        for (int x = 0; x < w; x++) {
            const unsigned char *s = bgrTopDown + ((size_t)y * w + x) * 3;
            row[x * 3 + 0] = s[0];
            row[x * 3 + 1] = s[1];
            row[x * 3 + 2] = s[2];
        }
        WriteFile(f, row, (DWORD)rowPad, &wr, NULL);
    }
    free(row);
    CloseHandle(f);
    return true;
}

/* 把一个 HICON 放大画进图纸格子；浅灰底便于分辨透明区域 */
void BlitIconToSheet(HDC hdc, HICON icon, int cellX, int cellY, int scale)
{
    if (icon == NULL) return;
    ICONINFO ii;
    if (!GetIconInfo(icon, &ii)) return;
    BITMAP bm;
    if (GetObject(ii.hbmColor, sizeof(bm), &bm) != sizeof(bm)) {
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        return;
    }
    int w = bm.bmWidth, h = bm.bmHeight;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    unsigned char *px = (unsigned char *)malloc((size_t)w * h * 4);
    HDC screen = GetDC(NULL);
    GetDIBits(screen, ii.hbmColor, 0, h, px, &bi, DIB_RGB_COLORS);
    ReleaseDC(NULL, screen);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const unsigned char *s = px + ((size_t)y * w + x) * 4;
            unsigned char b = s[0], g = s[1], r = s[2], a = s[3];
            const unsigned char bgc = 0xE8;
            unsigned char ob  = (unsigned char)((b * a + bgc * (255 - a)) / 255);
            unsigned char og  = (unsigned char)((g * a + bgc * (255 - a)) / 255);
            unsigned char orr = (unsigned char)((r * a + bgc * (255 - a)) / 255);
            RECT rc;
            rc.left   = cellX + x * scale;
            rc.top    = cellY + y * scale;
            rc.right  = rc.left + scale;
            rc.bottom = rc.top + scale;
            HBRUSH br = CreateSolidBrush(RGB(orr, og, ob));
            FillRect(hdc, &rc, br);
            DeleteObject(br);
        }
    }
    free(px);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
}

/* 生成图标预览图纸：行 = 样式（0 胶囊 / 1 数字），列 = 各种电量与状态 */
int IconPreview(const wchar_t *outPath)
{
    struct Case { int battery; bool charging, connected, linked, valid; const char *tag; };
    const Case cases[] = {
        { 5,   false, true,  true,  true,  "b5"     },
        { 20,  false, true,  true,  true,  "b20"    },
        { 42,  false, true,  true,  true,  "b42"    },
        { 70,  false, true,  true,  true,  "b70"    },
        { 100, false, true,  true,  true,  "b100"   },
        { 70,  true,  true,  true,  true,  "charge" },
        { 0,   false, false, false, false, "offline"},
        { 0,   false, true,  false, false, "sleep"  },
        { 0,   false, true,  true,  false, "noread" },
    };
    const int nCases = (int)(sizeof(cases) / sizeof(cases[0]));
    const int scale = 8, gap = 4, pad = 6;

    /* 覆盖常见托盘图标尺寸：100% = 16，125% = 20，150% = 24，200% = 32 */
    const int kSizes[] = { 16, 20, 24, 32 };
    const int nSizes = (int)(sizeof(kSizes) / sizeof(kSizes[0]));

    const int cellSize = kSizes[nSizes - 1];      /* 格子按最大尺寸留足空间 */
    int size = GetSystemMetrics(SM_CXSMICON);
    if (size < 16) size = 16;
    if (size > 64) size = 64;

    int cellW  = cellSize * scale;
    int sheetW = pad * 2 + nCases * (cellW + gap);
    int sheetH = pad * 2 + (nSizes * 2) * (cellW + gap);

    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);
    if (mem == NULL) { ReleaseDC(NULL, screen); printf("CreateCompatibleDC 失败\n"); return 1; }
    HBITMAP sheet = CreateCompatibleBitmap(screen, sheetW, sheetH);
    if (sheet == NULL) { DeleteDC(mem); ReleaseDC(NULL, screen); printf("CreateCompatibleBitmap 失败\n"); return 1; }
    HGDIOBJ old = SelectObject(mem, sheet);
    RECT all = { 0, 0, sheetW, sheetH };
    HBRUSH bg = CreateSolidBrush(RGB(0xE8, 0xE8, 0xE8));
    FillRect(mem, &all, bg);
    DeleteObject(bg);

    printf("=== 图标预览 ===\n");
    printf("当前系统托盘图标尺寸 SM_CXSMICON = %d，放大 %d 倍\n", size, scale);
    printf("列顺序: ");
    for (int i = 0; i < nCases; i++) printf("%s ", cases[i].tag);
    printf("\n行顺序（每行 = 一种托盘尺寸 × 两种样式）:\n");
    for (int s = 0; s < nSizes; s++) {
        printf("  第 %d 行: %2d px 胶囊   第 %d 行: %2d px 数字\n",
               s * 2 + 1, kSizes[s], s * 2 + 2, kSizes[s]);
    }
    printf("\n");

    for (int s = 0; s < nSizes; s++) {
        for (int style = 0; style < 2; style++) {
            int row = s * 2 + style;
            for (int i = 0; i < nCases; i++) {
                Device::State st;
                st.connected   = cases[i].connected;
                st.mouseLinked = cases[i].linked;
                st.batteryValid = cases[i].valid;
                st.battery     = cases[i].battery;
                st.charging    = cases[i].charging;
                st.connectMode = 1;
                wcscpy(st.modelName, L"A7 Pro");
                wcscpy(st.firmware, L"5.4.7.4");

                /* 强制指定尺寸并放大，用来核对高 DPI 下的排版是否符合预期 */
                HICON ic = TrayUi::CreateBatteryIconSized(st, style, kSizes[s]);
                BlitIconToSheet(mem, ic, pad + i * (cellW + gap),
                                pad + row * (cellW + gap), scale);
                if (ic) DestroyIcon(ic);
            }
        }
    }

    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = sheetW;
    bi.bmiHeader.biHeight = -sheetH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;

    int rowPad = (sheetW * 3 + 3) & ~3;
    unsigned char *pix = (unsigned char *)malloc((size_t)rowPad * sheetH);
    memset(pix, 0, (size_t)rowPad * sheetH);
    int got = GetDIBits(mem, sheet, 0, sheetH, pix, &bi, DIB_RGB_COLORS);

    SelectObject(mem, old);
    DeleteObject(sheet);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);

    bool written = false;
    if (got) written = WriteBmp24(outPath, pix, sheetW, sheetH);
    free(pix);

    if (written) printf("已导出: %ls  (%dx%d)\n", outPath, sheetW, sheetH);
    else         printf("导出失败\n");
    return written ? 0 : 1;
}

/*
 * 查询本进程的完整性级别（RID）。
 *
 * 为什么需要它：托盘图标是通过向 explorer 的通知区域窗口发送消息实现的。
 * 若本进程的完整性级别**低于** explorer，UIPI 会拦住该消息，
 * Shell_NotifyIconW(NIM_ADD) 就返回 ERROR_ACCESS_DENIED (5)，且不弹任何提示。
 * 典型诱因：exe 所在目录带有 "Mandatory Label\Low Mandatory Level" 标签
 * （某些沙箱/受限目录会给目录打可继承的 Low 标签，从那里启动的进程即为 Low）。
 */
DWORD GetOwnIntegrityRid()
{
    HANDLE tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return 0;
    DWORD sz = 0;
    GetTokenInformation(tok, TokenIntegrityLevel, NULL, 0, &sz);
    TOKEN_MANDATORY_LABEL *tml = (TOKEN_MANDATORY_LABEL *)malloc(sz ? sz : 64);
    DWORD rid = 0;
    if (tml != NULL && GetTokenInformation(tok, TokenIntegrityLevel, tml, sz, &sz)) {
        rid = *GetSidSubAuthority(tml->Label.Sid,
                (DWORD)(*GetSidSubAuthorityCount(tml->Label.Sid) - 1));
    }
    free(tml);
    CloseHandle(tok);
    return rid;
}

/* NIM_ADD 失败时给出可操作的提示（而不是静默无图标） */
void ExplainTrayFailure(DWORD err, DWORD ownIl)
{
    if (err == ERROR_ACCESS_DENIED) {
        wchar_t msg[1024];
        swprintf(msg, 1024,
            L"托盘图标注册失败（错误 5：拒绝访问）。\n\n"
            L"原因：本程序的完整性级别低于 explorer，Windows 的 UIPI 阻止了\n"
            L"向任务栏发送消息。托盘图标正是通过这条消息注册的。\n\n"
            L"本进程完整性级别：0x%04lX（explorer 通常为 0x2000 Medium）\n"
            L"程序路径：%ls\n\n"
            L"常见原因是 exe 所在目录带有“Low Mandatory Level”完整性标签——\n"
            L"从这类目录（如某些沙箱/受限工作区）启动的进程会被强制降为 Low。\n\n"
            L"解决办法：把 mchose-tray.exe 复制到一个普通目录再运行，例如\n"
            L"    %%LOCALAPPDATA%%\\Programs\\mchose-tray\\\n"
            L"然后运行该副本（不要从原目录启动）。",
            (unsigned long)ownIl, L"(见启动日志 mchose-tray-gui.log)");
        MessageBoxW(NULL, msg, L"mchose-tray：无法显示托盘图标",
                    MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    } else {
        wchar_t msg[256];
        swprintf(msg, 256, L"托盘图标注册失败，错误码 %lu。", err);
        MessageBoxW(NULL, msg, L"mchose-tray", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    }
}

/* 当前进程的 GDI 对象数（flag: 0=GDI, 1=USER）。用于泄漏验证。 */
DWORD GuiObjectCount(DWORD flag)
{
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32 == NULL) return 0;
    typedef DWORD (WINAPI *PFN_GetGuiResources)(HANDLE, DWORD);
    PFN_GetGuiResources fn =
        (PFN_GetGuiResources)(void *)GetProcAddress(u32, "GetGuiResources");
    return fn ? fn(GetCurrentProcess(), flag) : 0;
}

/*
 * --icon-stress：图标绘制路径的 GDI 泄漏压力测试。
 *
 * 托盘程序最容易泄漏的就是 GDI 对象——每次状态变化都会 CreateDIBSection +
 * CreateIconIndirect，一旦漏掉 DeleteObject/DestroyIcon，长时间运行后
 * GDI 句柄耗尽（默认上限 10000），程序就会画不出东西。
 * 这里循环创建/销毁图标，对比前后的 GDI 计数；差值应为 0。
 */
/*
 * --help：打印全部命令行选项。
 *
 * 本程序是 GUI 子系统（-mwindows），从 cmd 运行时 stdout 虽可用但控制台不会等待，
 * 因此同时写一份 mchose-tray-help.txt，保证任何启动方式都能看到内容。
 */
void PrintUsage()
{
    static const char kHelp[] =
        "mchose-tray — 迈从（MCHOSE）鼠标托盘控制台\n"
        "\n"
        "不带参数运行：进入托盘模式（右下角显示电量图标）。\n"
        "本程序是 GUI 程序，不会弹出主窗口。\n"
        "\n"
        "诊断与自检选项：\n"
        "  --help                显示本帮助\n"
        "  --selftest            菜单分发 + 宽字符格式化自测（33 项，不需要设备）\n"
        "  --dump                读取完整状态并写入 mchose-tray-dump.txt\n"
        "                          退出码 0=完整 / 2=鼠标休眠 / 1=其它不完整\n"
        "  --model-scan          机型识别报告（型号名 / 接口 VID:PID / 匹配到的机型与来源）\n"
        "  --watch <秒>          观察 N 秒，验证设置未变时不会重复通知 UI\n"
        "  --set-rate <Hz>       下发回报率并做写后回读校验（125/500/1000/2000/4000/8000）\n"
        "  --set-dpi-stage <n>   切换 DPI 档位（n 从 0 开始）并做写后回读校验\n"
        "  --icon-preview <文件> 导出两种样式的图标预览图纸（16/20/24/32 px）\n"
        "  --icon-stress <n>     图标路径 GDI 泄漏压力测试（预热后测量，增量应为 0）\n"
        "\n"
        "常用排查：\n"
        "  右下角没有图标       先看程序目录下 mchose-tray-gui.log\n"
        "                       若为 err=5，说明 exe 所在目录带 Low 完整性标签，\n"
        "                       请用 deploy.bat 安装到普通目录后运行\n"
        "  提示\"鼠标休眠中\"     接收器正常，移动鼠标唤醒即可，无需重启程序\n"
        "  诊断读到全零         托盘程序与诊断不要同时运行（会争抢设备命令缓冲）\n"
        "\n"
        "文档：README.md（用法与性能）· docs/PROTOCOL.md（协议规格）\n";

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != NULL && hOut != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(hOut, kHelp, (DWORD)strlen(kHelp), &w, NULL);
    }
    HANDLE hf = CreateFileW(L"mchose-tray-help.txt", GENERIC_WRITE, 0, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(hf, kHelp, (DWORD)strlen(kHelp), &w, NULL);
        CloseHandle(hf);
    }
}
int IconStress(int iterations)
{
    printf("=== 图标路径 GDI 泄漏压力测试 ===\n");
    printf("迭代次数: %d（每次同时测两种样式）\n", iterations);

    /* 一批"创建+销毁两种样式"，抽出来便于先预热 */
    struct Batch {
        static void Run(int n, int &created)
        {
            for (int i = 0; i < n; i++) {
                Device::State st;
                st.connected    = (i % 7) != 0;
                st.mouseLinked  = (i % 5) != 0;
                st.batteryValid = (i % 3) != 0;
                st.battery      = i % 101;
                st.charging     = (i % 2) == 0;
                st.connectMode  = 1;
                wcscpy(st.modelName, L"A7 Pro");
                wcscpy(st.firmware, L"5.4.7.4");
                for (int style = 0; style < 2; style++) {
                    HICON ic = TrayUi::CreateBatteryIcon(st, style);
                    if (ic != NULL) { DestroyIcon(ic); created++; }
                }
            }
        }
    };

    /*
     * 必须先预热再测量。
     * GDI 在首次创建内存 DC / 字体时会做一次性惰性初始化——实测固定多出 6 个对象，
     * 且与迭代次数无关（10 / 100 / 1000 / 2000 次增量都恰好是 6）。
     * 不预热就会把这一次性开销误判成"泄漏"。
     */
    int warm = 0;
    Batch::Run(20, warm);
    printf("预热: 已创建并销毁 %d 个图标（排除 GDI 一次性初始化）\n", warm);

    DWORD gdiBefore = GuiObjectCount(0);
    DWORD usrBefore = GuiObjectCount(1);
    printf("起线: GDI=%lu  USER=%lu\n",
           (unsigned long)gdiBefore, (unsigned long)usrBefore);

    int created = 0;
    Batch::Run(iterations, created);

    DWORD gdiAfter = GuiObjectCount(0);
    DWORD usrAfter = GuiObjectCount(1);
    printf("创建并销毁图标: %d 个\n", created);
    printf("结束: GDI=%lu  USER=%lu\n",
           (unsigned long)gdiAfter, (unsigned long)usrAfter);

    long dGdi = (long)gdiAfter - (long)gdiBefore;
    long dUsr = (long)usrAfter - (long)usrBefore;
    printf("GDI 增量 = %ld   USER 增量 = %ld\n", dGdi, dUsr);
    printf("结论: %s\n", (dGdi == 0 && dUsr == 0)
           ? "无泄漏（图标路径的 GDI 对象全部释放）"
           : "存在泄漏，请检查图标绘制路径的 DeleteObject/DestroyIcon");
    return (dGdi == 0 && dUsr == 0) ? 0 : 1;
}

HWND       g_hMain = NULL;
NOTIFYICONDATAW g_nid;

/* 托盘外观缓存：避免外观未变时重建图标/刷新任务栏（省 CPU 与 GDI 句柄抖动） */
struct TrayVisualCache {
    bool    valid        = false;
    int     battery      = -1;
    bool    charging     = false;
    bool    connected    = false;
    bool    linked       = false;
    bool    batteryValid = false;
    int     style        = -1;
    int     size         = 0;
    wchar_t tip[128]     = {0};
};
TrayVisualCache g_cache;

HICON      g_hIcon = NULL;
UINT       g_wmTaskbarCreated = 0;
bool       g_osdOnConnect = true;

/* 休眠可选分钟数 */
const int kSleepChoices[] = { 1, 3, 5, 10, 30, 60 };
const int kSleepCount = 6;

/* 回报率档位（顺序必须与协议 RateIndexToHz 一致） */
const int kRateChoices[] = { 125, 500, 1000, 2000, 4000, 8000 };
const int kRateCount = 6;

/* 菜单命令 → 动作。抽成纯函数是为了可自测：
 * 原缺陷正是分发逻辑内联在窗口过程里、区间写成 1601+200，
 * 把 DPI 的 6 个 ID 全吞掉且无法被任何测试发现。 */
enum MenuAction {
    ACT_NONE, ACT_EXIT, ACT_OSD, ACT_STYLE, ACT_AUTORUN, ACT_REFRESH,
    ACT_SET_RATE, ACT_SLEEP_OFF, ACT_SET_SLEEP, ACT_SET_DPI
};

struct MenuDecision {
    MenuAction action;
    int        arg;      /* ACT_SET_RATE: 档位下标；ACT_SET_SLEEP: 分钟数；ACT_SET_DPI: 档位下标 */
};

MenuDecision DecodeMenuCommand(int cmd)
{
    MenuDecision d = { ACT_NONE, 0 };

    if (cmd == IDM_EXIT)    { d.action = ACT_EXIT;    return d; }
    if (cmd == IDM_OSD)     { d.action = ACT_OSD;     return d; }
    if (cmd == IDM_STYLE)   { d.action = ACT_STYLE;   return d; }
    if (cmd == IDM_AUTORUN) { d.action = ACT_AUTORUN; return d; }
    if (cmd == IDM_REFRESH) { d.action = ACT_REFRESH; return d; }

    if (cmd >= IDM_RATE(0) && cmd < IDM_RATE(0) + kRateCount) {
        d.action = ACT_SET_RATE;
        d.arg = cmd - IDM_RATE(0);
        return d;
    }
    if (cmd == IDM_SLEEP_OFF) { d.action = ACT_SLEEP_OFF; return d; }
    if (cmd >= IDM_SLEEP(0) && cmd < IDM_SLEEP(0) + kSleepCount) {
        d.action = ACT_SET_SLEEP;
        d.arg = kSleepChoices[cmd - IDM_SLEEP(0)];
        return d;
    }
    if (cmd >= IDM_DPI(0) && cmd < IDM_DPI(0) + 6) {
        d.action = ACT_SET_DPI;
        d.arg = cmd - IDM_DPI(0);
        return d;
    }
    return d;
}

void RefreshTray(bool showOsdIfNeeded, bool newConnection, const wchar_t *note)
{
    Device::State st = Device::GetState();
    const int style = TrayUi::GetBatteryStyle();
    const int size = GetSystemMetrics(SM_CXSMICON);

    /*
     * 缓存"影响外观的输入"。状态通知可能相当频繁（电量推送、周期读），
     * 而 CreateDIBSection + CreateIconIndirect + DestroyIcon 加一次任务栏刷新并不便宜。
     * 外观没变就完全不动 GDI、也不打扰 shell。
     */
    const bool visualChanged =
        !g_cache.valid ||
        g_cache.battery      != st.battery ||
        g_cache.charging     != st.charging ||
        g_cache.connected    != st.connected ||
        g_cache.linked       != st.mouseLinked ||
        g_cache.batteryValid != st.batteryValid ||
        g_cache.style        != style ||
        g_cache.size         != size;

    if (visualChanged) {
        HICON icon = TrayUi::CreateBatteryIcon(st, style);
        if (icon != NULL) {
            /* 顺序很重要：先让 shell 切到新图标，再销毁旧图标，
             * 否则 shell 会短暂持有已销毁的 HICON。 */
            HICON old = g_hIcon;
            g_hIcon = icon;
            g_nid.hIcon = icon;
            g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
            Shell_NotifyIconW(NIM_MODIFY, &g_nid);
            if (old != NULL) DestroyIcon(old);
        }
        g_cache.valid        = true;
        g_cache.battery      = st.battery;
        g_cache.charging     = st.charging;
        g_cache.connected    = st.connected;
        g_cache.linked       = st.mouseLinked;
        g_cache.batteryValid = st.batteryValid;
        g_cache.style        = style;
        g_cache.size         = size;
    }

    /* 提示文本：内容真的变了才刷新（wcscmp 比一次 NIM_MODIFY 便宜得多） */
    wchar_t tip[128] = {0};
    TrayUi::BuildTooltipText(st, tip, 128);
    if (wcscmp(tip, g_cache.tip) != 0) {
        wcsncpy(g_cache.tip, tip, 127);
        g_cache.tip[127] = L'\0';
        wcsncpy(g_nid.szTip, tip, 127);
        g_nid.szTip[127] = L'\0';
        g_nid.hIcon = g_hIcon;
        g_nid.uFlags = NIF_TIP | NIF_MESSAGE | (g_hIcon ? NIF_ICON : 0);
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    }

    if (g_osdOnConnect && (newConnection || showOsdIfNeeded || note != NULL))
        TrayUi::ShowOsd(st, note);
}

/* 运行在 HID 工作线程 */
void OnDeviceStateChanged(const Device::State &st, DWORD changeMask)
{
    (void)st;
    if (g_hMain == NULL) return;
    PostMessageW(g_hMain, WM_APP_STATE, (WPARAM)changeMask, 0);
}

void ShowMenu(HWND hWnd)
{
    Device::State st = Device::GetState();

    HMENU hMenu = CreatePopupMenu();
    HMENU hRate = CreatePopupMenu();
    HMENU hSleep = CreatePopupMenu();
    HMENU hDpi = CreatePopupMenu();

    wchar_t header[192];
    if (st.connected) {
        swprintf(header, 192, L"%ls   %d%%%ls",
                 st.modelName[0] ? st.modelName : L"MCHOSE 鼠标",
                 st.batteryValid ? st.battery : 0,
                 st.charging ? L" ⚡充电中" : L"");
    } else {
        wcscpy(header, L"MCHOSE 鼠标：未连接");
    }
    AppendMenuW(hMenu, MF_STRING | MF_DISABLED, 0, header);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    /* 回报率 */
    int rateCount = (st.rateCount > 0) ? st.rateCount : 6;
    static const int kRates[6] = { 125, 500, 1000, 2000, 4000, 8000 };
    for (int i = 0; i < rateCount && i < 6; i++) {
        wchar_t label[32];
        swprintf(label, 32, L"%d Hz", kRates[i]);
        UINT flags = MF_STRING;
        if (st.connected && st.rateIndex == i) flags |= MF_CHECKED;
        if (!st.connected) flags |= MF_GRAYED;
        AppendMenuW(hRate, flags, IDM_RATE(i), label);
    }
    AppendMenuW(hMenu, MF_POPUP | MF_STRING, (UINT_PTR)hRate, L"回报率");

    /* 休眠 */
    {
        UINT flags = MF_STRING | (st.connected ? 0 : MF_GRAYED);
        AppendMenuW(hSleep, flags, IDM_SLEEP_OFF, L"关闭休眠");
        for (int i = 0; i < kSleepCount; i++) {
            wchar_t label[32];
            swprintf(label, 32, L"%d 分钟", kSleepChoices[i]);
            UINT f = MF_STRING | (st.connected ? 0 : MF_GRAYED);
            if (st.connected && st.sleepMinutes == kSleepChoices[i]) f |= MF_CHECKED;
            AppendMenuW(hSleep, f, IDM_SLEEP(i), label);
        }
    }
    AppendMenuW(hMenu, MF_POPUP | MF_STRING, (UINT_PTR)hSleep, L"休眠时间");

    /* DPI 档位。
     * 档位数不再是固定 6：不同机型档位数不同（取自机型库，未知机型回落 6）。
     * 另外跳过档值为 0 的档——档值由设备回读而来，未使用的档位返回 0
     * （合法 DPI 不可能为 0，官方最小值为 100），这样界面能自动贴合真实档数。 */
    int stageCount = (st.modelDpiStages > 0 && st.modelDpiStages < 6) ? st.modelDpiStages : 6;
    int shownStages = 0;
    for (int i = 0; i < stageCount; i++) {
        if (st.settingsValid && st.dpi[i] == 0) continue;   /* 该档位不存在 */
        wchar_t label[48];
        if (st.settingsValid)
            swprintf(label, 48, L"第 %d 档 · %u DPI", i + 1, st.dpi[i]);
        else
            swprintf(label, 48, L"第 %d 档", i + 1);
        UINT flags = MF_STRING;
        if (st.settingsValid && st.dpiActiveIndex == i) flags |= MF_CHECKED;
        if (!st.connected || !st.settingsValid) flags |= MF_GRAYED;
        AppendMenuW(hDpi, flags, IDM_DPI(i), label);
        shownStages++;
    }
    if (shownStages == 0)
        AppendMenuW(hDpi, MF_STRING | MF_GRAYED, IDM_DPI(0), L"(档位未知)");
    AppendMenuW(hMenu, MF_POPUP | MF_STRING, (UINT_PTR)hDpi, L"DPI 档位");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_OSD, L"显示状态悬浮窗");
    AppendMenuW(hMenu, MF_STRING, IDM_REFRESH, L"立即刷新");
    AppendMenuW(hMenu, MF_STRING, IDM_STYLE, L"切换图标样式");
    AppendMenuW(hMenu, MF_STRING | (TrayUi::IsAutoRunEnabled() ? MF_CHECKED : 0),
                IDM_AUTORUN, L"开机自启");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"退出");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hWnd);
    int cmd = (int)TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                  pt.x, pt.y, 0, hWnd, NULL);
    PostMessageW(hWnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);

    if (cmd == 0) return;

    /* 分发走纯函数 DecodeMenuCommand，便于 --selftest 覆盖验证 */
    MenuDecision d = DecodeMenuCommand(cmd);
    switch (d.action) {
    case ACT_EXIT:
        DestroyWindow(hWnd);
        return;
    case ACT_OSD:
        TrayUi::ShowOsd(Device::GetState(), NULL);
        return;
    case ACT_STYLE:
        TrayUi::CycleBatteryStyle();
        RefreshTray(true, false, NULL);
        return;
    case ACT_AUTORUN:
        TrayUi::ToggleAutoRun();
        return;
    case ACT_REFRESH:
        Device::RequestRefresh();
        TrayUi::ShowOsd(Device::GetState(), NULL);
        return;
    case ACT_SET_RATE:
        if (d.arg >= 0 && d.arg < kRateCount) Device::RequestSetPollingRate(kRateChoices[d.arg]);
        return;
    case ACT_SLEEP_OFF:
        Device::RequestSetSleep(false, 0);
        return;
    case ACT_SET_SLEEP:
        Device::RequestSetSleep(true, d.arg);
        return;
    case ACT_SET_DPI:
        Device::RequestSwitchDpiStage(d.arg);
        return;
    case ACT_NONE:
    default:
        return;
    }
}

LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_APP_STATE: {
        DWORD mask = (DWORD)wp;
        bool newConn = (mask & Device::CHANGE_CONNECTED) != 0;

        /* 命令结果反馈：写命令失败必须让用户看见，不能静默吞掉。
         * 只在 CHANGE_SETTINGS 上取结果——否则电量推送等无关通知会把结果吃掉，
         * 或在错误的时机弹出提示。 */
        wchar_t noteBuf[128];
        const wchar_t *note = NULL;
        if ((mask & Device::CHANGE_SETTINGS) != 0) {
            int res = Device::TakeLastCommandResult();
            if (res == Device::CMD_RESULT_OK_VERIFIED) {
                wcscpy(noteBuf, L"✔ 设置已下发并回读校验通过");
                note = noteBuf;
            } else if (res == Device::CMD_RESULT_SENT_UNVERIFIED) {
                wcscpy(noteBuf, L"✔ 设置已下发（该命令不支持回读校验）");
                note = noteBuf;
            } else if (res == Device::CMD_RESULT_FAILED) {
                wcscpy(noteBuf, L"✘ 设置失败或被设备拒绝");
                note = noteBuf;
            }
        }

        RefreshTray((mask & Device::CHANGE_SETTINGS) != 0, newConn, note);
        return 0;
    }
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
            TrayUi::ShowOsd(Device::GetState(), NULL);
            Device::RequestRefresh();
            return 0;
        case WM_LBUTTONDBLCLK:
            TrayUi::CycleBatteryStyle();
            RefreshTray(true, false, NULL);
            return 0;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowMenu(hWnd);
            return 0;
        }
        return 0;

    case WM_COMMAND:
        break;

    case WM_DEVICECHANGE:
        Device::RequestRefresh();
        return 0;

    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        RefreshTray(false, false, NULL);
        return 0;

    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }

    if (msg == g_wmTaskbarCreated) {
        /*
         * explorer 重启后必须重新挂载托盘图标。
         *
         * 两个坑：
         * 1) 必须把外观缓存置为失效。否则 RefreshTray 会认为"外观没变"而跳过
         *    NIM_MODIFY，图标就再也不会回来（explorer 重启后图标永久消失）。
         * 2) NIM_ADD 前要重新填好 hIcon/uFlags，g_nid 里可能残留旧值。
         */
        g_cache.valid = false;
        g_cache.tip[0] = L'\0';
        g_nid.hIcon = g_hIcon;
        g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
        if (!Shell_NotifyIconW(NIM_ADD, &g_nid))
            GuiLog("[!] TaskbarCreated 后 NIM_ADD 失败，稍后由状态刷新重试");
        else
            GuiLog("[6] TaskbarCreated：托盘图标已重新挂载");
        RefreshTray(false, false, NULL);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    /* --dump：控制台诊断模式，读取一次完整状态后退出（用于自动化验证） */
    {
        int argc = 0;
        LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        bool dump = false;
        bool selftest = false;
        int  setRateHz = 0;
        int  setDpiStage = -1;
        bool modelScan = false;
        bool wantHelp = false;
        int  watchSec = 0;
        bool doPreview = false;
        int  iconStress = 0;
        wchar_t previewPath[512] = {0};
        for (int i = 1; argv != NULL && i < argc; i++) {
            if (wcscmp(argv[i], L"--dump") == 0) dump = true;
            else if (wcscmp(argv[i], L"--selftest") == 0) selftest = true;
            else if (wcscmp(argv[i], L"--watch") == 0 && i + 1 < argc) {
                watchSec = _wtoi(argv[i + 1]);
            }
            else if (wcscmp(argv[i], L"--icon-stress") == 0 && i + 1 < argc) {
                iconStress = _wtoi(argv[i + 1]);
            }
            else if (wcscmp(argv[i], L"--icon-preview") == 0 && i + 1 < argc) {
                doPreview = true;
                wcsncpy(previewPath, argv[i + 1], 511);
                previewPath[511] = L'\0';
            }
            else if (wcscmp(argv[i], L"--help") == 0 || wcscmp(argv[i], L"-h") == 0 ||
                     wcscmp(argv[i], L"/?") == 0) {
                wantHelp = true;
            }
            else if (wcscmp(argv[i], L"--model-scan") == 0) {
                modelScan = true;
            }
            else if (wcscmp(argv[i], L"--set-dpi-stage") == 0 && i + 1 < argc) {
                setDpiStage = _wtoi(argv[i + 1]);
                if (setDpiStage < 0) setDpiStage = 0;
                if (setDpiStage > 5) setDpiStage = 5;
            }
            else if (wcscmp(argv[i], L"--set-rate") == 0 && i + 1 < argc) {
                dump = true;
                setRateHz = _wtoi(argv[i + 1]);
            }
        }
        if (argv) LocalFree(argv);

        /*
         * 诊断与托盘程序会争抢同一组 HID 特性报文——设备只有一个命令缓冲，
         * 两个进程同时读会互相干扰（可能读到滞后缓冲或全零）。这里检测并明确提示，
         * 而不是让人困惑于"为什么读不到数据"。
         */
        bool trayRunning = false;
        {
            HANDLE probe = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\mchose-tray-single");
            if (probe != NULL) { trayRunning = true; CloseHandle(probe); }
        }

        if (wantHelp) { PrintUsage(); return 0; }

        /* --icon-stress <n>：图标路径 GDI 泄漏压力测试 */
        if (iconStress > 0) return IconStress(iconStress);

        /* --icon-preview <输出.bmp>：把两种样式的图标放大导出，肉眼核对渲染 */
        if (doPreview) return IconPreview(previewPath);

        /* --selftest：菜单分发纯函数全量自测。
         * 回归重点：IDM_DPI(0..5) 必须解析为 ACT_SET_DPI，
         * 历史上它们被休眠分支的宽区间 [1601,1801) 吞掉，变成 99~104 分钟休眠。 */
        if (selftest) {
            char out[4096];
            size_t off = 0;
            out[0] = '\0';
            int fails = 0, checks = 0;

            AppendFmt(out, sizeof(out), off, "=== 菜单分发自测 ===\n");

            const char *names[] = { "回报率", "休眠", "DPI" };
            (void)names;

            for (int i = 0; i < kRateCount; i++) {
                MenuDecision d = DecodeMenuCommand(IDM_RATE(i));
                checks++;
                bool ok = (d.action == ACT_SET_RATE && d.arg == i);
                if (!ok) fails++;
                AppendFmt(out, sizeof(out), off, "  IDM_RATE(%d)=%d -> action=%d arg=%d %s\n",
                          i, IDM_RATE(i), (int)d.action, d.arg, ok ? "OK" : "**FAIL**");
            }
            for (int i = 0; i < kSleepCount; i++) {
                MenuDecision d = DecodeMenuCommand(IDM_SLEEP(i));
                checks++;
                bool ok = (d.action == ACT_SET_SLEEP && d.arg == kSleepChoices[i]);
                if (!ok) fails++;
                AppendFmt(out, sizeof(out), off, "  IDM_SLEEP(%d)=%d -> action=%d arg=%d(分钟) %s\n",
                          i, IDM_SLEEP(i), (int)d.action, d.arg, ok ? "OK" : "**FAIL**");
            }
            for (int i = 0; i < 6; i++) {
                MenuDecision d = DecodeMenuCommand(IDM_DPI(i));
                checks++;
                bool ok = (d.action == ACT_SET_DPI && d.arg == i);
                /* 显式回归断言：DPI 绝不能被解析成休眠 */
                bool regression = (d.action == ACT_SET_SLEEP);
                if (!ok) fails++;
                AppendFmt(out, sizeof(out), off,
                          "  IDM_DPI(%d)=%d -> action=%d arg=%d %s%s\n",
                          i, IDM_DPI(i), (int)d.action, d.arg,
                          ok ? "OK" : "**FAIL**",
                          regression ? "  <== 历史缺陷复现！被当成休眠" : "");
            }
            {
                struct { const char *n; int id; MenuAction a; } fix[] = {
                    { "IDM_EXIT",    IDM_EXIT,    ACT_EXIT    },
                    { "IDM_OSD",     IDM_OSD,     ACT_OSD     },
                    { "IDM_STYLE",   IDM_STYLE,   ACT_STYLE   },
                    { "IDM_AUTORUN", IDM_AUTORUN, ACT_AUTORUN },
                    { "IDM_REFRESH", IDM_REFRESH, ACT_REFRESH },
                    { "IDM_SLEEP_OFF", IDM_SLEEP_OFF, ACT_SLEEP_OFF },
                };
                for (int i = 0; i < 6; i++) {
                    MenuDecision d = DecodeMenuCommand(fix[i].id);
                    checks++;
                    bool ok = (d.action == fix[i].a);
                    if (!ok) fails++;
                    AppendFmt(out, sizeof(out), off, "  %s=%d -> action=%d %s\n",
                              fix[i].n, fix[i].id, (int)d.action, ok ? "OK" : "**FAIL**");
                }
            }
            /* 边界：区间空洞与越界必须是 ACT_NONE，不能误命中邻近功能。
             * 注意 1600 是 IDM_SLEEP_OFF（有效项），不在边界集合里。 */
            {
                int edge[] = { IDM_RATE(0) - 1, IDM_RATE(0) + kRateCount,
                               IDM_SLEEP(0) + kSleepCount,
                               IDM_DPI(0) - 1, IDM_DPI(0) + 6, 0, 1, 1299 };
                for (int i = 0; i < 8; i++) {
                    MenuDecision d = DecodeMenuCommand(edge[i]);
                    checks++;
                    bool ok = (d.action == ACT_NONE);
                    if (!ok) fails++;
                    AppendFmt(out, sizeof(out), off, "  边界 %d -> action=%d %s\n",
                              edge[i], (int)d.action, ok ? "OK" : "**FAIL**");
                }
            }

            /* 宽字符格式化自检。
             * 本工具链（MinGW + msvcrt）下 swprintf 的 %s 是**窄**语义，
             * 宽字符串必须用 %ls；写错不会报错、只会显示乱码，
             * 所以用一条断言把它钉住。 */
            {
                wchar_t wbuf[64] = {0};
                swprintf(wbuf, 64, L"[%ls]", L"OK");
                char u8[128] = {0};
                WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, u8, sizeof(u8), NULL, NULL);
                checks++;
                bool okw = (strcmp(u8, "[OK]") == 0);
                if (!okw) fails++;
                AppendFmt(out, sizeof(out), off,
                          "  宽字符 %%ls 格式化 -> \"%s\" %s\n", u8, okw ? "OK" : "**FAIL**");
            }

            AppendFmt(out, sizeof(out), off, "结果: %d 项检查，%d 项失败 —— %s\n",
                      checks, fails, fails == 0 ? "全部通过" : "存在缺陷");

            HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
            if (hOut != NULL && hOut != INVALID_HANDLE_VALUE) {
                DWORD w = 0; WriteFile(hOut, out, (DWORD)off, &w, NULL);
            }
            HANDLE hf = CreateFileW(L"mchose-tray-dump.txt", GENERIC_WRITE, 0, NULL,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hf != INVALID_HANDLE_VALUE) {
                DWORD w = 0; WriteFile(hf, out, (DWORD)off, &w, NULL); CloseHandle(hf);
            }
            return fails == 0 ? 0 : 1;
        }

        /* --watch：长时间观察，用于回归验证"设置未变化时不应重复通知 UI" */
        if (watchSec > 0) {
            Device::Start(NULL);
            for (int i = 0; i < 20; i++) {
                Sleep(500);
                Device::State s = Device::GetState();
                if (s.connected && s.settingsValid) break;
            }
            Sleep((DWORD)watchSec * 1000);
            Device::State st = Device::GetState();

            char out[1024];
            snprintf(out, sizeof(out),
                "=== --watch %d 秒观察 ===\n"
                "连接      : %s   电量: %d%%   回报率: %d Hz   推送: %lu 条\n"
                "总发布次数: %lu   其中 CHANGE_SETTINGS: %lu\n"
                "判定      : %s\n",
                watchSec, st.connected ? "已连接" : "未连接", st.battery, st.rateHz,
                st.pushReads, st.totalPublishes, st.settingsPublishes,
                (st.settingsPublishes <= 2)
                    ? "通过（设置未变化时不重复通知，OSD 不会周期性弹窗）"
                    : "失败（仍在周期性发布 CHANGE_SETTINGS）");

            HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
            if (hOut != NULL && hOut != INVALID_HANDLE_VALUE) {
                DWORD w = 0; WriteFile(hOut, out, (DWORD)strlen(out), &w, NULL);
            }
            HANDLE hf = CreateFileW(L"mchose-tray-dump.txt", GENERIC_WRITE, 0, NULL,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hf != INVALID_HANDLE_VALUE) {
                DWORD w = 0; WriteFile(hf, out, (DWORD)strlen(out), &w, NULL); CloseHandle(hf);
            }
            Device::Stop();
            return (st.settingsPublishes <= 2) ? 0 : 1;
        }

        /*
         * --model-scan：机型识别报告。
         *
         * 迈从不同机型 VID/PID 不同（实测 A7 Pro = 5253:1021，社区记录
         * A7 V2 Ultra = 3837:100B、K7 Ultra = 3837:1001），且 PID 会跨机型复用
         * （0x1020 在 A7 Pro 是有线、在 L7 Pro 是无线）。因此机型判定以
         * **设备自报的型号名**为准，本命令把判定过程与依据全部打印出来，
         * 便于新机型用户把结果反馈成一条机型库记录。
         */
        if (modelScan) {
            Device::Start(NULL);
            for (int i = 0; i < 20; i++) {
                Sleep(500);
                Device::State s = Device::GetState();
                if (s.connected && s.settingsValid) break;
            }
            Device::State st = Device::GetState();

            char name8[128] = {0}, prof8[128] = {0};
            WideCharToMultiByte(CP_UTF8, 0, st.modelName, -1, name8, sizeof(name8), NULL, NULL);
            WideCharToMultiByte(CP_UTF8, 0, st.modelProfile, -1, prof8, sizeof(prof8), NULL, NULL);

            char out[2048];
            size_t off = 0;
            out[0] = '\0';
            AppendFmt(out, sizeof(out), off, "=== 机型识别（--model-scan）===\n");
            AppendFmt(out, sizeof(out), off, "设备自报型号 : %s\n",
                      name8[0] ? name8 : "(未读到 —— 移动鼠标唤醒后重试)");
            AppendFmt(out, sizeof(out), off, "接口 VID/PID : 0x%04X / 0x%04X   <- 机型识别依据\n",
                      (unsigned)st.ifaceVid, (unsigned)st.ifacePid);
            AppendFmt(out, sizeof(out), off, "机体 VID/PID : 0x%04X / 0x%04X   <- 11 06 的设备类型码\n",
                      (unsigned)st.deviceVid, (unsigned)st.devicePid);
            AppendFmt(out, sizeof(out), off, "连接模式     : %d (%s)\n", st.connectMode,
                      st.connectMode == 1 ? "2.4G 无线" : "有线/其它");
            /* 诊断里直接自行解析一次：不依赖设备层的内部簿记，结论更可信 */
            {
                const McHose::ModelSpec *sp =
                    McHose::ResolveModel(st.modelName, st.ifaceVid, st.ifacePid);
                McHose::ModelCaps cp = McHose::CapsFor(sp, st.modelName);
                char cpName[128] = {0};
                if (cp.displayName) WideCharToMultiByte(CP_UTF8, 0, cp.displayName, -1,
                                                        cpName, sizeof(cpName), NULL, NULL);
                AppendFmt(out, sizeof(out), off, "机型库匹配   : %s\n",
                      (sp && sp->name) ? "已匹配" : "(未匹配到具体机型 —— 按同方案默认参数运行)");
                AppendFmt(out, sizeof(out), off, "  匹配机型名 : %s\n", cpName);
                AppendFmt(out, sizeof(out), off, "  识别状态   : %s\n",
                          cp.known ? "已识别" : "未知机型");
                AppendFmt(out, sizeof(out), off, "  数据来源   : %s\n", cp.source ? cp.source : "unknown");
                AppendFmt(out, sizeof(out), off, "  DPI 上限   : ");
                if (cp.dpiMax > 0) AppendFmt(out, sizeof(out), off, "%d\n", cp.dpiMax);
                else               AppendFmt(out, sizeof(out), off, "(未知)\n");
                AppendFmt(out, sizeof(out), off, "  DPI 档位数 : %d\n", cp.dpiStages);
                AppendFmt(out, sizeof(out), off, "  回报率档数 : 无线 %d / 有线 %d\n",
                          cp.rateCountWireless, cp.rateCountWired);
            }
            if (st.settingsValid) {
                AppendFmt(out, sizeof(out), off, "设备实读档值 :");
                for (int i = 0; i < 6; i++) AppendFmt(out, sizeof(out), off, " %u", st.dpi[i]);
                AppendFmt(out, sizeof(out), off, "\n");
            } else {
                /* 未读到设置时打印一排 0 会误导，明确说明原因 */
                AppendFmt(out, sizeof(out), off,
                          "设备实读档值 : (未读到 —— 鼠标可能休眠，移动鼠标唤醒后重试)\n");
            }
            AppendFmt(out, sizeof(out), off,
                      "\n若为\"未知机型\"，请用 tools\\hid_probe.exe 5253 与 3837 各跑一次，\n"
                      "记录收报 VID/PID 与 UsagePage，即可在 src\\model_db.cpp 增加一行。\n");

            HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
            if (hOut != NULL && hOut != INVALID_HANDLE_VALUE) {
                DWORD w = 0; WriteFile(hOut, out, (DWORD)off, &w, NULL);
            }
            HANDLE hf = CreateFileW(L"mchose-tray-dump.txt", GENERIC_WRITE, 0, NULL,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hf != INVALID_HANDLE_VALUE) {
                DWORD w = 0; WriteFile(hf, out, (DWORD)off, &w, NULL); CloseHandle(hf);
            }
            Device::Stop();
            return 0;
        }

        /*
         * --set-dpi-stage <n>：把 DPI 活动档位设为第 n 档（n 从 0 开始）。
         *
         * 存在的理由：逆向期间用 tools\mchose_probe.exe dpitest 做字段映射判定时会写 DPI 索引，
         * 而该芯片存在"读到的响应比当前命令滞后一条"的现象，导致 dpitest 的自校验还原
         * 可能误判成功、把活动档位留在错误值上（实测发生过：1600 DPI 被留成 800 DPI）。
         * 本命令走应用里那套"写后稳定期 + 丢弃一次 + 再读"的可靠校验，用于恢复。
         */
        if (setDpiStage >= 0) {
            Device::Start(NULL);
            for (int i = 0; i < 20; i++) {
                Sleep(500);
                Device::State s = Device::GetState();
                if (s.connected && s.settingsValid) break;
            }
            Device::State before = Device::GetState();
            Device::RequestSwitchDpiStage(setDpiStage);

            int result = Device::CMD_RESULT_NONE;
            for (int i = 0; i < 20; i++) {
                Sleep(400);
                result = Device::TakeLastCommandResult();
                if (result != Device::CMD_RESULT_NONE) break;
            }
            Device::State after = Device::GetState();

            char out[1500];
            size_t off = 0;
            out[0] = '\0';
            AppendFmt(out, sizeof(out), off, "=== DPI 档位恢复（--set-dpi-stage %d）===\n",
                      setDpiStage);
            AppendFmt(out, sizeof(out), off, "写前     : 活动第 %d 档 (%u DPI) / 写入字段 %d\n",
                      before.dpiActiveIndex + 1,
                      (before.dpiActiveIndex < 6) ? before.dpi[before.dpiActiveIndex] : 0,
                      before.dpiIndexRaw);
            AppendFmt(out, sizeof(out), off, "命令结果 : %s\n",
                      result == Device::CMD_RESULT_OK_VERIFIED ? "成功（写后回读一致）" :
                      result == Device::CMD_RESULT_FAILED ? "失败（回读不一致或写入被拒）" :
                      result == Device::CMD_RESULT_SENT_UNVERIFIED ? "已下发（未校验）" : "超时无结果");
            AppendFmt(out, sizeof(out), off, "写后     : 活动第 %d 档 (%u DPI) / 写入字段 %d\n",
                      after.dpiActiveIndex + 1,
                      (after.dpiActiveIndex < 6) ? after.dpi[after.dpiActiveIndex] : 0,
                      after.dpiIndexRaw);
            AppendFmt(out, sizeof(out), off,
                      "说明     : 写入字段立刻变化；活动档位由设备自行同步后才跟随，\n"
                      "           因此\"写后活动档位\"暂时等于写前值是正常现象。\n");

            HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
            if (hOut != NULL && hOut != INVALID_HANDLE_VALUE) {
                DWORD w = 0; WriteFile(hOut, out, (DWORD)off, &w, NULL);
            }
            HANDLE hf = CreateFileW(L"mchose-tray-dump.txt", GENERIC_WRITE, 0, NULL,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hf != INVALID_HANDLE_VALUE) {
                DWORD w = 0; WriteFile(hf, out, (DWORD)off, &w, NULL); CloseHandle(hf);
            }
            Device::Stop();
            return (result == 0) ? 0 : 1;
        }

        if (dump && setRateHz > 0) {            Device::Start(NULL);
            for (int i = 0; i < 20; i++) {
                Sleep(500);
                Device::State s = Device::GetState();
                if (s.connected && s.settingsValid) break;
            }
            Device::State before = Device::GetState();
            Device::RequestSetPollingRate(setRateHz);

            int result = Device::CMD_RESULT_NONE;
            for (int i = 0; i < 20; i++) {
                Sleep(400);
                result = Device::TakeLastCommandResult();
                if (result != Device::CMD_RESULT_NONE) break;
            }
            Device::State after = Device::GetState();

            char out[1024];
            size_t off = 0;
            out[0] = '\0';
            AppendFmt(out, sizeof(out), off, "=== 写命令验证（--set-rate %d）===\n", setRateHz);
            AppendFmt(out, sizeof(out), off, "写前     : 活动 %d Hz(档 %d) / 写入字段档 %d\n",
                      before.rateHz, before.rateIndex, before.writeRateIndex);
            AppendFmt(out, sizeof(out), off, "命令结果 : %s\n",
                      result == Device::CMD_RESULT_OK_VERIFIED ? "成功（写后回读一致）" :
                      result == Device::CMD_RESULT_FAILED ? "失败（回读不一致或写入被拒）" :
                      result == Device::CMD_RESULT_SENT_UNVERIFIED ? "已下发（未校验）" : "超时无结果");
            AppendFmt(out, sizeof(out), off, "写后     : 活动 %d Hz(档 %d) / 写入字段档 %d\n",
                      after.rateHz, after.rateIndex, after.writeRateIndex);

            HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
            if (hOut != NULL && hOut != INVALID_HANDLE_VALUE) {
                DWORD w = 0; WriteFile(hOut, out, (DWORD)off, &w, NULL);
            }
            HANDLE hf = CreateFileW(L"mchose-tray-dump.txt", GENERIC_WRITE, 0, NULL,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hf != INVALID_HANDLE_VALUE) {
                DWORD w = 0; WriteFile(hf, out, (DWORD)off, &w, NULL); CloseHandle(hf);
            }
            Device::Stop();
            return (result == 0) ? 0 : 1;
        }

        if (dump) {
            SetConsoleOutputCP(CP_UTF8);
            Device::Start(NULL);

            Device::State st;
            bool ok = false;
            /* 至少观察 6 秒：给设备主动推送留出时间（nudge 后约 2 秒到达） */
            for (int i = 0; i < 24; i++) {          /* 最多等 12 秒 */
                Sleep(500);
                st = Device::GetState();
                if (i >= 11 && st.connected && st.mouseLinked && st.settingsValid &&
                    st.batteryValid && st.dpi[0] != 0 && st.pushCount > 0) {
                    ok = true;
                    break;
                }
            }
            st = Device::GetState();
            /* 鼠标休眠是可区分状态：接收器在、但鼠标本体不应答 */
            const bool asleep = (st.connected && !st.mouseLinked);

            char name[128] = {0}, fw[64] = {0};
            if (st.modelName[0])
                WideCharToMultiByte(CP_UTF8, 0, st.modelName, -1, name, sizeof(name), NULL, NULL);
            if (st.firmware[0])
                WideCharToMultiByte(CP_UTF8, 0, st.firmware, -1, fw, sizeof(fw), NULL, NULL);

            char out[4096];
            size_t off = 0;
            out[0] = '\0';
            AppendFmt(out, sizeof(out), off, "=== mchose-tray 诊断模式 ===\n");
            if (trayRunning)
                AppendFmt(out, sizeof(out), off,
                          "并发提示  : 托盘程序正在运行，两者会争抢设备命令缓冲；\n"
                          "            若本报告显示读失败/全零，请先从托盘菜单退出再重试。\n");
            AppendFmt(out, sizeof(out), off, "连接状态  : %s\n",
                      !st.connected ? "未连接" :
                      (st.mouseLinked ? "已连接" : "接收器在线，鼠标休眠"));
            AppendFmt(out, sizeof(out), off, "型号      : %s\n", name[0] ? name : "(未知)");
            AppendFmt(out, sizeof(out), off, "固件版本  : %s\n", fw[0] ? fw : "(未读到)");
            AppendFmt(out, sizeof(out), off, "连接模式  : %s\n",
                      st.connectMode == 1 ? "2.4G 无线" : "有线/其它");
            AppendFmt(out, sizeof(out), off, "电量      : %d%%%s\n",
                      st.battery, st.charging ? " (充电中)" : "");
            AppendFmt(out, sizeof(out), off, "回报率    : %d Hz (档位 %d/%d)\n",
                      st.rateHz, st.rateIndex + 1, st.rateCount);
            {
                char prof8b[128] = {0};
                WideCharToMultiByte(CP_UTF8, 0, st.modelProfile, -1, prof8b, sizeof(prof8b), NULL, NULL);
                AppendFmt(out, sizeof(out), off, "机型库    : %s（%s，来源：%s）\n",
                          prof8b[0] ? prof8b : "未匹配",
                          st.modelKnown ? "已识别" : "未知机型",
                          st.modelSource ? st.modelSource : "unknown");
                AppendFmt(out, sizeof(out), off, "接口 VID/PID: 0x%04X / 0x%04X   机体: 0x%04X / 0x%04X\n",
                          (unsigned)st.ifaceVid, (unsigned)st.ifacePid,
                          (unsigned)st.deviceVid, (unsigned)st.devicePid);
            }
            AppendFmt(out, sizeof(out), off, "DPI 档位  : 第 %d 档 (活动索引 %d / 写入字段 %d)\n", st.dpiActiveIndex + 1, st.dpiActiveIndex, st.dpiIndexRaw);
            AppendFmt(out, sizeof(out), off, "DPI 档值  : ");
            for (int i = 0; i < 6; i++) AppendFmt(out, sizeof(out), off, "%u ", st.dpi[i]);
            AppendFmt(out, sizeof(out), off, "\n");
            AppendFmt(out, sizeof(out), off, "休眠      : %u 分钟\n", st.sleepMinutes);
            AppendFmt(out, sizeof(out), off, "推送通道  : %s，收到 %lu 条（有效 %lu 条）\n",
                      st.pushChannelOpen ? "已打开" : "未打开", st.pushReads, st.pushCount);
            AppendFmt(out, sizeof(out), off, "读失败    : %lu 次\n", st.readErrors);
            AppendFmt(out, sizeof(out), off, "最近推送  : ");
            for (int i = 0; i < st.lastPushLen; i++)
                AppendFmt(out, sizeof(out), off, "%02X ", st.lastPush[i]);
            if (st.lastPushLen == 0) AppendFmt(out, sizeof(out), off, "(无)");
            AppendFmt(out, sizeof(out), off, "\n");
            AppendFmt(out, sizeof(out), off, "结论      : %s\n",
                      ok ? "状态读取完整 —— 协议链路正常" :
                      (asleep ? "鼠标休眠中（接收器正常）—— 移动鼠标唤醒后重试"
                              : "状态不完整"));

            /* 顺带打印托盘提示与 OSD 文本，用于核对宽字符格式化是否正确。
             * 本工具链 swprintf 的 %s 是窄语义，宽串写错只会得到乱码、不会有编译警告。 */
            {
                wchar_t tip[256] = {0};
                wchar_t lines[3][160];
                memset(lines, 0, sizeof(lines));
                TrayUi::BuildTooltipText(st, tip, 256);
                TrayUi::BuildOsdLines(st, NULL, lines);

                char tip8[768] = {0}, l0[512] = {0}, l1[512] = {0}, l2[512] = {0};
                WideCharToMultiByte(CP_UTF8, 0, tip,     -1, tip8, sizeof(tip8), NULL, NULL);
                WideCharToMultiByte(CP_UTF8, 0, lines[0], -1, l0, sizeof(l0), NULL, NULL);
                WideCharToMultiByte(CP_UTF8, 0, lines[1], -1, l1, sizeof(l1), NULL, NULL);
                WideCharToMultiByte(CP_UTF8, 0, lines[2], -1, l2, sizeof(l2), NULL, NULL);
                for (char *p = tip8; *p; p++) if (*p == '\n') *p = '|';

                AppendFmt(out, sizeof(out), off, "提示文本  : %s\n", tip8);
                AppendFmt(out, sizeof(out), off, "悬浮窗    : %s / %s / %s\n", l0, l1, l2);
            }

            /* 1) 写入文件，保证任何启动方式都能取到结果 */
            HANDLE hf = CreateFileW(L"mchose-tray-dump.txt", GENERIC_WRITE, 0, NULL,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hf != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(hf, out, (DWORD)off, &written, NULL);
                CloseHandle(hf);
            }
            /* 2) 直接写 stdout 句柄（GUI 子系统下也能被管道捕获） */
            HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
            if (hOut != NULL && hOut != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(hOut, out, (DWORD)off, &written, NULL);
            }

            Device::Stop();
            /* 退出码：0 完整；2 鼠标休眠（链路本身正常）；1 其它不完整 */
            return ok ? 0 : (asleep ? 2 : 1);
        }
    }

    /* 单实例。注意：必须在确认自己是唯一实例之后才动日志文件，
     * 否则第二个实例会把正在运行的那个实例的日志清掉。 */
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\mchose-tray-single");
    if (hMutex != NULL && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    DeleteFileW(GuiLogPath());
    GuiLog("[1] 进入 GUI 模式");
    GuiLog("[2] 单实例互斥体已获取");

    /* 每显示器 DPI 感知 */
    typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        PFN_SetProcessDpiAwarenessContext fn =
            (PFN_SetProcessDpiAwarenessContext)(void *)GetProcAddress(
                user32, "SetProcessDpiAwarenessContext");
        if (fn) fn((HANDLE)-4);   /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
    }

    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"McHoseTrayMessageWnd";
    if (!RegisterClassExW(&wc)) { GuiLog("[X] RegisterClassExW 失败"); return 1; }
    GuiLog("[3] 窗口类注册成功");

    g_hMain = CreateWindowExW(0, L"McHoseTrayMessageWnd", L"mchose-tray",
                              WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (g_hMain == NULL) { GuiLog("[X] CreateWindowExW 失败"); return 1; }
    GuiLog("[4] 宿主窗口创建成功");

    TrayUi::Init(hInst);
    GuiLog("[5] TrayUi::Init 完成");

    /* 托盘图标 */
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hMain;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = TrayUi::CreateBatteryIcon(Device::GetState(), TrayUi::GetBatteryStyle());
    g_hIcon = g_nid.hIcon;
    {
        char dbg[200];
        snprintf(dbg, sizeof(dbg), "[6a] 图标句柄=%p cbSize=%u SM_CXSMICON=%d",
                 (void *)g_nid.hIcon, (unsigned)g_nid.cbSize,
                 (int)GetSystemMetrics(SM_CXSMICON));
        GuiLog(dbg);
    }
    g_nid.szTip[0] = L'\0';
    wcscpy(g_nid.szTip, L"MCHOSE 鼠标 (正在连接...)");
    SetLastError(0);
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        DWORD err = GetLastError();
        DWORD il = GetOwnIntegrityRid();
        char dbg[256];
        snprintf(dbg, sizeof(dbg),
                 "[!] Shell_NotifyIconW(NIM_ADD) 失败 err=%lu 本进程IL=0x%04lX",
                 (unsigned long)err, (unsigned long)il);
        GuiLog(dbg);
        ExplainTrayFailure(err, il);
    } else {
        GuiLog("[6] 托盘图标已添加");
    }

    if (!Device::Start(OnDeviceStateChanged)) GuiLog("[!] Device::Start 失败");
    else GuiLog("[7] 设备线程已启动");

    /* 一次性工作集裁剪：把当前不用的页面换出，让常驻内存更贴近真实需求。
     * 注意这**不是**减少分配——私有内存不变，只是降低工作集（任务管理器显示值）；
     * 后续被换出的页面在需要时会重新调入，代价是少量缺页。 */
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    GuiLog("[8] 进入消息循环");
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    GuiLog("[9] 消息循环结束");

    Device::Stop();
    TrayUi::Cleanup();
    if (g_hIcon) DestroyIcon(g_hIcon);
    if (hMutex) CloseHandle(hMutex);
    return 0;
}
