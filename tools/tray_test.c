/*
 * tray_test.c — 最小托盘图标可行性测试
 * 目的：判断 Shell_NotifyIconW(NIM_ADD) 返回 ERROR_ACCESS_DENIED 是
 *       本程序代码问题，还是当前执行环境（沙箱/受限令牌）的限制。
 * 编译： gcc -O2 -municode -mwindows -o tray_test.exe tray_test.c -lshell32 -luser32
 * 结果写入 tray_test_result.txt（GUI 子系统无控制台）。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>

static void Report(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    HANDLE h = CreateFileW(L"tray_test_result.txt", FILE_APPEND_DATA, FILE_SHARE_READ,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, buf, (DWORD)n, &w, NULL);
        CloseHandle(h);
    }
}

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    DeleteFileW(L"tray_test_result.txt");

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Proc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"TrayTestWnd";
    if (!RegisterClassExW(&wc)) { Report("RegisterClassExW failed %lu\n", GetLastError()); return 1; }

    HWND hwnd = CreateWindowExW(0, L"TrayTestWnd", L"t", WS_POPUP, 0, 0, 0, 0,
                               NULL, NULL, hInst, NULL);
    if (!hwnd) { Report("CreateWindowExW failed %lu\n", GetLastError()); return 1; }
    Report("窗口创建 OK, Shell_TrayWnd=%p\n", (void *)FindWindowW(L"Shell_TrayWnd", NULL));

    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_APP + 1;
    nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wcscpy(nid.szTip, L"tray_test");

    SetLastError(0);
    BOOL ok = Shell_NotifyIconW(NIM_ADD, &nid);
    Report("sizeof(NOTIFYICONDATAW)=%u  NIM_ADD=%s  err=%lu\n",
           (unsigned)sizeof(nid), ok ? "成功" : "失败", GetLastError());

    if (ok) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        SetLastError(0);
        BOOL v = Shell_NotifyIconW(NIM_SETVERSION, &nid);
        Report("NIM_SETVERSION=%s err=%lu\n", v ? "成功" : "失败", GetLastError());

        MSG msg;
        DWORD start = GetTickCount();
        while (GetTickCount() - start < 3000) {
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(50);
        }
        Shell_NotifyIconW(NIM_DELETE, &nid);
        Report("已删除托盘图标，测试结束\n");
    }
    return 0;
}
