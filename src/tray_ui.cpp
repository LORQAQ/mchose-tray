/*
 * tray_ui.cpp — 托盘图标绘制、OSD 悬浮窗、自启与样式持久化
 */

#include "tray_ui.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

namespace TrayUi {

namespace {

const wchar_t *kRegKey      = L"Software\\mchose-tray";
const wchar_t *kRunKey      = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t *kRunValue    = L"mchose-tray";
const wchar_t *kOsdClass    = L"McHoseOsdWindow";

const int TIMER_OSD_HIDE = 101;
const int TIMER_OSD_FADE = 102;

HINSTANCE g_hInst = NULL;
HWND      g_hOsd = NULL;
HFONT     g_hFont1 = NULL;
HFONT     g_hFont2 = NULL;
int       g_osdAlpha = 0;
int       g_osdW = 0, g_osdH = 0;
wchar_t   g_osdLine[3][160];

/* ---------------------------------------------------------- 注册表小工具 */

DWORD RegGetDword(const wchar_t *name, DWORD def)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return def;
    DWORD v = def, cb = sizeof(v), type = 0;
    RegQueryValueExW(k, name, NULL, &type, (LPBYTE)&v, &cb);
    RegCloseKey(k);
    return v;
}

void RegSetDword(const wchar_t *name, DWORD v)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, NULL, 0,
                        KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
    RegCloseKey(k);
}

/* ------------------------------------------------------------ 颜色选择 */

COLORREF ColorFor(const Device::State &st)
{
    if (!st.connected)     return RGB(120, 120, 120);
    if (!st.mouseLinked)   return RGB(135, 135, 145);   /* 接收器在、鼠标休眠 */
    if (st.charging)       return RGB(46, 204, 113);
    if (!st.batteryValid)  return RGB(150, 150, 150);
    if (st.battery <= 20)  return RGB(231, 76, 60);
    if (st.battery <= 40)  return RGB(241, 196, 15);
    return RGB(52, 152, 219);
}

const wchar_t *ModeText(const Device::State &st)
{
    if (!st.connected)   return L"未连接";
    if (!st.mouseLinked) return L"鼠标休眠中";
    return st.connectMode == 1 ? L"2.4G 无线" : L"有线";
}

/* 是否有可信的电量读数 */
bool BatteryUsable(const Device::State &st)
{
    return st.connected && st.mouseLinked && st.batteryValid;
}

/* ----------------------------------------------------------- 图标绘制 */

/*
 * 用 GDI 画到 32bpp DIB，然后把所有非黑像素的 alpha 置 255，得到带透明的图标。
 * （GDI 不会写 alpha，所以需要这一步后处理。）
 */
HICON BuildIconFromDib(HBITMAP hbm, void *bits, int size)
{
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size;      /* 自上而下 */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    /* 后处理 alpha：非黑即不透明 */
    unsigned char *px = (unsigned char *)bits;
    for (int i = 0; i < size * size; i++) {
        unsigned char b = px[i * 4 + 0], g = px[i * 4 + 1], r = px[i * 4 + 2];
        px[i * 4 + 3] = (r | g | b) ? 255 : 0;
    }

    HBITMAP hMask = CreateBitmap(size, size, 1, 1, NULL);
    ICONINFO ii;
    memset(&ii, 0, sizeof(ii));
    ii.fIcon = TRUE;
    ii.hbmMask = hMask;
    ii.hbmColor = hbm;
    HICON icon = CreateIconIndirect(&ii);
    if (hMask) DeleteObject(hMask);
    return icon;
}

HICON DrawCapsule(const Device::State &st, int size)
{
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (hbm == NULL) return NULL;

    memset(bits, 0, (size_t)size * size * 4);

    HDC hdc = CreateCompatibleDC(NULL);
    if (hdc == NULL) { DeleteObject(hbm); return NULL; }   /* 否则 SelectObject(NULL) 是 UB，且位图泄漏 */
    HGDIOBJ old = SelectObject(hdc, hbm);

    COLORREF col = ColorFor(st);
    int pad = size / 8;
    int bodyW = size - pad * 2 - size / 8;          /* 右侧留出电极凸起 */
    int bodyH = (size * 3) / 5;
    int top = (size - bodyH) / 2;
    int radius = size / 8;
    int lvl = BatteryUsable(st) ? st.battery : 0;

    /* 外壳 */
    HBRUSH brFrame = CreateSolidBrush(RGB(200, 200, 200));
    HPEN   penFrame = CreatePen(PS_SOLID, 1, RGB(160, 160, 160));
    HGDIOBJ ob = SelectObject(hdc, brFrame);
    HGDIOBJ op = SelectObject(hdc, penFrame);
    RoundRect(hdc, pad, top, pad + bodyW, top + bodyH, radius, radius);

    /* 电极 */
    RECT nub = { pad + bodyW + 1, top + bodyH / 4, pad + bodyW + size / 10, top + bodyH * 3 / 4 };
    FillRect(hdc, &nub, brFrame);

    /* 电量填充 */
    if (lvl > 0) {
        int innerW = bodyW - 4;
        int fillW = innerW * lvl / 100;
        if (fillW < 2) fillW = 2;
        RECT fr = { pad + 2, top + 2, pad + 2 + fillW, top + bodyH - 2 };
        HBRUSH brFill = CreateSolidBrush(col);
        FillRect(hdc, &fr, brFill);
        DeleteObject(brFill);
    }

    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(brFrame);
    DeleteObject(penFrame);

    SelectObject(hdc, old);
    DeleteDC(hdc);

    HICON icon = BuildIconFromDib(hbm, bits, size);
    DeleteObject(hbm);
    return icon;
}

