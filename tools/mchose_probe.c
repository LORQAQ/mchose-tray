/*
 * mchose_probe — 迈从（MCHOSE）鼠标 HID 协议验证工具
 * ---------------------------------------------------------------------------
 * 依据官方 MCHOSE HUB 渲染层（customPage/14）逆向出的协议实现：
 *
 *   通道：MI_02 的厂商自定义集合 UsagePage=0xFF01 / Usage=0x0001
 *         该集合 FeatureReportByteLength = 65
 *
 *   请求：sendFeatureReport(reportId, data)
 *           reportId = 0x11（20 字节负载）或 0x12（64 字节负载）
 *           data[i]  = order[i] XOR 0xFF        （order 见下表，含命令字节）
 *       等价 Win32：HidD_SetFeature(h, buf, 1 + payloadLen)
 *           buf[0] = reportId, buf[1+i] = order[i] ^ 0xFF
 *
 *   响应：receiveFeatureReport(reportId)  →  [reportId, cmd^0xFF, payload^0xFF ...]
 *       等价 Win32：HidD_GetFeature(h, buf, 1 + payloadLen)
 *           buf[0] = reportId
 *           buf[1] = 命令字节取反（用于校验命令是否混发）
 *           buf[2..] = 负载，逐字节 XOR 0xFF 后按 little-endian 解析
 *
 *   已还原的命令（order 均为「reportId 命令字节 参数...」，参数全 0）：
 *     11 03  →  bond(u8) vid(u16) pid(u16) connect(u8) game(u8)
 *     11 04  →  versionLength(u8) version[versionLength] (ASCII)
 *     11 06  →  vid(u16) pid(u16) fwVersion(u32) bit3.connectMode bit1.connectStatus
 *               bit4.reserved batteryLevel(u8) chargeStatus(u8)
 *     12 67  →  profileIndex(u8) gDpiIndex(bit4) gRateIndex(bit4) usbDpiIndex(bit4)
 *               usbRateIndex(bit4) reserved(u8) dpi0..dpi5(u16x6) dpiSum(u8)
 *               sensor(u8) keyDebounce(u8) sleep(u8)
 *
 * 编译（MinGW-w64）：
 *   gcc -O2 -Wall -o mchose_probe.exe mchose_probe.c -lhid -lsetupapi
 *
 * 用法：
 *   mchose_probe.exe             # 依次执行 11 03 / 11 04 / 11 06 / 12 67 并解析
 *   mchose_probe.exe raw 11 03   # 只发一条命令，打印原始与解码后的字节
 *   mchose_probe.exe loop 11 06 10   # 连续读 10 次 11 06（观察电量稳定性）
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

#ifndef GUID_DEVINTERFACE_HID_DEFINED
static const GUID GUID_DEVINTERFACE_HID_LOCAL =
    { 0x4D1E55B2, 0xF16F, 0x11CF, { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };
#define GUID_DEVINTERFACE_HID GUID_DEVINTERFACE_HID_LOCAL
#endif

#define TARGET_VID  0x5253
#define TARGET_PID  0x1021
#define TARGET_UP   0xFF01      /* 本机实测的控制集合 */
#define MAX_PAYLOAD 64

static void w2a(const wchar_t *src, char *dst, int dstLen)
{
    if (src == NULL) { dst[0] = '\0'; return; }
    WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dstLen, NULL, NULL);
}

static void hexline(const char *tag, const unsigned char *p, int n)
{
    printf("    %-10s", tag);
    for (int i = 0; i < n; i++) printf("%02X ", p[i]);
    printf("\n");
}

/* ---------------------------------------------------------------- 设备查找 */

