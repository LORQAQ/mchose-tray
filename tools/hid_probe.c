/*
 * mchose-tray HID 探针 (hid_probe)
 * ---------------------------------------------------------------------------
 * 用途：为迈从（MCHOSE）鼠标逆向私有控制协议做第一步侦察。
 *
 *   1. 通过 SetupAPI 枚举所有 HID 顶层集合（device interface, HID GUID）；
 *   2. 按 VID/PID 子串过滤，打印设备属性、厂商/产品字符串；
 *   3. 用 HidD_GetPreparsedData + HidP_GetCaps 打印每个集合的
 *      UsagePage / Usage / 输入·输出·特性报文长度；
 *   4. 用 HidP_GetValueCaps / HidP_GetButtonCaps 打印报文里每个字段的
 *      ReportID、UsagePage、Usage、位宽、报告数（等价于报告描述符的语义摘要）；
 *   5. 原样 hexdump 报告描述符（最权威的一手资料）；
 *   6. 可选：--read-feature <reportId> <len> 只读地 GET 一次特性报文。
 *
 * 本工具默认**不向设备写入任何东西**（只有显式 --read-feature 才会做一次读取，
 * Feature GET 是只读操作，不会改变鼠标配置）。
 *
 * 编译（MinGW-w64）：
 *   gcc -O2 -Wall -o hid_probe.exe hid_probe.c -lhid -lsetupapi
 *
 * 用法：
 *   hid_probe.exe                          # 列出所有 HID 集合
 *   hid_probe.exe 5253                     # 只看 VID/PID 含 "5253" 的
 *   hid_probe.exe 5253 --desc              # 额外完整 dump 报告描述符
 *   hid_probe.exe 5253 --read-feature 0x08 33
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>

/* MinGW-w64 的 hidsdi.h 不声明该 GUID，按 Windows SDK 的定义补上 */
#ifndef GUID_DEVINTERFACE_HID_DEFINED
static const GUID GUID_DEVINTERFACE_HID_LOCAL =
    { 0x4D1E55B2, 0xF16F, 0x11CF, { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };
#define GUID_DEVINTERFACE_HID GUID_DEVINTERFACE_HID_LOCAL
#endif

/* 读报告描述符：IOCTL_HID_GET_REPORT_DESCRIPTOR = CTL_CODE(FILE_DEVICE_KEYBOARD,0,NEITHER,ANY) */
#define IOCTL_HID_GET_REPORT_DESCRIPTOR 0x000B0003UL

typedef BOOLEAN (WINAPI *PFN_HidD_GetReportDescriptor)(HANDLE, PVOID, ULONG, PULONG);

/* 返回实际读到的描述符字节数；0 表示失败 */
static ULONG readReportDescriptor(HANDLE h, unsigned char *buf, ULONG bufLen)
{
    ULONG len = 0;

    /* 首选 hid.dll 导出的 HidD_GetReportDescriptor（SDK 头文件未声明，但确实导出） */
    HMODULE hid = GetModuleHandleW(L"hid.dll");
    if (hid == NULL) hid = LoadLibraryW(L"hid.dll");
    if (hid != NULL) {
        PFN_HidD_GetReportDescriptor fn =
            (PFN_HidD_GetReportDescriptor)(void *)GetProcAddress(hid, "HidD_GetReportDescriptor");
        if (fn != NULL && fn(h, buf, bufLen, &len)) return len;
        len = 0;
    }

    /* 退化路径：自己下发 IOCTL */
    if (DeviceIoControl(h, IOCTL_HID_GET_REPORT_DESCRIPTOR, NULL, 0,
                        buf, bufLen, &len, NULL)) return len;
    return 0;
}

/* ------------------------------------------------------------------ 工具函数 */

static void w2a(const wchar_t *src, char *dst, int dstLen)
{
    if (src == NULL) { dst[0] = '\0'; return; }
    WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dstLen, NULL, NULL);
}

