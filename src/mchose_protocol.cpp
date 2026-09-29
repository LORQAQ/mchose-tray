/*
 * mchose_protocol.cpp — 迈从（MCHOSE）鼠标 HID 协议实现
 * 见 mchose_protocol.h 顶部的协议说明与 docs/PROTOCOL.md。
 */

#include "mchose_protocol.h"

#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* MinGW-w64 的 hidsdi.h 不声明该 GUID */
#ifndef GUID_DEVINTERFACE_HID_DEFINED
static const GUID GUID_DEVINTERFACE_HID_LOCAL =
    { 0x4D1E55B2, 0xF16F, 0x11CF, { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };
#define GUID_DEVINTERFACE_HID GUID_DEVINTERFACE_HID_LOCAL
#endif

namespace McHose {

/* ------------------------------------------------------------ 小工具 */

static unsigned rdU16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned rdU32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static void w2a(const wchar_t *src, char *dst, int dstLen)
{
    if (src == NULL) { if (dstLen > 0) dst[0] = '\0'; return; }
    WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dstLen, NULL, NULL);
}

static void dbgBytes(const char *tag, const unsigned char *p, int n)
{
    printf("    [%s] ", tag);
    for (int i = 0; i < n; i++) printf("%02X ", p[i]);
    printf("\n");
}

/* ------------------------------------------------------------ 设备枚举 */

/*
 * 枚举 HID 顶层集合，找到 UsagePage = 0xFF01 / Usage = 0x0001 且 VID/PID 匹配的集合。
 * 厂商与机型判定交给 model_db：接受任何已知迈从 VID（实测 0x5253、社区 0x3837），
 * PID 不再写死——不同机型/不同连接模式枚举出的 PID 各不相同，且 PID 会跨机型复用。
 */
/* 最近一次成功匹配的控制集合的接口 VID/PID。
 * 机型识别按它查表——不是 11 06 回报的"机体 PID"（那是设备类型码，A7 Pro 报 0x0010）。 */
static unsigned short g_lastIfaceVid = 0;
static unsigned short g_lastIfacePid = 0;

bool GetLastOpenedInterfaceIds(unsigned short *vid, unsigned short *pid)
{
    if (g_lastIfaceVid == 0) return false;
    if (vid) *vid = g_lastIfaceVid;
    if (pid) *pid = g_lastIfacePid;
    return true;
}

HANDLE OpenControlDeviceEx(bool overlapped, char *outPath, int outPathLen)
{
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO devInfo = SetupDiGetClassDevsW(&hidGuid, NULL, NULL,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    SP_DEVICE_INTERFACE_DATA ifData;
    ifData.cbSize = sizeof(ifData);
    HANDLE found = INVALID_HANDLE_VALUE;
    int    bestFeatureLen = 0;

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, NULL, &hidGuid, i, &ifData); i++) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, NULL, 0, &needed, NULL);
        if (needed == 0) continue;

        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(needed);
        if (detail == NULL) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, needed, &needed, NULL)) {
            free(detail);
            continue;
        }

        /* 先用路径粗筛，避免打开无关设备 */
        char path[1024];
        w2a(detail->DevicePath, path, sizeof(path));
        /* 路径粗筛：只打开迈从（Realtek 方案）的 HID 集合。
         * 实测 A7 Pro 是 vid_5253，社区记录 A7 V2 Ultra / K7 Ultra 是 vid_3837，
         * 只认 0x5253 会整条新产品线都发现不了。 */
        if (!PathLooksLikeMchose(detail->DevicePath)) { free(detail); continue; }

        HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING,
                               overlapped ? FILE_FLAG_OVERLAPPED : 0, NULL);
        if (h == INVALID_HANDLE_VALUE && overlapped) {
            h = CreateFileW(detail->DevicePath, GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        }
        if (h == INVALID_HANDLE_VALUE) { free(detail); continue; }

        HIDD_ATTRIBUTES attr;
        attr.Size = sizeof(attr);
        PHIDP_PREPARSED_DATA pp = NULL;
        HIDP_CAPS caps;
        bool matched = false;

        if (HidD_GetAttributes(h, &attr) &&
            IsKnownMchoseVid(attr.VendorID) &&
            HidD_GetPreparsedData(h, &pp)) {

            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS &&
                caps.UsagePage == kUsagePage && caps.Usage == kUsage) {
                matched = true;
                /* 记录接口 VID/PID：机型识别按它查表（不是 11 06 的机体 PID） */
                g_lastIfaceVid = attr.VendorID;
                g_lastIfacePid = attr.ProductID;
                /* 该集合必须能容纳 0x12 的 65 字节帧，否则不是我们要的控制集合。
                 * 用 > 而非 >=：同长度时取先枚举到的那个，选中结果不依赖枚举顺序。 */
                if ((int)caps.FeatureReportByteLength >= 1 + kReport12Payload &&
                    (int)caps.FeatureReportByteLength > bestFeatureLen) {
                    if (found != INVALID_HANDLE_VALUE) CloseHandle(found);
                    found = h;
                    bestFeatureLen = (int)caps.FeatureReportByteLength;
                    if (outPath && outPathLen > 0) {
                        strncpy(outPath, path, outPathLen - 1);
                        outPath[outPathLen - 1] = '\0';
                    }
                }
            }
            HidD_FreePreparsedData(pp);
        }

        if (!matched || found != h) CloseHandle(h);
        free(detail);
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return found;
}