static HANDLE openControlCollection(char *outPath, int outPathLen)
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
            free(detail);
            continue;
        }

        /* 先用接口路径粗筛 VID/PID，避免打开无关设备 */
        char path[1024];
        w2a(detail->DevicePath, path, sizeof(path));
        if (strstr(path, "vid_5253") == NULL || strstr(path, "pid_1021") == NULL) {
            free(detail);
            continue;
        }

        HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) { free(detail); continue; }

        HIDD_ATTRIBUTES attr;
        attr.Size = sizeof(attr);
        PHIDP_PREPARSED_DATA pp = NULL;
        HIDP_CAPS caps;
        int matched = 0;
        if (HidD_GetAttributes(h, &attr) && attr.VendorID == TARGET_VID &&
            attr.ProductID == TARGET_PID && HidD_GetPreparsedData(h, &pp)) {
            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS &&
                caps.UsagePage == TARGET_UP) {
                matched = 1;
                if (outPath) strncpy(outPath, path, outPathLen - 1);
                printf("    命中集合: UsagePage=0x%04X Usage=0x%04X "
                       "Feature=%u In=%u Out=%u\n",
                       caps.UsagePage, caps.Usage,
                       caps.FeatureReportByteLength,
                       caps.InputReportByteLength, caps.OutputReportByteLength);
            }
            HidD_FreePreparsedData(pp);
        }
        if (matched) { found = h; free(detail); break; }
        CloseHandle(h);
        free(detail);
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return found;
}

/* ------------------------------------------------------------ 收发一条命令 */

/*
 * 发送 order 指定的命令并取回响应。
 *   order : 形如 {0x11,0x03,0x00,...} 的命令表（含 reportId 与命令字节）
 *   orderLen : order 字节数（1 个 reportId + payloadLen 个负载字节）
 * 返回解码后的负载字节数（不含 reportId 与命令回显），失败返回 -1。
 */
static int transact(HANDLE h, const unsigned char *order, int orderLen,
                    unsigned char *decoded, int decodedCap, int verbose)
{
    int payloadLen = orderLen - 1;          /* 去掉 reportId */
    unsigned char reportId = order[0];

    if (payloadLen < 1 || payloadLen > MAX_PAYLOAD) return -1;

    /* 请求：负载逐字节 XOR 0xFF */
    unsigned char req[1 + MAX_PAYLOAD];
    req[0] = reportId;
    for (int i = 0; i < payloadLen; i++)
        req[1 + i] = (unsigned char)(order[1 + i] ^ 0xFF);

    if (verbose) {
        printf("    -> SetFeature(rid=0x%02X, %d 字节)\n", reportId, payloadLen);
        hexline("原始命令", order, orderLen);
        hexline("实际发送", req, payloadLen + 1);
    }

    /* 该芯片的特性读会返回「上一命令的滞后缓冲」，其形态是解码后负载全 0，
     * 因此必须重试：只有命令回显匹配且解码后负载非全 0 的响应才被接受。
     * （与 src/mchose_protocol.cpp 的实现保持一致） */
    unsigned char res[1 + MAX_PAYLOAD];
    int n = -1;

    for (int attempt = 0; attempt < 4; attempt++) {
        if (!HidD_SetFeature(h, req, payloadLen + 1)) {
            printf("    HidD_SetFeature 失败: %lu\n", GetLastError());
            Sleep(30);
            continue;
        }

        Sleep(40);

        memset(res, 0, sizeof(res));
        res[0] = reportId;
        if (!HidD_GetFeature(h, res, payloadLen + 1)) {
            printf("    HidD_GetFeature 失败: %lu\n", GetLastError());
            continue;
        }

        if (verbose) hexline("原始响应", res, payloadLen + 1);

        /* 校验命令回显 */
        unsigned char expectEcho = (unsigned char)(order[1] ^ 0xFF);
        if (res[1] != expectEcho) {
            if (verbose)
                printf("    [try %d] 回显不匹配（期望 %02X 实际 %02X），重试\n",
                       attempt + 1, expectEcho, res[1]);
            continue;
        }

        /* 解码后全零 → 视为滞后缓冲，重试 */
        int cnt = payloadLen - 1;
        if (cnt > decodedCap) cnt = decodedCap;
        bool anyNonZero = false;
        for (int i = 0; i < cnt; i++) {
            unsigned char v = (unsigned char)(res[2 + i] ^ 0xFF);
            if (v != 0x00) { anyNonZero = true; break; }
        }
        if (!anyNonZero) {
            if (verbose) printf("    [try %d] 解码后负载全零，重试\n", attempt + 1);
            continue;
        }

        for (int i = 0; i < cnt; i++) decoded[i] = (unsigned char)(res[2 + i] ^ 0xFF);
        n = cnt;
        break;
    }

    return n;
}

