/*
 * mchose_protocol.h — 迈从（MCHOSE）鼠标 HID 协议层（纯逻辑，不含 UI）
 * ---------------------------------------------------------------------------
 * 协议来源：对官方 MCHOSE HUB 渲染层（https://cdn.mchose.com.cn/customPage/14）
 * 的逆向 + 在本机 MCHOSE A7 Pro 上的实测验证（详见 docs/PROTOCOL.md）。
 *
 * 通道选择（本机实测 MCHOSE A7 Pro，VID 0x5253 / PID 0x1021）：
 *   MI_02 的厂商自定义顶层集合：UsagePage = 0xFF01, Usage = 0x0001
 *   该集合 Input/Output/FeatureReportByteLength 均为 65 字节。
 *
 * 报文规则：
 *   请求 = sendFeatureReport(reportId, payload)，其中
 *          payload[i] = 命令表字节[i] XOR 0xFF，不足部分用 0xFF 补齐
 *          reportId = 0x11 时负载 20 字节；reportId = 0x12 时负载 64 字节
 *   响应 = receiveFeatureReport(reportId)
 *          buf[0] = reportId
 *          buf[1] = 命令字节 XOR 0xFF（用于校验命令是否「混发」）
 *          buf[2..] = 负载，逐字节 XOR 0xFF 后按 little-endian 解析
 */

#pragma once

#include <windows.h>
#include "model_db.h"

