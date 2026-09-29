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

}  // namespace TrayUi