static unsigned rdU16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned rdU32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

/* ---------------------------------------------------------------- 各命令解析 */

/* 11 03：bond vid pid connect game —— 使用 21 字节 order（reportId + 20 负载） */
static int cmdReceive(HANDLE h, int verbose)
{
    unsigned char order[21];
    memset(order, 0, sizeof(order));
    order[0] = 0x11; order[1] = 0x03;

    unsigned char d[64];
    int n = transact(h, order, sizeof(order), d, sizeof(d), verbose);
    if (n < 0) return -1;
    printf("    [11 03 连接信息]\n");
    hexline("解码负载", d, n);
    if (n >= 7) {
        printf("      bond=%u  vid=0x%04X  pid=0x%04X  connect=%u  game=%u\n",
               d[0], rdU16(d + 1), rdU16(d + 3), d[5], d[6]);
    }
    return 0;
}

/* 11 04：固件版本（ASCII） */
static int cmdVersion(HANDLE h, int verbose)
{
    unsigned char order[21];
    memset(order, 0, sizeof(order));
    order[0] = 0x11; order[1] = 0x04;

    unsigned char d[64];
    int n = transact(h, order, sizeof(order), d, sizeof(d), verbose);
    if (n < 0) return -1;
    printf("    [11 04 固件版本]\n");
    hexline("解码负载", d, n);
    if (n >= 1) {
        int len = d[0];
        if (len > n - 1) len = n - 1;
        printf("      versionLength=%d  version=\"", d[0]);
        for (int i = 0; i < len; i++) printf("%c", (d[1 + i] >= 32 && d[1 + i] < 127) ? d[1 + i] : '.');
        printf("\"\n");
    }
    return 0;
}

/* 11 06：设备信息 + 电量（核心） */
static int cmdDevice(HANDLE h, int verbose, int *outBattery)
{
    unsigned char order[21];
    memset(order, 0, sizeof(order));
    order[0] = 0x11; order[1] = 0x06;

    unsigned char d[64];
    int n = transact(h, order, sizeof(order), d, sizeof(d), verbose);
    if (n < 0) return -1;
    printf("    [11 06 设备信息/电量]\n");
    hexline("解码负载", d, n);
    if (n >= 11) {
        unsigned connectMode   = d[8] & 0x07;         /* bit3 */
        unsigned connectStatus = (d[8] >> 3) & 0x01;  /* bit1 */
        unsigned battery       = d[9];
        unsigned charge        = d[10];
        printf("      vid=0x%04X  pid=0x%04X  fwVersion=0x%08X\n",
               rdU16(d), rdU16(d + 2), rdU32(d + 4));
        printf("      connectMode=%u  connectStatus=%u  batteryLevel=%u%%  chargeStatus=%u\n",
               connectMode, connectStatus, battery, charge);
        if (outBattery) *outBattery = (int)battery;
    } else if (n >= 8) {
        printf("      vid=0x%04X pid=0x%04X fwVersion=0x%08X (负载偏短，无电量字段)\n",
               rdU16(d), rdU16(d + 2), rdU32(d + 4));
    }
    return 0;
}