HANDLE OpenControlDevice(char *outPath, int outPathLen)
{
    return OpenControlDeviceEx(false, outPath, outPathLen);
}

/* ------------------------------------------------------------ 读 / 写事务 */

int ReadCommand(HANDLE h, unsigned char reportId, unsigned char cmd,
                unsigned char *decoded, int cap, int verbose)
{
    if (h == NULL || h == INVALID_HANDLE_VALUE) return -1;

    const int payloadLen = (reportId == kReportControl) ? kReport11Payload
                                                         : kReport12Payload;

    /* 命令表：第一字节是命令字节，其余为参数（读命令全 0） */
    unsigned char order[64];
    memset(order, 0, sizeof(order));
    order[0] = cmd;

    /* 实际发送：逐字节 XOR 0xFF */
    unsigned char req[1 + 64];
    req[0] = reportId;
    for (int i = 0; i < payloadLen; i++) req[1 + i] = (unsigned char)(order[i] ^ 0xFF);

    /* 该芯片的特性读存在「返回上一命令缓冲」的现象（首个响应可能是全 0 或滞后），
     * 因此必须重试：只有命令回显匹配且负载非全 0 的响应才被接受。 */
    for (int attempt = 0; attempt < 4; attempt++) {
        if (!HidD_SetFeature(h, req, payloadLen + 1)) {
            if (verbose) printf("    HidD_SetFeature 失败: %lu\n", GetLastError());
            Sleep(30);
            continue;
        }

        Sleep(40);

        unsigned char res[1 + 64];
        memset(res, 0, sizeof(res));
        res[0] = reportId;
        if (!HidD_GetFeature(h, res, payloadLen + 1)) {
            if (verbose) printf("    HidD_GetFeature 失败: %lu\n", GetLastError());
            continue;
        }

        /* 校验命令回显，避免「指令混发」或拿到上一命令的缓冲 */
        if (res[1] != (unsigned char)(cmd ^ 0xFF)) {
            if (verbose)
                printf("    [try %d] 回显不匹配（期望 %02X 实际 %02X），重试\n",
                       attempt + 1, cmd ^ 0xFF, res[1]);
            continue;
        }

        /* 解码后全零视为无效响应（该芯片首次读常返回上一命令的滞后缓冲，
         * 其形态正是「解码后全 0」），必须重试。 */
        int n = payloadLen - 1;
        if (n > cap) n = cap;

        bool anyNonZero = false;
        for (int i = 0; i < n; i++) {
            unsigned char v = (unsigned char)(res[2 + i] ^ 0xFF);
            if (v != 0x00) { anyNonZero = true; break; }
        }
        /* 解码后全零必须一律视为无效（上一命令的滞后缓冲），继续重试。
         *
         * 注意：曾经为了"兼容设备真的处于全零状态"而放宽为"第 3 次起接受"，
         * 结果在重试拿到真数据之前就把滞后缓冲当有效数据接受了，
         * 实测表现为电量 0%、回报率 125 Hz、DPI 全 0 之类的垃圾状态。
         * 本协议用到的读命令（11 03/04/06、12 67）真实负载都不可能全零
         * （至少含 vid/pid、固件字符串或 DPI 档值），所以严格拒绝是正确取舍。 */
        if (!anyNonZero) {
            if (verbose) printf("    [try %d] 解码后负载全零，重试\n", attempt + 1);
            continue;
        }

        if (verbose) dbgBytes("接收", res, payloadLen + 1);

        for (int i = 0; i < n; i++) decoded[i] = (unsigned char)(res[2 + i] ^ 0xFF);
        return n;
    }

    return -1;
}