HICON DrawNumber(const Device::State &st, int size)
{
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (hbm == NULL) return NULL;
    memset(bits, 0, (size_t)size * size * 4);

    HDC hdc = CreateCompatibleDC(NULL);
    if (hdc == NULL) { DeleteObject(hbm); return NULL; }   /* 同上：避免 UB 与泄漏 */
    HGDIOBJ old = SelectObject(hdc, hbm);
    SetBkMode(hdc, TRANSPARENT);

    COLORREF col = ColorFor(st);
    const bool usable = BatteryUsable(st);

    wchar_t text[16];
    if (!st.connected)                 wcscpy(text, L"--");
    else if (!st.mouseLinked)          wcscpy(text, L"zZ");
    else if (!st.batteryValid)         wcscpy(text, L"..");
    else                               swprintf(text, 16, L"%d", st.battery);

    /* 底部横轨只在有可信读数时画；否则横轨无意义且会挤掉文字空间 */
    int railH = 0, railTop = 0;
    if (usable) {
        railH = (size >= 24) ? 3 : 2;
        railTop = size - 1 - railH;
    }
    const int textTop = 0;
    const int textBottom = usable ? railTop : size;

    /*
     * 字号自动适配：从可用高度开始逐级缩小，直到整串文字宽度能放进 size-2 像素。
     * 必须这样做——"100" 比 "5" 宽得多，固定字号必然把三位数压扁或裁掉。
     * 同时使用 NONANTIALIASED_QUALITY：抗锯齿像素会混向黑色背景，
     * 而 BuildIconFromDib 把任何非黑像素都置为不透明，于是形成深色脏边。
     * 关掉抗锯齿后像素是纯色，16px 下反而更清晰。
     */
    HFONT font = NULL;
    HGDIOBJ prevFont = NULL;      /* 文字的"前一个"GDI 对象，只用于取消选中字体 */
    {
        int avail = size - 2;                 /* 左右各留 1px 余量 */
        for (int fh = textBottom - textTop; fh >= 5; fh--) {
            HFONT f = CreateFontW(-fh, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  NONANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            if (f == NULL) continue;
            HGDIOBJ prev = SelectObject(hdc, f);
            SIZE sz;
            sz.cx = 0; sz.cy = 0;
            GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &sz);
            if (sz.cx <= avail || fh == 5) {
                font = f;                     /* 命中：保持选中，循环结束 */
                prevFont = prev;
                break;
            }
            SelectObject(hdc, prev);
            DeleteObject(f);
        }
    }

    if (font != NULL) {
        SetTextColor(hdc, col);
        RECT rc = { 0, textTop, size, textBottom };
        DrawTextW(hdc, text, -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
        /* 只取消选中"字体"，绝不能在这里恢复 old ——
         * old 是创建 DC 时的原始对象，恢复它会同时把 DIB 取消选中，
         * 之后画横轨就画到了别处（曾经导致电量横轨完全不显示）。 */
        if (prevFont != NULL) SelectObject(hdc, prevFont);
        DeleteObject(font);
        font = NULL;
    }

    if (usable) {
        RECT rail = { 0, railTop, size, railTop + railH };
        HBRUSH brRail = CreateSolidBrush(RGB(110, 115, 125));
        FillRect(hdc, &rail, brRail);
        DeleteObject(brRail);
        if (st.battery > 0) {
            RECT fr = rail;
            fr.right = fr.left + (rail.right - rail.left) * st.battery / 100;
            HBRUSH brFill = CreateSolidBrush(col);
            FillRect(hdc, &fr, brFill);
            DeleteObject(brFill);
        }
    }

    if (font == NULL) SelectObject(hdc, old);
    DeleteDC(hdc);

    HICON icon = BuildIconFromDib(hbm, bits, size);
    DeleteObject(hbm);
    return icon;
}

/* ---------------------------------------------------------------- OSD */

void OsdPaint(HWND hWnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);

    HDC mem = CreateCompatibleDC(hdc);
    if (mem == NULL) { EndPaint(hWnd, &ps); return; }
    HBITMAP bmp = CreateCompatibleBitmap(hdc, g_osdW, g_osdH);
    if (bmp == NULL) { DeleteDC(mem); EndPaint(hWnd, &ps); return; }
    HGDIOBJ old = SelectObject(mem, bmp);

    RECT full = { 0, 0, g_osdW, g_osdH };
    HBRUSH bg = CreateSolidBrush(RGB(28, 30, 36));
    FillRect(mem, &full, bg);
    DeleteObject(bg);

    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(240, 240, 240));
    HGDIOBJ of = SelectObject(mem, g_hFont1);
    RECT r1 = { 14, 10, g_osdW - 14, 10 + 26 };
    DrawTextW(mem, g_osdLine[0], -1, &r1, DT_LEFT | DT_SINGLELINE | DT_NOCLIP);

    SelectObject(mem, g_hFont2);
    SetTextColor(mem, RGB(180, 200, 230));
    RECT r2 = { 14, 38, g_osdW - 14, 38 + 22 };
    DrawTextW(mem, g_osdLine[1], -1, &r2, DT_LEFT | DT_SINGLELINE | DT_NOCLIP);

    SetTextColor(mem, RGB(150, 160, 175));
    RECT r3 = { 14, 60, g_osdW - 14, 60 + 20 };
    DrawTextW(mem, g_osdLine[2], -1, &r3, DT_LEFT | DT_SINGLELINE | DT_NOCLIP);

    SelectObject(mem, of);
    BitBlt(hdc, 0, 0, g_osdW, g_osdH, mem, 0, 0, SRCCOPY);

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hWnd, &ps);
}

