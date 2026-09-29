/*
 * mchose-tray HID 嗅探器 (hid_sniff)
 * ---------------------------------------------------------------------------
 * 只读侦察工具，用于确定迈从（MCHOSE）鼠标私有控制通道的报文语法。
 *
 *   1. 枚举 HID 顶层集合，按 VID/PID 子串过滤；
 *   2. 对厂商自定义集合（UsagePage >= 0xFF00）打印报文能力摘要；
 *   3. --feature：对每个 Feature Report ID 各做一次 HidD_GetFeature（只读），
 *      打印 hexdump —— 这是判断「谁回应、回应什么」最安全的手段；
 *   4. --listen <秒>：以重叠 IO 被动读取 Input Report，打印每条报文与时间戳；
 *   5. --set-feature <rid> <hex...>：显式下发一次 Feature SET（默认关闭，
 *      仅在确认语义后手动使用）。
 *
 * 默认行为：不做任何写操作。
 *
 * 编译（MinGW-w64）：
 *   gcc -O2 -Wall -o hid_sniff.exe hid_sniff.c -lhid -lsetupapi
 *
 * 用法：
 *   hid_sniff.exe 5253                       # 只打印能力摘要
 *   hid_sniff.exe 5253 --feature             # 逐个 GET 特性报文
 *   hid_sniff.exe 5253 --listen 20           # 被动监听输入报文 20 秒
 *   hid_sniff.exe 5253 --feature --listen 20
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* MinGW-w64 的 hidsdi.h 不声明该 GUID，按 Windows SDK 的定义补上 */
#ifndef GUID_DEVINTERFACE_HID_DEFINED
static const GUID GUID_DEVINTERFACE_HID_LOCAL =
    { 0x4D1E55B2, 0xF16F, 0x11CF, { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };
#define GUID_DEVINTERFACE_HID GUID_DEVINTERFACE_HID_LOCAL
#endif

/* ------------------------------------------------------------------ 工具 */

static void w2a(const wchar_t *src, char *dst, int dstLen)
{
    if (src == NULL) { dst[0] = '\0'; return; }
    WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dstLen, NULL, NULL);
}

/* 单行 hexdump：AA BB CC ... */
static void hexline(const unsigned char *p, DWORD n)
{
    for (DWORD i = 0; i < n; i++) printf("%02X ", p[i]);
    printf("\n");
}

/* 单行可打印视图 */
static void asciiline(const unsigned char *p, DWORD n)
{
    printf("        ascii: ");
    for (DWORD i = 0; i < n; i++) {
        unsigned char c = p[i];
        printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
    }
    printf("\n");
}

static int upperContains(const char *haystack, const char *needle)
{
    char h[512], n[256];
    strncpy(h, haystack, sizeof(h) - 1); h[sizeof(h) - 1] = '\0';
    strncpy(n, needle, sizeof(n) - 1);   n[sizeof(n) - 1] = '\0';
    for (char *p = h; *p; p++) *p = (char)toupper((unsigned char)*p);
    for (char *p = n; *p; p++) *p = (char)toupper((unsigned char)*p);
    return strstr(h, n) != NULL;
}

/* 收集某类型报文的 Report ID 与「负载字节数」 */
typedef struct {
    unsigned char rid;
    unsigned      payloadBytes;   /* 不含 Report ID */
} ReportInfo;

