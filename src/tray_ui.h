/*
 * tray_ui.h — 托盘图标、提示文本、OSD 悬浮窗、开机自启
 */

#pragma once

#include <windows.h>
#include "device_manager.h"

namespace TrayUi {

/* 注册 OSD 窗口类并准备主题（在创建主窗口之前调用） */
void Init(HINSTANCE hInst);
void Cleanup();

/* 按指定边长绘制图标（供高 DPI 验证与预览使用） */
HICON CreateBatteryIconSized(const Device::State &st, int style, int size);

/* 按状态绘制托盘图标，尺寸取系统托盘图标尺寸。style: 0 = 电池胶囊，1 = 大号数字 */
HICON CreateBatteryIcon(const Device::State &st, int style);

/* 组装托盘提示文本 */
void UpdateTooltip(NOTIFYICONDATAW &nid, const Device::State &st);

/* 生成提示文本 / OSD 三行文本。
 * 抽成独立函数是为了让 --dump 能打印出来核对文字格式：
 * 本工具链下 swprintf 的 %s 是**窄**语义，宽串必须用 %ls，
 * 写错会得到乱码且不会有任何编译警告。 */
void BuildTooltipText(const Device::State &st, wchar_t *out, int cap);

/*
 * 统一的"电量"显示文本。所有界面/诊断都必须走这里，不要各自拼字符串。
 *
 * 起因是一个真实缺陷：鼠标关机（或刚启动还没读到电量）时，接收器仍报告
 * connect=1，于是状态进入"已连接"分支，而各处的写法是
 *     st.batteryValid ? st.battery : 0
 * —— 没有有效读数时凭空打印出"电量 0%"，让人以为鼠标快没电了。
 * 这类"编造数据"比不显示更糟，所以集中到一处并写清语义：
 *   batteryValid=false -> "未知"（不写百分比）
 *   充电中            -> "69% ⚡"
 *   正常              -> "69%"
 */
void FormatBatteryText(const Device::State &st, wchar_t *out, int cap);

/*
 * 低电量提醒的判定逻辑（纯函数，不碰任何系统状态，因此可被 --selftest 覆盖）。
 *
 * armed 是"是否已武装"：跨过阈值时提醒一次并解除武装，避免电量停在 19% 时反复弹窗；
 * 电量回升到 kLowBatteryRearmPct 以上、或开始充电时会重新武装。
 * 这样"充上电→再掉到 20%"能再次提醒，而"一直低电量"只提醒一次。
 *
 * 返回 true 表示本次应当弹出提醒。
 */
constexpr int kLowBatteryPct      = 20;   /* 低于等于此值提醒 */
constexpr int kLowBatteryRearmPct = 25;   /* 回升到此值以上重新武装 */

bool LowBatteryShouldNotify(bool &armed, int battery, bool batteryValid, bool charging);

/* 通过托盘图标弹一条气泡/通知。title 可为空。 */
void NotifyBalloon(NOTIFYICONDATAW &nid, const wchar_t *title, const wchar_t *text, DWORD flags);
void BuildOsdLines(const Device::State &st, const wchar_t *note,
                   wchar_t out[3][160]);

/* 在屏幕右下角弹出一条 OSD；note 非空时用它替换第三行（用于命令结果反馈） */
void ShowOsd(const Device::State &st, const wchar_t *note);

/* 图标样式（持久化到注册表） */
int  GetBatteryStyle();
void CycleBatteryStyle();

/* 开机自启（HKCU\...\Run） */
bool IsAutoRunEnabled();
void ToggleAutoRun();

/*
 * 开机自启（写入 HKCU\...\Run，不需要管理员权限）。
 *
 * 为什么要单独暴露"读回已登记路径"：自启项存的是**绝对路径**，
 * 用户一旦移动/重命名 exe，自启就会静默失效——注册表里还留着旧路径，
 * 菜单照样打勾，但开机什么都不会发生。GetAutoRunCommand 让调用方能对比并修复。
 */
bool IsAutoRunEnabled();
bool GetAutoRunCommand(wchar_t *out, int cap);
bool EnableAutoRun();
bool DisableAutoRun();

}  // namespace TrayUi