static void hexdump(const unsigned char *p, DWORD n, const char *indent)
{
    for (DWORD i = 0; i < n; i += 16) {
        printf("%s%04lX  ", indent, (unsigned long)i);
        for (DWORD j = 0; j < 16; j++) {
            if (i + j < n) printf("%02X ", p[i + j]);
            else           printf("   ");
        }
        printf(" |");
        for (DWORD j = 0; j < 16 && i + j < n; j++) {
            unsigned char c = p[i + j];
            printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
        }
        printf("|\n");
    }
}

/* ------------------------------------------------- 报告字段（Value/Button）*/

static const char *reportTypeName(HIDP_REPORT_TYPE t)
{
    switch (t) {
    case HidP_Input:   return "Input  ";
    case HidP_Output:  return "Output ";
    case HidP_Feature: return "Feature";
    default:           return "Unknown";
    }
}

static void printValueCaps(HANDLE h, HIDP_REPORT_TYPE type, PHIDP_PREPARSED_DATA pp,
                           USHORT declaredLen)
{
    if (declaredLen == 0) return;

    HIDP_VALUE_CAPS *caps = (HIDP_VALUE_CAPS *)calloc(declaredLen, sizeof(HIDP_VALUE_CAPS));
    if (caps == NULL) return;

    USHORT len = declaredLen;
    NTSTATUS st = HidP_GetValueCaps(type, caps, &len, pp);
    if (st == HIDP_STATUS_SUCCESS) {
        for (USHORT i = 0; i < len; i++) {
            HIDP_VALUE_CAPS *c = &caps[i];
            if (c->IsRange) {
                printf("      %s rid=0x%02X up=0x%04X usage=0x%04X..0x%04X "
                       "bits=%u count=%u logical=%ld..%ld phys=%ld..%ld\n",
                       reportTypeName(type), c->ReportID, c->UsagePage,
                       c->Range.UsageMin, c->Range.UsageMax,
                       c->BitSize, c->ReportCount,
                       c->LogicalMin, c->LogicalMax, c->PhysicalMin, c->PhysicalMax);
            } else {
                printf("      %s rid=0x%02X up=0x%04X usage=0x%04X          "
                       "bits=%u count=%u logical=%ld..%ld phys=%ld..%ld\n",
                       reportTypeName(type), c->ReportID, c->UsagePage,
                       c->NotRange.Usage,
                       c->BitSize, c->ReportCount,
                       c->LogicalMin, c->LogicalMax, c->PhysicalMin, c->PhysicalMax);
            }
        }
    } else {
        printf("      %s HidP_GetValueCaps failed: 0x%08lX\n",
               reportTypeName(type), (unsigned long)st);
    }
    free(caps);
}

static void printButtonCaps(HANDLE h, HIDP_REPORT_TYPE type, PHIDP_PREPARSED_DATA pp,
                            USHORT declaredLen)
{
    if (declaredLen == 0) return;

    HIDP_BUTTON_CAPS *caps = (HIDP_BUTTON_CAPS *)calloc(declaredLen, sizeof(HIDP_BUTTON_CAPS));
    if (caps == NULL) return;

    USHORT len = declaredLen;
    NTSTATUS st = HidP_GetButtonCaps(type, caps, &len, pp);
    if (st == HIDP_STATUS_SUCCESS) {
        for (USHORT i = 0; i < len; i++) {
            HIDP_BUTTON_CAPS *c = &caps[i];
            if (c->IsRange) {
                printf("      %s rid=0x%02X up=0x%04X button=0x%04X..0x%04X\n",
                       reportTypeName(type), c->ReportID, c->UsagePage,
                       c->Range.UsageMin, c->Range.UsageMax);
            } else {
                printf("      %s rid=0x%02X up=0x%04X button=0x%04X\n",
                       reportTypeName(type), c->ReportID, c->UsagePage,
                       c->NotRange.Usage);
            }
        }
    }
    free(caps);
}