void OsdSetAlpha(int alpha)
{
    if (g_hOsd == NULL) return;
    g_osdAlpha = alpha;
    if (alpha <= 0) {
        ShowWindow(g_hOsd, SW_HIDE);
        return;
    }
    SetLayeredWindowAttributes(g_hOsd, 0, (BYTE)alpha, LWA_ALPHA);
    ShowWindow(g_hOsd, SW_SHOWNOACTIVATE);
    SetWindowPos(g_hOsd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_hOsd, NULL, FALSE);
}

LRESULT CALLBACK OsdProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        OsdPaint(hWnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (wp == TIMER_OSD_HIDE) {
            KillTimer(hWnd, TIMER_OSD_HIDE);
            SetTimer(hWnd, TIMER_OSD_FADE, 16, NULL);
        } else if (wp == TIMER_OSD_FADE) {
            int a = g_osdAlpha - 18;
            if (a <= 0) {
                KillTimer(hWnd, TIMER_OSD_FADE);
                OsdSetAlpha(0);
            } else {
                OsdSetAlpha(a);
            }
        }
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wp, lp);
}

void PositionOsd()
{
    HMONITOR mon = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR m2 = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (m2) mon = m2;

    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;

    int x = mi.rcWork.right - g_osdW - 24;
    int y = mi.rcWork.bottom - g_osdH - 24;
    SetWindowPos(g_hOsd, HWND_TOPMOST, x, y, g_osdW, g_osdH,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

}  // namespace

/* ---------------------------------------------------------------- 公开接口 */

void Init(HINSTANCE hInst)
{
    g_hInst = hInst;

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OsdProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = kOsdClass;
    RegisterClassExW(&wc);

    g_osdW = 300;
    g_osdH = 92;

    g_hOsd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             kOsdClass, L"", WS_POPUP,
                             0, 0, g_osdW, g_osdH, NULL, NULL, hInst, NULL);

    g_hFont1 = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    g_hFont2 = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");

    if (g_hOsd != NULL) SetLayeredWindowAttributes(g_hOsd, 0, 0, LWA_ALPHA);
}

void Cleanup()
{
    if (g_hOsd != NULL) { DestroyWindow(g_hOsd); g_hOsd = NULL; }
    if (g_hFont1) { DeleteObject(g_hFont1); g_hFont1 = NULL; }
    if (g_hFont2) { DeleteObject(g_hFont2); g_hFont2 = NULL; }
}

HICON CreateBatteryIcon(const Device::State &st, int style)
{
    int size = GetSystemMetrics(SM_CXSMICON);
    if (size < 16) size = 16;
    if (size > 64) size = 64;

    return (style == 1) ? DrawNumber(st, size) : DrawCapsule(st, size);
}

