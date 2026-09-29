/*
 * mchose_monitor — 迈从（MCHOSE）鼠标「设备主动推送」监听器
 * ---------------------------------------------------------------------------
 * 背景：同方案的迈从瑞昱芯片鼠标（L7 Pro / K7 Ultra）由设备**主动推送**
 *       input report 0x13（usage page 0xFF01 集合），负载逐字节 XOR 0xFF。
 *       负载布局（来自两处独立实测的结构对齐）：
 *         [0] 子类型（L7 Pro=0x1D，K7 Ultra=0xE2）——不要写死
 *         [1][2] 协议版本
 *         [3] 充电/座充状态
 *         [4] 电量百分比 0..100
 *         [5] 未知
 *         [6] 回报率档位索引 0..5
 *         [7] 未知
 *         [8] 型号代码
 *         [9..] 型号名 ASCII（如 "L7 Pro"）
 *
 *       发送 feature report [0x11, 0xF9, 0xFF x19]（即 11 06 取反后的读设备信息
 *       命令）可「nudge」设备立即推送一次。
 *
 * 本工具用于在 MCHOSE A7 Pro 上验证上述推送格式；纯只读，不修改任何配置。
 *
 * 编译： gcc -O2 -Wall -o mchose_monitor.exe mchose_monitor.c -lhid -lsetupapi
 * 用法： mchose_monitor.exe [监听秒数=20] [--no-nudge]
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GUID_DEVINTERFACE_HID_DEFINED
static const GUID GUID_DEVINTERFACE_HID_LOCAL =
    { 0x4D1E55B2, 0xF16F, 0x11CF, { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };
#define GUID_DEVINTERFACE_HID GUID_DEVINTERFACE_HID_LOCAL
#endif

#define TARGET_VID 0x5253
#define TARGET_UP  0xFF01

static void w2a(const wchar_t *src, char *dst, int dstLen)
{
    if (src == NULL) { dst[0] = '\0'; return; }
    WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dstLen, NULL, NULL);
}

static void hexline(const char *tag, const unsigned char *p, int n)
{
    printf("%s", tag);
    for (int i = 0; i < n; i++) printf("%02X ", p[i]);
    printf("\n");
}

/* 打开 0xFF01/0x0001 集合（重叠 IO，便于超时读） */
static HANDLE openCollection(USHORT *outInputLen)
{
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO devInfo = SetupDiGetClassDevsW(&hidGuid, NULL, NULL,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    SP_DEVICE_INTERFACE_DATA ifData;
    ifData.cbSize = sizeof(ifData);
    HANDLE found = INVALID_HANDLE_VALUE;

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, NULL, &hidGuid, i, &ifData); i++) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, NULL, 0, &needed, NULL);
        if (needed == 0) continue;

        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(needed);
        if (detail == NULL) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, needed, &needed, NULL)) {
            free(detail); continue;
        }

        char path[1024];
        w2a(detail->DevicePath, path, sizeof(path));
        if (strstr(path, "vid_5253") == NULL) { free(detail); continue; }

        HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (h == INVALID_HANDLE_VALUE) { free(detail); continue; }

        HIDD_ATTRIBUTES attr;
        attr.Size = sizeof(attr);
        PHIDP_PREPARSED_DATA pp = NULL;
        HIDP_CAPS caps;
        if (HidD_GetAttributes(h, &attr) && attr.VendorID == TARGET_VID &&
            HidD_GetPreparsedData(h, &pp)) {
            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS &&
                caps.UsagePage == TARGET_UP && caps.Usage == 0x0001) {
                printf("命中集合: UsagePage=0x%04X Usage=0x%04X Input=%u Feature=%u\n",
                       caps.UsagePage, caps.Usage,
                       caps.InputReportByteLength, caps.FeatureReportByteLength);
                printf("路径: %s\n\n", path);
                if (outInputLen) *outInputLen = caps.InputReportByteLength;
                HidD_FreePreparsedData(pp);
                found = h;
                free(detail);
                break;
            }
            HidD_FreePreparsedData(pp);
        }
        CloseHandle(h);
        free(detail);
    }
    SetupDiDestroyDeviceInfoList(devInfo);
    return found;
}