static int collectReports(HANDLE h, PHIDP_PREPARSED_DATA pp, HIDP_REPORT_TYPE type,
                          USHORT declared, ReportInfo *out, int maxOut)
{
    if (declared == 0) return 0;

    HIDP_VALUE_CAPS *caps = (HIDP_VALUE_CAPS *)calloc(declared, sizeof(HIDP_VALUE_CAPS));
    if (caps == NULL) return 0;

    USHORT len = declared;
    int n = 0;
    if (HidP_GetValueCaps(type, caps, &len, pp) == HIDP_STATUS_SUCCESS) {
        for (USHORT i = 0; i < len && n < maxOut; i++) {
            unsigned bytes = (unsigned)(caps[i].BitSize / 8) * caps[i].ReportCount;
            /* 同一 rid 可能有多段字段，取累加（这里按最大值处理即可满足侦察需求） */
            int found = -1;
            for (int k = 0; k < n; k++) if (out[k].rid == caps[i].ReportID) { found = k; break; }
            if (found >= 0) {
                if (bytes > out[found].payloadBytes) out[found].payloadBytes = bytes;
            } else {
                out[n].rid = caps[i].ReportID;
                out[n].payloadBytes = bytes;
                n++;
            }
        }
    }
    free(caps);
    return n;
}

/* ------------------------------------------------------------- 输入监听 */

static void listenInput(HANDLE h, USHORT reportLen, int seconds, const char *tag)
{
    if (reportLen == 0) return;

    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (ov.hEvent == NULL) return;

    unsigned char *buf = (unsigned char *)malloc(reportLen);
    if (buf == NULL) { CloseHandle(ov.hEvent); return; }

    printf("    >>> 开始监听输入报文 %d 秒（请移动鼠标 / 按 DPI 键 / 切回报率触发事件）...\n",
           seconds);
    DWORD start = GetTickCount();
    int count = 0;

    while ((int)((GetTickCount() - start) / 1000) < seconds) {
        ResetEvent(ov.hEvent);
        DWORD got = 0;
        BOOL ok = ReadFile(h, buf, reportLen, &got, &ov);

        if (!ok && GetLastError() != ERROR_IO_PENDING) {
            printf("    ReadFile 失败: %lu\n", GetLastError());
            break;
        }
        if (!ok) {
            DWORD w = WaitForSingleObject(ov.hEvent, 700);
            if (w != WAIT_OBJECT_0) {
                CancelIo(h);
                WaitForSingleObject(ov.hEvent, 1000);
                continue;
            }
            if (!GetOverlappedResult(h, &ov, &got, FALSE)) continue;
        } else {
            got = reportLen;
        }

        count++;
        printf("      [%s %6lu ms] #%-4d len=%2lu : ",
               tag, (unsigned long)(GetTickCount() - start), count, (unsigned long)got);
        hexline(buf, got);
        if (got >= 8) asciiline(buf, got);
    }

    printf("    <<< 监听结束，共 %d 条报文\n", count);
    free(buf);
    CloseHandle(ov.hEvent);
}

/* ------------------------------------------------------------------- 主体 */

