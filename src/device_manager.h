/*
 * device_manager.h — 设备状态机与后台工作线程
 * ---------------------------------------------------------------------------
 * 职责：
 *   - 独占一个后台线程负责全部 HID 交互（打开/关闭/读状态/下发命令）；
 *   - 通过两个来源更新状态：
 *       (a) 设备主动推送 report 0x13（电量/充电，零写入，最稳）
 *       (b) 特性报文读命令 11 06（设备信息/电量）、12 67（DPI/回报率/休眠）
 *   - 对外只暴露线程安全的快照与「投递命令」接口，UI 线程不碰 HID 句柄。
 */

#pragma once

#include <windows.h>
#include "mchose_protocol.h"

namespace Device {

/* 状态变化掩码 */
enum : DWORD {
    CHANGE_CONNECTED = 0x01,
    CHANGE_BATTERY   = 0x02,
    CHANGE_SETTINGS  = 0x04,   /* DPI / 回报率 / 休眠 等 */
    CHANGE_VERSION   = 0x08,
    /*
     * 细分位：仅当**值真的变了**才置位，用于"鼠标按键改了 DPI / 回报率"的弹窗提醒。
     * 与 CHANGE_SETTINGS 的区别：后者是"设置区有变化"的粗粒度通知（含档值编辑、
     * 休眠时间等），不能直接拿来判断是否该弹"DPI/回报率已切换"。
     */
    CHANGE_RATE      = 0x10,   /* 活动回报率档位变化（通常来自鼠标按键） */
    CHANGE_DPI       = 0x20,   /* 活动 DPI 档位变化（通常来自鼠标按键） */
};

struct State {
    bool  connected = false;        /* 接收器/设备句柄可用 */
    bool  mouseLinked = false;      /* 鼠标本体是否与接收器保持链路（11 03 的 connect） */
    bool  batteryValid = false;
    /* 11 06（设备信息/电量/连接模式）是否读到过有效值。
     * 未读到时 connectMode 会停留在默认值 0，直接显示会变成"有线/其它"，与事实不符。 */
    bool  deviceInfoValid = false;
    /*
     * 最后一次成功读到电量的 tick（特性读或推送都算）。
     * 用途：鼠标**关机**时接收器仍报告 connect=1，若只看 connect 就会一直
     * 显示最后一次读到的电量（陈旧值）。超过阈值没有新读数即视为失效，
     * 界面改为显示"未知"，而不是继续报一个可能早已过期的数字。
     */
    unsigned long lastBatteryTick = 0;

    /* 最近一次发布的变化掩码，仅用于诊断（可在 --dump 里核对细分位是否按预期置位） */
    DWORD lastChangeMask = 0;

    /* 细分变化计数：用于 --watch 端到端验证"鼠标按键改 DPI/回报率能被检出" */
    unsigned long rateChangeCount = 0;
    unsigned long dpiChangeCount  = 0;
    int   battery = 0;              /* 0..100 */
    bool  charging = false;
    unsigned char connectMode = 0;  /* 1 = 2.4G 无线 */
    unsigned short devicePid = 0;   /* 11 06 回报的机体 PID */
    wchar_t firmware[32] = {0};     /* 固件版本，如 L"5.4.7.4" */
    wchar_t modelName[64] = {0};    /* 设备推送的型号名（如 L"A7 Pro"） */

    /* 设置项：来自 12 67。
     * 说明：回报率索引取「回读 byte2 的高半字节」——该字段已被硬件实测确认
     * 由 0x41 写命令驱动（见 docs/PROTOCOL.md 「字段映射实测」）。 */
    bool  settingsValid = false;
    int   rateIndex = -1;           /* 活动回报率档位（byte1 高半字节） */
    int   rateHz = 0;
    int   rateCount = 6;
    int   writeRateIndex = -1;      /* 0x41 写命令实际落入的档位（byte2 高半字节） */
    unsigned char dpiIndexRaw = 0;

    /*
     * DPI 有两个索引，语义不同，不要混用：
     *   dpiActiveIndex = 回读 byte1 低半字节（gDpiIndex）  —— **活动**档位，界面显示它
     *   dpiIndexRaw    = 回读 byte2 低半字节（usbDpiIndex）—— 0x40 写命令的落点
     *
     * 实测：0x40 写完后 usbDpiIndex 立即变化，gDpiIndex 要等设备同步才跟随。
     * 早期界面用的是 dpiIndexRaw，于是切换档位后会显示成"尚未生效"的那个字段，
     * 与"校验通过"的提示自相矛盾。
     */
    unsigned char dpiActiveIndex = 0;
    unsigned short dpi[6] = {0, 0, 0, 0, 0, 0};
    unsigned char sleepMinutes = 0;

    /* 机型库（model_db）解析结果。dpiMax/modelSource 来自静态表，未识别时为默认值。 */
    unsigned short ifaceVid = 0;    /* HID 接口 VID（机型识别依据，如 0x5253） */
    unsigned short ifacePid = 0;    /* HID 接口 PID（如 0x1021） */
    unsigned short deviceVid = 0;   /* 11 06 回报的机体 VID */
    int  modelDpiMax = 0;           /* 规格 DPI 上限（0 = 未知） */
    int  modelDpiStages = 6;        /* 该机型 DPI 档位数 */
    bool modelKnown = false;        /* 是否在机型库中识别出具体型号 */
    const char *modelSource = "";   /* 数据来源（静态字符串） */
    wchar_t modelProfile[48] = {0}; /* 机型库里的型号名（可为空） */

    /* 累计统计，便于排错 */
    unsigned long readErrors = 0;
    unsigned long pushCount = 0;
    unsigned long pushReads = 0;
    bool pushChannelOpen = false;
    unsigned char lastPush[24] = {0};   /* 最近一条推送报文的原始字节（排错用） */
    int lastPushLen = 0;
    /* 发布计数：用于回归验证"设置未变化时不应重复通知 UI" */
    unsigned long totalPublishes = 0;
    unsigned long settingsPublishes = 0;
};

typedef void (*StateCallback)(const State &state, DWORD changeMask);

/* 启动 / 停止后台线程。
 * 状态一律通过 callback 通知（由回调实现方自行 PostMessage 到 UI 线程）；
 * 因此没有额外的通知窗口参数。 */
bool Start(StateCallback callback);
void Stop();

/* 取当前状态快照（加锁拷贝，跨线程安全） */
State GetState();

/* 投递命令（异步，由工作线程执行；返回是否成功入队） */
bool RequestSetPollingRate(int hz);
bool RequestSetSleep(bool enable, int minutes);
bool RequestSwitchDpiStage(int stageIndex);

/* 请求立即刷新一次（例如用户点击托盘图标时） */
void RequestRefresh();

/* 命令执行结果（由 TakeLastCommandResult 取出）。
 * 区分"已回读校验"与"仅下发"——不能把未校验的写入谎称为校验通过。 */
enum : int {
    CMD_RESULT_OK_VERIFIED     = 0,   /* 已下发，且写后回读与请求一致 */
    CMD_RESULT_FAILED          = 1,   /* 下发失败或被设备拒绝 */
    CMD_RESULT_NONE            = 2,   /* 没有待取的结果 */
    CMD_RESULT_SENT_UNVERIFIED = 3,   /* 已下发，但该命令无法回读校验 */
};

/* 挂起的命令执行结果（供 UI 提示用） */
int TakeLastCommandResult();

}  // namespace Device