/* 发送 nudge：11 06 的取反形式（读设备信息，触发一次推送） */
static void sendNudge(HANDLE h)
{
    unsigned char req[21];
    req[0] = 0x11;
    req[1] = 0x06 ^ 0xFF;           /* 0xF9 */
    for (int i = 2; i < 21; i++) req[i] = 0xFF;
    if (HidD_SetFeature(h, req, sizeof(req)))
        printf("[nudge] 已发送 11 06 读设备信息（触发推送）\n\n");
    else
        printf("[nudge] 发送失败: %lu\n\n", GetLastError());
}

int main(int argc, char **argv)
{
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, NULL, _IONBF, 0);

    int seconds = 20;
    int noNudge = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-nudge") == 0) noNudge = 1;
        else if (atoi(argv[i]) > 0) seconds = atoi(argv[i]);
    }

    printf("=== mchose_monitor — 推送报文监听 %d 秒%s ===\n\n",
           seconds, noNudge ? "（不发送 nudge）" : "");

    USHORT inputLen = 65;
    HANDLE h = openCollection(&inputLen);
    if (h == INVALID_HANDLE_VALUE) { printf("未找到 0xFF01 控制集合\n"); return 1; }

    if (!noNudge) sendNudge(h);

    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    unsigned char *buf = (unsigned char *)malloc(inputLen);
    unsigned char decoded[128];

    DWORD start = GetTickCount();
    int count = 0, pushes13 = 0;
    int lastBattery = -1;

    while ((int)((GetTickCount() - start) / 1000) < seconds) {
        ResetEvent(ov.hEvent);
        DWORD got = 0;
        BOOL ok = ReadFile(h, buf, inputLen, &got, &ov);

        if (!ok) {
            if (GetLastError() != ERROR_IO_PENDING) {
                printf("ReadFile 失败: %lu\n", GetLastError());
                break;
            }
            DWORD w = WaitForSingleObject(ov.hEvent, 1000);
            if (w != WAIT_OBJECT_0) { CancelIo(h); WaitForSingleObject(ov.hEvent, 1000); continue; }
            if (!GetOverlappedResult(h, &ov, &got, FALSE)) continue;
        } else {
            got = inputLen;
        }

        count++;
        DWORD ms = GetTickCount() - start;
        printf("[%6lu ms] #%-3d len=%2lu rid=0x%02X\n", (unsigned long)ms, count,
               (unsigned long)got, buf[0]);
        hexline("           原始 : ", buf, (int)got);

        int payloadLen = (int)got - 1;
        if (payloadLen > (int)sizeof(decoded)) payloadLen = (int)sizeof(decoded);
        for (int i = 0; i < payloadLen; i++) decoded[i] = (unsigned char)(buf[1 + i] ^ 0xFF);
        hexline("           解码 : ", decoded, payloadLen);

        if (buf[0] == 0x13) {
            pushes13++;
            printf("           >>> report 0x13  subtype=0x%02X charge=%u battery=%u%% rateIdx=%u modelCode=0x%02X name=\"",
                   decoded[0], decoded[3], decoded[4], decoded[6], decoded[8]);
            for (int i = 9; i < payloadLen; i++) {
                unsigned char c = decoded[i];
                if (c == 0) break;
                printf("%c", (c >= 32 && c < 127) ? c : '.');
            }
            printf("\"\n");
            if ((int)decoded[4] != lastBattery) {
                printf("           >>> 电量变化: %d%% -> %u%%\n", lastBattery, decoded[4]);
                lastBattery = decoded[4];
            }
        }
    }

    printf("\n=== 统计: 共 %d 条报文，其中 report 0x13 共 %d 条 ===\n", count, pushes13);
    if (pushes13 == 0)
        printf("    未捕获到 0x13 推送：该机型可能不主动推送，或需要其它触发方式。\n");

    free(buf);
    CloseHandle(ov.hEvent);
    CloseHandle(h);
    return 0;
}