/* 12 67：全部设置（DPI / 回报率档位 / 休眠 / 按键）—— 65 字节 order */
static int cmdAll(HANDLE h, int verbose, unsigned char *outRaw, int *outLen)
{
    unsigned char order[65];
    memset(order, 0, sizeof(order));
    order[0] = 0x12; order[1] = 0x67;

    unsigned char d[128];
    int n = transact(h, order, sizeof(order), d, sizeof(d), verbose);
    if (n < 0) return -1;
    if (outRaw && outLen && *outLen >= n) { memcpy(outRaw, d, n); *outLen = n; }

    printf("    [12 67 全部设置]\n");
    hexline("解码负载", d, n < 24 ? n : 24);
    if (n >= 20) {
        printf("      profileIndex=%u\n", d[0]);
        printf("      gDpiIndex=%u  gRateIndex=%u  usbDpiIndex=%u  usbRateIndex=%u\n",
               d[1] >> 4, d[1] & 0x0F, d[2] >> 4, d[2] & 0x0F);
        printf("      dpiSum=%u  sensor=%u  keyDebounce=%u  sleep=%u\n",
               d[16], d[17], d[18], d[19]);
        printf("      dpi0..5 = ");
        for (int i = 0; i < 6; i++) printf("%u ", rdU16(d + 4 + i * 2));
        printf("\n");
    }
    return 0;
}

/* 带重试的 12 67 读：该芯片的特性读常返回上一命令缓冲，必须重试直到回显匹配 */
static int readAllRetry(HANDLE h, unsigned char *out, int tries)
{
    for (int t = 0; t < tries; t++) {
        unsigned char order[65];
        memset(order, 0, sizeof(order));
        order[0] = 0x12; order[1] = 0x67;

        unsigned char req[65];
        req[0] = 0x12;
        req[1] = (unsigned char)(0x67 ^ 0xFF);      /* 命令字节取反 */
        for (int i = 2; i < 65; i++) req[i] = 0xFF; /* 参数 0x00 取反 */
        if (!HidD_SetFeature(h, req, 65)) {
            printf("      [try %d] SetFeature 失败 err=%lu\n", t + 1, GetLastError());
            continue;
        }
        Sleep(50);

        unsigned char res[65];
        memset(res, 0, sizeof(res));
        res[0] = 0x12;
        if (!HidD_GetFeature(h, res, 65)) {
            printf("      [try %d] GetFeature 失败 err=%lu\n", t + 1, GetLastError());
            continue;
        }
        printf("      [try %d] res[0]=%02X res[1]=%02X(期望 %02X) 前8字节: %02X %02X %02X %02X %02X %02X %02X %02X\n",
               t + 1, res[0], res[1], 0x67 ^ 0xFF,
               res[0], res[1], res[2], res[3], res[4], res[5], res[6], res[7]);
        if (res[1] != (unsigned char)(0x67 ^ 0xFF)) continue;   /* 回显不匹配 → 重试 */

        int nonzero = 0;
        for (int i = 2; i < 65; i++) if (res[i] != 0x00) { nonzero = 1; break; }
        if (!nonzero) continue;                                  /* 全零 → 无效 */

        /* res[0]=reportId, res[1]=命令回显, res[2..64]=63 字节解码前负载。
         * 上界必须是 63：写 64 会读越界到 res[65]（数组只有 0..64）。
         * 与 src/mchose_protocol.cpp 的 payloadLen-1 保持一致。 */
        for (int i = 0; i < 63; i++) out[i] = (unsigned char)(res[2 + i] ^ 0xFF);
        return t + 1;                                            /* 返回尝试次数 */
    }
    return -1;
}

static void printAll(const char *tag, const unsigned char *d)
{
    printf("    %-14s B1=0x%02X B2=0x%02X  [高前] gDpi=%u gRate=%u usbDpi=%u usbRate=%u | [低前] gDpi=%u gRate=%u usbDpi=%u usbRate=%u\n",
           tag, d[1], d[2],
           d[1] >> 4, d[1] & 0x0F, d[2] >> 4, d[2] & 0x0F,
           d[1] & 0x0F, d[1] >> 4, d[2] & 0x0F, d[2] >> 4);
}