namespace McHose {

/* ------------------------------------------------------------ 设备识别 */

constexpr unsigned short kVendorId   = 0x5253;
constexpr unsigned short kPidDongle  = 0x1021;   // 2.4G 接收器（本机实测）
constexpr unsigned short kPidWired   = 0x1020;   // 有线直连（按官方机型表 4128 推断）
constexpr unsigned short kPidDevice  = 0x0010;   // 11 06 回报的机体 PID（官方表里的 "16"）
constexpr unsigned short kUsagePage  = 0xFF01;   // 控制集合的 UsagePage
constexpr unsigned short kUsage      = 0x0001;

constexpr int kReport11Payload = 20;
constexpr int kReport12Payload = 64;

/* ------------------------------------------------------------ 命令字节 */

constexpr unsigned char kReportControl = 0x11;   // 控制/信息类
constexpr unsigned char kReportConfig  = 0x12;   // 配置类

constexpr unsigned char kCmdConnInfo    = 0x03;  // rid 0x11 读：绑定/vid/pid/连接/游戏模式
constexpr unsigned char kCmdVersion     = 0x04;  // rid 0x11 读：固件版本 ASCII
constexpr unsigned char kCmdDeviceInfo  = 0x06;  // rid 0x11 读：vid/pid/固件/连接模式/电量/充电
constexpr unsigned char kCmdAllRead     = 0x67;  // rid 0x12 读：全部设置（DPI/档位/休眠/按键）
constexpr unsigned char kCmdSetRate     = 0x41;  // rid 0x11 写：回报率
constexpr unsigned char kCmdSetSleep    = 0x0A;  // rid 0x11 写：休眠
constexpr unsigned char kCmdSetDpi      = 0x40;  // rid 0x11 写：DPI 档位（简单式）
constexpr unsigned char kCmdSetDpiXY    = 0x40;  // rid 0x12 写：DPI X/Y 独立式

/* ------------------------------------------------------------ 数据结构 */

struct ConnInfo {
    bool           valid = false;
    unsigned char  bond = 0;
    unsigned short vid = 0;
    unsigned short pid = 0;
    unsigned char  connect = 0;
    unsigned char  game = 0;
};

struct DeviceInfo {
    bool           valid = false;
    unsigned short vid = 0;
    unsigned short pid = 0;
    unsigned int   fwVersion = 0;      // 4 字节 BCD，如 0x04070405 → "5.4.7.4"
    unsigned char  connectMode = 0;    // 1 = 2.4G 无线，其它见文档
    unsigned char  connectStatus = 0;
    unsigned char  batteryLevel = 0;   // 0..100
    unsigned char  chargeStatus = 0;   // 0 = 未充电
};

struct AllSettings {
    bool           valid = false;
    unsigned char  profileIndex = 0;
    /* 回报率有两个字段，语义经硬件实测区分（见 docs/PROTOCOL.md）：
     *   gRateIndex   = 回读 byte1 高半字节 —— **活动**回报率档位；
     *                  与设备推送 report 0x13 的 payload[6] 一致，
     *                  也是官方软件在无线模式下显示的那一个。
     *   usbRateIndex = 回读 byte2 高半字节 —— 0x41 写命令实际落入的字段
     *                  （官方注释称设置先写 EEPROM，需重载才同步回 RAM）。 */
    unsigned char  gDpiIndex = 0;      /* byte1 低半字节 */
    unsigned char  gRateIndex = 0;     /* byte1 高半字节：活动回报率索引 */
    unsigned char  usbDpiIndex = 0;    /* byte2 低半字节 */
    unsigned char  usbRateIndex = 0;   /* byte2 高半字节：写入字段 */
    unsigned char  reserved = 0;
    unsigned short dpi[6] = {0, 0, 0, 0, 0, 0};
    unsigned char  dpiSum = 0;
    unsigned char  sensor = 0;
    unsigned char  keyDebounce = 0;
    unsigned char  sleep = 0;
};

/* ------------------------------------------------------------ HID 事务 */

/*
 * 打开迈从控制集合。成功返回句柄并可选返回设备路径。
 * 失败返回 INVALID_HANDLE_VALUE。
 * overlapped = true 时以 FILE_FLAG_OVERLAPPED 打开，用于被动监听推送报文。
 */
HANDLE OpenControlDeviceEx(bool overlapped, char *outPath, int outPathLen);

/* 等价于 OpenControlDeviceEx(false, ...)，用于特性报文读写 */
HANDLE OpenControlDevice(char *outPath, int outPathLen);

/*
 * 读命令：发送 [cmd, 0...]（XOR 0xFF 后）并取回解码负载。
 * 返回解码负载字节数；失败返回 -1。
 * verbose > 0 时打印原始收发字节（调试用）。
 */
int ReadCommand(HANDLE h, unsigned char reportId, unsigned char cmd,
                unsigned char *decoded, int cap, int verbose);

/*
 * 写命令：payload 为「命令表字节」（含命令字节），内部做 XOR 0xFF 并补齐。
 * payloadLen 不含 padding。成功返回 true。
 */
bool WriteCommand(HANDLE h, unsigned char reportId,
                  const unsigned char *payload, int payloadLen, int verbose);

/* 高层读写 */
bool ReadConnInfo(HANDLE h, ConnInfo &out, int verbose = 0);
bool ReadVersion(HANDLE h, char *out, int outLen, int verbose = 0);
bool ReadDeviceInfo(HANDLE h, DeviceInfo &out, int verbose = 0);
bool ReadAllSettings(HANDLE h, AllSettings &out, int verbose = 0);

/* 读取 HID 产品字符串（本机为 "MCHOSE A7 Pro"）。这是最可靠的型号来源，
 * 不依赖设备主动推送。成功返回 true。 */
bool GetProductString(HANDLE h, wchar_t *out, int outChars);

/* 设置回报率：hz ∈ {125,500,1000,2000,4000,8000}
 * rateCount 为该机型支持的档位数（A7 Pro 无线 = 6，有线 = 3） */
bool SetPollingRate(HANDLE h, int hz, int rateCount, int verbose = 0);

/* 设置休眠：enable=false 关闭；enable=true 时 minutes 为分钟数 */
bool SetSleep(HANDLE h, bool enable, int minutes, int verbose = 0);

/* 切换当前 DPI 档位（保留原档值，仅改索引） */
bool SetDpiStage(HANDLE h, const AllSettings &cur, int stageIndex, int verbose = 0);

/* ------------------------------------------------------------ 换算工具 */

/* 档位索引 → Hz（官方 rateMap 顺序），越界返回 0 */
int RateIndexToHz(int index, int rateCount);

/* Hz → 档位索引，不支持返回 -1 */
int HzToRateIndex(int hz, int rateCount);

/* 该机型档位数：无线/2.4G 支持到 8000Hz 共 6 档，有线常见 3 档 */
int RateCountForMode(unsigned char connectMode, const ModelCaps &caps);

/*
 * 最近一次成功打开的控制集合的**接口** VID/PID（例如 A7 Pro 2.4G = 5253:1021）。
 *
 * 必须与 11 06 回报的"机体 VID/PID"区分开：后者是设备类型码（A7 Pro 报 0x0010），
 * 而机型识别要按接口 VID/PID 查表——因为接收器/有线会以不同 PID 枚举。
 */
bool GetLastOpenedInterfaceIds(unsigned short *vid, unsigned short *pid);

}  // namespace McHose