void BuildTooltipText(const Device::State &st, wchar_t *out, int cap)
{
    if (out == NULL || cap <= 0) return;
    if (!st.connected) {
        swprintf(out, (size_t)cap, L"MCHOSE 鼠标\n未连接（请检查接收器）");
    } else if (!st.mouseLinked) {
        swprintf(out, (size_t)cap, L"%ls\n鼠标休眠中（接收器正常）\n移动鼠标即可唤醒",
                 st.modelName[0] ? st.modelName : L"MCHOSE 鼠标");
    } else {
        wchar_t rate[32];
        if (st.rateHz > 0) swprintf(rate, 32, L"%d Hz", st.rateHz);
        else               wcscpy(rate, L"未知");

        swprintf(out, (size_t)cap,
                 L"%ls\n电量 %d%%%ls · %ls\n回报率 %ls · DPI 档 %d",
                 st.modelName[0] ? st.modelName : L"MCHOSE 鼠标",
                 st.batteryValid ? st.battery : 0,
                 st.charging ? L"（充电中）" : L"",
                 ModeText(st),
                 rate,
                 st.dpiIndexRaw + 1);
    }
}

void BuildOsdLines(const Device::State &st, const wchar_t *note,
                   wchar_t out[3][160])
{
    wchar_t rate[32];
    if (st.rateHz > 0) swprintf(rate, 32, L"%d Hz", st.rateHz);
    else               wcscpy(rate, L"回报率未知");

    swprintf(out[0], 160, L"%ls   %ls",
             st.modelName[0] ? st.modelName : L"MCHOSE 鼠标",
             ModeText(st));

    if (st.connected && !st.mouseLinked) {
        swprintf(out[1], 160, L"鼠标休眠中");
        swprintf(out[2], 160, L"接收器正常，移动鼠标即可唤醒");
    } else if (st.connected && st.settingsValid) {
        unsigned dpiVal = (st.dpiIndexRaw < 6) ? st.dpi[st.dpiIndexRaw] : 0;
        swprintf(out[1], 160, L"第 %d 档 · %u DPI", st.dpiIndexRaw + 1, dpiVal);
        swprintf(out[2], 160, L"电量 %d%%%ls · %ls · 固件 %ls",
                 st.batteryValid ? st.battery : 0,
                 st.charging ? L" ⚡" : L"",
                 rate,
                 st.firmware[0] ? st.firmware : L"-");
    } else if (st.connected) {
        swprintf(out[1], 160, L"正在读取设置…");
        swprintf(out[2], 160, L"电量 %d%%%ls",
                 st.batteryValid ? st.battery : 0,
                 st.charging ? L" ⚡" : L"");
    } else {
        swprintf(out[1], 160, L"未连接");
        swprintf(out[2], 160, L"请检查 2.4G 接收器是否插好");
    }

    /* note 非空时用第三行显示命令反馈（设置成功/失败），比固件版本更即时有用 */
    if (note != NULL && note[0] != L'\0') {
        wcsncpy(out[2], note, 159);
        out[2][159] = L'\0';
    }
}

void UpdateTooltip(NOTIFYICONDATAW &nid, const Device::State &st)
{
    wchar_t buf[256] = {0};
    BuildTooltipText(st, buf, 256);
    wcsncpy(nid.szTip, buf, 127);
    nid.szTip[127] = L'\0';
}

void ShowOsd(const Device::State &st, const wchar_t *note)
{
    if (g_hOsd == NULL) return;

    BuildOsdLines(st, note, g_osdLine);

    PositionOsd();
    KillTimer(g_hOsd, TIMER_OSD_HIDE);
    KillTimer(g_hOsd, TIMER_OSD_FADE);
    OsdSetAlpha(238);
    SetTimer(g_hOsd, TIMER_OSD_HIDE, 1800, NULL);
}

int GetBatteryStyle()
{
    DWORD v = RegGetDword(L"BatteryStyle", 1);   /* 默认：数字样式（常驻显示百分比） */
    return (int)(v % 2);
}

void CycleBatteryStyle()
{
    int s = (GetBatteryStyle() + 1) % 2;
    RegSetDword(L"BatteryStyle", (DWORD)s);
}

bool IsAutoRunEnabled()
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    wchar_t val[MAX_PATH] = {0};
    DWORD cb = sizeof(val), type = 0;
    bool found = (RegQueryValueExW(k, kRunValue, NULL, &type, (LPBYTE)val, &cb) == ERROR_SUCCESS);
    RegCloseKey(k);
    return found;
}

void ToggleAutoRun()
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, NULL, 0,
                        KEY_ALL_ACCESS, NULL, &k, NULL) != ERROR_SUCCESS) return;

    if (IsAutoRunEnabled()) {
        RegDeleteValueW(k, kRunValue);
    } else {
        wchar_t path[MAX_PATH] = {0};
        GetModuleFileNameW(NULL, path, MAX_PATH);
        wchar_t quoted[MAX_PATH + 8];
        swprintf(quoted, MAX_PATH + 8, L"\"%ls\"", path);
        RegSetValueExW(k, kRunValue, 0, REG_SZ,
                       (const BYTE *)quoted, (DWORD)((wcslen(quoted) + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(k);
}

}  // namespace TrayUi