int main(int argc, char **argv)
{
    SetConsoleOutputCP(CP_UTF8);

    const char *filter   = NULL;
    int  doFeature       = 0;
    int  listenSeconds   = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--feature") == 0) {
            doFeature = 1;
        } else if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
            listenSeconds = atoi(argv[++i]);
        } else if (filter == NULL) {
            filter = argv[i];
        }
    }
    if (filter == NULL) filter = "";

    HDEVINFO devInfo = SetupDiGetClassDevs(&GUID_DEVINTERFACE_HID, NULL, NULL,
                                          DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        printf("SetupDiGetClassDevs failed: %lu\n", GetLastError());
        return 1;
    }

    printf("=== MCHOSE HID 嗅探 (filter=\"%s\", feature=%d, listen=%ds) ===\n\n",
           filter, doFeature, listenSeconds);

    int index = 0, hits = 0;
    SP_DEVICE_INTERFACE_DATA ifData;
    ifData.cbSize = sizeof(ifData);

    while (SetupDiEnumDeviceInterfaces(devInfo, NULL, &GUID_DEVINTERFACE_HID,
                                       index++, &ifData)) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, NULL, 0, &needed, NULL);
        if (needed == 0) continue;

        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(needed);
        if (detail == NULL) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        SP_DEVINFO_DATA infoData;
        infoData.cbSize = sizeof(infoData);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, needed,
                                              &needed, &infoData)) {
            free(detail);
            continue;
        }

        wchar_t instanceIdW[512] = {0};
        char    instanceId[512]  = {0};
        if (!SetupDiGetDeviceInstanceIdW(devInfo, &infoData, instanceIdW, 512, NULL))
            wcscpy(instanceIdW, L"(unknown)");
        w2a(instanceIdW, instanceId, sizeof(instanceId));

        if (!upperContains(instanceId, filter)) { free(detail); continue; }

        HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            h = CreateFileW(detail->DevicePath, GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        }
        if (h == INVALID_HANDLE_VALUE) { free(detail); continue; }

        PHIDP_PREPARSED_DATA pp = NULL;
        if (!HidD_GetPreparsedData(h, &pp)) {
            CloseHandle(h); free(detail); continue;
        }
        HIDP_CAPS caps;
        memset(&caps, 0, sizeof(caps));
        if (HidP_GetCaps(pp, &caps) != HIDP_STATUS_SUCCESS) {
            HidD_FreePreparsedData(pp); CloseHandle(h); free(detail); continue;
        }

        /* 只关心厂商自定义集合 */
        if (caps.UsagePage < 0xFF00) {
            HidD_FreePreparsedData(pp); CloseHandle(h); free(detail); continue;
        }

        hits++;
        printf("================================================================\n");
        printf("[%d] %s\n", hits, instanceId);
        printf("    UsagePage=0x%04X Usage=0x%04X  In=%u Out=%u Feat=%u bytes\n",
               caps.UsagePage, caps.Usage,
               caps.InputReportByteLength, caps.OutputReportByteLength,
               caps.FeatureReportByteLength);

        ReportInfo feat[32], in[32], out[32];
        int nFeat = collectReports(h, pp, HidP_Feature, caps.NumberFeatureValueCaps, feat, 32);
        int nIn   = collectReports(h, pp, HidP_Input,   caps.NumberInputValueCaps,   in,   32);
        int nOut  = collectReports(h, pp, HidP_Output,  caps.NumberOutputValueCaps,  out,  32);

        printf("    Feature IDs : ");
        for (int i = 0; i < nFeat; i++) printf("0x%02X(%u) ", feat[i].rid, feat[i].payloadBytes);
        printf("\n    Input IDs   : ");
        for (int i = 0; i < nIn; i++) printf("0x%02X(%u) ", in[i].rid, in[i].payloadBytes);
        printf("\n    Output IDs  : ");
        for (int i = 0; i < nOut; i++) printf("0x%02X(%u) ", out[i].rid, out[i].payloadBytes);
        printf("\n");

        /* ---- 逐个 GET 特性报文 ---- */
        if (doFeature && nFeat > 0) {
            printf("\n    ---- Feature GET 结果 ----\n");
            for (int i = 0; i < nFeat; i++) {
                unsigned len = feat[i].payloadBytes + 1;
                if (len > 512) len = 512;
                if (len < caps.FeatureReportByteLength) len = caps.FeatureReportByteLength;
                if (len > 512) len = 512;

                unsigned char buf[512];
                memset(buf, 0xCC, sizeof(buf));
                buf[0] = feat[i].rid;

                SetLastError(0);
                if (HidD_GetFeature(h, buf, len)) {
                    printf("      rid=0x%02X len=%u : ", feat[i].rid, len);
                    hexline(buf, len);
                    asciiline(buf, len);
                } else {
                    printf("      rid=0x%02X len=%u : FAILED (err=%lu)\n",
                           feat[i].rid, len, GetLastError());
                }
            }
        }

        /* ---- 被动监听输入报文 ---- */
        if (listenSeconds > 0) {
            printf("\n");
            char tag[16];
            snprintf(tag, sizeof(tag), "%04X/%04X", caps.UsagePage, caps.Usage);
            listenInput(h, caps.InputReportByteLength, listenSeconds, tag);
        }

        printf("\n");
        HidD_FreePreparsedData(pp);
        CloseHandle(h);
        free(detail);
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    printf("=== 命中 %d 个厂商自定义集合 ===\n", hits);
    return 0;
}