/* 写 DPI 命令：0x11 0x40，负载 [0x40, i1, i2, reserved, d0..d5 LE, sum] */
static int writeDpi(HANDLE h, int i1, int i2, const unsigned char *orig, int verbose)
{
    unsigned char req[21];
    req[0] = 0x11;
    req[1] = (unsigned char)(0x40 ^ 0xFF);
    req[2] = (unsigned char)(i1 ^ 0xFF);
    req[3] = (unsigned char)(i2 ^ 0xFF);
    req[4] = (unsigned char)(orig[3] ^ 0xFF);            /* reserved（原样回写） */
    for (int i = 0; i < 12; i++)                          /* dpi0..5 小端（原样回写） */
        req[5 + i] = (unsigned char)(orig[4 + i] ^ 0xFF);
    req[17] = (unsigned char)(orig[16] ^ 0xFF);           /* sum（原样回写） */
    for (int i = 18; i < 21; i++) req[i] = 0xFF;
    if (verbose) {
        printf("    发送 DPI 写：i1=%d i2=%d reserved=0x%02X sum=0x%02X\n", i1, i2, orig[3], orig[16]);
    }
    return HidD_SetFeature(h, req, 21) ? 0 : -1;
}

/*
 * DPI 档位字段判定 + 自校验还原。
 * 步骤：读原始 → 写两个索引字段=4 → 回读看哪个半字节变化 → 枚举候选组合还原到原始字节。
 */
static void cmdDpiTest(HANDLE h)
{
    unsigned char orig[64], a[64];
    printf("--- 1) 原始状态 ---\n");
    if (readAllRetry(h, orig, 6) < 0) { printf("    读取失败\n"); return; }
    printAll("原始", orig);
    printf("    reserved=0x%02X sum=0x%02X sleep=%u sensor=0x%02X\n\n", orig[3], orig[16], orig[19], orig[17]);

    printf("--- 2) 写 DPI 索引字段 = 4 ---\n");
    if (writeDpi(h, 4, 4, orig, 1) < 0) { printf("    写失败\n"); return; }
    Sleep(300);
    if (readAllRetry(h, a, 6) < 0) { printf("    读取失败\n"); return; }
    printAll("写后", a);

    printf("\n--- 3) 判定 ---\n");
    struct { const char *what; int val; } hits[4];
    int nh = 0;
    if ((a[1] >> 4) == 4) { hits[nh].what = "回读 byte1 高半字节"; hits[nh++].val = 4; }
    if ((a[1] & 0x0F) == 4) { hits[nh].what = "回读 byte1 低半字节"; hits[nh++].val = 4; }
    if ((a[2] >> 4) == 4) { hits[nh].what = "回读 byte2 高半字节"; hits[nh++].val = 4; }
    if ((a[2] & 0x0F) == 4) { hits[nh].what = "回读 byte2 低半字节"; hits[nh++].val = 4; }
    if (nh == 0) printf("    未在回读中观察到 4（写入可能被忽略）\n");
    for (int i = 0; i < nh; i++) printf("    ✔ %s 变为 4\n", hits[i].what);

    printf("\n--- 4) 自校验还原 ---\n");
    /* 枚举两个索引字段的候选值，直到回读的 B1/B2 与原始完全一致 */
    int cand[6] = { 0, 1, 2, 3, 4, 5 };
    int done = 0;
    for (int x = 0; x < 6 && !done; x++) {
        for (int y = 0; y < 6 && !done; y++) {
            writeDpi(h, cand[x], cand[y], orig, 0);
            Sleep(220);
            unsigned char t[64];
            if (readAllRetry(h, t, 3) < 0) continue;
            if (t[1] == orig[1] && t[2] == orig[2]) {
                printf("    ✔ 用 i1=%d i2=%d 还原成功（B1=0x%02X B2=0x%02X）\n", cand[x], cand[y], t[1], t[2]);
                done = 1;
            }
        }
    }
    if (!done) {
        printf("    ✘ 枚举未找到还原组合，最后一次回读：\n");
        unsigned char t[64];
        if (readAllRetry(h, t, 3) >= 0) printAll("当前", t);
        printf("      原始 B1=0x%02X B2=0x%02X —— 请用 MCHOSE HUB 手动恢复 DPI 档位\n",
               orig[1], orig[2]);
    }
}
/* 写一对 {usbRate, freeRate} —— 0x11 0x41 */
static int writePair(HANDLE h, int b1, int b2)
{
    unsigned char req[21];
    req[0] = 0x11;
    req[1] = (unsigned char)(0x41 ^ 0xFF);
    req[2] = (unsigned char)(b1 ^ 0xFF);
    req[3] = (unsigned char)(b2 ^ 0xFF);
    for (int i = 4; i < 21; i++) req[i] = 0xFF;
    return HidD_SetFeature(h, req, 21) ? 0 : -1;
}