bool WriteCommand(HANDLE h, unsigned char reportId,
                  const unsigned char *payload, int payloadLen, int verbose)
{
    if (h == NULL || h == INVALID_HANDLE_VALUE) return false;
    if (payload == NULL || payloadLen <= 0) return false;

    const int reportLen = (reportId == kReportControl) ? kReport11Payload
                                                        : kReport12Payload;
    if (payloadLen > reportLen) return false;

    /* 命令表：负载 + 0x00 补齐；发送时整体 XOR 0xFF（补齐位因此变成 0xFF） */
    unsigned char order[64];
    memset(order, 0, sizeof(order));
    memcpy(order, payload, payloadLen);

    unsigned char req[1 + 64];
    req[0] = reportId;
    for (int i = 0; i < reportLen; i++) req[1 + i] = (unsigned char)(order[i] ^ 0xFF);

    if (verbose) {
        printf("    -> 写命令 rid=0x%02X\n", reportId);
        dbgBytes("发送", req, reportLen + 1);
    }

    bool ok = HidD_SetFeature(h, req, reportLen + 1) != FALSE;
    if (verbose && !ok) printf("    HidD_SetFeature 失败: %lu\n", GetLastError());
    return ok;
}

/* ------------------------------------------------------------ 高层读 */

bool ReadConnInfo(HANDLE h, ConnInfo &out, int verbose)
{
    unsigned char d[64];
    int n = ReadCommand(h, kReportControl, kCmdConnInfo, d, sizeof(d), verbose);
    if (n < 7) return false;

    out.valid   = true;
    out.bond    = d[0];
    out.vid     = (unsigned short)rdU16(d + 1);
    out.pid     = (unsigned short)rdU16(d + 3);
    out.connect = d[5];
    out.game    = d[6];
    return true;
}

bool ReadVersion(HANDLE h, char *out, int outLen, int verbose)
{
    if (out == NULL || outLen <= 0) return false;
    out[0] = '\0';

    unsigned char d[64];
    int n = ReadCommand(h, kReportControl, kCmdVersion, d, sizeof(d), verbose);
    if (n < 1) return false;

    int len = d[0];
    if (len > n - 1) len = n - 1;
    if (len > outLen - 1) len = outLen - 1;
    for (int i = 0; i < len; i++) {
        unsigned char c = d[1 + i];
        out[i] = (c >= 32 && c < 127) ? (char)c : '.';
    }
    out[len] = '\0';
    return true;
}

bool ReadDeviceInfo(HANDLE h, DeviceInfo &out, int verbose)
{
    unsigned char d[64];
    int n = ReadCommand(h, kReportControl, kCmdDeviceInfo, d, sizeof(d), verbose);
    if (n < 11) return false;

    out.valid         = true;
    out.vid           = (unsigned short)rdU16(d);
    out.pid           = (unsigned short)rdU16(d + 2);
    out.fwVersion     = rdU32(d + 4);
    out.connectMode   = (unsigned char)(d[8] & 0x07);
    out.connectStatus = (unsigned char)((d[8] >> 3) & 0x01);
    out.batteryLevel  = d[9];
    out.chargeStatus  = d[10];
    return true;
}

bool ReadAllSettings(HANDLE h, AllSettings &out, int verbose)
{
    unsigned char d[64];
    int n = ReadCommand(h, kReportConfig, kCmdAllRead, d, sizeof(d), verbose);
    if (n < 20) return false;

    /* 半字节分配由硬件实测确定（官方 parser 只用 bit4 声明，未写死先后）：
     * 实测结论：**先声明的 bit4 字段落在低半字节**。证据：
     *   - 0x40（DPI）写 i1/i2 后，回读 d[1] 与 d[2] 的**低**半字节同时变为写入值；
     *   - 0x41（回报率）写 byte2 后，回读 d[2] 的**高**半字节变为写入值。
     * 因此：
     *   d[1] 低 = gDpiIndex，d[1] 高 = gRateIndex（活动回报率）
     *   d[2] 低 = usbDpiIndex，d[2] 高 = usbRateIndex（0x41 写入字段）
     * 详见 docs/PROTOCOL.md「字段映射实测」。 */
    out.valid        = true;
    out.profileIndex = d[0];
    out.gDpiIndex    = (unsigned char)(d[1] & 0x0F);
    out.gRateIndex   = (unsigned char)(d[1] >> 4);
    out.usbRateIndex = (unsigned char)(d[2] >> 4);
    out.usbDpiIndex  = (unsigned char)(d[2] & 0x0F);
    out.reserved     = d[3];
    for (int i = 0; i < 6; i++)
        out.dpi[i] = (unsigned short)rdU16(d + 4 + i * 2);
    out.dpiSum      = d[16];
    out.sensor      = d[17];
    out.keyDebounce = d[18];
    out.sleep       = d[19];
    return true;
}