/* ------------------------------------------------------------------- 主体 */

int main(int argc, char **argv)
{
    SetConsoleOutputCP(CP_UTF8);

    const char *filter    = NULL;
    int         wantDesc  = 0;
    int         readFeat  = 0;
    unsigned    featId    = 0;
    unsigned    featLen   = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--desc") == 0) {
            wantDesc = 1;
        } else if (strcmp(argv[i], "--read-feature") == 0 && i + 2 < argc) {
            readFeat = 1;
            featId   = (unsigned)strtoul(argv[++i], NULL, 0);
            featLen  = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (filter == NULL) {
            filter = argv[i];
        }
    }

    HDEVINFO devInfo = SetupDiGetClassDevs(&GUID_DEVINTERFACE_HID, NULL, NULL,
                                          DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        printf("SetupDiGetClassDevs failed: %lu\n", GetLastError());
        return 1;
    }

    printf("=== HID 顶层集合枚举 %s ===\n\n",
           filter ? (filter) : "(全部)");

    int index = 0, shown = 0;
    SP_DEVICE_INTERFACE_DATA ifData;
    ifData.cbSize = sizeof(ifData);

    while (SetupDiEnumDeviceInterfaces(devInfo, NULL, &GUID_DEVINTERFACE_HID,
                                       index++, &ifData)) {

        /* 取得接口路径 */
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

        /* 设备实例 ID（例如 HID\VID_5253&PID_1021&MI_02&COL01\...） */
        wchar_t instanceIdW[512] = {0};
        char    instanceId[512]  = {0};
        if (!SetupDiGetDeviceInstanceIdW(devInfo, &infoData, instanceIdW,
                                         512, NULL)) {
            wcscpy(instanceIdW, L"(unknown)");
        }
        w2a(instanceIdW, instanceId, sizeof(instanceId));

        /* 过滤 */
        if (filter != NULL) {
            char upperId[512];
            strncpy(upperId, instanceId, sizeof(upperId) - 1);
            upperId[sizeof(upperId) - 1] = '\0';
            for (char *p = upperId; *p; p++) *p = (char)toupper((unsigned char)*p);

            char upperFilter[256];
            strncpy(upperFilter, filter, sizeof(upperFilter) - 1);
            upperFilter[sizeof(upperFilter) - 1] = '\0';
            for (char *p = upperFilter; *p; p++) *p = (char)toupper((unsigned char)*p);

            if (strstr(upperId, upperFilter) == NULL) {
                free(detail);
                continue;
            }
        }

        char path[1024];
        w2a(detail->DevicePath, path, sizeof(path));
        shown++;

        printf("----------------------------------------------------------------\n");
        printf("[%d] %s\n", shown, instanceId);
        printf("    path : %s\n", path);

        /* 打开设备：先尝试读写，失败再退化为查询 */
        HANDLE h = CreateFileW(detail->DevicePath,
                               GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        const char *access = "READ|WRITE";
        if (h == INVALID_HANDLE_VALUE) {
            h = CreateFileW(detail->DevicePath, GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, 0, NULL);
            access = "READ only";
        }
        if (h == INVALID_HANDLE_VALUE) {
            h = CreateFileW(detail->DevicePath, 0,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, 0, NULL);
            access = "query only";
        }

        if (h == INVALID_HANDLE_VALUE) {
            printf("    open : FAILED (GetLastError=%lu)\n\n", GetLastError());
            free(detail);
            continue;
        }
        printf("    open : ok (%s)\n", access);

        /* VID / PID / 版本 */
        HIDD_ATTRIBUTES attr;
        attr.Size = sizeof(attr);
        if (HidD_GetAttributes(h, &attr)) {
            printf("    ids  : VID=%04X PID=%04X VER=%04X\n",
                   attr.VendorID, attr.ProductID, attr.VersionNumber);
        }

        /* 字符串描述符 */
        wchar_t wbuf[256];
        char    abuf[512];
        if (HidD_GetManufacturerString(h, wbuf, sizeof(wbuf))) {
            w2a(wbuf, abuf, sizeof(abuf));
            printf("    mfg  : %s\n", abuf);
        }
        if (HidD_GetProductString(h, wbuf, sizeof(wbuf))) {
            w2a(wbuf, abuf, sizeof(abuf));
            printf("    prod : %s\n", abuf);
        }
        if (HidD_GetSerialNumberString(h, wbuf, sizeof(wbuf))) {
            w2a(wbuf, abuf, sizeof(abuf));
            printf("    sn   : %s\n", abuf);
        }

        /* HIDP_CAPS */
        PHIDP_PREPARSED_DATA pp = NULL;
        if (!HidD_GetPreparsedData(h, &pp)) {
            printf("    HidD_GetPreparsedData FAILED (%lu)\n\n", GetLastError());
            CloseHandle(h);
            free(detail);
            continue;
        }

        HIDP_CAPS caps;
        memset(&caps, 0, sizeof(caps));
        if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
            printf("    ---- HIDP_CAPS ----\n");
            printf("    UsagePage          : 0x%04X\n", caps.UsagePage);
            printf("    Usage              : 0x%04X\n", caps.Usage);
            printf("    InputReportBytes   : %u\n", caps.InputReportByteLength);
            printf("    OutputReportBytes  : %u\n", caps.OutputReportByteLength);
            printf("    FeatureReportBytes : %u\n", caps.FeatureReportByteLength);
            printf("    linkCollections    : %u\n", caps.NumberLinkCollectionNodes);
            printf("    valueCaps in/out/feat : %u / %u / %u\n",
                   caps.NumberInputValueCaps, caps.NumberOutputValueCaps,
                   caps.NumberFeatureValueCaps);
            printf("    buttonCaps in/out/feat: %u / %u / %u\n",
                   caps.NumberInputButtonCaps, caps.NumberOutputButtonCaps,
                   caps.NumberFeatureButtonCaps);

            printf("    ---- 报告字段 ----\n");
            printValueCaps(h, HidP_Input,   pp, caps.NumberInputValueCaps);
            printValueCaps(h, HidP_Output,  pp, caps.NumberOutputValueCaps);
            printValueCaps(h, HidP_Feature, pp, caps.NumberFeatureValueCaps);
            printButtonCaps(h, HidP_Input,   pp, caps.NumberInputButtonCaps);
            printButtonCaps(h, HidP_Output,  pp, caps.NumberOutputButtonCaps);
            printButtonCaps(h, HidP_Feature, pp, caps.NumberFeatureButtonCaps);
        } else {
            printf("    HidP_GetCaps FAILED\n");
        }

        /* 报告描述符原文 */
        if (wantDesc) {
            unsigned char desc[4096];
            ULONG descLen = readReportDescriptor(h, desc, sizeof(desc));
            if (descLen > 0) {
                printf("    ---- 报告描述符 (%lu 字节) ----\n", (unsigned long)descLen);
                hexdump(desc, descLen, "      ");
            } else {
                printf("    readReportDescriptor FAILED (GetLastError=%lu)\n", GetLastError());
            }
        }

        /* 只读 GET 一次特性报文 */
        if (readFeat && featLen > 0 && featLen <= 512) {
            unsigned char buf[512];
            memset(buf, 0, sizeof(buf));
            buf[0] = (unsigned char)featId;
            if (HidD_GetFeature(h, buf, featLen)) {
                printf("    ---- Feature GET rid=0x%02X len=%u ----\n", featId, featLen);
                hexdump(buf, featLen, "      ");
            } else {
                printf("    Feature GET rid=0x%02X len=%u FAILED (%lu)\n",
                       featId, featLen, GetLastError());
            }
        }

        HidD_FreePreparsedData(pp);
        CloseHandle(h);
        free(detail);
        printf("\n");
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    printf("=== 命中 %d 个接口 ===\n", shown);
    return 0;
}