/*
 * 映射判定：分别把 7 写进 byte1 / byte2，观察 12 67 回读的哪个半字节变化，
 * 从而确定「写入字节 → 回读字段」的对应关系，最后把原始值写回。
 */
static void cmdMapping(HANDLE h)
{
    unsigned char orig[64], a[64], b[64];

    printf("--- 1) 原始状态（带重试读）---\n");
    int t = readAllRetry(h, orig, 6);
    if (t < 0) { printf("    读取失败\n"); return; }
    printf("    (第 %d 次尝试成功)\n", t);
    printAll("原始", orig);
    printf("    完整负载: ");
    for (int i = 0; i < 24; i++) printf("%02X ", orig[i]);
    printf("\n\n");

    printf("--- 2) 写 byte1=7, byte2=0 ---\n");
    if (writePair(h, 7, 0) < 0) { printf("    写失败\n"); return; }
    Sleep(300);
    t = readAllRetry(h, a, 6);
    if (t < 0) { printf("    读取失败\n"); return; }
    printAll("写后", a);
    printf("\n");

    printf("--- 3) 写 byte1=0, byte2=7 ---\n");
    if (writePair(h, 0, 7) < 0) { printf("    写失败\n"); return; }
    Sleep(300);
    t = readAllRetry(h, b, 6);
    if (t < 0) { printf("    读取失败\n"); return; }
    printAll("写后", b);
    printf("\n");

    /* 逐位比较，指出 byte1 / byte2 分别驱动了哪个半字节 */
    printf("--- 4) 判定 ---\n");
    if (a[1] != orig[1] || a[2] != orig[2]) {
        if ((a[2] >> 4) == 7)       printf("    byte2 → 回读 byte2 的高半字节\n");
        else if ((a[2] & 0x0F) == 7) printf("    byte2 → 回读 byte2 的低半字节\n");
        else if ((a[1] >> 4) == 7)   printf("    byte2 → 回读 byte1 的高半字节\n");
        else if ((a[1] & 0x0F) == 7) printf("    byte2 → 回读 byte1 的低半字节\n");
        else                          printf("    byte2 写入未在回读中体现（可能被设备忽略）\n");
    }
    if (b[1] != orig[1] || b[2] != orig[2]) {
        if ((b[1] >> 4) == 7)       printf("    byte1 → 回读 byte1 的高半字节\n");
        else if ((b[1] & 0x0F) == 7) printf("    byte1 → 回读 byte1 的低半字节\n");
        else if ((b[2] >> 4) == 7)   printf("    byte2 → 回读 byte2 的高半字节\n");
        else if ((b[2] & 0x0F) == 7) printf("    byte2 → 回读 byte2 的低半字节\n");
        else                          printf("    byte1 写入未在回读中体现（可能被设备忽略）\n");
    }

    /* 还原：把原始半字节值写回对应字段（先按原始字节两位都试一遍，再回读确认） */
    int r1 = orig[1] & 0x0F, r2 = orig[2] & 0x0F;
    printf("\n--- 5) 还原 byte1=%d byte2=%d ---\n", r1, r2);
    writePair(h, r1, r2);
    Sleep(300);
    unsigned char fin[64];
    t = readAllRetry(h, fin, 6);
    if (t >= 0) {
        printAll("还原后", fin);
        if (fin[1] == orig[1] && fin[2] == orig[2])
            printf("    ✔ 已完全还原到原始状态\n");
        else
            printf("    ✘ 未完全还原，原始 B1=0x%02X B2=0x%02X\n", orig[1], orig[2]);
    }
}