bool GetProductString(HANDLE h, wchar_t *out, int outChars)
{
    if (h == NULL || h == INVALID_HANDLE_VALUE || out == NULL || outChars <= 0) return false;
    out[0] = L'\0';

    wchar_t buf[256];
    memset(buf, 0, sizeof(buf));
    if (!HidD_GetProductString(h, buf, sizeof(buf))) return false;

    wcsncpy(out, buf, outChars - 1);
    out[outChars - 1] = L'\0';
    return out[0] != L'\0';
}

/* ------------------------------------------------------------ 高层写 */

bool SetPollingRate(HANDLE h, int hz, int rateCount, int verbose)
{
    int idx = HzToRateIndex(hz, rateCount);
    if (idx < 0) return false;

    /* 实测映射：0x41 的 byte2 决定回读 12 67 byte2 的高半字节。
     * byte1 在 12 67 中无可观测影响，置 0。 */
    unsigned char p[3];
    p[0] = kCmdSetRate;          /* 0x41 */
    p[1] = 0x00;                 /* 未使用（实测无影响） */
    p[2] = (unsigned char)idx;   /* 回报率索引 → 12 67 byte2 高半字节 */
    return WriteCommand(h, kReportControl, p, sizeof(p), verbose);
}

bool SetSleep(HANDLE h, bool enable, int minutes, int verbose)
{
    if (minutes < 0) minutes = 0;
    if (minutes > 255) minutes = 255;

    /* 官方：Qr("0x11 0x0A", {command:10, sleepStatus:enable?1:0, sleep:minutes}) */
    unsigned char p[3];
    p[0] = kCmdSetSleep;
    p[1] = (unsigned char)(enable ? 1 : 0);
    p[2] = (unsigned char)minutes;
    return WriteCommand(h, kReportControl, p, sizeof(p), verbose);
}

bool SetDpiStage(HANDLE h, const AllSettings &cur, int stageIndex, int verbose)
{
    if (!cur.valid) return false;
    if (stageIndex < 0 || stageIndex > 5) return false;

    /* 官方简单式：{command:64, usbDpiIndex, gDpiIndex, reserved, dpi0..5, sum}
     * 只改当前档位索引，其余档值原样回写，避免动到用户配置。 */
    unsigned char p[17];
    memset(p, 0, sizeof(p));
    p[0] = kCmdSetDpi;
    p[1] = (unsigned char)stageIndex;   /* usbDpiIndex */
    p[2] = (unsigned char)stageIndex;   /* gDpiIndex（同时更新无线档位索引） */
    p[3] = cur.reserved;
    for (int i = 0; i < 6; i++) {
        p[4 + i * 2]     = (unsigned char)(cur.dpi[i] & 0xFF);
        p[4 + i * 2 + 1] = (unsigned char)(cur.dpi[i] >> 8);
    }
    p[16] = cur.dpiSum;
    return WriteCommand(h, kReportControl, p, sizeof(p), verbose);
}

/* ------------------------------------------------------------ 换算 */

int RateIndexToHz(int index, int rateCount)
{
    static const int kRates[6] = { 125, 500, 1000, 2000, 4000, 8000 };
    if (index < 0 || index >= 6) return 0;
    if (rateCount > 0 && index >= rateCount) return 0;
    return kRates[index];
}

int HzToRateIndex(int hz, int rateCount)
{
    static const int kRates[6] = { 125, 500, 1000, 2000, 4000, 8000 };
    for (int i = 0; i < 6; i++) {
        if (kRates[i] == hz) {
            if (rateCount > 0 && i >= rateCount) return -1;
            return i;
        }
    }
    return -1;
}

int RateCountForMode(unsigned char connectMode, const ModelCaps &caps)
{
    /* connectMode == 1 为 2.4G 无线（多数机型支持 125..8000 共 6 档）
     * 其余（有线）按官方 rateMap[1000] = 125/500/1000 共 3 档。
     * 档位数取自机型库（未知机型回落为 6 / 3）。 */
    return (connectMode == 1) ? caps.rateCountWireless : caps.rateCountWired;
}

}  // namespace McHose
