/*
 * query_il.c — 查询进程完整性级别（Integrity Level）与桌面归属
 * ---------------------------------------------------------------------------
 * 用途：诊断 Shell_NotifyIconW(NIM_ADD) 返回 ERROR_ACCESS_DENIED (5) 的原因。
 * 托盘图标是通过向 explorer 的通知区域窗口发送消息实现的；若 explorer 的完整性
 * 级别高于调用进程，UIPI 会拦住该消息，API 就返回 ACCESS_DENIED。
 *
 * 编译： gcc -O2 -o query_il.exe query_il.c -ladvapi32
 * 用法： query_il.exe            # 列出当前进程 + explorer 的级别
 *        query_il.exe <pid>      # 指定进程
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static const char *IlName(DWORD rid)
{
    switch (rid) {
    case 0x0000: return "Untrusted";
    case 0x1000: return "Low";
    case 0x2000: return "Medium";
    case 0x2100: return "Medium-Plus";
    case 0x3000: return "High";
    case 0x4000: return "System";
    case 0x5000: return "Protected";
    default:     return "Unknown";
    }
}

static void Report(DWORD pid)
{
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (p == NULL) {
        printf("  PID %-6lu : 无法打开进程 (err=%lu)\n", (unsigned long)pid, GetLastError());
        return;
    }
    /* GDI / USER 对象数：长期运行的程序若有泄漏，这两个数字会持续增长 */
    DWORD gdi = 0, user = 0;
    {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        if (u32) {
            typedef DWORD (WINAPI *PFN_GetGuiResources)(HANDLE, DWORD);
            PFN_GetGuiResources fn =
                (PFN_GetGuiResources)(void *)GetProcAddress(u32, "GetGuiResources");
            if (fn) { gdi = fn(p, 0); user = fn(p, 1); }
        }
    }

    /* 内存不在这里报：PowerShell 的 PrivateMemorySize64 / WorkingSet64 更可靠，
     * 本工具只补 GDI/USER 对象计数（PS 拿不到）。 */

    HANDLE tok = NULL;
    if (!OpenProcessToken(p, TOKEN_QUERY, &tok)) {
        printf("  PID %-6lu : 无法打开令牌 (err=%lu)\n", (unsigned long)pid, GetLastError());
        CloseHandle(p);
        return;
    }
    DWORD sz = 0;
    GetTokenInformation(tok, TokenIntegrityLevel, NULL, 0, &sz);
    TOKEN_MANDATORY_LABEL *tml = (TOKEN_MANDATORY_LABEL *)malloc(sz ? sz : 64);
    if (tml != NULL && GetTokenInformation(tok, TokenIntegrityLevel, tml, sz, &sz)) {
        DWORD rid = *GetSidSubAuthority(tml->Label.Sid,
                        (DWORD)(*GetSidSubAuthorityCount(tml->Label.Sid) - 1));
        printf("  PID %-6lu : IL = 0x%04lX (%s)\n",
               (unsigned long)pid, (unsigned long)rid, IlName(rid));
    } else {
        printf("  PID %-6lu : 查询完整性级别失败 (err=%lu)\n",
               (unsigned long)pid, GetLastError());
    }
    printf("              GDI 对象 = %lu   USER 对象 = %lu\n",
           (unsigned long)gdi, (unsigned long)user);
    free(tml);
    CloseHandle(tok);
    CloseHandle(p);
}

int main(int argc, char **argv)
{
    SetConsoleOutputCP(CP_UTF8);

    /* 当前进程的窗口站与桌面，用于确认是否在交互桌面上 */
    HWINSTA ws = GetProcessWindowStation();
    HDESK   dk = GetThreadDesktop(GetCurrentThreadId());
    char wsName[128] = {0}, dkName[128] = {0};
    DWORD n = 0;
    if (ws) GetUserObjectInformationA(ws, UOI_NAME, wsName, sizeof(wsName), &n);
    if (dk) GetUserObjectInformationA(dk, UOI_NAME, dkName, sizeof(dkName), &n);

    printf("=== 进程完整性级别与桌面 ===\n");
    printf("当前窗口站/桌面 : %s\\%s\n", wsName[0] ? wsName : "?", dkName[0] ? dkName : "?");
    printf("Shell_TrayWnd   : %p\n\n", (void *)FindWindowW(L"Shell_TrayWnd", NULL));

    if (argc > 1) {
        for (int i = 1; i < argc; i++) Report((DWORD)strtoul(argv[i], NULL, 10));
    } else {
        Report(GetCurrentProcessId());
        /* 找 explorer */
        HWND h = FindWindowW(L"Shell_TrayWnd", NULL);
        DWORD pid = 0;
        if (h) GetWindowThreadProcessId(h, &pid);
        if (pid) Report(pid);
    }
    return 0;
}