int main(int argc, char **argv)
{
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("=== mchose_probe — MCHOSE A7 Pro (VID %04X / PID %04X) 协议验证 ===\n\n",
           TARGET_VID, TARGET_PID);

    char path[1024] = {0};
    HANDLE h = openControlCollection(path, sizeof(path));
    if (h == INVALID_HANDLE_VALUE) {
        printf("未找到目标集合（UsagePage 0x%04X）。请确认鼠标已连接。\n", TARGET_UP);
        return 1;
    }
    printf("    设备路径: %s\n\n", path);

    int rc = 0;

    if (argc >= 2 && strcmp(argv[1], "dpitest") == 0) {
        cmdDpiTest(h);
    }
    else if (argc >= 2 && strcmp(argv[1], "mapping") == 0) {
        cmdMapping(h);
    }
    else if (argc >= 3 && strcmp(argv[1], "writepair") == 0) {
        int b1 = (argc >= 3) ? atoi(argv[2]) : 0;
        int b2 = (argc >= 4) ? atoi(argv[3]) : 0;
        printf("--- 写 byte1=%d byte2=%d ---\n", b1, b2);
        writePair(h, b1, b2);
        Sleep(300);
        unsigned char d[64];
        int t = readAllRetry(h, d, 6);
        if (t >= 0) { printf("    (第 %d 次尝试成功)\n", t); printAll("回读", d); }
        else printf("    读取失败\n");
    }
    else if (argc >= 4 && strcmp(argv[1], "raw") == 0) {
        /* raw <rid> <cmd> */
        unsigned char order[65];
        memset(order, 0, sizeof(order));
        order[0] = (unsigned char)strtoul(argv[2], NULL, 16);
        order[1] = (unsigned char)strtoul(argv[3], NULL, 16);
        int orderLen = (order[0] == 0x12) ? 65 : 21;
        unsigned char d[128];
        int n = transact(h, order, orderLen, d, sizeof(d), 1);
        if (n >= 0) { printf("    解码负载 (%d 字节):\n", n); hexline("", d, n); }
        else rc = 1;
    }
    else if (argc >= 4 && strcmp(argv[1], "loop") == 0) {
        int times = (argc >= 5) ? atoi(argv[4]) : 5;
        unsigned char order[21];
        memset(order, 0, sizeof(order));
        order[0] = (unsigned char)strtoul(argv[2], NULL, 16);
        order[1] = (unsigned char)strtoul(argv[3], NULL, 16);
        for (int i = 0; i < times; i++) {
            unsigned char d[64];
            int n = transact(h, order, sizeof(order), d, sizeof(d), 0);
            printf("    #%d  ", i + 1);
            if (n >= 0) hexline("", d, n);
            else printf("失败\n");
            Sleep(300);
        }
    }
    else {
        printf("--- 1) 连接信息 ---\n");
        cmdReceive(h, 1);
        printf("\n--- 2) 固件版本 ---\n");
        cmdVersion(h, 1);
        printf("\n--- 3) 设备信息 / 电量 ---\n");
        int battery = -1;
        cmdDevice(h, 1, &battery);
        printf("\n--- 4) 全部设置 ---\n");
        cmdAll(h, 1, NULL, NULL);

        printf("\n=== 结论 ===\n");
        if (battery >= 0) {
            printf("    从 11 06 读到的电量: %d%%\n", battery);
            printf("    请与 MCHOSE HUB 的 appdata 记录（files/config/mc_main_store_key.json 的 batteryLevel）对比\n");
        } else {
            printf("    未能解析电量，协议可能需调整。\n");
            rc = 1;
        }
    }

    CloseHandle(h);
    return rc;
}
